/*
 * S10eDiag.sys - bring-up diagnostics for Windows on the Galaxy S10e (Exynos 9820).
 * notes/m11-s10ediag.md.
 *
 * Legacy WDM driver (no INF, no hardware): Services\S10eDiag Type=1 Start=1. Gives the admin-only
 * tools physical reads/writes and the two ways back to TWRP:
 *   - RECOVERY_ON_RESET: INFORM3 = recovery now and again from the last-chance shutdown notification,
 *     so the clean `shutdown /r` that follows (FADT reset register = PMU SWRESET, W32) lands in TWRP.
 *   - HARD_RESET / FALLBACK: INFORM3 = recovery + SWRESET now / when a timer expires.
 *
 * PMU constants are the VERIFIED ones from
 * firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h:273-305 (notes/s10e-reboot-wdt-constants.md).
 */

#include <ntddk.h>
#include <wdmsec.h>
#include "S10eDiagIoctl.h"

#define S10E_PMU_PHYSICAL_BASE     0x15860000ULL   // EXYNOS_PMU_BASE
#define S10E_PMU_WINDOW            0x4000UL
#define S10E_PMU_RSTSTAT           0x0404UL        // EXYNOS_PMU_RSTSTAT_OFFSET
#define S10E_PMU_INFORM3           0x080CUL        // EXYNOS_PMU_INFORM3_OFFSET
#define S10E_PMU_SWRESET           0x3A00UL        // EXYNOS_PMU_SWRESET_OFFSET
#define S10E_PMU_SWRESET_VALUE     0x2UL           // EXYNOS_PMU_SWRESET_VALUE
#define S10E_REBOOT_RECOVERY       0x12345674UL    // EXYNOS_REBOOT_REASON_BASE | EXYNOS_REBOOT_MODE_RECOVERY

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD S10eUnload;
static DRIVER_DISPATCH S10eCreateClose;
static DRIVER_DISPATCH S10eIoctl;
static DRIVER_DISPATCH S10eShutdown;
static KDEFERRED_ROUTINE S10eFallbackDpc;

static PDEVICE_OBJECT gDevice;
static volatile UCHAR *gPmu;
static LONG gRecoveryArmed;
static ULONG gFallbackSeconds;
static KTIMER gFallbackTimer;
static KDPC gFallbackDpc;
static BOOLEAN gShutdownRegistered;

// SYSTEM + Administrators only.
static const WCHAR gSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
// {6A1C3B52-8E0B-4E59-9C47-2F35D9C0E10A}
static const GUID gClassGuid = { 0x6a1c3b52, 0x8e0b, 0x4e59, { 0x9c, 0x47, 0x2f, 0x35, 0xd9, 0xc0, 0xe1, 0x0a } };

static VOID
S10eWritePmu(ULONG Offset, ULONG Value)
{
    if (gPmu != NULL) {
        WRITE_REGISTER_ULONG((volatile ULONG *)(gPmu + Offset), Value);
        KeMemoryBarrier();
    }
}

static VOID
S10eHardResetToRecovery(VOID)
{
    S10eWritePmu(S10E_PMU_INFORM3, S10E_REBOOT_RECOVERY);
    S10eWritePmu(S10E_PMU_RSTSTAT, 0);
    S10eWritePmu(S10E_PMU_SWRESET, S10E_PMU_SWRESET_VALUE);
    for (;;) {
        KeStallExecutionProcessor(1000);
    }
}

static VOID
S10eFallbackDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    S10eHardResetToRecovery();
}

static NTSTATUS
S10eReadPhys(const S10EDIAG_READ_IN *In, PVOID Out, ULONG OutLength, PULONG_PTR Information)
{
    PHYSICAL_ADDRESS Physical;
    volatile ULONG *Map;
    ULONG Index;

    if ((In->Length == 0) || (In->Length > S10EDIAG_READ_MAX) || (In->Length > OutLength)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (In->Mode == S10EDIAG_READ_RAM) {
        MM_COPY_ADDRESS Source;
        SIZE_T Copied = 0;
        NTSTATUS Status;

        Source.PhysicalAddress.QuadPart = (LONGLONG)In->Address;
        Status = MmCopyMemory(Out, Source, In->Length, MM_COPY_MEMORY_PHYSICAL, &Copied);
        *Information = Copied;
        return Status;
    }

    if ((In->Mode != S10EDIAG_READ_MMIO) || ((In->Address & 3ULL) != 0) || ((In->Length & 3UL) != 0)) {
        return STATUS_INVALID_PARAMETER;
    }

    Physical.QuadPart = (LONGLONG)In->Address;
    Map = (volatile ULONG *)MmMapIoSpaceEx(Physical, In->Length, PAGE_READWRITE | PAGE_NOCACHE);
    if (Map == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (Index = 0; Index < In->Length / 4; Index++) {
        ((PULONG)Out)[Index] = READ_REGISTER_ULONG(Map + Index);
    }

    MmUnmapIoSpace((PVOID)Map, In->Length);
    *Information = In->Length;
    return STATUS_SUCCESS;
}

static NTSTATUS
S10eWrite32(const S10EDIAG_WRITE32_IN *In)
{
    PHYSICAL_ADDRESS Physical;
    volatile ULONG *Map;

    if ((In->Address & 3ULL) != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    Physical.QuadPart = (LONGLONG)In->Address;
    Map = (volatile ULONG *)MmMapIoSpaceEx(Physical, sizeof(ULONG), PAGE_READWRITE | PAGE_NOCACHE);
    if (Map == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WRITE_REGISTER_ULONG(Map, In->Value);
    KeMemoryBarrier();
    MmUnmapIoSpace((PVOID)Map, sizeof(ULONG));
    return STATUS_SUCCESS;
}

static NTSTATUS
S10eIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG InLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG OutLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Information = 0;

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (Stack->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_S10EDIAG_READ_PHYS:
        if (InLength < sizeof(S10EDIAG_READ_IN)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            S10EDIAG_READ_IN In = *(S10EDIAG_READ_IN *)Buffer;   // the output overwrites the same buffer

            Status = S10eReadPhys(&In, Buffer, OutLength, &Information);
        }
        break;

    case IOCTL_S10EDIAG_WRITE32:
        Status = (InLength < sizeof(S10EDIAG_WRITE32_IN)) ? STATUS_BUFFER_TOO_SMALL :
                 S10eWrite32((S10EDIAG_WRITE32_IN *)Buffer);
        break;

    case IOCTL_S10EDIAG_RECOVERY_ON_RESET:
        InterlockedExchange(&gRecoveryArmed, 1);
        S10eWritePmu(S10E_PMU_INFORM3, S10E_REBOOT_RECOVERY);
        Status = STATUS_SUCCESS;
        break;

    case IOCTL_S10EDIAG_HARD_RESET:
        S10eHardResetToRecovery();
        break;

    case IOCTL_S10EDIAG_FALLBACK:
        if (InLength < sizeof(ULONG)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        gFallbackSeconds = *(PULONG)Buffer;
        KeCancelTimer(&gFallbackTimer);
        if (gFallbackSeconds != 0) {
            LARGE_INTEGER Due;

            Due.QuadPart = -(LONGLONG)gFallbackSeconds * 10000000LL;
            KeSetTimer(&gFallbackTimer, Due, &gFallbackDpc);
        }
        Status = STATUS_SUCCESS;
        break;

    case IOCTL_S10EDIAG_INFO:
        if (OutLength < sizeof(S10EDIAG_INFO)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            S10EDIAG_INFO *Info = (S10EDIAG_INFO *)Buffer;

            RtlZeroMemory(Info, sizeof(*Info));
            Info->Version = S10EDIAG_VERSION;
            Info->RecoveryArmed = (ULONG)gRecoveryArmed;
            Info->FallbackSeconds = gFallbackSeconds;
            Info->Inform3 = (gPmu != NULL) ? READ_REGISTER_ULONG((volatile ULONG *)(gPmu + S10E_PMU_INFORM3)) : 0;
            Info->UptimeMs = KeQueryInterruptTime() / 10000ULL;
            Information = sizeof(*Info);
            Status = STATUS_SUCCESS;
        }
        break;

    default:
        break;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) ? Information : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS
S10eShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    // Last-chance notification: nothing in Windows writes INFORM3 after this point.
    if (gRecoveryArmed != 0) {
        S10eWritePmu(S10E_PMU_INFORM3, S10E_REBOOT_RECOVERY);
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
S10eCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static VOID
S10eUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING Dos;

    UNREFERENCED_PARAMETER(DriverObject);
    KeCancelTimer(&gFallbackTimer);
    if (gShutdownRegistered) {
        IoUnregisterShutdownNotification(gDevice);
    }
    RtlInitUnicodeString(&Dos, S10EDIAG_DOS_NAME);
    IoDeleteSymbolicLink(&Dos);
    if (gPmu != NULL) {
        MmUnmapIoSpace((PVOID)gPmu, S10E_PMU_WINDOW);
    }
    IoDeleteDevice(gDevice);
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Name;
    UNICODE_STRING Dos;
    UNICODE_STRING Sddl;
    PHYSICAL_ADDRESS Physical;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    KeInitializeTimer(&gFallbackTimer);
    KeInitializeDpc(&gFallbackDpc, S10eFallbackDpc, NULL);

    Physical.QuadPart = (LONGLONG)S10E_PMU_PHYSICAL_BASE;
    gPmu = (volatile UCHAR *)MmMapIoSpaceEx(Physical, S10E_PMU_WINDOW, PAGE_READWRITE | PAGE_NOCACHE);

    RtlInitUnicodeString(&Name, S10EDIAG_DEVICE_NAME);
    RtlInitUnicodeString(&Sddl, gSddl);
    Status = IoCreateDeviceSecure(DriverObject, 0, &Name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                  FALSE, &Sddl, &gClassGuid, &gDevice);
    if (!NT_SUCCESS(Status)) {
        goto Fail;
    }

    RtlInitUnicodeString(&Dos, S10EDIAG_DOS_NAME);
    Status = IoCreateSymbolicLink(&Dos, &Name);
    if (!NT_SUCCESS(Status)) {
        IoDeleteDevice(gDevice);
        goto Fail;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = S10eCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = S10eCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S10eIoctl;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = S10eShutdown;
    DriverObject->DriverUnload = S10eUnload;

    gShutdownRegistered = NT_SUCCESS(IoRegisterLastChanceShutdownNotification(gDevice));
    gDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;

Fail:
    if (gPmu != NULL) {
        MmUnmapIoSpace((PVOID)gPmu, S10E_PMU_WINDOW);
        gPmu = NULL;
    }
    return Status;
}
