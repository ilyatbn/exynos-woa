/*  CxAdapter.cpp - Exynos9820WlanCx: the station NetAdapter and its tx/rx queues (notes/m12-wifi-wificx.md).

    EvtWifiDeviceCreateAdapter (WiFiCx sample device.cpp:34-94, adapter.cpp:22-63): station type only,
    datapath callbacks, NetAdapterCreate, WifiAdapterInitialize, link-layer/datapath capabilities, MAC =
    the firmware's cur_etheraddr, NetAdapterStart.
    W1: the queues exist but carry no traffic (tx packets are completed as dropped, rx never fills).
*/

#include "Cx.h"

#define CX_MTU              1500        /* Ethernet payload MTU on the NetAdapter */
#define CX_RX_BUFFER_SIZE   2048        /* one rx frame per OS buffer (msgbuf rx buffers are 2048 B, WlanMsg.c) */
#define CX_LINK_SPEED       54000000ull /* placeholder link speed (bit/s) until W4 reports the real rate */

typedef struct _CX_QUEUE {
    PCX_DEVICE                 Dev;
    NET_RING_COLLECTION const *Rings;
    ULONG                      Dropped;
} CX_QUEUE, *PCX_QUEUE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CX_QUEUE, CxGetQueue)

/* ---------------------------------------------------------------- tx (W1: drop) */

static VOID
CxTxAdvance(NETPACKETQUEUE Queue)
{
    PCX_QUEUE Q  = CxGetQueue(Queue);
    NET_RING *Pr = NetRingCollectionGetPacketRing(Q->Rings);
    NET_RING *Fr = NetRingCollectionGetFragmentRing(Q->Rings);
    UINT32    I;

    for (I = Pr->BeginIndex; I != Pr->EndIndex; I = NetRingIncrementIndex(Pr, I)) {
        Q->Dropped++;
    }
    Pr->BeginIndex = Pr->EndIndex;
    Fr->BeginIndex = Fr->EndIndex;
}

static VOID
CxTxSetNotification(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Enabled);
}

static VOID
CxTxCancel(NETPACKETQUEUE Queue)
{
    CxTxAdvance(Queue);
}

/* ---------------------------------------------------------------- rx (W1: nothing received) */

static VOID
CxRxAdvance(NETPACKETQUEUE Queue)
{
    UNREFERENCED_PARAMETER(Queue);
}

static VOID
CxRxSetNotification(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Enabled);
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

/* ---------------------------------------------------------------- queue creation */

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
    return STATUS_SUCCESS;
}

static NTSTATUS
CxCreateRxQueue(NETADAPTER Adapter, NETRXQUEUE_INIT *Init)
{
    WDF_OBJECT_ATTRIBUTES   Attr;
    NET_PACKET_QUEUE_CONFIG Cfg;
    NETPACKETQUEUE          Queue;
    NTSTATUS                Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, CX_QUEUE);
    NET_PACKET_QUEUE_CONFIG_INIT(&Cfg, CxRxAdvance, CxRxSetNotification, CxRxCancel);
    Status = NetRxQueueCreate(Init, &Attr, &Cfg, &Queue);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    CxGetQueue(Queue)->Dev   = CxGetAdapter(Adapter)->Dev;
    CxGetQueue(Queue)->Rings = NetRxQueueGetRingCollection(Queue);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- adapter */

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
