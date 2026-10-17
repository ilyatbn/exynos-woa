/*
 * S10ePower - the Windows ACPM client for CPU/GPU clocks and temperatures (notes/clocks-dvfs.md).
 *
 * Task C0 (this file, READ-ONLY): map the ACPM SRAM window and record the AP channel table that the ACPM framework
 * publishes at SRAM+0x7000 (ipc_channels / ipc_ap_max), so the live channel ids, types and queue geometry are known
 * under Windows before any message is sent. Nothing is written to SRAM and the mailbox is not touched.
 *
 * Constants: EXYNOS_ACPM_SRAM_BASE / _SIZE / _INITDATA_OFFSET are VERIFIED (firmware Exynos9820.h, live DT acpm_ipc);
 * framework / channel offsets are the device-validated UEFI client's (Beyond0LteGpuProbeDxe/GpuAcpm.c, kernel
 * framework.h citations there).
 *
 * Output (service key HKLM\SYSTEM\CurrentControlSet\Services\S10ePower): AcpmTableOff, AcpmApMax (REG_DWORD),
 * AcpmChannels (REG_BINARY, ApMax x 0x48 bytes, the raw ipc_channel entries), AcpmStatus.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <ntddk.h>

#define ACPM_SRAM_BASE        0x0000000002039000ULL   /* VERIFIED Exynos9820.h EXYNOS_ACPM_SRAM_BASE */
#define ACPM_SRAM_SIZE        0x00030000u             /* VERIFIED Exynos9820.h EXYNOS_ACPM_SRAM_SIZE */
#define ACPM_INITDATA_OFFSET  0x00007000u             /* VERIFIED Exynos9820.h EXYNOS_ACPM_INITDATA_OFFSET */
#define ACPM_FW_IPC_CHANNELS  0x08u                   /* framework.h:47 (GpuAcpm.c) */
#define ACPM_FW_IPC_AP_MAX    0x18u                   /* framework.h:52 (GpuAcpm.c) */
#define ACPM_CH_STRIDE        0x48u                   /* GpuAcpm.c */
#define ACPM_MAX_CHANNELS     64u

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD S10ePowerUnload;

static VOID S10ePowerUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

static VOID WriteDword(PCWSTR Key, PCWSTR Name, ULONG Value)
{
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, Key, Name, REG_DWORD, &Value, sizeof(Value));
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WCHAR Key[256];
    PHYSICAL_ADDRESS Physical;
    volatile UCHAR *Sram;
    ULONG TableOff, ApMax, Status = 0, i;
    UCHAR *Copy;

    DriverObject->DriverUnload = S10ePowerUnload;
    if (RegistryPath->Length >= sizeof(Key))
        return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(Key, RegistryPath->Buffer, RegistryPath->Length);
    Key[RegistryPath->Length / sizeof(WCHAR)] = 0;

    Physical.QuadPart = (LONGLONG)ACPM_SRAM_BASE;
    Sram = (volatile UCHAR *)MmMapIoSpaceEx(Physical, ACPM_SRAM_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (Sram == NULL) {
        WriteDword(Key, L"AcpmStatus", 1);   /* map failed */
        return STATUS_SUCCESS;
    }
    TableOff = READ_REGISTER_ULONG((volatile ULONG *)(Sram + ACPM_INITDATA_OFFSET + ACPM_FW_IPC_CHANNELS));
    ApMax = READ_REGISTER_ULONG((volatile ULONG *)(Sram + ACPM_INITDATA_OFFSET + ACPM_FW_IPC_AP_MAX));
    WriteDword(Key, L"AcpmTableOff", TableOff);
    WriteDword(Key, L"AcpmApMax", ApMax);
    if (ApMax == 0 || ApMax > ACPM_MAX_CHANNELS || (TableOff & 3) != 0 || TableOff >= ACPM_SRAM_SIZE ||
        (ULONGLONG)TableOff + (ULONGLONG)ApMax * ACPM_CH_STRIDE > ACPM_SRAM_SIZE) {
        Status = 2;   /* implausible table: not read further */
    } else {
        Copy = (UCHAR *)ExAllocatePool2(POOL_FLAG_PAGED, ApMax * ACPM_CH_STRIDE, 'rPeS');
        if (Copy == NULL) {
            Status = 3;
        } else {
            for (i = 0; i < ApMax * ACPM_CH_STRIDE / 4; i++)
                ((ULONG *)Copy)[i] = READ_REGISTER_ULONG((volatile ULONG *)(Sram + TableOff + i * 4));
            (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, Key, L"AcpmChannels", REG_BINARY, Copy,
                                        ApMax * ACPM_CH_STRIDE);
            ExFreePoolWithTag(Copy, 'rPeS');
        }
    }
    WriteDword(Key, L"AcpmStatus", Status);
    MmUnmapIoSpace((PVOID)Sram, ACPM_SRAM_SIZE);
    return STATUS_SUCCESS;
}
