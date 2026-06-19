/*
 * pvdisk.c - IP54 Paravirtual Disk Driver
 *
 * Block device driver for the sgi-bootdisk MMIO interface.
 * The device lives at physical 0x17000000 (KSEG1 0xB7000000).
 *
 * It exposes a simple command-register interface:
 *   Write SECTOR_LO/HI, COUNT, COMMAND=READ → data appears at offset 0x200.
 *   Write data to offset 0x200+, set SECTOR_LO/HI, COUNT, COMMAND=WRITE.
 *
 * At init time this driver:
 *   1. Reads the SGI volume header (LBA 0) to discover partition layout.
 *   2. Creates hwgraph path /hw/scsi_ctlr/0/target/1/lun/0/disk/partition/N/block
 *      for every valid partition, so vfs_mountroot() can find the root device
 *      at the path set by the PROM (dksc(0,1,0) → partition/0/block).
 *
 * Copyright 1996-2024, Silicon Graphics, Inc. / QEMU IP54 project.
 */
#ident "$Revision: 1.0 $"

#if IP54

#include "sys/types.h"
#include "sys/param.h"
#include "sys/systm.h"
#include "sys/buf.h"
#include "sys/cmn_err.h"
#include "sys/debug.h"
#include "sys/errno.h"
#include "sys/sbd.h"
#include "sys/cpu.h"
#include "sys/hwgraph.h"
#include "sys/iograph.h"
#include "sys/invent.h"
#include "sys/dvh.h"
#include "sys/edt.h"
#include "sys/cred.h"
#include "sys/sema.h"
#include "string.h"

/*
 * sgi-bootdisk MMIO base: physical 0x17000000 → KSEG1 0xB7000000
 */
#define PVDISK_BASE         PHYS_TO_K1(0x1F480600ULL)
#define PVDISK_REG(off)     (*(volatile __uint32_t *)(PVDISK_BASE + (off)))
#define PVDISK_DATA_8(off)  (*(volatile unsigned char *)(PVDISK_BASE + 0x200 + (off)))
#define PVDISK_DATA_32(off) (*(volatile __uint32_t *)(PVDISK_BASE + 0x200 + (off)))

/* Register offsets (from sgi_bootdisk.h) */
#define PVDISK_SECTOR_LO    0x000
#define PVDISK_SECTOR_HI    0x004
#define PVDISK_COUNT        0x008
#define PVDISK_COMMAND      0x00C
#define PVDISK_STATUS       0x010
#define PVDISK_SIZE_LO      0x014
#define PVDISK_SIZE_HI      0x018

/* Commands */
#define PVDISK_CMD_READ     1
#define PVDISK_CMD_WRITE    2

/* Status bits */
#define PVDISK_STATUS_READY (1U << 31)
#define PVDISK_STATUS_ERROR (1U << 0)

#define PVDISK_SECTOR_SIZE  512
#define PVDISK_NPART        NPARTAB     /* 16, from sys/dvh.h */

/* Per-partition state (populated from the SGI volume header at init) */
static int pvdisk_pt_firstlbn[PVDISK_NPART];
static int pvdisk_pt_nblks[PVDISK_NPART];

static int pvdisk_initialized;

/*
 * pvdisk_lock serializes all MMIO accesses to the sgi-bootdisk device.
 * The device has a single shared SECTOR_LO/HI/COMMAND register set and
 * a single 512-byte data window.  Concurrent strategy calls (e.g. from
 * XFS and exec() running on different kernel threads) would overwrite
 * each other's SECTOR_LO/COMMAND writes, corrupting the data window.
 *
 * We use a spinlock at splhi (interrupts disabled) so that even an
 * interrupt-driven strategy call cannot race with a process-level one.
 */
static lock_t pvdisk_lock;

int pvdiskdevflag = 0;

/* -----------------------------------------------------------------------
 * Low-level single-sector I/O
 * ---------------------------------------------------------------------- */

/*
 * pvdisk_do_read - read one 512-byte sector from absolute LBA into buf.
 * Returns 0 on success, EIO on error.
 *
 * MUST be called with pvdisk_lock held (via mutex_spinlock).
 */
/*
 * Aligned staging buffer for 32-bit data-window transfers (callers may
 * pass unaligned buffers).  Protected by pvdisk_lock like the window.
 * 32-bit big-endian MMIO matches MIPS BE memory layout byte-for-byte;
 * 128 exits per sector instead of 512.
 */
static __uint32_t pvdisk_sec32[PVDISK_SECTOR_SIZE / 4];

static int
pvdisk_do_read(__uint64_t lba, caddr_t buf)
{
    int i, tries = 100000;

    PVDISK_REG(PVDISK_SECTOR_LO) = (unsigned int)(lba & 0xFFFFFFFF);
    PVDISK_REG(PVDISK_SECTOR_HI) = (unsigned int)(lba >> 32);
    PVDISK_REG(PVDISK_COUNT)     = PVDISK_SECTOR_SIZE;
    PVDISK_REG(PVDISK_COMMAND)   = PVDISK_CMD_READ;

    while (tries-- > 0) {
        unsigned int st = PVDISK_REG(PVDISK_STATUS);
        if (st & PVDISK_STATUS_ERROR)
            return EIO;
        if (st & PVDISK_STATUS_READY)
            break;
    }
    if (tries <= 0)
        return EIO;

    for (i = 0; i < PVDISK_SECTOR_SIZE / 4; i++)
        pvdisk_sec32[i] = PVDISK_DATA_32(i * 4);
    bcopy((caddr_t)pvdisk_sec32, buf, PVDISK_SECTOR_SIZE);

    return 0;
}

/*
 * pvdisk_do_write - write one 512-byte sector from buf to absolute LBA.
 * Returns 0 on success, EIO on error.
 *
 * MUST be called with pvdisk_lock held (via mutex_spinlock).
 */
static int
pvdisk_do_write(__uint64_t lba, caddr_t buf)
{
    int i, tries = 100000;

    /* Fill data window with sector data (32-bit, via aligned staging) */
    bcopy(buf, (caddr_t)pvdisk_sec32, PVDISK_SECTOR_SIZE);
    for (i = 0; i < PVDISK_SECTOR_SIZE / 4; i++)
        PVDISK_DATA_32(i * 4) = pvdisk_sec32[i];

    PVDISK_REG(PVDISK_SECTOR_LO) = (unsigned int)(lba & 0xFFFFFFFF);
    PVDISK_REG(PVDISK_SECTOR_HI) = (unsigned int)(lba >> 32);
    PVDISK_REG(PVDISK_COUNT)     = PVDISK_SECTOR_SIZE;
    PVDISK_REG(PVDISK_COMMAND)   = PVDISK_CMD_WRITE;

    while (tries-- > 0) {
        unsigned int st = PVDISK_REG(PVDISK_STATUS);
        if (st & PVDISK_STATUS_ERROR)
            return EIO;
        if (st & PVDISK_STATUS_READY)
            break;
    }
    if (tries <= 0)
        return EIO;

    return 0;
}

/* -----------------------------------------------------------------------
 * IRIX block device interface
 * ---------------------------------------------------------------------- */

/* ARGSUSED */
int
pvdiskopen(dev_t *devp, int oflag, int otyp, cred_t *crp)
{
    if (!pvdisk_initialized)
        return ENXIO;
    return 0;
}

/* ARGSUSED */
int
pvdiskclose(dev_t dev, int oflag, int otyp, cred_t *crp)
{
    return 0;
}

/*
 * pvdiskstrategy - block I/O strategy function.
 *
 * b_blkno is relative to the start of the partition (in 512-byte sectors).
 * We look up the partition's pt_firstlbn offset stored as hwgraph fastinfo
 * on the block device vertex.
 */
void
pvdiskstrategy(struct buf *bp)
{
    vertex_hdl_t  vhdl      = dev_to_vhdl(bp->b_edev);
    int           pt_off    = (int)(long)hwgraph_fastinfo_get(vhdl);
    __uint64_t    abs_lba   = (__uint64_t)pt_off + (__uint64_t)(int)bp->b_blkno;
    unsigned int  nbytes    = bp->b_bcount;
    caddr_t       buf;
    int           need_mapout = 0;
    unsigned int  done      = 0;
    int           err       = 0;
    int           spl_save;
    char          sec[PVDISK_SECTOR_SIZE];

    /*
     * B_PAGEIO buffers describe I/O via b_pages (physical page frames) and
     * have an invalid b_un.b_addr.  Map them into kernel virtual space first.
     */
    if (!BP_ISMAPPED(bp)) {
        buf = bp_mapin(bp);
        need_mapout = 1;
    } else {
        buf = bp->b_dmaaddr;
    }

    /*
     * Acquire the pvdisk spinlock at splhi to serialize all MMIO accesses.
     * The sgi-bootdisk hardware exposes a single SECTOR_LO/HI/COMMAND set
     * and a single 512-byte data window.  Without this lock, concurrent
     * strategy calls (e.g. XFS inode read + exec() binary load) interleave
     * their MMIO accesses and corrupt each other's data.
     */
    spl_save = mutex_spinlock(&pvdisk_lock);

    if (bp->b_flags & B_READ) {
        /* Read: sector by sector into caller's buffer */
        while (done < nbytes) {
            unsigned int chunk = nbytes - done;
            if (chunk > PVDISK_SECTOR_SIZE)
                chunk = PVDISK_SECTOR_SIZE;

            err = pvdisk_do_read(abs_lba, sec);
            if (err) {
                bp->b_error  = err;
                bp->b_flags |= B_ERROR;
                break;
            }
            bcopy(sec, buf + done, chunk);
            done += chunk;
            abs_lba++;
        }
    } else {
        /* Write: sector by sector from caller's buffer */
        while (done < nbytes) {
            unsigned int chunk = nbytes - done;
            if (chunk > PVDISK_SECTOR_SIZE)
                chunk = PVDISK_SECTOR_SIZE;

            /* Partial sector: read-modify-write */
            if (chunk < PVDISK_SECTOR_SIZE) {
                err = pvdisk_do_read(abs_lba, sec);
                if (err) {
                    bp->b_error  = err;
                    bp->b_flags |= B_ERROR;
                    break;
                }
            }
            bcopy(buf + done, sec, chunk);
            err = pvdisk_do_write(abs_lba, sec);
            if (err) {
                bp->b_error  = err;
                bp->b_flags |= B_ERROR;
                break;
            }
            done += chunk;
            abs_lba++;
        }
    }

    mutex_spinunlock(&pvdisk_lock, spl_save);

    /*
     * CACHE COHERENCY (critical for B_PAGEIO / mmap'd file pages and exec'd
     * binaries):  we filled `buf` via CPU bcopy, so the data sits dirty in the
     * dcache at the *kernel* mapping's virtually-indexed cache lines while
     * physical memory still holds the page's previous (zero-filled) contents.
     * A user mapping of the same page (rld mmap'ing a shared library) or an
     * instruction fetch reads through a different mapping and would see the
     * stale zeros -> "first 4 bytes are 0x00, not ELF".  Real disk drivers
     * (DMA) dki_dcache_inval after the device fills memory; this PIO driver
     * must write the bcopy'd data back to physmemory AND invalidate the stale
     * lines.  Without this, large reads intermittently come back as zeros
     * (the sgi-ip54 "read fragility").  Writes need no flush (device-bound).
     *
     * VCE / cache-aliasing extension (2026-06-19): plain dki_dcache_wbinval
     * only flushes lines indexed by `buf`'s color (the kernel-VA index).  On
     * R4k/R5k's VIPT primary dcache a user mmap of the same physical page
     * at a different VA color has its OWN cache lines that may still hold
     * stale zeros.  The xdm second-wave crashes pinned to libpthread.so
     * GOT entries reading as zero at runtime (xdm /core EPC =
     * libpthread+0xfaac in _SGIPT_stk_fork_child, t9=0 from a GOT[90] load,
     * while the on-disk GOT[90] = 0xc22faa0 is correct) match exactly this
     * cache-alias pattern.  Use data_cache_wbinval with CACH_OTHER_COLORS
     * to flush every color alias of the filled region in addition to the
     * current-color flush.  See progress_notes/ip54_libpthread_got_zero_2026-06-19.md.
     */
    if ((bp->b_flags & B_READ) && done > 0) {
        dki_dcache_wbinval(buf, (int)done);
#ifdef CACH_OTHER_COLORS
        data_cache_wbinval(buf, (int)done,
                           CACH_OTHER_COLORS | CACH_IO_COHERENCY);
#endif
    }

    bp->b_resid = nbytes - done;

    if (need_mapout)
        bp_mapout(bp);

    biodone(bp);
}

/*
 * pvdisksize - return device size in 512-byte blocks.
 *
 * Called by the kernel to validate partition sizes.  We return the
 * partition size from the cached volume header data.
 */
int
pvdisksize(dev_t dev)
{
    vertex_hdl_t  vhdl   = dev_to_vhdl(dev);
    int           pt_off = (int)(long)hwgraph_fastinfo_get(vhdl);
    int           i;
    __uint64_t    total;

    /* Find partition by its pt_firstlbn offset and return pt_nblks */
    for (i = 0; i < PVDISK_NPART; i++) {
        if (pvdisk_pt_firstlbn[i] == pt_off && pvdisk_pt_nblks[i] > 0)
            return pvdisk_pt_nblks[i];
    }

    /* Fallback: return total disk sectors (capped to INT_MAX) */
    total = ((__uint64_t)PVDISK_REG(PVDISK_SIZE_HI) << 32) |
              PVDISK_REG(PVDISK_SIZE_LO);
    if (total > 0x7FFFFFFF)
        total = 0x7FFFFFFF;
    return (int)total;
}

/*
 * pvdisk_register_partitions - build the hwgraph subtree for all valid
 * partitions found in the SGI volume header.
 *
 * Creates /hw/scsi_ctlr/0/target/1/lun/0/disk/partition/<N>/block for
 * each partition with pt_nblks > 0.  Stores pt_firstlbn as the vertex
 * fastinfo so pvdiskstrategy() can translate b_blkno to absolute LBA.
 */
static void
pvdisk_register_partitions(void)
{
    int           part;
    graph_error_t rc;
    vertex_hdl_t  part_vhdl, block_vhdl;
    char          path[80];

    for (part = 0; part < PVDISK_NPART; part++) {
        if (pvdisk_pt_nblks[part] <= 0)
            continue;

        /* Build the path component relative to hwgraph_root (/hw) */
        sprintf(path, "scsi_ctlr/0/target/1/lun/0/disk/partition/%d", part);

        rc = hwgraph_path_add(hwgraph_root, path, &part_vhdl);
        if (rc != GRAPH_SUCCESS) {
            cmn_err(CE_WARN,
                    "pvdisk: hwgraph_path_add(%s) failed (rc=%d)", path, rc);
            continue;
        }

        rc = hwgraph_block_device_add(part_vhdl, "block", "pvdisk",
                                      &block_vhdl);
        if (rc != GRAPH_SUCCESS) {
            cmn_err(CE_WARN,
                    "pvdisk: hwgraph_block_device_add(part %d) failed (rc=%d)",
                    part, rc);
            hwgraph_vertex_unref(part_vhdl);
            continue;
        }

        /*
         * Store the absolute start LBA of this partition on the block
         * device vertex.  pvdiskstrategy() reads this to translate
         * partition-relative b_blkno to an absolute disk sector.
         */
        hwgraph_fastinfo_set(block_vhdl,
                             (arbitrary_info_t)(long)pvdisk_pt_firstlbn[part]);

        hwgraph_inventory_add(block_vhdl, INV_DISK, INV_SCSI, 0, 1, 0);

        hwgraph_vertex_unref(block_vhdl);
        hwgraph_vertex_unref(part_vhdl);
    }
}

/* -----------------------------------------------------------------------
 * Driver initialisation — called once at boot via edtinit (s=soft flag)
 * ---------------------------------------------------------------------- */

/* ARGSUSED */
void
pvdiskedtinit(struct edt *edtp)
{
    char                  vhbuf[PVDISK_SECTOR_SIZE];
    struct volume_header *vh = (struct volume_header *)vhbuf;
    __uint64_t            total_sectors;
    int                   i;

    /* Initialize the MMIO serialization spinlock */
    spinlock_init(&pvdisk_lock, "pvdisk");

    /* Verify the device is accessible */
    if (badaddr((void *)PVDISK_BASE, 4)) {
        /* Not present — must be a different machine */
        return;
    }

    /* Check that a disk is attached (SIZE_LO == 0 means no disk) */
    if (PVDISK_REG(PVDISK_SIZE_LO) == 0 &&
        PVDISK_REG(PVDISK_SIZE_HI) == 0) {
        cmn_err(CE_NOTE, "pvdisk: no disk attached");
        return;
    }

    /* Read the SGI volume header (always at LBA 0) */
    if (pvdisk_do_read(0, vhbuf) != 0) {
        cmn_err(CE_WARN, "pvdisk: cannot read volume header");
        return;
    }

    /* Validate magic */
    if (vh->vh_magic != VHMAGIC) {
        cmn_err(CE_WARN,
                "pvdisk: bad volume header magic 0x%x (expected 0x%x); "
                "disk may not have an SGI volume header",
                vh->vh_magic, VHMAGIC);
        return;
    }

    /* Cache partition table */
    for (i = 0; i < PVDISK_NPART; i++) {
        pvdisk_pt_firstlbn[i] = vh->vh_pt[i].pt_firstlbn;
        pvdisk_pt_nblks[i]    = vh->vh_pt[i].pt_nblks;
    }

    pvdisk_initialized = 1;

    /* Register all valid partitions in hwgraph */
    pvdisk_register_partitions();

    total_sectors = ((__uint64_t)PVDISK_REG(PVDISK_SIZE_HI) << 32) |
                     PVDISK_REG(PVDISK_SIZE_LO);

    cmn_err(CE_NOTE,
            "pvdisk: IP54 paravirtual disk, %llu sectors (%llu MB), "
            "root partition 0 at LBA %d",
            (unsigned long long)total_sectors,
            (unsigned long long)(total_sectors / 2048),
            pvdisk_pt_firstlbn[0]);
}

#endif /* IP54 */
