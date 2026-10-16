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
    d->BaseAddress.QuadPart = MK_VA_START;  /* nominal; MwScanoutLookup matches flip PrimaryAddress against it + offset */
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
    d->Flags.DirectFlip = MwDirectFlip ? TRUE : FALSE;   /* G15 S4, see adapter.c MwDirectFlip */
    /* As viogpu3d. Paging buffers stay empty: map and unmap are done on
     * the CPU while the paging buffer is built. */
    out->PagingBufferSegmentId = MW_SEGMENT_ID;
    out->PagingBufferSize = 10 * PAGE_SIZE;
    out->PagingBufferPrivateDataSize = 0;
    return STATUS_SUCCESS;
}

/* G15 S1: WDDM 2.0 asks QUERYSEGMENT4 - the same single aperture segment as MwQuerySegment, described through
 * DXGK_SEGMENTDESCRIPTOR4 elements of SegmentDescriptorStride bytes (d3dkmddi.h). */
NTSTATUS MwQuerySegment4(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_QUERYSEGMENTOUT4 *out = (DXGK_QUERYSEGMENTOUT4 *)Info->pOutputData;
    UNREFERENCED_PARAMETER(A);

    if (Info->OutputDataSize < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;
    if (out->pSegmentDescriptor == NULL) {
        out->NbSegment = 1;
        out->SegmentDescriptorStride = sizeof(DXGK_SEGMENTDESCRIPTOR4);
        return STATUS_SUCCESS;
    }
    if (out->SegmentDescriptorStride != 0 && out->SegmentDescriptorStride < sizeof(DXGK_SEGMENTDESCRIPTOR4))
        return STATUS_INVALID_PARAMETER;
    DXGK_SEGMENTDESCRIPTOR4 *d = (DXGK_SEGMENTDESCRIPTOR4 *)out->pSegmentDescriptor;
    RtlZeroMemory(d, sizeof(*d));
    d->BaseAddress.QuadPart = MK_VA_START;
    d->Size = MW_SEGMENT_SIZE;
    d->CommitLimit = MW_SEGMENT_SIZE;
    d->Flags.Aperture = TRUE;
    d->Flags.CacheCoherent = FALSE;
    d->Flags.CpuVisible = (MwSegmentCpuVisible && !(MwWddm2Quirk & 4)) ? TRUE : FALSE;
    d->Flags.DirectFlip = MwDirectFlip ? TRUE : FALSE;
    out->PagingBufferSegmentId = MW_SEGMENT_ID;
    out->PagingBufferSize = 10 * PAGE_SIZE;
    out->PagingBufferPrivateDataSize = 0;
    out->SegmentDescriptorStride = sizeof(DXGK_SEGMENTDESCRIPTOR4);
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
    /* S7a: on the GpuMmu adapter the Mali node stays physical and names allocations through the allocation list;
     * Learn gpu-segments "Accessing allocations by physical address": only AccessedPhysically allocations may be. */
    if (MwWddm2Gpummu)
        Out->FlagsWddm2.AccessedPhysically = 1;
    return STATUS_SUCCESS;
}

static void FreeAllocation(MW_ADAPTER *A, MW_ALLOCATION *Alloc)
{
    MwScanoutForget(A, Alloc);      /* G76: never latch a freed primary (FlipMmio) */
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
        MwPramText(TRUE, "Describe wxh ", NULL, (alloc->Width << 16) | (alloc->Height & 0xFFFF), TRUE);
        MwPramText(TRUE, "Describe fmt.flags ", NULL, ((ULONG)alloc->Format << 8) | (alloc->Flags & 0xFF), TRUE);
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
    default: {
        /* g39 follow-up: which other standard types (GDISURFACE = DWM redirection) does the OS ask for? Each
         * type once, to PRAM, with the GDI surface type when there is one. */
        ULONG bit = 1u << ((ULONG)Arg->StandardAllocationType & 31);
        if ((InterlockedOr(&((MW_ADAPTER *)hAdapter)->StdRefused, (LONG)bit) & (LONG)bit) == 0)
            MwPramText(TRUE, "StdAlloc unsupported type.gdi ", NULL,
                       ((ULONG)Arg->StandardAllocationType << 16) |
                       (Arg->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_GDISURFACE &&
                        Arg->pCreateGdiSurfaceData != NULL ? ((ULONG)Arg->pCreateGdiSurfaceData->Type & 0xFFFF) : 0),
                       TRUE);
        return STATUS_NOT_SUPPORTED;
    }
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
    {
        static LONG stdLogged;     /* the first 4 standard allocations, into the first-call PRAM lines */
        if (InterlockedIncrement(&stdLogged) <= 4) {
            MwPramText(TRUE, "StdAlloc type.fmt ", NULL,
                       ((ULONG)Arg->StandardAllocationType << 24) | ((ULONG)f & 0xFFFFFF), TRUE);
            MwPramText(TRUE, "StdAlloc wxh ", NULL, (w << 16) | (h & 0xFFFF), TRUE);
        }
    }
    return STATUS_SUCCESS;
}

/* Allocation handle -> our MW_ALLOCATION. G15 S1 run 10: on the WDDM 2.0 adapter DxgkCbGetHandleData returned NULL
 * for every allocation (OpenAllocation and the UMD's escapes failed 0xC0000008). DxgkCbAcquireHandleData is the
 * WDDM 2.0 callback for the same lookup (MS Learn DXGKCB_ACQUIREHANDLEDATA, DXGKRNL_INTERFACE WDDM2_0 block;
 * qemus/virtio-drivers-windows adapter.rs allocation_from_handle falls back to it the same way). The reference is
 * dropped at once: the caller only uses the pointer while dxgkrnl holds the allocation (open, or named by the
 * escaping device). Only filled in for a 2.x interface, hence the guard. */
MW_ALLOCATION *MwAllocationFromHandle(MW_ADAPTER *A, D3DKMT_HANDLE h)
{
    DXGKRNL_INTERFACE *dx = &A->Dxgk;
    DXGKARGCB_GETHANDLEDATA get = {0};
    PVOID p;

    get.hObject = h;
    get.Type = DXGK_HANDLE_ALLOCATION;
    p = dx->DxgkCbGetHandleData(&get);
    if (p == NULL && MwWddm2 && dx->DxgkCbAcquireHandleData != NULL && dx->DxgkCbReleaseHandleData != NULL) {
        DXGKARG_RELEASE_HANDLE rel = NULL;
        p = dx->DxgkCbAcquireHandleData(&get, &rel);
        if (rel != NULL) {
            DXGKARGCB_RELEASEHANDLEDATA r = {0};
            r.ReleaseHandle = rel;
            r.Type = DXGK_HANDLE_ALLOCATION;
            dx->DxgkCbReleaseHandleData(r);
        }
    }
    return (MW_ALLOCATION *)p;
}

NTSTATUS APIENTRY MwOpenAllocation(IN_CONST_HANDLE hDevice, const DXGKARG_OPENALLOCATION *Arg)
{
    MW_DEVICE *dev = (MW_DEVICE *)hDevice;

    LONG n = InterlockedIncrement(&dev->Adapter->Opens);
    NTSTATUS st = STATUS_SUCCESS;

    for (UINT i = 0; i < Arg->NumAllocations; i++) {
        Arg->pOpenAllocation[i].hDeviceSpecificAllocation =
            MwAllocationFromHandle(dev->Adapter, Arg->pOpenAllocation[i].hAllocation);
        /* MS Learn DXGKCB_GETHANDLEDATA: "If a NULL pointer is returned, the display miniport driver should fail
         * its currently running DDI function with STATUS_INVALID_HANDLE." (g41full: every open got NULL here and
         * this function still returned success.) */
        if (Arg->pOpenAllocation[i].hDeviceSpecificAllocation == NULL)
            st = STATUS_INVALID_HANDLE;
    }
    /* g39/g41: DWM composes black because d3d10umd's OpenResource fails; what does it open? Identified from the
     * private data the allocation was created with (ours or GetStandardAllocationDriverData's MW_ALLOCATION_INFO),
     * which does not depend on DxgkCbGetHandleData. Opens 1-8 and every 16th go to the PRAM ring (First FALSE). */
    if (n <= 8 || (n & 15) == 0) {
        const DXGK_OPENALLOCATIONINFO *o = Arg->NumAllocations ? &Arg->pOpenAllocation[0] : NULL;
        MwPramText(FALSE, "Open n.allocs ", NULL, ((ULONG)n << 16) | (Arg->NumAllocations & 0xFFFF), TRUE);
        if (o != NULL) {
            MwPramText(FALSE, "Open h.size ", NULL,
                       ((ULONG)(o->hDeviceSpecificAllocation != NULL) << 24) | (o->PrivateDriverDataSize & 0xFFFFFF), TRUE);
            if (o->pPrivateDriverData != NULL && o->PrivateDriverDataSize >= sizeof(MW_ALLOCATION_INFO)) {
                const MW_ALLOCATION_INFO *pi = (const MW_ALLOCATION_INFO *)o->pPrivateDriverData;
                MwPramText(FALSE, "Open ver.flags ", NULL, (pi->Version << 16) | (pi->Flags & 0xFFFF), TRUE);
                MwPramText(FALSE, "Open wxh ", NULL, (pi->Width << 16) | (pi->Height & 0xFFFF), TRUE);
                MwPramText(FALSE, "Open fmt ", NULL, pi->Format, TRUE);
            }
        }
        if (!NT_SUCCESS(st))
            MwPramText(FALSE, "Open status ", NULL, (ULONG)st, TRUE);
    }
    return st;
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
            alloc->SegOffset = (ULONG64)Arg->MapApertureSegment.OffsetInPages * PAGE_SIZE;
            alloc->SegAddress = MK_VA_START + alloc->SegOffset;
            ExReleaseFastMutex(&a->MmuLock);
            if (alloc->Flags & MW_ALLOC_PRIMARY)
                MwScanoutRegister(a, alloc, TRUE);
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
        alloc->SegOffset = (ULONG64)Arg->MapApertureSegment.OffsetInPages * PAGE_SIZE;
        alloc->SegAddress = MK_VA_START + alloc->SegOffset;
        ExReleaseFastMutex(&a->MmuLock);
        if (alloc->Flags & MW_ALLOC_PRIMARY)
            MwScanoutRegister(a, alloc, TRUE);
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
        MwScanoutRegister(a, alloc, FALSE);
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
    case DXGK_OPERATION_UPDATE_PAGE_TABLE:
    case DXGK_OPERATION_FLUSH_TLB:
    case DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES:
    case DXGK_OPERATION_VIRTUAL_TRANSFER:
    case DXGK_OPERATION_VIRTUAL_FILL:
    case DXGK_OPERATION_UPDATE_CONTEXT_ALLOCATION:
        /* S7a: the GpuMmu page tables belong to the software copy node and nothing walks them (the Mali node is
         * physical); virtual transfers/fills only move data between segments, and the one aperture segment never
         * needs it. Accepted as no-ops, counted in PagingOps. */
        if (MwWddm2Gpummu)
            return STATUS_SUCCESS;
        return STATUS_NOT_SUPPORTED;
    case DXGK_OPERATION_NOTIFY_RESIDENCY:
        /* G15 S1: WDDM 2.0 tells the driver an allocation became resident / was evicted. Our mapping happens in
         * MAP/UNMAP_APERTURE_SEGMENT (both "common WDDMv1 and WDDMv2"), so nothing to do here. */
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* ---- the UMD's question: where is this allocation on the GPU? ---- */

/* H4a (notes/gpu-hw-path.md): the GOP framebuffer the panel scans (A->Fb, physically contiguous, from
 * DxgkCbAcquirePostDisplayOwnership) mapped into the GPU's address space once, NoExec, so that the UMD can copy
 * DWM's frame into it with the GPU instead of three CPU passes. PASSIVE, after the GPU half is up. */
void MwMapScanout(MW_ADAPTER *A)
{
    ULONG pages;
    PFN_NUMBER *pfn;
    ULONG64 va = 0;
    NTSTATUS st = STATUS_UNSUCCESSFUL;

    if (!A->GpuUp || A->FbVa == NULL || A->FbSize == 0 || A->Fb.PhysicAddress.QuadPart == 0)
        return;
    pages = (ULONG)((A->FbSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
    pfn = (PFN_NUMBER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)pages * sizeof(PFN_NUMBER), MW_TAG);
    if (pfn == NULL)
        return;
    for (ULONG i = 0; i < pages; i++)
        pfn[i] = (PFN_NUMBER)((A->Fb.PhysicAddress.QuadPart >> PAGE_SHIFT) + i);
    ExAcquireFastMutex(&A->MmuLock);
    if (MkVaAlloc(&A->Gpu, pages, FALSE, &va)) {
        st = MkMmuMapPages(&A->Gpu, va, pfn, pages, TRUE);
        if (!NT_SUCCESS(st))
            MkVaFree(&A->Gpu, va, pages);
    }
    ExReleaseFastMutex(&A->MmuLock);
    ExFreePoolWithTag(pfn, MW_TAG);
    A->ScanoutGpuVa = NT_SUCCESS(st) ? va : 0;
    MwLog(A, L"ScanoutGpuVa", (ULONG)A->ScanoutGpuVa);
    MwLog(A, L"ScanoutMap", (ULONG)st);
}

NTSTATUS MwEscapeScanout(MW_ADAPTER *A, const DXGKARG_ESCAPE *Esc, MW_ESCAPE_SCANOUT_DATA *D)
{
    if (D->hAllocation != 0) {
        MW_ALLOCATION *alloc;

        if (Esc->hDevice == NULL)
            return STATUS_INVALID_PARAMETER;
        alloc = MwAllocationFromHandle(A, (D3DKMT_HANDLE)D->hAllocation);
        if (alloc == NULL)
            return STATUS_INVALID_HANDLE;
        alloc->GpuPresented = A->ScanoutGpuVa != 0;
    }
    D->GpuVa = A->ScanoutGpuVa;
    D->Width = A->Fb.Width;
    D->Height = A->Fb.Height;
    D->Pitch = A->Fb.Pitch;
    D->Format = (mw_u32)A->Fb.ColorFormat;
    return STATUS_SUCCESS;
}

NTSTATUS MwEscapeAllocInfo(MW_ADAPTER *A, const DXGKARG_ESCAPE *Esc, MW_ESCAPE_ALLOC_INFO_DATA *D)
{
    MW_ALLOCATION *alloc;

    if (Esc->hDevice == NULL)
        return STATUS_INVALID_PARAMETER;
    alloc = MwAllocationFromHandle(A, (D3DKMT_HANDLE)D->hAllocation);
    if (alloc == NULL)
        return STATUS_INVALID_HANDLE;
    D->GpuVa = alloc->GpuVa;
    D->Size = alloc->Size;
    return STATUS_SUCCESS;
}
