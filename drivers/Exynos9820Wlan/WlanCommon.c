/*  WlanCommon.c - S10e BCM4375 WLAN: code shared by the KMDF probe driver (Exynos9820Wlan, WlanProbe.c) and the
    WiFiCx client (drivers/Exynos9820WlanCx): the report sink, the Parameters settings and the read-only chip probe
    of PrepareHardware. Moved verbatim out of WlanProbe.c (M9 T2, notes/m12-wifi-wificx.md); behaviour unchanged.
*/

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include "WlanDl.h"

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

/* Parameters key settings: Download (default 1), OtpProbe (default 1), NvramFile, FirmwareFile
   (empty = automatic / default), Msgbuf (default 1), HostCap (default WLAN_HOSTCAP_AUTO),
   DoorbellDar (default 0), Scan (default 1), Resident (default 1), RescanSeconds (default 30), CacheMaint
   (default 1), D11Reset (default 1, M9 T1). */
VOID
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
    DlCfg->D11Reset   = 1;
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
    RtlInitUnicodeString(&Name, L"D11Reset");
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
        DlCfg->D11Reset = V;
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

/* PrepareHardware's chip probe (WlanProbe.c before M9 T2): _CRS contract ([0] = CFG window, [1] = MEM window), the
   endpoint config header through the CFG0 window, BAR0 sanity, ChipCommon chipid through BAR0. Writes nothing to the
   device. *CfgOut = the mapped CFG window (caller unmaps WLAN_CFG_WINDOW_SIZE) whenever it was mapped; P->Result =
   WLAN_RESULT_*; returns STATUS_DEVICE_CONFIGURATION_ERROR only for a _CRS mismatch (device fails start, Code 10). */
NTSTATUS
WlanProbeChip(_In_ WDFCMRESLIST Translated, _Inout_ PWLAN_REPORT R, _Out_ PWLAN_PROBE Out,
              _Outptr_result_maybenull_ volatile UCHAR **CfgOut, _Out_ PULONG Bar0LoOut)
{
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
    ULONG                            Bar0Lo = 0;
    NTSTATUS                         Status = STATUS_SUCCESS;

    RtlZeroMemory(&P, sizeof(P));
    Phys[0].QuadPart = Phys[1].QuadPart = 0;
    Len[0] = Len[1] = 0;

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
    } else {
        ReportLine(R, "RESULT %u: chip id mismatch\r\n", WLAN_RESULT_CHIPID_MISMATCH);
        P.Result = WLAN_RESULT_CHIPID_MISMATCH;
    }

Report:
    if (Bar0 != NULL) {
        MmUnmapIoSpace((PVOID)Bar0, PCIE_BAR0_MIN_SIZE);
    }
    *Out       = P;
    *CfgOut    = Cfg;
    *Bar0LoOut = Bar0Lo;
    return Status;
}
