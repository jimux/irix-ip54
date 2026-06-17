/*
 * csu.IP54.s - IP54 Paravirtual SGI Workstation CPU startup
 *
 * Minimal startup shim for IP54.  The ip54prom ARCS firmware has already
 * cleared BSS, set up the TLB, and established a valid stack before
 * jumping to the kernel entry point (typically _start or kernel_entry).
 *
 * This file provides:
 *   ip54_smp_getcpuid()  - return hardware CPU ID via sgi-smp register
 *   ip54_smp_ipi_send()  - send an IPI to another CPU
 *   ip54_smp_ipi_clear() - clear our own IPI pending bit
 *
 * Modeled on: irix/kern/ml/IP22asm.s and irix/kern/ml/RACER/slave.s
 */

#ident "$Revision: 1.0 $"

#if IP54

#include <sys/asm.h>
#include <sys/regdef.h>
#include <sys/sbd.h>
#include <sys/cpu.h>
#include <sys/IP54addrs.h>

/*
 * ip54_smp_getcpuid - return the current CPU's hardware ID.
 *
 * Reads sgi-smp CPU_ID register at IP54_PV_SMP + 0x08.
 * Returns: v0 = CPU ID (0 = boot CPU)
 */
LEAF(ip54_smp_getcpuid)
    .set    noreorder
    LI      t0, IP54_PV_SMP + 0x08   /* SGI_SMP_CPU_ID */
    ld      v0, 0(t0)                 /* 64-bit load */
    j       ra
    nop
    .set    reorder
    END(ip54_smp_getcpuid)

/*
 * ip54_smp_ipi_send(cpuid) - send IPI to the given CPU.
 *
 * Writes cpuid bitmask to sgi-smp IPI_SET register.
 * a0 = target CPU ID
 */
LEAF(ip54_smp_ipi_send)
    .set    noreorder
    LI      t0, IP54_PV_SMP + 0x10   /* SGI_SMP_IPI_SET */
    li      t1, 1
    sll     t1, a0                    /* bitmask for target CPU */
    sd      t1, 0(t0)
    j       ra
    nop
    .set    reorder
    END(ip54_smp_ipi_send)

/*
 * ip54_smp_ipi_clear(cpuid) - acknowledge/clear IPI for the given CPU.
 *
 * Writes cpuid bitmask to sgi-smp IPI_CLEAR register.
 * a0 = CPU ID to clear (usually: our own)
 */
LEAF(ip54_smp_ipi_clear)
    .set    noreorder
    LI      t0, IP54_PV_SMP + 0x18   /* SGI_SMP_IPI_CLEAR */
    li      t1, 1
    sll     t1, a0
    sd      t1, 0(t0)
    j       ra
    nop
    .set    reorder
    END(ip54_smp_ipi_clear)

/*
 * dummy_func stubs — routines not applicable to IP54.
 * Return 0 or nothing; prevent link errors from generic kernel code.
 */
LEAF(ip54_dummy_func)
XLEAF(vme_init)
XLEAF(vme_ivec_init)
XLEAF(vme_adapter)
XLEAF(is_vme_space)
XLEAF(dma_mapinit)
XLEAF(apsfail)
XLEAF(disallowboot)
XLEAF(rmi_fixecc)
    j       ra
    END(ip54_dummy_func)

LEAF(ip54_dummyret0)
XLEAF(getcpuid)
    move    v0, zero
    j       ra
    END(ip54_dummyret0)

/*
 * ip54_get_timestamp - return a 64-bit free-running cycle counter.
 *
 * Original IP30 implementation read HEART h_count at 0x0FF00080.
 * IP54 has no HEART; use the pvtimer 64-bit counter at 0x14000038
 * (KSEG1: 0xB4000038).  The pvtimer runs at 66 MHz — same order of
 * magnitude as HEART h_count, so calibration loops that use this
 * for delta-time measurements will behave correctly.
 */
LEAF(ip54_get_timestamp)
XLEAF(_get_timestamp)
    .set    noreorder
    LI      t0, 0xB4000038   /* pvtimer counter register (66 MHz) */
    ld      v0, 0(t0)
    j       ra
    nop
    .set    reorder
    END(ip54_get_timestamp)

#endif /* IP54 */
