/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The Mali-G76 of the Galaxy S10e (Exynos 9820), for MaliG76Wddm: power-up and reset, the GPU's page tables and
 * address space, the job slots and the interrupt status. No scheduling, no objects: the driver decides who maps
 * what and when jobs run.
 *
 * Derived from drivers/common/maligpu.h of wddm-mali-g52 (https://github.com/gahingwoo/wddm-mali-g52, commit
 * 16f7e7c6, GPL-2.0, Copyright gahingwoo), written there for the RK3576's Mali-G52. Retargeted per
 * notes/gpu-wddm-mali-g52.md R4-R11 and notes/gpu-m16-g10.md:
 *   R4  "powered" = PMU+0x2104 (EMBEDDED_G3D_STATUS) bit0 == 1 AND PMU+0x2384 (G3D_STATUS) bit0 == 1, read through
 *       the PMU page that is the second memory resource of DSDT GPU0 (RK: a hard-coded PMU PA, bits clear = on).
 *   R5  GPU_ID gate ((id >> 16) & 0xFF0F) == 0x7201 (G76) instead of (id >> 16) == 0x7402 (G52).
 *   R6  panfrost_gpu_init_quirks for G76: SHADER_CONFIG = SC_TLS_HASH_ENABLE in addition to TILER/JM_CONFIG.
 *   R7  one core group only (L2_PRESENT one bit, shader cores in the low word), else the GPU half stays off.
 *   R11 page tables (and the WDDM HighestAcceptableAddress) below min(2^40, 2^MMU_FEATURES.PA_BITS).
 * Register names and offsets as in Linux drm/panfrost panfrost_regs.h (same values as the original file).
 * NOT VALIDATED ON HARDWARE.
 */
#pragma once
#include <ntddk.h>

/* ---- the PMU page in _CRS (EXYNOS_PMU_BASE 0x15860000 + 0x2000, Exynos9820.h; notes/gpu-m16-g2g3.md F3) ---- */
#define MK_PMU_EMB_G3D_STATUS   0x104       /* EMBEDDED_G3D_STATUS 0x2104 - 0x2000 */
#define MK_PMU_G3D_STATUS       0x384       /* G3D_STATUS 0x2384 - 0x2000 */

#define GPU_ID                  0x000
#define GPU_L2_FEATURES         0x004
#define GPU_CORE_FEATURES       0x008
#define GPU_TILER_FEATURES      0x00C
#define GPU_MEM_FEATURES        0x010
#define GPU_MMU_FEATURES        0x014       /* PA bits = (v >> 8) & 0xFF (panfrost_regs.h:18-20) */
#define GPU_AS_PRESENT          0x018
#define GPU_JS_PRESENT          0x01C
#define GPU_INT_RAWSTAT         0x020
#define GPU_INT_CLEAR           0x024
#define GPU_INT_MASK            0x028
#define GPU_INT_STAT            0x02C
#define GPU_CMD                 0x030
#define GPU_STATUS              0x034
#define GPU_FAULT_STATUS        0x03C
#define GPU_FAULT_ADDRESS_LO    0x040
#define GPU_AFBC_FEATURES       0x04C
#define GPU_THREAD_MAX_THREADS  0x0A0
#define GPU_THREAD_MAX_WORKGROUP_SIZE 0x0A4
#define GPU_THREAD_MAX_BARRIER_SIZE 0x0A8
#define GPU_THREAD_FEATURES     0x0AC
#define GPU_TEXTURE_FEATURES(n) (0x0B0 + (n) * 4)
#define GPU_JS_FEATURES(n)      (0x0C0 + (n) * 4)
#define GPU_SHADER_PRESENT_LO   0x100
#define GPU_SHADER_PRESENT_HI   0x104
#define GPU_TILER_PRESENT_LO    0x110
#define GPU_TILER_PRESENT_HI    0x114
#define GPU_L2_PRESENT_LO       0x120
#define GPU_L2_PRESENT_HI       0x124
#define SHADER_READY_LO         0x140
#define TILER_READY_LO          0x150
#define L2_READY_LO             0x160
#define SHADER_PWRON_LO         0x180
#define TILER_PWRON_LO          0x190
#define L2_PWRON_LO             0x1A0
#define GPU_COHERENCY_FEATURES  0x300
#define GPU_THREAD_TLS_ALLOC    0x310
#define GPU_STACK_PRESENT_LO    0xE00
#define GPU_STACK_PRESENT_HI    0xE04
#define GPU_JM_CONFIG           0xF00
#define GPU_SHADER_CONFIG       0xF04       /* panfrost_regs.h:180 */
#define GPU_TILER_CONFIG        0xF08

#define GPU_CMD_SOFT_RESET      0x01
#define GPU_CMD_HARD_RESET      0x02
#define GPU_IRQ_FAULT           (1u << 0)
#define GPU_IRQ_MULTIPLE_FAULT  (1u << 7)
#define GPU_IRQ_RESET_COMPLETED (1u << 8)

#define SC_TLS_HASH_ENABLE      (1u << 17)  /* panfrost_regs.h:211; panfrost_gpu.c:141-145 (G76 has TLS_HASHING) */
#define JM_IDVS_GROUP_SIZE_SHIFT 16
#define JM_DEFAULT_IDVS_GROUP_SIZE 0xF

#define MK_G76_ID_MASK          0xFF0Fu     /* R5: masks arch_rev, bits 7:4 of GPU_ID[31:16] (pan_model.h:25-28) */
#define MK_G76_ID_MODEL         0x7201u     /* PAN_PROD_ID(7,2,1) "G76", mesa pan_model.c:80 */

#define JOB_INT_RAWSTAT         0x1000
#define JOB_INT_CLEAR           0x1004
#define JOB_INT_MASK            0x1008
#define JOB_INT_STAT            0x100C
#define JS_BASE                 0x1800
#define JS_SLOT(n)              (JS_BASE + (n) * 0x80)
#define JS_HEAD_LO              0x00
#define JS_HEAD_HI              0x04
#define JS_COMMAND              0x20
#define JS_STATUS               0x24
#define JS_HEAD_NEXT_LO         0x40
#define JS_HEAD_NEXT_HI         0x44
#define JS_AFFINITY_NEXT_LO     0x50
#define JS_AFFINITY_NEXT_HI     0x54
#define JS_CONFIG_NEXT          0x58
#define JS_COMMAND_NEXT         0x60
#define JS_COMMAND_START        0x01
#define JS_COMMAND_HARD_STOP    0x03

#define JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE (3u << 8)
#define JS_CONFIG_JOB_CHAIN_FLAG (1u << 11)
#define JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE (3u << 12)
#define JS_CONFIG_THREAD_PRI(n) ((n) << 16)

#define MMU_INT_RAWSTAT         0x2000
#define MMU_INT_CLEAR           0x2004
#define MMU_INT_MASK            0x2008
#define MMU_INT_STAT            0x200C
#define AS0                     0x2400
#define AS_TRANSTAB_LO          0x00
#define AS_TRANSTAB_HI          0x04
#define AS_MEMATTR_LO           0x08
#define AS_MEMATTR_HI           0x0C
#define AS_LOCKADDR_LO          0x10
#define AS_LOCKADDR_HI          0x14
#define AS_COMMAND              0x18
#define AS_FAULTSTATUS          0x1C
#define AS_FAULTADDRESS_LO      0x20
#define AS_FAULTADDRESS_HI      0x24
#define AS_STATUS               0x28
#define AS_TRANSCFG_LO          0x30
#define AS_TRANSCFG_HI          0x34
#define AS_COMMAND_UPDATE       0x01
#define AS_COMMAND_LOCK         0x02
#define AS_COMMAND_FLUSH_PT     0x04
#define AS_STATUS_ACTIVE        0x01

/* ---- the GPU virtual address space, as Linux Panfrost lays it out ---- */
#define MK_VA_START             0x2000000ULL            /* 32 MB */
#define MK_VA_END               0x100000000ULL          /* 4 GB */
#define MK_VA_PAGES             ((ULONG)((MK_VA_END - MK_VA_START) >> PAGE_SHIFT))
#define MK_EXEC_WINDOW          0x1000000ULL            /* the PC is 24 bits */

/* Mali LPAE page table entries (R10: the legacy format of the original; mainline panfrost uses it on every SoC but
 * MediaTek MT8188/8192/8370, panfrost_mmu.c:786-795 - whether the G76 accepts it is what G9b/G11 prove). */
#define MK_PTE_TABLE            0x3ULL
#define MK_PTE_PAGE             0x2C5ULL    /* block | ATTRINDX 1 | read | write | outer shareable */
#define MK_PTE_XN               (1ULL << 54)

#define MK_PA_BITS_MAX          40          /* the original's limit (maligpu.c AllocTable) */

typedef struct _GPU_FEATURES {
    ULONG Id, Revision;
    ULONG64 ShaderPresent, TilerPresent, L2Present, StackPresent;
    ULONG AsPresent, JsPresent;
    ULONG L2Features, CoreFeatures, TilerFeatures, MemFeatures, MmuFeatures;
    ULONG ThreadFeatures, MaxThreads, MaxWorkgroupSize, MaxBarrierSize;
    ULONG CoherencyFeatures, AfbcFeatures, ThreadTlsAlloc, NrCoreGroups;
    ULONG TextureFeatures[4];
    ULONG JsFeatures[16];
} GPU_FEATURES;

/* Diagnostics go wherever the owning driver keeps them (the registry). */
typedef void (*MK_LOG_FN)(void *Owner, PCWSTR Name, ULONG Value);

typedef struct _MK_GPU {
    volatile ULONG *Regs;       /* first _CRS memory resource: the G3D block */
    SIZE_T RegsLength;
    volatile ULONG *Pmu;        /* second _CRS memory resource: the PMU page with the G3D status registers */
    SIZE_T PmuLength;
    BOOLEAN Ready;              /* powered and AS0 live: registers may be touched */
    ULONG PaBits;               /* min(MK_PA_BITS_MAX, MMU_FEATURES.PA_BITS); 0 until MkGpuProbe */
    GPU_FEATURES F;

    /* AS0's page tables: L0, one L1, up to four L2 (one per GB), L3 on demand.
     * Callers serialise map and unmap. */
    PULONG64 L0, L1, L2[4];
    PULONG64 L3[4][512];
    ULONG64 L0Pa;
    RTL_BITMAP VaMap;
    PULONG VaBits;

    /* What the ISR collected, for whoever waits on jobs. */
    volatile LONG JobIrqStatus; /* JOB_INT_STAT bits */
    volatile LONG MmuIrqStatus;
    ULONG Irqs, Resets, LastMmuFault, IsrUnpowered;
    ULONG64 LastFaultAddress;

    MK_LOG_FN Log;
    void *LogOwner;
} MK_GPU;

#define MK_RD(g, o)     READ_REGISTER_ULONG((PULONG)((PUCHAR)(g)->Regs + (o)))
#define MK_WR(g, o, v)  WRITE_REGISTER_ULONG((PULONG)((PUCHAR)(g)->Regs + (o)), (v))
#define MK_LOG(g, n, v) do { if ((g)->Log) (g)->Log((g)->LogOwner, (n), (v)); } while (0)

/* Complete every store this CPU has made, to memory the GPU reads through a
 * write-combined (Normal non-cacheable) mapping, before the GPU is told. */
#define MkStoreBarrier() __dsb(_ARM64_BARRIER_SY)

/* R4: both PMU status bits 1. Safe at any IRQL (the PMU is always on); needs Gpu->Pmu. */
BOOLEAN MkPowered(MK_GPU *Gpu);

/* Before anything else: powered, GPU_ID is a G76, PA bits. Reads only (GPU_ID, MMU_FEATURES). */
NTSTATUS MkGpuProbe(MK_GPU *Gpu);
/* Quiet the three interrupt outputs (masks 0, pending bits cleared) - the GpuEnable=0 full-adapter mode. */
void MkGpuQuiet(MK_GPU *Gpu);

/* Power-up from cold (after MkGpuProbe and MkMmuInit); Bringup again after a fault (reset, power, AS0, masks). */
NTSTATUS MkGpuInit(MK_GPU *Gpu);
BOOLEAN MkGpuBringup(MK_GPU *Gpu);
void MkGpuStop(MK_GPU *Gpu);

/* Page tables and addresses. MkVaAlloc keeps executable ranges inside one
 * 16 MB window (the shader program counter is 24 bits). */
NTSTATUS MkMmuInit(MK_GPU *Gpu);
void MkMmuFree(MK_GPU *Gpu);
BOOLEAN MkVaAlloc(MK_GPU *Gpu, ULONG Pages, BOOLEAN Exec, ULONG64 *Va);
void MkVaFree(MK_GPU *Gpu, ULONG64 Va, ULONG Pages);
NTSTATUS MkMmuMapPages(MK_GPU *Gpu, ULONG64 Va, const PFN_NUMBER *Pfn, ULONG Pages, BOOLEAN NoExec);
void MkMmuUnmapPages(MK_GPU *Gpu, ULONG64 Va, ULONG Pages);

/* Jobs: start a chain on a slot without waiting; collect its done (bit
 * slot) and error (bit 16 + slot) status, from the ISR or the raw register. */
void MkJobStart(MK_GPU *Gpu, ULONG Slot, ULONG64 Jc, BOOLEAN ChainFlag);
ULONG MkJobTakeStatus(MK_GPU *Gpu, ULONG Slot);
ULONG MkJobSlotStatus(MK_GPU *Gpu, ULONG Slot);
void MkJobHardStop(MK_GPU *Gpu, ULONG Slot);

/* One handler for all three lines; claims whatever is pending. */
BOOLEAN MkIsr(MK_GPU *Gpu);
