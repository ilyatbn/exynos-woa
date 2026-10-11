/*  WlanProbe.c - S10e BCM4375 WLAN, milestone-1 probe (KMDF, no NDIS).

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

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD WlanEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE WlanEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE WlanEvtReleaseHardware;

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

    RtlInitUnicodeString(&Name, WLAN_REPORT_FILE);
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
   (empty = automatic / default). */
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

    UNREFERENCED_PARAMETER(Raw);
    RtlZeroMemory(&Dl, sizeof(Dl));

    R = (WLAN_REPORT *)ExAllocatePool2(POOL_FLAG_PAGED, sizeof(*R), 'nlWE');
    if (R == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&P, sizeof(P));
    Phys[0].QuadPart = Phys[1].QuadPart = 0;
    Len[0] = Len[1] = 0;
    ReportLine(R, "Exynos9820Wlan milestone-1 probe (ACPI\\SAMS9825)\r\n");

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
            WlanDlRun(R, Cfg, Bar0Lo, P.Bar[2] & EP_CFG_BAR_MEM_MASK, P.Command, &DlCfg, &Dl);
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

NTSTATUS
WlanEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Translated);
    return STATUS_SUCCESS;   /* nothing stays mapped after PrepareHardware */
}

NTSTATUS
WlanEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDFDEVICE                    Device;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = WlanEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = WlanEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    return WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &Device);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, WlanEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
