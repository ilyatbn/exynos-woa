/*  Cx.h - Exynos9820WlanCx: WiFiCx 1.0 + NetAdapterCx 2.2 client for the S10e BCM4375 (notes/m12-wifi-wificx.md).

    One worker thread owns the msgbuf state of the shared C code (WlanMsg.c is PASSIVE_LEVEL, single owner): it runs
    the queued WiFiCx commands, polls the D2H rings and raises unsolicited indications. EvtWifiDeviceSendCommand
    (<= DISPATCH_LEVEL) only queues.
*/
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <ntintsafe.h>
#include <netadaptercx.h>
#include <netiodef.h>
#include <net/virtualaddress.h>     /* NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_* */
#include <wificx.h>
#include <TlvGeneratorParser.hpp>

extern "C" {
#include "WlanDl.h"
#include "WlanMsg.h"
}

#define CX_POOL_TAG             'xClW'
#define CX_IDLE_WAIT_MS         50          /* worker poll period while not associated (host choice) */
#define CX_LINK_WAIT_MS         5           /* while associated: rx latency bound (host choice, timer resolution) */
#define CX_LOG_ROTATE           (WLAN_REPORT_MAX * 3 / 4)

/* W2 scan cache: one entry per BSSID from the escan records of the current WDI_TASK_SCAN */
#define CX_MAX_BSS              64          /* = SCAN_MAX_BSS of WlanMsg.c */
#define CX_BSS_IES_MAX          1400        /* host choice: IEs kept per BSS (longer beacons are cut, logged) */
#define CX_BSS_PER_INDICATION   16          /* host choice: BSS entries per WDI_INDICATION_BSS_ENTRY_LIST */

typedef struct _CX_BSS {
    UCHAR           Bssid[6];
    USHORT          BeaconPeriod;
    USHORT          Capability;
    USHORT          Chanspec;
    UCHAR           Channel;            /* control channel (wl_bss_info ctl_ch) */
    SHORT           Rssi;
    BOOLEAN         Cut;                /* IEs longer than CX_BSS_IES_MAX */
    ULONG           IesLen;
    UCHAR           Ies[CX_BSS_IES_MAX];
} CX_BSS, *PCX_BSS;

/* W3 data path: copy FIFOs between the NetAdapter queues and the worker (msgbuf owner). Frames are 802.3. */
#define CX_FRAME_MAX            2048        /* = DATA_BUF_SIZE of WlanMsg.c (one rx/tx buffer) */
#define CX_FIFO_SLOTS           64          /* host choice, per direction (64 x 2 KB) */

typedef struct _CX_FIFO {
    KSPIN_LOCK      Lock;
    ULONG           Head;               /* next slot to read */
    ULONG           Count;
    ULONG           Len[CX_FIFO_SLOTS];
    UCHAR           Data[CX_FIFO_SLOTS][CX_FRAME_MAX];
    ULONG           In, Out, Dropped;   /* counters for the run log */
} CX_FIFO, *PCX_FIFO;

typedef struct _CX_CMD {
    LIST_ENTRY      Link;
    WIFIREQUEST     Request;
    UINT16          MessageId;
} CX_CMD, *PCX_CMD;

typedef struct _CX_DEVICE {
    /* NetAdapterCx expects the first field of the WDFDEVICE context to point at WDF_TRIAGE_INFO (WiFiCx sample
       device.h WIFI_IHV_DEVICE_CONTEXT, static_assert offset 0). */
    PVOID               WdfTriageInfoPtr;
    WDFDEVICE           Device;
    TLV_CONTEXT         Tlv;
    ULONG               OsWdiVersion;
    WDFDMAENABLER       DmaEnabler;
    WLAN_MSG_CTX        Msg;            /* resident firmware (WlanMsg.c), owned by the worker after PrepareHardware */
    volatile UCHAR     *Cfg;            /* EP config window, kept while Msg.Resident */
    BOOLEAN             CapsSet;
    NETADAPTER          Adapter;        /* the station adapter (EvtWifiDeviceCreateAdapter) */
    WDI_PORT_ID         PortId;
    BOOLEAN             RadioOn;
    PCX_BSS             Bss;            /* CX_MAX_BSS entries (WDF memory, child of the device) */
    ULONG               BssN;
    ULONG               BssRecords;     /* escan records seen in the current scan */
    BOOLEAN             BssCached;      /* the list being reported is the previous scan's (escan refused) */

    /* W3 data path */
    PCX_FIFO            RxFifo;         /* worker (RxFn) -> rx queue */
    PCX_FIFO            TxFifo;         /* tx queue -> worker (WlanMsgTx) */
    NETPACKETQUEUE      RxQueue;        /* under RxFifo->Lock; NULL when the queue is gone */
    BOOLEAN             RxNotify;       /* under RxFifo->Lock: NetAdapterCx asked to be told about new frames */
    ULONG               TxLogged;
    volatile LONG       TxLayoutLog;    /* tx packet layouts recorded (first 8), logged by the worker */
    LONG                TxLayoutSeen;
    UCHAR               TxLayout[15];   /* Layer2Type + first 14 bytes of the latest recorded tx packet */
    BOOLEAN             Linked;         /* W4: associated (worker polls every CX_LINK_WAIT_MS) */
    UCHAR               Bssid[6];       /* W4: the BSS of the current / last connect */
    BOOLEAN             KeysSet;        /* cx5: a pairwise key is installed (rx frames are marked Protected) */
    BOOLEAN             RxDot11;        /* cx5: rx frames go up as 802.11 data frames (Parameters\RxFormat 0) */
    volatile LONG       RxLayoutLog;    /* rx packets recorded as delivered (first 8), logged by the worker */
    LONG                RxLayoutSeen;
    UCHAR               RxLayout[4 + 32];   /* Layer2Type, Layer2HeaderLength, FragmentCount, Ignore + 32 bytes */
    ULONG               RxLayoutLen;

    /* worker */
    PKTHREAD            Thread;
    KEVENT              Wake;
    volatile LONG       Stop;
    KSPIN_LOCK          CmdLock;
    LIST_ENTRY          CmdList;
    ULONG               Commands;
    PWLAN_REPORT        RunR;           /* runtime log (Exynos9820WlanCx-run.txt, rotated) */
    ULONG               RunGen;
    BOOLEAN             RunDirty;
} CX_DEVICE, *PCX_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CX_DEVICE, CxGetDevice)

typedef struct _CX_ADAPTER {
    PCX_DEVICE          Dev;
} CX_ADAPTER, *PCX_ADAPTER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CX_ADAPTER, CxGetAdapter)

/* CxDevice.cpp */
VOID CxLog(_Inout_ PCX_DEVICE Dev, _In_z_ _Printf_format_string_ PCSTR Fmt, ...);
VOID CxLogFlush(_Inout_ PCX_DEVICE Dev);
VOID CxWake(_Inout_ PCX_DEVICE Dev);

/* CxWifi.cpp */
EVT_WIFI_DEVICE_SEND_COMMAND        CxEvtSendCommand;
EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE CxEvtCreateWifiDirectDevice;
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS CxSetCapabilities(_Inout_ PCX_DEVICE Dev);
_IRQL_requires_(PASSIVE_LEVEL) VOID CxRunCommand(_Inout_ PCX_DEVICE Dev, _In_ PCX_CMD Cmd);
_IRQL_requires_(PASSIVE_LEVEL) VOID CxFailCommand(_Inout_ PCX_DEVICE Dev, _In_ PCX_CMD Cmd, _In_ NTSTATUS Status);

/* CxScan.cpp */
WLAN_BSS_FN CxBssRecord;
_IRQL_requires_(PASSIVE_LEVEL) VOID CxTaskScan(_Inout_ PCX_DEVICE Dev, _In_ PCX_CMD Cmd, _In_ const WDI_MESSAGE_HEADER *H,
                                               _In_reads_bytes_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen);
_IRQL_requires_(PASSIVE_LEVEL) PCX_BSS CxFindBss(_In_ PCX_DEVICE Dev, _In_reads_(6) const UCHAR *Bssid);

/* CxWifi.cpp (shared with the other command files) */
VOID CxIndicate(_Inout_ PCX_DEVICE Dev, _In_ UINT16 MessageId, _In_opt_ const WDI_MESSAGE_HEADER *Req,
                _In_ NDIS_STATUS Status, _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen);
VOID CxM3(_Inout_ PCX_DEVICE Dev, _In_ PCX_CMD Cmd, _In_ NTSTATUS Status);

/* CxData.cpp */
BOOLEAN CxFifoPut(_Inout_ PCX_FIFO F, _In_reads_bytes_(Len) const UCHAR *Frame, _In_ ULONG Len);
WLAN_RX_FN CxRxFrame;
_IRQL_requires_(PASSIVE_LEVEL) VOID CxTxDrain(_Inout_ PCX_DEVICE Dev);

/* CxConnect.cpp */
WLAN_LINK_FN CxLinkLost;
_IRQL_requires_(PASSIVE_LEVEL) VOID CxUpdateRate(_Inout_ PCX_DEVICE Dev);
_IRQL_requires_(PASSIVE_LEVEL) VOID CxSetLinked(_Inout_ PCX_DEVICE Dev, _In_ BOOLEAN Linked);
_IRQL_requires_(PASSIVE_LEVEL) VOID CxDisconnect(_Inout_ PCX_DEVICE Dev, _In_ WDI_ASSOC_STATUS Reason);
_IRQL_requires_(PASSIVE_LEVEL) VOID CxTaskConnect(_Inout_ PCX_DEVICE Dev, _In_ PCX_CMD Cmd, _In_ const WDI_MESSAGE_HEADER *H,
                                                  _In_reads_bytes_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen);
_IRQL_requires_(PASSIVE_LEVEL) VOID CxTaskDisconnect(_Inout_ PCX_DEVICE Dev, _In_ PCX_CMD Cmd, _In_ const WDI_MESSAGE_HEADER *H);
_IRQL_requires_(PASSIVE_LEVEL) VOID CxSetAddKeys(_Inout_ PCX_DEVICE Dev, _In_ PCX_CMD Cmd,
                                                 _In_reads_bytes_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen);

/* CxAdapter.cpp */
EVT_WIFI_DEVICE_CREATE_ADAPTER      CxEvtCreateAdapter;
