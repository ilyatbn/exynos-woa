/*  CxData.cpp - Exynos9820WlanCx W3: worker side of the data path (notes/m12-wifi-wificx.md).

    rx: WlanMsg.c RxComplete -> CxRxFrame (worker, PASSIVE) copies the 802.3 frame into RxFifo and nudges the
        NetAdapter rx queue; CxAdapter.cpp's rx Advance copies FIFO frames into the OS buffers.
    tx: CxAdapter.cpp's tx Advance copies each NetAdapter packet into TxFifo and wakes the worker; CxTxDrain posts
        them on the msgbuf flowring (WlanMsgTx). A frame the flowring cannot take yet (no buffer / ring full) stays
        queued; without a flowring (not associated) it is dropped.
*/

#include "Cx.h"

_Use_decl_annotations_
BOOLEAN
CxFifoPut(PCX_FIFO F, const UCHAR *Frame, ULONG Len)
{
    KIRQL   Irql;
    BOOLEAN Ok = FALSE;

    if (Len == 0 || Len > CX_FRAME_MAX) {
        InterlockedIncrement((volatile LONG *)&F->Dropped);
        return FALSE;
    }
    KeAcquireSpinLock(&F->Lock, &Irql);
    if (F->Count < CX_FIFO_SLOTS) {
        ULONG Slot = (F->Head + F->Count) % CX_FIFO_SLOTS;

        RtlCopyMemory(F->Data[Slot], Frame, Len);
        F->Len[Slot] = Len;
        F->Count++;
        F->In++;
        Ok = TRUE;
    } else {
        F->Dropped++;
    }
    KeReleaseSpinLock(&F->Lock, Irql);
    return Ok;
}

/* WLAN_RX_FN: one received 802.3 frame (EAPOL included: the OS supplicant needs it), on the worker thread. */
_Use_decl_annotations_
VOID
CxRxFrame(PVOID Ctx, const UCHAR *Frame, ULONG Len)
{
    PCX_DEVICE     Dev = (PCX_DEVICE)Ctx;
    KIRQL          Irql;
    NETPACKETQUEUE Q = NULL;

    if (Dev == NULL || Dev->RxFifo == NULL) {
        return;
    }
    if (!CxFifoPut(Dev->RxFifo, Frame, Len)) {
        return;
    }
    KeAcquireSpinLock(&Dev->RxFifo->Lock, &Irql);
    if (Dev->RxNotify && Dev->RxQueue != NULL) {
        Dev->RxNotify = FALSE;   /* one-shot: NetAdapterCx re-arms through EvtSetNotificationEnabled */
        Q = Dev->RxQueue;
        NetRxQueueNotifyMoreReceivedPacketsAvailable(Q);
    }
    KeReleaseSpinLock(&Dev->RxFifo->Lock, Irql);
}

_Use_decl_annotations_
VOID
CxTxDrain(PCX_DEVICE Dev)
{
    PCX_FIFO F = Dev->TxFifo;
    UCHAR   *Frame;
    ULONG    Len, Rc;
    KIRQL    Irql;

    if (F == NULL) {
        return;
    }
    Frame = (UCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, CX_FRAME_MAX, CX_POOL_TAG);
    if (Frame == NULL) {
        return;
    }
    for (;;) {
        KeAcquireSpinLock(&F->Lock, &Irql);
        if (F->Count == 0) {
            KeReleaseSpinLock(&F->Lock, Irql);
            break;
        }
        Len = F->Len[F->Head];
        RtlCopyMemory(Frame, F->Data[F->Head], Len);
        KeReleaseSpinLock(&F->Lock, Irql);

        Rc = WlanMsgTx(&Dev->Msg, Dev->RunR, Frame, Len);
        if (Rc == TX_NO_BUFFER || Rc == TX_RING_FULL) {
            break;   /* keep it; retried on the next worker pass after tx completions were polled */
        }
        KeAcquireSpinLock(&F->Lock, &Irql);
        F->Head = (F->Head + 1) % CX_FIFO_SLOTS;
        F->Count--;
        if (Rc == TX_OK) {
            F->Out++;
        } else {
            F->Dropped++;
        }
        KeReleaseSpinLock(&F->Lock, Irql);
        if (Rc != TX_OK && Dev->TxLogged++ < 16) {
            CxLog(Dev, "[tx] frame of %lu B dropped: TX %lu (%s)\r\n", Len, Rc,
                  (Rc == TX_NO_FLOWRING) ? "not associated" : "bad length");
        }
    }
    ExFreePoolWithTag(Frame, CX_POOL_TAG);
}
