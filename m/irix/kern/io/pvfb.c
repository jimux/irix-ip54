/*
 * IP54 Paravirtual Framebuffer / Graphics Board Driver (pvfb)
 *
 * Dual-purpose driver:
 *   1. Char device /dev/pvfb for direct framebuffer access via glaccel
 *   2. GfxRegisterBoard("NEWPORT") so Xsgi's Newport DDX can drive pvrex3
 *
 * The pvrex3 device at PA 0x1F490000 provides a Newport-compatible REX3
 * register interface (8KB). This driver maps those registers into Xsgi's
 * address space via gf_MapGfx, making the Xsgi Newport DDX work directly.
 *
 * Copyright 1996-2024, Silicon Graphics, Inc. / QEMU IP54 project.
 */
#ident "$Revision: 2.0 $"

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
#include "sys/mman.h"
#include "sys/sbd.h"
#include "sys/cpu.h"
#include "sys/conf.h"
#include "sys/cred.h"
#include "sys/edt.h"
#include "sys/hwgraph.h"
#include "sys/invent.h"
#include "sys/gfx.h"
#include "ksys/ddmap.h"

/*
 * sgi-glaccel register base (KSEG1 uncached) — for /dev/pvfb char device.
 */
#define GLACCEL_BASE        PHYS_TO_K1(0x1F480300ULL)
#define GLACCEL_REG(off)    (*(volatile __uint32_t *)(GLACCEL_BASE + (off)))

#define GLACCEL_STATUS      0x00
#define GLACCEL_WIDTH       0x04
#define GLACCEL_HEIGHT      0x08
#define GLACCEL_CMD_BASE    0x0C
#define GLACCEL_CMD_LEN     0x10
#define GLACCEL_FB_BASE     0x14
#define GLACCEL_EXEC        0x18
#define GLACCEL_FORMAT      0x1C
#define GLACCEL_STRIDE      0x20

#define GLACCEL_EXEC_RESET      (1 << 0)
#define GLACCEL_EXEC_PROCESS    (1 << 1)
#define GLACCEL_FMT_RGBA8888    0
#define GLACCEL_FMT_RGB565      1
#define GLACCEL_STATUS_DONE     (1 << 0)

/*
 * PVRex3 device — Newport-compatible REX3 register interface.
 * Physical address 0x1F490000, size 8KB (0x2000).
 * Xsgi maps this into user space via gf_MapGfx.
 */
#define PVREX3_PHYS_BASE    0x1F490000ULL
#define PVREX3_REG_SIZE     0x2000          /* 8KB REX3 register space */
/* IRIX/MIPS page size is 16KB (0x4000). 8KB REX3 = 1 page. */
#define PVREX3_PAGE_SIZE    0x4000
#define PVREX3_MAP_PAGES    ((PVREX3_REG_SIZE + PVREX3_PAGE_SIZE - 1) / PVREX3_PAGE_SIZE)
#define PVREX3_K1_BASE      PHYS_TO_K1(PVREX3_PHYS_BASE)

/* REX3 register access (KSEG1 uncached) for the HW cursor (gf_PositionCursor) */
#define PVREX3_REG(off)     (*(volatile __uint32_t *)(PVREX3_K1_BASE + (off)))
#define REX3_DCBMODE        0x0238      /* DCB mode (slave/regsel/datawidth) */
#define REX3_DCBDATA0       0x0240      /* DCB data MSW — write triggers transfer */

/*
 * GL window clip channel (VirGL roadmap Step 4) — a private pvrex3 register block
 * (REX3-unused high offsets 0x1C00+) that forwards this window's screen geometry +
 * occlusion-aware visible clip pieces from gf_ValidateClip to the QEMU compositor,
 * so the host-rendered GL overlay tracks/clips to the real desktop window.
 */
#define REX3_CLIP_WID        0x1c00
#define REX3_CLIP_XORG       0x1c04
#define REX3_CLIP_YORG       0x1c08
#define REX3_CLIP_XSIZE      0x1c0c
#define REX3_CLIP_YSIZE      0x1c10
#define REX3_CLIP_OBSCURED   0x1c14
#define REX3_CLIP_NUMPIECES  0x1c18
#define REX3_CLIP_PIECE_X    0x1c20
#define REX3_CLIP_PIECE_Y    0x1c24
#define REX3_CLIP_PIECE_W    0x1c28
#define REX3_CLIP_PIECE_H    0x1c2c
#define REX3_CLIP_PIECE_PUSH 0x1c30
#define REX3_CLIP_COMMIT     0x1c3c
#define PVFB_CLIP_MAX_PIECES 32

/* ioctl numbers for /dev/pvfb char device */
#define PVFB_SET_MODE   0x5000
#define PVFB_FLIP       0x5001

#define PVFB_BPP_RGBA8888   4
#define PVFB_BPP_RGB565     2
#define PVFB_MAX_WIDTH      640
#define PVFB_MAX_HEIGHT     480
#define PVFB_MAX_BPP        4
#define PVFB_MAX_FBSIZE     (PVFB_MAX_WIDTH * PVFB_MAX_HEIGHT * PVFB_MAX_BPP)

struct pvfb_mode {
    __uint32_t width;
    __uint32_t height;
    __uint32_t format;
};

/* Per-open state for /dev/pvfb char device */
struct pvfb_state {
    int           ps_open;
    __uint32_t    ps_width;
    __uint32_t    ps_height;
    __uint32_t    ps_format;
    __uint32_t    ps_bpp;
    size_t        ps_fbsize;
    void         *ps_fbk1;
    __uint64_t    ps_fbphys;
    uint          ps_fbpages;
};

static struct pvfb_state pvfb_state;
static char pvfb_static_fb[PVFB_MAX_FBSIZE];

int pvfbdevflag = 0;

/* =====================================================================
 * GfxRegisterBoard support — makes pvrex3 visible to Xsgi as "NEWPORT"
 * ===================================================================== */

/*
 * Board-private data. The gfx_data struct is the first member so that
 * GfxRegisterBoard / RRM can use it directly.
 */
struct pvfb_bdata {
    struct gfx_data     bd_gfxdata;
    struct gfx_info     bd_gfxinfo;
    caddr_t             bd_rex3base;    /* KSEG1 base of REX3 regs */
};

static struct pvfb_bdata pvfb_bdata;
static int pvfb_board_registered;

/* Forward declarations for gfx_fncs vtable */
static int pvfb_gf_Info(struct gfx_data *, void *, unsigned int, int *);
static int pvfb_gf_Attach(struct gfx_gfx *, caddr_t);
static int pvfb_gf_Detach(struct gfx_gfx *);
static int pvfb_gf_Initialize(struct gfx_gfx *);
static int pvfb_gf_Download(struct gfx_gfx *, struct gfx_download_args *);
static int pvfb_gf_Start(struct gfx_gfx *);
static int pvfb_gf_PositionCursor(struct gfx_data *, int, int);
static int pvfb_gf_Exit(struct gfx_gfx *);
static int pvfb_gf_CreateDDRN(struct gfx_data *, struct rrm_rnode *);
static int pvfb_gf_DestroyDDRN(struct gfx_data *, struct gfx_gfx *, struct rrm_rnode *);
static int pvfb_gf_ValidateClip(struct gfx_gfx *, struct rrm_rnode *,
    struct rrm_rnode *, struct RRM_ValidateClip *);
static int pvfb_gf_SetNullClip(struct RRM_ValidateClip *);
static int pvfb_gf_MapGfx(struct gfx_gfx *, __psunsigned_t, int);
static int pvfb_gf_UnMapGfx(struct gfx_gfx *);
static int pvfb_gf_InvalTLB(struct gfx_gfx *);
static int pvfb_gf_PcxSwap(struct gfx_data *, struct rrm_rnode *,
    struct rrm_rnode *, struct rrm_rnode *);
static int pvfb_gf_PcxSwitch(struct gfx_data *, struct rrm_rnode *,
    struct rrm_rnode *);
static int pvfb_gf_SchedSwapBuf(struct gfx_data *, struct rrm_rnode *, int, int);
static int pvfb_gf_UnSchedSwapBuf(struct gfx_gfx *, struct rrm_rnode *, int);
static int pvfb_gf_SchedRetraceEvent(struct gfx_data *, struct rrm_rnode *);
static int pvfb_gf_SetDisplayMode(struct gfx_gfx *, int, unsigned int);
static int pvfb_gf_SchedMGRSwapBuf(struct gfx_gfx *, int, int, int, int);
static int pvfb_gf_Suspend(struct gfx_data *, struct gfx_gfx *, int);
static int pvfb_gf_Resume(struct gfx_data *, struct gfx_gfx *, int);
static int pvfb_gf_ReleaseGfxSema(struct gfx_gfx *);
static int pvfb_gf_Private(struct gfx_gfx *, struct rrm_rnode *,
    unsigned int, void *, int *);
static int pvfb_gf_FrsInstall(struct gfx_data *, void *);
static int pvfb_gf_FrsUninstall(struct gfx_data *);

static struct gfx_fncs pvfb_gfx_fncs = {
    pvfb_gf_Info,
    pvfb_gf_Attach,
    pvfb_gf_Detach,
    pvfb_gf_Initialize,
    pvfb_gf_Download,
    pvfb_gf_Start,
    pvfb_gf_PositionCursor,
    pvfb_gf_Exit,
    pvfb_gf_CreateDDRN,
    pvfb_gf_DestroyDDRN,
    pvfb_gf_ValidateClip,
    pvfb_gf_SetNullClip,
    pvfb_gf_MapGfx,
    pvfb_gf_UnMapGfx,
    pvfb_gf_InvalTLB,
    pvfb_gf_PcxSwap,
    pvfb_gf_PcxSwitch,
    pvfb_gf_SchedSwapBuf,
    pvfb_gf_UnSchedSwapBuf,
    pvfb_gf_SchedRetraceEvent,
    pvfb_gf_SetDisplayMode,
    pvfb_gf_SchedMGRSwapBuf,
    pvfb_gf_Suspend,
    pvfb_gf_Resume,
    pvfb_gf_ReleaseGfxSema,
    pvfb_gf_Private,
    pvfb_gf_FrsInstall,
    pvfb_gf_FrsUninstall,
};

/*
 * gf_Info — return board info to userland.
 * Copies gfx_info struct into user buffer.
 */
/* ARGSUSED */
static int
pvfb_gf_Info(struct gfx_data *bdata, void *buf, unsigned int len, int *rlen)
{
    struct pvfb_bdata *bd = (struct pvfb_bdata *)bdata;
    unsigned int copylen;

    copylen = len;
    if (copylen > sizeof(struct gfx_info))
        copylen = sizeof(struct gfx_info);

    if (copyout(&bd->bd_gfxinfo, buf, copylen))
        return EFAULT;

    if (rlen)
        *rlen = copylen;

    return 0;
}

/*
 * gf_Attach — called when a process attaches to the board.
 *
 * Must call gfxdd_mmap() to create the user-space mapping region
 * and store the ddv_handle_t in gfxp->gx_ddv. This handle is then
 * used by gf_MapGfx to populate the page tables.
 */
static int
pvfb_gf_Attach(struct gfx_gfx *gfxp, caddr_t vaddr)
{
    int err;

    cmn_err(CE_NOTE, "pvfb: gf_Attach called vaddr=0x%x gfxp=0x%x",
            (unsigned int)(__psunsigned_t)vaddr,
            (unsigned int)(__psunsigned_t)gfxp);

    err = gfxdd_mmap(0,                         /* flag: 0 = normal */
                     PVREX3_REG_SIZE,            /* size: 8KB REX3 regs */
                     vaddr,                      /* uvaddr: user's requested addr */
                     gfx_fault,                  /* fault handler */
                     (void *)gfxp,               /* opaque arg for fault */
                     NULL,                       /* no cache attr ranges */
                     0,                          /* vmap_size */
                     &gfxp->gx_ddv);             /* OUTPUT: ddv handle */
    if (err) {
        cmn_err(CE_WARN, "pvfb: gfxdd_mmap failed (%d)", err);
        return err;
    }

    cmn_err(CE_NOTE, "pvfb: gf_Attach OK ddv=0x%x",
            (unsigned int)(__psunsigned_t)gfxp->gx_ddv);
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_Detach(struct gfx_gfx *gfxp)
{
    if (gfxp->gx_ddv) {
        gfxdd_munmap(gfxp->gx_ddv);
        gfxp->gx_ddv = NULL;
    }
    gfxp->gx_flags &= ~GFX_MAPPED;
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_Initialize(struct gfx_gfx *gfxp)
{
    cmn_err(CE_NOTE, "pvfb: gf_Initialize called");
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_Download(struct gfx_gfx *gfxp, struct gfx_download_args *args)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_Start(struct gfx_gfx *gfxp)
{
    return 0;
}

/*
 * gf_PositionCursor — move the pvrex3 VC2 hardware cursor sprite.
 *
 * The kernel shmiq/gfx cursor-tracking calls this to move the HW cursor on
 * pointer motion (and for QIOCSETCPOS).  On IP54 the cursor is the pvrex3 VC2
 * sprite; we move it by writing VC2 CURSOR_X (reg 2) / CURSOR_Y (reg 3) through
 * the REX3 DCB.  Sequence (verified against the pvrex3 newport_dcb_write path):
 *   DCBMODE  = 0x3                      slave=VC2(0), regsel=0, datawidth=3 (combined)
 *   DCBDATA0 = (reg<<24)|(val<<8)       -> vc2_reg[reg] = val
 * Each 32-bit DCBDATA0 store triggers an immediate DCB transfer to VC2.
 */
static int
pvfb_gf_PositionCursor(struct gfx_data *bdata, int x, int y)
{
    static int poscur_logged = 0;

    if (poscur_logged < 6) {
        cmn_err(CE_NOTE, "pvfb: PositionCursor(%d, %d)", x, y);
        poscur_logged++;
    }

    PVREX3_REG(REX3_DCBMODE)  = 0x00000003;
    PVREX3_REG(REX3_DCBDATA0) = (2 << 24) | ((x & 0xffff) << 8);   /* VC2 CURSOR_X */
    PVREX3_REG(REX3_DCBDATA0) = (3 << 24) | ((y & 0xffff) << 8);   /* VC2 CURSOR_Y */
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_Exit(struct gfx_gfx *gfxp)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_CreateDDRN(struct gfx_data *bdata, struct rrm_rnode *rnp)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_DestroyDDRN(struct gfx_data *bdata, struct gfx_gfx *gfxp,
    struct rrm_rnode *rnp)
{
    return 0;
}

/*
 * gf_ValidateClip — forward the GL window's screen geometry + visible clip pieces
 * to the QEMU compositor via the pvrex3 clip channel (VirGL roadmap Step 4).
 *
 * RRM calls this whenever a window's clip state changes (open/move/resize/restack/
 * occlude). vclip carries the window origin/size, the WID, an obscured flag, and a
 * piecelist of the currently-visible sub-rectangles (occlusion). We stage those into
 * the pvrex3 clip registers and latch with CLIP_COMMIT, so the host-rendered GL overlay
 * for this WID is positioned and clipped to exactly the visible region of its window.
 *
 * piecelist may be NULL when numpieces<=1 (RRM convention: the whole window rect is
 * visible) — we send numpieces=0 and the compositor uses the window rect.
 */
static int
pvfb_gf_ValidateClip(struct gfx_gfx *gfxp, struct rrm_rnode *hwrnp,
    struct rrm_rnode *swrnp, struct RRM_ValidateClip *vclip)
{
    static int vclip_logged = 0;
    int n, i;

    if (vclip == NULL) {
        return 0;
    }

    n = vclip->numpieces;
    if (n < 0) {
        n = 0;
    }
    if (n > PVFB_CLIP_MAX_PIECES) {
        n = PVFB_CLIP_MAX_PIECES;
    }

    if (vclip_logged < 8) {
        cmn_err(CE_NOTE,
            "pvfb: ValidateClip wid=%d org=(%d,%d) size=(%d,%d) pieces=%d obsc=%d",
            vclip->wid, vclip->xorg, vclip->yorg, vclip->xsize, vclip->ysize,
            vclip->numpieces, vclip->obscured);
        vclip_logged++;
    }

    PVREX3_REG(REX3_CLIP_WID)       = (__uint32_t)vclip->wid;
    PVREX3_REG(REX3_CLIP_XORG)      = (__uint32_t)vclip->xorg;
    PVREX3_REG(REX3_CLIP_YORG)      = (__uint32_t)vclip->yorg;
    PVREX3_REG(REX3_CLIP_XSIZE)     = (__uint32_t)vclip->xsize;
    PVREX3_REG(REX3_CLIP_YSIZE)     = (__uint32_t)vclip->ysize;
    PVREX3_REG(REX3_CLIP_OBSCURED)  = (__uint32_t)vclip->obscured;
    PVREX3_REG(REX3_CLIP_NUMPIECES) = (__uint32_t)n;

    if (vclip->piecelist != NULL) {
        for (i = 0; i < n; i++) {
            struct RRM_PieceList *p = &vclip->piecelist[i];
            PVREX3_REG(REX3_CLIP_PIECE_X)    = (__uint32_t)p->x;
            PVREX3_REG(REX3_CLIP_PIECE_Y)    = (__uint32_t)p->y;
            PVREX3_REG(REX3_CLIP_PIECE_W)    = (__uint32_t)p->xsize;
            PVREX3_REG(REX3_CLIP_PIECE_H)    = (__uint32_t)p->ysize;
            PVREX3_REG(REX3_CLIP_PIECE_PUSH) = (__uint32_t)i;
        }
    }

    PVREX3_REG(REX3_CLIP_COMMIT) = 1;
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_SetNullClip(struct RRM_ValidateClip *vclip)
{
    return 0;
}

/*
 * gf_MapGfx — map pvrex3 REX3 registers into user address space.
 *
 * Called after gf_Attach has set up gfxp->gx_ddv via gfxdd_mmap().
 * Uses ddv_mappages() to populate the page tables with the physical
 * pages backing the REX3 register space (PA 0x1F490000, 8KB).
 */
/* ARGSUSED */
static int
pvfb_gf_MapGfx(struct gfx_gfx *gfxp, __psunsigned_t vaddr, int flags)
{
    caddr_t rex3_k1 = (caddr_t)PVREX3_K1_BASE;

    cmn_err(CE_NOTE, "pvfb: gf_MapGfx called vaddr=0x%x flags=0x%x ddv=0x%x",
            (unsigned int)vaddr, flags,
            (unsigned int)(__psunsigned_t)gfxp->gx_ddv);

    if (!gfxp->gx_ddv) {
        cmn_err(CE_WARN, "pvfb: gf_MapGfx called with NULL gx_ddv");
        return ENOMEM;
    }

    /*
     * Use ddv_mappages without explicit locking — the gfx subsystem
     * already holds appropriate locks when calling gf_MapGfx.
     * ddv_lock would deadlock.
     */
    ddv_mappages(gfxp->gx_ddv, 0, rex3_k1, PVREX3_MAP_PAGES);

    gfxp->gx_flags |= GFX_MAPPED;

    cmn_err(CE_NOTE, "pvfb: mapped pvrex3 REX3 regs at PA 0x%x into user space",
            (unsigned int)PVREX3_PHYS_BASE);

    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_UnMapGfx(struct gfx_gfx *gfxp)
{
    gfxp->gx_flags &= ~GFX_MAPPED;
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_InvalTLB(struct gfx_gfx *gfxp)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_PcxSwap(struct gfx_data *bdata, struct rrm_rnode *cur,
    struct rrm_rnode *new_rn, struct rrm_rnode *old)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_PcxSwitch(struct gfx_data *bdata, struct rrm_rnode *cur,
    struct rrm_rnode *new_rn)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_SchedSwapBuf(struct gfx_data *bdata, struct rrm_rnode *rnp,
    int a, int b)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_UnSchedSwapBuf(struct gfx_gfx *gfxp, struct rrm_rnode *rnp, int a)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_SchedRetraceEvent(struct gfx_data *bdata, struct rrm_rnode *rnp)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_SetDisplayMode(struct gfx_gfx *gfxp, int mode, unsigned int flags)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_SchedMGRSwapBuf(struct gfx_gfx *gfxp, int a, int b, int c, int d)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_Suspend(struct gfx_data *bdata, struct gfx_gfx *gfxp, int flags)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_Resume(struct gfx_data *bdata, struct gfx_gfx *gfxp, int flags)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_ReleaseGfxSema(struct gfx_gfx *gfxp)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_Private(struct gfx_gfx *gfxp, struct rrm_rnode *rnp,
    unsigned int cmd, void *arg, int *rval)
{
    /*
     * Evidence logging: Xsgi's rex3DrawImage sends NG1_PIXELDMA here for
     * large images and does NOT fall back to PIO on failure (regions
     * stay unpainted).  Log the cmd and the head of the arg struct so
     * the ng1_pixeldma_args layout can be recovered from a live run
     * (sys/ng1.h was never shipped).
     */
    {
        unsigned int w[10];
        int i;
        for (i = 0; i < 10; i++)
            w[i] = 0xdeadbeef;
        if (arg) {
            /* arg is a user pointer for ioctls; try copyin, fall back
             * to direct read if it is already a kernel address. */
            if (copyin(arg, (caddr_t)w, sizeof(w)) != 0) {
                for (i = 0; i < 10; i++)
                    w[i] = ((unsigned int *)arg)[i];
            }
        }
        cmn_err(CE_NOTE,
            "pvfb: gf_Private cmd=0x%x arg=0x%x w0=%x w1=%x w2=%x w3=%x w4=%x",
            cmd, (unsigned int)(__psunsigned_t)arg, w[0], w[1], w[2], w[3], w[4]);
        cmn_err(CE_NOTE,
            "pvfb: gf_Private w5=%x w6=%x w7=%x w8=%x w9=%x",
            w[5], w[6], w[7], w[8], w[9]);
    }
    return EINVAL;
}

/* ARGSUSED */
static int
pvfb_gf_FrsInstall(struct gfx_data *bdata, void *intrgroup)
{
    return 0;
}

/* ARGSUSED */
static int
pvfb_gf_FrsUninstall(struct gfx_data *bdata)
{
    return 0;
}

/* =====================================================================
 * /dev/pvfb character device interface (unchanged from v1)
 * ===================================================================== */

/* ARGSUSED */
int
pvfbopen(dev_t dev, int oflag, int otyp, cred_t *crp)
{
    struct pvfb_state *ps = &pvfb_state;
    void *k0buf;

    if (ps->ps_open)
        return EBUSY;

    GLACCEL_REG(GLACCEL_EXEC) = GLACCEL_EXEC_RESET;

    k0buf = pvfb_static_fb;
    bzero(k0buf, PVFB_MAX_FBSIZE);

    ps->ps_fbphys  = (__uint64_t)kvtophys((caddr_t)k0buf);
    ps->ps_fbk1    = (void *)PHYS_TO_K1(ps->ps_fbphys);
    ps->ps_fbpages = 0;
    ps->ps_fbsize  = PVFB_MAX_FBSIZE;
    ps->ps_width   = 640;
    ps->ps_height  = 480;
    ps->ps_format  = GLACCEL_FMT_RGBA8888;
    ps->ps_bpp     = PVFB_BPP_RGBA8888;
    ps->ps_open    = 1;

    return 0;
}

/* ARGSUSED */
int
pvfbclose(dev_t dev, int oflag, int otyp, cred_t *crp)
{
    struct pvfb_state *ps = &pvfb_state;

    if (!ps->ps_open)
        return EINVAL;

    GLACCEL_REG(GLACCEL_EXEC) = GLACCEL_EXEC_RESET;

    ps->ps_fbk1    = NULL;
    ps->ps_fbphys  = 0;
    ps->ps_fbpages = 0;
    ps->ps_fbsize  = 0;
    ps->ps_open = 0;
    return 0;
}

/* ARGSUSED */
int
pvfbioctl(dev_t dev, int cmd, caddr_t arg, int mode, cred_t *crp, int *rvalp)
{
    struct pvfb_state *ps = &pvfb_state;
    struct pvfb_mode   m;

    if (!ps->ps_open)
        return EINVAL;

    switch (cmd) {
    case PVFB_SET_MODE:
        if (copyin(arg, &m, sizeof(m)))
            return EFAULT;

        if (m.width == 0 || m.width > PVFB_MAX_WIDTH  ||
            m.height == 0 || m.height > PVFB_MAX_HEIGHT) {
            return EINVAL;
        }

        switch (m.format) {
        case GLACCEL_FMT_RGBA8888:
            ps->ps_bpp = PVFB_BPP_RGBA8888;
            break;
        case GLACCEL_FMT_RGB565:
            ps->ps_bpp = PVFB_BPP_RGB565;
            break;
        default:
            return EINVAL;
        }

        ps->ps_width  = m.width;
        ps->ps_height = m.height;
        ps->ps_format = m.format;

        if (ps->ps_width * ps->ps_height * ps->ps_bpp > (uint)ps->ps_fbsize) {
            cmn_err(CE_WARN, "pvfb: mode %dx%d fmt %d exceeds buffer",
                    m.width, m.height, m.format);
            return ENOMEM;
        }

        GLACCEL_REG(GLACCEL_WIDTH)   = ps->ps_width;
        GLACCEL_REG(GLACCEL_HEIGHT)  = ps->ps_height;
        GLACCEL_REG(GLACCEL_FORMAT)  = ps->ps_format;
        GLACCEL_REG(GLACCEL_STRIDE)  = 0;
        GLACCEL_REG(GLACCEL_FB_BASE) = (__uint32_t)(ps->ps_fbphys & 0xFFFFFFFF);
        GLACCEL_REG(GLACCEL_EXEC)    = GLACCEL_EXEC_PROCESS;
        break;

    case PVFB_FLIP:
        GLACCEL_REG(GLACCEL_EXEC) = GLACCEL_EXEC_PROCESS;
        {
            int tries = 10000;
            while (tries-- > 0 &&
                   !(GLACCEL_REG(GLACCEL_STATUS) & GLACCEL_STATUS_DONE))
                ;
        }
        break;

    default:
        return EINVAL;
    }

    return 0;
}

/* ARGSUSED */
int
pvfbmap(dev_t dev, vhandl_t *vt, off_t off, int len, int prot)
{
    struct pvfb_state *ps = &pvfb_state;

    if (!ps->ps_open || ps->ps_fbk1 == NULL)
        return EINVAL;

    if ((size_t)(off + len) > ps->ps_fbsize)
        return EINVAL;

    return v_mapphys(vt, (caddr_t)ps->ps_fbk1 + off, len);
}

/* =====================================================================
 * pvfbedtinit — called from io_init[] at boot.
 *
 * Registers:
 *   1. /hw/pvfb character device (for direct framebuffer access)
 *   2. GfxRegisterBoard("NG1") so Xsgi sees a Newport board
 *   3. add_to_inventory for hinv
 * ===================================================================== */

void
pvfbedtinit(struct edt *edtp)
{
    vertex_hdl_t pvfb_vhdl;
    graph_error_t rv;
    struct pvfb_bdata *bd = &pvfb_bdata;
    struct gfx_data *gd;

    /* Check glaccel is present (probe the PV bank) */
    if (badaddr((void *)GLACCEL_BASE, sizeof(__uint32_t))) {
        return;
    }

    /* Register /hw/pvfb char device */
    rv = hwgraph_char_device_add(hwgraph_root, "pvfb", "pvfb", &pvfb_vhdl);
    if (rv == GRAPH_SUCCESS) {
        hwgraph_chmod(pvfb_vhdl, 0666);
        cmn_err(CE_NOTE, "pvfb: registered /hw/pvfb");
    } else {
        cmn_err(CE_WARN, "pvfb: hwgraph_char_device_add failed (%d)", rv);
    }

    /* Check pvrex3 is present (probe the REX3 register space) */
    if (badaddr((void *)PVREX3_K1_BASE, sizeof(__uint32_t))) {
        cmn_err(CE_NOTE, "pvfb: no pvrex3 at PA 0x%x, skipping gfx registration",
                (unsigned int)PVREX3_PHYS_BASE);
        return;
    }

    /* Initialize board data */
    bzero(bd, sizeof(struct pvfb_bdata));

    /* Set up gfx_data */
    gd = &bd->bd_gfxdata;
    gd->numpcx = 1;

    /* Set up gfx_info */
    strncpy(bd->bd_gfxinfo.name, "NG1", GFX_INFO_NAME_SIZE);
    bd->bd_gfxinfo.xpmax = 1280;
    bd->bd_gfxinfo.ypmax = 1024;
    bd->bd_gfxinfo.length = sizeof(struct gfx_info);

    /* Store REX3 base for gf_MapGfx */
    bd->bd_rex3base = (caddr_t)PVREX3_K1_BASE;

    /* Register with the graphics subsystem */
    if (GfxRegisterBoard(&pvfb_gfx_fncs, gd, &bd->bd_gfxinfo) < 0) {
        cmn_err(CE_WARN, "pvfb: GfxRegisterBoard failed");
        return;
    }

    pvfb_board_registered = 1;
    cmn_err(CE_NOTE, "pvfb: registered NEWPORT board (pvrex3 at PA 0x%x)",
            (unsigned int)PVREX3_PHYS_BASE);

    /* Add to hardware inventory for hinv */
    add_to_inventory(INV_GRAPHICS, INV_NEWPORT, 0, 0, 0);
}

#endif /* IP54 */
