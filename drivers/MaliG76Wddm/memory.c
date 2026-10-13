// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm's memory model (docs/M5.2-PLAN.md): one aperture segment, as
 * viogpu3d. VidMm owns residency; the driver owns the Mali's address space.
 * An allocation gets its GPU address at creation and keeps it; when VidMm
 * makes it resident (MAP_APERTURE_SEGMENT) its pages are mapped there, and
 * unmapped again on eviction. The aperture offsets VidMm assigns are not
 * used for anything: the Mali MMU is the aperture.
 *
 * MaliG76Wddm (Galaxy S10e, Mali-G76): derived from wddm-mali-g52 drivers/maliwddm/memory.c (commit 16f7e7c6,
 * GPL-2.0, Copyright gahingwoo). Changes are marked "G76:" (notes/gpu-m16-g10.md).
 */
#include "MaliG76Wddm.h"

/* ---- the segment ---- */

NTSTATUS MwQuerySegment(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_QUERYSEGMENTOUT3 *out = (DXGK_QUERYSEGMENTOUT3 *)Info->pOutputData;
    UNREFERENCED_PARAMETER(A);

    if (Info->OutputDataSize < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;
    if (out->pSegmentDescriptor == NULL) {
        out->NbSegment = 1;                 /* the first call asks how many */
        return STATUS_SUCCESS;
    }
    DXGK_SEGMENTDESCRIPTOR3 *d = &out->pSegmentDescriptor[0];
    RtlZeroMemory(d, sizeof(*d));
    d->BaseAddress.QuadPart = MK_VA_START;  /* nominal: nothing uses aperture offsets */
    d->Size = MW_SEGMENT_SIZE;
    d->CommitLimit = MW_SEGMENT_SIZE;
    d->Flags.Aperture = TRUE;
    /* The Mali is not coherent with the CPU caches, so VidMm maps CPU views
     * of these allocations uncached, as malikm's write-combined BOs. */
    d->Flags.CacheCoherent = FALSE;
    /* G76: dxgkrnl refuses a shared primary unless every segment in its SupportedWriteSegmentSet is CPU visible -
     * DxgKrnl ETW event 494 "All segment specified in supported write segment set for a shared primary must be CPU
     * visible." (run g18etw), after which it destroyed the primary and committed a VidPN with 0 paths. Our presents
     * are CPU copies anyway (AllocationVa), so the segment is CPU visible; Parameters\SegmentCpuVisible 0 = the
     * original's flags. */
    d->Flags.CpuVisible = MwSegmentCpuVisible ? TRUE : FALSE;
    /* As viogpu3d. Paging buffers stay empty: map and unmap are done on
     * the CPU while the paging buffer is built. */
    out->PagingBufferSegmentId = MW_SEGMENT_ID;
    out->PagingBufferSize = 10 * PAGE_SIZE;
    out->PagingBufferPrivateDataSize = 0;
    return STATUS_SUCCESS;
}

/* ---- allocations ---- */

static NTSTATUS NewAllocation(MW_ADAPTER *A, const MW_ALLOCATION_INFO *In, DXGK_ALLOCATIONINFO *Out)
{
    MW_ALLOCATION *alloc;
    ULONG pages;

    if (In->Size == 0 || In->Size > MW_SEGMENT_SIZE)
        return STATUS_INVALID_PARAMETER;
    pages = (ULONG)((In->Size + PAGE_SIZE - 1) >> PAGE_SHIFT);
    alloc = (MW_ALLOCATION *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*alloc), MW_TAG);
    if (alloc == NULL)
        return STATUS_NO_MEMORY;
    alloc->Pages = pages;
    alloc->Size = (SIZE_T)pages << PAGE_SHIFT;
    alloc->Flags = In->Flags;
    alloc->Width = In->Width;
    alloc->Height = In->Height;
    alloc->Pitch = In->Pitch;
    alloc->Format = (D3DDDIFORMAT)In->Format;

    /* The GPU address, for the allocation's whole life. */
    ExAcquireFastMutex(&A->MmuLock);
    BOOLEAN ok = A->GpuUp && MkVaAlloc(&A->Gpu, pages, !(In->Flags & MW_ALLOC_NOEXEC), &alloc->GpuVa);
    ExReleaseFastMutex(&A->MmuLock);
    if (!ok && A->GpuUp) {
        ExFreePoolWithTag(alloc, MW_TAG);
        return STATUS_GRAPHICS_NO_VIDEO_MEMORY;
    }

    A->Allocations++;
    A->LastAllocVa = alloc->GpuVa;
    Out->hAllocation = alloc;
    Out->Size = alloc->Size;
    Out->Alignment = 0;
    Out->PitchAlignedSize = 0;
    Out->HintedBank.Value = 0;
    Out->PreferredSegment.Value = 0;
    Out->PreferredSegment.SegmentId0 = MW_SEGMENT_ID;
    Out->SupportedReadSegmentSet = 1u << (MW_SEGMENT_ID - 1);
    Out->SupportedWriteSegmentSet = 1u << (MW_SEGMENT_ID - 1);
    Out->EvictionSegmentSet = 0;
    Out->MaximumRenamingListLength = 0;
    Out->pAllocationUsageHint = NULL;
    Out->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    Out->Flags.Value = 0;
    Out->Flags.CpuVisible = TRUE;
    return STATUS_SUCCESS;
}

static void FreeAllocation(MW_ADAPTER *A, MW_ALLOCATION *Alloc)
{
    ExAcquireFastMutex(&A->MmuLock);
    if (Alloc->Mapped && A->GpuUp)
        MkMmuUnmapPages(&A->Gpu, Alloc->GpuVa, Alloc->Pages);
    if (Alloc->GpuVa != 0 && A->GpuUp)
        MkVaFree(&A->Gpu, Alloc->GpuVa, Alloc->Pages);
    ExReleaseFastMutex(&A->MmuLock);
    ExFreePoolWithTag(Alloc, MW_TAG);
}

NTSTATUS APIENTRY MwCreateAllocation(IN_CONST_HANDLE hAdapter, DXGKARG_CREATEALLOCATION *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    for (UINT i = 0; i < Arg->NumAllocations; i++) {
        DXGK_ALLOCATIONINFO *info = &Arg->pAllocationInfo[i];
        const MW_ALLOCATION_INFO *in = (const MW_ALLOCATION_INFO *)info->pPrivateDriverData;
        NTSTATUS st;

        if (in == NULL || info->PrivateDriverDataSize < sizeof(*in) || in->Version != MW_ABI_VERSION)
            st = STATUS_INVALID_PARAMETER;
        else
            st = NewAllocation(a, in, info);
        if (!NT_SUCCESS(st)) {
            for (UINT j = 0; j < i; j++)
                FreeAllocation(a, (MW_ALLOCATION *)Arg->pAllocationInfo[j].hAllocation);
            return st;
        }
    }
    /* A resource needs a handle of its own; nothing is kept in it. */
    if (Arg->Flags.Resource && Arg->hResource == NULL)
        Arg->hResource = (HANDLE)a;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwDestroyAllocation(IN_CONST_HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    for (UINT i = 0; i < Arg->NumAllocations; i++)
        if (Arg->pAllocationList[i] != NULL)
            FreeAllocation(a, (MW_ALLOCATION *)Arg->pAllocationList[i]);
    return STATUS_SUCCESS;
}

/* G76: dxgkrnl describes a flip's primary to check it against the current mode; the original's fixed 60/1 refresh
 * is not the target mode's VSyncFreq (PixelRate / (totalW * totalH) as a rational, display.c FillSignal), and every
 * flip of DWM's swap chain came back STATUS_GRAPHICS_PRESENT_MODE_CHANGED (0xC01E0005, DxgKrnl ETW event 184,
 * run g22full) with a DescribeAllocation pair before each retry. A primary now reports the mode's own refresh
 * rational; the first description of each kind goes to PRAM ("Describe wxh", "Describe fmt.flags"). */
NTSTATUS APIENTRY MwDescribeAllocation(IN_CONST_HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    MW_ALLOCATION *alloc = (MW_ALLOCATION *)Arg->hAllocation;
    static LONG logged;

    Arg->Width = alloc->Width;
    Arg->Height = alloc->Height;
    Arg->Format = alloc->Format;
    Arg->MultisampleMethod.NumSamples = 0;
    Arg->MultisampleMethod.NumQualityLevels = 0;
    Arg->RefreshRate.Numerator = 60;
    Arg->RefreshRate.Denominator = 1;
    if ((alloc->Flags & MW_ALLOC_PRIMARY) && MwModeRefreshExact)
        MwModeRefresh(a, &Arg->RefreshRate);
    Arg->PrivateDriverFormatAttribute = 0;
    if (InterlockedIncrement(&logged) <= 8) {
        MwPramText(FALSE, "Describe wxh ", NULL, (alloc->Width << 16) | (alloc->Height & 0xFFFF), TRUE);
        MwPramText(FALSE, "Describe fmt.flags ", NULL, ((ULONG)alloc->Format << 8) | (alloc->Flags & 0xFF), TRUE);
    }
    return STATUS_SUCCESS;
}

/* The runtime's own surfaces: describe them in our private data format.
 * Called twice: first with no buffers, for the sizes. */
NTSTATUS APIENTRY MwGetStandardAllocationDriverData(IN_CONST_HANDLE hAdapter,
                                                    DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *Arg)
{
    MW_ALLOCATION_INFO *out = (MW_ALLOCATION_INFO *)Arg->pAllocationPrivateDriverData;
    UINT w = 0, h = 0;
    D3DDDIFORMAT f = D3DDDIFMT_A8R8G8B8;
    UINT *pitch = NULL;
    ULONG flags = MW_ALLOC_NOEXEC;
    UNREFERENCED_PARAMETER(hAdapter);

    switch (Arg->StandardAllocationType) {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
        w = Arg->pCreateSharedPrimarySurfaceData->Width;
        h = Arg->pCreateSharedPrimarySurfaceData->Height;
        f = Arg->pCreateSharedPrimarySurfaceData->Format;
        flags |= MW_ALLOC_PRIMARY;
        break;
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
        w = Arg->pCreateShadowSurfaceData->Width;
        h = Arg->pCreateShadowSurfaceData->Height;
        f = Arg->pCreateShadowSurfaceData->Format;
        pitch = &Arg->pCreateShadowSurfaceData->Pitch;
        break;
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
        w = Arg->pCreateStagingSurfaceData->Width;
        h = Arg->pCreateStagingSurfaceData->Height;
        pitch = &Arg->pCreateStagingSurfaceData->Pitch;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    if (out == NULL) {
        Arg->AllocationPrivateDriverDataSize = sizeof(MW_ALLOCATION_INFO);
        Arg->ResourcePrivateDriverDataSize = 0;
        return STATUS_SUCCESS;
    }
    if (Arg->AllocationPrivateDriverDataSize < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(out, sizeof(*out));
    out->Version = MW_ABI_VERSION;
    out->Flags = flags;
    out->Width = w;
    out->Height = h;
    out->Pitch = w * 4;
    out->Format = (mw_u32)f;
    out->Size = (mw_u64)out->Pitch * h;
    if (pitch != NULL)
        *pitch = out->Pitch;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwOpenAllocation(IN_CONST_HANDLE hDevice, const DXGKARG_OPENALLOCATION *Arg)
{
    MW_DEVICE *dev = (MW_DEVICE *)hDevice;
    DXGKRNL_INTERFACE *dx = &dev->Adapter->Dxgk;

    for (UINT i = 0; i < Arg->NumAllocations; i++) {
        DXGKARGCB_GETHANDLEDATA get = {0};
        get.hObject = Arg->pOpenAllocation[i].hAllocation;
        get.Type = DXGK_HANDLE_ALLOCATION;
        Arg->pOpenAllocation[i].hDeviceSpecificAllocation = dx->DxgkCbGetHandleData(&get);
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCloseAllocation(IN_CONST_HANDLE hDevice, const DXGKARG_CLOSEALLOCATION *Arg)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

/* ---- residency ---- */

/* Map and unmap happen here, on the CPU, as viogpu3d does: the paging
 * buffer itself stays empty. Everything else VidMm may ask (transfers,
 * fills) is not supported, as in viogpu3d; with one aperture segment it
 * has not been needed there. */
NTSTATUS APIENTRY MwBuildPagingBuffer(IN_CONST_HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    NTSTATUS st = STATUS_SUCCESS;

    if ((ULONG)Arg->Operation < 16)
        a->PagingOps[Arg->Operation]++;
    switch (Arg->Operation) {
    case DXGK_OPERATION_MAP_APERTURE_SEGMENT: {
        MW_ALLOCATION *alloc = (MW_ALLOCATION *)Arg->MapApertureSegment.hAllocation;
        if (alloc == NULL)
            return STATUS_SUCCESS;
        if (!a->GpuUp) {
            /* G76: no GPU mapping, but the pages are still how a present reaches the pixels (command.c
             * AllocationVa); the original returned before recording them, so with the GPU half off every present
             * was skipped. */
            ExAcquireFastMutex(&a->MmuLock);
            alloc->Mdl = Arg->MapApertureSegment.pMdl;
            alloc->MdlPage = (ULONG)Arg->MapApertureSegment.MdlOffset;
            ExReleaseFastMutex(&a->MmuLock);
            a->Maps++;
            return STATUS_SUCCESS;
        }
        ULONG pages = (ULONG)Arg->MapApertureSegment.NumberOfPages;
        if (pages > alloc->Pages)
            pages = alloc->Pages;
        PPFN_NUMBER pfn = MmGetMdlPfnArray(Arg->MapApertureSegment.pMdl) + Arg->MapApertureSegment.MdlOffset;
        ExAcquireFastMutex(&a->MmuLock);
        st = MkMmuMapPages(&a->Gpu, alloc->GpuVa, pfn, pages, (alloc->Flags & MW_ALLOC_NOEXEC) != 0);
        alloc->Mapped = NT_SUCCESS(st);
        alloc->Mdl = Arg->MapApertureSegment.pMdl;
        alloc->MdlPage = (ULONG)Arg->MapApertureSegment.MdlOffset;
        ExReleaseFastMutex(&a->MmuLock);
        a->Maps++;
        if (!NT_SUCCESS(st))
            a->MapFails++;
        a->LastMapVa = alloc->GpuVa;
        a->LastMapPages = pages;
        a->LastMapStatus = st;
        return st;
    }
    case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT: {
        MW_ALLOCATION *alloc = (MW_ALLOCATION *)Arg->UnmapApertureSegment.hAllocation;
        if (alloc == NULL)
            return STATUS_SUCCESS;
        if (!a->GpuUp) {
            ExAcquireFastMutex(&a->MmuLock);
            alloc->Mdl = NULL;
            ExReleaseFastMutex(&a->MmuLock);
            a->Unmaps++;
            return STATUS_SUCCESS;
        }
        a->Unmaps++;
        ExAcquireFastMutex(&a->MmuLock);
        if (alloc->Mapped)
            MkMmuUnmapPages(&a->Gpu, alloc->GpuVa, alloc->Pages);
        alloc->Mapped = FALSE;
        alloc->Mdl = NULL;
        ExReleaseFastMutex(&a->MmuLock);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* ---- the UMD's question: where is this allocation on the GPU? ---- */

NTSTATUS MwEscapeAllocInfo(MW_ADAPTER *A, const DXGKARG_ESCAPE *Esc, MW_ESCAPE_ALLOC_INFO_DATA *D)
{
    DXGKARGCB_GETHANDLEDATA get = {0};
    MW_ALLOCATION *alloc;

    if (Esc->hDevice == NULL)
        return STATUS_INVALID_PARAMETER;
    get.hObject = (D3DKMT_HANDLE)D->hAllocation;
    get.Type = DXGK_HANDLE_ALLOCATION;
    alloc = (MW_ALLOCATION *)A->Dxgk.DxgkCbGetHandleData(&get);
    if (alloc == NULL)
        return STATUS_INVALID_HANDLE;
    D->GpuVa = alloc->GpuVa;
    D->Size = alloc->Size;
    return STATUS_SUCCESS;
}
