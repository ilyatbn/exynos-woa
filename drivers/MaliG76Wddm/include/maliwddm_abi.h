/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The private data maliwddm (the WDDM 1.3 kernel driver) exchanges with its
 * user-mode side through the D3D runtime: allocation private data, the
 * command buffer of a render, and escapes. Shared by the KMD, the UMD's
 * Panfrost backend and the D3DKMT test (tools/windows/m5test.c).
 *
 * The model (docs/M5.2-PLAN.md): the KMD owns the Mali's GPU address space.
 * It reserves a GPU address for every allocation when it is created, maps
 * the allocation's pages there whenever VidMm makes it resident, and the UMD
 * asks for the address once (MW_ESCAPE_ALLOC_INFO). Addresses never move,
 * so nothing is ever patched.
 */
#pragma once

#ifndef _KERNEL_MODE
#include <stdint.h>
typedef uint32_t mw_u32;
typedef uint64_t mw_u64;
#else
typedef unsigned __int32 mw_u32;
typedef unsigned __int64 mw_u64;
#endif

/* Allocation private data (D3DDDI_ALLOCATIONINFO.pPrivateDriverData). */
#define MW_ALLOC_NOEXEC         0x1     /* not shader code: may lie anywhere, XN */
#define MW_ALLOC_PRIMARY        0x2     /* scan-out capable (step d) */
#define MW_ALLOC_CPU_ONDEMAND   0x4     /* exynos-woa G-game: CPU address space only while locked (KMD parameter
                                         * CpuVisibleOnDemand = 4) - set by the shim on Panfrost BOs only */

typedef struct _MW_ALLOCATION_INFO {
    mw_u32 Version;                     /* MW_ABI_VERSION */
    mw_u32 Flags;                       /* MW_ALLOC_* */
    mw_u64 Size;                        /* bytes; rounded up to pages */
    /* Surfaces the runtime creates itself (primary, shadow, staging); 0 for
     * plain buffers. Format is a D3DDDIFORMAT. */
    mw_u32 Width, Height, Pitch, Format;
} MW_ALLOCATION_INFO;

#define MW_ABI_VERSION          1

/* A render's command buffer is a sequence of these. */
#define MW_CMD_JOB              1       /* run one job chain, wait for it */

typedef struct _MW_COMMAND {
    mw_u32 Type;                        /* MW_CMD_* */
    mw_u32 Slot;                        /* 0: fragment, 1: vertex/tiler/compute */
    mw_u64 Jc;                          /* GPU address of the first job header */
} MW_COMMAND;

/* At most this many commands per render; also the DMA buffer private data. */
#define MW_MAX_COMMANDS         32

/* Escapes (D3DKMTEscape, private driver data starts with the code). */
#define MW_ESCAPE_ALLOC_INFO    1
#define MW_ESCAPE_GPU_INFO      2
#define MW_ESCAPE_STATS         3
#define MW_ESCAPE_SCANOUT       4       /* exynos-woa H4a: the scan-out framebuffer in the GPU's address space */
#define MW_ESCAPE_CLOCK         5       /* exynos-woa: GPU cycle counter + QPC, for a live clock reading */
#define MW_ESCAPE_PANEL_POWER   6       /* exynos-woa: panel display off / on (DCS 0x28 / 0x29) - the power button */

typedef struct _MW_ESCAPE_ALLOC_INFO_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_ALLOC_INFO */
    mw_u32 hAllocation;                 /* in: D3DKMT allocation handle */
    mw_u64 GpuVa;                       /* out */
    mw_u64 Size;                        /* out */
} MW_ESCAPE_ALLOC_INFO_DATA;

/* exynos-woa H4a: the framebuffer the panel scans, mapped at GpuVa (0 = not mapped: no GPU half / no framebuffer),
 * linear, Format a D3DDDIFORMAT. hAllocation != 0: from now on that allocation (a primary the UMD presents from)
 * is "GPU-presented" - the UMD has copied its frame into the framebuffer with the GPU already, so the KMD's CPU
 * blt / latch copy of it to the framebuffer is skipped. */
typedef struct _MW_ESCAPE_SCANOUT_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_SCANOUT */
    mw_u32 hAllocation;                 /* in: D3DKMT allocation to mark GPU-presented, or 0 */
    mw_u64 GpuVa;                       /* out */
    mw_u32 Width, Height, Pitch, Format;    /* out */
} MW_ESCAPE_SCANOUT_DATA;

/* GET_PARAM's values for the UMD, by Panfrost parameter number. */
#define MW_GPU_PARAMS           45
typedef struct _MW_ESCAPE_GPU_INFO_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_GPU_INFO */
    mw_u32 Count;                       /* out: valid entries */
    mw_u64 Value[MW_GPU_PARAMS];        /* out; index = DRM_PANFROST_PARAM_* */
    mw_u64 Valid;                       /* out: bit n = Value[n] is meaningful */
} MW_ESCAPE_GPU_INFO_DATA;

/* The KMD's counters, for tests and bring-up. */
typedef struct _MW_ESCAPE_STATS_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_STATS */
    mw_u32 GpuUp;
    mw_u32 Renders, Presents, Submits;
    mw_u32 LastSubmittedFence, LastCompletedFence;
    mw_u32 Queued, Running;
    mw_u32 JobsDone, JobsFailed, JobsTimedOut;
    mw_u32 LastJsStatus, LastFaultStatus;
    mw_u32 Irqs, Resets;
    mw_u64 LastFaultAddress;
    /* BuildPagingBuffer: calls by DXGK_BUILDPAGINGBUFFER_OPERATION (0-15),
     * and the last aperture map. */
    mw_u32 PagingOps[16];
    mw_u32 Maps, MapFails, Unmaps, LastMapPages, LastMapStatus, Allocations;
    mw_u64 LastMapVa, LastAllocVa;
    /* What VidMm handed DxgkDdiPatch: the allocation list of the last
     * patched DMA buffer, first entry. */
    mw_u32 Patches, PatchAllocations, PatchPatchLocations, PatchSegment0;
    mw_u64 PatchAddress0;
    /* Presents carried out on the CPU, and skipped (no pages known); PresentGpu (was Reserved0): presents from a
     * GPU-presented allocation, whose CPU copy was skipped (exynos-woa H4a). */
    mw_u32 PresentBlts, PresentFills, PresentSkips, PresentGpu;
} MW_ESCAPE_STATS_DATA;

/* MW_ESCAPE_CLOCK: two calls a known time apart give the GPU clock: (Cycles2 - Cycles1) * QpcFreq / (Qpc2 - Qpc1).
 * The KMD starts the cycle counter (GPU_CMD 5) after every reset. Valid 0 = GPU not powered / not up. */
typedef struct _MW_ESCAPE_CLOCK_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_CLOCK */
    mw_u32 Valid;
    mw_u64 Cycles;                      /* GPU_CYCLE_COUNT */
    mw_u64 Timestamp;                   /* GPU_TIMESTAMP (system counter) */
    mw_u64 Qpc, QpcFreq;                /* KeQueryPerformanceCounter at the read */
} MW_ESCAPE_CLOCK_DATA;

/* MW_ESCAPE_PANEL_POWER (s10ekeys "screen"): On = 1 lights the panel, 0 darkens it; Windows' own display state is not
 * touched (it never wakes the display again on this phone - no recognised power button, touch ignored while off). */
typedef struct _MW_ESCAPE_PANEL_POWER_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_PANEL_POWER */
    mw_u32 On;                          /* in */
    mw_u32 Done;                        /* out: 1 if the DCS command went out */
} MW_ESCAPE_PANEL_POWER_DATA;
