// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm's command path (docs/M5.2-PLAN.md): Render copies the UMD's
 * MW_COMMANDs into the DMA buffer's private data, SubmitCommand queues them
 * with their fence, and the hardware works through the queue one job chain
 * at a time. When a submission's last chain finishes, its fence is reported
 * DMA_COMPLETED.
 *
 * The queue is shared with the ISR, so every change to it happens at the
 * device's interrupt level: inside DxgkCbSynchronizeExecution or the ISR.
 * Whether dxgkrnl connects the Mali's interrupts on this ACPI device is not
 * yet known, so a timer also polls the job status while work is in flight;
 * with working interrupts it only costs latency.
 *
 * MaliG76Wddm (Galaxy S10e, Mali-G76): derived from wddm-mali-g52 drivers/maliwddm/command.c (commit 16f7e7c6,
 * GPL-2.0, Copyright gahingwoo). Changes are marked "G76:" (notes/gpu-m16-g10.md).
 */
#include "MaliG76Wddm.h"

#define MW_DMA_MAGIC            0x4D57444Du /* 'MWDM' in the private data */
#define MW_DMA_BUFFER_SIZE      (16 * 1024)
#define MW_LIST_SIZE            256     /* Panfrost submits reference many BOs */
#define MW_JOB_TIMEOUT_MS       2000
#define MW_POLL_MS              1

/* ---- devices and contexts ---- */

NTSTATUS APIENTRY MwCreateDevice(IN_CONST_HANDLE hAdapter, DXGKARG_CREATEDEVICE *Arg)
{
    MW_DEVICE *dev = (MW_DEVICE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*dev), MW_TAG);

    if (dev == NULL)
        return STATUS_NO_MEMORY;
    dev->Adapter = (MW_ADAPTER *)hAdapter;
    Arg->hDevice = dev;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwDestroyDevice(IN_CONST_HANDLE hDevice)
{
    ExFreePoolWithTag(hDevice, MW_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCreateContext(IN_CONST_HANDLE hDevice, DXGKARG_CREATECONTEXT *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx), MW_TAG);

    if (ctx == NULL)
        return STATUS_NO_MEMORY;
    ctx->Device = (MW_DEVICE *)hDevice;
    Arg->hContext = ctx;
    if (MwWddm2Gpummu && (Arg->NodeOrdinal != 0 || Arg->Flags.VirtualAddressing)) {
        /* S7a: a context on the software copy node: no lists; a virtual one needs no patching (Learn
         * DXGK_CONTEXTINFO_CAPS); a physical one (the system paging context) gets a DMA buffer for paging buffers. */
        Arg->ContextInfo.DmaBufferSize = Arg->Flags.VirtualAddressing ? 0 : MW_DMA_BUFFER_SIZE;
        Arg->ContextInfo.DmaBufferSegmentSet = 0;
        Arg->ContextInfo.DmaBufferPrivateDataSize = 0;
        Arg->ContextInfo.AllocationListSize = 0;
        Arg->ContextInfo.PatchLocationListSize = 0;
        Arg->ContextInfo.Caps.NoPatchingRequired = Arg->Flags.VirtualAddressing ? 1 : 0;
        return STATUS_SUCCESS;
    }
    Arg->ContextInfo.DmaBufferSize = MW_DMA_BUFFER_SIZE;
    Arg->ContextInfo.DmaBufferSegmentSet = 0;           /* system memory, as viogpu3d */
    Arg->ContextInfo.DmaBufferPrivateDataSize = sizeof(MW_DMA_PRIVATE);
    Arg->ContextInfo.AllocationListSize = MW_LIST_SIZE;
    Arg->ContextInfo.PatchLocationListSize = MW_LIST_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwDestroyContext(IN_CONST_HANDLE hContext)
{
    ExFreePoolWithTag(hContext, MW_TAG);
    return STATUS_SUCCESS;
}

/* ---- building DMA buffers ---- */

/* The DMA buffer itself is never read: SubmitCommand gets only its physical
 * address. It carries one marker word so that no submission is empty. */
static void MarkDmaBuffer(VOID **DmaBuffer)
{
    *(ULONG *)*DmaBuffer = MW_DMA_MAGIC;
    *DmaBuffer = (PUCHAR)*DmaBuffer + sizeof(ULONG);
}

/* One patch location per allocation the buffer references. Nothing in our
 * DMA buffer needs patching (GPU addresses are fixed), but WDDM 1.x VidMm
 * learns from the patch list what to make resident: with the UMD's empty
 * list the job's allocation was never mapped into the aperture (WinPE runs
 * 13-14: no MAP_APERTURE_SEGMENT for it, translation fault at its address). */
static void PatchEveryAllocation(UINT Allocations, D3DDDI_PATCHLOCATIONLIST **Out, UINT OutSize)
{
    for (UINT i = 0; i < Allocations && i < OutSize; i++) {
        RtlZeroMemory(*Out, sizeof(**Out));
        (*Out)->AllocationIndex = i;
        (*Out)->SlotId = i;
        (*Out)++;
    }
}

NTSTATUS APIENTRY MwRender(IN_CONST_HANDLE hContext, DXGKARG_RENDER *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)hContext;
    MW_DMA_PRIVATE *priv = (MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;
    ULONG count;

    if (priv == NULL || Arg->DmaBufferPrivateDataSize < sizeof(*priv) || Arg->DmaSize < sizeof(ULONG))
        return STATUS_INVALID_PARAMETER;
    if (Arg->CommandLength % sizeof(MW_COMMAND) != 0)
        return STATUS_INVALID_PARAMETER;
    count = Arg->CommandLength / sizeof(MW_COMMAND);
    if (count > MW_MAX_COMMANDS)
        return STATUS_INVALID_PARAMETER;

    /* pCommand is the UMD's buffer, mapped in the caller's process. */
    __try {
        for (ULONG i = 0; i < count; i++) {
            MW_COMMAND c = ((const MW_COMMAND *)Arg->pCommand)[i];
            if (c.Type != MW_CMD_JOB || c.Slot > 1 || c.Jc == 0)
                return STATUS_INVALID_PARAMETER;
            priv->Commands[i] = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_INVALID_USER_BUFFER;
    }
    priv->Count = count;
    priv->Reserved = MW_DMA_MAGIC;

    PatchEveryAllocation(Arg->AllocationListSize, &Arg->pPatchLocationListOut, Arg->PatchLocationListOutSize);
    MarkDmaBuffer(&Arg->pDmaBuffer);
    ctx->Device->Adapter->Renders++;
    return STATUS_SUCCESS;
}

/* Presents are done by the CPU at submission (MwSubmitCommand): a blt from
 * the source allocation's pages, or a colour fill, into the primary (the
 * POST framebuffer) or the destination allocation's pages. Render records
 * what to do in the DMA buffer's private data and patches both allocations
 * so that VidMm makes them resident, which is when their pages are known.
 * Flips: with FlipMmio an MMIO flip (no DMA buffer) is only validated here and runs through
 * SetVidPnSourceAddress; with FlipBlt and FlipMmio 0 a DMA flip becomes a whole-surface blt (below). */
NTSTATUS APIENTRY MwPresent(IN_CONST_HANDLE hContext, DXGKARG_PRESENT *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)hContext;
    MW_ADAPTER *a = ctx->Device->Adapter;
    MW_DMA_PRIVATE *priv = (MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;

    /* MMIO flip (FlipCaps.FlipOnVSyncMmIo, Parameters\FlipMmio): MS Learn DxgkDdiPresent - "called with the
     * pDmaBuffer member ... set to NULL because a MMIO-based flip does not require a DMA buffer ... must validate
     * the source surface and program flip hardware as required"; dxgkrnl then runs the flip through
     * DxgkDdiSetVidPnSourceAddress (display.c: queued, latched by the next vsync DPC). Run g33full: with no private
     * data this function returned STATUS_INVALID_PARAMETER - not a documented DxgkDdiPresent status, dxgkrnl ETW 494
     * "Driver returned an invalid NTSTATUS code" - on every DWM flip. */
    if (Arg->Flags.Flip && MwFlipMmio && Arg->pDmaBuffer == NULL) {
        if (Arg->pAllocationList == NULL ||
            Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation == NULL)
            return STATUS_INVALID_HANDLE;
        a->Presents++;
        return STATUS_SUCCESS;
    }
    /* Not a documented DxgkDdiPresent status either, but only reachable with a DMA buffer and no private data. */
    if (priv == NULL || Arg->DmaBufferPrivateDataSize < sizeof(*priv))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(priv, sizeof(*priv));
    priv->Reserved = MW_DMA_MAGIC;
    /* G76: a flip names only the new primary (the source slot); with FlipBlt it becomes a whole-surface blt into the
     * GOP framebuffer, which the firmware's scan-out shows. The original dropped flips (step d not done), so a
     * compositor that flips never reached the panel. */
    BOOLEAN flip = Arg->Flags.Flip && MwFlipBlt && !MwFlipMmio && Arg->pAllocationList != NULL &&
                   Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation != NULL;
    if (Arg->Flags.Blt || Arg->Flags.ColorFill || flip) {
        MW_PRESENT_OP *op = &priv->Present;
        op->Kind = Arg->Flags.ColorFill ? MW_PRESENT_FILL : MW_PRESENT_BLT;
        op->ToScanout = flip && !Arg->Flags.Blt && !Arg->Flags.ColorFill;
        op->Color = Arg->Color;
        /* A present's allocation list has fixed slots; an unused one has
         * no allocation. */
        op->Src = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
        op->Dst = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation;
        op->SrcRect = Arg->SrcRect;
        op->DstRect = Arg->DstRect;
        op->NumRects = Arg->SubRectCnt <= MW_MAX_RECTS ? Arg->SubRectCnt : 0;
        for (ULONG i = 0; i < op->NumRects; i++)
            op->Rects[i] = Arg->pDstSubRects[i];
        if (op->ToScanout) {
            /* The whole new primary (a flip carries no rectangles). */
            const MW_ALLOCATION *s = op->Src;
            op->SrcRect.left = op->SrcRect.top = 0;
            op->SrcRect.right = (LONG)s->Width;
            op->SrcRect.bottom = (LONG)s->Height;
            op->DstRect = op->SrcRect;
            op->NumRects = 0;
        }
        /* Both allocations must be resident (and their pages known). */
        for (UINT i = DXGK_PRESENT_SOURCE_INDEX;
             i <= DXGK_PRESENT_DESTINATION_INDEX && Arg->PatchLocationListOutSize > 0; i++) {
            if (Arg->pAllocationList[i].hDeviceSpecificAllocation == NULL)
                continue;
            RtlZeroMemory(Arg->pPatchLocationListOut, sizeof(*Arg->pPatchLocationListOut));
            Arg->pPatchLocationListOut->AllocationIndex = i;
            Arg->pPatchLocationListOut->SlotId = i;
            Arg->pPatchLocationListOut++;
            Arg->PatchLocationListOutSize--;
        }
    }
    if (Arg->pDmaBuffer != NULL && Arg->DmaSize >= sizeof(ULONG))
        MarkDmaBuffer(&Arg->pDmaBuffer);
    a->Presents++;
    return STATUS_SUCCESS;
}

/* The CPU view of a resident allocation, or NULL. */
static PUCHAR AllocationVa(MW_ALLOCATION *Alloc)
{
    PUCHAR va;
    if (Alloc == NULL || Alloc->Mdl == NULL)
        return NULL;
    va = (PUCHAR)MmGetSystemAddressForMdlSafe(Alloc->Mdl, NormalPagePriority | MdlMappingNoExecute);
    return va != NULL ? va + (SIZE_T)Alloc->MdlPage * PAGE_SIZE : NULL;
}

static void ClipRect(RECT *R, LONG W, LONG H)
{
    if (R->left < 0) R->left = 0;
    if (R->top < 0) R->top = 0;
    if (R->right > W) R->right = W;
    if (R->bottom > H) R->bottom = H;
}

/* G20 (notes/g20-scaled-modes.md): a present into the Fb from a primary smaller than the Fb (the scaled source mode,
 * VW x VH) - nearest-neighbour stretch of each rectangle (primary coordinates) onto the Fb. Fb pixel (X, Y) shows
 * primary pixel (X * VW / FbW, Y * VH / FbH); a rectangle [t, b) of primary rows covers Fb rows
 * [ceil(t * FbH / VH), ceil(b * FbH / VH)). The GPU path (Mesa d3d10_scanout_present) is the normal one; this is the
 * fallback for CPU-presented frames. */
static void ScalePresent(MW_ADAPTER *A, const MW_PRESENT_OP *Op, PUCHAR Src, LONG SrcW, LONG SrcH, LONG SrcPitch,
                         LONG VW, LONG VH)
{
    LONG fw = (LONG)A->Fb.Width, fh = (LONG)A->Fb.Height, fp = (LONG)A->Fb.Pitch;
    ULONG n = Op->NumRects ? Op->NumRects : 1;

    for (ULONG i = 0; i < n; i++) {
        RECT r = Op->NumRects ? Op->Rects[i] : Op->DstRect;
        LONG dx = Op->SrcRect.left - Op->DstRect.left, dy = Op->SrcRect.top - Op->DstRect.top;

        ClipRect(&r, VW, VH);
        if (Op->Kind == MW_PRESENT_BLT) {
            if (r.left + dx < 0) r.left = -dx;
            if (r.top + dy < 0) r.top = -dy;
            if (r.right + dx > SrcW) r.right = SrcW - dx;
            if (r.bottom + dy > SrcH) r.bottom = SrcH - dy;
        }
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        LONG y0 = (LONG)(((LONG64)r.top * fh + VH - 1) / VH), y1 = (LONG)(((LONG64)r.bottom * fh + VH - 1) / VH);
        LONG x0 = (LONG)(((LONG64)r.left * fw + VW - 1) / VW), x1 = (LONG)(((LONG64)r.right * fw + VW - 1) / VW);
        for (LONG y = y0; y < y1 && y < fh; y++) {
            ULONG *d = (ULONG *)(A->FbVa + (SIZE_T)y * fp);
            if (Op->Kind == MW_PRESENT_BLT) {
                const ULONG *s = (const ULONG *)(Src + (SIZE_T)((LONG)((LONG64)y * VH / fh) + dy) * SrcPitch);
                for (LONG x = x0; x < x1 && x < fw; x++)
                    d[x] = s[(LONG)((LONG64)x * VW / fw) + dx];
            } else {
                for (LONG x = x0; x < x1 && x < fw; x++)
                    d[x] = Op->Color;
            }
        }
    }
    if (Op->Kind == MW_PRESENT_BLT)
        A->PresentBlts++;
    else
        A->PresentFills++;
}

/* Carry out a present on the CPU. 32-bit pixels only. */
static void DoPresent(MW_ADAPTER *A, const MW_PRESENT_OP *Op)
{
    PUCHAR dst, src = NULL;
    LONG dstW, dstH, dstPitch, srcPitch = 0, srcW = 0, srcH = 0;
    ULONG n = Op->NumRects ? Op->NumRects : 1;

    /* H4a: a GPU-presented primary's frame is in the framebuffer already (the UMD copied it there on the GPU). */
    if (Op->Kind == MW_PRESENT_BLT && Op->Src != NULL && Op->Src->GpuPresented &&
        (Op->ToScanout || (Op->Dst != NULL && (Op->Dst->Flags & MW_ALLOC_PRIMARY)))) {
        A->PresentGpu++;
        return;
    }
    if ((Op->ToScanout || (Op->Dst != NULL && (Op->Dst->Flags & MW_ALLOC_PRIMARY))) && A->FbVa != NULL) {
        dst = A->FbVa;
        dstW = (LONG)A->Fb.Width;
        dstH = (LONG)A->Fb.Height;
        dstPitch = (LONG)A->Fb.Pitch;
    } else if (Op->ToScanout) {
        A->PresentSkips++;
        return;
    } else {
        dst = AllocationVa(Op->Dst);
        if (dst == NULL) {
            A->PresentSkips++;
            return;
        }
        dstW = (LONG)Op->Dst->Width;
        dstH = (LONG)Op->Dst->Height;
        dstPitch = (LONG)(Op->Dst->Pitch ? Op->Dst->Pitch : Op->Dst->Width * 4);
    }
    if (Op->Kind == MW_PRESENT_BLT) {
        src = AllocationVa(Op->Src);
        if (src == NULL) {
            A->PresentSkips++;
            return;
        }
        srcW = (LONG)Op->Src->Width;
        srcH = (LONG)Op->Src->Height;
        srcPitch = (LONG)(Op->Src->Pitch ? Op->Src->Pitch : Op->Src->Width * 4);
    }
    if (MwScaledModes && dst == A->FbVa) {
        /* G20: the primary's own size - the flipped source, or the primary named as destination. */
        const MW_ALLOCATION *prim = Op->ToScanout ? Op->Src : Op->Dst;
        LONG vw = prim != NULL ? (LONG)prim->Width : dstW, vh = prim != NULL ? (LONG)prim->Height : dstH;
        if (vw > 0 && vh > 0 && vw <= dstW && vh <= dstH && (vw != dstW || vh != dstH)) {
            ScalePresent(A, Op, src, srcW, srcH, srcPitch, vw, vh);
            return;
        }
    }

    for (ULONG i = 0; i < n; i++) {
        RECT r = Op->NumRects ? Op->Rects[i] : Op->DstRect;
        LONG dx = Op->SrcRect.left - Op->DstRect.left, dy = Op->SrcRect.top - Op->DstRect.top;

        ClipRect(&r, dstW, dstH);
        if (Op->Kind == MW_PRESENT_BLT) {
            /* Keep the source side inside the source too. */
            if (r.left + dx < 0) r.left = -dx;
            if (r.top + dy < 0) r.top = -dy;
            if (r.right + dx > srcW) r.right = srcW - dx;
            if (r.bottom + dy > srcH) r.bottom = srcH - dy;
        }
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        for (LONG y = r.top; y < r.bottom; y++) {
            ULONG *d = (ULONG *)(dst + (SIZE_T)y * dstPitch) + r.left;
            if (Op->Kind == MW_PRESENT_BLT) {
                const ULONG *s = (const ULONG *)(src + (SIZE_T)(y + dy) * srcPitch) + r.left + dx;
                RtlCopyMemory(d, s, (SIZE_T)(r.right - r.left) * 4);
            } else {
                for (LONG x = 0; x < r.right - r.left; x++)
                    d[x] = Op->Color;
            }
        }
    }
    if (Op->Kind == MW_PRESENT_BLT)
        A->PresentBlts++;
    else
        A->PresentFills++;
}

NTSTATUS APIENTRY MwPatch(IN_CONST_HANDLE hAdapter, const DXGKARG_PATCH *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    /* Nothing to patch; record where VidMm put the first allocation. */
    a->Patches++;
    a->PatchAllocations = Arg->AllocationListSize;
    a->PatchPatchLocations = Arg->PatchLocationListSubmissionLength;
    if (Arg->AllocationListSize != 0 && Arg->pAllocationList != NULL) {
        a->PatchSegment0 = Arg->pAllocationList[0].SegmentId;
        a->PatchAddress0 = (ULONG64)Arg->pAllocationList[0].PhysicalAddress.QuadPart;
    }
    return STATUS_SUCCESS;
}

/* ---- the queue, at interrupt level ---- */

static void NotifyCompletedNode(MW_ADAPTER *A, UINT Node, ULONG Fence)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {0};

    n.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    n.DmaCompleted.SubmissionFenceId = Fence;
    n.DmaCompleted.NodeOrdinal = Node;
    n.DmaCompleted.EngineOrdinal = 0;
    A->Dxgk.DxgkCbNotifyInterrupt(A->Dxgk.DeviceHandle, &n);
    if (Node == 0)
        A->LastCompletedFence = Fence;
    else
        A->LastCompletedFenceCopy = Fence;
    A->NeedDpc = TRUE;
}

static void NotifyCompleted(MW_ADAPTER *A, ULONG Fence)
{
    NotifyCompletedNode(A, 0, Fence);
}

typedef struct {
    MW_ADAPTER *A;
    UINT Node;
    ULONG Fence;
} MW_COMPLETE;

static BOOLEAN SyncCompleteNow(PVOID Context)
{
    MW_COMPLETE *c = (MW_COMPLETE *)Context;
    NotifyCompletedNode(c->A, c->Node, c->Fence);
    c->A->NeedDpc = FALSE;
    c->A->Dxgk.DxgkCbQueueDpc(c->A->Dxgk.DeviceHandle);
    return TRUE;
}

/* Start the next job chain, completing every submission that has none left.
 * Called with the queue owned (interrupt level). */
static void Kick(MW_ADAPTER *A)
{
    while (!A->Running && A->QCount != 0) {
        MW_SUBMISSION *s = &A->Queue[A->QHead];

        if (!A->GpuUp || s->Next >= s->Count) {
            NotifyCompleted(A, s->FenceId);
            A->QHead = (A->QHead + 1) % MW_QUEUE_DEPTH;
            A->QCount--;
            continue;
        }
        const MW_COMMAND *c = &s->Commands[s->Next++];
        A->RunningSlot = c->Slot;
        A->Running = TRUE;
        A->JobStart = KeQueryInterruptTime();
        (void)MkJobTakeStatus(&A->Gpu, c->Slot);
        InterlockedExchange(&A->Gpu.MmuIrqStatus, 0);
        MkJobStart(&A->Gpu, c->Slot, c->Jc, (A->ChainFlag++ & 1) != 0);
    }
}

/* Collect the running chain's outcome. TRUE if it finished, either way. */
static BOOLEAN Reap(MW_ADAPTER *A, BOOLEAN CheckTimeout)
{
    MK_GPU *gpu = &A->Gpu;
    ULONG slot = A->RunningSlot;
    ULONG bits;
    BOOLEAN failed;

    if (!A->Running)
        return FALSE;
    bits = MkJobTakeStatus(gpu, slot);
    if (bits & (1u << (16 + slot)) || gpu->MmuIrqStatus != 0) {
        A->LastJsStatus = MkJobSlotStatus(gpu, slot);
        failed = TRUE;
    } else if (bits & (1u << slot)) {
        failed = FALSE;
    } else if (CheckTimeout && (KeQueryInterruptTime() - A->JobStart) / 10000 > MW_JOB_TIMEOUT_MS) {
        A->LastJsStatus = MkJobSlotStatus(gpu, slot);
        MkJobHardStop(gpu, slot);
        A->JobsTimedOut++;
        failed = TRUE;
    } else {
        return FALSE;
    }

    A->Running = FALSE;
    if (!failed) {
        A->JobsDone++;
        return TRUE;
    }
    /* A fault leaves the address space stopped: start the GPU over, and
     * drop the rest of this submission (its later chains depend on it). */
    A->JobsFailed++;
    A->LastFaultStatus = gpu->LastMmuFault;
    A->LastFaultAddress = gpu->LastFaultAddress;
    A->Queue[A->QHead].Next = A->Queue[A->QHead].Count;
    gpu->Resets++;
    if (!MkGpuBringup(gpu))
        A->GpuUp = FALSE;
    return TRUE;
}

static BOOLEAN Synchronize(MW_ADAPTER *A, PKSYNCHRONIZE_ROUTINE Routine, PVOID Context);

/* S7a: complete a submission of a node without hardware (the software copy node) right away, at interrupt level as
 * DxgkCbNotifyInterrupt requires. */
void MwCompleteNow(MW_ADAPTER *A, UINT Node, ULONG Fence)
{
    MW_COMPLETE c = { A, Node, Fence };
    (void)Synchronize(A, SyncCompleteNow, &c);
}

/* From the ISR, after MkIsr has collected the status. */
BOOLEAN MwJobInterrupt(MW_ADAPTER *A)
{
    if (Reap(A, FALSE))
        Kick(A);
    if (A->NeedDpc) {
        A->NeedDpc = FALSE;
        A->Dxgk.DxgkCbQueueDpc(A->Dxgk.DeviceHandle);
    }
    return TRUE;
}

typedef struct {
    MW_ADAPTER *A;
    const MW_SUBMISSION *New;
    NTSTATUS Status;
} MW_SYNC;

static BOOLEAN SyncSubmit(PVOID Context)
{
    MW_SYNC *s = (MW_SYNC *)Context;
    MW_ADAPTER *a = s->A;

    if (a->QCount == MW_QUEUE_DEPTH) {
        s->Status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        return FALSE;
    }
    a->Queue[(a->QHead + a->QCount) % MW_QUEUE_DEPTH] = *s->New;
    a->QCount++;
    a->LastSubmittedFence = s->New->FenceId;
    Kick(a);
    if (a->NeedDpc) {
        a->NeedDpc = FALSE;
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
    }
    s->Status = STATUS_SUCCESS;
    return TRUE;
}

static BOOLEAN SyncPoll(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;

    if (Reap(a, TRUE))
        Kick(a);
    if (a->NeedDpc) {
        a->NeedDpc = FALSE;
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
    }
    return a->QCount != 0;
}

static BOOLEAN Synchronize(MW_ADAPTER *A, PKSYNCHRONIZE_ROUTINE Routine, PVOID Context)
{
    BOOLEAN ret = FALSE;
    NTSTATUS st = A->Dxgk.DxgkCbSynchronizeExecution(A->Dxgk.DeviceHandle, Routine, Context, 0, &ret);
    return NT_SUCCESS(st) && ret;
}

/* The fallback for interrupts that never arrive, and the job timeout. */
static VOID PollDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (a->Stopping) {
        InterlockedExchange(&a->PollArmed, 0);
        return;
    }
    if (Synchronize(a, SyncPoll, a)) {
        MwArmPoll(a);
        return;
    }
    /* Idle. A submission that raced with this saw PollArmed set and did
     * not arm the timer, so look once more after letting go. */
    InterlockedExchange(&a->PollArmed, 0);
    KeMemoryBarrier();
    if (a->QCount != 0 && InterlockedExchange(&a->PollArmed, 1) == 0)
        MwArmPoll(a);
}

void MwArmPoll(MW_ADAPTER *A)
{
    LARGE_INTEGER due;
    due.QuadPart = -10000LL * MW_POLL_MS;
    KeSetTimer(&A->PollTimer, due, &A->PollDpc);
}

/* ---- software vsync ----
 * G76: the S10e's display (DECON) is the firmware's and this driver has no interrupt from it, but a full WDDM
 * driver that is asked to enable DXGK_INTERRUPT_CRTC_VSYNC must report one every vertical retrace (MS Learn,
 * DxgkDdiControlInterrupt remarks). The original answered "enabled" and never reported one, so every vblank wait in
 * dxgkrnl (DWM pacing, sync-interval presents, flips) waited forever - the prime suspect for the full-adapter black
 * screen (notes/gpu-full-adapter-hang.md). A periodic timer stands in: its DPC reports the vsync at the device's
 * interrupt level through DxgkCbSynchronizeExecution and queues our DPC, as DxgkCbNotifyInterrupt requires. The
 * panel's real refresh is whatever sboot programmed (60 Hz on this phone, UNVERIFIED by a register read). The timer
 * period is 1/60 s on a high-resolution EX_TIMER (MwCommandInit), matching the 60 Hz the VidPN reports (display.c
 * FillSignal). Fallback when ExAllocateTimer fails: a 16 ms KeSetTimerEx timer (whole ms, 62.5 Hz nominal) whose
 * expiries land on the system clock tick (15.6 ms by default) and jitter by a tick - measured 8.9-39 ms intervals,
 * enough to make media presenters late (notes/g17-dxva-mfc.md). */
#define MW_VSYNC_PERIOD_MS      16              /* KeSetTimerEx fallback */
#define MW_VSYNC_PERIOD_100NS   166667ULL       /* 1/60 s: the EX_TIMER period, and GetScanLine's frame */

static BOOLEAN SyncVsync(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {0};

    n.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    n.CrtcVsync.VidPnTargetId = 0;
    n.CrtcVsync.PhysicalAddress = a->ScanoutAddress;
    a->Dxgk.DxgkCbNotifyInterrupt(a->Dxgk.DeviceHandle, &n);
    a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
    return TRUE;
}

static void DoPresent(MW_ADAPTER *A, const MW_PRESENT_OP *Op);

/* FlipMmio: the "scan-out latch" at a vsync (DISPATCH, VsDpc). The queued primary is copied into the GOP fb
 * whole, and its address becomes the one the vsync reports - what a display controller does when it latches a
 * new base address at vblank. ScanLock keeps FreeAllocation (MwScanoutForget) from freeing it meanwhile. */
static void Latch(MW_ADAPTER *A)
{
    KIRQL irql;
    MW_ALLOCATION *p;

    KeAcquireSpinLock(&A->ScanLock, &irql);
    p = (MW_ALLOCATION *)InterlockedExchangePointer((PVOID volatile *)&A->PendingPrimary, NULL);
    if (p != NULL) {
        MW_PRESENT_OP op;
        KeMemoryBarrier();
        RtlZeroMemory(&op, sizeof(op));
        op.Kind = MW_PRESENT_BLT;
        op.ToScanout = TRUE;
        op.Src = p;
        op.SrcRect.right = (LONG)p->Width;
        op.SrcRect.bottom = (LONG)p->Height;
        op.DstRect = op.SrcRect;
        /* Run g35full: DWM's MMIO flips all succeed and retire, the panel stays dark, and the counter dump was lost
         * to the hard reset. The first 4 latches go to PRAM: the source's CPU view (0 = no MDL -> skipped) and its
         * first and middle pixels (all 0 = the frame itself is black). */
        /* g38full: the first 4 fell out of the 255-line PRAM ring before the trace was read (boot-console flips) -
         * every 16th latch is logged too, so the latest ones are in the ring. */
        if (A->Latches < 4 || (A->Latches & 15) == 0) {
            PUCHAR va = AllocationVa(p);
            ULONG mid = (ULONG)(((SIZE_T)(p->Height / 2) * (p->Pitch ? p->Pitch : p->Width * 4)) +
                                (SIZE_T)(p->Width / 2) * 4);
            MwPramText(TRUE, "Latch va lo ", NULL, (ULONG)(ULONG_PTR)va, TRUE);
            if (va != NULL) {
                MwPramText(TRUE, "Latch px0 ", NULL, *(volatile ULONG *)va, TRUE);
                MwPramText(TRUE, "Latch pxmid ", NULL, *(volatile ULONG *)(va + mid), TRUE);
            }
            MwPramText(TRUE, "Latch fb skips ", NULL, ((A->FbVa != NULL) << 24) | (A->PresentSkips & 0xFFFFFF), TRUE);
        }
        DoPresent(A, &op);
        A->ScanoutAddress = A->PendingAddress;
        InterlockedIncrement(&A->Latches);
    }
    KeReleaseSpinLock(&A->ScanLock, irql);
}

/* PASSIVE (FreeAllocation). Under ScanLock: a latch in progress finishes first; a queued one is dropped. */
void MwScanoutForget(MW_ADAPTER *A, MW_ALLOCATION *Alloc)
{
    KIRQL irql;
    KeAcquireSpinLock(&A->ScanLock, &irql);
    (void)InterlockedCompareExchangePointer((PVOID volatile *)&A->PendingPrimary, NULL, Alloc);
    KeReleaseSpinLock(&A->ScanLock, irql);
    MwScanoutRegister(A, Alloc, FALSE);
}

/* PASSIVE (BuildPagingBuffer map/unmap, FreeAllocation): keep the table of mapped primaries that
 * MwScanoutLookup searches. A full table is reported once to PRAM. */
void MwScanoutRegister(MW_ADAPTER *A, MW_ALLOCATION *Alloc, BOOLEAN Mapped)
{
    static LONG full;
    ULONG i;

    for (i = 0; i < ARRAYSIZE(A->Prims); i++) {
        if (A->Prims[i] == Alloc) {
            if (!Mapped)
                (void)InterlockedExchangePointer((PVOID volatile *)&A->Prims[i], NULL);
            return;
        }
    }
    if (!Mapped)
        return;
    for (i = 0; i < ARRAYSIZE(A->Prims); i++) {
        if (InterlockedCompareExchangePointer((PVOID volatile *)&A->Prims[i], Alloc, NULL) == NULL)
            return;
    }
    if (InterlockedExchange(&full, 1) == 0)
        MwPramText(TRUE, "Prims table full", NULL, 0, FALSE);
}

/* Any IRQL (SetVidPnSourceAddress runs at DIRQL with FlipOnVSyncMmIo): the mapped primary at PrimaryAddress, or
 * NULL. The first 6 lookups go to PRAM with the address and which form matched (1 base+offset, 2 offset, 0 none). */
MW_ALLOCATION *MwScanoutLookup(MW_ADAPTER *A, ULONG64 PrimaryAddress)
{
    MW_ALLOCATION *hit = NULL;
    ULONG how = 0, i;

    for (i = 0; i < ARRAYSIZE(A->Prims) && hit == NULL; i++) {
        MW_ALLOCATION *p = A->Prims[i];
        if (p == NULL)
            continue;
        if (p->SegAddress == PrimaryAddress) {
            hit = p;
            how = 1;
        } else if (p->SegOffset == PrimaryAddress) {
            hit = p;
            how = 2;
        }
    }
    if (InterlockedIncrement(&A->Lookups) <= 6) {
        MwPramText(TRUE, "Lookup addr lo ", NULL, (ULONG)PrimaryAddress, TRUE);
        MwPramText(TRUE, "Lookup how ", NULL, how, TRUE);
    }
    return hit;
}

static void VsCancel(MW_ADAPTER *a)
{
    if (a->VsHrTimer != NULL)
        ExCancelTimer(a->VsHrTimer, NULL);
    else
        KeCancelTimer(&a->VsTimer);
}

static void VsTick(MW_ADAPTER *a)
{
    if (a->Stopping || !a->VsEnabled)
        return;
    if (MwFlipMmio)
        Latch(a);
    if (Synchronize(a, SyncVsync, a))
        InterlockedIncrement(&a->VsyncTicks);
    else if (a->VsyncTicks == 0 && InterlockedExchange(&a->VsEnabled, 0) != 0) {
        /* No interrupt object to synchronize with: stop instead of retrying 60 times a second. */
        VsCancel(a);
        MwPramText(TRUE, "SoftVsync sync failed, stopped", NULL, 0, FALSE);
    }
}

static VOID VsDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    VsTick((MW_ADAPTER *)Context);
}

/* EXT_CALLBACK of the high-resolution timer (DISPATCH_LEVEL, like the DPC). */
static VOID VsHrCallback(PEX_TIMER Timer, PVOID Context)
{
    UNREFERENCED_PARAMETER(Timer);
    VsTick((MW_ADAPTER *)Context);
}

void MwCommandInit(MW_ADAPTER *A)
{
    A->Stopping = FALSE;
    KeInitializeTimer(&A->PollTimer);
    KeInitializeDpc(&A->PollDpc, PollDpc, A);
    KeInitializeTimer(&A->VsTimer);
    KeInitializeDpc(&A->VsDpc, VsDpc, A);
    /* G17 pacing (notes/g17-dxva-mfc.md): the default-resolution KTIMER lands on the 15.6 ms clock tick (measured vblank
     * intervals 8.9-39 ms); MS Learn "High-Resolution Timers": an EX_TIMER_HIGH_RESOLUTION timer "never expires early"
     * and at most ~1 ms late. Its 16.67 ms period is above the default tick, as the page asks. */
    A->VsHrTimer = ExAllocateTimer(VsHrCallback, A, EX_TIMER_HIGH_RESOLUTION);
}

/* RemoveDevice (PASSIVE): the high-resolution timer is cancelled and its callback has returned before the adapter goes. */
void MwCommandFree(MW_ADAPTER *A)
{
    if (A->VsHrTimer != NULL) {
        ExDeleteTimer(A->VsHrTimer, TRUE, TRUE, NULL);
        A->VsHrTimer = NULL;
    }
}

void MwCommandStop(MW_ADAPTER *A)
{
    A->Stopping = TRUE;
    KeMemoryBarrier();
    KeCancelTimer(&A->PollTimer);
    InterlockedExchange(&A->VsEnabled, 0);
    VsCancel(A);
    KeFlushQueuedDpcs();
    InterlockedExchange(&A->PollArmed, 0);
}

NTSTATUS APIENTRY MwSubmitCommand(IN_CONST_HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    const MW_DMA_PRIVATE *priv = (const MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;
    MW_SUBMISSION sub;
    MW_SYNC sync;

    /* S7a: node 1 = the software copy/paging node (paging buffers arrive here; their operations ran on the CPU in
     * BuildPagingBuffer) - nothing to execute, complete at once on that node. */
    if (Arg->NodeOrdinal != 0) {
        MwCompleteNow(a, Arg->NodeOrdinal, Arg->SubmissionFenceId);
        a->Submits++;
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&sub, sizeof(sub));
    sub.FenceId = Arg->SubmissionFenceId;
    /* A present: done here, on the CPU; the submission then carries no
     * job and completes at once. */
    if (!Arg->Flags.Paging && priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) &&
        priv->Reserved == MW_DMA_MAGIC && priv->Present.Kind != MW_PRESENT_NONE)
        DoPresent(a, &priv->Present);
    /* Paging buffers (empty: map/unmap happen on the CPU), presents and
     * anything without our private data carry no jobs. */
    if (!Arg->Flags.Paging && priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) &&
        priv->Reserved == MW_DMA_MAGIC && priv->Count <= MW_MAX_COMMANDS) {
        sub.Count = priv->Count;
        RtlCopyMemory(sub.Commands, priv->Commands, sub.Count * sizeof(MW_COMMAND));
    }

    sync.A = a;
    sync.New = &sub;
    sync.Status = STATUS_UNSUCCESSFUL;
    (void)Synchronize(a, SyncSubmit, &sync);
    if (NT_SUCCESS(sync.Status) && sub.Count != 0 && !a->Stopping &&
        InterlockedExchange(&a->PollArmed, 1) == 0)
        MwArmPoll(a);
    a->Submits++;
    return sync.Status;
}

NTSTATUS APIENTRY MwPreemptCommand(IN_CONST_HANDLE hAdapter, const DXGKARG_PREEMPTCOMMAND *Arg)
{
    /* Preemption is off (EnablePreemption=0 in the INF). */
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryCurrentFence(IN_CONST_HANDLE hAdapter, DXGKARG_QUERYCURRENTFENCE *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    Arg->CurrentFence = Arg->NodeOrdinal != 0 ? a->LastCompletedFenceCopy : a->LastCompletedFence;
    return STATUS_SUCCESS;
}

/* ---- recovery ---- */

static BOOLEAN SyncReset(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;

    if (a->Running)
        MkJobHardStop(&a->Gpu, a->RunningSlot);
    a->Running = FALSE;
    a->QHead = a->QCount = 0;
    a->NeedDpc = FALSE;
    /* G76: only a GPU half that came up (MkGpuInit set Ready); never in the GpuEnable=0 modes, where Regs may be
     * mapped just to mask the interrupt outputs. */
    if (a->Gpu.Regs != NULL && a->Gpu.Ready && a->Mode == MwModeFullGpu) {
        a->Gpu.Resets++;
        a->GpuUp = MkGpuBringup(&a->Gpu);
    }
    return TRUE;
}

NTSTATUS APIENTRY MwResetFromTimeout(IN_CONST_HANDLE hAdapter)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    (void)Synchronize(a, SyncReset, a);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwRestartFromTimeout(IN_CONST_HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCollectDbgInfo(IN_CONST_HANDLE hAdapter, const DXGKARG_COLLECTDBGINFO *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    if (Arg->pBuffer != NULL && Arg->BufferSize > 0)
        (void)RtlStringCbPrintfA((NTSTRSAFE_PSTR)Arg->pBuffer, Arg->BufferSize,
                                 "MaliG76Wddm: submitted %u completed %u running %u queued %u "
                                 "done %u failed %u timedout %u js 0x%x fault 0x%x @0x%llx",
                                 a->LastSubmittedFence, a->LastCompletedFence, a->Running, a->QCount,
                                 a->JobsDone, a->JobsFailed, a->JobsTimedOut, a->LastJsStatus,
                                 a->LastFaultStatus, a->LastFaultAddress);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwGetNodeMetadata(IN_CONST_HANDLE hAdapter, UINT NodeOrdinal,
                                    DXGKARG_GETNODEMETADATA *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    RtlZeroMemory(Arg, sizeof(*Arg));
    if (MwWddm2Gpummu && NodeOrdinal == 1) {
        /* S7a: the software copy node, GpuMmu (the paging node) */
        Arg->EngineType = DXGK_ENGINE_TYPE_COPY;
        Arg->GpuMmuSupported = TRUE;
        (void)RtlStringCbCopyW(Arg->FriendlyName, sizeof(Arg->FriendlyName), L"CPU copy");
        return STATUS_SUCCESS;
    }
    if (NodeOrdinal != 0)
        return STATUS_INVALID_PARAMETER;
    Arg->EngineType = DXGK_ENGINE_TYPE_3D;
    (void)RtlStringCbCopyW(Arg->FriendlyName, sizeof(Arg->FriendlyName), L"Mali-G76 job manager");   /* G76: name only */
    return STATUS_SUCCESS;
}

/* ---- what WDDM 1.2+ requires of a full driver ----
 * dxgkrnl failed the adapter (code 43) right after DRIVERCAPS while these
 * five were NULL; viogpu3d fills all of them. */

NTSTATUS APIENTRY MwResetEngine(IN_CONST_HANDLE hAdapter, DXGKARG_RESETENGINE *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    (void)Synchronize(a, SyncReset, a);
    Arg->LastAbortedFenceId = a->LastCompletedFence;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryEngineStatus(IN_CONST_HANDLE hAdapter, DXGKARG_QUERYENGINESTATUS *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    Arg->EngineStatus.Responsive = 1;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCancelCommand(IN_CONST_HANDLE hAdapter, const DXGKARG_CANCELCOMMAND *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

/* G76: CRTC_VSYNC is the only type dxgkrnl passes; any other must be NOT_IMPLEMENTED (MS Learn). With SoftVsync the
 * timer above runs while enabled; with SoftVsync 0 the original's answer (enabled, never reported). */
NTSTATUS APIENTRY MwControlInterrupt(IN_CONST_HANDLE hAdapter, IN_CONST_DXGK_INTERRUPT_TYPE Type,
                                     IN_BOOLEAN Enable)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    if (Type != DXGK_INTERRUPT_CRTC_VSYNC)
        return STATUS_NOT_IMPLEMENTED;
    MwPramText(FALSE, Enable ? "Vsync on " : "Vsync off ", NULL, MwSoftVsync, TRUE);
    if (!MwSoftVsync || a->Stopping)
        return STATUS_SUCCESS;
    if (Enable) {
        if (InterlockedExchange(&a->VsEnabled, 1) == 0) {
            if (a->VsHrTimer != NULL) {
                /* 100 ns units: relative first expiry one frame from now, then every 1/60 s */
                ExSetTimer(a->VsHrTimer, -(LONGLONG)MW_VSYNC_PERIOD_100NS, (LONGLONG)MW_VSYNC_PERIOD_100NS, NULL);
            } else {
                LARGE_INTEGER due;
                due.QuadPart = -10000LL * MW_VSYNC_PERIOD_MS;
                KeSetTimerEx(&a->VsTimer, due, MW_VSYNC_PERIOD_MS, &a->VsDpc);
            }
        }
    } else if (InterlockedExchange(&a->VsEnabled, 0) != 0) {
        VsCancel(a);
    }
    return STATUS_SUCCESS;
}

/* G76: with SoftVsync, a scan position from the clock (one 60 Hz frame of Height + 45 lines, display.c FillSignal's
 * blanking), so a poller waiting for the vertical blank sees one; the original always said line 0, never in blank. */
NTSTATUS APIENTRY MwGetScanLine(IN_CONST_HANDLE hAdapter, DXGKARG_GETSCANLINE *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    ULONG total = a->Fb.Height + 45;
    ULONG line;

    if (!MwSoftVsync || a->Fb.Height == 0) {
        Arg->InVerticalBlank = FALSE;
        Arg->ScanLine = 0;
        return STATUS_SUCCESS;
    }
    line = (ULONG)((KeQueryInterruptTime() % MW_VSYNC_PERIOD_100NS) * total / MW_VSYNC_PERIOD_100NS);
    Arg->InVerticalBlank = line >= a->Fb.Height;
    Arg->ScanLine = Arg->InVerticalBlank ? 0 : line;
    return STATUS_SUCCESS;
}

/* GDI hardware acceleration (SupportKernelModeCommandBuffer). A render-only
 * adapter is never GDI's, so this is not expected to be called; it says so
 * instead of building a DMA buffer it cannot. */
NTSTATUS APIENTRY MwRenderKm(IN_CONST_HANDLE hContext, DXGKARG_RENDER *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)hContext;
    UNREFERENCED_PARAMETER(Arg);
    MwLog(ctx->Device->Adapter, L"RenderKmCalled", 1);
    return STATUS_NOT_SUPPORTED;
}

/* The third DDI SupportPerEngineTDR requires (with ResetEngine and
 * QueryEngineStatus): dxgkrnl said so in WinPE run 10 ("Driver reports
 * SupportPerEngineTDR cap but does not fill in all of the required DDIs")
 * and failed the start with STATUS_INVALID_PARAMETER. One engine, so
 * nothing depends on it. */
NTSTATUS APIENTRY MwQueryDependentEngineGroup(IN_CONST_HANDLE hAdapter,
                                              DXGKARG_QUERYDEPENDENTENGINEGROUP *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    Arg->DependentNodeOrdinalMask = 0;
    return STATUS_SUCCESS;
}
