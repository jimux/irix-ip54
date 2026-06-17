/*
 * IP54 Paravirtual Ethernet Driver (pvnet)
 *
 * Simple DMA NIC attached at PHYS_TO_K1(0x1F480200).
 * Big-endian 64-bit registers, 8-byte stride.
 *
 * TX: synchronous poll (write TX_BASE/TX_LEN, CMD=TX_START, poll TX_DONE).
 * RX: timeout()-based polling every PVNET_POLL_TICKS (v1).
 *     Interrupt-driven path (HEART ISR bit 20 → IP4) added in Phase 9.
 *
 * Modeled on if_ec2.c (Seeq/HPC3) and if_me.c (MACE/IP32).
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
#include "sys/edt.h"
#include "sys/errno.h"
#include "sys/immu.h"
#include "sys/invent.h"
#include "sys/kopt.h"
#include "sys/mbuf.h"
#include "sys/sbd.h"
#include "sys/socket.h"
#include "sys/cpu.h"
#include "net/if.h"
#include "net/raw.h"
#include "net/soioctl.h"
#include "ether.h"
#include "sys/kmem.h"
#include "sys/ddi.h"
#include "sys/hwgraph.h"
#include "sys/iograph.h"
#include "string.h"

/*
 * pvnet MMIO base (KSEG1 uncached).
 * Registers are 64-bit wide, big-endian, offset stride 8.
 */
#define PVNET_BASE          PHYS_TO_K1(0x1F480200ULL)
#define PVNET_REG64(off)    (*(volatile __uint64_t *)(PVNET_BASE + (off)))

/* Register offsets (8-byte stride, addr &= ~7 in QEMU) */
#define PVNET_CMD           0x00    /* write-only */
#define PVNET_STATUS        0x08
#define PVNET_INTR_STATUS   0x10    /* W1C */
#define PVNET_INTR_MASK     0x18
#define PVNET_TX_BASE       0x20    /* physical DMA address */
#define PVNET_TX_LEN        0x28    /* byte count */
#define PVNET_RX_BASE       0x30    /* physical DMA address */
#define PVNET_RX_LEN        0x38    /* max receive byte count */
#define PVNET_MAC_HI        0x40    /* MAC bytes [0:1] */
#define PVNET_MAC_LO        0x48    /* MAC bytes [2:5] */
#define PVNET_RX_ACTUAL     0x50    /* actual RX byte count (set on RX_DONE) */

/* CMD bits */
#define PVNET_CMD_TX_START  ((__uint64_t)1 << 0)
#define PVNET_CMD_RX_START  ((__uint64_t)1 << 1)
#define PVNET_CMD_RESET     ((__uint64_t)1 << 2)

/* Interrupt status bits (INTR_STATUS, INTR_MASK) */
#define PVNET_INTR_TX_DONE  ((__uint64_t)1 << 0)
#define PVNET_INTR_RX_DONE  ((__uint64_t)1 << 1)

/* Ethernet frame size limits (no FCS — QEMU strips it) */
#define PVNET_MAX_FRAME     1520    /* ETHERMAXLEN + a few bytes headroom */
#define EHDR_LEN            14      /* sizeof(struct ether_header) */

/* Poll interval: pvnet_intr masks device IRQ on RX, poll re-enables.
 * Fast poll (2 ticks = ~20ms) ensures deferred packets are picked up quickly. */
#define PVNET_POLL_TICKS    1   /* 10ms @ 100Hz (was 2; halves idle RTT) */

/*
 * Convert KSEG0 virtual → physical (works for addresses in first 256MB).
 * kvtophys() handles all kernel virtual → physical conversions correctly.
 */
#define PVNET_KVTOPHYS(va)  ((__uint64_t)kvtophys((caddr_t)(va)))

/* Private per-interface data */
struct pvnet_info {
    struct etherif   pi_eif;     /* MUST be first — ether layer indexes through this */
    struct etheraddr pi_addr;    /* hardware ethernet address */
    int              pi_unit;
    char            *pi_rxbuf;   /* KSEG1 uncached RX DMA buffer */
    __uint64_t       pi_rxphys;  /* physical address of pi_rxbuf */
    char            *pi_txbuf;   /* KSEG1 uncached TX DMA buffer */
    __uint64_t       pi_txphys;  /* physical address of pi_txbuf */
};

/*
 * Only one pvnet device per machine.
 * eiftoei: get pvnet_info from etherif pointer (etherif.eif_private).
 */
static struct pvnet_info pvnet_info_table[1];
#define eiftoei(eif)  ((struct pvnet_info *)((eif)->eif_private))

/* IP54.c interrupt wiring — called after heart.c init */
extern void ip54_intr_init(void);

/* Forward declarations */
static int  pvnet_init(struct etherif *, int);
static void pvnet_reset(struct etherif *);
static void pvnet_watchdog(struct ifnet *);
static int  pvnet_transmit(struct etherif *, struct etheraddr *,
                            struct etheraddr *, u_short, struct mbuf *);
static int  pvnet_ioctl(struct etherif *, int, void *);
static void pvnet_poll(struct pvnet_info *);

static struct etherifops pvnetops = {
    pvnet_init, pvnet_reset, pvnet_watchdog, pvnet_transmit,
    (int (*)(struct etherif *, int, void *))pvnet_ioctl
};

int if_pvnetdevflag = 0;

/*
 * pvnet_alloc_dma_buf - allocate an uncached DMA buffer.
 * Returns KSEG1 pointer and fills *phys_out with the physical address.
 */
static char *
pvnet_alloc_dma_buf(int size, __uint64_t *phys_out)
{
    char *k0buf;
    __uint64_t phys;

    k0buf = (char *)kmem_alloc(size, KM_CACHEALIGN | KM_SLEEP);
    if (k0buf == NULL)
        return NULL;

    phys = PVNET_KVTOPHYS(k0buf);
    *phys_out = phys;

    /* Return KSEG1 (uncached) virtual for CPU access — avoids cache aliasing */
    return (char *)PHYS_TO_K1(phys);
}

/*
 * pvnet_init - bring up the interface.
 * Called by the ether layer when the interface is ifconfig'd up.
 */
static int
pvnet_init(struct etherif *eif, int flags)
{
    struct pvnet_info *pi = eiftoei(eif);

    /* Reset device, clear pending interrupts */
    PVNET_REG64(PVNET_CMD) = PVNET_CMD_RESET;
    PVNET_REG64(PVNET_INTR_STATUS) = ~(__uint64_t)0;   /* W1C: clear all */

    /* Allocate RX DMA buffer if not yet done */
    if (pi->pi_rxbuf == NULL) {
        pi->pi_rxbuf = pvnet_alloc_dma_buf(PVNET_MAX_FRAME, &pi->pi_rxphys);
        if (pi->pi_rxbuf == NULL) {
            cmn_err(CE_WARN, "pvnet%d: cannot allocate RX buffer", pi->pi_unit);
            return ENOBUFS;
        }
    }

    /* Allocate TX DMA buffer if not yet done */
    if (pi->pi_txbuf == NULL) {
        pi->pi_txbuf = pvnet_alloc_dma_buf(PVNET_MAX_FRAME + EHDR_LEN,
                                            &pi->pi_txphys);
        if (pi->pi_txbuf == NULL) {
            cmn_err(CE_WARN, "pvnet%d: cannot allocate TX buffer", pi->pi_unit);
            return ENOBUFS;
        }
    }

    /* Arm RX: give device a buffer to fill */
    PVNET_REG64(PVNET_RX_BASE) = pi->pi_rxphys;
    PVNET_REG64(PVNET_RX_LEN)  = PVNET_MAX_FRAME;

    /* Polling mode: PVNET_INTR_MASK left at 0 (no GPIO assertion).
     * pvnet_intr() can enable interrupts via PROM IMR0 + c0vec_tbl wiring. */

    /* Poll every PVNET_POLL_TICKS */
    timeout(pvnet_poll, (caddr_t)pi, PVNET_POLL_TICKS);

    return 0;
}

/*
 * pvnet_reset - quiesce the device.
 * Called when the interface goes down or on error recovery.
 */
static void
pvnet_reset(struct etherif *eif)
{
    PVNET_REG64(PVNET_CMD) = PVNET_CMD_RESET;
    PVNET_REG64(PVNET_INTR_STATUS) = ~(__uint64_t)0;
}

/*
 * pvnet_watchdog - called by ether layer watchdog timer.
 * TX is synchronous so there is nothing to recover.
 */
static void
pvnet_watchdog(struct ifnet *ifp)
{
    /* No-op: synchronous TX cannot wedge */
}

/*
 * pvnet_transmit - send a packet described by the mbuf chain m.
 * Ethernet header is built from edst/esrc/type; m contains the payload.
 * TX is synchronous: we poll for TX_DONE before returning.
 */
static int
pvnet_transmit(
    struct etherif   *eif,
    struct etheraddr *edst,
    struct etheraddr *esrc,
    u_short           type,
    struct mbuf      *m0)
{
    struct pvnet_info   *pi = eiftoei(eif);
    struct ether_header *eh;
    struct mbuf         *m;
    char                *p;
    int                  totlen;
    int                  tries;

    /* Build Ethernet header in TX buffer */
    eh = (struct ether_header *)pi->pi_txbuf;
    *(struct etheraddr *)eh->ether_dhost = *edst;
    *(struct etheraddr *)eh->ether_shost = *esrc;
    eh->ether_type = htons(type);

    /* Copy mbuf chain payload immediately after the header */
    p      = pi->pi_txbuf + EHDR_LEN;
    totlen = EHDR_LEN;

    for (m = m0; m; m = m->m_next) {
        if (m->m_len == 0)
            continue;
        if (totlen + m->m_len > PVNET_MAX_FRAME + EHDR_LEN) {
            cmn_err(CE_WARN, "pvnet%d: packet too large (%d bytes)",
                    pi->pi_unit, totlen + m->m_len);
            m_freem(m0);
            return EMSGSIZE;
        }
        bcopy(mtod(m, caddr_t), p, m->m_len);
        p      += m->m_len;
        totlen += m->m_len;
    }
    m_freem(m0);

    /* Program DMA registers and kick TX */
    PVNET_REG64(PVNET_TX_BASE) = pi->pi_txphys;
    PVNET_REG64(PVNET_TX_LEN)  = (__uint64_t)totlen;
    PVNET_REG64(PVNET_CMD)     = PVNET_CMD_TX_START;

    /* Poll for completion — virtual device responds immediately */
    for (tries = 100000; tries > 0; tries--) {
        if (PVNET_REG64(PVNET_INTR_STATUS) & PVNET_INTR_TX_DONE)
            break;
    }
    /* Clear TX_DONE bit (W1C) */
    PVNET_REG64(PVNET_INTR_STATUS) = PVNET_INTR_TX_DONE;

    return 0;
}

/*
 * pvnet_ioctl - handle ioctl requests.
 * Only the ether layer's standard ioctls are needed; we rely on the
 * etherifops dispatch for those.
 */
static int
pvnet_ioctl(struct etherif *eif, int cmd, void *data)
{
    return EINVAL;
}

/*
 * pvnet_poll - periodic RX check, called via timeout().
 *
 * Drain loop: while INTR_STATUS shows RX_DONE, ack, copy the frame,
 * deliver via ether_input(), and re-arm.  The RX_LEN re-arm write makes
 * QEMU flush its queued-packet backlog synchronously (under the BQL),
 * so the next queued packet is already delivered by the time we re-read
 * INTR_STATUS — the whole SLIRP backlog drains in ONE poll instead of
 * one packet per 20ms tick.  Budget bounds time at splimp.
 * Reschedules itself unconditionally.
 */
#define PVNET_RX_BUDGET 32

static void
pvnet_poll(struct pvnet_info *pi)
{
    struct etherif      *eif = &pi->pi_eif;
    int                  rawlen, paylen;
    int                  budget = PVNET_RX_BUDGET;
    struct mbuf         *m;
    void                *rbp;

    while ((PVNET_REG64(PVNET_INTR_STATUS) & PVNET_INTR_RX_DONE)
           && --budget >= 0) {
        /* Acknowledge RX_DONE */
        PVNET_REG64(PVNET_INTR_STATUS) = PVNET_INTR_RX_DONE;

        rawlen = (int)PVNET_REG64(PVNET_RX_ACTUAL);

        if (rawlen >= EHDR_LEN && rawlen <= PVNET_MAX_FRAME) {
            /*
             * rawlen = ETHER_HDRLEN (14) + IP payload
             * paylen = bytes after ethernet header
             *
             * IRIX ether_input() expects an mbuf where:
             *   - The first sizeof(etherbufhead) bytes are the ifheader area
             *   - The ethernet header sits at the LAST EHDR_LEN bytes of
             *     the etherbufhead (i.e. offset sizeof(ebh) - EHDR_LEN)
             *   - Payload follows immediately after
             *
             * We copy the raw frame (eth_hdr+payload) so that the ethernet
             * header lands at the correct offset within the mbuf.
             */
            paylen = rawlen - EHDR_LEN;
            m = m_vget(M_DONTWAIT,
                       (int)(sizeof(struct etherbufhead) + paylen),
                       MT_DATA);
            if (m != NULL) {
                rbp = mtod(m, void *);

                /* Initialize the ifheader area */
                IF_INITHEADER(rbp, &eif->eif_arpcom.ac_if,
                              sizeof(struct etherbufhead));

                /*
                 * Copy raw frame so that the Ethernet header lands at
                 * the last EHDR_LEN bytes of the etherbufhead region,
                 * and the payload follows.
                 */
                bcopy(pi->pi_rxbuf,
                      (caddr_t)rbp + sizeof(struct etherbufhead) - EHDR_LEN,
                      rawlen);

                m->m_len = (int)(sizeof(struct etherbufhead) + paylen);

                eif->eif_arpcom.ac_if.if_ipackets++;
                eif->eif_arpcom.ac_if.if_ibytes += paylen;

                ether_input(eif, 0, m);
            } else {
                eif->eif_arpcom.ac_if.if_ierrors++;
            }
        }

        /*
         * Re-arm RX buffer.  Side effect (QEMU): flushes the queued
         * packet backlog, potentially re-raising RX_DONE immediately.
         */
        PVNET_REG64(PVNET_RX_BASE) = pi->pi_rxphys;
        PVNET_REG64(PVNET_RX_LEN)  = PVNET_MAX_FRAME;
    }

    /* Reschedule polling */
    timeout(pvnet_poll, (caddr_t)pi, PVNET_POLL_TICKS);
}

/*
 * pvnet_intr - interrupt handler for pvnet RX/TX completion.
 *
 * Called via c0vec_tbl[4] trampoline when HEART ISR bit 20 fires
 * (LEVEL1 → IP4).
 *
 * We CANNOT call ether_input() from hardware interrupt context on IRIX —
 * network semaphores aren't safe at this priority level.  Instead, we
 * just acknowledge the device interrupt (W1C INTR_STATUS) to deassert
 * the GPIO/IRQ line, and schedule an immediate pvnet_poll() via
 * timeout(1 tick) to process the packet at splnet.
 */
void
pvnet_intr(intr_arg_t arg)
{
    struct pvnet_info   *pi = &pvnet_info_table[0];
    __uint64_t           istat;

    istat = PVNET_REG64(PVNET_INTR_STATUS);

    if (istat) {
        /* Mask device interrupts to deassert GPIO (→ ISR bit cleared by
         * trampoline CLR_ISR).  Leave INTR_STATUS set so pvnet_poll sees
         * the pending RX_DONE and processes the packet at safe spl.
         * pvnet_poll re-enables the mask after processing.
         *
         * Do NOT call timeout() here — IRIX callout locking is not safe
         * at this hardware interrupt priority level.  The safety poll
         * timer (PVNET_POLL_TICKS) will pick up the packet within 1s. */
        PVNET_REG64(PVNET_INTR_MASK) = 0;
    }
}

/*
 * if_pvnetedtinit - probe and attach the pvnet device.
 *
 * Called once at boot because the master.d flags include 's' (soft).
 * A NULL edtp is acceptable (no EDT entry for virtual devices).
 */
/* ARGSUSED */
void
if_pvnetedtinit(struct edt *edtp)
{
    struct pvnet_info *pi = &pvnet_info_table[0];
    struct etheraddr   ea;
    __uint64_t         mac_hi, mac_lo;

    /* Probe: device is present only on IP54 machines */
    if (badaddr((void *)PVNET_BASE, sizeof(__uint64_t))) {
        /* Not present — must be running on a different machine */
        return;
    }

    pi->pi_unit   = 0;
    pi->pi_rxbuf  = NULL;
    pi->pi_txbuf  = NULL;
    pi->pi_rxphys = 0;
    pi->pi_txphys = 0;

    /* Read 6-byte MAC address from device registers */
    mac_hi = PVNET_REG64(PVNET_MAC_HI);   /* bytes [0:1] in low 16 bits */
    mac_lo = PVNET_REG64(PVNET_MAC_LO);   /* bytes [2:5] in low 32 bits */

    ea.ea_vec[0] = (u_char)((mac_hi >> 8) & 0xff);
    ea.ea_vec[1] = (u_char)(mac_hi & 0xff);
    ea.ea_vec[2] = (u_char)((mac_lo >> 24) & 0xff);
    ea.ea_vec[3] = (u_char)((mac_lo >> 16) & 0xff);
    ea.ea_vec[4] = (u_char)((mac_lo >> 8) & 0xff);
    ea.ea_vec[5] = (u_char)(mac_lo & 0xff);

    pi->pi_addr = ea;

    /*
     * Attach to the IRIX ethernet layer.
     * "pvnet" is the interface name prefix (→ pvnet0).
     * eif_private points back to our pvnet_info for eiftoei().
     */
    ether_attach(&pi->pi_eif, "pvnet", 0, (caddr_t)pi,
                 &pvnetops, &ea, INV_ETHER_EF, 0);

    cmn_err(CE_NOTE,
            "pvnet0: IP54 paravirtual ethernet, "
            "addr %02x:%02x:%02x:%02x:%02x:%02x",
            ea.ea_vec[0], ea.ea_vec[1], ea.ea_vec[2],
            ea.ea_vec[3], ea.ea_vec[4], ea.ea_vec[5]);

    /* Wire pvnet interrupt via HEART — must be after heart_mlreset() */
    ip54_intr_init();
}

#endif /* IP54 */
