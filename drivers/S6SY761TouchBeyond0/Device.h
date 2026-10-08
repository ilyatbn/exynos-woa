/*++

Copyright (C) Microsoft Corporation, All Rights Reserved

Module Name:

    vhidmini.h

Abstract:

    This module contains the type definitions for the driver

Environment:

    Windows Driver Framework (WDF)

--*/

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#include <ntddk.h>
#include <wdm.h>
#include <ntstrsafe.h>

#include <wdf.h>

#include <hidport.h>  // located in $(DDK_INC_PATH)/wdm
#include <spb.h>

#include "common.h"

#define RESHUB_USE_HELPER_ROUTINES
#include "reshub.h"

#define STAR2_ALIVE_GPIO_PHYSICAL_ADDRESS  0x14050000ULL
#define STAR2_ALIVE_GPIO_LENGTH            0x00001000UL

#define STAR2_GPA1_CON                     0x040UL
#define STAR2_GPA1_PUD                     0x048UL
#define STAR2_EINT_CON                     0x704UL
#define STAR2_EINT_FILTER                  0x808UL
#define STAR2_EINT_MASK                    0x904UL
#define STAR2_EINT_PENDING                 0xA04UL

#define STAR2_GPA1_0_FUNCTION_MASK         0x0000000FUL
#define STAR2_GPA1_0_EINT_FUNCTION         0x0000000FUL
#define STAR2_GPA1_0_PULL_MASK             0x0000000FUL
#define STAR2_EINT8_TRIGGER_MASK           0x0000000FUL
#define STAR2_EINT8_LEVEL_LOW              0x00000000UL
#define STAR2_EINT8_FILTER_MASK            0x000000FFUL
#define STAR2_EINT8_FILTER_VALUE           0x000000C0UL
#define STAR2_EINT8_BIT                    0x00000001UL

#define S6SY761_MAX_CONTACTS               10
#define S6SY761_INPUT_REPORT_QUEUE_DEPTH   64

typedef UCHAR HID_REPORT_DESCRIPTOR, * PHID_REPORT_DESCRIPTOR;

typedef struct __declspec(align(2)) _S6SY761_INPUT_POINT
{
    BYTE  FingerState;
    BYTE  ContactIdentifier;
    BYTE  XLow;
    BYTE  XHigh;
    BYTE  YLow;
    BYTE  YHigh;
} inputpoint;

typedef struct __declspec(align(2)) _S6SY761_INPUT_REPORT
{
    BYTE  reportId;
    BYTE  points[S6SY761_MAX_CONTACTS * sizeof(inputpoint)];
    BYTE  DIG_TouchScreenScanTimeLow;
    BYTE  DIG_TouchScreenScanTimeHigh;
    BYTE  DIG_TouchScreenContactCount;
} inputReport54_t;

C_ASSERT(sizeof(inputpoint) == 6);
C_ASSERT(sizeof(inputReport54_t) == 64);

typedef struct _S6SY761_CONTACT_STATE
{
    BOOLEAN Active;
    BOOLEAN ReleasePending;
    USHORT  X;
    USHORT  Y;
} S6SY761_CONTACT_STATE;

DRIVER_INITIALIZE                   DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD           EvtDeviceAdd;
EVT_WDF_TIMER                       EvtTimerFunc;
EVT_WDF_OBJECT_CONTEXT_CLEANUP      EvtDriverCleanup;

EVT_WDF_DEVICE_PREPARE_HARDWARE      OnPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE      OnReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY              OnD0Entry;
EVT_WDF_DEVICE_D0_EXIT               OnD0Exit;

typedef struct _DEVICE_CONTEXT
{
    WDFDEVICE               Device;
    WDFQUEUE                DefaultQueue;
    WDFQUEUE                ManualQueue;
    HID_DEVICE_ATTRIBUTES   HidDeviceAttributes;
    BYTE                    DeviceData;
    HID_DESCRIPTOR          HidDescriptor;
    PHID_REPORT_DESCRIPTOR  ReportDescriptor;
    BOOLEAN                 ReadReportDescFromRegistry;

    LARGE_INTEGER           PeripheralId;
    WDFINTERRUPT            Interrupt;
    WDFIOTARGET             SpbController;
    PUCHAR                  AliveRegisters;
    ULONG                   AliveRegistersLength;
    WDFWAITLOCK             InputLock;
    S6SY761_CONTACT_STATE   Contacts[S6SY761_MAX_CONTACTS];
    inputReport54_t         InputReports[S6SY761_INPUT_REPORT_QUEUE_DEPTH];
    BOOLEAN                 InputReportIsMove[S6SY761_INPUT_REPORT_QUEUE_DEPTH];
    ULONG                   InputReportHead;
    ULONG                   InputReportTail;
    ULONG                   InputReportCount;
    volatile LONG           InputDrainActive;
    BOOLEAN                 InputStopping;
    volatile LONG           InputInterruptCount;
    volatile LONG           InputI2cErrorCount;
    volatile LONG           InputOverflowCount;
    volatile LONG           InputQueueFlushCount;
    volatile LONG           InputReportCoalesceCount;
    volatile LONG           InputReportCompleteCount;

    PVOID PoFxPowerSettingCallbackHandle1;
    PVOID PoFxPowerSettingCallbackHandle2;

} DEVICE_CONTEXT, * PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext);

typedef struct _QUEUE_CONTEXT
{
    WDFQUEUE                Queue;
    PDEVICE_CONTEXT         DeviceContext;
    UCHAR                   OutputReport;

} QUEUE_CONTEXT, * PQUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUE_CONTEXT, GetQueueContext);

NTSTATUS
QueueCreate(
    _In_  WDFDEVICE         Device,
    _Out_ WDFQUEUE* Queue
);

typedef struct _MANUAL_QUEUE_CONTEXT
{
    WDFQUEUE                Queue;
    PDEVICE_CONTEXT         DeviceContext;
    WDFTIMER                Timer;

} MANUAL_QUEUE_CONTEXT, * PMANUAL_QUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MANUAL_QUEUE_CONTEXT, GetManualQueueContext);

NTSTATUS
ManualQueueCreate(
    _In_  WDFDEVICE         Device,
    _Out_ WDFQUEUE* Queue
);

NTSTATUS
ReadReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request,
    _Always_(_Out_)
    BOOLEAN* CompleteRequest
);

NTSTATUS
WriteReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
);

NTSTATUS
GetFeature(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
);

NTSTATUS
SetFeature(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
);

NTSTATUS
GetInputReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
);

NTSTATUS
SetOutputReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
);

NTSTATUS
GetString(
    _In_  WDFREQUEST        Request
);

NTSTATUS
GetIndexedString(
    _In_  WDFREQUEST        Request
);

NTSTATUS
GetStringId(
    _In_  WDFREQUEST        Request,
    _Out_ ULONG* StringId,
    _Out_ ULONG* LanguageId
);

NTSTATUS
RequestCopyFromBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             SourceBuffer,
    _When_(NumBytesToCopyFrom == 0, __drv_reportError(NumBytesToCopyFrom cannot be zero))
    _In_  size_t            NumBytesToCopyFrom
);

NTSTATUS
RequestGetHidXferPacket_ToReadFromDevice(
    _In_  WDFREQUEST        Request,
    _Out_ HID_XFER_PACKET* Packet
);

NTSTATUS
RequestGetHidXferPacket_ToWriteToDevice(
    _In_  WDFREQUEST        Request,
    _Out_ HID_XFER_PACKET* Packet
);

BOOLEAN
OnInterruptIsr(
    _In_  WDFINTERRUPT FxInterrupt,
    _In_  ULONG        MessageID
);

NTSTATUS
SpbDeviceOpen(
    _In_  PDEVICE_CONTEXT  pDevice
);
VOID
SpbDeviceClose(
    _In_  PDEVICE_CONTEXT  pDevice
);
NTSTATUS
SpbDeviceWrite(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ PVOID pInputBuffer,
    _In_ size_t inputBufferLength
);
NTSTATUS
SpbDeviceWriteRead(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ PVOID pInputBuffer,
    _In_ PVOID pOutputBuffer,
    _In_ size_t inputBufferLength,
    _In_ size_t outputBufferLength
);

NTSTATUS
ReadDescriptorFromRegistry(
    WDFDEVICE Device
);
NTSTATUS
CheckRegistryForDescriptor(
    WDFDEVICE Device
);

//
// Misc definitions
//
#define CONTROL_FEATURE_REPORT_ID   0x54

//
// These are the device attributes returned by the mini driver in response
// to IOCTL_HID_GET_DEVICE_ATTRIBUTES.
//
#define HIDMINI_PID             0xFEED
#define HIDMINI_VID             0xDEED
#define HIDMINI_VERSION         0x0101
