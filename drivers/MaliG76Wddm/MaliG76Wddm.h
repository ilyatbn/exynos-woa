/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MaliG76Wddm: the display + GPU driver for the Galaxy S10e (Exynos 9820, Mali-G76), M16 G10/G11.
 *
 * Derived from drivers/maliwddm (the WDDM 1.3 driver) and drivers/malidod (the display-only driver) of
 * wddm-mali-g52 (https://github.com/gahingwoo/wddm-mali-g52, commit 16f7e7c6, GPL-2.0, Copyright gahingwoo),
 * written there for the RK3576's Mali-G52 and its HDMI output. Shape and several WDDM details follow virtio-win's
 * viogpu3d (BSD-3), as the original says.
 *
 * One binary, three modes, chosen in DriverEntry from Services\MaliG76Wddm\Parameters (notes/gpu-m16-g10.md):
 *   GpuEnable 0, FullAdapter 0 (default): display-only (KMDOD, malidod's DDI set). No interrupt routine, no GPU
 *       register access. Replaces Basic Display on the GOP framebuffer; DWM renders with Basic Render as today.
 *   FullAdapter 1, GpuEnable 0: the full WDDM 1.3 adapter (maliwddm) with its GPU half off; the GPU's interrupt
 *       outputs are only masked. The INF names maliumd.dll (G12 Mesa build) as the UMD.
 *   GpuEnable 1: the full adapter with the GPU half (maligpu.c, retargeted to the G76): aperture segment, Mali
 *       MMU, one job chain at a time, fences on the job interrupt plus a 1 ms poll.
 * Rescue: firmware hides DSDT GPU0 (BEYOND0LTE_GPU_POWER_ON 0 image, or ESP \S10EDIAG\nogpu.flg) - then this
 * driver never loads and Basic Display keeps the screen.
 * NOT VALIDATED ON HARDWARE.
 */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>
#include <ntstrsafe.h>
#include "maligpu.h"
#include "maliwddm_abi.h"

#define MW_TAG                  'dwlM'
#define MW_SEGMENT_ID           1           /* the aperture segment */
/* 1 GiB of GPU VA that VidMm commits system pages into (not a carve-out). The S10e gives Windows 5.24 GiB; the
 * VA allocator spans MK_VA_START..MK_VA_END, so this must stay below 4 GB - 32 MB (notes/gpu-m16-g10.md). */
#define MW_SEGMENT_SIZE         (1024ULL * 1024 * 1024)
#define MW_QUEUE_DEPTH          16          /* submissions the KMD holds */

#define MW_W2(x)                L##x
#define MW_W(x)                 MW_W2(x)

/* ---- the DSDT GPU0 _CRS contract (Dsdt.asl Device (GPU0); Exynos9820.h) ---- */
#define MW_GPU_PA               0x18500000ULL   /* EXYNOS_G3D_BASE (SOURCE-ONLY, mali@18500000) */
#define MW_GPU_LEN              0x5000u         /* EXYNOS_G3D_SIZE */
#define MW_PMU_PAGE_PA          0x15862000ULL   /* EXYNOS_PMU_BASE 0x15860000 (VERIFIED) + 0x2000 */
#define MW_PMU_PAGE_LEN         0x1000u
#define MW_IRQS                 3u              /* JOB 320, MMU 319, GPU 321 (GSIV, UNVERIFIED trigger) */

/* ---- the GOP framebuffer the firmware leaves (Exynos9820.h EXYNOS_FB_*, SimpleFramebufferDxe.c:420-430) ----
 * Only compared with what dxgkrnl reports (FbMatchesGop); dxgkrnl's values are the ones used. */
#define MW_GOP_FB_PA            0xCA000000ULL   /* EXYNOS_FB_BASE (VERIFIED, s3cfb.bootloaderfb) */
#define MW_GOP_WIDTH            1080u           /* EXYNOS_FB_WIDTH (VERIFIED, DTBO s6e3fa7_beyond0) */
#define MW_GOP_HEIGHT           2280u           /* EXYNOS_FB_HEIGHT (VERIFIED) */
#define MW_GOP_PITCH            (1080u * 4u)    /* EXYNOS_FB_STRIDE_PIXELS * 4 (GOP PixelBlueGreenRedReserved8) */

/* ---- modes ---- */
typedef enum _MW_MODE {
    MwModeDisplayOnly = 0,      /* KMDOD */
    MwModeFullNoGpu = 1,        /* WDDM 1.3, GPU half off */
    MwModeFullGpu = 2,          /* WDDM 1.3 + GPU */
} MW_MODE;

extern MW_MODE MwMode;          /* set once in DriverEntry */
/* Parameters\SoftVsync (default 1): the full adapter reports a 60 Hz software vsync while dxgkrnl has it enabled
 * (command.c); 0 = the original's behaviour (ControlInterrupt says yes, no vsync is ever reported). */
extern ULONG MwSoftVsync;
/* Parameters\FlipBlt (default 1): a Flip present is carried out as a CPU copy of the new primary into the GOP
 * framebuffer (command.c); 0 = the original's behaviour (flips are dropped, the panel never shows them). */
extern ULONG MwFlipBlt;
/* Parameters\RecommendVidPn (default 1): the full adapter answers RecommendFunctionalVidPn with its one path and
 * mode pinned (display.c); 0 = the original's "no recommendation". Display-only never recommends (malidod). */
extern ULONG MwRecommendVidPn;
/* Parameters\SynthEdid (default 1): the full adapter answers QueryDeviceDescriptor with an EDID it builds for its
 * own timing (display.c); 0 = no descriptor, as the original. Display-only never sends one (malidod). */
extern ULONG MwSynthEdid;
/* Parameters\SegmentCpuVisible (default 1): the aperture segment is reported CPU visible (memory.c), which dxgkrnl
 * requires of every segment a shared primary may be written in. */
extern ULONG MwSegmentCpuVisible;
/* Parameters\ModeRefreshExact (default 1): DescribeAllocation reports a primary's refresh rate exactly as the
 * VidPN target mode carries it (memory.c); 0 = the original's 60/1. */
extern ULONG MwModeRefreshExact;
/* Parameters\Refresh601 (default 1): the full adapter's timing frequencies as reduced rationals (60/1). */
extern ULONG MwRefresh601;
/* Parameters\FlipMmio (default 1): the full adapter reports FlipCaps.FlipOnVSyncMmIo; dxgkrnl then flips through
 * SetVidPnSourceAddress, the software vsync "latches" the new primary (CPU copy into the GOP fb) and reports its
 * address (command.c). 0 = no flip caps (DMA flips, FlipBlt at SubmitCommand). */
extern ULONG MwFlipMmio;

/* ---- objects ---- */

typedef struct _MW_ALLOCATION {
    SIZE_T Size;
    ULONG Pages;
    ULONG Flags;                /* MW_ALLOC_* */
    ULONG64 GpuVa;              /* reserved at creation, fixed for life */
    BOOLEAN Mapped;             /* resident: pages mapped at GpuVa */
    /* Standard allocations (primary, shadow, staging) the runtime creates. */
    UINT Width, Height, Pitch;
    D3DDDIFORMAT Format;
    /* While mapped: the backing pages VidMm handed MAP_APERTURE_SEGMENT,
     * which is how a present reaches the pixels on the CPU. */
    PMDL Mdl;
    ULONG MdlPage;
    /* While mapped: segment BaseAddress (MK_VA_START) + OffsetInPages * PAGE_SIZE, the form PrimaryAddress is
     * matched against for flips that come without hAllocation (UNVERIFIED form - the PRAM lookup lines check it). */
    ULONG64 SegAddress;
    ULONG64 SegOffset;          /* OffsetInPages * PAGE_SIZE alone, matched too */
} MW_ALLOCATION;

typedef struct _MW_ADAPTER MW_ADAPTER;

typedef struct _MW_DEVICE {
    MW_ADAPTER *Adapter;
} MW_DEVICE;

typedef struct _MW_CONTEXT {
    MW_DEVICE *Device;
} MW_CONTEXT;

/* A present the CPU carries out at submission (step d): a blt or a colour
 * fill into the primary (the POST framebuffer) or another allocation. */
#define MW_PRESENT_NONE         0
#define MW_PRESENT_BLT          1
#define MW_PRESENT_FILL         2
#define MW_MAX_RECTS            16

typedef struct _MW_PRESENT_OP {
    ULONG Kind;
    BOOLEAN ToScanout;          /* G76: a flip - the destination is the GOP framebuffer, whatever Dst is */
    ULONG Color;
    MW_ALLOCATION *Src, *Dst;
    RECT SrcRect, DstRect;
    ULONG NumRects;             /* 0: DstRect alone */
    RECT Rects[MW_MAX_RECTS];   /* destination sub-rectangles */
} MW_PRESENT_OP;

/* What a DMA buffer carries, kept in its private data by Render and found
 * there again by SubmitCommand. */
typedef struct _MW_DMA_PRIVATE {
    ULONG Count;
    ULONG Reserved;
    MW_COMMAND Commands[MW_MAX_COMMANDS];
    MW_PRESENT_OP Present;
} MW_DMA_PRIVATE;

/* A submission the hardware works through, one job chain at a time. */
typedef struct _MW_SUBMISSION {
    ULONG FenceId;
    ULONG Count, Next;          /* commands, and the next one to start */
    MW_COMMAND Commands[MW_MAX_COMMANDS];
} MW_SUBMISSION;

struct _MW_ADAPTER {
    PDEVICE_OBJECT Pdo;
    DXGKRNL_INTERFACE Dxgk;
    MW_MODE Mode;
    ULONG64 Seen;               /* DDIs already traced (see MwRet) */
    ULONG64 Entered;            /* DDIs whose first entry is traced */
    ULONG CallOrder;

    /* Display: the firmware's framebuffer, as malidod. */
    DXGK_DISPLAY_INFORMATION Fb;
    PUCHAR FbVa;
    SIZE_T FbSize;
    BOOLEAN SourceVisible, PathActive;

    /* GPU. MmuLock serialises the address allocator and the page tables. */
    MK_GPU Gpu;
    FAST_MUTEX MmuLock;
    BOOLEAN GpuUp;
    BOOLEAN ResourcesOk;        /* the translated resources match the GPU0 contract */

    /* Submissions, shared with the ISR (DxgkCbSynchronizeExecution). */
    MW_SUBMISSION Queue[MW_QUEUE_DEPTH];
    ULONG QHead;                /* ring; QHead is the one running */
    volatile ULONG QCount;
    BOOLEAN Running;            /* a job chain of Queue[QHead] is on the GPU */
    BOOLEAN NeedDpc;            /* a fence completed: queue the DPC */
    ULONG RunningSlot;
    ULONG64 JobStart;           /* interrupt time the running chain started */
    ULONG LastSubmittedFence, LastCompletedFence;
    ULONG JobsDone, JobsFailed, JobsTimedOut, ChainFlag;
    ULONG LastJsStatus, LastFaultStatus;
    ULONG64 LastFaultAddress;

    /* Polls the job status while work is in flight (command.c). */
    KTIMER PollTimer;
    KDPC PollDpc;
    volatile LONG PollArmed;
    volatile BOOLEAN Stopping;

    /* Counters for the trace. */
    ULONG Renders, Presents, Submits, DodPresents;
    ULONG PagingOps[16];
    ULONG Maps, MapFails, Unmaps, LastMapPages, Allocations;
    NTSTATUS LastMapStatus;
    ULONG64 LastMapVa, LastAllocVa;
    ULONG Patches, PatchAllocations, PatchPatchLocations, PatchSegment0;
    ULONG PresentBlts, PresentFills, PresentSkips;
    ULONG64 PatchAddress0;

    /* PRAM trace heartbeat (pram.c): a 1 Hz timer from StartDevice to StopDevice, and what it reports. */
    KTIMER HbTimer;
    KDPC HbDpc;
    BOOLEAN HbArmed;

    /* Software vsync (command.c, Parameters\SoftVsync): a 60 Hz timer while dxgkrnl has CRTC_VSYNC enabled. */
    KTIMER VsTimer;
    KDPC VsDpc;
    volatile LONG VsEnabled;
    PHYSICAL_ADDRESS ScanoutAddress;    /* the last SetVidPnSourceAddress PrimaryAddress, reported with each vsync */
    /* FlipMmio: the primary SetVidPnSourceAddress named, copied to the GOP fb at the next vsync (ScanLock). */
    KSPIN_LOCK ScanLock;                /* held by the latch while it copies, and by FreeAllocation's forget */
    MW_ALLOCATION * volatile PendingPrimary;    /* set lock-free by SetVidPnSourceAddress (any IRQL) */
    PHYSICAL_ADDRESS PendingAddress;
    /* Mapped primaries, for flips whose DXGKARG_SETVIDPNSOURCEADDRESS.hAllocation is NULL (MS Learn: hAllocation is
     * only "if non-NULL"; the allocation is then named by PrimarySegment/PrimaryAddress). Written at PASSIVE with
     * interlocked exchanges, read lock-free at DIRQL. */
    MW_ALLOCATION * volatile Prims[8];
    volatile LONG Lookups;
    volatile LONG Latches;
    volatile LONG IsrCalls, DpcCalls, VsyncTicks;
};

/* ---- trace: Seen_<ddi>, Fail_<ddi> in the device's registry key ---- */
void MwLog(MW_ADAPTER *A, PCWSTR Name, ULONG Value);
void MwLogHook(void *Owner, PCWSTR Name, ULONG Value);
NTSTATUS MwRet(MW_ADAPTER *A, PCWSTR Name, NTSTATUS St);
void MwEnter(MW_ADAPTER *A, PCWSTR Name);

/* pram.c: DDI trace into the reserved RAM the firmware saves as \WINPRAM.BIN (any IRQL) */
void MwPramInit(ULONG Enable, ULONG Mode, ULONG Build);
void MwPramFree(void);
void MwPramText(BOOLEAN First, const CHAR *Tag, PCWSTR Name, ULONG Value, BOOLEAN HasValue);
void MwPramDdi(BOOLEAN First, BOOLEAN Exit, const CHAR *Name, NTSTATUS Status);
void MwPramHeartbeat(ULONG Isr, ULONG Dpc, ULONG Vsync, ULONG Submits, ULONG Presents, ULONG Fence);

/* display.c: VidPN and the POST framebuffer (both modes), PresentDisplayOnly (display-only mode) */
NTSTATUS MwDisplayStart(MW_ADAPTER *A);
void MwDisplayStop(MW_ADAPTER *A);
void MwModeRefresh(MW_ADAPTER *A, D3DDDI_RATIONAL *Refresh);   /* the VidPN target mode's VSyncFreq */
NTSTATUS MwDisplayOnlyCaps(const DXGKARG_QUERYADAPTERINFO *Info);
DXGKDDI_QUERY_CHILD_RELATIONS MwQueryChildRelations;
DXGKDDI_QUERY_CHILD_STATUS MwQueryChildStatus;
DXGKDDI_QUERY_DEVICE_DESCRIPTOR MwQueryDeviceDescriptor;
DXGKDDI_ISSUPPORTEDVIDPN MwIsSupportedVidPn;
DXGKDDI_RECOMMENDFUNCTIONALVIDPN MwRecommendFunctionalVidPn;
DXGKDDI_ENUMVIDPNCOFUNCMODALITY MwEnumVidPnCofuncModality;
DXGKDDI_SETVIDPNSOURCEVISIBILITY MwSetVidPnSourceVisibility;
DXGKDDI_COMMITVIDPN MwCommitVidPn;
DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH MwUpdateActiveVidPnPresentPath;
DXGKDDI_RECOMMENDMONITORMODES MwRecommendMonitorModes;
DXGKDDI_QUERYVIDPNHWCAPABILITY MwQueryVidPnHWCapability;
DXGKDDI_SETVIDPNSOURCEADDRESS MwSetVidPnSourceAddress;
DXGKDDI_PRESENTDISPLAYONLY MwPresentDisplayOnly;
DXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP MwStopDeviceAndReleasePostDisplayOwnership;
DXGKDDI_SYSTEM_DISPLAY_ENABLE MwSystemDisplayEnable;
DXGKDDI_SYSTEM_DISPLAY_WRITE MwSystemDisplayWrite;

/* memory.c: allocations and the aperture segment */
DXGKDDI_CREATEALLOCATION MwCreateAllocation;
DXGKDDI_DESTROYALLOCATION MwDestroyAllocation;
DXGKDDI_DESCRIBEALLOCATION MwDescribeAllocation;
DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA MwGetStandardAllocationDriverData;
DXGKDDI_OPENALLOCATIONINFO MwOpenAllocation;
DXGKDDI_CLOSEALLOCATION MwCloseAllocation;
DXGKDDI_BUILDPAGINGBUFFER MwBuildPagingBuffer;
NTSTATUS MwQuerySegment(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info);
NTSTATUS MwEscapeAllocInfo(MW_ADAPTER *A, const DXGKARG_ESCAPE *Esc, MW_ESCAPE_ALLOC_INFO_DATA *D);

/* command.c: devices, contexts, render, submission, fences */
DXGKDDI_CREATEDEVICE MwCreateDevice;
DXGKDDI_DESTROYDEVICE MwDestroyDevice;
DXGKDDI_CREATECONTEXT MwCreateContext;
DXGKDDI_DESTROYCONTEXT MwDestroyContext;
DXGKDDI_RENDER MwRender;
DXGKDDI_RENDERKM MwRenderKm;
DXGKDDI_PRESENT MwPresent;
DXGKDDI_PATCH MwPatch;
DXGKDDI_SUBMITCOMMAND MwSubmitCommand;
DXGKDDI_PREEMPTCOMMAND MwPreemptCommand;
DXGKDDI_QUERYCURRENTFENCE MwQueryCurrentFence;
DXGKDDI_RESETFROMTIMEOUT MwResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT MwRestartFromTimeout;
DXGKDDI_COLLECTDBGINFO MwCollectDbgInfo;
DXGKDDI_GETNODEMETADATA MwGetNodeMetadata;
DXGKDDI_RESETENGINE MwResetEngine;
DXGKDDI_QUERYENGINESTATUS MwQueryEngineStatus;
DXGKDDI_QUERYDEPENDENTENGINEGROUP MwQueryDependentEngineGroup;
DXGKDDI_CANCELCOMMAND MwCancelCommand;
DXGKDDI_CONTROLINTERRUPT MwControlInterrupt;
DXGKDDI_GETSCANLINE MwGetScanLine;
BOOLEAN MwJobInterrupt(MW_ADAPTER *A);    /* from the ISR, at DIRQL */
void MwScanoutForget(MW_ADAPTER *A, MW_ALLOCATION *Alloc);   /* memory.c FreeAllocation: drop a pending latch */
void MwScanoutRegister(MW_ADAPTER *A, MW_ALLOCATION *Alloc, BOOLEAN Mapped); /* memory.c map/unmap of a primary */
MW_ALLOCATION *MwScanoutLookup(MW_ADAPTER *A, ULONG64 PrimaryAddress);     /* display.c, any IRQL */
void MwCommandInit(MW_ADAPTER *A);
void MwCommandStop(MW_ADAPTER *A);
void MwArmPoll(MW_ADAPTER *A);
