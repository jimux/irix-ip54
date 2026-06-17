/*
 * pvuart_cn.c - IP54 Paravirtual UART console driver
 *
 * Implements the du_* symbols that cn.c calls by name, backed by the
 * sgi-pvuart device at physical 0x1F620178.  This module replaces
 * sduart (zduart.c / Z85130) for the IP54 paravirtual machine.
 *
 * sgi-pvuart register layout (MMIO, big-endian, 8-byte stride):
 *   +0  (byte 3): TX data register (write) / RX data register (read)
 *   +4  (byte 6): Line Status Register (read-only)
 *                   bit 0: DR   - Data Ready (RX byte available)
 *                   bit 5: THRE - TX Holding Register Empty (ready to send)
 *
 * The register access uses byte offsets within an 8-byte window:
 *   PVUART_BASE + 3  = THR/RBR  (byte offset into 64-bit register)
 *   PVUART_BASE + 6  = LSR
 *
 * lboot PREFIX=du exports:
 *   duinfo     (struct streamtab)
 *   dudevflag  (int)
 * and calls:
 *   du_init()  at boot
 *
 * cn.c externs:
 *   du_init()
 *   ducons_write()
 *   ducons_flush()
 *   du_putchar()
 *   du_getchar()
 *   du_conpoll()
 *   ducons_read()
 *   get_cons_dev()
 *
 * Modeled on: irix/kern/io/zduart.c (Revision 1.150)
 * See also:   irix/kern/io/uart16550.c, irix/kern/master.d/sduart
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
#include "sys/sbd.h"
#include "sys/cpu.h"
#include "sys/conf.h"
#include "sys/stream.h"
#include "sys/strids.h"
#include "sys/strmp.h"
#include "sys/stropts.h"
#include "sys/stty_ld.h"
#include "sys/termio.h"
#include "sys/ddi.h"
#include "sys/cred.h"
#include "sys/kmem.h"
#include "sys/hwgraph.h"

/*
 * sgi-pvuart MMIO base (KSEG1 uncached).
 * Physical address: 0x1F620178
 * The device has an 8-byte register window. Byte offsets are used for
 * the data and status registers because IRIX runs big-endian.
 */
/*
 * KSEG1 uncached address = physical | 0xA0000000
 * PHYS_TO_K1 macro is not reliably available in all include configurations,
 * so we use the literal KSEG1 address directly.
 */
#define PVUART_BASE         0xBF620178ULL
#define PVUART_DATA         (*(volatile u_char *)(PVUART_BASE + 3))
#define PVUART_LSR          (*(volatile u_char *)(PVUART_BASE + 6))

/* Line Status Register bits */
#define PVUART_LSR_DR       0x01    /* bit 0: RX data ready */
#define PVUART_LSR_THRE     0x20    /* bit 5: TX holding register empty */

/*
 * 8042 keyboard/mouse controller (sgi-ioc2-kbd QEMU device) status
 * register, KSEG1 uncached.  Same address the stock pckm driver uses
 * (KB_REG_64 = HPC3_KBD_MOUSE1 + 3 = PA 0x1FBD9847).
 */
#define PCKM_8042_STATUS    (*(volatile u_char *)0xBFBD9847UL)
#define PCKM_8042_PENDING   0x21    /* SR_OBF | SR_MSFULL */

/*
 * lcl2vec_tbl entry layout, verified against the kernel.o binary
 * (setlclvector disassembly: entry address = lcl2vec_tbl + 0x20 +
 * (vec & 7) * 32; fields at +0 isr, +4 arg, +8 bit, +0xc thd_flags).
 * VECTOR_KBDMS = 20, so pckm_intr is installed at lcl2vec_tbl[5].
 *
 * Declared locally instead of via sys/mc.h because the real lclvec_t
 * embeds thd_int_t whose size depends on kernel compile flags; the
 * 32-byte stride below is what the linked kernel.o actually uses.
 */
typedef struct ip54_lclvec {
    void (*isr)(__psint_t, void *); /* interrupt service routine */
    __psint_t arg;                  /* arg to pass */
    unsigned int bit;               /* corresponding bit in sr */
    int thd_flags;                  /* thd_int_t info (unused here) */
    char _pad[16];                  /* rest of thd_int_t */
} ip54_lclvec_t;

extern ip54_lclvec_t lcl2vec_tbl[]; /* kernel.o global */
extern int pckm_kbdexists(void);    /* pckm.o global */

/* Major device number — same as sduart (see master.d/sduart SOFT=260) */
#define DU_MAJOR            260

/* Console port index (port 0 = primary ttyd1) */
#define DU_CONSOLE_PORT     0
#define DU_NPORTS           2       /* ttyd1 and ttyd2 */

/* Character that triggers kernel debugger entry when kdebug is set */
#define DEBUG_CHAR          0x01    /* Ctrl-A */

/*
 * Forward declarations for STREAMS entry points.
 */
static int du_open(queue_t *, dev_t *, int, int, struct cred *);
static int du_close(queue_t *, int, struct cred *);
static int du_wput(queue_t *, mblk_t *);
static int du_rsrv(queue_t *);
static void du_poll(void);
static void du_pckm_poll(void);

/*
 * STREAMS module info — matches sduart (STRID_DUART).
 */
static struct module_info dum_info = {
    STRID_DUART,    /* module ID */
    "DUART",        /* module name */
    0,              /* minimum packet size */
    INFPSZ,         /* maximum packet size — infinite */
    128,            /* hi-water mark */
    16,             /* lo-water mark */
};

static struct qinit du_rinit = {
    NULL, (int (*)())du_rsrv, du_open, du_close, NULL, &dum_info, NULL
};
static struct qinit du_winit = {
    du_wput, NULL, NULL, NULL, NULL, &dum_info, NULL
};

/*
 * Exported STREAMS table and device flag.
 * lboot PREFIX=du exports these as duinfo and dudevflag.
 */
int dudevflag = 0;
struct streamtab duinfo = {
    &du_rinit, &du_winit, NULL, NULL
};

/* dev_t for each console port — populated in du_init() */
static dev_t cons_devs[DU_NPORTS];

/* external kdebug flag — declared as short in sys/systm.h */
extern short kdebug;

/* RX poll timer state — schedules qenable() on read queue every 2 ticks */
static toid_t du_poll_id;
static queue_t *du_poll_rq;

/*
 * Default termio settings returned for TCGETA.
 * Matches def_stty_ld.st_termio from stty_ld.c.
 */
static struct termio du_termio = {
    ICRNL|IXON|IXANY|BRKINT|IGNPAR|ISTRIP,	/* c_iflag */
    OPOST|ONLCR|TAB3,				/* c_oflag */
    B9600|CS8|HUPCL|CREAD|CLOCAL,		/* c_cflag */
    ISIG|ICANON|ECHO|ECHOE|ECHOK,		/* c_lflag */
    B9600,					/* c_ospeed */
    B9600,					/* c_ispeed */
    0,						/* c_line  */
    /* c_cc[]: CINTR, CQUIT, CERASE, CKILL, CEOF, CEOL, CEOL2, CSWTCH */
    { 0177, 034, 010, 025, 04, 0, 0, 0 },
};

/*
 * ---------------------------------------------------------------------------
 * Low-level polled I/O helpers
 * ---------------------------------------------------------------------------
 */

/*
 * pvuart_txrdy - return non-zero if the TX holding register is empty.
 */
static int
pvuart_txrdy(void)
{
    return (PVUART_LSR & PVUART_LSR_THRE) != 0;
}

/*
 * pvuart_rxrdy - return non-zero if a received byte is available.
 */
static int
pvuart_rxrdy(void)
{
    return (PVUART_LSR & PVUART_LSR_DR) != 0;
}

/*
 * pvuart_putc - write a single byte to the TX register, spinning until ready.
 */
static void
pvuart_putc(u_char c)
{
    while (!pvuart_txrdy())
        ;
    PVUART_DATA = c;
}

/*
 * pvuart_getc - read a single byte if one is available, else return -1.
 */
static int
pvuart_getc(void)
{
    if (!pvuart_rxrdy())
        return -1;
    return (int)(PVUART_DATA & 0xff);
}

/*
 * ---------------------------------------------------------------------------
 * Console interface — called by cn.c
 * ---------------------------------------------------------------------------
 */

/*
 * du_init - hardware initialisation, called from cn_init().
 *
 * sgi-pvuart is self-initialising in QEMU (no hardware reset sequence
 * required).  We populate cons_devs[] for get_cons_dev().
 */
void
du_init(void)
{
    cons_devs[0] = makedevice(DU_MAJOR, 0); /* ttyd1 — primary console */
    cons_devs[1] = makedevice(DU_MAJOR, 1); /* ttyd2 — secondary */
}

/*
 * ducons_write - polled write to console, callable from interrupt context.
 *
 * Inserts a CR before each LF (matching zduart.c behaviour for terminals
 * that require CR+LF line endings).
 *
 * Returns the number of bytes written (cn.c declares it void but
 * discards the return value; zduart.c returns int).
 */
int
ducons_write(u_char *buf, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        pvuart_putc(buf[i]);
        if (buf[i] == '\n')
            pvuart_putc('\r');
    }
    return len;
}

/*
 * ducons_flush - flush console TX.
 *
 * sgi-pvuart has no FIFO; a write completes synchronously, so this
 * is a no-op (matching zduart.c: void ducons_flush(void) {}).
 */
void
ducons_flush(void)
{
}

/*
 * ducons_read - polled read from console, drains all available bytes.
 * Returns the number of bytes actually read.
 */
int
ducons_read(u_char *buf, int len)
{
    int i = 0;
    int c;

    while (i < len) {
        if ((c = pvuart_getc()) == -1)
            break;
        *buf++ = (u_char)c;
        i++;
    }
    return i;
}

/*
 * du_getchar - polled read from a named port (0 = ttyd1, 1 = ttyd2).
 * Returns -1 if no character is available.
 */
int
du_getchar(int port)
{
    /* IP54 has one physical UART; port argument is ignored */
    return pvuart_getc();
}

/*
 * du_putchar - polled write of a single byte to a named port.
 */
void
du_putchar(int port, unsigned char c)
{
    pvuart_putc(c);
}

/*
 * du_conpoll - poll for kernel debugger entry character (Ctrl-A).
 *
 * Called from spinlock-contention paths when kdebug is set.
 * Tossing everything but DEBUG_CHAR is intentional (matching zduart.c).
 */
void
du_conpoll(void)
{
    int c;

    if (!kdebug)
        return;

    if ((c = pvuart_getc()) != -1) {
        if ((c & 0xff) == DEBUG_CHAR)
            debug("ring");
    }
}

/*
 * get_cons_dev - return the dev_t for console port 1 (ttyd1) or 2 (ttyd2).
 * which=1 → primary console; which=2 → secondary console.
 */
dev_t
get_cons_dev(int which)
{
    ASSERT(which == 1 || which == 2);
    return cons_devs[which - 1];
}

/*
 * tcgeta - copy current termio parameters into an ioctl reply message.
 *
 * Called by stty_ld and pts.a for TCGETA ioctls.  The reply message
 * block must already have a b_cont allocation large enough for
 * struct termio (STERMIO assumes this).
 *
 * STERMIO(bp) is defined in sys/stty_ld.h as:
 *   ((struct termio*)(bp)->b_cont->b_rptr)
 */
void
tcgeta(queue_t *wq, mblk_t *bp, struct termio *p)
{
    struct iocblk *iocp = (struct iocblk *)bp->b_rptr;
    if (!bp->b_cont) {
        bp->b_cont = allocb(sizeof(struct termio), BPRI_MED);
        if (!bp->b_cont) {
            bp->b_datap->db_type = M_IOCNAK;
            iocp->ioc_error = ENOMEM;
            qreply(wq, bp);
            return;
        }
        bp->b_cont->b_wptr += sizeof(struct termio);
    }
    *STERMIO(bp) = *p;
    bp->b_datap->db_type = M_IOCACK;
    iocp->ioc_count = sizeof(struct termio);
    qreply(wq, bp);
}

/*
 * du_lateinit - late device initialisation, called from main.c after hwgraph
 * is set up (du_init is called before hwgraph init time).
 *
 * Creates /hw/ttys/ttyd1 and /hw/ttys/ttyd2 hwgraph nodes so that the
 * on-disk symlinks /dev/ttyd1 -> /hw/ttys/ttyd1 resolve correctly.
 * Updates cons_devs[] with the hwgraph dev_t values.
 */
void
du_lateinit(void)
{
    vertex_hdl_t ttys_vhdl, port_vhdl;
    graph_error_t rv;

    /* Create /hw/ttys */
    rv = hwgraph_path_add(hwgraph_root, "ttys", &ttys_vhdl);
    if (rv != GRAPH_SUCCESS) {
        cmn_err(CE_WARN, "pvuart: cannot create /hw/ttys (err %d)", rv);
        return;
    }

    /* Create /hw/ttys/ttyd1 — primary console */
    rv = hwgraph_char_device_add(ttys_vhdl, "ttyd1", "du", &port_vhdl);
    if (rv == GRAPH_SUCCESS) {
        hwgraph_chmod(port_vhdl, 0666);
        /*
         * Do NOT update cons_devs[0] here.  The original value
         * (makedevice(260,0) from du_init) is what cn.c expects.
         * Changing it to vhdl_to_dev() produces an hwgraph dev_t
         * that can cause init to block on console open.
         */
    } else {
        cmn_err(CE_WARN, "pvuart: cannot create ttyd1 (err %d)", rv);
    }

    /* Create /hw/ttys/ttyd2 — secondary port */
    rv = hwgraph_char_device_add(ttys_vhdl, "ttyd2", "du", &port_vhdl);
    if (rv == GRAPH_SUCCESS) {
        hwgraph_chmod(port_vhdl, 0666);
    } else {
        cmn_err(CE_WARN, "pvuart: cannot create ttyd2 (err %d)", rv);
    }
}

/*
 * ---------------------------------------------------------------------------
 * STREAMS driver entry points
 * ---------------------------------------------------------------------------
 *
 * This is a minimal polled STREAMS driver.  It allows /dev/ttyf01 (or
 * whatever hwgraph vertex lboot creates for major 260) to be opened and
 * written.  No interrupt-driven path or full line-discipline support is
 * implemented in V1; all writes go through ducons_write() synchronously.
 */

/*
 * du_open - STREAMS open.
 */
static int
du_open(queue_t *rq, dev_t *devp, int flag, int sflag, struct cred *crp)
{
    if (sflag)          /* only simple stream opens */
        return ENXIO;

    if (rq->q_ptr)      /* already open */
        return 0;

    /* Store port index (minor number) as queue private data */
    rq->q_ptr = WR(rq)->q_ptr = (caddr_t)(long)getminor(*devp);

    qprocson(rq);

    /* Start RX poll timer on first open */
    du_poll_rq = rq;
    if (!du_poll_id)
        du_poll_id = timeout((void (*)())du_poll, 0, 2);

    return 0;
}

/*
 * du_close - STREAMS close.
 */
static int
du_close(queue_t *rq, int flag, struct cred *crp)
{
    /* Cancel RX poll timer */
    if (du_poll_id) {
        untimeout(du_poll_id);
        du_poll_id = 0;
    }
    du_poll_rq = NULL;

    qprocsoff(rq);
    rq->q_ptr = WR(rq)->q_ptr = NULL;
    return 0;
}

/*
 * du_wput - STREAMS write put procedure.
 *
 * Handles M_DATA (pass through ducons_write), M_FLUSH, and discards
 * everything else.  This is a synchronous polled path; no service
 * procedure is used for write.
 */
static int
du_wput(queue_t *wq, mblk_t *bp)
{
    mblk_t *nbp;

    switch (bp->b_datap->db_type) {
    case M_DATA:
        for (nbp = bp; nbp; nbp = nbp->b_cont) {
            int len = nbp->b_wptr - nbp->b_rptr;
            if (len > 0)
                ducons_write(nbp->b_rptr, len);
        }
        freemsg(bp);
        break;

    case M_IOCTL: {
        struct iocblk *iocp = (struct iocblk *)bp->b_rptr;
        switch (iocp->ioc_cmd) {
        case TCGETA:
            tcgeta(wq, bp, &du_termio);
            break;
        case TCSETA:
        case TCSETAW:
        case TCSETAF:
            /* Accept but ignore — pvuart has no configurable baud/parity */
            bp->b_datap->db_type = M_IOCACK;
            iocp->ioc_count = 0;
            qreply(wq, bp);
            break;
        case TIOCGWINSZ: {
            /* Return 24x80 window size */
            struct winsize *ws;
            if (bp->b_cont)
                freemsg(bp->b_cont);
            bp->b_cont = allocb(sizeof(struct winsize), BPRI_MED);
            if (bp->b_cont) {
                ws = (struct winsize *)bp->b_cont->b_rptr;
                ws->ws_row = 24;
                ws->ws_col = 80;
                ws->ws_xpixel = 0;
                ws->ws_ypixel = 0;
                bp->b_cont->b_wptr += sizeof(struct winsize);
                bp->b_datap->db_type = M_IOCACK;
                iocp->ioc_count = sizeof(struct winsize);
                qreply(wq, bp);
            } else {
                bp->b_datap->db_type = M_IOCNAK;
                iocp->ioc_error = ENOMEM;
                qreply(wq, bp);
            }
            break;
        }
        default:
            bp->b_datap->db_type = M_IOCNAK;
            qreply(wq, bp);
            break;
        }
        break;
    }

    case M_FLUSH:
        if (*bp->b_rptr & FLUSHW)
            flushq(wq, FLUSHDATA);
        if (*bp->b_rptr & FLUSHR) {
            *bp->b_rptr &= ~FLUSHW;
            qreply(wq, bp);
        } else {
            freemsg(bp);
        }
        break;

    default:
        freemsg(bp);
        break;
    }

    return 0;
}

/*
 * du_rsrv - STREAMS read service procedure.
 *
 * Polled receive: drain any available bytes and pass upstream as M_DATA.
 * This is called from the scheduler; in V1 it is a simple poll, not
 * interrupt-driven.
 */
static int
du_rsrv(queue_t *rq)
{
    mblk_t *bp;
    int c;

    while (pvuart_rxrdy()) {
        if ((bp = allocb(1, BPRI_MED)) == NULL)
            break;
        c = pvuart_getc();
        if (c < 0) {
            freeb(bp);
            break;
        }
        /* CR→NL: belt-and-suspenders for ldterm ICRNL */
        if (c == '\r')
            c = '\n';
        *bp->b_wptr++ = (u_char)c;
        if (canput(rq->q_next))
            putnext(rq, bp);
        else {
            freemsg(bp);
            break;
        }
    }

    return 0;
}

/*
 * du_poll - periodic RX poll callback.
 *
 * Called from the kernel timeout subsystem every 2 clock ticks (~20ms at
 * 100Hz).  If data is available on the UART, schedules the read service
 * procedure via qenable().  This is the same polling pattern used by
 * zduart.c for its fallback path.
 */
static void
du_poll(void)
{
    if (du_poll_rq && pvuart_rxrdy())
        qenable(du_poll_rq);
    du_pckm_poll();
    du_poll_id = timeout((void (*)())du_poll, 0, 2);
}

/*
 * du_pckm_poll - drain the 8042 keyboard/mouse controller.
 *
 * IP54 has no INT3/LIO interrupt hardware, so pckm_intr is never
 * invoked by an interrupt.  Dispatch it from callout context instead
 * (mutex-safe: pckm's own pckm_reinit dtimeout runs the same code
 * path) whenever the 8042 status shows pending data.  pckm_intr reads
 * the status register itself and consumes one byte per call.
 *
 * Gates:
 *  - pckm_kbdexists(): the early_pckminit probe succeeded.
 *  - isr differs from the stray initializer: setlclvector(20, ...)
 *    has installed pckm_intr.  Entry 6 (vector 21) is never
 *    registered on IP54, so it still holds the static stray value
 *    to compare against.
 *
 * Budget 32 bytes per 20ms poll = 1.6 KB/s, well above combined
 * keyboard + 50Hz mouse demand; QEMU's 256-byte PS/2 queue absorbs
 * bursts.
 */
static void
du_pckm_poll(void)
{
    int budget = 32;

    if (!pckm_kbdexists() ||
        lcl2vec_tbl[5].isr == lcl2vec_tbl[6].isr)
        return;

    while ((PCKM_8042_STATUS & PCKM_8042_PENDING) && --budget)
        lcl2vec_tbl[5].isr(lcl2vec_tbl[5].arg, 0);
}

#endif /* IP54 */
