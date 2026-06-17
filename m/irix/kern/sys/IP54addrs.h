#if IP54
#ifndef __SYS_IP54ADDRS_H__
#define __SYS_IP54ADDRS_H__

/*
 * IP54addrs.h - Physical address map for the IP54 paravirtual workstation.
 *
 * IP54 is a purpose-built QEMU paravirtual SGI workstation.
 * It uses the HEART/BRIDGE chipset substrate (like IP30/Octane) but
 * replaces real hardware peripherals with a paravirtual device bank.
 *
 * Physical memory map:
 *   0x00000000-0x0FFEFFFF  System RAM (SEG0, first 256MB-64K)
 *   0x0FF00000-0x0FF6FFFF  HEART PIU (processor-side registers)
 *   0x10000000-0x17FFFFFF  Xbow crossbar (stub)
 *   0x1F000000-0x1FBFFFFF  BRIDGE widget 0xF (IOC3 UART only)
 *   0x1F480000-0x1F4807FF  Paravirtual device bank (PV_BASE)
 *   0x1FC00000-0x1FCFFFFF  PROM flash (ip54prom.bin)
 *   0x20000000-...         System RAM (SEG0 base for HEART/QEMU)
 *
 * Paravirtual device bank layout (256 bytes each):
 *   PV_BASE + 0x000  sgi-smp    @ 0x1F480000  SMP/IPI controller
 *   PV_BASE + 0x100  sgi-pvmem  @ 0x1F480100  Memory descriptor
 *   PV_BASE + 0x200  sgi-pvnet  @ 0x1F480200  NIC (HEART ISR bit 20)
 *   PV_BASE + 0x300  sgi-glaccel@ 0x1F480300  Framebuffer (HEART ISR bit 21)
 *   PV_BASE + 0x400  sgi-pvaudio@ 0x1F480400  Audio (HEART ISR bit 22)
 */

#ident "$Revision: 1.0 $"

/* HEART PIU base (matches IP30) */
#define IP54_HEART_BASE     0x0FF00000ULL

/* BRIDGE base (thin stub for IOC3 UART) */
#define IP54_BRIDGE_BASE    0x1F000000ULL

/* Paravirtual device bank base */
#define IP54_PV_BASE        0x1F480000ULL

/* Individual device bases */
#define IP54_PV_SMP         (IP54_PV_BASE + 0x000)  /* sgi-smp */
#define IP54_PV_MEM         (IP54_PV_BASE + 0x100)  /* sgi-pvmem */
#define IP54_PV_NET         (IP54_PV_BASE + 0x200)  /* sgi-pvnet */
#define IP54_PV_GLACCEL     (IP54_PV_BASE + 0x300)  /* sgi-glaccel */
#define IP54_PV_AUDIO       (IP54_PV_BASE + 0x400)  /* sgi-pvaudio */

/* PROM base (standard MIPS reset vector) */
#define IP54_PROM_BASE      0x1FC00000ULL
#define IP54_PROM_SIZE      0x00100000      /* 1MB */

/* RAM base (SEG0 per HEART/Octane convention) */
#define IP54_RAM_BASE       0x20000000ULL   /* QEMU maps RAM here */

/* pvmem registers (report RAM size to kernel) */
#define IP54_PVMEM_TOTAL_RAM    (IP54_PV_MEM + 0x00)
#define IP54_PVMEM_HIGH_BASE    (IP54_PV_MEM + 0x08)
#define IP54_PVMEM_HIGH_SIZE    (IP54_PV_MEM + 0x10)

/* sgi-smp registers (SMP/IPI) */
#define IP54_SMP_CPU_COUNT  (IP54_PV_SMP + 0x00)
#define IP54_SMP_CPU_ID     (IP54_PV_SMP + 0x08)
#define IP54_SMP_IPI_SET    (IP54_PV_SMP + 0x10)
#define IP54_SMP_IPI_CLEAR  (IP54_PV_SMP + 0x18)
#define IP54_SMP_IPI_STATUS (IP54_PV_SMP + 0x20)
#define IP54_SMP_BOOT_ADDR  (IP54_PV_SMP + 0x28)
#define IP54_SMP_BOOT_GO    (IP54_PV_SMP + 0x30)
#define IP54_SMP_BOOT_STATUS (IP54_PV_SMP + 0x38)

/* HEART ISR bit assignments for PV device interrupts */
#define IP54_HEART_ISR_PVNET    20  /* pvnet RX/TX done → IP4 */
#define IP54_HEART_ISR_PVFB     21  /* pvfb frame done → IP4 */
#define IP54_HEART_ISR_PVAUDIO  22  /* pvaudio buf done → IP4 */

/* CPU board type */
#define CPUBOARD    IP54

#endif /* __SYS_IP54ADDRS_H__ */
#endif /* IP54 */
