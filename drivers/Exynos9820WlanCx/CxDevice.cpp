/*  CxDevice.cpp - Exynos9820WlanCx: driver/device objects, BCM4375 bring-up (shared C code from
    drivers/Exynos9820Wlan), the worker thread and the runtime log. notes/m12-wifi-wificx.md.

    Order (WiFiCx sample driver.cpp:99-180, learn.microsoft.com "Develop a WiFiCx client driver"):
    NetDeviceInitConfig -> WifiDeviceInitConfig -> WdfDeviceCreate -> WifiDeviceInitialize; capabilities from
    EvtDevicePrepareHardware; WiFiCx then calls EvtWifiDeviceCreateAdapter (CxAdapter.cpp).
*/

#include "Cx.h"

extern "C" DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD        CxEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE  CxEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE  CxEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY          CxEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT           CxEvtD0Exit;
static KSTART_ROUTINE                   CxWorker;

/* ---------------------------------------------------------------- runtime log */

static const PCWSTR CxRunOld[4] = {
    L"\\SystemRoot\\System32\\LogFiles\\Exynos9820WlanCx-run.1.txt",
    L"\\SystemRoot\\System32\\LogFiles\\Exynos9820WlanCx-run.2.txt",
    L"\\SystemRoot\\System32\\LogFiles\\Exynos9820WlanCx-run.3.txt",
    L"\\SystemRoot\\System32\\LogFiles\\Exynos9820WlanCx-run.4.txt",
};

/* Worker-thread (or worker-stopped) context only: WlanMsg.c writes the same report without a lock. */
VOID
CxLog(PCX_DEVICE Dev, PCSTR Fmt, ...)
{
    CHAR    Line[512];
    va_list Args;

    if (Dev->RunR == NULL) {
        return;
    }
    va_start(Args, Fmt);
    Line[0] = 0;
    (VOID)RtlStringCbVPrintfA(Line, sizeof(Line), Fmt, Args);   /* a truncated line is still NUL-terminated */
    ReportLine(Dev->RunR, "%s", Line);
    va_end(Args);
    Dev->RunDirty = TRUE;
}

/* Writes the run log; once it is 3/4 full the text goes to Exynos9820WlanCx-run.<n>.txt (n = 1..4 cycling) and the
   live file starts again, so a long run keeps its latest lines. */
VOID
CxLogFlush(PCX_DEVICE Dev)
{
    PWLAN_REPORT R = Dev->RunR;

    if (R == NULL) {
        return;
    }
    if (R->Used >= CX_LOG_ROTATE) {
        R->FileName = CxRunOld[Dev->RunGen % ARRAYSIZE(CxRunOld)];
        ReportWriteFile(R);
        R->FileName = WLAN_RUN_REPORT_FILE;
        R->Used     = 0;
        ReportLine(R, "[log] continued; previous part in Exynos9820WlanCx-run.%lu.txt\r\n",
                   Dev->RunGen % ARRAYSIZE(CxRunOld) + 1);
        Dev->RunGen++;
    }
    ReportWriteFile(R);
    Dev->RunDirty = FALSE;
}

VOID
CxWake(PCX_DEVICE Dev)
{
    KeSetEvent(&Dev->Wake, IO_NO_INCREMENT, FALSE);
}

/* ---------------------------------------------------------------- worker */

static VOID
CxWorker(PVOID Context)
{
    PCX_DEVICE    Dev = (PCX_DEVICE)Context;
    LARGE_INTEGER Timeout;
    ULONG         Ticks = 0, Polled = 0, FlushTicks = 0;
    KIRQL         Irql;

    CxLog(Dev, "[cx] worker started\r\n");
    while (InterlockedCompareExchange(&Dev->Stop, 0, 0) == 0) {
        PCX_CMD Cmd;

        Timeout.QuadPart = -10000LL * (Dev->Linked ? CX_LINK_WAIT_MS : CX_IDLE_WAIT_MS);
        (VOID)KeWaitForSingleObject(&Dev->Wake, Executive, KernelMode, FALSE, &Timeout);
        if (InterlockedCompareExchange(&Dev->Stop, 0, 0) != 0) {
            break;
        }
        /* commands, one at a time, in arrival order */
        for (;;) {
            Cmd = NULL;
            KeAcquireSpinLock(&Dev->CmdLock, &Irql);
            if (!IsListEmpty(&Dev->CmdList)) {
                Cmd = CONTAINING_RECORD(RemoveHeadList(&Dev->CmdList), CX_CMD, Link);
            }
            KeReleaseSpinLock(&Dev->CmdLock, Irql);
            if (Cmd == NULL) {
                break;
            }
            CxRunCommand(Dev, Cmd);
            ExFreePoolWithTag(Cmd, CX_POOL_TAG);
        }
        if (Dev->Msg.Resident && !Dev->Msg.Quiesced) {
            CxTxDrain(Dev);                                   /* W3: queued tx frames -> flowring */
            Polled += WlanMsgPoll(&Dev->Msg, Dev->RunR);      /* rx -> RxFifo, tx completions, events */
            CxTxDrain(Dev);                                   /* frames that waited for a tx buffer */
        }
        if (Dev->TxLayoutLog != Dev->TxLayoutSeen) {
            Dev->TxLayoutSeen = Dev->TxLayoutLog;
            CxLog(Dev, "[tx] packet layout #%ld: layer2 type %u (%s), first bytes %02X:%02X:%02X:%02X:%02X:%02X "
                  "%02X:%02X:%02X:%02X:%02X:%02X type %02X%02X\r\n", Dev->TxLayoutSeen, Dev->TxLayout[0],
                  (Dev->TxLayout[0] == NetPacketLayer2TypeEthernet) ? "Ethernet" :
                  (Dev->TxLayout[0] == NetPacketLayer2TypeIeee80211) ? "802.11, converted" : "unspecified",
                  Dev->TxLayout[1], Dev->TxLayout[2], Dev->TxLayout[3], Dev->TxLayout[4], Dev->TxLayout[5],
                  Dev->TxLayout[6], Dev->TxLayout[7], Dev->TxLayout[8], Dev->TxLayout[9], Dev->TxLayout[10],
                  Dev->TxLayout[11], Dev->TxLayout[12], Dev->TxLayout[13], Dev->TxLayout[14]);
        }
        Ticks++;
        if (Dev->RunR != NULL && (Ticks % (60000 / CX_IDLE_WAIT_MS)) == 0) {
            CxLog(Dev, "[cx] alive: %lu D2H items, %lu commands; rx fifo in %lu out %lu drop %lu; tx fifo in %lu out %lu "
                  "drop %lu\r\n", Polled, Dev->Commands, Dev->RxFifo ? Dev->RxFifo->In : 0,
                  Dev->RxFifo ? Dev->RxFifo->Out : 0, Dev->RxFifo ? Dev->RxFifo->Dropped : 0,
                  Dev->TxFifo ? Dev->TxFifo->In : 0, Dev->TxFifo ? Dev->TxFifo->Out : 0,
                  Dev->TxFifo ? Dev->TxFifo->Dropped : 0);
        }
        if (++FlushTicks >= 1000 / CX_IDLE_WAIT_MS) {
            FlushTicks = 0;
            if (Dev->RunDirty || (Dev->RunR != NULL && Dev->RunR->Used >= CX_LOG_ROTATE)) {
                CxLogFlush(Dev);
            }
        }
    }
    CxLog(Dev, "[cx] worker stopping after %lu ticks\r\n", Ticks);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS
CxWorkerStart(PCX_DEVICE Dev)
{
    HANDLE            H;
    OBJECT_ATTRIBUTES Oa;
    NTSTATUS          Status;

    if (Dev->Thread != NULL) {
        return STATUS_SUCCESS;
    }
    InterlockedExchange(&Dev->Stop, 0);
    InitializeObjectAttributes(&Oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&H, THREAD_ALL_ACCESS, &Oa, NULL, NULL, CxWorker, Dev);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = ObReferenceObjectByHandle(H, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&Dev->Thread, NULL);
    ZwClose(H);
    if (!NT_SUCCESS(Status)) {
        /* cannot wait for it later: stop it now (it only touches Dev, which outlives this call) */
        InterlockedExchange(&Dev->Stop, 1);
        CxWake(Dev);
        Dev->Thread = NULL;
    }
    return Status;
}

/* After this returns the worker no longer runs; queued commands are failed (WiFiCx expects an M3 for each). */
static VOID
CxWorkerStop(PCX_DEVICE Dev)
{
    KIRQL Irql;

    if (Dev->Thread != NULL) {
        InterlockedExchange(&Dev->Stop, 1);
        CxWake(Dev);
        (VOID)KeWaitForSingleObject(Dev->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Dev->Thread);
        Dev->Thread = NULL;
    }
    for (;;) {
        PCX_CMD Cmd = NULL;

        KeAcquireSpinLock(&Dev->CmdLock, &Irql);
        if (!IsListEmpty(&Dev->CmdList)) {
            Cmd = CONTAINING_RECORD(RemoveHeadList(&Dev->CmdList), CX_CMD, Link);
        }
        KeReleaseSpinLock(&Dev->CmdLock, Irql);
        if (Cmd == NULL) {
            break;
        }
        CxFailCommand(Dev, Cmd, STATUS_DEVICE_NOT_READY);
        ExFreePoolWithTag(Cmd, CX_POOL_TAG);
    }
}

/* ---------------------------------------------------------------- PnP / power */

static NTSTATUS
CxEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PCX_DEVICE     Dev = CxGetDevice(Device);
    PWLAN_REPORT   R;
    WLAN_PROBE     P;
    WLAN_DL_CONFIG DlCfg;
    WLAN_DL_RESULT Dl;
    volatile UCHAR *Cfg = NULL;
    ULONG          Bar0Lo = 0;
    NTSTATUS       Status;

    UNREFERENCED_PARAMETER(Raw);
    R = (PWLAN_REPORT)ExAllocatePool2(POOL_FLAG_PAGED, sizeof(*R), CX_POOL_TAG);
    if (R == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(&Dl, sizeof(Dl));
    ReportLine(R, "Exynos9820WlanCx (WiFiCx 1.0 / NetAdapterCx 2.2, ACPI\\SAMS9825), OS WDI version 0x%08lX\r\n",
               Dev->OsWdiVersion);
    Status = WlanProbeChip(Translated, R, &P, &Cfg, &Bar0Lo);
    if (P.Result == WLAN_RESULT_OK) {
        ReportWriteFile(R);
        if ((P.Bar[2] & EP_CFG_BAR_TYPE_MASK) == EP_CFG_BAR_TYPE_64 && P.Bar[3] == 0) {
            WlanReadConfig(WdfDeviceGetDriver(Device), &DlCfg);
            DlCfg.Resident      = 1;   /* the WiFiCx driver always keeps the firmware */
            DlCfg.RescanSeconds = 0;   /* scans come from WDI_TASK_SCAN */
            RtlZeroMemory(&Dev->Msg, sizeof(Dev->Msg));
            Dev->Msg.DmaEnabler = Dev->DmaEnabler;
            Dev->Msg.Config     = &DlCfg;
            Dev->Msg.Result     = MSG_SKIPPED;
            Dev->Msg.ScanResult = SCAN_SKIPPED;
            WlanDlRun(R, Cfg, Bar0Lo, P.Bar[2] & EP_CFG_BAR_MEM_MASK, P.Command, &DlCfg, &Dl, WlanMsgRun, &Dev->Msg);
            if (Dev->Msg.Resident) {
                Dev->Cfg = Cfg;    /* Msg.Bus.Cfg points into it: unmapped in ReleaseHardware */
                Cfg      = NULL;
                /* W2: escan records -> the WDI BSS list; EAPOL frames pass through to the data path */
                WlanMsgSetCxHooks(&Dev->Msg, CxBssRecord, CxLinkLost, Dev);   /* W2 BSS list, W4 link loss */
                WlanMsgSetRx(&Dev->Msg, CxRxFrame, Dev);   /* W3: received 802.3 frames -> RxFifo */
            }
        } else {
            ReportLine(R, "DL 2: BAR2 0x%08lX/0x%08lX is not a 64-bit BAR below 4 GB\r\n", P.Bar[2], P.Bar[3]);
        }
    }
    if (Cfg != NULL) {
        MmUnmapIoSpace((PVOID)Cfg, WLAN_CFG_WINDOW_SIZE);
    }
    if (NT_SUCCESS(Status) && !Dev->Msg.Resident) {
        ReportLine(R, "CX 1: firmware not resident (probe %lu, DL %lu, MSG %lu, SCAN %lu) - device start fails\r\n",
                   P.Result, Dl.DlResult, Dev->Msg.Result, Dev->Msg.ScanResult);
        Status = STATUS_DEVICE_NOT_READY;
    }
    if (NT_SUCCESS(Status)) {
        Status = CxSetCapabilities(Dev);   /* no log of its own (the run log starts at D0 entry) */
        ReportLine(R, "CX %lu: WiFiCx capabilities 0x%08lX; MAC %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                   NT_SUCCESS(Status) ? 0UL : 2UL, Status, Dev->Msg.Mac[0], Dev->Msg.Mac[1], Dev->Msg.Mac[2],
                   Dev->Msg.Mac[3], Dev->Msg.Mac[4], Dev->Msg.Mac[5]);
    }
    ReportWriteFile(R);
    ExFreePoolWithTag(R, CX_POOL_TAG);
    return Status;
}

static NTSTATUS
CxEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PCX_DEVICE Dev = CxGetDevice(Device);

    UNREFERENCED_PARAMETER(Translated);
    CxWorkerStop(Dev);
    if (Dev->Msg.Resident) {
        if (!Dev->Msg.Quiesced && Dev->RunR != NULL) {
            WlanMsgQuiesce(&Dev->Msg, Dev->RunR);
        }
        WlanMsgFree(&Dev->Msg);
    }
    if (Dev->Cfg != NULL && !Dev->Msg.Resident) {
        MmUnmapIoSpace((PVOID)Dev->Cfg, WLAN_CFG_WINDOW_SIZE);
        Dev->Cfg = NULL;
    }
    if (Dev->RunR != NULL) {
        ReportLine(Dev->RunR, "[cx] released\r\n");
        CxLogFlush(Dev);
        ExFreePoolWithTag(Dev->RunR, CX_POOL_TAG);
        Dev->RunR = NULL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
CxEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PCX_DEVICE Dev = CxGetDevice(Device);
    NTSTATUS   Status;

    if (Dev->RunR == NULL) {
        Dev->RunR = (PWLAN_REPORT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(WLAN_REPORT), CX_POOL_TAG);
        if (Dev->RunR == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Dev->RunR->FileName = WLAN_RUN_REPORT_FILE;
        ReportLine(Dev->RunR, "Exynos9820WlanCx run log (worker poll %u ms)\r\n", CX_IDLE_WAIT_MS);
    }
    CxLog(Dev, "[cx] D0 entry from D%d (resident %u quiesced %u)\r\n", (int)PreviousState - (int)WdfPowerDeviceD0,
          Dev->Msg.Resident, Dev->Msg.Quiesced);
    if (!Dev->Msg.Resident || Dev->Msg.Quiesced) {
        CxLog(Dev, "[cx] firmware not running - a restart needs a new PrepareHardware\r\n");
        CxLogFlush(Dev);
        return STATUS_DEVICE_NOT_READY;
    }
    Status = CxWorkerStart(Dev);
    CxLog(Dev, "[cx] worker start 0x%08lX\r\n", Status);
    CxLogFlush(Dev);
    return Status;
}

/* Leaving D0 (sleep, shutdown, removal): stop the worker, then quiesce - the firmware must not DMA while the system
   changes power state (same rule as WlanProbe.c WlanEvtD0Exit). ReleaseHardware frees. */
static NTSTATUS
CxEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PCX_DEVICE Dev = CxGetDevice(Device);

    CxWorkerStop(Dev);
    if (Dev->RunR != NULL) {
        CxLog(Dev, "[cx] D0 exit to state %d\r\n", (int)TargetState);
        CxSetLinked(Dev, FALSE);   /* releases the 1 ms timer resolution; the firmware is quiesced below */
        if (Dev->Msg.Resident && !Dev->Msg.Quiesced) {
            WlanMsgQuiesce(&Dev->Msg, Dev->RunR);
        }
        CxLogFlush(Dev);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
CxEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES        Attr;
    WDF_DMA_ENABLER_CONFIG       Dma;
    WIFI_DEVICE_CONFIG           Wifi;
    WDFDEVICE                    Device;
    PCX_DEVICE                   Dev;
    NTSTATUS                     Status;

    UNREFERENCED_PARAMETER(Driver);
    Status = NetDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WifiDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = CxEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = CxEvtReleaseHardware;
    Pnp.EvtDeviceD0Entry         = CxEvtD0Entry;
    Pnp.EvtDeviceD0Exit          = CxEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, CX_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Dev = CxGetDevice(Device);
    Dev->WdfTriageInfoPtr = WdfGetTriageInfo();
    Dev->Device  = Device;
    Dev->RadioOn = TRUE;
    KeInitializeEvent(&Dev->Wake, SynchronizationEvent, FALSE);
    {
        WDF_OBJECT_ATTRIBUTES M;
        WDFMEMORY             Mem;

        WDF_OBJECT_ATTRIBUTES_INIT(&M);
        M.ParentObject = Device;
        if (NT_SUCCESS(WdfMemoryCreate(&M, NonPagedPoolNx, CX_POOL_TAG, CX_MAX_BSS * sizeof(CX_BSS), &Mem,
                                       (PVOID *)&Dev->Bss))) {
            RtlZeroMemory(Dev->Bss, CX_MAX_BSS * sizeof(CX_BSS));
        } else {
            Dev->Bss = NULL;   /* WDI_TASK_SCAN then fails with STATUS_INSUFFICIENT_RESOURCES */
        }
        /* W3 FIFOs (a failure leaves the data path off: tx dropped, nothing received) */
        if (NT_SUCCESS(WdfMemoryCreate(&M, NonPagedPoolNx, CX_POOL_TAG, sizeof(CX_FIFO), &Mem, (PVOID *)&Dev->RxFifo))) {
            RtlZeroMemory(Dev->RxFifo, sizeof(CX_FIFO));
            KeInitializeSpinLock(&Dev->RxFifo->Lock);
        } else {
            Dev->RxFifo = NULL;
        }
        if (NT_SUCCESS(WdfMemoryCreate(&M, NonPagedPoolNx, CX_POOL_TAG, sizeof(CX_FIFO), &Mem, (PVOID *)&Dev->TxFifo))) {
            RtlZeroMemory(Dev->TxFifo, sizeof(CX_FIFO));
            KeInitializeSpinLock(&Dev->TxFifo->Lock);
        } else {
            Dev->TxFifo = NULL;
        }
    }
    KeInitializeSpinLock(&Dev->CmdLock);
    InitializeListHead(&Dev->CmdList);

    /* 32-bit packet profile as WlanProbe.c: the ring buffers must be below 4 GB (WlanMsgRun checks). */
    WdfDeviceSetAlignmentRequirement(Device, FILE_QUAD_ALIGNMENT);
    WDF_DMA_ENABLER_CONFIG_INIT(&Dma, WdfDmaProfilePacket, MSG_DMA_SIZE);
    if (!NT_SUCCESS(WdfDmaEnablerCreate(Device, &Dma, WDF_NO_OBJECT_ATTRIBUTES, &Dev->DmaEnabler))) {
        Dev->DmaEnabler = NULL;
    }

    WIFI_DEVICE_CONFIG_INIT(&Wifi, WDI_VERSION_LATEST, CxEvtSendCommand, CxEvtCreateAdapter,
                            CxEvtCreateWifiDirectDevice);
    Status = WifiDeviceInitialize(Device, &Wifi);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Dev->OsWdiVersion          = WifiDeviceGetOsWdiVersion(Device);
    Dev->Tlv.AllocationContext = 0;
    Dev->Tlv.PeerVersion       = Dev->OsWdiVersion;
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, CxEvtDeviceAdd);
    Config.DriverPoolTag = CX_POOL_TAG;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
