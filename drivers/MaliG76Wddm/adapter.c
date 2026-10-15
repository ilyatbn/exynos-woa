// SPDX-License-Identifier: GPL-2.0
/*
 * MaliG76Wddm: driver entry and mode choice, PnP and power, adapter capabilities, interrupts, escapes, and the
 * registry trace that found every M5.1 bug of the original.
 *
 * Derived from wddm-mali-g52 drivers/maliwddm/adapter.c and drivers/malidod/malidod.c (DriverEntry) (commit
 * 16f7e7c6, GPL-2.0, Copyright gahingwoo). Changes are marked "G76:" (notes/gpu-m16-g10.md): the mode switch
 * (Parameters\GpuEnable, Parameters\FullAdapter), the GPU0 resource contract (GPU block + PMU page + 3 interrupts),
 * the GPU half only when GpuEnable = 1, and HighestAcceptableAddress from the GPU's PA bits.
 */
#include "MaliG76Wddm.h"

MW_MODE MwMode = MwModeDisplayOnly;
ULONG MwSoftVsync = 1;
ULONG MwFlipBlt = 1;
ULONG MwRecommendVidPn = 1;
ULONG MwSynthEdid = 1;
ULONG MwSegmentCpuVisible = 1;
ULONG MwModeRefreshExact = 1;
ULONG MwRefresh601 = 1;
ULONG MwFlipMmio = 1;
ULONG MwScanoutStride = 0;
ULONG MwOfferRotation = 0;      /* Parameters\OfferRotation: 1 = offer 90/180/270 (needs a rotating present path) */
ULONG MwCddDwmInterop = 1;
/* G15 S1 (notes/gpu-wddm2-design.md): Parameters\Wddm2 = 1 -> DXGKDDI_INTERFACE_VERSION_WDDM2_0 in physical-engine mode
 * (no GPU VA caps; Render/Patch/SubmitCommand unchanged). Default 0 = the WDDM 1.3 adapter, byte-identical paths. */
ULONG MwWddm2 = 0;

/* ---- trace ---- */

void MwLog(MW_ADAPTER *A, PCWSTR Name, ULONG Value)
{
    HANDLE key, sub;
    UNICODE_STRING name, subName = RTL_CONSTANT_STRING(L"MaliG76Wddm");
    OBJECT_ATTRIBUTES oa;

    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "MaliG76Wddm: %ws = 0x%08x\n", Name, Value);
    /* Into PRAM first: the registry write and its flush below are what a hang or a reset loses. */
    MwPramText(FALSE, "R ", Name, Value, TRUE);
    if (A == NULL || A->Pdo == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(A->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key)))
        return;
    /* A subkey of Device Parameters of its own, so a test run can delete the previous run's trace in one step. */
    InitializeObjectAttributes(&oa, &subName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, key, NULL);
    if (NT_SUCCESS(ZwCreateKey(&sub, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
        RtlInitUnicodeString(&name, Name);
        (void)ZwSetValueKey(sub, &name, 0, REG_DWORD, &Value, sizeof(Value));
        /* To disk now: a bugcheck loses registry writes still in memory,
         * and the last value before one is the one that matters. */
        (void)ZwFlushKey(sub);
        ZwClose(sub);
    }
    ZwClose(key);
}

/* WDK builds are deterministic, so __DATE__ and __TIME__ do not exist; build-package.ps1 passes a number. */
#ifndef MW_BUILD
#define MW_BUILD 0
#endif

void MwLogHook(void *Owner, PCWSTR Name, ULONG Value)
{
    MwLog((MW_ADAPTER *)Owner, Name, Value);
}

/* Enter_<ddi> = call order, written BEFORE a DDI's first call runs, so a
 * DDI that never returns (a bugcheck inside it) is still on record. */
void MwEnter(MW_ADAPTER *A, PCWSTR Name)
{
    if (A == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    ULONG h = 0;
    for (PCWSTR c = Name; *c; c++)
        h = h * 31 + *c;
    if (A->Entered & (1ULL << (h % 64)))
        return;
    A->Entered |= 1ULL << (h % 64);
    MwLog(A, Name, ++A->CallOrder);
}

/* Seen_<ddi> on a DDI's first call, Fail_<ddi> on any failure; Name is "Fail_<ddi>". */
NTSTATUS MwRet(MW_ADAPTER *A, PCWSTR Name, NTSTATUS St)
{
    if (A == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return St;
    ULONG h = 0;
    for (PCWSTR c = Name; *c; c++)
        h = h * 31 + *c;
    if (!(A->Seen & (1ULL << (h % 64)))) {
        WCHAR seen[64];
        UNICODE_STRING us;
        A->Seen |= 1ULL << (h % 64);
        RtlInitEmptyUnicodeString(&us, seen, sizeof(seen));
        if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"Seen_%ws", Name + 5)))
            MwLog(A, seen, (ULONG)St);
        RtlInitEmptyUnicodeString(&us, seen, sizeof(seen));
        if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"Order_%ws", Name + 5)))
            MwLog(A, seen, ++A->CallOrder);
    }
    if (!NT_SUCCESS(St)) {
        MwLog(A, Name, (ULONG)St);
        MwLog(A, L"LastFailedDdi", (ULONG)St);
    }
    return St;
}

/* ---- PnP ---- */

/* PRAM heartbeat, 1 Hz while started (pram.c MwPramHeartbeat). */
static VOID HbDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    MwPramHeartbeat((ULONG)a->IsrCalls, (ULONG)a->DpcCalls, (ULONG)a->VsyncTicks, a->Submits,
                    a->Presents + a->DodPresents + (ULONG)a->Latches, a->LastCompletedFence);
    MwPramStats((ULONG)a->Opens, (ULONG)a->StdRefused, (ULONG)a->Latches, a->PresentSkips, a->PresentBlts);
}

static void HbStop(MW_ADAPTER *A)
{
    if (!A->HbArmed)
        return;
    KeCancelTimer(&A->HbTimer);
    KeFlushQueuedDpcs();
    A->HbArmed = FALSE;
}

static NTSTATUS APIENTRY MwAddDevice(IN_CONST_PDEVICE_OBJECT Pdo, OUT_PPVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(MW_ADAPTER), MW_TAG);
    MwPramDdi(TRUE, FALSE, "AddDevice", 0);
    if (a == NULL)
        return STATUS_NO_MEMORY;
    a->Pdo = (PDEVICE_OBJECT)Pdo;
    a->Mode = MwMode;
    ExInitializeFastMutex(&a->MmuLock);
    a->Gpu.Log = MwLogHook;
    a->Gpu.LogOwner = a;
    MwCommandInit(a);
    KeInitializeSpinLock(&a->ScanLock);
    KeInitializeTimer(&a->HbTimer);
    KeInitializeDpc(&a->HbDpc, HbDpc, a);
    *Context = a;
    MwPramDdi(TRUE, TRUE, "AddDevice", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

/* G76: the DSDT GPU0 _CRS contract (Dsdt.asl): the G3D block first, then the PMU page, then 3 interrupts. The
 * original took the first memory resource whatever it was (maliwddm adapter.c:106-126). Anything else and the GPU
 * half stays off - the display does not depend on it. */
static NTSTATUS MapResources(MW_ADAPTER *A)
{
    DXGK_DEVICE_INFO info;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR mem[2] = {NULL, NULL};
    ULONG memCount = 0, irqCount = 0;
    NTSTATUS st = A->Dxgk.DxgkCbGetDeviceInformation(A->Dxgk.DeviceHandle, &info);

    if (!NT_SUCCESS(st))
        return st;
    PCM_RESOURCE_LIST list = info.TranslatedResourceList;
    for (ULONG l = 0; list && l < list->Count; l++) {
        PCM_PARTIAL_RESOURCE_LIST p = &list->List[l].PartialResourceList;
        for (ULONG i = 0; i < p->Count; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &p->PartialDescriptors[i];
            if (d->Type == CmResourceTypeMemory) {
                if (memCount < 2)
                    mem[memCount] = d;
                memCount++;
            } else if (d->Type == CmResourceTypeInterrupt) {
                irqCount++;
            }
        }
    }
    MwLog(A, L"ResMemory", memCount);
    MwLog(A, L"ResInterrupts", irqCount);
    if (memCount != 2 || irqCount != MW_IRQS ||
        (ULONG64)mem[0]->u.Memory.Start.QuadPart != MW_GPU_PA || mem[0]->u.Memory.Length != MW_GPU_LEN ||
        (ULONG64)mem[1]->u.Memory.Start.QuadPart != MW_PMU_PAGE_PA || mem[1]->u.Memory.Length != MW_PMU_PAGE_LEN) {
        MwLog(A, L"ResContract", 0);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    MwLog(A, L"ResContract", 1);
    A->ResourcesOk = TRUE;
    A->Gpu.PmuLength = MW_PMU_PAGE_LEN;
    A->Gpu.Pmu = (volatile ULONG *)MmMapIoSpaceEx(mem[1]->u.Memory.Start, MW_PMU_PAGE_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    A->Gpu.RegsLength = MW_GPU_LEN;
    A->Gpu.Regs = (volatile ULONG *)MmMapIoSpaceEx(mem[0]->u.Memory.Start, MW_GPU_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    return (A->Gpu.Pmu != NULL && A->Gpu.Regs != NULL) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static void StopGpu(MW_ADAPTER *A)
{
    MkGpuStop(&A->Gpu);
    MkMmuFree(&A->Gpu);
    if (A->Gpu.Regs != NULL) {
        MmUnmapIoSpace((PVOID)A->Gpu.Regs, A->Gpu.RegsLength);
        A->Gpu.Regs = NULL;
    }
    if (A->Gpu.Pmu != NULL) {
        MmUnmapIoSpace((PVOID)A->Gpu.Pmu, A->Gpu.PmuLength);
        A->Gpu.Pmu = NULL;
    }
    A->GpuUp = FALSE;
}

static NTSTATUS APIENTRY MwStartDevice(IN_CONST_PVOID Context, IN_PDXGK_START_INFO StartInfo,
                                       IN_PDXGKRNL_INTERFACE Dxgk, OUT_PULONG Sources, OUT_PULONG Children)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(StartInfo);

    MwPramDdi(TRUE, FALSE, "StartDevice", 0);
    a->Dxgk = *Dxgk;
    MwLog(a, L"Build", MW_BUILD);
    MwLog(a, L"Mode", (ULONG)a->Mode);
    st = MwDisplayStart(a);
    if (!NT_SUCCESS(st)) {
        MwPramDdi(TRUE, TRUE, "StartDevice", st);
        return MwRet(a, L"Fail_StartDisplay", st);
    }

    /* G76: display-only touches nothing of the GPU. The full adapter without the GPU half only masks the GPU's
     * interrupt outputs (dxgkrnl connects our ISR to the three lines; a level line left asserted by the firmware
     * would otherwise never be acknowledged). The GPU half runs only with GpuEnable = 1. */
    if (a->Mode != MwModeDisplayOnly) {
        st = MapResources(a);
        if (NT_SUCCESS(st))
            st = MkGpuProbe(&a->Gpu);
        MwLog(a, L"GpuProbe", (ULONG)st);
        if (NT_SUCCESS(st) && a->Mode == MwModeFullGpu) {
            st = MkMmuInit(&a->Gpu);
            if (NT_SUCCESS(st))
                st = MkGpuInit(&a->Gpu);
            if (!NT_SUCCESS(st))
                MkGpuQuiet(&a->Gpu);
            a->GpuUp = NT_SUCCESS(st);
            MwMapScanout(a);    /* H4a: the GOP framebuffer in the GPU address space */
        } else if (NT_SUCCESS(st)) {
            MkGpuQuiet(&a->Gpu);
            MwLog(a, L"GpuQuieted", 1);
        }
        MwLog(a, L"GpuUp", a->GpuUp ? 1 : (ULONG)st);
    }

    *Sources = 1;
    *Children = 1;
    MwLog(a, L"Started", 1);
    {
        LARGE_INTEGER due;
        due.QuadPart = -10000000LL;     /* 1 s, then every second */
        a->HbArmed = TRUE;
        KeSetTimerEx(&a->HbTimer, due, 1000, &a->HbDpc);
    }
    MwPramDdi(TRUE, TRUE, "StartDevice", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY MwStopDevice(IN_CONST_PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    HbStop(a);
    if (a->Mode != MwModeDisplayOnly) {
        MwCommandStop(a);
        MwLog(a, L"Renders", a->Renders);
        MwLog(a, L"Submits", a->Submits);
        MwLog(a, L"JobsDone", a->JobsDone);
        MwLog(a, L"JobsFailed", a->JobsFailed);
        MwLog(a, L"Irqs", a->Gpu.Irqs);
        MwLog(a, L"IsrUnpowered", a->Gpu.IsrUnpowered);
        StopGpu(a);
    }
    MwDisplayStop(a);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY MwRemoveDevice(IN_CONST_PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    MwPramDdi(TRUE, FALSE, "RemoveDevice", 0);
    HbStop(a);
    MwDisplayStop(a);
    ExFreePoolWithTag(Context, MW_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY MwDispatchIoRequest(IN_CONST_PVOID Context, IN_ULONG Source,
                                             IN_PVIDEO_REQUEST_PACKET Vrp)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Source);
    UNREFERENCED_PARAMETER(Vrp);
    return STATUS_NOT_SUPPORTED;
}

/* No power management: nothing under Windows changes the G3D domain or its clock (the firmware set 260 MHz,
 * MILESTONES G9), and the panel keeps sboot's scan-out. */
static NTSTATUS APIENTRY MwSetPowerState(IN_CONST_PVOID Context, IN_ULONG Uid,
                                         IN_DEVICE_POWER_STATE State, IN_POWER_ACTION Action)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;

    MwLog(a, L"SetPowerStateLast", (Uid << 16) | ((ULONG)State << 8) | (ULONG)Action);
    /* The monitor child (ChildUid 0, display.c) = the internal panel: Windows' display off / on (SC_MONITORPOWER,
     * display-off timeout). The adapter itself (DISPLAY_ADAPTER_HW_ID) keeps the old no-op. */
    /* 1.0.45.1 test (2026-10-14): D3 -> DCS 0x28 worked, but Windows never sends D0 again on this phone (touch is
     * ignored while the display is off and the power button is not a Windows button), so the panel stayed dark.
     * Until a Windows-recognised power button exists, D3 is only logged; D0 makes sure the panel is lit. The power
     * button darkens the panel itself (MW_ESCAPE_PANEL_POWER). */
    if (Uid == 0 && State == PowerDeviceD0)
        MwPanelDisplayPower(a, TRUE);
    return STATUS_SUCCESS;
}

/* Called at bugcheck / hibernation time: stores only. */
static VOID APIENTRY MwResetDevice(IN_CONST_PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    MwPramDdi(TRUE, FALSE, "ResetDevice", 0);
}

static VOID APIENTRY MwUnload(VOID)
{
    MwPramFree();
}

static NTSTATUS APIENTRY MwQueryInterface(IN_CONST_PVOID Context, IN_PQUERY_INTERFACE Qi)
{
    return MwPanelQueryInterface((MW_ADAPTER *)Context, Qi);    /* brightness only (panel.c) */
}

/* ---- interrupts (full adapter only; the display-only mode registers none) ---- */

static BOOLEAN APIENTRY MwInterruptRoutine(IN_CONST_PVOID Context, IN_ULONG MessageNumber)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    UNREFERENCED_PARAMETER(MessageNumber);
    InterlockedIncrement(&a->IsrCalls);
    if (!a->GpuUp || !MkIsr(&a->Gpu))
        return FALSE;
    (void)MwJobInterrupt(a);
    return TRUE;
}

static VOID APIENTRY MwDpcRoutine(IN_CONST_PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    InterlockedIncrement(&a->DpcCalls);
    a->Dxgk.DxgkCbNotifyDpc(a->Dxgk.DeviceHandle);
}

/* ---- adapter information ---- */

/* A field of a structure dxgkrnl sized for our interface version: write it
 * only if it lies inside the buffer (M5.1: the WDK's sizeof is larger). */
#define FITS(Size, Type, Field) (FIELD_OFFSET(Type, Field) + RTL_FIELD_SIZE(Type, Field) <= (Size))

static NTSTATUS DriverCaps(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_DRIVERCAPS *caps = (DXGK_DRIVERCAPS *)Info->pOutputData;
    UINT size = Info->OutputDataSize;
    ULONG bits = (A->GpuUp && A->Gpu.PaBits != 0) ? A->Gpu.PaBits : MK_PA_BITS_MAX;

    if (!FITS(size, DXGK_DRIVERCAPS, WDDMVersion))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(caps, size);
    /* DXGK_DRIVERCAPS.WDDMVersion: 0 for interfaces newer than WIN7 (MS Learn DXGK_DRIVERCAPS) - set for 2.0 only so
     * the 1.3 answer stays as it was. */
    caps->WDDMVersion = MwWddm2 ? (DXGK_WDDMVERSION)0 : DXGKDDI_WDDMv1_3;
    /* G76 (R11): what the Mali reports it can address (MMU_FEATURES PA bits), at most the original's 40 bits. */
    caps->HighestAcceptableAddress.QuadPart = (LONGLONG)((1ULL << bits) - 1);
    caps->MaxAllocationListSlotId = 256;   /* Render patches every allocation, SlotId = index */
    caps->SchedulingCaps.MultiEngineAware = 1;
    /* Preemption off system-wide (the full-mode ESP script sets EnablePreemption 0), as viogpu3d does. */
    caps->SchedulingCaps.PreemptionAware = 1;
    caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
    /* G76: MMIO flips (Parameters\FlipMmio) - every DMA flip of DWM's swap chain came back
     * STATUS_GRAPHICS_PRESENT_MODE_CHANGED (runs g22-g25) whatever the primary looked like. */
    if (MwFlipMmio)
        caps->FlipCaps.FlipOnVSyncMmIo = 1;
    /* H4c (notes/gpu-hw-path.md): MS Learn DXGK_PRESENTATIONCAPS.DriverSupportsCddDwmInterop - "supports CDD present
     * operations to texture allocations that are created by the user-mode driver for the DWM". Windowed D3D swap
     * chains are presented "redirected" (PresentDXGI Blt, no destination: run h3n) and none of the cube's presents
     * ever reached DxgkDdiPresent or DWM (runs h3m-h3q) while this was 0. DoPresent's blt is a generic
     * allocation-to-allocation CPU copy. Parameters\CddDwmInterop (default 1; 0 = the caps before 1.0.37.1). */
    if (MwCddDwmInterop)
        caps->PresentationCaps.DriverSupportsCddDwmInterop = 1;
    if (FITS(size, DXGK_DRIVERCAPS, SupportNonVGA))
        caps->SupportNonVGA = TRUE;
    if (FITS(size, DXGK_DRIVERCAPS, SupportSmoothRotation))
        caps->SupportSmoothRotation = TRUE;
    /* G15 S1 run 2 (notes/gpu-wddm2-design.md): run 1's 2.0 adapter was failed by dxgkrnl right after this query.
     * MS Learn "TDR in Windows 8 and later": per-engine TDR is Mandatory for full graphics drivers (WDDM 1.2+) - the
     * three DDIs are implemented (command.c MwResetEngine/MwQueryEngineStatus/MwQueryDependentEngineGroup); "GPU
     * preemption": WDDM 1.2+ KMDs must support the Windows 8 preemption model - we preempt only between DMA buffers,
     * so DMA_BUFFER_BOUNDARY is the honest granularity. 2.0 only; the 1.3 answer is unchanged. */
    if (MwWddm2) {
        if (FITS(size, DXGK_DRIVERCAPS, SupportPerEngineTDR))
            caps->SupportPerEngineTDR = TRUE;
        if (FITS(size, DXGK_DRIVERCAPS, PreemptionCaps)) {
            caps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
            caps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        }
    }
    return STATUS_SUCCESS;
}

static void GpuInfo(MW_ADAPTER *A, MW_ESCAPE_GPU_INFO_DATA *D)
{
    const GPU_FEATURES *f = &A->Gpu.F;
    mw_u64 v[MW_GPU_PARAMS] = {0};

    /* Indices are DRM_PANFROST_PARAM_*, as malikm's GET_PARAM. */
    v[0] = f->Id; v[1] = f->Revision; v[2] = f->ShaderPresent; v[3] = f->TilerPresent;
    v[4] = f->L2Present; v[5] = f->StackPresent; v[6] = f->AsPresent; v[7] = f->JsPresent;
    v[8] = f->L2Features; v[9] = f->CoreFeatures; v[10] = f->TilerFeatures; v[11] = f->MemFeatures;
    v[12] = f->MmuFeatures; v[13] = f->ThreadFeatures; v[14] = f->MaxThreads;
    v[15] = f->MaxWorkgroupSize; v[16] = f->MaxBarrierSize; v[17] = f->CoherencyFeatures;
    for (ULONG i = 0; i < 4; i++)
        v[18 + i] = f->TextureFeatures[i];
    for (ULONG i = 0; i < 16; i++)
        v[22 + i] = f->JsFeatures[i];
    v[38] = f->NrCoreGroups; v[39] = f->ThreadTlsAlloc; v[40] = f->AfbcFeatures;
    v[44] = 31;                                  /* SELECTED_COHERENCY: none */
    RtlCopyMemory(D->Value, v, sizeof(v));
    D->Count = MW_GPU_PARAMS;
    D->Valid = (1ULL << 41) - 1;                 /* 0..40 */
    D->Valid |= 1ULL << 44;
    if (!A->GpuUp)
        D->Valid = 0;
}

static NTSTATUS APIENTRY MwQueryAdapterInfo(IN_CONST_HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *Info)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    NTSTATUS st;

    if (a->Mode == MwModeDisplayOnly) {
        st = MwDisplayOnlyCaps(Info);
    } else {
        switch (Info->Type) {
        case DXGKQAITYPE_DRIVERCAPS:
            st = DriverCaps(a, Info);
            break;
        case DXGKQAITYPE_QUERYSEGMENT3:
            st = MwQuerySegment(a, Info);
            break;
        case DXGKQAITYPE_QUERYSEGMENT4:      /* WDDM 2.0: the same single aperture segment, stride-described */
            st = MwWddm2 ? MwQuerySegment4(a, Info) : STATUS_NOT_SUPPORTED;
            break;
        case DXGKQAITYPE_PHYSICALADAPTERCAPS:
            /* WDDM 2.0 physical-engine mode: one node, paging on it, no IoMmu/GpuMmu flags. DxgkPhysicalAdapterHandle =
             * the DXGKRNL_INTERFACE::DeviceHandle from DxgkDdiStartDevice (MS Learn DXGK_PHYSICALADAPTERCAPS). */
            if (!MwWddm2) {
                st = STATUS_NOT_SUPPORTED;
            } else if (Info->OutputDataSize < FIELD_OFFSET(DXGK_PHYSICALADAPTERCAPS, Flags) + sizeof(DXGK_PHYSICALADAPTERFLAGS)) {
                st = STATUS_BUFFER_TOO_SMALL;
            } else {
                DXGK_PHYSICALADAPTERCAPS *pc = (DXGK_PHYSICALADAPTERCAPS *)Info->pOutputData;
                RtlZeroMemory(pc, Info->OutputDataSize);
                pc->NumExecutionNodes = 1;
                pc->PagingNodeIndex = 0;
                pc->DxgkPhysicalAdapterHandle = a->Dxgk.DeviceHandle;
                st = STATUS_SUCCESS;
            }
            break;
        case DXGKQAITYPE_HISTORYBUFFERPRECISION:
            /* WDDM 1.3 asks this of every 1.3 driver; NOT_SUPPORTED was logged by dxgkrnl as an invalid
             * NTSTATUS right before failures (original's WinPE runs 5-8). */
            if (Info->OutputDataSize < sizeof(DXGKARG_HISTORYBUFFERPRECISION)) {
                st = STATUS_BUFFER_TOO_SMALL;
            } else {
                ((DXGKARG_HISTORYBUFFERPRECISION *)Info->pOutputData)->PrecisionBits = 64;
                st = STATUS_SUCCESS;
            }
            break;
        case 47: /* DXGKQAITYPE_64BITONLYCAPS: zero = not restricted to 64-bit */
            RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
            st = STATUS_SUCCESS;
            break;
        case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
            RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
            st = STATUS_SUCCESS;
            break;
        default:
            st = STATUS_NOT_SUPPORTED;
            break;
        }
    }
    WCHAR name[32];
    UNICODE_STRING us;
    RtlInitEmptyUnicodeString(&us, name, sizeof(name));
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"QAI_%u", (ULONG)Info->Type)))
        MwLog(a, name, (ULONG)st);
    RtlInitEmptyUnicodeString(&us, name, sizeof(name));
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"QAI_%u_Size", (ULONG)Info->Type)))
        MwLog(a, name, Info->OutputDataSize);
    return st;
}

static void Stats(MW_ADAPTER *A, MW_ESCAPE_STATS_DATA *D)
{
    D->GpuUp = A->GpuUp;
    D->Renders = A->Renders;
    D->Presents = A->Presents;
    D->Submits = A->Submits;
    D->LastSubmittedFence = A->LastSubmittedFence;
    D->LastCompletedFence = A->LastCompletedFence;
    D->Queued = A->QCount;
    D->Running = A->Running;
    D->JobsDone = A->JobsDone;
    D->JobsFailed = A->JobsFailed;
    D->JobsTimedOut = A->JobsTimedOut;
    D->LastJsStatus = A->LastJsStatus;
    D->LastFaultStatus = A->LastFaultStatus;
    D->Irqs = A->Gpu.Irqs;
    D->Resets = A->Gpu.Resets;
    D->LastFaultAddress = A->LastFaultAddress;
    for (ULONG i = 0; i < 16; i++)
        D->PagingOps[i] = A->PagingOps[i];
    D->Maps = A->Maps;
    D->MapFails = A->MapFails;
    D->Unmaps = A->Unmaps;
    D->LastMapPages = A->LastMapPages;
    D->LastMapStatus = (mw_u32)A->LastMapStatus;
    D->Allocations = A->Allocations;
    D->LastMapVa = A->LastMapVa;
    D->LastAllocVa = A->LastAllocVa;
    D->Patches = A->Patches;
    D->PatchAllocations = A->PatchAllocations;
    D->PatchPatchLocations = A->PatchPatchLocations;
    D->PatchSegment0 = A->PatchSegment0;
    D->PatchAddress0 = A->PatchAddress0;
    D->PresentBlts = A->PresentBlts;
    D->PresentFills = A->PresentFills;
    D->PresentSkips = A->PresentSkips;
    D->PresentGpu = A->PresentGpu;
}

static NTSTATUS APIENTRY MwEscape(IN_CONST_HANDLE hAdapter, const DXGKARG_ESCAPE *Esc)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    mw_u32 code;

    if (Esc->PrivateDriverDataSize < sizeof(code)) {
        /* The original's run 6: an escape we do not know (DWM's, by the timing) failed
         * here and its device was destroyed next. Record what it was. */
        MwLog(a, L"EscapeSmallSize", Esc->PrivateDriverDataSize);
        MwLog(a, L"EscapeSmallFlags", Esc->Flags.Value);
        return STATUS_INVALID_PARAMETER;
    }
    code = *(const mw_u32 *)Esc->pPrivateDriverData;
    switch (code) {
    case MW_ESCAPE_ALLOC_INFO:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_ALLOC_INFO_DATA))
            return STATUS_INVALID_PARAMETER;
        return MwEscapeAllocInfo(a, Esc, (MW_ESCAPE_ALLOC_INFO_DATA *)Esc->pPrivateDriverData);
    case MW_ESCAPE_GPU_INFO:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_GPU_INFO_DATA))
            return STATUS_INVALID_PARAMETER;
        GpuInfo(a, (MW_ESCAPE_GPU_INFO_DATA *)Esc->pPrivateDriverData);
        return STATUS_SUCCESS;
    case MW_ESCAPE_SCANOUT:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_SCANOUT_DATA))
            return STATUS_INVALID_PARAMETER;
        return MwEscapeScanout(a, Esc, (MW_ESCAPE_SCANOUT_DATA *)Esc->pPrivateDriverData);
    case MW_ESCAPE_CLOCK: {
        MW_ESCAPE_CLOCK_DATA *c = (MW_ESCAPE_CLOCK_DATA *)Esc->pPrivateDriverData;
        LARGE_INTEGER f, q;

        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_CLOCK_DATA))
            return STATUS_INVALID_PARAMETER;
        c->Valid = a->GpuUp && MkReadClock(&a->Gpu, &c->Cycles, &c->Timestamp);
        q = KeQueryPerformanceCounter(&f);
        c->Qpc = (mw_u64)q.QuadPart;
        c->QpcFreq = (mw_u64)f.QuadPart;
        return STATUS_SUCCESS;
    }
    case MW_ESCAPE_PANEL_POWER: {
        MW_ESCAPE_PANEL_POWER_DATA *pp = (MW_ESCAPE_PANEL_POWER_DATA *)Esc->pPrivateDriverData;

        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_PANEL_POWER_DATA))
            return STATUS_INVALID_PARAMETER;
        MwPanelDisplayPower(a, pp->On != 0);
        pp->Done = (a->PanelDisplayOn == (pp->On != 0)) ? 1u : 0u;
        return STATUS_SUCCESS;
    }
    case MW_ESCAPE_STATS:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_STATS_DATA))
            return STATUS_INVALID_PARAMETER;
        Stats(a, (MW_ESCAPE_STATS_DATA *)Esc->pPrivateDriverData);
        return STATUS_SUCCESS;
    default:
        MwLog(a, L"EscapeUnknownCode", code);
        MwLog(a, L"EscapeUnknownSize", Esc->PrivateDriverDataSize);
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS APIENTRY MwSetPointerPosition(IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERPOSITION *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY MwSetPointerShape(IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERSHAPE *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_NOT_SUPPORTED;
}

/* ---- the traced DDI table ---- */

/* The wrappers find the adapter from the DDI's first parameter, whose type
 * differs: an adapter, a device (OpenAllocation, CreateContext, ...), a
 * context (Render, Present, ...) or the miniport context. */
/* Every call is also traced into PRAM (pram.c): the entry before anything that can block, the exit with its status;
 * the first call of each DDI also lands in the first-call lines. */
#define TRACE_WITH(Name, Proto, Args, AdapterExpr)                             \
    static NTSTATUS APIENTRY T_##Name Proto                                    \
    {                                                                          \
        static LONG first_;                                                    \
        BOOLEAN f_ = InterlockedExchange(&first_, 1) == 0;                     \
        MW_ADAPTER *a_ = (AdapterExpr);                                        \
        NTSTATUS st_;                                                          \
        MwPramDdi(f_, FALSE, #Name, 0);                                        \
        MwEnter(a_, L"Enter_" MW_W(#Name));                                    \
        st_ = Mw##Name Args;                                                   \
        MwPramDdi(f_, TRUE, #Name, st_);                                       \
        return MwRet(a_, L"Fail_" MW_W(#Name), st_);                           \
    }
#define TRACE_ADAPTER(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, (MW_ADAPTER *)hAdapter)
#define TRACE_DEVICE(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, ((MW_DEVICE *)hDevice)->Adapter)
#define TRACE_CTX(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, ((MW_CONTEXT *)hContext)->Device->Adapter)

TRACE_ADAPTER(QueryAdapterInfo, (IN_CONST_HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *a), (hAdapter, a))
TRACE_ADAPTER(Escape, (IN_CONST_HANDLE hAdapter, const DXGKARG_ESCAPE *a), (hAdapter, a))
TRACE_ADAPTER(CreateAllocation, (IN_CONST_HANDLE hAdapter, DXGKARG_CREATEALLOCATION *a), (hAdapter, a))
TRACE_ADAPTER(DestroyAllocation, (IN_CONST_HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *a), (hAdapter, a))
TRACE_ADAPTER(DescribeAllocation, (IN_CONST_HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *a), (hAdapter, a))
TRACE_ADAPTER(GetStandardAllocationDriverData, (IN_CONST_HANDLE hAdapter, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *a), (hAdapter, a))
TRACE_ADAPTER(BuildPagingBuffer, (IN_CONST_HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER *a), (hAdapter, a))
TRACE_ADAPTER(CreateDevice, (IN_CONST_HANDLE hAdapter, DXGKARG_CREATEDEVICE *a), (hAdapter, a))
TRACE_ADAPTER(IsSupportedVidPn, (IN_CONST_HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *a), (hAdapter, a))
TRACE_ADAPTER(RecommendFunctionalVidPn, (IN_CONST_HANDLE hAdapter, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *a), (hAdapter, a))
TRACE_ADAPTER(EnumVidPnCofuncModality, (IN_CONST_HANDLE hAdapter, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *a), (hAdapter, a))
TRACE_ADAPTER(SetVidPnSourceVisibility, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEVISIBILITY *a), (hAdapter, a))
TRACE_ADAPTER(CommitVidPn, (IN_CONST_HANDLE hAdapter, const DXGKARG_COMMITVIDPN *a), (hAdapter, a))
TRACE_ADAPTER(UpdateActiveVidPnPresentPath, (IN_CONST_HANDLE hAdapter, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *a), (hAdapter, a))
TRACE_ADAPTER(RecommendMonitorModes, (IN_CONST_HANDLE hAdapter, const DXGKARG_RECOMMENDMONITORMODES *a), (hAdapter, a))
TRACE_ADAPTER(QueryVidPnHWCapability, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYVIDPNHWCAPABILITY *a), (hAdapter, a))
TRACE_ADAPTER(SetVidPnSourceAddress, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *a), (hAdapter, a))
TRACE_ADAPTER(PresentDisplayOnly, (IN_CONST_HANDLE hAdapter, const DXGKARG_PRESENT_DISPLAYONLY *a), (hAdapter, a))
TRACE_ADAPTER(ResetFromTimeout, (IN_CONST_HANDLE hAdapter), (hAdapter))
TRACE_ADAPTER(RestartFromTimeout, (IN_CONST_HANDLE hAdapter), (hAdapter))
TRACE_ADAPTER(ResetEngine, (IN_CONST_HANDLE hAdapter, DXGKARG_RESETENGINE *a), (hAdapter, a))
TRACE_ADAPTER(QueryEngineStatus, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYENGINESTATUS *a), (hAdapter, a))
TRACE_ADAPTER(ControlInterrupt, (IN_CONST_HANDLE hAdapter, IN_CONST_DXGK_INTERRUPT_TYPE t, IN_BOOLEAN e), (hAdapter, t, e))
TRACE_ADAPTER(GetScanLine, (IN_CONST_HANDLE hAdapter, DXGKARG_GETSCANLINE *a), (hAdapter, a))

#define TRACE_CONTEXT(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, (MW_ADAPTER *)Context)

TRACE_CONTEXT(QueryChildRelations, (IN_CONST_PVOID Context, PDXGK_CHILD_DESCRIPTOR d, IN_ULONG n), (Context, d, n))
TRACE_CONTEXT(QueryChildStatus, (IN_CONST_PVOID Context, INOUT_PDXGK_CHILD_STATUS c, IN_BOOLEAN b), (Context, c, b))
TRACE_CONTEXT(QueryDeviceDescriptor, (IN_CONST_PVOID Context, IN_ULONG u, INOUT_PDXGK_DEVICE_DESCRIPTOR d), (Context, u, d))
TRACE_CONTEXT(SetPowerState, (IN_CONST_PVOID Context, IN_ULONG u, IN_DEVICE_POWER_STATE p, IN_POWER_ACTION a), (Context, u, p, a))
TRACE_CONTEXT(QueryInterface, (IN_CONST_PVOID Context, IN_PQUERY_INTERFACE q), (Context, q))
TRACE_CONTEXT(DispatchIoRequest, (IN_CONST_PVOID Context, IN_ULONG s, IN_PVIDEO_REQUEST_PACKET v), (Context, s, v))
TRACE_CONTEXT(StopDevice, (IN_CONST_PVOID Context), (Context))
TRACE_DEVICE(OpenAllocation, (IN_CONST_HANDLE hDevice, const DXGKARG_OPENALLOCATION *a), (hDevice, a))
TRACE_DEVICE(CloseAllocation, (IN_CONST_HANDLE hDevice, const DXGKARG_CLOSEALLOCATION *a), (hDevice, a))
TRACE_DEVICE(CreateContext, (IN_CONST_HANDLE hDevice, DXGKARG_CREATECONTEXT *a), (hDevice, a))
TRACE_DEVICE(DestroyDevice, (IN_CONST_HANDLE hDevice), (hDevice))
TRACE_CTX(DestroyContext, (IN_CONST_HANDLE hContext), (hContext))
TRACE_CTX(Render, (IN_CONST_HANDLE hContext, DXGKARG_RENDER *a), (hContext, a))
TRACE_CTX(Present, (IN_CONST_HANDLE hContext, DXGKARG_PRESENT *a), (hContext, a))
TRACE_ADAPTER(SetPointerPosition, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERPOSITION *a), (hAdapter, a))
TRACE_ADAPTER(SetPointerShape, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERSHAPE *a), (hAdapter, a))
TRACE_ADAPTER(Patch, (IN_CONST_HANDLE hAdapter, const DXGKARG_PATCH *a), (hAdapter, a))
TRACE_ADAPTER(SubmitCommand, (IN_CONST_HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *a), (hAdapter, a))
TRACE_ADAPTER(QueryCurrentFence, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYCURRENTFENCE *a), (hAdapter, a))
TRACE_ADAPTER(CollectDbgInfo, (IN_CONST_HANDLE hAdapter, const DXGKARG_COLLECTDBGINFO *a), (hAdapter, a))
TRACE_ADAPTER(CancelCommand, (IN_CONST_HANDLE hAdapter, const DXGKARG_CANCELCOMMAND *a), (hAdapter, a))
TRACE_ADAPTER(PreemptCommand, (IN_CONST_HANDLE hAdapter, const DXGKARG_PREEMPTCOMMAND *a), (hAdapter, a))
TRACE_CTX(RenderKm, (IN_CONST_HANDLE hContext, DXGKARG_RENDER *a), (hContext, a))
TRACE_ADAPTER(GetNodeMetadata, (IN_CONST_HANDLE hAdapter, UINT n, DXGKARG_GETNODEMETADATA *a), (hAdapter, n, a))
TRACE_ADAPTER(QueryDependentEngineGroup, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYDEPENDENTENGINEGROUP *a), (hAdapter, a))
TRACE_CONTEXT(StopDeviceAndReleasePostDisplayOwnership,
              (IN_CONST_PVOID Context, IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID t, PDXGK_DISPLAY_INFORMATION d),
              (Context, t, d))

/* The bugcheck screen: PRAM only (stores), the registry trace is not callable there. */
static NTSTATUS APIENTRY T_SystemDisplayEnable(IN_CONST_PVOID Context, IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID t,
                                               PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS f, OUT UINT *w, OUT UINT *h,
                                               OUT D3DDDIFORMAT *c)
{
    NTSTATUS st;
    MwPramDdi(TRUE, FALSE, "SystemDisplayEnable", 0);
    st = MwSystemDisplayEnable(Context, t, f, w, h, c);
    MwPramDdi(TRUE, TRUE, "SystemDisplayEnable", st);
    return st;
}

/* G76: Services\MaliG76Wddm\Parameters\<Name> as a DWORD; Default when the key or the value is absent or not a
 * DWORD. */
static ULONG ReadParameter(HANDLE Params, PCWSTR Name, ULONG Default)
{
    UNICODE_STRING name;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len = 0;

    if (Params == NULL)
        return Default;
    RtlInitUnicodeString(&name, Name);
    if (!NT_SUCCESS(ZwQueryValueKey(Params, &name, KeyValuePartialInformation, v, sizeof(buf), &len)) ||
        v->Type != REG_DWORD || v->DataLength != sizeof(ULONG))
        return Default;
    return *(const ULONG *)v->Data;
}

/* Why the mode is what it is, for the PRAM trace: what was read (MwPramText "Params" line in DriverEntry). */
static ULONG ParamGpu, ParamFull, ParamKeys;

/* The mode, and the switches that are not modes (Parameters\PramTrace, default 1). */
static MW_MODE ReadMode(PUNICODE_STRING RegistryPath, ULONG *PramTrace)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING sub = RTL_CONSTANT_STRING(L"Parameters");
    HANDLE svc = NULL, params = NULL;
    ULONG gpu, full;

    InitializeObjectAttributes(&oa, RegistryPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&svc, KEY_READ, &oa))) {
        InitializeObjectAttributes(&oa, &sub, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, svc, NULL);
        if (!NT_SUCCESS(ZwOpenKey(&params, KEY_READ, &oa)))
            params = NULL;
    }
    gpu = ReadParameter(params, L"GpuEnable", 0);
    full = ReadParameter(params, L"FullAdapter", 0);
    ParamGpu = gpu;
    ParamFull = full;
    ParamKeys = (svc != NULL ? 1u : 0u) | (params != NULL ? 2u : 0u);
    *PramTrace = ReadParameter(params, L"PramTrace", 1);
    MwSoftVsync = ReadParameter(params, L"SoftVsync", 1) != 0;
    MwFlipBlt = ReadParameter(params, L"FlipBlt", 1) != 0;
    MwRecommendVidPn = ReadParameter(params, L"RecommendVidPn", 1) != 0;
    MwSynthEdid = ReadParameter(params, L"SynthEdid", 1) != 0;
    MwSegmentCpuVisible = ReadParameter(params, L"SegmentCpuVisible", 1) != 0;
    MwModeRefreshExact = ReadParameter(params, L"ModeRefreshExact", 1) != 0;
    MwRefresh601 = ReadParameter(params, L"Refresh601", 1) != 0;
    MwFlipMmio = ReadParameter(params, L"FlipMmio", 1) != 0;
    MwScanoutStride = ReadParameter(params, L"ScanoutStride", 0);
    MwOfferRotation = ReadParameter(params, L"OfferRotation", 0);
    MwPanelCmdMode = ReadParameter(params, L"PanelCmdMode", 1);
    MwPanelBrightness = ReadParameter(params, L"PanelBrightness", 1);
    MwCddDwmInterop = ReadParameter(params, L"CddDwmInterop", 1) != 0;
    MwWddm2 = ReadParameter(params, L"Wddm2", 0) == 1;
    if (params != NULL)
        ZwClose(params);
    if (svc != NULL)
        ZwClose(svc);
    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "MaliG76Wddm: GpuEnable %lu FullAdapter %lu\n", gpu, full);
    if (gpu == 1)
        return MwModeFullGpu;
    if (full == 1)
        return MwModeFullNoGpu;
    return MwModeDisplayOnly;     /* default, and any value other than exactly 1 */
}

/* G76: the display-only driver (malidod.c:836-873 DriverEntry): the same DDIs, no interrupt, DPC, VSync or
 * rendering DDIs. KMDOD arrived with Windows 8, so the WIN8 interface version, as malidod. */
static NTSTATUS InitDisplayOnly(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    KMDDOD_INITIALIZATION_DATA init = {0};

    init.Version = DXGKDDI_INTERFACE_VERSION_WIN8;
    init.DxgkDdiAddDevice = MwAddDevice;
    init.DxgkDdiStartDevice = MwStartDevice;
    init.DxgkDdiStopDevice = T_StopDevice;
    init.DxgkDdiRemoveDevice = MwRemoveDevice;
    init.DxgkDdiDispatchIoRequest = T_DispatchIoRequest;
    init.DxgkDdiQueryChildRelations = T_QueryChildRelations;
    init.DxgkDdiQueryChildStatus = T_QueryChildStatus;
    init.DxgkDdiQueryDeviceDescriptor = T_QueryDeviceDescriptor;
    init.DxgkDdiSetPowerState = T_SetPowerState;
    init.DxgkDdiResetDevice = MwResetDevice;
    init.DxgkDdiUnload = MwUnload;
    init.DxgkDdiQueryInterface = T_QueryInterface;
    init.DxgkDdiQueryAdapterInfo = T_QueryAdapterInfo;
    init.DxgkDdiSetPointerPosition = T_SetPointerPosition;
    init.DxgkDdiSetPointerShape = T_SetPointerShape;
    init.DxgkDdiIsSupportedVidPn = T_IsSupportedVidPn;
    init.DxgkDdiRecommendFunctionalVidPn = T_RecommendFunctionalVidPn;
    init.DxgkDdiEnumVidPnCofuncModality = T_EnumVidPnCofuncModality;
    init.DxgkDdiSetVidPnSourceVisibility = T_SetVidPnSourceVisibility;
    init.DxgkDdiCommitVidPn = T_CommitVidPn;
    init.DxgkDdiUpdateActiveVidPnPresentPath = T_UpdateActiveVidPnPresentPath;
    init.DxgkDdiRecommendMonitorModes = T_RecommendMonitorModes;
    init.DxgkDdiQueryVidPnHWCapability = T_QueryVidPnHWCapability;
    init.DxgkDdiPresentDisplayOnly = T_PresentDisplayOnly;
    init.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = T_StopDeviceAndReleasePostDisplayOwnership;
    init.DxgkDdiSystemDisplayEnable = T_SystemDisplayEnable;
    init.DxgkDdiSystemDisplayWrite = MwSystemDisplayWrite;
    return DxgkInitializeDisplayOnlyDriver(DriverObject, RegistryPath, &init);
}

static NTSTATUS InitFull(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA init = {0};

    init.Version = MwWddm2 ? DXGKDDI_INTERFACE_VERSION_WDDM2_0 : DXGKDDI_INTERFACE_VERSION_WDDM1_3;
    init.DxgkDdiAddDevice = MwAddDevice;
    init.DxgkDdiStartDevice = MwStartDevice;
    init.DxgkDdiStopDevice = T_StopDevice;
    init.DxgkDdiRemoveDevice = MwRemoveDevice;
    init.DxgkDdiDispatchIoRequest = T_DispatchIoRequest;
    init.DxgkDdiInterruptRoutine = MwInterruptRoutine;
    init.DxgkDdiDpcRoutine = MwDpcRoutine;
    init.DxgkDdiQueryChildRelations = T_QueryChildRelations;
    init.DxgkDdiQueryChildStatus = T_QueryChildStatus;
    init.DxgkDdiQueryDeviceDescriptor = T_QueryDeviceDescriptor;
    init.DxgkDdiSetPowerState = T_SetPowerState;
    init.DxgkDdiResetDevice = MwResetDevice;
    init.DxgkDdiUnload = MwUnload;
    init.DxgkDdiQueryInterface = T_QueryInterface;

    init.DxgkDdiQueryAdapterInfo = T_QueryAdapterInfo;
    init.DxgkDdiEscape = T_Escape;
    init.DxgkDdiCreateAllocation = T_CreateAllocation;
    init.DxgkDdiDestroyAllocation = T_DestroyAllocation;
    init.DxgkDdiDescribeAllocation = T_DescribeAllocation;
    init.DxgkDdiGetStandardAllocationDriverData = T_GetStandardAllocationDriverData;
    init.DxgkDdiOpenAllocation = T_OpenAllocation;
    init.DxgkDdiCloseAllocation = T_CloseAllocation;
    init.DxgkDdiBuildPagingBuffer = T_BuildPagingBuffer;

    init.DxgkDdiCreateDevice = T_CreateDevice;
    init.DxgkDdiDestroyDevice = T_DestroyDevice;
    init.DxgkDdiCreateContext = T_CreateContext;
    init.DxgkDdiDestroyContext = T_DestroyContext;
    init.DxgkDdiRender = T_Render;
    init.DxgkDdiRenderKm = T_RenderKm;
    init.DxgkDdiPresent = T_Present;
    init.DxgkDdiPatch = T_Patch;
    init.DxgkDdiSubmitCommand = T_SubmitCommand;
    init.DxgkDdiPreemptCommand = T_PreemptCommand;
    init.DxgkDdiQueryCurrentFence = T_QueryCurrentFence;
    init.DxgkDdiResetFromTimeout = T_ResetFromTimeout;
    init.DxgkDdiRestartFromTimeout = T_RestartFromTimeout;
    init.DxgkDdiCollectDbgInfo = T_CollectDbgInfo;
    init.DxgkDdiGetNodeMetadata = T_GetNodeMetadata;
    init.DxgkDdiResetEngine = T_ResetEngine;
    init.DxgkDdiQueryEngineStatus = T_QueryEngineStatus;
    init.DxgkDdiQueryDependentEngineGroup = T_QueryDependentEngineGroup;
    init.DxgkDdiCancelCommand = T_CancelCommand;
    init.DxgkDdiControlInterrupt = T_ControlInterrupt;
    init.DxgkDdiGetScanLine = T_GetScanLine;

    init.DxgkDdiSetPointerPosition = T_SetPointerPosition;
    init.DxgkDdiSetPointerShape = T_SetPointerShape;
    init.DxgkDdiIsSupportedVidPn = T_IsSupportedVidPn;
    init.DxgkDdiRecommendFunctionalVidPn = T_RecommendFunctionalVidPn;
    init.DxgkDdiEnumVidPnCofuncModality = T_EnumVidPnCofuncModality;
    init.DxgkDdiSetVidPnSourceVisibility = T_SetVidPnSourceVisibility;
    init.DxgkDdiCommitVidPn = T_CommitVidPn;
    init.DxgkDdiUpdateActiveVidPnPresentPath = T_UpdateActiveVidPnPresentPath;
    init.DxgkDdiSetVidPnSourceAddress = T_SetVidPnSourceAddress;
    init.DxgkDdiRecommendMonitorModes = T_RecommendMonitorModes;
    init.DxgkDdiQueryVidPnHWCapability = T_QueryVidPnHWCapability;
    init.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = T_StopDeviceAndReleasePostDisplayOwnership;
    init.DxgkDdiSystemDisplayEnable = T_SystemDisplayEnable;
    init.DxgkDdiSystemDisplayWrite = MwSystemDisplayWrite;
    return DxgkInitialize(DriverObject, RegistryPath, &init);
}

DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ULONG pram = 1;
    NTSTATUS st;

    MwMode = ReadMode(RegistryPath, &pram);
    MwPramInit(pram, (ULONG)MwMode, MW_BUILD);
    {
        /* The mode's reason: GpuEnable (bits 31..24, low byte of the value), FullAdapter (23..16), keys opened
         * (15..8: 1 service key, 2 Parameters), SoftVsync (7..4), FlipBlt (3..0). The mode is 2 if GpuEnable is
         * exactly 1, else 1 if FullAdapter is exactly 1, else 0 - read once here, nothing changes it later. */
        LARGE_INTEGER now;
        MwPramText(TRUE, "Params g.f.keys.vs.fb ", NULL,
                   ((ParamGpu & 0xFF) << 24) | ((ParamFull & 0xFF) << 16) | (ParamKeys << 8) |
                   ((MwSoftVsync & 0xF) << 4) | (MwFlipBlt & 0xF), TRUE);
        MwPramText(TRUE, "Switches ", NULL,
                   (MwWddm2 << 24) | (MwFlipMmio << 20) | (MwRefresh601 << 16) | (MwModeRefreshExact << 12) | (MwSegmentCpuVisible << 8) | (MwRecommendVidPn << 4) | MwSynthEdid, TRUE);
        /* Wall clock (Unix seconds), so a saved \WINPRAM.BIN can be matched to the boot that wrote it. */
        KeQuerySystemTimePrecise(&now);
        MwPramText(TRUE, "UnixTime ", NULL, (ULONG)(now.QuadPart / 10000000LL - 11644473600LL), TRUE);
    }
    st = MwMode == MwModeDisplayOnly ? InitDisplayOnly(DriverObject, RegistryPath)
                                     : InitFull(DriverObject, RegistryPath);
    MwPramText(TRUE, "DxgkInitialize ", NULL, (ULONG)st, TRUE);
    if (!NT_SUCCESS(st))
        MwPramFree();       /* no DxgkDdiUnload follows a failed DriverEntry */
    return st;
}
