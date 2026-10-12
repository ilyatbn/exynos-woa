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
    WLAN_JOIN_CONFIG Join;      /* milestone 6 target (Parameters\Ssid + Passphrase/Psk), PMK only */
    BOOLEAN         JoinTried;
    volatile LONG   PollBusy;   /* one poller at a time: the timer re-enqueues the item while a rescan runs */
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

/* Milestone 6 target from Parameters: Ssid, Passphrase (or Psk). The passphrase buffer is wiped after use. */
static VOID
WlanReadJoin(_In_ WDFDRIVER Driver, _Out_ PWLAN_JOIN_CONFIG J, _Inout_ PWLAN_REPORT R)
{
    WDFKEY         Key;
    WCHAR          SsidBuf[40], PassBuf[72], PskBuf[72];
    UNICODE_STRING Name, Ssid, Pass, Psk;
    struct { PCWSTR Name; PUNICODE_STRING Value; PWCHAR Buf; USHORT Size; } Items[] = {
        { L"Ssid", &Ssid, SsidBuf, sizeof(SsidBuf) },
        { L"Passphrase", &Pass, PassBuf, sizeof(PassBuf) },
        { L"Psk", &Psk, PskBuf, sizeof(PskBuf) },
    };
    ULONG I;

    RtlZeroMemory(J, sizeof(*J));
    (VOID)WlanJoinSelfTest(R);
    for (I = 0; I < ARRAYSIZE(Items); I++) {
        Items[I].Value->Buffer        = Items[I].Buf;
        Items[I].Value->Length        = 0;
        Items[I].Value->MaximumLength = Items[I].Size;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(Driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
        return;
    }
    for (I = 0; I < ARRAYSIZE(Items); I++) {
        RtlInitUnicodeString(&Name, Items[I].Name);
        if (!NT_SUCCESS(WdfRegistryQueryUnicodeString(Key, &Name, NULL, Items[I].Value))) {
            Items[I].Value->Length = 0;
        }
        /* REG_SZ values may carry their terminating NUL in Length */
        while (Items[I].Value->Length >= sizeof(WCHAR) &&
               Items[I].Value->Buffer[Items[I].Value->Length / sizeof(WCHAR) - 1] == 0) {
            Items[I].Value->Length -= sizeof(WCHAR);
        }
    }
    WdfRegistryClose(Key);
    (VOID)WlanJoinPrepare(&Ssid, &Pass, &Psk, J, R);
    RtlSecureZeroMemory(PassBuf, sizeof(PassBuf));
    RtlSecureZeroMemory(PskBuf, sizeof(PskBuf));
}

NTSTATUS
WlanEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    WLAN_REPORT                     *R;
    WLAN_PROBE                       P;
    volatile UCHAR                  *Cfg = NULL;
    ULONG                            Bar0Lo = 0;
    NTSTATUS                         Status;
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

    ReportLine(R, "Exynos9820Wlan milestone-3 (ACPI\\SAMS9825)\r\n");
    /* _CRS contract, endpoint header, BAR0, chipid (WlanCommon.c, M9 T2: moved out of here verbatim) */
    Status = WlanProbeChip(Translated, R, &P, &Cfg, &Bar0Lo);
    if (P.Result == WLAN_RESULT_OK) {
        ReportWriteFile(R);
        /* Milestone 2 (notes/m12-wifi-m2.md): firmware download + ARM start. BAR2 = 64-bit TCM window. */
        if ((P.Bar[2] & EP_CFG_BAR_TYPE_MASK) == EP_CFG_BAR_TYPE_64 && P.Bar[3] == 0) {
            WlanReadConfig(WdfDeviceGetDriver(Device), &DlCfg);
            WlanReadJoin(WdfDeviceGetDriver(Device), &Ctx->Join, R);
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
    /* run m6-1: a second instance drained the D2H ring concurrently during the 2 s rescan (KMDF clears the
       enqueued state before calling us, so the 100 ms timer queues it again) */
    if (InterlockedCompareExchange(&Ctx->PollBusy, 1, 0) != 0) {
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
        if (Ctx->Join.Valid && !Ctx->JoinTried) {
            Ctx->JoinTried = TRUE;
            (VOID)WlanMsgJoin(&Ctx->Msg, R, &Ctx->Join);
            ReportWriteFile(R);
        }
    }
    if ((Ctx->Ticks % (60000 / WLAN_POLL_MS)) == 0) {
        ReportLine(R, "[run] t=%lu s: alive, %lu D2H items polled\r\n", Ctx->Ticks * WLAN_POLL_MS / 1000, Ctx->Polled);
        ReportWriteFile(R);
    }
    InterlockedExchange(&Ctx->PollBusy, 0);
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
