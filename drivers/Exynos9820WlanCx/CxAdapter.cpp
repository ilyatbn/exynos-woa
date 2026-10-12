/*  CxAdapter.cpp - Exynos9820WlanCx: the station NetAdapter and its tx/rx queues (notes/m12-wifi-wificx.md).

    EvtWifiDeviceCreateAdapter (WiFiCx sample device.cpp:34-94, adapter.cpp:22-63): station type only,
    datapath callbacks, NetAdapterCreate, WifiAdapterInitialize, link-layer/datapath capabilities, MAC =
    the firmware's cur_etheraddr, NetAdapterStart.
    W3: tx packets are copied into the device's TxFifo (sent by the worker), received frames come from RxFifo.
*/

#include "Cx.h"

#define CX_MTU              1500        /* Ethernet payload MTU on the NetAdapter */
#define CX_RX_BUFFER_SIZE   2048        /* one rx frame per OS buffer (msgbuf rx buffers are 2048 B, WlanMsg.c) */
#define CX_LINK_SPEED       54000000ull /* placeholder link speed (bit/s) until W4 reports the real rate */

typedef struct _CX_QUEUE {
    PCX_DEVICE                 Dev;
    NET_RING_COLLECTION const *Rings;
    NET_EXTENSION              Va;          /* fragment virtual addresses */
    ULONG                      Dropped;
} CX_QUEUE, *PCX_QUEUE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CX_QUEUE, CxGetQueue)

/* ---------------------------------------------------------------- tx */

/* IEEE 802.11 data frame (FromDS 0 / ToDS 1, as a STA sends it) + LLC/SNAP -> 802.3: DA = addr3, SA = addr2,
   EtherType from the SNAP header (802.11-2016 9.3.2.1 address table, RFC 1042 encapsulation AA AA 03 00 00 00).
   Only used if NetAdapterCx hands us Layer2Type IEEE 802.11 frames, which is UNVERIFIED for WiFiCx (the run log
   records the layout of the first tx packets). Returns the 802.3 length or 0. */
static ULONG
CxDot11ToEth(_Inout_updates_bytes_(Len) PUCHAR F, _In_ ULONG Len)
{
    ULONG Hdr = 24;

    if (Len < 24 || (F[0] & 0x0C) != 0x08) {               /* frame control type = data */
        return 0;
    }
    if ((F[0] & 0x80) != 0) {                             /* QoS data subtype: + QoS control */
        Hdr += 2;
    }
    if (Len < Hdr + 8 || F[Hdr] != 0xAA || F[Hdr + 1] != 0xAA || F[Hdr + 2] != 0x03 || F[Hdr + 3] != 0 ||
        F[Hdr + 4] != 0 || F[Hdr + 5] != 0) {
        return 0;
    }
    {
        UCHAR Da[6], Sa[6];

        RtlCopyMemory(Da, F + 16, 6);
        RtlCopyMemory(Sa, F + 10, 6);
        RtlMoveMemory(F + 12, F + Hdr + 6, Len - Hdr - 6);   /* EtherType + payload */
        RtlCopyMemory(F, Da, 6);
        RtlCopyMemory(F + 6, Sa, 6);
    }
    return Len - Hdr - 6 + 12;
}

/* Copy every packet the OS posted into TxFifo (gathering its fragments) and return it at once; the worker sends.
   A packet that does not fit (FIFO full, too long, unusable layout) is dropped and counted. */
static VOID
CxTxAdvance(NETPACKETQUEUE Queue)
{
    PCX_QUEUE  Q   = CxGetQueue(Queue);
    PCX_DEVICE Dev = Q->Dev;
    NET_RING  *Pr  = NetRingCollectionGetPacketRing(Q->Rings);
    NET_RING  *Fr  = NetRingCollectionGetFragmentRing(Q->Rings);
    UCHAR      Frame[CX_FRAME_MAX];
    BOOLEAN    Any = FALSE;

    while (Pr->BeginIndex != Pr->EndIndex) {
        NET_PACKET *P   = NetRingGetPacketAtIndex(Pr, Pr->BeginIndex);
        UINT32      Fi  = P->FragmentIndex;
        ULONG       Len = 0, K;
        BOOLEAN     Ok  = !P->Ignore;

        for (K = 0; K < P->FragmentCount; K++) {
            NET_FRAGMENT const *Fg = NetRingGetFragmentAtIndex(Fr, Fi);
            PUCHAR              Va = (PUCHAR)NetExtensionGetFragmentVirtualAddress(&Q->Va, Fi)->VirtualAddress;

            if (Ok && Len + Fg->ValidLength <= sizeof(Frame)) {
                RtlCopyMemory(Frame + Len, Va + Fg->Offset, (SIZE_T)Fg->ValidLength);
                Len += (ULONG)Fg->ValidLength;
            } else {
                Ok = FALSE;
            }
            Fi = NetRingIncrementIndex(Fr, Fi);
        }
        if (Ok && P->Layout.Layer2Type == NetPacketLayer2TypeIeee80211) {
            Len = CxDot11ToEth(Frame, Len);
            Ok  = (Len != 0);
        }
        if (Dev->TxLayoutLog < 8) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "Exynos9820WlanCx: tx packet layer2 type %u hdr %u, %lu B, %02X %02X %02X %02X %02X %02X %02X %02X "
                       "%02X %02X %02X %02X %02X %02X\n", P->Layout.Layer2Type, P->Layout.Layer2HeaderLength, Len,
                       Frame[0], Frame[1], Frame[2], Frame[3], Frame[4], Frame[5], Frame[6], Frame[7], Frame[8],
                       Frame[9], Frame[10], Frame[11], Frame[12], Frame[13]);
            InterlockedIncrement((volatile LONG *)&Dev->TxLayoutLog);   /* the worker logs the latest one too */
            Dev->TxLayout[0] = P->Layout.Layer2Type;
            RtlCopyMemory(Dev->TxLayout + 1, Frame, 14);
        }
        if (!Ok || Len < 14 || !CxFifoPut(Dev->TxFifo, Frame, Len)) {
            Q->Dropped++;
        } else {
            Any = TRUE;
        }
        Fr->BeginIndex = Fi;
        Pr->BeginIndex = NetRingIncrementIndex(Pr, Pr->BeginIndex);
    }
    if (Any) {
        CxWake(Dev);
    }
}

static VOID
CxTxSetNotification(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    /* every packet is returned inside Advance, so there is never a completion to announce later */
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Enabled);
}

static VOID
CxTxCancel(NETPACKETQUEUE Queue)
{
    PCX_QUEUE Q  = CxGetQueue(Queue);
    NET_RING *Pr = NetRingCollectionGetPacketRing(Q->Rings);
    NET_RING *Fr = NetRingCollectionGetFragmentRing(Q->Rings);

    Pr->BeginIndex = Pr->EndIndex;
    Fr->BeginIndex = Fr->EndIndex;
}

/* ---------------------------------------------------------------- rx */

/* Fill OS packets from RxFifo: one frame = one packet = one fragment (system-managed 2 KB buffers). */
static VOID
CxRxAdvance(NETPACKETQUEUE Queue)
{
    PCX_QUEUE  Q   = CxGetQueue(Queue);
    PCX_DEVICE Dev = Q->Dev;
    PCX_FIFO   F   = Dev->RxFifo;
    NET_RING  *Pr  = NetRingCollectionGetPacketRing(Q->Rings);
    NET_RING  *Fr  = NetRingCollectionGetFragmentRing(Q->Rings);
    KIRQL      Irql;

    if (F == NULL) {
        return;
    }
    KeAcquireSpinLock(&F->Lock, &Irql);
    while (F->Count != 0 && Pr->BeginIndex != Pr->EndIndex && Fr->BeginIndex != Fr->EndIndex) {
        UINT32        Fi  = Fr->BeginIndex;
        NET_FRAGMENT *Fg  = NetRingGetFragmentAtIndex(Fr, Fi);
        NET_PACKET   *P   = NetRingGetPacketAtIndex(Pr, Pr->BeginIndex);
        ULONG         Len = F->Len[F->Head];
        PUCHAR        Va  = (PUCHAR)NetExtensionGetFragmentVirtualAddress(&Q->Va, Fi)->VirtualAddress;

        if (Fg->Offset + Len <= Fg->Capacity) {
            RtlCopyMemory(Va + Fg->Offset, F->Data[F->Head], Len);
            Fg->ValidLength  = Len;
            P->FragmentIndex = Fi;
            P->FragmentCount = 1;
            RtlZeroMemory(&P->Layout, sizeof(P->Layout));
            P->Layout.Layer2Type = NetPacketLayer2TypeEthernet;
            P->Ignore = 0;
            F->Out++;
        } else {
            Fg->ValidLength  = 0;
            P->FragmentIndex = Fi;
            P->FragmentCount = 1;
            P->Ignore        = 1;
            F->Dropped++;
        }
        F->Head = (F->Head + 1) % CX_FIFO_SLOTS;
        F->Count--;
        Fr->BeginIndex = NetRingIncrementIndex(Fr, Fi);
        Pr->BeginIndex = NetRingIncrementIndex(Pr, Pr->BeginIndex);
    }
    KeReleaseSpinLock(&F->Lock, Irql);
}

static VOID
CxRxSetNotification(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    PCX_DEVICE Dev = CxGetQueue(Queue)->Dev;
    KIRQL      Irql;

    if (Dev->RxFifo == NULL) {
        return;
    }
    KeAcquireSpinLock(&Dev->RxFifo->Lock, &Irql);
    Dev->RxNotify = Enabled;
    if (Enabled && Dev->RxFifo->Count != 0) {
        Dev->RxNotify = FALSE;
        NetRxQueueNotifyMoreReceivedPacketsAvailable(Queue);
    }
    KeReleaseSpinLock(&Dev->RxFifo->Lock, Irql);
}

/* Return every packet the OS gave us, marked Ignore (NetAdapterCx rx cancel contract). */
static VOID
CxRxCancel(NETPACKETQUEUE Queue)
{
    PCX_QUEUE Q  = CxGetQueue(Queue);
    NET_RING *Pr = NetRingCollectionGetPacketRing(Q->Rings);
    NET_RING *Fr = NetRingCollectionGetFragmentRing(Q->Rings);
    UINT32    I;

    for (I = Pr->BeginIndex; I != Pr->EndIndex; I = NetRingIncrementIndex(Pr, I)) {
        NetRingGetPacketAtIndex(Pr, I)->Ignore = 1;
    }
    Pr->BeginIndex = Pr->EndIndex;
    Fr->BeginIndex = Fr->EndIndex;
}

/* The rx queue object is going away: no more notifications to it. */
static VOID
CxRxDestroy(WDFOBJECT Object)
{
    PCX_DEVICE Dev = CxGetQueue((NETPACKETQUEUE)Object)->Dev;
    KIRQL      Irql;

    if (Dev != NULL && Dev->RxFifo != NULL) {
        KeAcquireSpinLock(&Dev->RxFifo->Lock, &Irql);
        if (Dev->RxQueue == (NETPACKETQUEUE)Object) {
            Dev->RxQueue  = NULL;
            Dev->RxNotify = FALSE;
        }
        KeReleaseSpinLock(&Dev->RxFifo->Lock, Irql);
    }
}

/* ---------------------------------------------------------------- queue creation */

static NTSTATUS
CxQueueVa(NETPACKETQUEUE Queue, BOOLEAN Rx)
{
    NET_EXTENSION_QUERY Query;
    PCX_QUEUE           Q = CxGetQueue(Queue);

    NET_EXTENSION_QUERY_INIT(&Query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    if (Rx) {
        NetRxQueueGetExtension(Queue, &Query, &Q->Va);
    } else {
        NetTxQueueGetExtension(Queue, &Query, &Q->Va);
    }
    return Q->Va.Enabled ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

static NTSTATUS
CxCreateTxQueue(NETADAPTER Adapter, NETTXQUEUE_INIT *Init)
{
    WDF_OBJECT_ATTRIBUTES   Attr;
    NET_PACKET_QUEUE_CONFIG Cfg;
    NETPACKETQUEUE          Queue;
    NTSTATUS                Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, CX_QUEUE);
    NET_PACKET_QUEUE_CONFIG_INIT(&Cfg, CxTxAdvance, CxTxSetNotification, CxTxCancel);
    Status = NetTxQueueCreate(Init, &Attr, &Cfg, &Queue);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    CxGetQueue(Queue)->Dev   = CxGetAdapter(Adapter)->Dev;
    CxGetQueue(Queue)->Rings = NetTxQueueGetRingCollection(Queue);
    return CxQueueVa(Queue, FALSE);
}

static NTSTATUS
CxCreateRxQueue(NETADAPTER Adapter, NETRXQUEUE_INIT *Init)
{
    WDF_OBJECT_ATTRIBUTES   Attr;
    NET_PACKET_QUEUE_CONFIG Cfg;
    NETPACKETQUEUE          Queue;
    PCX_DEVICE              Dev = CxGetAdapter(Adapter)->Dev;
    NTSTATUS                Status;
    KIRQL                   Irql;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, CX_QUEUE);
    Attr.EvtDestroyCallback = CxRxDestroy;
    NET_PACKET_QUEUE_CONFIG_INIT(&Cfg, CxRxAdvance, CxRxSetNotification, CxRxCancel);
    Status = NetRxQueueCreate(Init, &Attr, &Cfg, &Queue);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    CxGetQueue(Queue)->Dev   = Dev;
    CxGetQueue(Queue)->Rings = NetRxQueueGetRingCollection(Queue);
    Status = CxQueueVa(Queue, TRUE);
    if (NT_SUCCESS(Status) && Dev->RxFifo != NULL) {
        KeAcquireSpinLock(&Dev->RxFifo->Lock, &Irql);
        Dev->RxQueue  = Queue;
        Dev->RxNotify = FALSE;
        KeReleaseSpinLock(&Dev->RxFifo->Lock, Irql);
    }
    return Status;
}

/* ---------------------------------------------------------------- adapter */

/* The adapter object goes away with the device stack (or on a WiFiCx re-create): forget it so a later
   EvtWifiDeviceCreateAdapter is accepted again. */
static VOID
CxAdapterCleanup(WDFOBJECT Object)
{
    PCX_DEVICE Dev = CxGetAdapter((NETADAPTER)Object)->Dev;

    if (Dev != NULL && Dev->Adapter == (NETADAPTER)Object) {
        Dev->Adapter = NULL;
    }
}

_Use_decl_annotations_
NTSTATUS
CxEvtCreateAdapter(WDFDEVICE Device, NETADAPTER_INIT *AdapterInit)
{
    PCX_DEVICE                         Dev = CxGetDevice(Device);
    NET_ADAPTER_DATAPATH_CALLBACKS     Dp;
    WDF_OBJECT_ATTRIBUTES              Attr;
    NETADAPTER                         Adapter;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES Ll;
    NET_ADAPTER_TX_CAPABILITIES        Tx;
    NET_ADAPTER_RX_CAPABILITIES        Rx;
    NET_ADAPTER_LINK_LAYER_ADDRESS     Mac;
    NTSTATUS                           Status;

    if (WifiAdapterInitGetType(AdapterInit) != WIFI_ADAPTER_EXTENSIBLE_STATION) {
        return STATUS_NOT_SUPPORTED;   /* no Wi-Fi Direct roles */
    }
    if (Dev->Adapter != NULL || !Dev->Msg.MacValid) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&Dp, CxCreateTxQueue, CxCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(AdapterInit, &Dp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, CX_ADAPTER);
    Attr.EvtCleanupCallback = CxAdapterCleanup;
    Status = NetAdapterCreate(AdapterInit, &Attr, &Adapter);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    CxGetAdapter(Adapter)->Dev = Dev;
    Status = WifiAdapterInitialize(Adapter);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&Ll, CX_LINK_SPEED, CX_LINK_SPEED);
    NetAdapterSetLinkLayerCapabilities(Adapter, &Ll);
    NetAdapterSetLinkLayerMtuSize(Adapter, CX_MTU);
    NET_ADAPTER_TX_CAPABILITIES_INIT(&Tx, 1);
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&Rx, CX_RX_BUFFER_SIZE, 1);
    NetAdapterSetDataPathCapabilities(Adapter, &Tx, &Rx);
    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&Mac, 6, Dev->Msg.Mac);
    NetAdapterSetPermanentLinkLayerAddress(Adapter, &Mac);
    NetAdapterSetCurrentLinkLayerAddress(Adapter, &Mac);

    Status = NetAdapterStart(Adapter);
    if (NT_SUCCESS(Status)) {
        Dev->Adapter = Adapter;
        Dev->PortId  = (WDI_PORT_ID)WifiAdapterGetPortId(Adapter);   /* port of unsolicited indications */
    }
    return Status;
}
