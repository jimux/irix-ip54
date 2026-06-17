/*
 * IP54 Paravirtual Audio Driver (pvaudio)
 *
 * Char device /dev/pvaudio for the sgi-pvaudio ring-buffer PCM device.
 * Registers at PHYS_TO_K1(0x1F480400), 4-byte aligned, big-endian.
 *
 * Usage:
 *   open("/dev/pvaudio")
 *   ioctl(PVAUDIO_SET_RATE, rate)       — sample rate (default 44100)
 *   ioctl(PVAUDIO_SET_CHANNELS, ch)     — channels (default 2)
 *   ioctl(PVAUDIO_SET_BITS, bits)       — bits/sample (default 16)
 *   write(fd, pcm_data, nbytes)         — stream PCM into ring buffer
 *   close()
 *
 * V1: BUF_DONE interrupt uses timeout()-based polling (HEART ISR
 *     bit 22 → IP4 interrupt path wired in Phase 9).
 *
 * See: include/hw/misc/sgi_pvaudio.h for register definitions.
 *
 * Copyright 1996-2024, Silicon Graphics, Inc. / QEMU IP54 project.
 */
#ident "$Revision: 1.0 $"

#if IP54

#include "sys/types.h"
#include "sys/param.h"
#include "sys/systm.h"
#include "sys/sysmacros.h"
#include "sys/cmn_err.h"
#include "sys/debug.h"
#include "sys/errno.h"
#include "sys/immu.h"
#include "sys/kmem.h"
#include "sys/sbd.h"
#include "sys/cpu.h"
#include "sys/conf.h"
#include "sys/edt.h"
#include "sys/hwgraph.h"
#include "sys/uio.h"
#include "sys/cred.h"

/*
 * sgi-pvaudio register base (KSEG1 uncached).
 * Registers are 32-bit, big-endian, 4-byte aligned.
 */
#define PVAUDIO_BASE        PHYS_TO_K1(0x1F480400ULL)
#define PVAUDIO_REG(off)    (*(volatile __uint32_t *)(PVAUDIO_BASE + (off)))

/* Register offsets */
#define PVAUDIO_CTRL        0x00    /* control: PLAY / RESET */
#define PVAUDIO_STATUS      0x04    /* status bits */
#define PVAUDIO_INTR_STAT   0x08    /* interrupt status (W1C) */
#define PVAUDIO_INTR_MASK   0x0C    /* interrupt mask */
#define PVAUDIO_BUF_BASE    0x10    /* ring buffer physical base */
#define PVAUDIO_BUF_SIZE    0x14    /* ring buffer size in bytes */
#define PVAUDIO_BUF_HEAD    0x18    /* guest write pointer (bytes from base) */
#define PVAUDIO_BUF_TAIL    0x1C    /* QEMU read pointer (read-only) */
#define PVAUDIO_SAMPLE_RATE 0x20    /* samples per second */
#define PVAUDIO_CHANNELS    0x24    /* number of channels */
#define PVAUDIO_BITS        0x28    /* bits per sample */

/* CTRL bits */
#define PVAUDIO_CTRL_PLAY   (1 << 0)
#define PVAUDIO_CTRL_RESET  (1 << 1)

/* STATUS bits */
#define PVAUDIO_STATUS_UNDERRUN (1 << 0)
#define PVAUDIO_STATUS_PLAYING  (1 << 1)

/* Interrupt bits */
#define PVAUDIO_INTR_BUF_DONE   (1 << 0)

/* User-visible ioctl numbers */
#define PVAUDIO_SET_RATE    0x6000  /* arg: int (sample rate in Hz) */
#define PVAUDIO_SET_CHANNELS 0x6001 /* arg: int (1 or 2) */
#define PVAUDIO_SET_BITS    0x6002  /* arg: int (8 or 16) */

/* Ring buffer size: 64KB — ~0.36s of stereo 16-bit 44100Hz audio */
#define PVAUDIO_RINGBUF_SIZE    (64 * 1024)

/* Per-open state */
struct pvaudio_state {
    int        ps_open;
    __uint32_t ps_rate;
    __uint32_t ps_channels;
    __uint32_t ps_bits;
    __uint32_t ps_head;     /* shadow of BUF_HEAD (bytes from base) */
    __uint32_t ps_bufsize;  /* ring buffer size */
    void      *ps_bufk1;   /* KSEG1 virtual pointer to ring buffer */
    __uint64_t ps_bufphys;  /* physical address */
    uint       ps_bufpages; /* pages allocated (for kvpfree) */
};

static struct pvaudio_state pvaudio_state;
static char pvaudio_static_buf[PVAUDIO_RINGBUF_SIZE];

int pvaudiodevflag = 0;

/*
 * pvaudio_open - open /dev/pvaudio.
 * Allocates the ring buffer, programs default audio format, starts device.
 */
/* ARGSUSED */
int
pvaudioopen(dev_t dev, int oflag, int otyp, cred_t *crp)
{
    struct pvaudio_state *ps = &pvaudio_state;
    void *k0buf;
    uint npages;

    if (ps->ps_open)
        return EBUSY;

    /* Reset device */
    PVAUDIO_REG(PVAUDIO_CTRL) = PVAUDIO_CTRL_RESET;

    /* Use static BSS buffer for ring buffer */
    k0buf = pvaudio_static_buf;
    bzero(k0buf, PVAUDIO_RINGBUF_SIZE);

    ps->ps_bufphys  = (__uint64_t)kvtophys((caddr_t)k0buf);
    ps->ps_bufk1    = (void *)PHYS_TO_K1(ps->ps_bufphys);
    ps->ps_bufpages = 0;
    ps->ps_bufsize  = PVAUDIO_RINGBUF_SIZE;
    ps->ps_head     = 0;

    /* Default: 44100Hz, stereo, 16-bit signed PCM */
    ps->ps_rate     = 44100;
    ps->ps_channels = 2;
    ps->ps_bits     = 16;

    /* Program device */
    PVAUDIO_REG(PVAUDIO_BUF_BASE)    = (__uint32_t)(ps->ps_bufphys & 0xFFFFFFFF);
    PVAUDIO_REG(PVAUDIO_BUF_SIZE)    = ps->ps_bufsize;
    PVAUDIO_REG(PVAUDIO_BUF_HEAD)    = 0;
    PVAUDIO_REG(PVAUDIO_SAMPLE_RATE) = ps->ps_rate;
    PVAUDIO_REG(PVAUDIO_CHANNELS)    = ps->ps_channels;
    PVAUDIO_REG(PVAUDIO_BITS)        = ps->ps_bits;
    PVAUDIO_REG(PVAUDIO_CTRL)        = PVAUDIO_CTRL_PLAY;

    ps->ps_open = 1;
    return 0;
}

/*
 * pvaudio_close - close /dev/pvaudio.
 * Stops playback and frees the ring buffer.
 */
/* ARGSUSED */
int
pvaudioclose(dev_t dev, int oflag, int otyp, cred_t *crp)
{
    struct pvaudio_state *ps = &pvaudio_state;

    if (!ps->ps_open)
        return EINVAL;

    PVAUDIO_REG(PVAUDIO_CTRL) = PVAUDIO_CTRL_RESET;

    /* Static buffer - don't free, just clear state */
    ps->ps_bufk1    = NULL;
    ps->ps_bufphys  = 0;
    ps->ps_bufpages = 0;
    ps->ps_bufsize  = 0;

    ps->ps_open = 0;
    return 0;
}

/*
 * pvaudio_write - copy PCM data from user into the ring buffer.
 *
 * Splits the write into one or two chunks to handle ring wrap.
 * If the buffer is full, spins (busy-waits) until QEMU drains it.
 * V1 spin strategy; v2 will sleep on BUF_DONE interrupt.
 */
int
pvaudiowrite(dev_t dev, uio_t *uiop, cred_t *crp)
{
    struct pvaudio_state *ps = &pvaudio_state;
    __uint32_t tail, avail, chunk, head_new;
    caddr_t dst;
    int error = 0;

    if (!ps->ps_open)
        return EINVAL;

    while (uiop->uio_resid > 0) {
        /* Read current QEMU read pointer */
        tail = PVAUDIO_REG(PVAUDIO_BUF_TAIL);

        /* Compute available space in ring buffer */
        if (ps->ps_head >= tail) {
            avail = ps->ps_bufsize - (ps->ps_head - tail) - 1;
        } else {
            avail = tail - ps->ps_head - 1;
        }

        if (avail == 0) {
            /* Buffer full — poll until QEMU consumes some data */
            int tries = 100000;
            while (tries-- > 0) {
                tail  = PVAUDIO_REG(PVAUDIO_BUF_TAIL);
                avail = (ps->ps_head >= tail)
                    ? (ps->ps_bufsize - (ps->ps_head - tail) - 1)
                    : (tail - ps->ps_head - 1);
                if (avail > 0)
                    break;
            }
            if (avail == 0) {
                /* Stuck — device underrun or stalled */
                error = EIO;
                break;
            }
        }

        chunk = (avail < (uint)uiop->uio_resid) ? avail : (uint)uiop->uio_resid;

        /* Contiguous write up to end of ring buffer */
        if (ps->ps_head + chunk > ps->ps_bufsize)
            chunk = ps->ps_bufsize - ps->ps_head;

        dst = (caddr_t)ps->ps_bufk1 + ps->ps_head;
        error = uiomove(dst, chunk, UIO_WRITE, uiop);
        if (error)
            break;

        head_new = (ps->ps_head + chunk) % ps->ps_bufsize;
        ps->ps_head = head_new;

        /* Advance device write pointer */
        PVAUDIO_REG(PVAUDIO_BUF_HEAD) = ps->ps_head;
    }

    return error;
}

/*
 * pvaudio_ioctl - handle device control requests.
 *
 * PVAUDIO_SET_RATE:     change sample rate
 * PVAUDIO_SET_CHANNELS: change channel count
 * PVAUDIO_SET_BITS:     change bits per sample
 *
 * After any format change the device is reconfigured with RESET+PLAY.
 */
/* ARGSUSED */
int
pvaudioioctl(dev_t dev, int cmd, caddr_t arg, int mode, cred_t *crp, int *rvalp)
{
    struct pvaudio_state *ps = &pvaudio_state;
    int val;

    if (!ps->ps_open)
        return EINVAL;

    /* All ioctls take an int argument */
    if (copyin(arg, &val, sizeof(val)))
        return EFAULT;

    switch (cmd) {
    case PVAUDIO_SET_RATE:
        if (val < 8000 || val > 96000)
            return EINVAL;
        ps->ps_rate = (__uint32_t)val;
        break;

    case PVAUDIO_SET_CHANNELS:
        if (val != 1 && val != 2)
            return EINVAL;
        ps->ps_channels = (__uint32_t)val;
        break;

    case PVAUDIO_SET_BITS:
        if (val != 8 && val != 16)
            return EINVAL;
        ps->ps_bits = (__uint32_t)val;
        break;

    default:
        return EINVAL;
    }

    /* Reconfigure device (stops and restarts playback with new format) */
    ps->ps_head = 0;
    PVAUDIO_REG(PVAUDIO_CTRL)        = PVAUDIO_CTRL_RESET;
    PVAUDIO_REG(PVAUDIO_BUF_HEAD)    = 0;
    PVAUDIO_REG(PVAUDIO_SAMPLE_RATE) = ps->ps_rate;
    PVAUDIO_REG(PVAUDIO_CHANNELS)    = ps->ps_channels;
    PVAUDIO_REG(PVAUDIO_BITS)        = ps->ps_bits;
    PVAUDIO_REG(PVAUDIO_CTRL)        = PVAUDIO_CTRL_PLAY;

    return 0;
}

/*
 * pvaudioedtinit - register /hw/pvaudio character device node via hwgraph.
 */
void
pvaudioedtinit(struct edt *edtp)
{
    vertex_hdl_t pvaudio_vhdl;
    graph_error_t rv;

    if (badaddr((void *)PVAUDIO_BASE, sizeof(__uint32_t))) {
        return;
    }

    rv = hwgraph_char_device_add(hwgraph_root, "pvaudio", "pvaudio", &pvaudio_vhdl);
    if (rv == GRAPH_SUCCESS) {
        hwgraph_chmod(pvaudio_vhdl, 0666);
        cmn_err(CE_NOTE, "pvaudio: registered /hw/pvaudio");
    } else {
        cmn_err(CE_WARN, "pvaudio: hwgraph_char_device_add failed (%d)", rv);
    }
}

#endif /* IP54 */
