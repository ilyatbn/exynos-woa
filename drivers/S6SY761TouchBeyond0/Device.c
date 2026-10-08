/*++

Copyright (C) Microsoft Corporation, All Rights Reserved.

SPDX-Licence-Identifier: GPL-2.0
Copyright (c) 2017  Samsung Electronics Co., Ltd.
Copyright (c) 2017  Andi Shyti <andi@etezian.org>
					Andi Shyti <andi.shyti@samsung.com>

Copyright (c) 2022 - 2023  Morc - Richard Gráčik (TheMorc)
Based on TheMorc's S6SY761TouchBeyond0: https://github.com/TheMorc/S6SY761TouchBeyond0

Module Name:


	S6SY761TouchBeyond0.c

Abstract:F

	This module contains the implementation of the driver

Environment:

	Windows Driver Framework (WDF)

--*/

#include "Device.h"


NTSTATUS
RequestGetHidXferPacket_ToReadFromDevice(
	_In_  WDFREQUEST        Request,
	_Out_ HID_XFER_PACKET* Packet
)
{
	NTSTATUS                status;
	WDF_REQUEST_PARAMETERS  params;

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET)) {
		status = STATUS_BUFFER_TOO_SMALL;
		KdPrint(("RequestGetHidXferPacket: invalid HID_XFER_PACKET\n"));
		return status;
	}

	RtlCopyMemory(Packet, WdfRequestWdmGetIrp(Request)->UserBuffer, sizeof(HID_XFER_PACKET));
	return STATUS_SUCCESS;
}

NTSTATUS
RequestGetHidXferPacket_ToWriteToDevice(
	_In_  WDFREQUEST        Request,
	_Out_ HID_XFER_PACKET* Packet
)
{
	NTSTATUS                status;
	WDF_REQUEST_PARAMETERS  params;

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET)) {
		status = STATUS_BUFFER_TOO_SMALL;
		KdPrint(("RequestGetHidXferPacket: invalid HID_XFER_PACKET\n"));
		return status;
	}

	RtlCopyMemory(Packet, WdfRequestWdmGetIrp(Request)->UserBuffer, sizeof(HID_XFER_PACKET));
	return STATUS_SUCCESS;
}


#define S6SY761_MASK_LEFT_EVENTS 0x3f
#define S6SY761_EVENT_SIZE  8
#define S6SY761_EVENT_COUNT  (S6SY761_MASK_LEFT_EVENTS + 1)
#define S6SY761_SPB_TIMEOUT_MS 250

C_ASSERT(S6SY761_EVENT_COUNT == 64);
C_ASSERT(S6SY761_INPUT_REPORT_QUEUE_DEPTH >= S6SY761_EVENT_COUNT);

#define S6SY761_MASK_TOUCH_STATE 0xc0
#define S6SY761_MASK_TID 0x3c
#define S6SY761_MASK_EID 0x03
#define S6SY761_MASK_X 0xf0
#define S6SY761_MASK_Y 0x0f

#define S6SY761_TS_NONE 0x00
#define S6SY761_TS_PRESS 0x01
#define S6SY761_TS_MOVE 0x02
#define S6SY761_TS_RELEASE 0x03

/* event id */
#define S6SY761_EVENT_ID_COORDINATE	0x00
#define S6SY761_EVENT_ID_STATUS		0x01
#define S6SY761_COORDINATE_MAX		0x0FFF

BYTE S6SY761_READ_ALL_EVENTS[1] = { 0x61 }; //read all events
BYTE S6SY761_READ_ONE_EVENT[1] = { 0x60 }; //read single event
BYTE S6SY761_CLEAR_EVENT_STACK[1] = { 0x62 }; //drain all last commands
BYTE S6SY761_SENSE_ON[1] = { 0x10 }; //enable sensing

UINT8 eventbuf[S6SY761_EVENT_SIZE * S6SY761_EVENT_COUNT];

ULONG XRevert = 0;
ULONG YRevert = 0;
ULONG XYExchange = 0;
ULONG XMin = 0;
ULONG XMax = S6SY761_COORDINATE_MAX;
ULONG YMin = 0;
ULONG YMax = S6SY761_COORDINATE_MAX;

static
VOID
RecordStartValues(
	_In_ WDFDEVICE Device,
	_In_ ULONG Stage,
	_In_ NTSTATUS Status
)
{
	WDFKEY key;
	UNICODE_STRING stageName;
	UNICODE_STRING statusName;
	NTSTATUS registryStatus;

	registryStatus = WdfDeviceOpenRegistryKey(
		Device,
		PLUGPLAY_REGKEY_DEVICE,
		KEY_SET_VALUE,
		WDF_NO_OBJECT_ATTRIBUTES,
		&key);
	if (!NT_SUCCESS(registryStatus))
	{
		return;
	}

	RtlInitUnicodeString(&stageName, L"TouchStartStage");
	RtlInitUnicodeString(&statusName, L"TouchStartStatus");
	WdfRegistryAssignULong(key, &stageName, Stage);
	WdfRegistryAssignULong(key, &statusName, (ULONG)Status);
	WdfRegistryClose(key);
}

static
VOID
RecordStartValue(
	_In_ WDFDEVICE Device,
	_In_ PCWSTR Name,
	_In_ ULONG Value
)
{
	WDFKEY key;
	UNICODE_STRING valueName;
	NTSTATUS registryStatus;

	registryStatus = WdfDeviceOpenRegistryKey(
		Device,
		PLUGPLAY_REGKEY_DEVICE,
		KEY_SET_VALUE,
		WDF_NO_OBJECT_ATTRIBUTES,
		&key);
	if (!NT_SUCCESS(registryStatus))
	{
		return;
	}

	RtlInitUnicodeString(&valueName, Name);
	WdfRegistryAssignULong(key, &valueName, Value);
	WdfRegistryClose(key);
}

static
VOID
TouchResetInputStateLocked(
	_Inout_ PDEVICE_CONTEXT DeviceContext
)
{
	RtlZeroMemory(DeviceContext->Contacts, sizeof(DeviceContext->Contacts));
	RtlZeroMemory(DeviceContext->InputReports, sizeof(DeviceContext->InputReports));
	RtlZeroMemory(DeviceContext->InputReportIsMove, sizeof(DeviceContext->InputReportIsMove));
	DeviceContext->InputReportHead = 0;
	DeviceContext->InputReportTail = 0;
	DeviceContext->InputReportCount = 0;
}

static
VOID
TouchInitializeReport(
	_Out_ inputReport54_t* Report
)
{
	ULONGLONG scanTime;

	RtlZeroMemory(Report, sizeof(*Report));
	Report->reportId = CONTROL_FEATURE_REPORT_ID;
	scanTime = KeQueryInterruptTime() / 1000ULL;
	Report->DIG_TouchScreenScanTimeLow = (BYTE)(scanTime & 0xff);
	Report->DIG_TouchScreenScanTimeHigh =
		(BYTE)((scanTime >> 8) & 0xff);
}

static
VOID
TouchAppendContact(
	_Inout_ inputReport54_t* Report,
	_In_ UCHAR ContactId,
	_In_ UCHAR FingerState,
	_In_ USHORT X,
	_In_ USHORT Y
)
{
	ULONG pointOffset;

	pointOffset =
		(ULONG)Report->DIG_TouchScreenContactCount * (ULONG)sizeof(inputpoint);
	Report->points[pointOffset + 0] = FingerState;
	Report->points[pointOffset + 1] = ContactId;
	Report->points[pointOffset + 2] = (BYTE)(X & 0xff);
	Report->points[pointOffset + 3] = (BYTE)((X >> 8) & 0x0f);
	Report->points[pointOffset + 4] = (BYTE)(Y & 0xff);
	Report->points[pointOffset + 5] = (BYTE)((Y >> 8) & 0x0f);
	Report->DIG_TouchScreenContactCount++;
}

static
BOOLEAN
TouchBuildCurrentReportLocked(
	_Inout_ PDEVICE_CONTEXT DeviceContext,
	_Out_ inputReport54_t* Report
)
{
	ULONG contactId;

	TouchInitializeReport(Report);

	for (contactId = 0; contactId < S6SY761_MAX_CONTACTS; contactId++)
	{
		S6SY761_CONTACT_STATE* contact = &DeviceContext->Contacts[contactId];

		if (contact->Active)
		{
			TouchAppendContact(
				Report,
				(UCHAR)contactId,
				0x01,
				contact->X,
				contact->Y);
		}
		else if (contact->ReleasePending)
		{
			TouchAppendContact(
				Report,
				(UCHAR)contactId,
				0x00,
				contact->X,
				contact->Y);
		}
	}

	return Report->DIG_TouchScreenContactCount != 0;
}

static
BOOLEAN
TouchBuildAllUpReportLocked(
	_Inout_ PDEVICE_CONTEXT DeviceContext,
	_Out_ inputReport54_t* Report
)
{
	ULONG contactId;

	TouchInitializeReport(Report);

	for (contactId = 0; contactId < S6SY761_MAX_CONTACTS; contactId++)
	{
		S6SY761_CONTACT_STATE* contact = &DeviceContext->Contacts[contactId];

		if (contact->Active || contact->ReleasePending)
		{
			TouchAppendContact(
				Report,
				(UCHAR)contactId,
				0x00,
				contact->X,
				contact->Y);
		}
	}

	RtlZeroMemory(DeviceContext->Contacts, sizeof(DeviceContext->Contacts));
	return Report->DIG_TouchScreenContactCount != 0;
}

static
BOOLEAN
TouchReportCanCoalesce(
	_In_ const inputReport54_t* Report,
	_Out_ PULONG ActiveContactMask
)
{
	ULONG activeContactMask = 0;
	ULONG pointIndex;
	ULONG pointCount;

	*ActiveContactMask = 0;
	pointCount = Report->DIG_TouchScreenContactCount;
	if ((pointCount == 0) || (pointCount > S6SY761_MAX_CONTACTS))
	{
		return FALSE;
	}

	for (pointIndex = 0; pointIndex < pointCount; pointIndex++)
	{
		ULONG pointOffset = pointIndex * (ULONG)sizeof(inputpoint);
		UCHAR fingerState = Report->points[pointOffset + 0];
		UCHAR contactId = Report->points[pointOffset + 1];
		ULONG contactBit;

		if (((fingerState & 0x01) == 0) ||
			(contactId >= S6SY761_MAX_CONTACTS))
		{
			return FALSE;
		}

		contactBit = 1UL << contactId;
		if ((activeContactMask & contactBit) != 0)
		{
			return FALSE;
		}

		activeContactMask |= contactBit;
	}

	*ActiveContactMask = activeContactMask;
	return TRUE;
}

static
BOOLEAN
TouchEnqueueReportLocked(
	_Inout_ PDEVICE_CONTEXT DeviceContext,
	_In_ const inputReport54_t* Report,
	_In_ BOOLEAN IsMove
)
{
	ULONG activeContactMask;

	if (IsMove && (DeviceContext->InputReportCount != 0) &&
		TouchReportCanCoalesce(Report, &activeContactMask))
	{
		ULONG tailIndex =
			(DeviceContext->InputReportTail +
			 S6SY761_INPUT_REPORT_QUEUE_DEPTH - 1) %
			S6SY761_INPUT_REPORT_QUEUE_DEPTH;
		ULONG queuedActiveContactMask;
		inputReport54_t* queuedReport =
			&DeviceContext->InputReports[tailIndex];

		// Keep the first down's position and scan time, even under backpressure.
		if (DeviceContext->InputReportIsMove[tailIndex] &&
			TouchReportCanCoalesce(
				queuedReport,
				&queuedActiveContactMask) &&
			(queuedActiveContactMask == activeContactMask))
		{
			RtlCopyMemory(
				queuedReport,
				Report,
				sizeof(*queuedReport));
			InterlockedIncrement(
				&DeviceContext->InputReportCoalesceCount);
			return TRUE;
		}
	}

	if (DeviceContext->InputReportCount == S6SY761_INPUT_REPORT_QUEUE_DEPTH)
	{
		return FALSE;
	}

	DeviceContext->InputReportIsMove[DeviceContext->InputReportTail] = IsMove;
	DeviceContext->InputReports[DeviceContext->InputReportTail] = *Report;
	DeviceContext->InputReportTail =
		(DeviceContext->InputReportTail + 1) %
		S6SY761_INPUT_REPORT_QUEUE_DEPTH;
	DeviceContext->InputReportCount++;
	return TRUE;
}

static
VOID
TouchPreserveQueuedReleasesLocked(
	_Inout_ PDEVICE_CONTEXT DeviceContext
)
{
	ULONG reportIndex;
	ULONG reportNumber;

	reportIndex = DeviceContext->InputReportHead;
	for (reportNumber = 0;
		reportNumber < DeviceContext->InputReportCount;
		reportNumber++)
	{
		const inputReport54_t* report =
			&DeviceContext->InputReports[reportIndex];
		ULONG pointIndex;
		ULONG pointCount;

		pointCount = min(
			(ULONG)report->DIG_TouchScreenContactCount,
			(ULONG)S6SY761_MAX_CONTACTS);
		for (pointIndex = 0; pointIndex < pointCount; pointIndex++)
		{
			ULONG pointOffset = pointIndex * (ULONG)sizeof(inputpoint);
			UCHAR fingerState = report->points[pointOffset + 0];
			UCHAR contactId = report->points[pointOffset + 1];

			if (((fingerState & 0x01) == 0) &&
				(contactId < S6SY761_MAX_CONTACTS) &&
				!DeviceContext->Contacts[contactId].Active)
			{
				DeviceContext->Contacts[contactId].ReleasePending = TRUE;
			}
		}

		reportIndex =
			(reportIndex + 1) % S6SY761_INPUT_REPORT_QUEUE_DEPTH;
	}
}

static
BOOLEAN
TouchQueueCurrentReportLocked(
	_Inout_ PDEVICE_CONTEXT DeviceContext,
	_In_ BOOLEAN IsMove
)
{
	inputReport54_t report;
	ULONG contactId;

	if (DeviceContext->InputStopping)
	{
		return FALSE;
	}

	if (!TouchBuildCurrentReportLocked(DeviceContext, &report))
	{
		return FALSE;
	}

	if (!TouchEnqueueReportLocked(DeviceContext, &report, IsMove))
	{
		TouchPreserveQueuedReleasesLocked(DeviceContext);
		DeviceContext->InputReportHead = 0;
		DeviceContext->InputReportTail = 0;
		DeviceContext->InputReportCount = 0;
		InterlockedIncrement(&DeviceContext->InputQueueFlushCount);

		if (TouchBuildAllUpReportLocked(DeviceContext, &report))
		{
			TouchEnqueueReportLocked(DeviceContext, &report, FALSE);
			return TRUE;
		}

		return FALSE;
	}

	for (contactId = 0; contactId < S6SY761_MAX_CONTACTS; contactId++)
	{
		DeviceContext->Contacts[contactId].ReleasePending = FALSE;
	}
	return TRUE;
}

static
VOID
TouchDrainInputReports(
	_Inout_ PDEVICE_CONTEXT DeviceContext
)
{
	for (;;)
	{
		ULONG pendingRequests;
		BOOLEAN pairReady;

		if (InterlockedCompareExchange(
				&DeviceContext->InputDrainActive,
				1,
				0) != 0)
		{
			return;
		}

		for (;;)
		{
			WDFREQUEST request;
			inputReport54_t report;
			NTSTATUS status;

			WdfWaitLockAcquire(DeviceContext->InputLock, NULL);
			if (DeviceContext->InputStopping ||
				(DeviceContext->InputReportCount == 0))
			{
				WdfWaitLockRelease(DeviceContext->InputLock);
				break;
			}

			status = WdfIoQueueRetrieveNextRequest(
				DeviceContext->ManualQueue,
				&request);
			if (!NT_SUCCESS(status))
			{
				WdfWaitLockRelease(DeviceContext->InputLock);
				break;
			}

			report =
				DeviceContext->InputReports[DeviceContext->InputReportHead];
			status = RequestCopyFromBuffer(
				request,
				&report,
				sizeof(report));
			if (NT_SUCCESS(status))
			{
				DeviceContext->InputReportHead =
					(DeviceContext->InputReportHead + 1) %
					S6SY761_INPUT_REPORT_QUEUE_DEPTH;
				DeviceContext->InputReportCount--;
			}
			WdfWaitLockRelease(DeviceContext->InputLock);
			WdfRequestComplete(request, status);
			InterlockedIncrement(
				&DeviceContext->InputReportCompleteCount);
		}

		InterlockedExchange(&DeviceContext->InputDrainActive, 0);

		pendingRequests = 0;
		WdfWaitLockAcquire(DeviceContext->InputLock, NULL);
		WdfIoQueueGetState(
			DeviceContext->ManualQueue,
			&pendingRequests,
			NULL);
		pairReady =
			!DeviceContext->InputStopping &&
			(DeviceContext->InputReportCount != 0) &&
			(pendingRequests != 0);
		WdfWaitLockRelease(DeviceContext->InputLock);

		if (!pairReady)
		{
			return;
		}
	}
}

static
VOID
TouchQueueAllUpReport(
	_Inout_ PDEVICE_CONTEXT DeviceContext
)
{
	inputReport54_t report;
	BOOLEAN queued;

	queued = FALSE;
	WdfWaitLockAcquire(DeviceContext->InputLock, NULL);
	if (!DeviceContext->InputStopping)
	{
		if (DeviceContext->InputReportCount != 0)
		{
			TouchPreserveQueuedReleasesLocked(DeviceContext);
			InterlockedIncrement(&DeviceContext->InputQueueFlushCount);
		}

		DeviceContext->InputReportHead = 0;
		DeviceContext->InputReportTail = 0;
		DeviceContext->InputReportCount = 0;

		if (TouchBuildAllUpReportLocked(DeviceContext, &report))
		{
			TouchEnqueueReportLocked(DeviceContext, &report, FALSE);
			queued = TRUE;
		}
	}
	WdfWaitLockRelease(DeviceContext->InputLock);

	if (queued)
	{
		TouchDrainInputReports(DeviceContext);
	}
}

static
VOID
TouchStopInput(
	_Inout_ PDEVICE_CONTEXT DeviceContext
)
{
	WdfWaitLockAcquire(DeviceContext->InputLock, NULL);
	DeviceContext->InputStopping = TRUE;
	TouchResetInputStateLocked(DeviceContext);
	WdfWaitLockRelease(DeviceContext->InputLock);
}

#define DEFINE_GUID2(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
        EXTERN_C const GUID DECLSPEC_SELECTANY name \
                = { l, w1, w2, { b1, b2,  b3,  b4,  b5,  b6,  b7,  b8 } }





typedef struct
{
	BYTE  reportId;                                 // Report ID = 0x54 (84) 'T'
													   // Collection: TouchScreen
	BYTE  DIG_TouchScreenContactCountMaximum;       // Usage 0x000D0055: Contact Count Maximum, Value = 0 to 10
} featureReport54_t;

//
// This is the default report descriptor for the virtual Hid device returned
// by the mini driver in response to IOCTL_HID_GET_REPORT_DESCRIPTOR.
//
/*HID_REPORT_DESCRIPTOR       G_DefaultReportDescriptor[] = {
	0x06,0x00, 0xFF,                // USAGE_PAGE (Vender Defined Usage Page)
	0x09,0x01,                      // USAGE (Vendor Usage 0x01)
	0xA1,0x01,                      // COLLECTION (Application)
	0x85,CONTROL_FEATURE_REPORT_ID,    // REPORT_ID (1)
	0x09,0x01,                         // USAGE (Vendor Usage 0x01)
	0x15,0x00,                         // LOGICAL_MINIMUM(0)
	0x26,0xff, 0x00,                   // LOGICAL_MAXIMUM(255)
	0x75,0x08,                         // REPORT_SIZE (0x08)
	0x96,(FEATURE_REPORT_SIZE_CB & 0xff), (FEATURE_REPORT_SIZE_CB >> 8), // REPORT_COUNT
	0xB1,0x00,                         // FEATURE (Data,Ary,Abs)
	0x09,0x01,                         // USAGE (Vendor Usage 0x01)
	0x75,0x08,                         // REPORT_SIZE (0x08)
	0x96,(INPUT_REPORT_SIZE_CB & 0xff), (INPUT_REPORT_SIZE_CB >> 8), // REPORT_COUNT
	0x81,0x00,                         // INPUT (Data,Ary,Abs)
	0x09,0x01,                         // USAGE (Vendor Usage 0x01)
	0x75,0x08,                         // REPORT_SIZE (0x08)
	0x96,(OUTPUT_REPORT_SIZE_CB & 0xff), (OUTPUT_REPORT_SIZE_CB >> 8), // REPORT_COUNT
	0x91,0x00,                         // OUTPUT (Data,Ary,Abs)
	0xC0,                           // END_COLLECTION
};*/

//
// ---------------------------------------------------------------------------
// 9820 PORTING — the panel geometry below is still the Galaxy S9+'s.
//
// This driver has no MMIO: it talks to the firmware's ACPI\SCSY0761 function
// device, so the only device-specific numbers in it are the ones in this
// descriptor. Six usage pages repeat the same pair:
//
//   X  LOGICAL_MAXIMUM 1080   PHYSICAL_MAXIMUM 271  (2.71 in)
//   Y  LOGICAL_MAXIMUM 2250   PHYSICAL_MAXIMUM 558  (5.58 in)
//
// UNIT_EXPONENT is -2, so physical values are hundredths of an inch and the
// logical space is 400 units per inch. Those describe the TOUCHABLE area of a
// curved S9+ panel, not its pixel size (1440 x 2960), so they cannot be read
// off a spec sheet.
//
// The S10e's panel is 1440 x 3040 and its own sec_ts node declares
// sec,max_coords = 4096 x 4096 (notes/dock-desktop.md §14), but the touchable
// extent of the S10e's curve has not been measured. Until it is, these bytes
// stay at the 9810 values and this package is NOT validated: a wrong physical
// extent changes Windows' gesture heuristics, whose flick and rotate thresholds
// are distance-based — a behaviour bug, not a cosmetic one.
//
// The command bytes above are NOT device-specific: READ_ALL_EVENTS 0x61,
// READ_ONE_EVENT 0x60, CLEAR_EVENT_STACK 0x62 and SENSE_ON 0x10 all match the
// 9820's sec_ts opcode list (notes/exynos9820-research.md §8) exactly.
// ---------------------------------------------------------------------------
//
HID_REPORT_DESCRIPTOR       G_DefaultReportDescriptor[] = {
	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x04,     //   (LOCAL)USAGE              0x000D0004 Touch Screen(Application Collection)
	0xA1, 0x01,     //   (MAIN)COLLECTION         0x01 Application(Usage = 0x000D0004: Page = Digitizer Device Page, Usage = Touch Screen, Type = Application Collection)
	0x85, 0x54,     //     (GLOBAL)REPORT_ID          0x54 (84) 'T'

	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x14,           //    (GLOBAL)LOGICAL_MINIMUM(0)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x07,     //       (GLOBAL)REPORT_COUNT       0x07 (7) padding bits
	0x81, 0x03,     //       (MAIN)INPUT                Constant padding
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x25, 0x09,     //       (GLOBAL)LOGICAL_MAXIMUM    0x09 (9)
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //46 47
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x55, 0x0E,     //       (GLOBAL)UNIT_EXPONENT      -2
	0x65, 0x13,     //       (GLOBAL)UNIT                Inch, English linear
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x34,           //       (GLOBAL)PHYSICAL_MINIMUM    0
	0x46, 0x0F, 0x01, //     (GLOBAL)PHYSICAL_MAXIMUM    271 (2.71 inches)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //55 56
	0x46, 0x2E, 0x02,   // (GLOBAL) PHYSICAL_MAXIMUM   558 (5.58 inches)
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x07,     //       (GLOBAL)REPORT_COUNT       0x07 (7) padding bits
	0x81, 0x03,     //       (MAIN)INPUT                Constant padding
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x25, 0x09,     //       (GLOBAL)LOGICAL_MAXIMUM    0x09 (9)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x55, 0x0E,     //       (GLOBAL)UNIT_EXPONENT      -2
	0x65, 0x13,     //       (GLOBAL)UNIT                Inch, English linear
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x34,           //       (GLOBAL)PHYSICAL_MINIMUM    0
	0x46, 0x0F, 0x01, //     (GLOBAL)PHYSICAL_MAXIMUM    271 (2.71 inches)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x46, 0x2E, 0x02,   // (GLOBAL) PHYSICAL_MAXIMUM   558 (5.58 inches)
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x07,     //       (GLOBAL)REPORT_COUNT       0x07 (7) padding bits
	0x81, 0x03,     //       (MAIN)INPUT                Constant padding
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x25, 0x09,     //       (GLOBAL)LOGICAL_MAXIMUM    0x09 (9)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x55, 0x0E,     //       (GLOBAL)UNIT_EXPONENT      -2
	0x65, 0x13,     //       (GLOBAL)UNIT                Inch, English linear
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x34,           //       (GLOBAL)PHYSICAL_MINIMUM    0
	0x46, 0x0F, 0x01, //     (GLOBAL)PHYSICAL_MAXIMUM    271 (2.71 inches)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x46, 0x2E, 0x02,   // (GLOBAL) PHYSICAL_MAXIMUM   558 (5.58 inches)
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x07,     //       (GLOBAL)REPORT_COUNT       0x07 (7) padding bits
	0x81, 0x03,     //       (MAIN)INPUT                Constant padding
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x25, 0x09,     //       (GLOBAL)LOGICAL_MAXIMUM    0x09 (9)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x55, 0x0E,     //       (GLOBAL)UNIT_EXPONENT      -2
	0x65, 0x13,     //       (GLOBAL)UNIT                Inch, English linear
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x34,           //       (GLOBAL)PHYSICAL_MINIMUM    0
	0x46, 0x0F, 0x01, //     (GLOBAL)PHYSICAL_MAXIMUM    271 (2.71 inches)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x46, 0x2E, 0x02,   // (GLOBAL) PHYSICAL_MAXIMUM   558 (5.58 inches)
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x07,     //       (GLOBAL)REPORT_COUNT       0x07 (7) padding bits
	0x81, 0x03,     //       (MAIN)INPUT                Constant padding
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x25, 0x09,     //       (GLOBAL)LOGICAL_MAXIMUM    0x09 (9)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x55, 0x0E,     //       (GLOBAL)UNIT_EXPONENT      -2
	0x65, 0x13,     //       (GLOBAL)UNIT                Inch, English linear
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x34,           //       (GLOBAL)PHYSICAL_MINIMUM    0
	0x46, 0x0F, 0x01, //     (GLOBAL)PHYSICAL_MAXIMUM    271 (2.71 inches)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x46, 0x2E, 0x02,   // (GLOBAL) PHYSICAL_MAXIMUM   558 (5.58 inches)
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x07,     //       (GLOBAL)REPORT_COUNT       0x07 (7) padding bits
	0x81, 0x03,     //       (MAIN)INPUT                Constant padding
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x25, 0x09,     //       (GLOBAL)LOGICAL_MAXIMUM    0x09 (9)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x55, 0x0E,     //       (GLOBAL)UNIT_EXPONENT      -2
	0x65, 0x13,     //       (GLOBAL)UNIT                Inch, English linear
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x34,           //       (GLOBAL)PHYSICAL_MINIMUM    0
	0x46, 0x0F, 0x01, //     (GLOBAL)PHYSICAL_MAXIMUM    271 (2.71 inches)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x46, 0x2E, 0x02,   // (GLOBAL) PHYSICAL_MAXIMUM   558 (5.58 inches)
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x07,     //       (GLOBAL)REPORT_COUNT       0x07 (7) padding bits
	0x81, 0x03,     //       (MAIN)INPUT                Constant padding
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x25, 0x09,     //       (GLOBAL)LOGICAL_MAXIMUM    0x09 (9)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x55, 0x0E,     //       (GLOBAL)UNIT_EXPONENT      -2
	0x65, 0x13,     //       (GLOBAL)UNIT                Inch, English linear
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x34,           //       (GLOBAL)PHYSICAL_MINIMUM    0
	0x46, 0x0F, 0x01, //     (GLOBAL)PHYSICAL_MAXIMUM    271 (2.71 inches)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x46, 0x2E, 0x02,   // (GLOBAL) PHYSICAL_MAXIMUM   558 (5.58 inches)
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x07,     //       (GLOBAL)REPORT_COUNT       0x07 (7) padding bits
	0x81, 0x03,     //       (MAIN)INPUT                Constant padding
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x25, 0x09,     //       (GLOBAL)LOGICAL_MAXIMUM    0x09 (9)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x55, 0x0E,     //       (GLOBAL)UNIT_EXPONENT      -2
	0x65, 0x13,     //       (GLOBAL)UNIT                Inch, English linear
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x34,           //       (GLOBAL)PHYSICAL_MINIMUM    0
	0x46, 0x0F, 0x01, //     (GLOBAL)PHYSICAL_MAXIMUM    271 (2.71 inches)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x46, 0x2E, 0x02,   // (GLOBAL) PHYSICAL_MAXIMUM   558 (5.58 inches)
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x07,     //       (GLOBAL)REPORT_COUNT       0x07 (7) padding bits
	0x81, 0x03,     //       (MAIN)INPUT                Constant padding
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x25, 0x09,     //       (GLOBAL)LOGICAL_MAXIMUM    0x09 (9)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x55, 0x0E,     //       (GLOBAL)UNIT_EXPONENT      -2
	0x65, 0x13,     //       (GLOBAL)UNIT                Inch, English linear
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x34,           //       (GLOBAL)PHYSICAL_MINIMUM    0
	0x46, 0x0F, 0x01, //     (GLOBAL)PHYSICAL_MAXIMUM    271 (2.71 inches)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x46, 0x2E, 0x02,   // (GLOBAL) PHYSICAL_MAXIMUM   558 (5.58 inches)
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x07,     //       (GLOBAL)REPORT_COUNT       0x07 (7) padding bits
	0x81, 0x03,     //       (MAIN)INPUT                Constant padding
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x25, 0x09,     //       (GLOBAL)LOGICAL_MAXIMUM    0x09 (9)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x55, 0x0E,     //       (GLOBAL)UNIT_EXPONENT      -2
	0x65, 0x13,     //       (GLOBAL)UNIT                Inch, English linear
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x34,           //       (GLOBAL)PHYSICAL_MINIMUM    0
	0x46, 0x0F, 0x01, //     (GLOBAL)PHYSICAL_MAXIMUM    271 (2.71 inches)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x46, 0x2E, 0x02,   // (GLOBAL) PHYSICAL_MAXIMUM   558 (5.58 inches)
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x55, 0x0C,     //     (GLOBAL)UNIT_EXPONENT      -4 (100 microseconds)
	0x66, 0x01, 0x10, //   (GLOBAL)UNIT               Seconds
	0x47, 0xFF, 0xFF, 0x00, 0x00, // PHYSICAL_MAXIMUM 65535
	0x27, 0xFF, 0xFF, 0x00, 0x00, // LOGICAL_MAXIMUM  65535
	0x75, 0x10,     //     (GLOBAL)REPORT_SIZE        16
	0x95, 0x01,     //     (GLOBAL)REPORT_COUNT       1
	0x09, 0x56,     //     (LOCAL)USAGE               Scan Time
	0x81, 0x02,     //     (MAIN)INPUT                Data,Var,Abs
	0x55, 0x00,     //     (GLOBAL)UNIT_EXPONENT      0
	0x65, 0x00,     //     (GLOBAL)UNIT               None
	0x09, 0x54,     //     (LOCAL)USAGE              0x000D0054 Contact Count(Dynamic Value)
	0x75, 0x08,     //     (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x25, 0x0A,     //     (GLOBAL)LOGICAL_MAXIMUM    0x0A (10)
	0x81, 0x02,     //     (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x55,     //     (LOCAL)USAGE              0x000D0055 Contact Count Maximum(Static Value)
	0xB1, 0x02,     //     (MAIN)FEATURE            0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Application

};

featureReport54_t features = { 0x54,10 };
//
// This is the default HID descriptor returned by the mini driver
// in response to IOCTL_HID_GET_DEVICE_DESCRIPTOR. The size
// of report descriptor is currently the size of G_DefaultReportDescriptor.
//

HID_DESCRIPTOR              G_DefaultHidDescriptor = {
	0x09,   // length of HID descriptor
	0x21,   // descriptor type == HID  0x21
	0x0100, // hid spec release
	0x00,   // country code == Not Specified
	0x01,   // number of HID class descriptors
	{                                       //DescriptorList[0]
		0x22,                               //report descriptor type 0x22
		sizeof(G_DefaultReportDescriptor)   //total length of report descriptor
	}
};

NTSTATUS
DriverEntry(
	_In_  PDRIVER_OBJECT    DriverObject,
	_In_  PUNICODE_STRING   RegistryPath
)
/*++

Routine Description:
	DriverEntry initializes the driver and is the first routine called by the
	system after the driver is loaded. DriverEntry specifies the other entry
	points in the function driver, such as EvtDevice and DriverUnload.

Parameters Description:

	DriverObject - represents the instance of the function driver that is loaded
	into memory. DriverEntry must initialize members of DriverObject before it
	returns to the caller. DriverObject is allocated by the system before the
	driver is loaded, and it is released by the system after the system unloads
	the function driver from memory.

	RegistryPath - represents the driver specific path in the Registry.
	The function driver can use the path to store driver related data between
	reboots. The path does not store hardware instance specific data.

Return Value:

	STATUS_SUCCESS, or another status value for which NT_SUCCESS(status) equals
					TRUE if successful,

	STATUS_UNSUCCESSFUL, or another status for which NT_SUCCESS(status) equals
					FALSE otherwise.

--*/
{
	WDF_DRIVER_CONFIG       config;
	WDF_OBJECT_ATTRIBUTES driverAttributes;
	NTSTATUS                status;

#ifdef _KERNEL_MODE
	//
	// Opt-in to using non-executable pool memory on Windows 8 and later.
	// https://msdn.microsoft.com/en-us/library/windows/hardware/hh920402(v=vs.85).aspx
	//
	ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
#endif

	WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&driverAttributes);
	driverAttributes.EvtCleanupCallback = EvtDriverCleanup;

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&driverAttributes,
		&config,
		WDF_NO_HANDLE);
	if (!NT_SUCCESS(status)) {

		goto Exit;
	}
Exit:
	return status;
}
VOID
EvtDriverCleanup(
	_In_ WDFOBJECT Object
)
{
	UNREFERENCED_PARAMETER(Object);
}
NTSTATUS
EvtDeviceAdd(
	_In_  WDFDRIVER         Driver,
	_Inout_ PWDFDEVICE_INIT DeviceInit
)
/*++
Routine Description:

	EvtDeviceAdd is called by the framework in response to AddDevice
	call from the PnP manager. We create and initialize a device object to
	represent a new instance of the device.

Arguments:

	Driver - Handle to a framework driver object created in DriverEntry

	DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

	NTSTATUS

--*/
{
	NTSTATUS                status;
	WDF_OBJECT_ATTRIBUTES   deviceAttributes;
	WDFDEVICE               device;
	PDEVICE_CONTEXT         deviceContext;
	PHID_DEVICE_ATTRIBUTES  hidAttributes;
	UNREFERENCED_PARAMETER(Driver);

	WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

	pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
	pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
	pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
	pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;


	//
	// Mark ourselves as a filter, which also relinquishes power policy ownership
	//
	WdfFdoInitSetFilter(DeviceInit);

	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
		&deviceAttributes,
		DEVICE_CONTEXT);

	status = WdfDeviceCreate(&DeviceInit,
		&deviceAttributes,
		&device);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	deviceContext = GetDeviceContext(device);
	deviceContext->Device = device;
	deviceContext->DeviceData = 0;
	deviceContext->SpbController = WDF_NO_HANDLE;
	deviceContext->Interrupt = WDF_NO_HANDLE;
	deviceContext->AliveRegisters = NULL;
	deviceContext->AliveRegistersLength = 0;
	deviceContext->InputStopping = TRUE;

	WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
	deviceAttributes.ParentObject = device;
	status = WdfWaitLockCreate(
		&deviceAttributes,
		&deviceContext->InputLock);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	hidAttributes = &deviceContext->HidDeviceAttributes;
	RtlZeroMemory(hidAttributes, sizeof(HID_DEVICE_ATTRIBUTES));
	hidAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	hidAttributes->VendorID = HIDMINI_VID;
	hidAttributes->ProductID = HIDMINI_PID;
	hidAttributes->VersionNumber = HIDMINI_VERSION;

	status = QueueCreate(device,
		&deviceContext->DefaultQueue);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = ManualQueueCreate(device,
		&deviceContext->ManualQueue);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	//
	// Use default "HID Descriptor" (hardcoded). We will set the
	// wReportLength memeber of HID descriptor when we read the
	// the report descriptor either from registry or the hard-coded
	// one.
	//
	deviceContext->HidDescriptor = G_DefaultHidDescriptor;

	//
	// We need to read read descriptor from registry
	//
	(void)ReadDescriptorFromRegistry(device);

	// The Y761 reports unscaled 12-bit coordinates regardless of stale package metadata.
	XMax = S6SY761_COORDINATE_MAX;
	YMax = S6SY761_COORDINATE_MAX;

	for (ULONG contact = 0; contact < S6SY761_MAX_CONTACTS; contact++)
	{
		const ULONG descriptorOffset = 58 * contact;

		G_DefaultReportDescriptor[40 + descriptorOffset] = XMax & 0xFF;
		G_DefaultReportDescriptor[41 + descriptorOffset] = (XMax >> 8) & 0x0F;
		G_DefaultReportDescriptor[57 + descriptorOffset] = YMax & 0xFF;
		G_DefaultReportDescriptor[58 + descriptorOffset] = (YMax >> 8) & 0x0F;
	}

	deviceContext->ReportDescriptor = G_DefaultReportDescriptor;
	status = STATUS_SUCCESS;
	RecordStartValues(device, 1, status);

	return status;
}

NTSTATUS
PowerSettingCallback(
	_In_ LPCGUID SettingGuid,
	_In_ PVOID Value,
	_In_ ULONG ValueLength,
	_Inout_opt_ PVOID Context
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT devContext = NULL;
	UNREFERENCED_PARAMETER(SettingGuid);
	UNREFERENCED_PARAMETER(ValueLength);
	UNREFERENCED_PARAMETER(Value);

	DbgPrint("PowerSettingsCallback");

	if (Context == NULL)
	{
		DbgPrint(
			"TchPowerSettingCallback: Context is NULL"
		);

		status = STATUS_INVALID_DEVICE_REQUEST;
		goto exit;
	}

	devContext = (PDEVICE_CONTEXT)Context;
	

exit:
	return status;
}

static
ULONG
TouchReadRegister(
	_In_ PDEVICE_CONTEXT Device,
	_In_ ULONG Offset
)
{
	return READ_REGISTER_ULONG(
		(volatile ULONG*)(Device->AliveRegisters + Offset));
}

static
VOID
TouchWriteRegister(
	_In_ PDEVICE_CONTEXT Device,
	_In_ ULONG Offset,
	_In_ ULONG Value
)
{
	WRITE_REGISTER_ULONG(
		(volatile ULONG*)(Device->AliveRegisters + Offset),
		Value);
}

static
VOID
TouchSetInterruptMasked(
	_In_ PDEVICE_CONTEXT Device,
	_In_ BOOLEAN Masked
)
{
	ULONG value = TouchReadRegister(Device, STAR2_EINT_MASK);

	if (Masked)
	{
		value |= STAR2_EINT8_BIT;
	}
	else
	{
		value &= ~STAR2_EINT8_BIT;
	}

	TouchWriteRegister(Device, STAR2_EINT_MASK, value);
}

static
VOID
TouchAcknowledgeInterrupt(
	_In_ PDEVICE_CONTEXT Device
)
{
	if (Device->AliveRegisters != NULL)
	{
		TouchWriteRegister(
			Device,
			STAR2_EINT_PENDING,
			STAR2_EINT8_BIT);
	}
}

static
VOID
TouchConfigureInterrupt(
	_In_ PDEVICE_CONTEXT Device
)
{
	ULONG value;

	TouchSetInterruptMasked(Device, TRUE);

	value = TouchReadRegister(Device, STAR2_GPA1_CON);
	value &= ~STAR2_GPA1_0_FUNCTION_MASK;
	value |= STAR2_GPA1_0_EINT_FUNCTION;
	TouchWriteRegister(Device, STAR2_GPA1_CON, value);

	value = TouchReadRegister(Device, STAR2_GPA1_PUD);
	value &= ~STAR2_GPA1_0_PULL_MASK;
	TouchWriteRegister(Device, STAR2_GPA1_PUD, value);

	value = TouchReadRegister(Device, STAR2_EINT_CON);
	value &= ~STAR2_EINT8_TRIGGER_MASK;
	value |= STAR2_EINT8_LEVEL_LOW;
	TouchWriteRegister(Device, STAR2_EINT_CON, value);

	value = TouchReadRegister(Device, STAR2_EINT_FILTER);
	value &= ~STAR2_EINT8_FILTER_MASK;
	value |= STAR2_EINT8_FILTER_VALUE;
	TouchWriteRegister(Device, STAR2_EINT_FILTER, value);

	TouchAcknowledgeInterrupt(Device);
}

NTSTATUS
OnPrepareHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesRaw,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

	Routine Description:

	This routine caches the SPB resource connection ID.

	Arguments:

	FxDevice - a handle to the framework device object
	FxResourcesRaw - list of translated hardware resources that
		the PnP manager has assigned to the device
	FxResourcesTranslated - list of raw hardware resources that
		the PnP manager has assigned to the device

	Return Value:

	Status

--*/
{
	PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	BOOLEAN fInterruptResourceFound = FALSE;
	BOOLEAN fAliveResourceFound = FALSE;
	ULONG interruptIndex = 0;
	PHYSICAL_ADDRESS alivePhysicalAddress;
	ULONG aliveLength = 0;
	NTSTATUS status;
	ULONG resourceMask;

	RecordStartValues(FxDevice, 10, STATUS_SUCCESS);
	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);
	alivePhysicalAddress.QuadPart = 0;
	pDevice->PeripheralId.QuadPart = 0;

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);
		if (pDescriptor == NULL)
		{
			return STATUS_DEVICE_CONFIGURATION_ERROR;
		}

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:

			//
			// Look for I2C or SPI resource and save connection ID.
			//

			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;

			if ((Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL) &&
				((Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)))
			{
				if (fSpbResourceFound == FALSE)
				{
					pDevice->PeripheralId.LowPart =
						pDescriptor->u.Connection.IdLowPart;
					pDevice->PeripheralId.HighPart =
						pDescriptor->u.Connection.IdHighPart;

					fSpbResourceFound = TRUE;
				}
			}

			break;

		case CmResourceTypeInterrupt:

			if (fInterruptResourceFound == FALSE)
			{
				fInterruptResourceFound = TRUE;
				interruptIndex = i;
			}
			break;

		case CmResourceTypeMemory:

			if ((ULONGLONG)pDescriptor->u.Memory.Start.QuadPart ==
				STAR2_ALIVE_GPIO_PHYSICAL_ADDRESS)
			{
				if (fAliveResourceFound)
				{
					return STATUS_DEVICE_CONFIGURATION_ERROR;
				}

				alivePhysicalAddress = pDescriptor->u.Memory.Start;
				aliveLength = pDescriptor->u.Memory.Length;
				fAliveResourceFound = TRUE;
			}
			break;

		default:

			//
			// Ignoring all other resource types.
			//

			break;
		}
	}

	resourceMask =
		(fSpbResourceFound ? 0x1UL : 0) |
		(fInterruptResourceFound ? 0x2UL : 0) |
		(fAliveResourceFound ? 0x4UL : 0);
	RecordStartValue(FxDevice, L"TouchResourceCount", resourceCount);
	RecordStartValue(FxDevice, L"TouchResourceMask", resourceMask);
	RecordStartValue(FxDevice, L"TouchAliveLength", aliveLength);
	RecordStartValue(
		FxDevice,
		L"TouchPeripheralIdLow",
		pDevice->PeripheralId.LowPart);
	RecordStartValue(
		FxDevice,
		L"TouchPeripheralIdHigh",
		pDevice->PeripheralId.HighPart);
	RecordStartValues(FxDevice, 11, STATUS_SUCCESS);

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		RecordStartValues(
			FxDevice,
			12,
			STATUS_DEVICE_CONFIGURATION_ERROR);
		return STATUS_DEVICE_CONFIGURATION_ERROR;
	}

	if (!fInterruptResourceFound ||
		!fAliveResourceFound ||
		(aliveLength < STAR2_ALIVE_GPIO_LENGTH))
	{
		RecordStartValues(
			FxDevice,
			12,
			STATUS_DEVICE_CONFIGURATION_ERROR);
		return STATUS_DEVICE_CONFIGURATION_ERROR;
	}

	pDevice->AliveRegisters = (PUCHAR)MmMapIoSpaceEx(
		alivePhysicalAddress,
		aliveLength,
		PAGE_READWRITE | PAGE_NOCACHE);
	if (pDevice->AliveRegisters == NULL)
	{
		RecordStartValues(FxDevice, 13, STATUS_INSUFFICIENT_RESOURCES);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	pDevice->AliveRegistersLength = aliveLength;

	{
		WDF_INTERRUPT_CONFIG interruptConfig;
		PCM_PARTIAL_RESOURCE_DESCRIPTOR rawInterrupt;

		rawInterrupt = WdfCmResourceListGetDescriptor(
			FxResourcesRaw,
			interruptIndex);
		if (rawInterrupt == NULL ||
			rawInterrupt->Type != CmResourceTypeInterrupt)
		{
			status = STATUS_DEVICE_CONFIGURATION_ERROR;
			RecordStartValues(FxDevice, 14, status);
			goto Exit;
		}

		WDF_INTERRUPT_CONFIG_INIT(
			&interruptConfig,
			OnInterruptIsr,
			NULL);
		interruptConfig.PassiveHandling = TRUE;
		interruptConfig.InterruptTranslated =
			WdfCmResourceListGetDescriptor(
				FxResourcesTranslated,
				interruptIndex);
		interruptConfig.InterruptRaw = rawInterrupt;

		status = WdfInterruptCreate(
			pDevice->Device,
			&interruptConfig,
			WDF_NO_OBJECT_ATTRIBUTES,
			&pDevice->Interrupt);
		if (!NT_SUCCESS(status))
		{
			RecordStartValues(FxDevice, 15, status);
		}
	}

Exit:
	if (!NT_SUCCESS(status))
	{
		MmUnmapIoSpace(
			pDevice->AliveRegisters,
			pDevice->AliveRegistersLength);
		pDevice->AliveRegisters = NULL;
		pDevice->AliveRegistersLength = 0;
	}
	else
	{
		RecordStartValues(FxDevice, 19, STATUS_SUCCESS);
	}
	return status;
}

NTSTATUS
OnReleaseHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

	Routine Description:

	Arguments:

	FxDevice - a handle to the framework device object
	FxResourcesTranslated - list of raw hardware resources that
		the PnP manager has assigned to the device

	Return Value:

	Status

--*/
{
	PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	if (pDevice->Interrupt != NULL)
	{
		WdfObjectDelete(pDevice->Interrupt);
		pDevice->Interrupt = WDF_NO_HANDLE;
	}

	if (pDevice->AliveRegisters != NULL)
	{
		MmUnmapIoSpace(
			pDevice->AliveRegisters,
			pDevice->AliveRegistersLength);
		pDevice->AliveRegisters = NULL;
		pDevice->AliveRegistersLength = 0;
	}

	return status;
}

NTSTATUS
OnD0Entry(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

	Routine Description:

	This routine allocates objects needed by the driver.

	Arguments:

	FxDevice - a handle to the framework device object
	FxPreviousState - previous power state

	Return Value:

	Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status;

	WdfWaitLockAcquire(pDevice->InputLock, NULL);
	TouchResetInputStateLocked(pDevice);
	pDevice->InputStopping = FALSE;
	WdfWaitLockRelease(pDevice->InputLock);
	InterlockedExchange(&pDevice->InputDrainActive, 0);
	InterlockedExchange(&pDevice->InputInterruptCount, 0);
	InterlockedExchange(&pDevice->InputI2cErrorCount, 0);
	InterlockedExchange(&pDevice->InputOverflowCount, 0);
	InterlockedExchange(&pDevice->InputQueueFlushCount, 0);
	InterlockedExchange(&pDevice->InputReportCoalesceCount, 0);
	InterlockedExchange(&pDevice->InputReportCompleteCount, 0);

	RecordStartValues(FxDevice, 20, STATUS_SUCCESS);
	TouchConfigureInterrupt(pDevice);

	//
	// Create the SPB target.
	//

	WDF_OBJECT_ATTRIBUTES targetAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&targetAttributes);

	status = WdfIoTargetCreate(
		pDevice->Device,
		&targetAttributes,
		&pDevice->SpbController);

	if (!NT_SUCCESS(status))
	{
		RecordStartValues(FxDevice, 21, status);
		TouchSetInterruptMasked(pDevice, TRUE);
		TouchStopInput(pDevice);
		return status;
	}
	RecordStartValues(FxDevice, 22, STATUS_SUCCESS);

	status = SpbDeviceOpen(pDevice);
	if (!NT_SUCCESS(status))
	{
		RecordStartValue(FxDevice, L"TouchD0Status", (ULONG)status);
		WdfObjectDelete(pDevice->SpbController);
		pDevice->SpbController = WDF_NO_HANDLE;
		TouchSetInterruptMasked(pDevice, TRUE);
		TouchStopInput(pDevice);
		return status;
	}

	TouchAcknowledgeInterrupt(pDevice);
	TouchSetInterruptMasked(pDevice, FALSE);
	RecordStartValue(FxDevice, L"TouchD0Status", STATUS_SUCCESS);
	RecordStartValues(FxDevice, 29, STATUS_SUCCESS);

	return STATUS_SUCCESS;
}

NTSTATUS
OnD0Exit(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

	Routine Description:

	This routine destroys objects needed by the driver.

	Arguments:

	FxDevice - a handle to the framework device object
	FxPreviousState - previous power state

	Return Value:

	Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);

	TouchSetInterruptMasked(pDevice, TRUE);
	TouchAcknowledgeInterrupt(pDevice);
	TouchQueueAllUpReport(pDevice);
	RecordStartValue(
		FxDevice,
		L"TouchIrqCount",
		(ULONG)pDevice->InputInterruptCount);
	RecordStartValue(
		FxDevice,
		L"TouchI2cErrorCount",
		(ULONG)pDevice->InputI2cErrorCount);
	RecordStartValue(
		FxDevice,
		L"TouchOverflowCount",
		(ULONG)pDevice->InputOverflowCount);
	RecordStartValue(
		FxDevice,
		L"TouchQueueFlushCount",
		(ULONG)pDevice->InputQueueFlushCount);
	RecordStartValue(
		FxDevice,
		L"TouchReportCoalesceCount",
		(ULONG)pDevice->InputReportCoalesceCount);
	RecordStartValue(
		FxDevice,
		L"TouchReportCompleteCount",
		(ULONG)pDevice->InputReportCompleteCount);
	TouchStopInput(pDevice);
	SpbDeviceClose(pDevice);

	if (pDevice->SpbController != WDF_NO_HANDLE)
	{
		WdfObjectDelete(pDevice->SpbController);
		pDevice->SpbController = WDF_NO_HANDLE;
	}

	return STATUS_SUCCESS;
}

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoDeviceControl;


NTSTATUS
QueueCreate(
	_In_  WDFDEVICE         Device,
	_Out_ WDFQUEUE* Queue
)
/*++
Routine Description:

	This function creates a default, parallel I/O queue to proces IOCTLs
	from hidclass.sys.

Arguments:

	Device - Handle to a framework device object.

	Queue - Output pointer to a framework I/O queue handle, on success.

Return Value:

	NTSTATUS

--*/
{
	NTSTATUS                status;
	WDF_IO_QUEUE_CONFIG     queueConfig;
	WDF_OBJECT_ATTRIBUTES   queueAttributes;
	WDFQUEUE                queue;
	PQUEUE_CONTEXT          queueContext;

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
		&queueConfig,
		WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = EvtIoDeviceControl;


	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
		&queueAttributes,
		QUEUE_CONTEXT);
	queueAttributes.ExecutionLevel = WdfExecutionLevelPassive;

	status = WdfIoQueueCreate(
		Device,
		&queueConfig,
		&queueAttributes,
		&queue);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	queueContext = GetQueueContext(queue);
	queueContext->Queue = queue;
	queueContext->DeviceContext = GetDeviceContext(Device);
	queueContext->OutputReport = 0;

	*Queue = queue;

	return status;
}

VOID
EvtIoDeviceControl(
	_In_  WDFQUEUE          Queue,
	_In_  WDFREQUEST        Request,
	_In_  size_t            OutputBufferLength,
	_In_  size_t            InputBufferLength,
	_In_  ULONG             IoControlCode
)
/*++
Routine Description:

	This event callback function is called when the driver receives an

	(KMDF) IOCTL_HID_Xxx code when handlng IRP_MJ_INTERNAL_DEVICE_CONTROL
	(UMDF) IOCTL_HID_Xxx, IOCTL_UMDF_HID_Xxx when handling IRP_MJ_DEVICE_CONTROL

Arguments:

	Queue - A handle to the queue object that is associated with the I/O request

	Request - A handle to a framework request object.

	OutputBufferLength - The length, in bytes, of the request's output buffer,
			if an output buffer is available.

	InputBufferLength - The length, in bytes, of the request's input buffer, if
			an input buffer is available.

	IoControlCode - The driver or system defined IOCTL associated with the request

Return Value:

	NTSTATUS

--*/
{
	NTSTATUS                status;
	BOOLEAN                 completeRequest = TRUE;
	WDFDEVICE               device = WdfIoQueueGetDevice(Queue);
	PDEVICE_CONTEXT         deviceContext = NULL;
	PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	deviceContext = GetDeviceContext(device);

	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:   // METHOD_NEITHER
		//
		// Retrieves the device's HID descriptor.
		//
		_Analysis_assume_(deviceContext->HidDescriptor.bLength != 0);
		status = RequestCopyFromBuffer(Request,
			&deviceContext->HidDescriptor,
			deviceContext->HidDescriptor.bLength);
		break;

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:   // METHOD_NEITHER
		//
		//Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
		//
		status = RequestCopyFromBuffer(Request,
			&queueContext->DeviceContext->HidDeviceAttributes,
			sizeof(HID_DEVICE_ATTRIBUTES));
		break;

	case IOCTL_HID_GET_REPORT_DESCRIPTOR:   // METHOD_NEITHER
		//
		//Obtains the report descriptor for the HID device.
		//
		status = RequestCopyFromBuffer(Request,
			deviceContext->ReportDescriptor,
			deviceContext->HidDescriptor.DescriptorList[0].wReportLength);
		break;

	case IOCTL_HID_READ_REPORT:             // METHOD_NEITHER
		//
		// Returns a report from the device into a class driver-supplied
		// buffer.
		//
		status = ReadReport(queueContext, Request, &completeRequest);
		break;

	case IOCTL_HID_WRITE_REPORT:            // METHOD_NEITHER
		//
		// Transmits a class driver-supplied report to the device.
		//
		status = WriteReport(queueContext, Request);
		break;

	case IOCTL_HID_GET_FEATURE:             // METHOD_OUT_DIRECT

		status = GetFeature(queueContext, Request);
		break;

	case IOCTL_HID_SET_FEATURE:             // METHOD_IN_DIRECT

		status = SetFeature(queueContext, Request);
		break;

	case IOCTL_HID_GET_INPUT_REPORT:        // METHOD_OUT_DIRECT

		status = GetInputReport(queueContext, Request);
		break;

	case IOCTL_HID_SET_OUTPUT_REPORT:       // METHOD_IN_DIRECT

		status = SetOutputReport(queueContext, Request);
		break;


	case IOCTL_HID_GET_STRING:                      // METHOD_NEITHER

		status = GetString(Request);
		break;

	case IOCTL_HID_GET_INDEXED_STRING:              // METHOD_OUT_DIRECT

		status = GetIndexedString(Request);
		break;

	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:  // METHOD_NEITHER
		//
		// This has the USBSS Idle notification callback. If the lower driver
		// can handle it (e.g. USB stack can handle it) then pass it down
		// otherwise complete it here as not inplemented. For a virtual
		// device, idling is not needed.
		//
		// Not implemented. fall through...
		//
	case IOCTL_HID_ACTIVATE_DEVICE:                 // METHOD_NEITHER
	case IOCTL_HID_DEACTIVATE_DEVICE:               // METHOD_NEITHER
	case IOCTL_GET_PHYSICAL_DESCRIPTOR:             // METHOD_OUT_DIRECT
		//
		// We don't do anything for these IOCTLs but some minidrivers might.
		//
		// Not implemented. fall through...
		//
	default:
		status = STATUS_NOT_IMPLEMENTED;
		break;
	}

	//
	// Complete the request. Information value has already been set by request
	// handlers.
	//
	if (completeRequest) {
		WdfRequestComplete(Request, status);
	}
}

NTSTATUS
RequestCopyFromBuffer(
	_In_  WDFREQUEST        Request,
	_In_  PVOID             SourceBuffer,
	_When_(NumBytesToCopyFrom == 0, __drv_reportError(NumBytesToCopyFrom cannot be zero))
	_In_  size_t            NumBytesToCopyFrom
)
/*++

Routine Description:

	A helper function to copy specified bytes to the request's output memory

Arguments:

	Request - A handle to a framework request object.

	SourceBuffer - The buffer to copy data from.

	NumBytesToCopyFrom - The length, in bytes, of data to be copied.

Return Value:

	NTSTATUS

--*/
{
	NTSTATUS                status;
	WDFMEMORY               memory;
	size_t                  outputBufferLength;

	status = WdfRequestRetrieveOutputMemory(Request, &memory);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	WdfMemoryGetBuffer(memory, &outputBufferLength);
	if (outputBufferLength < NumBytesToCopyFrom) {
		status = STATUS_INVALID_BUFFER_SIZE;
		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0,
		SourceBuffer,
		NumBytesToCopyFrom);
	if (!NT_SUCCESS(status)) {

		return status;
	}
	WdfRequestSetInformation(Request, NumBytesToCopyFrom);
	return status;
}

NTSTATUS
ReadReport(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request,
	_Always_(_Out_)
	BOOLEAN* CompleteRequest
)
/*++

Routine Description:

	Handles IOCTL_HID_READ_REPORT for the HID collection. Normally the request
	will be forwarded to a manual queue for further process. In that case, the
	caller should not try to complete the request at this time, as the request
	will later be retrieved back from the manually queue and completed there.
	However, if for some reason the forwarding fails, the caller still need
	to complete the request with proper error code immediately.

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to  Request Packet.

	CompleteRequest - A boolean output value, indicating whether the caller
			should complete the request or not

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;

	//
	// forward the request to manual queue
	//
	status = WdfRequestForwardToIoQueue(
		Request,
		QueueContext->DeviceContext->ManualQueue);
	if (!NT_SUCCESS(status)) {
		*CompleteRequest = TRUE;
	}
	else {
		*CompleteRequest = FALSE;
		TouchDrainInputReports(QueueContext->DeviceContext);
	}

	return status;
}

NTSTATUS
WriteReport(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_WRITE_REPORT all the collection.

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to  Request Packet.

Return Value:

	NT status code.

--*/

{
	NTSTATUS                status;
	HID_XFER_PACKET         packet;
	ULONG                   reportSize;
	PHIDMINI_OUTPUT_REPORT  outputReport;

	status = RequestGetHidXferPacket_ToWriteToDevice(
		Request,
		&packet);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
		//
		// Return error for unknown collection
		//
		status = STATUS_INVALID_PARAMETER;
		return status;
	}

	//
	// before touching buffer make sure buffer is big enough.
	//
	reportSize = sizeof(HIDMINI_OUTPUT_REPORT);

	if (packet.reportBufferLen < reportSize) {
		status = STATUS_INVALID_BUFFER_SIZE;

		return status;
	}

	outputReport = (PHIDMINI_OUTPUT_REPORT)packet.reportBuffer;

	//
	// Store the device data in device extension.
	//
	QueueContext->DeviceContext->DeviceData = outputReport->Data;

	//
	// set status and information
	//
	WdfRequestSetInformation(Request, reportSize);
	return status;
}


HRESULT
GetFeature(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_GET_FEATURE for all the collection.

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to  Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	HID_XFER_PACKET         packet;
	ULONG                   reportSize;

	UNREFERENCED_PARAMETER(QueueContext);
	status = RequestGetHidXferPacket_ToReadFromDevice(
		Request,
		&packet);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
		//
		// If collection ID is not for control collection then handle
		// this request just as you would for a regular collection.
		//
		status = STATUS_INVALID_PARAMETER;


		return status;
	}

	//
	// Since output buffer is for write only (no read allowed by UMDF in output
	// buffer), any read from output buffer would be reading garbage), so don't
	// let app embed custom control code in output buffer. The minidriver can
	// support multiple features using separate report ID instead of using
	// custom control code. Since this is targeted at report ID 1, we know it
	// is a request for getting attributes.
	//
	// While KMDF does not enforce the rule (disallow read from output buffer),
	// it is good practice to not do so.
	//

	reportSize = sizeof(features);
	if (packet.reportBufferLen < reportSize) {
		status = STATUS_INVALID_BUFFER_SIZE;


		return status;
	}

	//
	// Since this device has one report ID, hidclass would pass on the report
	// ID in the buffer (it wouldn't if report descriptor did not have any report
	// ID). However, since UMDF allows only writes to an output buffer, we can't
	// "read" the report ID from "output" buffer. There is no need to read the
	// report ID since we get it other way as shown above, however this is
	// something to keep in mind.
	//
	packet.reportBuffer[0] = features.reportId;
	packet.reportBuffer[1] = features.DIG_TouchScreenContactCountMaximum;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, reportSize);
	return status;
}

NTSTATUS
SetFeature(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_SET_FEATURE for all the collection.
	For control collection (custom defined collection) it handles
	the user-defined control codes for sideband communication

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	HID_XFER_PACKET         packet;
	ULONG                   reportSize;
	PHIDMINI_CONTROL_INFO   controlInfo;
	PHID_DEVICE_ATTRIBUTES  hidAttributes = &QueueContext->DeviceContext->HidDeviceAttributes;

	status = RequestGetHidXferPacket_ToWriteToDevice(
		Request,
		&packet);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
		//
		// If collection ID is not for control collection then handle
		// this request just as you would for a regular collection.
		//
		status = STATUS_INVALID_PARAMETER;


		return status;
	}

	//
	// before touching control code make sure buffer is big enough.
	//
	reportSize = sizeof(HIDMINI_CONTROL_INFO);

	if (packet.reportBufferLen < reportSize) {
		status = STATUS_INVALID_BUFFER_SIZE;


		return status;
	}

	controlInfo = (PHIDMINI_CONTROL_INFO)packet.reportBuffer;

	switch (controlInfo->ControlCode)
	{
	case HIDMINI_CONTROL_CODE_SET_ATTRIBUTES:
		//
		// Store the device attributes in device extension
		//
		hidAttributes->ProductID = controlInfo->u.Attributes.ProductID;
		hidAttributes->VendorID = controlInfo->u.Attributes.VendorID;
		hidAttributes->VersionNumber = controlInfo->u.Attributes.VersionNumber;

		//
		// set status and information
		//
		WdfRequestSetInformation(Request, reportSize);
		break;

	case HIDMINI_CONTROL_CODE_DUMMY1:
		status = STATUS_NOT_IMPLEMENTED;

		break;

	case HIDMINI_CONTROL_CODE_DUMMY2:
		status = STATUS_NOT_IMPLEMENTED;

		break;

	default:
		status = STATUS_NOT_IMPLEMENTED;
		break;
	}
	return status;
}

NTSTATUS
GetInputReport(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_GET_INPUT_REPORT for all the collection.

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	HID_XFER_PACKET         packet;
	ULONG                   reportSize;
	PHIDMINI_INPUT_REPORT   reportBuffer;

	status = RequestGetHidXferPacket_ToReadFromDevice(
		Request,
		&packet);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
		//
		// If collection ID is not for control collection then handle
		// this request just as you would for a regular collection.
		//
		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	reportSize = sizeof(HIDMINI_INPUT_REPORT);
	if (packet.reportBufferLen < reportSize) {
		status = STATUS_INVALID_BUFFER_SIZE;

		return status;
	}

	reportBuffer = (PHIDMINI_INPUT_REPORT)(packet.reportBuffer);

	reportBuffer->ReportId = CONTROL_COLLECTION_REPORT_ID;
	reportBuffer->Data = QueueContext->OutputReport;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, reportSize);
	return status;
}


NTSTATUS
SetOutputReport(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_SET_OUTPUT_REPORT for all the collection.

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	HID_XFER_PACKET         packet;
	ULONG                   reportSize;
	PHIDMINI_OUTPUT_REPORT  reportBuffer;

	status = RequestGetHidXferPacket_ToWriteToDevice(
		Request,
		&packet);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
		//
		// If collection ID is not for control collection then handle
		// this request just as you would for a regular collection.
		//
		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	//
	// before touching buffer make sure buffer is big enough.
	//
	reportSize = sizeof(HIDMINI_OUTPUT_REPORT);

	if (packet.reportBufferLen < reportSize) {
		status = STATUS_INVALID_BUFFER_SIZE;
		return status;
	}

	reportBuffer = (PHIDMINI_OUTPUT_REPORT)packet.reportBuffer;

	QueueContext->OutputReport = reportBuffer->Data;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, reportSize);
	return status;
}


NTSTATUS
GetStringId(
	_In_  WDFREQUEST        Request,
	_Out_ ULONG* StringId,
	_Out_ ULONG* LanguageId
)
/*++

Routine Description:

	Helper routine to decode IOCTL_HID_GET_INDEXED_STRING and IOCTL_HID_GET_STRING.

Arguments:

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	ULONG                   inputValue;

	WDF_REQUEST_PARAMETERS  requestParameters;

	//
	// IOCTL_HID_GET_STRING:                      // METHOD_NEITHER
	// IOCTL_HID_GET_INDEXED_STRING:              // METHOD_OUT_DIRECT
	//
	// The string id (or string index) is passed in Parameters.DeviceIoControl.
	// Type3InputBuffer. However, Parameters.DeviceIoControl.InputBufferLength
	// was not initialized by hidclass.sys, therefore trying to access the
	// buffer with WdfRequestRetrieveInputMemory will fail
	//
	// Another problem with IOCTL_HID_GET_INDEXED_STRING is that METHOD_OUT_DIRECT
	// expects the input buffer to be Irp->AssociatedIrp.SystemBuffer instead of
	// Type3InputBuffer. That will also fail WdfRequestRetrieveInputMemory.
	//
	// The solution to the above two problems is to get Type3InputBuffer directly
	//
	// Also note that instead of the buffer's content, it is the buffer address
	// that was used to store the string id (or index)
	//

	WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
	WdfRequestGetParameters(Request, &requestParameters);

	inputValue = PtrToUlong(
		requestParameters.Parameters.DeviceIoControl.Type3InputBuffer);

	status = STATUS_SUCCESS;

	//
	// The least significant two bytes of the INT value contain the string id.
	//
	*StringId = (inputValue & 0x0ffff);

	//
	// The most significant two bytes of the INT value contain the language
	// ID (for example, a value of 1033 indicates English).
	//
	*LanguageId = (inputValue >> 16);
	return status;
}


NTSTATUS
GetIndexedString(
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_GET_INDEXED_STRING

Arguments:

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	ULONG                   languageId, stringIndex;

	status = GetStringId(Request, &stringIndex, &languageId);

	// While we don't use the language id, some minidrivers might.
	//
	UNREFERENCED_PARAMETER(languageId);

	if (NT_SUCCESS(status)) {

		if (stringIndex != VHIDMINI_DEVICE_STRING_INDEX)
		{
			status = STATUS_INVALID_PARAMETER;

			return status;
		}

		status = RequestCopyFromBuffer(Request, VHIDMINI_DEVICE_STRING, sizeof(VHIDMINI_DEVICE_STRING));
	}
	return status;
}


NTSTATUS
GetString(
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_GET_STRING.

Arguments:

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	ULONG                   languageId, stringId;
	size_t                  stringSizeCb;
	PWSTR                   string;

	status = GetStringId(Request, &stringId, &languageId);

	// While we don't use the language id, some minidrivers might.
	//
	UNREFERENCED_PARAMETER(languageId);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	switch (stringId) {
	case HID_STRING_ID_IMANUFACTURER:
		stringSizeCb = sizeof(VHIDMINI_MANUFACTURER_STRING);
		string = VHIDMINI_MANUFACTURER_STRING;
		break;
	case HID_STRING_ID_IPRODUCT:
		stringSizeCb = sizeof(VHIDMINI_PRODUCT_STRING);
		string = VHIDMINI_PRODUCT_STRING;
		break;
	case HID_STRING_ID_ISERIALNUMBER:
		stringSizeCb = sizeof(VHIDMINI_SERIAL_NUMBER_STRING);
		string = VHIDMINI_SERIAL_NUMBER_STRING;
		break;
	default:
		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	status = RequestCopyFromBuffer(Request, string, stringSizeCb);
	return status;
}


NTSTATUS
ManualQueueCreate(
	_In_  WDFDEVICE         Device,
	_Out_ WDFQUEUE* Queue
)
/*++
Routine Description:

	This function creates a manual I/O queue to receive IOCTL_HID_READ_REPORT
	forwarded from the device's default queue handler.

	It also creates a periodic timer to check the queue and complete any pending
	request with data from the device. Here timer expiring is used to simulate
	a hardware event that new data is ready.

	The workflow is like this:

	- Hidclass.sys sends an ioctl to the miniport to read input report.

	- The request reaches the driver's default queue. As data may not be avaiable
	  yet, the request is forwarded to a second manual queue temporarily.

	- Later when data is ready (as simulated by timer expiring), the driver
	  checks for any pending request in the manual queue, and then completes it.

	- Hidclass gets notified for the read request completion and return data to
	  the caller.

	On the other hand, for IOCTL_HID_WRITE_REPORT request, the driver simply
	sends the request to the hardware (as simulated by storing the data at
	DeviceContext->DeviceData) and completes the request immediately. There is
	no need to use another queue for write operation.

Arguments:

	Device - Handle to a framework device object.

	Queue - Output pointer to a framework I/O queue handle, on success.

Return Value:

	NTSTATUS

--*/
{
	NTSTATUS                status;
	WDF_IO_QUEUE_CONFIG     queueConfig;
	WDF_OBJECT_ATTRIBUTES   queueAttributes;
	WDFQUEUE                queue;
	PMANUAL_QUEUE_CONTEXT   queueContext;

	WDF_IO_QUEUE_CONFIG_INIT(
		&queueConfig,
		WdfIoQueueDispatchManual);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
		&queueAttributes,
		MANUAL_QUEUE_CONTEXT);

	status = WdfIoQueueCreate(
		Device,
		&queueConfig,
		&queueAttributes,
		&queue);

	if (!NT_SUCCESS(status)) {


		return status;
	}

	queueContext = GetManualQueueContext(queue);
	queueContext->Queue = queue;
	queueContext->DeviceContext = GetDeviceContext(Device);

	*Queue = queue;
	return status;
}

void
EvtTimerFunc(
	_In_  WDFTIMER          Timer
)
/*++
Routine Description:

	This periodic timer callback routine checks the device's manual queue and
	completes any pending request with data from the device.

Arguments:

	Timer - Handle to a timer object that was obtained from WdfTimerCreate.

Return Value:

	VOID

--*/
{
	NTSTATUS                status;
	WDFQUEUE                queue;
	PMANUAL_QUEUE_CONTEXT   queueContext;
	WDFREQUEST              request;
	HIDMINI_INPUT_REPORT    readReport;

	queue = (WDFQUEUE)WdfTimerGetParentObject(Timer);
	queueContext = GetManualQueueContext(queue);

	//
	// see if we have a request in manual queue
	//
	status = WdfIoQueueRetrieveNextRequest(
		queueContext->Queue,
		&request);

	if (NT_SUCCESS(status)) {

		readReport.ReportId = CONTROL_FEATURE_REPORT_ID;
		readReport.Data = queueContext->DeviceContext->DeviceData;

		status = RequestCopyFromBuffer(request,
			&readReport,
			sizeof(readReport));

		WdfRequestComplete(request, status);
	}
}
BOOLEAN
OnInterruptIsr(
	_In_  WDFINTERRUPT FxInterrupt,
	_In_  ULONG        MessageID
)
/*++

  Routine Description:

	This routine responds to interrupts generated by the H/W.
	It then waits indefinitely for the user to signal that
	the interrupt has been acknowledged, allowing the ISR to
	return. This ISR is called at PASSIVE_LEVEL.

  Arguments:

	Interrupt - a handle to a framework interrupt object
	MessageID - message number identifying the device's
		hardware interrupt message (if using MSI)

  Return Value:

	TRUE if interrupt recognized.

--*/
{
	WDFDEVICE               device;
	PDEVICE_CONTEXT         pDevice;
	int                     remain, x, y;
	NTSTATUS                status;
	BOOLEAN                 queued;
	BYTE                    touchType, touchId;
	UINT8					event_id, * event;
	UNREFERENCED_PARAMETER(MessageID);

	device = WdfInterruptGetDevice(FxInterrupt);
	pDevice = GetDeviceContext(device);
	InterlockedIncrement(&pDevice->InputInterruptCount);
	RtlZeroMemory(eventbuf, sizeof(eventbuf));
	queued = FALSE;

	// get some
	status = SpbDeviceWriteRead(pDevice, S6SY761_READ_ONE_EVENT, eventbuf, 1, S6SY761_EVENT_SIZE);
	if (!NT_SUCCESS(status))
	{
		InterlockedIncrement(&pDevice->InputI2cErrorCount);
		TouchQueueAllUpReport(pDevice);
		goto Exit;
	}
	if (!eventbuf[0])
	{
		goto Exit;
	}
	//interrupt single event print
	//DbgPrint("%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n", eventbuf[0], eventbuf[1], eventbuf[2], eventbuf[3], eventbuf[4], eventbuf[5], eventbuf[6], eventbuf[7]);

	// get some more
	remain = eventbuf[7] & S6SY761_MASK_LEFT_EVENTS;
	if (remain > S6SY761_EVENT_COUNT - 1)
	{
		InterlockedIncrement(&pDevice->InputOverflowCount);
		TouchQueueAllUpReport(pDevice);
		status = SpbDeviceWrite(
			pDevice,
			S6SY761_CLEAR_EVENT_STACK,
			sizeof(S6SY761_CLEAR_EVENT_STACK));
		if (!NT_SUCCESS(status))
		{
			InterlockedIncrement(&pDevice->InputI2cErrorCount);
		}
		goto Exit;
	}
	if (remain)
	{
        //DbgPrint("events remaining: %d\n", remain);
        status = SpbDeviceWriteRead(pDevice, S6SY761_READ_ALL_EVENTS, eventbuf + S6SY761_EVENT_SIZE, 1, remain * S6SY761_EVENT_SIZE);
		if (!NT_SUCCESS(status))
		{
			InterlockedIncrement(&pDevice->InputI2cErrorCount);
			TouchQueueAllUpReport(pDevice);
			goto Exit;
		}
	}

	// handle the events now
	WdfWaitLockAcquire(pDevice->InputLock, NULL);
	for (int i = 0; i <= remain; ++i)
	{
		event = &eventbuf[i * S6SY761_EVENT_SIZE];
		event_id = event[0] & S6SY761_MASK_EID;
		if (!event[0])
		{
			break;
		}

		switch (event_id)
		{
		case S6SY761_EVENT_ID_COORDINATE:
		{
			S6SY761_CONTACT_STATE* contact;
			BOOLEAN isMove;

			if (pDevice->InputStopping)
			{
				break;
			}
			if (!(event[0] & S6SY761_MASK_TID))
			{
				break;
			}
            touchId = ((event[0] & S6SY761_MASK_TID) >> 2) - 1;
            touchType = (event[0] & S6SY761_MASK_TOUCH_STATE) >> 6;
			if (touchType == S6SY761_TS_NONE ||
				touchId >= S6SY761_MAX_CONTACTS)
			{
				break;
			}

			contact = &pDevice->Contacts[touchId];
			x = (event[1] << 4) | ((event[3] & 0xf0) >> 4);
			y = (event[2] << 4) | (event[3] & 0x0f);

			switch (touchType)
			{
			case S6SY761_TS_RELEASE:
				if (contact->Active)
				{
					contact->Active = FALSE;
					contact->ReleasePending = TRUE;
					queued =
						TouchQueueCurrentReportLocked(pDevice, FALSE) ||
						queued;
				}
				break;
			case S6SY761_TS_MOVE:
			case S6SY761_TS_PRESS:
				isMove = touchType == S6SY761_TS_MOVE && contact->Active;
				contact->Active = TRUE;
				contact->ReleasePending = FALSE;
				contact->X = (USHORT)x;
				contact->Y = (USHORT)y;
				queued =
					TouchQueueCurrentReportLocked(pDevice, isMove) ||
					queued;
				break;
			}

			break;
		}
		case S6SY761_EVENT_ID_STATUS:
			break;
		default:
			break;
		}
	}

	WdfWaitLockRelease(pDevice->InputLock);

	if (queued)
	{
		TouchDrainInputReports(pDevice);
	}

Exit:
	TouchAcknowledgeInterrupt(pDevice);
	return TRUE;
}
NTSTATUS
SpbDeviceOpen(
	_In_  PDEVICE_CONTEXT  pDevice
)
{
	WDF_IO_TARGET_OPEN_PARAMS  openParams;
	NTSTATUS status;
	DECLARE_UNICODE_STRING_SIZE(DevicePath, RESOURCE_HUB_PATH_SIZE);
	RESOURCE_HUB_CREATE_PATH_FROM_ID(
		&DevicePath,
		pDevice->PeripheralId.LowPart,
		pDevice->PeripheralId.HighPart);
	RecordStartValues(pDevice->Device, 30, STATUS_SUCCESS);

	//
	// Open a handle to the SPB controller.
	//

	WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
		&openParams,
		&DevicePath,
		(GENERIC_READ | GENERIC_WRITE));

	openParams.ShareAccess = 0;
	openParams.CreateDisposition = FILE_OPEN;
	openParams.FileAttributes = FILE_ATTRIBUTE_NORMAL;

	status = WdfIoTargetOpen(
		pDevice->SpbController,
		&openParams);

	if (!NT_SUCCESS(status))
	{
		RecordStartValues(pDevice->Device, 31, status);
		return status;
	}
	RecordStartValues(pDevice->Device, 32, STATUS_SUCCESS);

	status = SpbDeviceWrite(
		pDevice,
		S6SY761_SENSE_ON,
		sizeof(S6SY761_SENSE_ON));
	if (NT_SUCCESS(status))
	{
		RecordStartValues(pDevice->Device, 34, STATUS_SUCCESS);
		status = SpbDeviceWrite(
			pDevice,
			S6SY761_CLEAR_EVENT_STACK,
			sizeof(S6SY761_CLEAR_EVENT_STACK));
		if (NT_SUCCESS(status))
		{
			RecordStartValues(pDevice->Device, 36, STATUS_SUCCESS);
		}
		else
		{
			RecordStartValues(pDevice->Device, 35, status);
		}
	}
	else
	{
		RecordStartValues(pDevice->Device, 33, status);
	}

	if (!NT_SUCCESS(status))
	{
		WdfIoTargetClose(pDevice->SpbController);
	}

	return status;
}
VOID
SpbDeviceClose(
	_In_  PDEVICE_CONTEXT  pDevice
)
{
	if (pDevice->SpbController != WDF_NO_HANDLE)
	{
		WdfIoTargetClose(pDevice->SpbController);
	}
}
NTSTATUS
SpbDeviceWrite(
	_In_ PDEVICE_CONTEXT pDevice,
	_In_ PVOID pInputBuffer,
	_In_ size_t inputBufferLength
)
{
	WDF_MEMORY_DESCRIPTOR  inMemoryDescriptor;
	WDF_REQUEST_SEND_OPTIONS options;
	ULONG_PTR  bytesWritten = 0;
	NTSTATUS status;

	if (pInputBuffer == NULL ||
		inputBufferLength == 0 ||
		inputBufferLength > MAXULONG)
	{
		return STATUS_INVALID_PARAMETER;
	}

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inMemoryDescriptor,
		pInputBuffer,
		(ULONG)inputBufferLength);
	WDF_REQUEST_SEND_OPTIONS_INIT(
		&options,
		WDF_REQUEST_SEND_OPTION_TIMEOUT);
	WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
		&options,
		WDF_REL_TIMEOUT_IN_MS(S6SY761_SPB_TIMEOUT_MS));

	status = WdfIoTargetSendWriteSynchronously(
		pDevice->SpbController,
		NULL,
		&inMemoryDescriptor,
		NULL,
		&options,
		&bytesWritten
	);

	if (NT_SUCCESS(status) && bytesWritten != inputBufferLength)
	{
		status = STATUS_DEVICE_DATA_ERROR;
	}

	return status;
}

NTSTATUS
SpbDeviceWriteRead(
	_In_ PDEVICE_CONTEXT pDevice,
	_In_ PVOID pInputBuffer,
	_In_ PVOID pOutputBuffer,
	_In_ size_t inputBufferLength,
	_In_ size_t outputBufferLength
)
{
	SPB_TRANSFER_LIST_AND_ENTRIES(2) sequence;
	WDF_MEMORY_DESCRIPTOR sequenceDescriptor;
	WDF_OBJECT_ATTRIBUTES requestAttributes;
	WDF_REQUEST_SEND_OPTIONS options;
	WDFREQUEST request;
	ULONG_PTR bytesTransferred;
	NTSTATUS status;

	if (pInputBuffer == NULL ||
		pOutputBuffer == NULL ||
		inputBufferLength == 0 ||
		outputBufferLength == 0 ||
		inputBufferLength > MAXULONG ||
		outputBufferLength > MAXULONG)
	{
		return STATUS_INVALID_PARAMETER;
	}

	request = WDF_NO_HANDLE;
	bytesTransferred = 0;
	WDF_OBJECT_ATTRIBUTES_INIT(&requestAttributes);
	status = WdfRequestCreate(
		&requestAttributes,
		pDevice->SpbController,
		&request);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	SPB_TRANSFER_LIST_INIT(&sequence.List, 2);
	sequence.List.Transfers[0] =
		SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
			SpbTransferDirectionToDevice,
			0,
			pInputBuffer,
			(ULONG)inputBufferLength);
	sequence.ExtraTransfers[0] =
		SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
			SpbTransferDirectionFromDevice,
			0,
			pOutputBuffer,
			(ULONG)outputBufferLength);

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
		&sequenceDescriptor,
		&sequence,
		sizeof(sequence));
	WDF_REQUEST_SEND_OPTIONS_INIT(&options, 0);
	WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
		&options,
		WDF_REL_TIMEOUT_IN_MS(S6SY761_SPB_TIMEOUT_MS));

	status = WdfIoTargetSendIoctlSynchronously(
		pDevice->SpbController,
		request,
		IOCTL_SPB_EXECUTE_SEQUENCE,
		&sequenceDescriptor,
		NULL,
		&options,
		&bytesTransferred);
	if (NT_SUCCESS(status) &&
		bytesTransferred != inputBufferLength + outputBufferLength)
	{
		status = STATUS_DEVICE_DATA_ERROR;
	}

	WdfObjectDelete(request);
	return status;
}

NTSTATUS
ReadDescriptorFromRegistry(
	WDFDEVICE Device
)
/*++
Routine Description:
	Read HID report descriptor from registry
Arguments:
	device - pointer to a device object.
Return Value:
	NT status code.
--*/
{
	WDFKEY          hKey = NULL;
	NTSTATUS        status;
	UNICODE_STRING  xRevertName;
	UNICODE_STRING  yRevertName;
	UNICODE_STRING  xYExchangeName;
	UNICODE_STRING  xMinName;
	UNICODE_STRING  xMaxName;
	UNICODE_STRING  yMinName;
	UNICODE_STRING  yMaxName;
	PDEVICE_CONTEXT deviceContext;
	WDF_OBJECT_ATTRIBUTES   attributes;

	deviceContext = GetDeviceContext(Device);

	status = WdfDeviceOpenRegistryKey(Device,
		PLUGPLAY_REGKEY_DEVICE,
		KEY_READ,
		WDF_NO_OBJECT_ATTRIBUTES,
		&hKey);

	if (NT_SUCCESS(status)) {

		RtlInitUnicodeString(&xRevertName, L"XRevert");
		RtlInitUnicodeString(&yRevertName, L"YRevert");
		RtlInitUnicodeString(&xYExchangeName, L"XYExchange");
		RtlInitUnicodeString(&xMinName, L"XMin");
		RtlInitUnicodeString(&xMaxName, L"XMax");
		RtlInitUnicodeString(&yMinName, L"YMin");
		RtlInitUnicodeString(&yMaxName, L"YMax");

		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		attributes.ParentObject = Device;

		status = WdfRegistryQueryULong(hKey, &xRevertName, &XRevert);
		status = WdfRegistryQueryULong(hKey, &yRevertName, &YRevert);
		status = WdfRegistryQueryULong(hKey, &xYExchangeName, &XYExchange);
		status = WdfRegistryQueryULong(hKey, &xMinName, &XMin);
		status = WdfRegistryQueryULong(hKey, &xMaxName, &XMax);
		status = WdfRegistryQueryULong(hKey, &yMinName, &YMin);
		status = WdfRegistryQueryULong(hKey, &yMaxName, &YMax);

		WdfRegistryClose(hKey);
	}

	return status;
}
