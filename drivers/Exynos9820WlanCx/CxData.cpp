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

/* cx5 (wifi-cx4 run: 4 EAPOL msg 1 frames delivered as Ethernet II, the OS supplicant never answered): the adapter
   is NdisMediumNative802_11 (INF *MediaType 16), whose receive path carries 802.11 data frames, not 802.3 (Native
   802.11 = the medium WDI/WiFiCx sits on; NET_PACKET_LAYER2_TYPE has an Ieee80211 value for it, packet.h). The
   firmware hands us 802.3, so it is re-framed: 802.11 data header FromDS (FC 08 02; A1 = DA, A2 = BSSID, A3 = SA,
   802.11-2016 9.3.2.1 address table), sequence 0, then RFC 1042 LLC/SNAP AA AA 03 00 00 00 + EtherType + payload.
   Protected (FC bit 14) is set once a pairwise key is installed: the firmware decrypted the frame, and a clear
   frame would be dropped by the OS's ExcludeUnencrypted rule (WDI_TLV_CONNECTION_SETTINGS) - UNVERIFIED.
   Parameters\RxFormat 1 restores the cx4 Ethernet II delivery for an A/B run. Returns the 802.11 length or 0. */
static ULONG
CxEthToDot11(PCX_DEVICE Dev, const UCHAR *E, ULONG Len, PUCHAR Out)
{
    if (Len < 14 || Len - 14 + 32 > CX_FRAME_MAX) {
        return 0;
    }
    Out[0] = 0x08;                                  /* type data, subtype 0 */
    Out[1] = (UCHAR)(0x02 | (Dev->KeysSet ? 0x40 : 0x00));   /* FromDS (+ Protected) */
    Out[2] = Out[3] = 0;                            /* duration */
    RtlCopyMemory(Out + 4, E, 6);                   /* A1 = DA */
    RtlCopyMemory(Out + 10, Dev->Bssid, 6);         /* A2 = BSSID */
    RtlCopyMemory(Out + 16, E + 6, 6);              /* A3 = SA */
    Out[22] = Out[23] = 0;                          /* sequence control */
    Out[24] = 0xAA; Out[25] = 0xAA; Out[26] = 0x03; Out[27] = Out[28] = Out[29] = 0;
    RtlCopyMemory(Out + 30, E + 12, Len - 12);      /* EtherType + payload */
    return Len - 14 + 32;
}

/* WLAN_RX_FN: one received 802.3 frame (EAPOL included: the OS supplicant needs it), on the worker thread. */
_Use_decl_annotations_
VOID
CxRxFrame(PVOID Ctx, const UCHAR *Frame, ULONG Len)
{
    PCX_DEVICE     Dev = (PCX_DEVICE)Ctx;
    KIRQL          Irql;
    NETPACKETQUEUE Q = NULL;
    UCHAR          Dot11[CX_FRAME_MAX];

    if (Dev == NULL || Dev->RxFifo == NULL) {
        return;
    }
    if (Dev->RxDot11) {
        Len = CxEthToDot11(Dev, Frame, Len, Dot11);
        if (Len == 0) {
            InterlockedIncrement((volatile LONG *)&Dev->RxFifo->Dropped);
            return;
        }
        Frame = Dot11;
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
