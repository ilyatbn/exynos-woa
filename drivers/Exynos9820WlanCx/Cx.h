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

/* CxAdapter.cpp */
EVT_WIFI_DEVICE_CREATE_ADAPTER      CxEvtCreateAdapter;
