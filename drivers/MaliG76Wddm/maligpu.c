// SPDX-License-Identifier: GPL-2.0
/*
 * The Mali-G76 itself: see maligpu.h. Derived from drivers/common/maligpu.c of wddm-mali-g52 (commit 16f7e7c6,
 * GPL-2.0, Copyright gahingwoo); the sequences follow Linux Panfrost (panfrost_gpu.c, panfrost_mmu.c,
 * panfrost_job.c). Changes for the S10e are marked "G76:" and listed in maligpu.h.
 */
#include "maligpu.h"

/* Poll (reg & mask) == want, 10 us apart, for up to Us microseconds. */
static BOOLEAN Poll(MK_GPU *Gpu, ULONG Off, ULONG Mask, ULONG Want, ULONG Us)
{
    for (ULONG i = 0; i <= Us / 10; i++) {
        if ((MK_RD(Gpu, Off) & Mask) == Want)
            return TRUE;
        KeStallExecutionProcessor(10);
    }
    return FALSE;
}

static ULONG PopCount64(ULONG64 V)
{
    ULONG n = 0;
    for (; V != 0; V &= V - 1)
        n++;
    return n;
}

/* ---- power-up ---- */

enum { STEP_PMU = 1, STEP_ID, STEP_RESET, STEP_POWER, STEP_MMU, STEP_READY, STEP_LAYOUT };

/* G76: the firmware (Beyond0LteGpuProbeDxe, M16 G9) powers the G3D domain and sets its clock; DSDT GPU0 only
 * exists when it did. A GPU access while the domain is off is an SError, so look before touching - through the
 * PMU page in _CRS, polarity "1 = on" for both status registers (notes/gpu-m16-g2g3.md F3, F5). */
BOOLEAN MkPowered(MK_GPU *Gpu)
{
    if (Gpu->Pmu == NULL)
        return FALSE;
    return (READ_REGISTER_ULONG((PULONG)((PUCHAR)Gpu->Pmu + MK_PMU_EMB_G3D_STATUS)) & 1) != 0 &&
           (READ_REGISTER_ULONG((PULONG)((PUCHAR)Gpu->Pmu + MK_PMU_G3D_STATUS)) & 1) != 0;
}

NTSTATUS MkGpuProbe(MK_GPU *Gpu)
{
    ULONG id, pa;

    if (Gpu->Regs == NULL || Gpu->Pmu == NULL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    MK_LOG(Gpu, L"Step", STEP_PMU);
    MK_LOG(Gpu, L"PmuEmbeddedG3dStatus", READ_REGISTER_ULONG((PULONG)((PUCHAR)Gpu->Pmu + MK_PMU_EMB_G3D_STATUS)));
    MK_LOG(Gpu, L"PmuG3dStatus", READ_REGISTER_ULONG((PULONG)((PUCHAR)Gpu->Pmu + MK_PMU_G3D_STATUS)));
    if (!MkPowered(Gpu))
        return STATUS_DEVICE_NOT_READY;

    MK_LOG(Gpu, L"Step", STEP_ID);
    id = MK_RD(Gpu, GPU_ID);
    MK_LOG(Gpu, L"GpuId", id);
    if (((id >> 16) & MK_G76_ID_MASK) != MK_G76_ID_MODEL)      /* G76 (R5) */
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    /* G76 (R11): the PA width the MMU reports bounds the page tables and every page VidMm may hand us. */
    Gpu->F.MmuFeatures = MK_RD(Gpu, GPU_MMU_FEATURES);
    pa = (Gpu->F.MmuFeatures >> 8) & 0xFF;
    Gpu->PaBits = (pa == 0 || pa > MK_PA_BITS_MAX) ? MK_PA_BITS_MAX : pa;
    MK_LOG(Gpu, L"MmuFeatures", Gpu->F.MmuFeatures);
    MK_LOG(Gpu, L"PaBits", Gpu->PaBits);
    return STATUS_SUCCESS;
}

void MkGpuQuiet(MK_GPU *Gpu)
{
    if (Gpu->Regs == NULL || !MkPowered(Gpu))
        return;
    MK_WR(Gpu, GPU_INT_MASK, 0);
    MK_WR(Gpu, JOB_INT_MASK, 0);
    MK_WR(Gpu, MMU_INT_MASK, 0);
    MK_WR(Gpu, GPU_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, JOB_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, MMU_INT_CLEAR, 0xffffffff);
}

static void ReadFeatures(MK_GPU *Gpu)
{
    GPU_FEATURES *f = &Gpu->F;
    ULONG id = MK_RD(Gpu, GPU_ID);

    f->Id = id >> 16;
    f->Revision = id & 0xffff;
    f->L2Features = MK_RD(Gpu, GPU_L2_FEATURES);
    f->CoreFeatures = MK_RD(Gpu, GPU_CORE_FEATURES);
    f->TilerFeatures = MK_RD(Gpu, GPU_TILER_FEATURES);
    f->MemFeatures = MK_RD(Gpu, GPU_MEM_FEATURES);
    f->MmuFeatures = MK_RD(Gpu, GPU_MMU_FEATURES);
    f->ThreadFeatures = MK_RD(Gpu, GPU_THREAD_FEATURES);
    f->MaxThreads = MK_RD(Gpu, GPU_THREAD_MAX_THREADS);
    f->MaxWorkgroupSize = MK_RD(Gpu, GPU_THREAD_MAX_WORKGROUP_SIZE);
    f->MaxBarrierSize = MK_RD(Gpu, GPU_THREAD_MAX_BARRIER_SIZE);
    f->CoherencyFeatures = MK_RD(Gpu, GPU_COHERENCY_FEATURES);   /* Bifrost v7 has COHERENCY_REG */
    f->AfbcFeatures = MK_RD(Gpu, GPU_AFBC_FEATURES);
    for (ULONG i = 0; i < 4; i++)
        f->TextureFeatures[i] = MK_RD(Gpu, GPU_TEXTURE_FEATURES(i));
    f->AsPresent = MK_RD(Gpu, GPU_AS_PRESENT);
    f->JsPresent = MK_RD(Gpu, GPU_JS_PRESENT);
    for (ULONG i = 0; i < 16; i++)
        if (f->JsPresent & (1u << i))
            f->JsFeatures[i] = MK_RD(Gpu, GPU_JS_FEATURES(i));
    f->ShaderPresent = MK_RD(Gpu, GPU_SHADER_PRESENT_LO) | ((ULONG64)MK_RD(Gpu, GPU_SHADER_PRESENT_HI) << 32);
    f->TilerPresent = MK_RD(Gpu, GPU_TILER_PRESENT_LO) | ((ULONG64)MK_RD(Gpu, GPU_TILER_PRESENT_HI) << 32);
    f->L2Present = MK_RD(Gpu, GPU_L2_PRESENT_LO) | ((ULONG64)MK_RD(Gpu, GPU_L2_PRESENT_HI) << 32);
    f->StackPresent = MK_RD(Gpu, GPU_STACK_PRESENT_LO) | ((ULONG64)MK_RD(Gpu, GPU_STACK_PRESENT_HI) << 32);
    f->ThreadTlsAlloc = MK_RD(Gpu, GPU_THREAD_TLS_ALLOC);
    f->NrCoreGroups = PopCount64(f->L2Present);
}

static BOOLEAN SoftReset(MK_GPU *Gpu)
{
    /* G76: every output masked first (as MaliG76Probe), not only GPU_INT. */
    MK_WR(Gpu, GPU_INT_MASK, 0);
    MK_WR(Gpu, JOB_INT_MASK, 0);
    MK_WR(Gpu, MMU_INT_MASK, 0);
    MK_WR(Gpu, GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
    MK_WR(Gpu, GPU_CMD, GPU_CMD_SOFT_RESET);
    if (!Poll(Gpu, GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 10000)) {
        MK_WR(Gpu, GPU_CMD, GPU_CMD_HARD_RESET);
        if (!Poll(Gpu, GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 10000))
            return FALSE;
    }
    MK_WR(Gpu, GPU_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, JOB_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, MMU_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, GPU_INT_MASK, GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT);
    return TRUE;
}

static BOOLEAN GpuPowerOn(MK_GPU *Gpu)
{
    GPU_FEATURES *f = &Gpu->F;

    /* panfrost_gpu_init_quirks (panfrost_gpu.c:117-175) for a G76: SC_TLS_HASH_ENABLE because hw_features_g76 has
     * TLS_HASHING (panfrost_features.h:85-97) and hw_issues_g76 is 0 (panfrost_issues.h:259); TILER_CONFIG
     * written back as read; the default IDVS group size. GPU_L2_MMU_CONFIG is not written (panfrost does not; R6). */
    MK_WR(Gpu, GPU_SHADER_CONFIG, SC_TLS_HASH_ENABLE);
    MK_WR(Gpu, GPU_TILER_CONFIG, MK_RD(Gpu, GPU_TILER_CONFIG));
    MK_WR(Gpu, GPU_JM_CONFIG, JM_DEFAULT_IDVS_GROUP_SIZE << JM_IDVS_GROUP_SIZE_SHIFT);

    /* G76: 20 ms for every domain, as panfrost's readl_relaxed_poll_timeout (the original gave the tiler 1 ms). */
    MK_WR(Gpu, L2_PWRON_LO, (ULONG)f->L2Present);
    if (!Poll(Gpu, L2_READY_LO, 0xffffffff, (ULONG)f->L2Present, 20000))
        return FALSE;
    MK_WR(Gpu, SHADER_PWRON_LO, (ULONG)f->ShaderPresent);
    if (!Poll(Gpu, SHADER_READY_LO, 0xffffffff, (ULONG)f->ShaderPresent, 20000))
        return FALSE;
    MK_WR(Gpu, TILER_PWRON_LO, (ULONG)f->TilerPresent);
    return Poll(Gpu, TILER_READY_LO, 0xffffffff, (ULONG)f->TilerPresent, 20000);
}

static BOOLEAN AsWaitReady(MK_GPU *Gpu)
{
    return Poll(Gpu, AS0 + AS_STATUS, AS_STATUS_ACTIVE, 0, 10000);
}

static void AsEnable(MK_GPU *Gpu)
{
    MK_WR(Gpu, MMU_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, MMU_INT_MASK, 0xffffffff);
    /* Mali LPAE: table, read-inner. Memory attributes as Linux programs them
     * for this format; ATTRINDX 1 is write-allocate. */
    MK_WR(Gpu, AS0 + AS_TRANSTAB_LO, (ULONG)(Gpu->L0Pa | 0x7));
    MK_WR(Gpu, AS0 + AS_TRANSTAB_HI, (ULONG)(Gpu->L0Pa >> 32));
    MK_WR(Gpu, AS0 + AS_MEMATTR_LO, 0x00888d88);
    MK_WR(Gpu, AS0 + AS_MEMATTR_HI, 0);
    MK_WR(Gpu, AS0 + AS_TRANSCFG_LO, 0);
    MK_WR(Gpu, AS0 + AS_TRANSCFG_HI, 0);
    (void)AsWaitReady(Gpu);
    MK_WR(Gpu, AS0 + AS_COMMAND, AS_COMMAND_UPDATE);
    (void)AsWaitReady(Gpu);
}

/* Done and error for job slots 0-2 (the original's 0x00070007), limited to the slots JS_PRESENT reports. */
static ULONG JobIntMask(MK_GPU *Gpu)
{
    ULONG js = Gpu->F.JsPresent & 0x7;
    return js | (js << 16);
}

BOOLEAN MkGpuBringup(MK_GPU *Gpu)
{
    if (!MkPowered(Gpu))
        return FALSE;
    if (!SoftReset(Gpu))
        return FALSE;
    if (!GpuPowerOn(Gpu))
        return FALSE;
    AsEnable(Gpu);
    MK_WR(Gpu, JOB_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, JOB_INT_MASK, JobIntMask(Gpu));
    return TRUE;
}

NTSTATUS MkGpuInit(MK_GPU *Gpu)
{
    GPU_FEATURES *f = &Gpu->F;

    /* MkGpuProbe passed (powered, G76) and MkMmuInit built L0; look again, time has passed. */
    if (!MkPowered(Gpu) || Gpu->L0 == NULL)
        return STATUS_DEVICE_NOT_READY;

    MK_LOG(Gpu, L"Step", STEP_RESET);
    if (!SoftReset(Gpu))
        return STATUS_DEVICE_NOT_READY;
    ReadFeatures(Gpu);
    MK_LOG(Gpu, L"ShaderPresent", (ULONG)f->ShaderPresent);
    MK_LOG(Gpu, L"ShaderPresentHi", (ULONG)(f->ShaderPresent >> 32));
    MK_LOG(Gpu, L"TilerPresent", (ULONG)f->TilerPresent);
    MK_LOG(Gpu, L"L2Present", (ULONG)f->L2Present);
    MK_LOG(Gpu, L"JsPresent", f->JsPresent);
    MK_LOG(Gpu, L"AsPresent", f->AsPresent);
    MK_LOG(Gpu, L"CoherencyFeatures", f->CoherencyFeatures);
    MK_LOG(Gpu, L"Js1Features", f->JsFeatures[1]);

    /* G76 (R7): the power-up and the job affinity write only the *_LO words and use every core, which is what
     * panfrost_get_core_mask (panfrost_gpu.c:429-450) does only when there is one L2 slice. Anything else
     * (two core groups, cores above bit 31) needs the panfrost core-group mask - not guessed here. */
    MK_LOG(Gpu, L"Step", STEP_LAYOUT);
    if (PopCount64(f->L2Present) != 1 || (f->ShaderPresent >> 32) != 0 || (ULONG)f->ShaderPresent == 0 ||
        (f->TilerPresent >> 32) != 0 || (ULONG)f->TilerPresent == 0 || (f->L2Present >> 32) != 0)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    MK_LOG(Gpu, L"Step", STEP_POWER);
    if (!GpuPowerOn(Gpu))
        return STATUS_DEVICE_NOT_READY;
    MK_LOG(Gpu, L"ShaderConfig", MK_RD(Gpu, GPU_SHADER_CONFIG));
    MK_LOG(Gpu, L"JmConfig", MK_RD(Gpu, GPU_JM_CONFIG));

    MK_LOG(Gpu, L"Step", STEP_MMU);
    AsEnable(Gpu);
    MK_WR(Gpu, JOB_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, JOB_INT_MASK, JobIntMask(Gpu));

    Gpu->Ready = TRUE;
    MK_LOG(Gpu, L"Step", STEP_READY);
    return STATUS_SUCCESS;
}

void MkGpuStop(MK_GPU *Gpu)
{
    if (!Gpu->Ready)
        return;
    Gpu->Ready = FALSE;
    if (!MkPowered(Gpu))
        return;
    MK_WR(Gpu, JOB_INT_MASK, 0);
    MK_WR(Gpu, MMU_INT_MASK, 0);
    MK_WR(Gpu, GPU_INT_MASK, 0);
}

/* ---- page tables ---- */

static PULONG64 AllocTable(MK_GPU *Gpu, PULONG64 Pa)
{
    PHYSICAL_ADDRESS low, high, skip;
    PVOID p;
    ULONG bits = Gpu->PaBits != 0 ? Gpu->PaBits : MK_PA_BITS_MAX;

    low.QuadPart = 0;
    high.QuadPart = (LONGLONG)((1ULL << bits) - 1);     /* G76: what the GPU reports it can address (R11) */
    skip.QuadPart = 0;
    /* Uncached, so the GPU's walker sees every write without maintenance. */
    p = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, low, high, skip, MmNonCached);
    if (p == NULL)
        return NULL;
    RtlZeroMemory(p, PAGE_SIZE);
    *Pa = (ULONG64)MmGetPhysicalAddress(p).QuadPart;
    return (PULONG64)p;
}

static void FreeTable(PULONG64 T)
{
    if (T != NULL)
        MmFreeContiguousMemorySpecifyCache(T, PAGE_SIZE, MmNonCached);
}

NTSTATUS MkMmuInit(MK_GPU *Gpu)
{
    ULONG64 l1pa;
    SIZE_T bytes = ((SIZE_T)MK_VA_PAGES + 31) / 32 * 4;

    Gpu->L0 = AllocTable(Gpu, &Gpu->L0Pa);
    Gpu->L1 = AllocTable(Gpu, &l1pa);
    Gpu->VaBits = (PULONG)ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, 'kmlM');
    if (Gpu->L0 == NULL || Gpu->L1 == NULL || Gpu->VaBits == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Gpu->L0[0] = l1pa | MK_PTE_TABLE;
    RtlInitializeBitMap(&Gpu->VaMap, Gpu->VaBits, MK_VA_PAGES);
    RtlClearAllBits(&Gpu->VaMap);
    return STATUS_SUCCESS;
}

void MkMmuFree(MK_GPU *Gpu)
{
    for (ULONG g = 0; g < 4; g++) {
        for (ULONG i = 0; i < 512; i++) {
            FreeTable(Gpu->L3[g][i]);
            Gpu->L3[g][i] = NULL;
        }
        FreeTable(Gpu->L2[g]);
        Gpu->L2[g] = NULL;
    }
    FreeTable(Gpu->L1);
    FreeTable(Gpu->L0);
    Gpu->L1 = Gpu->L0 = NULL;
    if (Gpu->VaBits != NULL)
        ExFreePoolWithTag(Gpu->VaBits, 'kmlM');
    Gpu->VaBits = NULL;
}

/* The L3 table covering Va, created on first use. */
static PULONG64 L3For(MK_GPU *Gpu, ULONG64 Va)
{
    ULONG g = (ULONG)(Va >> 30) & 3, m = (ULONG)(Va >> 21) & 511;
    ULONG64 pa;

    if (Gpu->L2[g] == NULL) {
        Gpu->L2[g] = AllocTable(Gpu, &pa);
        if (Gpu->L2[g] == NULL)
            return NULL;
        Gpu->L1[g] = pa | MK_PTE_TABLE;
    }
    if (Gpu->L3[g][m] == NULL) {
        Gpu->L3[g][m] = AllocTable(Gpu, &pa);
        if (Gpu->L3[g][m] == NULL)
            return NULL;
        Gpu->L2[g][m] = pa | MK_PTE_TABLE;
    }
    return Gpu->L3[g][m];
}

static ULONG Fls64(ULONG64 X)
{
    ULONG idx;
    return _BitScanReverse64(&idx, X) ? idx + 1 : 0;
}

/* panfrost_mmu.c: lock the region, then flush the page-table caches. */
static void FlushRange(MK_GPU *Gpu, ULONG64 Va, ULONG64 Size)
{
    ULONG width;
    ULONG64 region;

    if (!Gpu->Ready || !MkPowered(Gpu))
        return;
    width = Fls64(Va ^ (Va + Size - 1));
    if (width < 15)
        width = 15;                         /* AS_LOCK_REGION_MIN_SIZE: 32 KB */
    width -= 1;
    region = (Va & ~((1ULL << width) - 1)) | width;

    (void)AsWaitReady(Gpu);
    MK_WR(Gpu, AS0 + AS_LOCKADDR_LO, (ULONG)region);
    MK_WR(Gpu, AS0 + AS_LOCKADDR_HI, (ULONG)(region >> 32));
    MK_WR(Gpu, AS0 + AS_COMMAND, AS_COMMAND_LOCK);
    (void)AsWaitReady(Gpu);
    MK_WR(Gpu, AS0 + AS_COMMAND, AS_COMMAND_FLUSH_PT);
    (void)AsWaitReady(Gpu);
}

BOOLEAN MkVaAlloc(MK_GPU *Gpu, ULONG Pages, BOOLEAN Exec, ULONG64 *Va)
{
    ULONG hint = 0, idx;

    if (Exec && (ULONG64)Pages * PAGE_SIZE > MK_EXEC_WINDOW)
        return FALSE;
    for (ULONG tries = 0; tries < 512; tries++) {
        idx = RtlFindClearBits(&Gpu->VaMap, Pages, hint);
        if (idx == 0xFFFFFFFF)
            return FALSE;
        ULONG64 start = MK_VA_START + ((ULONG64)idx << PAGE_SHIFT);
        ULONG64 last = start + ((ULONG64)Pages << PAGE_SHIFT) - 1;
        if (!Exec || (start / MK_EXEC_WINDOW) == (last / MK_EXEC_WINDOW)) {
            RtlSetBits(&Gpu->VaMap, idx, Pages);
            *Va = start;
            return TRUE;
        }
        hint = (ULONG)(((last & ~(MK_EXEC_WINDOW - 1)) - MK_VA_START) >> PAGE_SHIFT);
    }
    return FALSE;
}

void MkVaFree(MK_GPU *Gpu, ULONG64 Va, ULONG Pages)
{
    RtlClearBits(&Gpu->VaMap, (ULONG)((Va - MK_VA_START) >> PAGE_SHIFT), Pages);
}

NTSTATUS MkMmuMapPages(MK_GPU *Gpu, ULONG64 Va, const PFN_NUMBER *Pfn, ULONG Pages, BOOLEAN NoExec)
{
    ULONG64 attr = MK_PTE_PAGE | (NoExec ? MK_PTE_XN : 0);
    ULONG64 limit = 1ULL << (Gpu->PaBits != 0 ? Gpu->PaBits : MK_PA_BITS_MAX);

    /* G76 (R11): a page the GPU cannot address would alias low memory - refuse before writing anything. */
    for (ULONG i = 0; i < Pages; i++)
        if (((ULONG64)Pfn[i] << PAGE_SHIFT) >= limit)
            return STATUS_GRAPHICS_NO_VIDEO_MEMORY;

    for (ULONG i = 0; i < Pages; i++) {
        ULONG64 va = Va + ((ULONG64)i << PAGE_SHIFT);
        PULONG64 l3 = L3For(Gpu, va);
        if (l3 == NULL) {
            /* Undo what was written; the tables themselves stay. */
            for (ULONG j = 0; j < i; j++) {
                ULONG64 v = Va + ((ULONG64)j << PAGE_SHIFT);
                L3For(Gpu, v)[(v >> 12) & 511] = 0;
            }
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        l3[(va >> 12) & 511] = ((ULONG64)Pfn[i] << PAGE_SHIFT) | attr;
    }
    MkStoreBarrier();
    FlushRange(Gpu, Va, (ULONG64)Pages << PAGE_SHIFT);
    return STATUS_SUCCESS;
}

void MkMmuUnmapPages(MK_GPU *Gpu, ULONG64 Va, ULONG Pages)
{
    for (ULONG i = 0; i < Pages; i++) {
        ULONG64 va = Va + ((ULONG64)i << PAGE_SHIFT);
        PULONG64 l3 = Gpu->L3[(va >> 30) & 3][(va >> 21) & 511];
        if (l3 != NULL)
            l3[(va >> 12) & 511] = 0;
    }
    MkStoreBarrier();
    FlushRange(Gpu, Va, (ULONG64)Pages << PAGE_SHIFT);
}

/* ---- jobs ---- */

void MkJobStart(MK_GPU *Gpu, ULONG Slot, ULONG64 Jc, BOOLEAN ChainFlag)
{
    ULONG cfg = 0 /* AS0 */ | JS_CONFIG_THREAD_PRI(8) |
                JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE | JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE |
                (ChainFlag ? JS_CONFIG_JOB_CHAIN_FLAG : 0);

    MK_WR(Gpu, JS_SLOT(Slot) + JS_HEAD_NEXT_LO, (ULONG)Jc);
    MK_WR(Gpu, JS_SLOT(Slot) + JS_HEAD_NEXT_HI, (ULONG)(Jc >> 32));
    MK_WR(Gpu, JS_SLOT(Slot) + JS_AFFINITY_NEXT_LO, (ULONG)Gpu->F.ShaderPresent);
    MK_WR(Gpu, JS_SLOT(Slot) + JS_AFFINITY_NEXT_HI, (ULONG)(Gpu->F.ShaderPresent >> 32));
    MK_WR(Gpu, JS_SLOT(Slot) + JS_CONFIG_NEXT, cfg);
    MkStoreBarrier();
    MK_WR(Gpu, JS_SLOT(Slot) + JS_COMMAND_NEXT, JS_COMMAND_START);
}

/* What the ISR saw plus what the raw register says now, so a misrouted
 * interrupt only costs latency. The raw bits are acknowledged. */
ULONG MkJobTakeStatus(MK_GPU *Gpu, ULONG Slot)
{
    ULONG mine = (1u << Slot) | (1u << (16 + Slot));
    ULONG bits = (ULONG)InterlockedAnd(&Gpu->JobIrqStatus, ~(LONG)mine) & mine;
    ULONG raw = MK_RD(Gpu, JOB_INT_RAWSTAT) & mine;

    if (raw != 0)
        MK_WR(Gpu, JOB_INT_CLEAR, raw);
    return bits | raw;
}

ULONG MkJobSlotStatus(MK_GPU *Gpu, ULONG Slot)
{
    return MK_RD(Gpu, JS_SLOT(Slot) + JS_STATUS);
}

void MkJobHardStop(MK_GPU *Gpu, ULONG Slot)
{
    MK_WR(Gpu, JS_SLOT(Slot) + JS_COMMAND, JS_COMMAND_HARD_STOP);
}

/* ---- interrupts ---- */

/* One handler for all three lines (job, MMU, GPU): it claims whatever is
 * pending, so a wrong line order in the DSDT costs nothing. */
BOOLEAN MkIsr(MK_GPU *Gpu)
{
    ULONG j, m, g;

    if (!Gpu->Ready)
        return FALSE;
    /* G76: never read a GPU register of a domain that went down (that is an SError, not a missed interrupt). */
    if (!MkPowered(Gpu)) {
        Gpu->IsrUnpowered++;
        return FALSE;
    }
    j = MK_RD(Gpu, JOB_INT_STAT);
    m = MK_RD(Gpu, MMU_INT_STAT);
    g = MK_RD(Gpu, GPU_INT_STAT);
    if ((j | m | g) == 0)
        return FALSE;
    if (j != 0) {
        MK_WR(Gpu, JOB_INT_CLEAR, j);
        InterlockedOr(&Gpu->JobIrqStatus, (LONG)j);
    }
    if (m != 0) {
        Gpu->LastMmuFault = MK_RD(Gpu, AS0 + AS_FAULTSTATUS);
        Gpu->LastFaultAddress = MK_RD(Gpu, AS0 + AS_FAULTADDRESS_LO) |
                                ((ULONG64)MK_RD(Gpu, AS0 + AS_FAULTADDRESS_HI) << 32);
        MK_WR(Gpu, MMU_INT_CLEAR, m);
        InterlockedOr(&Gpu->MmuIrqStatus, (LONG)m);
    }
    if (g != 0)
        MK_WR(Gpu, GPU_INT_CLEAR, g);
    Gpu->Irqs++;
    return TRUE;
}
