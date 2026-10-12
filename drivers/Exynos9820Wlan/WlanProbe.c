/*  WlanProbe.c - S10e BCM4375 WLAN, probe, firmware download (WlanDl.c) and msgbuf bring-up (WlanMsg.c) (KMDF, no NDIS).

    Binds ACPI\SAMS9825 (Dsdt.asl Device (WLAN), present only when Beyond0LtePcieDxe trained the
    PCIe link and read a Broadcom vendor ID this boot), maps the two _CRS memory windows and reports:
      - the endpoint config header (VID/DID, COMMAND, class/rev, subsystem, BAR0..BAR5) read through
        the CFG0 iATU window,
      - the ChipCommon chipid read through BAR0 + 0x3000 (only when MEM decode is on and BAR0 is a
        >= 16 KB memory BAR inside the MEM window the firmware assigned).
    It WRITES NOTHING to the device: no BAR sizing, no COMMAND change, no interrupt connect.

    Report sinks (README.md "Reading the result"):
      1. \SystemRoot\System32\LogFiles\Exynos9820Wlan.txt  (C:\Windows\System32\LogFiles\...)
      2. HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Wlan\Parameters  (REG_SZ Report + DWORDs)
      3. DbgPrintEx (kernel debugger / DbgView)
    The device always starts (Code 0) once its resources are sane, so a failed chip read is a
    report line, not a yellow bang.
*/

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include "WlanDl.h"
#include "WlanMsg.h"

typedef struct _WLAN_DEVICE_CONTEXT {
    WDFDMAENABLER   DmaEnabler; /* milestone 3 msgbuf memory (NULL if creation failed) */
    /* milestone 5: resident firmware (notes/m12-wifi-m5.md) */
    WLAN_MSG_CTX    Msg;        /* owns rings, buffer, BAR0/BAR2 while Msg.Resident */
    volatile UCHAR *Cfg;        /* EP config window kept for Msg.Bus.Cfg (BME off at quiesce) */
    WDFTIMER        PollTimer;  /* WLAN_POLL_MS -> PollWork */
    WDFWORKITEM     PollWork;
    PWLAN_REPORT    RunR;       /* Exynos9820Wlan-run.txt */
    ULONG           Ticks;
    ULONG           RescanTicks;
    ULONG           Polled;     /* D2H items consumed at runtime */
} WLAN_DEVICE_CONTEXT, *PWLAN_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WLAN_DEVICE_CONTEXT, WlanGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD WlanEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE WlanEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE WlanEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY         WlanEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT          WlanEvtD0Exit;
EVT_WDF_TIMER                   WlanPollTimer;
EVT_WDF_WORKITEM                WlanPollWork;

typedef struct _WLAN_PROBE {
    ULONG   VendorDevice;
    ULONG   Command;
    ULONG   ClassRev;
    ULONG   Subsystem;
    ULONG   Bar[EP_CFG_BAR_COUNT];
    ULONG   ChipId;          /* 0 = not read */
    ULONG   Result;          /* 0 = chip id matched 0x4375; see WLAN_RESULT_* */
} WLAN_PROBE, *PWLAN_PROBE;

#define WLAN_RESULT_OK              0
#define WLAN_RESULT_NO_RESOURCES    1
#define WLAN_RESULT_MAP_FAILED      2
#define WLAN_RESULT_CFG_DEAD        3
#define WLAN_RESULT_NOT_BROADCOM    4
#define WLAN_RESULT_BAR0_UNUSABLE   5
#define WLAN_RESULT_CHIPID_MISMATCH 6

VOID
ReportLine(_Inout_ PWLAN_REPORT R, _In_z_ _Printf_format_string_ PCSTR Fmt, ...)
{
    va_list Args;
    size_t  Left = sizeof(R->Text) - R->Used;
    size_t  Remaining = 0;

    va_start(Args, Fmt);
    if (Left > 1 &&
        NT_SUCCESS(RtlStringCbVPrintfExA(R->Text + R->Used, Left, NULL, &Remaining, 0, Fmt, Args))) {
        /* one DbgPrint per line: a single DbgPrint of the whole report would be cut at 512 bytes */
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Exynos9820Wlan: %s", R->Text + R->Used);
        R->Used = sizeof(R->Text) - Remaining;
    }
    va_end(Args);
}

VOID
ReportWriteFile(_In_ PWLAN_REPORT R)
{
    UNICODE_STRING    Name;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK   Iosb;
    HANDLE            File;
    NTSTATUS          Status;

    RtlInitUnicodeString(&Name, (R->FileName != NULL) ? R->FileName : WLAN_REPORT_FILE);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_WRITE | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OVERWRITE_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, NULL, 0);
    if (!NT_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Exynos9820Wlan: report file %08x\n", Status);
        return;
    }

    (VOID)ZwWriteFile(File, NULL, NULL, NULL, &Iosb, R->Text, (ULONG)R->Used, NULL, NULL);
    ZwClose(File);
}

static VOID
RegSetDword(_In_ WDFKEY Key, _In_ PCWSTR Name, _In_ ULONG Value)
{
    UNICODE_STRING N;

    RtlInitUnicodeString(&N, Name);
    (VOID)WdfRegistryAssignULong(Key, &N, Value);
}

static VOID
ReportWriteRegistry(_In_ WDFDRIVER Driver, _In_ PWLAN_REPORT R, _In_ PWLAN_PROBE P, _In_opt_ PWLAN_DL_RESULT Dl)
{
    WDFKEY         Key;
    UNICODE_STRING Name;
    UNICODE_STRING Value;
    ANSI_STRING    Ansi;
    NTSTATUS       Status;
    ULONG          I;
    WCHAR          BarName[8];

    Status = WdfDriverOpenParametersRegistryKey(Driver, KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &Key);
    if (!NT_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Exynos9820Wlan: Parameters key %08x\n", Status);
        return;
    }

    RegSetDword(Key, L"Result", P->Result);
    RegSetDword(Key, L"VendorDevice", P->VendorDevice);
    RegSetDword(Key, L"Command", P->Command);
    RegSetDword(Key, L"ClassRev", P->ClassRev);
    RegSetDword(Key, L"Subsystem", P->Subsystem);
    RegSetDword(Key, L"ChipId", P->ChipId);
    if (Dl != NULL) {
        RegSetDword(Key, L"DlResult", Dl->DlResult);
        RegSetDword(Key, L"RamSize", Dl->RamSize);
        RegSetDword(Key, L"OtpVid", Dl->OtpVid);
        RegSetDword(Key, L"NvramToken", Dl->NvramToken);
        RegSetDword(Key, L"SharedAddr", Dl->SharedAddr);
        RegSetDword(Key, L"SharedFlags", Dl->SharedFlags);
        RegSetDword(Key, L"ArmReleased", Dl->ArmReleased);
    }
    for (I = 0; I < EP_CFG_BAR_COUNT; I++) {
        if (NT_SUCCESS(RtlStringCchPrintfW(BarName, ARRAYSIZE(BarName), L"Bar%u", I))) {
            RegSetDword(Key, BarName, P->Bar[I]);
        }
    }

    Ansi.Buffer        = R->Text;
    Ansi.Length        = (USHORT)R->Used;
    Ansi.MaximumLength = (USHORT)sizeof(R->Text);
    if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&Value, &Ansi, TRUE))) {
        RtlInitUnicodeString(&Name, L"Report");
        (VOID)WdfRegistryAssignUnicodeString(Key, &Name, &Value);
        RtlFreeUnicodeString(&Value);
    }

    WdfRegistryClose(Key);
}

/* Parameters key settings: Download (default 1), OtpProbe (default 1), NvramFile, FirmwareFile
   (empty = automatic / default), Msgbuf (default 1), HostCap (default WLAN_HOSTCAP_AUTO),
   DoorbellDar (default 0), Scan (default 1), Resident (default 1), RescanSeconds (default 30). */
static VOID
WlanReadConfig(_In_ WDFDRIVER Driver, _Out_ PWLAN_DL_CONFIG DlCfg)
{
    WDFKEY         Key;
    UNICODE_STRING Name;
    UNICODE_STRING Value;
    ULONG          V;

    RtlZeroMemory(DlCfg, sizeof(*DlCfg));
    DlCfg->Download = 1;
    DlCfg->OtpProbe = 1;
    DlCfg->Msgbuf   = 1;                    /* defaults documented in WlanDl.h */
    DlCfg->HostCap  = WLAN_HOSTCAP_AUTO;
    DlCfg->Scan     = 1;
    DlCfg->Resident = 1;
    DlCfg->RescanSeconds = 30;
    DlCfg->CacheMaint = 1;
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(Driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
        return;
    }
    RtlInitUnicodeString(&Name, L"Download");
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
        DlCfg->Download = V;
    }
    RtlInitUnicodeString(&Name, L"OtpProbe");
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
        DlCfg->OtpProbe = V;
    }
    RtlInitUnicodeString(&Name, L"Msgbuf");
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
        DlCfg->Msgbuf = V;
    }
    RtlInitUnicodeString(&Name, L"HostCap");
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
        DlCfg->HostCap = V;
    }
    RtlInitUnicodeString(&Name, L"Scan");
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
        DlCfg->Scan = V;
    }
    RtlInitUnicodeString(&Name, L"Resident");
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
        DlCfg->Resident = V;
    }
    RtlInitUnicodeString(&Name, L"RescanSeconds");
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
        DlCfg->RescanSeconds = V;
    }
    RtlInitUnicodeString(&Name, L"CacheMaint");
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
        DlCfg->CacheMaint = V;
    }
    RtlInitUnicodeString(&Name, L"DoorbellDar");
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
        DlCfg->DoorbellDar = V;
    }
    RtlInitUnicodeString(&Name, L"NvramFile");
    Value.Buffer        = DlCfg->NvramFile;
    Value.Length        = 0;
    Value.MaximumLength = (USHORT)(sizeof(DlCfg->NvramFile) - sizeof(WCHAR));
    if (!NT_SUCCESS(WdfRegistryQueryUnicodeString(Key, &Name, NULL, &Value))) {
        DlCfg->NvramFile[0] = 0;
    }
    RtlInitUnicodeString(&Name, L"FirmwareFile");
    Value.Buffer        = DlCfg->FirmwareFile;
    Value.Length        = 0;
    Value.MaximumLength = (USHORT)(sizeof(DlCfg->FirmwareFile) - sizeof(WCHAR));
    if (!NT_SUCCESS(WdfRegistryQueryUnicodeString(Key, &Name, NULL, &Value))) {
        DlCfg->FirmwareFile[0] = 0;
    }
    WdfRegistryClose(Key);
}

static ULONG
CfgRead(_In_ volatile UCHAR *Cfg, _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Cfg + Offset));
}

NTSTATUS
WlanEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    WLAN_REPORT                     *R;
    WLAN_PROBE                       P;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR  D;
    PHYSICAL_ADDRESS                 Phys[2];
    ULONG                            Len[2];
    ULONG                            MemCount = 0;
    ULONG                            IrqCount = 0;
    ULONG                            IrqVector = 0;
    ULONG                            Count;
    ULONG                            I;
    volatile UCHAR                  *Cfg = NULL;
    volatile UCHAR                  *Bar0 = NULL;
    PHYSICAL_ADDRESS                 Bar0Phys;
    ULONG                            Bar0Lo;
    NTSTATUS                         Status = STATUS_SUCCESS;
    WLAN_DL_CONFIG                   DlCfg;
    WLAN_DL_RESULT                   Dl;
    BOOLEAN                          DlRan = FALSE;
    PWLAN_DEVICE_CONTEXT             Ctx = WlanGetContext(Device);
    PWLAN_MSG_CTX                    Msg = &Ctx->Msg;

    UNREFERENCED_PARAMETER(Raw);
    RtlZeroMemory(&Dl, sizeof(Dl));

    R = (WLAN_REPORT *)ExAllocatePool2(POOL_FLAG_PAGED, sizeof(*R), 'nlWE');
    if (R == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&P, sizeof(P));
    Phys[0].QuadPart = Phys[1].QuadPart = 0;
    Len[0] = Len[1] = 0;
    ReportLine(R, "Exynos9820Wlan milestone-3 (ACPI\\SAMS9825)\r\n");

    Count = WdfCmResourceListGetCount(Translated);
    for (I = 0; I < Count; I++) {
        D = WdfCmResourceListGetDescriptor(Translated, I);
        if (D == NULL) {
            continue;
        }
        if (D->Type == CmResourceTypeMemory) {
            if (MemCount < 2) {
                Phys[MemCount] = D->u.Memory.Start;
                Len[MemCount]  = D->u.Memory.Length;
            }
            MemCount++;
        } else if (D->Type == CmResourceTypeInterrupt) {
            IrqVector = D->u.Interrupt.Vector;
            IrqCount++;
        }
    }

    ReportLine(R, "resources: %lu memory, %lu interrupt (vector %lu, not connected)\r\n",
               MemCount, IrqCount, IrqVector);
    for (I = 0; I < 2 && I < MemCount; I++) {
        ReportLine(R, "  mem%lu: 0x%I64X + 0x%lX\r\n", I, Phys[I].QuadPart, Len[I]);
    }

    /* _CRS contract: [0] = CFG window, [1] = MEM window, exactly as the DSDT publishes them. */
    if (MemCount < 2 ||
        (ULONGLONG)Phys[0].QuadPart != WLAN_CFG_WINDOW_BASE || Len[0] < WLAN_CFG_WINDOW_SIZE ||
        (ULONGLONG)Phys[1].QuadPart != WLAN_MEM_WINDOW_BASE || Len[1] < WLAN_MEM_WINDOW_SIZE) {
        ReportLine(R, "RESULT %u: _CRS does not match the DSDT contract - nothing mapped\r\n",
                   WLAN_RESULT_NO_RESOURCES);
        P.Result = WLAN_RESULT_NO_RESOURCES;
        Status   = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Report;
    }

    Cfg = (volatile UCHAR *)MmMapIoSpaceEx(Phys[0], WLAN_CFG_WINDOW_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Cfg == NULL) {
        ReportLine(R, "RESULT %u: MmMapIoSpaceEx(CFG) failed\r\n", WLAN_RESULT_MAP_FAILED);
        P.Result = WLAN_RESULT_MAP_FAILED;
        goto Report;
    }

    P.VendorDevice = CfgRead(Cfg, EP_CFG_VENDOR_DEVICE);
    P.Command      = CfgRead(Cfg, EP_CFG_COMMAND);
    P.ClassRev     = CfgRead(Cfg, EP_CFG_CLASS_REV);
    P.Subsystem    = CfgRead(Cfg, EP_CFG_SUBSYSTEM);
    for (I = 0; I < EP_CFG_BAR_COUNT; I++) {
        P.Bar[I] = CfgRead(Cfg, EP_CFG_BAR0 + 4 * I);
    }

    ReportLine(R, "endpoint %04lX:%04lX (expected 14E4:%04X, device id UNVERIFIED) command 0x%08lX\r\n",
               P.VendorDevice & 0xFFFF, P.VendorDevice >> 16, BCM4375_EXPECTED_PCI_ID_UNVERIFIED,
               P.Command);
    ReportLine(R, "class/rev 0x%08lX subsystem 0x%08lX\r\n", P.ClassRev, P.Subsystem);
    for (I = 0; I < EP_CFG_BAR_COUNT; I++) {
        ReportLine(R, "  BAR%lu = 0x%08lX\r\n", I, P.Bar[I]);
    }

    if (P.VendorDevice == 0xFFFFFFFFUL || P.VendorDevice == 0) {
        ReportLine(R, "RESULT %u: config space reads 0x%08lX - link or iATU not latched\r\n",
                   WLAN_RESULT_CFG_DEAD, P.VendorDevice);
        P.Result = WLAN_RESULT_CFG_DEAD;
        goto Report;
    }

    if ((P.VendorDevice & 0xFFFF) != BCM4375_PCI_VENDOR_ID) {
        ReportLine(R, "RESULT %u: vendor is not Broadcom\r\n", WLAN_RESULT_NOT_BROADCOM);
        P.Result = WLAN_RESULT_NOT_BROADCOM;
        goto Report;
    }

    /* BAR0 must be a memory BAR the firmware placed inside the MEM window, decode on, 16 KB fits. */
    Bar0Lo = P.Bar[0] & EP_CFG_BAR_MEM_MASK;
    if ((P.Bar[0] & EP_CFG_BAR_IO) != 0 ||
        ((P.Bar[0] & EP_CFG_BAR_TYPE_MASK) == EP_CFG_BAR_TYPE_64 && P.Bar[1] != 0) ||
        (P.Command & EP_CFG_COMMAND_MEM_EN) == 0 ||
        (ULONGLONG)Bar0Lo < WLAN_MEM_WINDOW_BASE ||
        (ULONGLONG)Bar0Lo + PCIE_BAR0_MIN_SIZE > WLAN_MEM_WINDOW_BASE + WLAN_MEM_WINDOW_SIZE) {
        ReportLine(R, "RESULT %u: BAR0 0x%08lX/0x%08lX not usable (command 0x%08lX)\r\n",
                   WLAN_RESULT_BAR0_UNUSABLE, P.Bar[0], P.Bar[1], P.Command);
        P.Result = WLAN_RESULT_BAR0_UNUSABLE;
        goto Report;
    }

    Bar0Phys.QuadPart = Bar0Lo;
    Bar0 = (volatile UCHAR *)MmMapIoSpaceEx(Bar0Phys, PCIE_BAR0_MIN_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Bar0 == NULL) {
        ReportLine(R, "RESULT %u: MmMapIoSpaceEx(BAR0) failed\r\n", WLAN_RESULT_MAP_FAILED);
        P.Result = WLAN_RESULT_MAP_FAILED;
        goto Report;
    }

    P.ChipId = READ_REGISTER_ULONG((volatile ULONG *)(Bar0 + PCIE_BAR0_CCCORE_OFF + CC_CHIPID_REG));
    ReportLine(R, "chipcommon chipid 0x%08lX: id 0x%04lX rev %lu (expected id 0x%04X)\r\n",
               P.ChipId, P.ChipId & CC_CHIPID_MASK, (P.ChipId >> 16) & 0xF, BCM4375_CHIP_ID);
    if ((P.ChipId & CC_CHIPID_MASK) == BCM4375_CHIP_ID) {
        ReportLine(R, "RESULT 0: BCM4375 alive\r\n");
        P.Result = WLAN_RESULT_OK;
        ReportWriteFile(R);
        /* Milestone 2 (notes/m12-wifi-m2.md): firmware download + ARM start. BAR2 = 64-bit TCM window. */
        if ((P.Bar[2] & EP_CFG_BAR_TYPE_MASK) == EP_CFG_BAR_TYPE_64 && P.Bar[3] == 0) {
            WlanReadConfig(WdfDeviceGetDriver(Device), &DlCfg);
            RtlZeroMemory(Msg, sizeof(*Msg));
            Msg->DmaEnabler = Ctx->DmaEnabler;
            Msg->Config     = &DlCfg;
            Msg->Result     = MSG_SKIPPED;
            Msg->ScanResult = SCAN_SKIPPED;
            /* milestone 3 (notes/m12-wifi-m3.md) runs inside WlanDlRun once pcie_shared is up */
            WlanDlRun(R, Cfg, Bar0Lo, P.Bar[2] & EP_CFG_BAR_MEM_MASK, P.Command, &DlCfg, &Dl, WlanMsgRun, Msg);
            if (Msg->Resident) {
                Ctx->Cfg         = Cfg;   /* Msg->Bus.Cfg points into it: unmapped in ReleaseHardware */
                Cfg              = NULL;
                Ctx->RescanTicks = DlCfg.RescanSeconds * (1000 / WLAN_POLL_MS);
            }
            DlRan = TRUE;
        } else {
            ReportLine(R, "DL 2: BAR2 0x%08lX/0x%08lX is not a 64-bit BAR below 4 GB\r\n", P.Bar[2], P.Bar[3]);
        }
    } else {
        ReportLine(R, "RESULT %u: chip id mismatch\r\n", WLAN_RESULT_CHIPID_MISMATCH);
        P.Result = WLAN_RESULT_CHIPID_MISMATCH;
    }

Report:
    if (Bar0 != NULL) {
        MmUnmapIoSpace((PVOID)Bar0, PCIE_BAR0_MIN_SIZE);
    }
    if (Cfg != NULL) {
        MmUnmapIoSpace((PVOID)Cfg, WLAN_CFG_WINDOW_SIZE);
    }

    ReportWriteFile(R);
    ReportWriteRegistry(WdfDeviceGetDriver(Device), R, &P, DlRan ? &Dl : NULL);
    ExFreePoolWithTag(R, 'nlWE');
    return Status;
}

/* Run-time report: appended, the whole file rewritten each time (WLAN_REPORT_MAX caps it). */
static PWLAN_REPORT
RunReport(_Inout_ PWLAN_DEVICE_CONTEXT Ctx)
{
    if (Ctx->RunR == NULL) {
        Ctx->RunR = (PWLAN_REPORT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(WLAN_REPORT), 'rlWE');
        if (Ctx->RunR != NULL) {
            Ctx->RunR->FileName = WLAN_RUN_REPORT_FILE;
            ReportLine(Ctx->RunR, "Exynos9820Wlan milestone-5 run log (resident firmware, poll %u ms)\r\n", WLAN_POLL_MS);
        }
    }
    return Ctx->RunR;
}

/* Timer (DISPATCH) only queues the passive work item; the item does the ring work. */
VOID
WlanPollTimer(_In_ WDFTIMER Timer)
{
    WdfWorkItemEnqueue(WlanGetContext(WdfTimerGetParentObject(Timer))->PollWork);
}

VOID
WlanPollWork(_In_ WDFWORKITEM WorkItem)
{
    PWLAN_DEVICE_CONTEXT Ctx = WlanGetContext(WdfWorkItemGetParentObject(WorkItem));
    PWLAN_REPORT         R   = RunReport(Ctx);
    ULONG                N;

    if (R == NULL || !Ctx->Msg.Resident || Ctx->Msg.Quiesced) {
        return;
    }
    N = WlanMsgPoll(&Ctx->Msg, R);
    Ctx->Polled += N;
    Ctx->Ticks++;
    if (N != 0) {
        ReportWriteFile(R);
    }
    if (Ctx->RescanTicks != 0 && Ctx->Ticks == Ctx->RescanTicks) {
        ReportLine(R, "[run] t=%lu s: rescan with the resident firmware (%lu D2H items polled so far)\r\n",
                   Ctx->Ticks * WLAN_POLL_MS / 1000, Ctx->Polled);
        (VOID)WlanMsgRescan(&Ctx->Msg, R);
        ReportWriteFile(R);
    }
    if ((Ctx->Ticks % (60000 / WLAN_POLL_MS)) == 0) {
        ReportLine(R, "[run] t=%lu s: alive, %lu D2H items polled\r\n", Ctx->Ticks * WLAN_POLL_MS / 1000, Ctx->Polled);
        ReportWriteFile(R);
    }
}

NTSTATUS
WlanEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PWLAN_DEVICE_CONTEXT Ctx = WlanGetContext(Device);
    PWLAN_REPORT         R;

    if (Ctx->Msg.Resident) {
        R = RunReport(Ctx);
        if (Ctx->Msg.Quiesced) {
            if (R != NULL) {
                ReportLine(R, "[run] D0 entry from D%d: firmware was quiesced; a restart needs a new PrepareHardware\r\n",
                           (int)PreviousState - (int)WdfPowerDeviceD0);
                ReportWriteFile(R);
            }
        } else {
            WdfTimerStart(Ctx->PollTimer, WDF_REL_TIMEOUT_IN_MS(WLAN_POLL_MS));
        }
    }
    return STATUS_SUCCESS;
}

/* Leaving D0 (sleep, shutdown, removal): stop polling and quiesce - the firmware must not DMA while the
   system changes power state. ReleaseHardware frees. */
NTSTATUS
WlanEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PWLAN_DEVICE_CONTEXT Ctx = WlanGetContext(Device);
    PWLAN_REPORT         R;

    WdfTimerStop(Ctx->PollTimer, TRUE);
    WdfWorkItemFlush(Ctx->PollWork);
    if (Ctx->Msg.Resident && !Ctx->Msg.Quiesced) {
        R = RunReport(Ctx);
        if (R != NULL) {
            ReportLine(R, "[run] D0 exit to state %d after %lu s\r\n", (int)TargetState, Ctx->Ticks * WLAN_POLL_MS / 1000);
            WlanMsgQuiesce(&Ctx->Msg, R);
            ReportWriteFile(R);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
WlanEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PWLAN_DEVICE_CONTEXT Ctx = WlanGetContext(Device);
    PWLAN_REPORT         R;

    UNREFERENCED_PARAMETER(Translated);
    WdfTimerStop(Ctx->PollTimer, TRUE);
    WdfWorkItemFlush(Ctx->PollWork);
    if (Ctx->Msg.Resident) {
        R = RunReport(Ctx);
        if (!Ctx->Msg.Quiesced && R != NULL) {
            WlanMsgQuiesce(&Ctx->Msg, R);
        }
        WlanMsgFree(&Ctx->Msg);
    }
    if (Ctx->Cfg != NULL && !Ctx->Msg.Resident) {
        MmUnmapIoSpace((PVOID)Ctx->Cfg, WLAN_CFG_WINDOW_SIZE);
        Ctx->Cfg = NULL;
    }
    if (Ctx->RunR != NULL) {
        ReportLine(Ctx->RunR, "[run] released\r\n");
        ReportWriteFile(Ctx->RunR);
        ExFreePoolWithTag(Ctx->RunR, 'rlWE');
        Ctx->RunR = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
WlanEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDFDEVICE                    Device;
    WDF_OBJECT_ATTRIBUTES        Attr;
    WDF_DMA_ENABLER_CONFIG       Dma;
    NTSTATUS                     Status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = WlanEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = WlanEvtReleaseHardware;
    Pnp.EvtDeviceD0Entry         = WlanEvtD0Entry;
    Pnp.EvtDeviceD0Exit          = WlanEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, WLAN_DEVICE_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    /* 32-bit packet profile: the common buffer lands below 4 GB, as the 32-bit ring addresses need.
       A failure only disables milestone 3 (WlanMsgRun reports MSG_NO_DMA). */
    WdfDeviceSetAlignmentRequirement(Device, FILE_QUAD_ALIGNMENT);   /* common buffers are page-aligned anyway; WlanMsgRun checks */
    WDF_DMA_ENABLER_CONFIG_INIT(&Dma, WdfDmaProfilePacket, MSG_DMA_SIZE);
    if (!NT_SUCCESS(WdfDmaEnablerCreate(Device, &Dma, WDF_NO_OBJECT_ATTRIBUTES, &WlanGetContext(Device)->DmaEnabler))) {
        WlanGetContext(Device)->DmaEnabler = NULL;
    }
    /* milestone 5 poll: periodic timer -> passive work item (both children of the device) */
    {
        WDF_TIMER_CONFIG      Tc;
        WDF_WORKITEM_CONFIG   Wc;
        WDF_OBJECT_ATTRIBUTES A;

        WDF_WORKITEM_CONFIG_INIT(&Wc, WlanPollWork);
        WDF_OBJECT_ATTRIBUTES_INIT(&A);
        A.ParentObject = Device;
        Status = WdfWorkItemCreate(&Wc, &A, &WlanGetContext(Device)->PollWork);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        WDF_TIMER_CONFIG_INIT_PERIODIC(&Tc, WlanPollTimer, WLAN_POLL_MS);
        Tc.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&A);
        A.ParentObject = Device;
        Status = WdfTimerCreate(&Tc, &A, &WlanGetContext(Device)->PollTimer);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, WlanEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
