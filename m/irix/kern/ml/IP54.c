/*
 * IP54.c - IP54 Paravirtual SGI Workstation board support
 *
 * IP54 is a purpose-built QEMU paravirtual SGI workstation.  It uses
 * the HEART interrupt controller (same as IP30/Octane) as its substrate
 * but replaces all real hardware peripherals with a paravirtual device
 * bank (sgi-smp, sgi-pvmem, sgi-pvnet, sgi-glaccel, sgi-pvaudio).
 *
 * This file replaces IP30.c for the IP54 target.  The guiding principle
 * is: only emulate what cannot be virtualized.  All I/O goes through
 * the paravirtual register interfaces.
 *
 * Modeled on: irix/kern/ml/RACER/IP30.c
 *
 * Copyright 1996-2024, Silicon Graphics, Inc. / QEMU IP54 project.
 */
#ident "$Revision: 1.0 $"

#if IP54

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/cmn_err.h>
#include <sys/sbd.h>
#include <sys/pda.h>
#include <sys/proc.h>
#include <sys/runq.h>
#include <sys/invent.h>
#include <sys/kopt.h>
#include <sys/syssgi.h>
#include <sys/fpu.h>
#include <sys/conf.h>
#include <sys/callo.h>
#include <sys/debug.h>
#include <sys/sysinfo.h>
#include <sys/iograph.h>
#include <sys/atomic_ops.h>
#include <sys/clksupport.h>
#include <sys/cpu.h>
#include <sys/IP54addrs.h>
#include <sys/RACER/heart.h>
/* heartio.h and arcs headers not installed on build system — declare what we need */
extern char *arcs_getenv(const char *name);

/* earlybadaddr: safe version of badaddr() usable before curthreadp is set up */
extern int earlybadaddr(volatile void *, int);

/* -----------------------------------------------------------------------
 * Machine identification
 * ---------------------------------------------------------------------- */

short cputype __attribute__((section(".sdata"))) = 54;             /* xx in IPxx */
static uint sys_id;             /* serial number from ethernet MAC */

int maxcpus __attribute__((section(".sdata"))) = 1;    /* IP54: single CPU at boot; _bclean_caches needs UP path */

pdaindr_t pdaindr[MAXCPU];
int processor_enabled[MAXCPU];

char slave_loop_ready[MAXCPU];
static volatile int cb_wait = -1;
int nmi_maxcpus = 0;

extern int intstacksize;

/* IP54 HEART PIU base (same physical location as IP30) */
heart_piu_t *heart_piu = (heart_piu_t *)PHYS_TO_K1(IP54_HEART_BASE);

/* -----------------------------------------------------------------------
 * IRIX identification predicates
 * ---------------------------------------------------------------------- */

int
is_octane(void)
{
    return 0;
}

int
is_fullhouse(void)
{
    return 0;
}

int
is_ip54(void)
{
    return 1;
}

/* -----------------------------------------------------------------------
 * pvmem: read actual RAM size from the paravirtual memory descriptor
 * ---------------------------------------------------------------------- */

/*
 * ip54_pvmem_total_ram - return RAM size in bytes as reported by sgi-pvmem.
 *
 * sgi-pvmem register TOTAL_RAM is a 64-bit value at PHYS_TO_K1(IP54_PV_MEM).
 * Returns the value or 64MB as a safe fallback.
 */
static __uint64_t
ip54_pvmem_total_ram(void)
{
    volatile __uint64_t *pvmem = (volatile __uint64_t *)PHYS_TO_K1(IP54_PV_MEM);
    __uint64_t total;

    if (earlybadaddr((volatile void *)pvmem, sizeof(__uint64_t)))
        return 64ULL * 1024 * 1024;    /* safe fallback: 64MB */

    total = pvmem[0];   /* offset 0x00: TOTAL_RAM */
    if (total == 0)
        return 64ULL * 1024 * 1024;

    return total;
}

/* -----------------------------------------------------------------------
 * Early machine reset
 * ---------------------------------------------------------------------- */

/* table of probeable kmem addresses */
struct kmem_ioaddr kmem_ioaddr[] = {
    { PHYS_TO_K1(IP54_PV_NET),    0x100 },
    { PHYS_TO_K1(IP54_PV_GLACCEL), 0x100 },
    { PHYS_TO_K1(IP54_PV_AUDIO),  0x100 },
    { 0, 0 },
};

/*
 * Temporary ring buffer for cmn_err output before setup_lowmem() runs.
 *
 * The kernel clears BSS at startup (before mlreset is called), so this
 * array is safely zeroed.  mlreset() points putbuf at it so that any
 * cmn_err() call between BSS-clear and setup_lowmem() writes here
 * instead of to NULL (VA 0, KUSEG) which would cause a TLBS exception.
 * setup_lowmem() overwrites putbuf/putbufsz with real allocations.
 */
extern char *putbuf;        /* defined in os/printf.c */
extern int   putbufsz;      /* defined in os/printf.c */
static char  ip54_early_putbuf[4096];   /* BSS: zeroed before mlreset() */

/*
 * mlreset - very early machine reset.
 * Called before interrupts, before paging, before malloc.
 * is_slave: 0 for boot CPU, 1 for secondary CPUs.
 */
void
mlreset(int is_slave)
{
    if (is_slave) {
        /* Secondary CPUs: minimal init, wait for signal from master */
        return;
    }

    /*
     * IP54: no real BRIDGE/IOC3/XBOW init needed.
     * HEART is the only real chipset; it is initialised by the PROM.
     * The paravirtual devices are ready at boot.
     *
     * Pre-initialise putbuf so that any cmn_err() call between here and
     * setup_lowmem() does not crash with a NULL dereference.  BSS clearing
     * happens BEFORE mlreset() so ip54_early_putbuf is safely zero-filled.
     * setup_lowmem() will overwrite putbuf/putbufsz with proper values.
     */
    putbuf   = ip54_early_putbuf;
    putbufsz = sizeof(ip54_early_putbuf);
}

/*
 * cpuboard_name - return a human-readable board name.
 */
char *
cpuboard_name(void)
{
    return "IP54 Paravirtual SGI";
}

/* -----------------------------------------------------------------------
 * System ID / serial number (derived from pvnet MAC address)
 * ---------------------------------------------------------------------- */

#define ISXDIGIT(c) \
    ((('a' <= (c)) && ((c) <= 'f')) || (('0' <= (c)) && ((c) <= '9')))
#define HEXVAL(c) \
    ((('0' <= (c)) && ((c) <= '9')) ? ((c) - '0') : ((c) - 'a' + 10))

char eaddr[6];

static unsigned char *
etoh(char *enet)
{
    static unsigned char dig[6];
    unsigned char *cp;
    int i;

    for (i = 0, cp = (unsigned char *)enet; *cp; ) {
        if (*cp == ':') { cp++; continue; }
        if (!ISXDIGIT(*cp) || !ISXDIGIT(*(cp + 1)))
            return NULL;
        if (i >= 6) return NULL;
        dig[i++] = (HEXVAL(*cp) << 4) + HEXVAL(*(cp + 1));
        cp += 2;
    }
    return (i == 6) ? dig : NULL;
}

static void
init_sysid(void)
{
    char *cp;
    unsigned char *ep;

    cp = (char *)arcs_getenv("eaddr");
    if (cp == NULL || (ep = etoh(cp)) == NULL) {
        /*
         * Fall back: read MAC directly from pvnet registers.
         * MAC_HI at offset 0x40, MAC_LO at 0x48.
         */
        volatile __uint64_t *pvnet = (volatile __uint64_t *)PHYS_TO_K1(IP54_PV_NET);
        __uint64_t mac_hi, mac_lo;
        if (!earlybadaddr((volatile void *)pvnet, sizeof(__uint64_t))) {
            mac_hi = pvnet[8];   /* offset 0x40 = index 8 */
            mac_lo = pvnet[9];   /* offset 0x48 = index 9 */
            eaddr[0] = (mac_hi >> 8) & 0xff;
            eaddr[1] =  mac_hi       & 0xff;
            eaddr[2] = (mac_lo >> 24) & 0xff;
            eaddr[3] = (mac_lo >> 16) & 0xff;
            eaddr[4] = (mac_lo >> 8)  & 0xff;
            eaddr[5] =  mac_lo        & 0xff;
        } else {
            bzero(eaddr, 6);
        }
    } else {
        bcopy(ep, eaddr, 6);
    }

    sys_id = ((uint)eaddr[2] << 24) | ((uint)eaddr[3] << 16) |
             ((uint)eaddr[4] << 8)  |  (uint)eaddr[5];
}

int
getsysid(char *hostident)
{
    *(uint *)hostident = sys_id;
    bzero(hostident + 4, MAXSYSIDSIZE - 4);
    return 0;
}

/* -----------------------------------------------------------------------
 * Memory initialisation
 * ---------------------------------------------------------------------- */

/*
 * ip54_init_memory - tell the IRIX VM subsystem about installed RAM.
 *
 * For IP54 the pvmem device reports the total RAM size.  We register
 * a single contiguous memory segment starting at IP54_RAM_BASE.
 */
void
ip54_init_memory(void)
{
    __uint64_t total_ram = ip54_pvmem_total_ram();
    __uint64_t ram_pages = btoct(total_ram);

    /*
     * NOTE: cmn_err() cannot be called here.  setup_lowmem() has not yet
     * allocated the ring buffers (putbuf/conbuf/errbuf); any cmn_err call
     * before that causes a NULL-dereference of putbuf and a double panic.
     * Use ip54_pvmem_total_ram() return value silently; the caller (mlsetup)
     * will log memory configuration once the buffers are ready.
     */
    /*
     * Register physical memory with the IRIX VM subsystem.
     * physmem_add() or physstk_add() is the appropriate IRIX call.
     * The actual call depends on the IRIX version; this is a stub.
     * The PROM ARCS memory descriptors may do this before the kernel
     * initialisation if ip54prom is configured correctly.
     */
    /* physmem_add(IP54_RAM_BASE >> PNUMSHIFT, ram_pages); */
    (void)ram_pages;
}

/* -----------------------------------------------------------------------
 * Interrupt initialisation
 * ---------------------------------------------------------------------- */

/*
 * ip54_intr_init - wire paravirtual device interrupts via HEART.
 *
 * HEART ISR bit 20 → IP4 → heart_intr_med → pvnet_intr
 * HEART ISR bit 21 → IP4 → heart_intr_med → (future: pvfb_intr)
 * HEART ISR bit 22 → IP4 → heart_intr_med → (future: pvaudio_intr)
 *
 * heart_intr_preconn() sets heart_ivec[vec].hv_func and enables the
 * IMR bit on the current (boot) CPU.  The HEART shim in QEMU then
 * asserts CPU IP4 whenever ISR & IMR[0] has LEVEL1 bits set.
 */
extern void pvnet_intr(intr_arg_t);

void
ip54_intr_init(void)
{
    /*
     * TODO: Wire pvnet interrupt via HEART vector 20.
     *
     * The QEMU HEART shim supports IRQ interposition (ISR bits 20-22,
     * GPIO inputs from PV devices, edge-detected update_irq).
     * Kernel-side wiring requires setting heart_ivec[20].hv_func and
     * enabling IMR[0] bit 20.  Deferred until heart_ivec_t struct size
     * can be determined (heart_vec.h has deep header dependencies).
     *
     * For now, pvnet uses polling (PVNET_POLL_TICKS=2, ~20ms).
     */
}

/* -----------------------------------------------------------------------
 * SMP: secondary CPU bringup via sgi-smp paravirtual device
 * ---------------------------------------------------------------------- */

#if MAXCPU > 1

/*
 * ip54_launch_slave - kick secondary CPU n via sgi-smp BOOT registers.
 * The slave starts executing at 'entry' (physical or KSEG0 address).
 */
void
ip54_launch_slave(int cpuid, __uint64_t entry)
{
    volatile __uint64_t *smp = (volatile __uint64_t *)PHYS_TO_K1(IP54_PV_SMP);

    /* BOOT_ADDR at offset 0x28, BOOT_GO at offset 0x30 */
    smp[5] = entry;         /* BOOT_ADDR */
    smp[6] = (__uint64_t)cpuid; /* BOOT_GO: writing cpuid kicks that CPU */
}

#endif /* MAXCPU > 1 */

/* -----------------------------------------------------------------------
 * Hardware inventory
 * ---------------------------------------------------------------------- */

/*
 * ip54_add_inventory - register IP54 hardware in IRIX hinv database.
 */
void
ip54_add_inventory(void)
{
    add_to_inventory(INV_PROCESSOR, INV_CPUBOARD, 0, 0, IP54);
    add_to_inventory(INV_NETWORK, INV_NET_ETHER, 0, 0, INV_ETHER_EF);
}

/* -----------------------------------------------------------------------
 * Clock support
 * ---------------------------------------------------------------------- */

/*
 * IP54 uses the HEART COMPARE timer for the scheduling clock,
 * identical to IP30.  The actual timer initialisation is in
 * ml/RACER/hwtimer.c which is shared.
 */

/* -----------------------------------------------------------------------
 * Cycle counter / fast clock
 * ---------------------------------------------------------------------- */

/*
 * The HEART h_count register is the system cycle counter on both
 * IP30 and IP54.  ip30_count_cycles() in hwtimer.c handles this.
 */

/* -----------------------------------------------------------------------
 * NMI / panic
 * ---------------------------------------------------------------------- */

void
ip54_nmi_handler(void)
{
    cmn_err(CE_PANIC, "IP54: NMI received — system halted");
}

#endif /* IP54 */
