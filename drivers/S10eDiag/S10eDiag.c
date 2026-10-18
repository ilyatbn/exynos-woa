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
#include <aux_klib.h>
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
static volatile UCHAR *gMarks;          // PRAM mark ring (S10EDIAG_MARK_PHYSICAL), non-cached
static volatile ULONG *gProgress;       // firmware boot progress word page (WINPRAM + 0xF000), non-cached
static KSPIN_LOCK gMarkLock;
static KBUGCHECK_CALLBACK_RECORD gBugcheckRecord;
static BOOLEAN gBugcheckRegistered;
static BOOLEAN gAuxKlibReady;

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

//
// One line into the PRAM mark ring. Callable at any IRQL (stores only, no allocation); the bugcheck
// callback calls it without the lock.
//
static VOID
S10eMarkRaw(const CHAR *Text, ULONG Length)
{
    static const CHAR Digits[] = "0123456789";
    ULONGLONG Ms = KeQueryInterruptTime() / 10000ULL;
    volatile UCHAR *Line;
    ULONG Next;
    ULONG Index;
    ULONG Out = 0;
    CHAR Number[10];

    if (gMarks == NULL) {
        return;
    }
    Next = *(volatile ULONG *)(gMarks + 8);
    if ((Next == 0) || (Next >= S10EDIAG_MARK_SIZE / S10EDIAG_MARK_LINE)) {
        Next = 1;
    }
    Line = gMarks + (Next * S10EDIAG_MARK_LINE);
    for (Index = 0; Index < 10; Index++) {
        Number[9 - Index] = Digits[Ms % 10];
        Ms /= 10;
    }
    for (Index = 0; Index < 10; Index++) {
        Line[Out++] = (UCHAR)Number[Index];
    }
    Line[Out++] = ' ';
    for (Index = 0; (Index < Length) && (Out < S10EDIAG_MARK_LINE - 1); Index++) {
        CHAR C = Text[Index];
        Line[Out++] = (UCHAR)(((C >= 0x20) && (C < 0x7F)) ? C : '.');
    }
    while (Out < S10EDIAG_MARK_LINE - 1) {
        Line[Out++] = ' ';
    }
    Line[Out] = '\n';
    *(volatile ULONG *)(gMarks + 8) = Next + 1;
    KeMemoryBarrier();
}

static VOID
S10eMark(const CHAR *Text, ULONG Length)
{
    KIRQL Irql;

    KeAcquireSpinLock(&gMarkLock, &Irql);
    S10eMarkRaw(Text, Length);
    KeReleaseSpinLock(&gMarkLock, Irql);
}

static VOID
S10eMarkHex(const CHAR *Prefix, ULONGLONG Value)
{
    static const CHAR Hex[] = "0123456789ABCDEF";
    CHAR Line[48];
    ULONG Out = 0;
    ULONG Index;

    for (Index = 0; (Prefix[Index] != '\0') && (Out < 24); Index++) {
        Line[Out++] = Prefix[Index];
    }
    Line[Out++] = ' ';
    for (Index = 0; Index < 16; Index++) {
        Line[Out++] = Hex[(Value >> ((15 - Index) * 4)) & 0xF];
    }
    S10eMarkRaw(Line, Out);
}

static VOID S10eHardResetToRecovery(VOID);

static VOID
S10eBugcheckCallback(PVOID Buffer, ULONG Length)
{
    KBUGCHECK_DATA Data = { 0 };
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);

    // No lock - the system is already stopped.
    S10eMarkRaw("BUGCHECK callback", 17);
    Data.BugCheckDataSize = sizeof(Data);
    //
    // g27 (notes/g27-x64-apps.md): a bugcheck left only "BUGCHECK callback" (no BC lines) - record why: the init status
    // at load ("AuxKlib init") and the status of this call. MS Learn: AuxKlibGetBugCheckData "can be called only from a
    // BugCheckCallback routine", after AuxKlibInitialize.
    //
    Status = gAuxKlibReady ? AuxKlibGetBugCheckData(&Data) : STATUS_NOT_SUPPORTED;
    S10eMarkHex("BC status", (ULONGLONG)(ULONG)Status);
    if (NT_SUCCESS(Status)) {
        S10eMarkHex("BC", Data.BugCheckCode);
        S10eMarkHex("BC P1", (ULONGLONG)Data.Parameter1);
        S10eMarkHex("BC P2", (ULONGLONG)Data.Parameter2);
        S10eMarkHex("BC P3", (ULONGLONG)Data.Parameter3);
        S10eMarkHex("BC P4", (ULONGLONG)Data.Parameter4);
    }

    //
    // m11 run 5 (user: "the phone is rebooting into windows after a crash"): Exynos9820Ufs's callback resets
    // with INFORM3 = normal and leaves the rest to the firmware boot pair, which did not always route to
    // TWRP. Callbacks run newest-first and this driver registers after the boot-start UFS driver, so reset
    // here, with INFORM3 = recovery: sboot then boots TWRP directly. The PRAM (UFS marks + this ring) stays
    // in RAM; `adb reboot` from TWRP with \WINRUN.FLG present makes the firmware save it as \WINPRAM.BIN.
    //
    S10eMarkRaw("reset to recovery", 17);
    S10eHardResetToRecovery();
}

static VOID
S10eHardResetToRecovery(VOID)
{
    //
    // m12 run 8: a run that ended here after Windows' shutdown SRB had stored progress 0x80 made the next
    // firmware boot "continue into Windows" instead of dumping PRAM and going to TWRP. 0x81 = "S10eDiag
    // reset to recovery" (any value but 0x80 takes the dump path in Beyond0LteBootLogDxe OnReadyToBoot).
    //
    if (gProgress != NULL) {
        gProgress[1] = 0x81;
        KeMemoryBarrier();
    }
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
        S10eWritePmu(S10E_PMU_RSTSTAT, 0);   // m14 run 18: Samsung's recovery reboot clears RSTSTAT too
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

    case IOCTL_S10EDIAG_MARK:
        if (InLength == 0) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        S10eMark((const CHAR *)Buffer, InLength);
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

    S10eMark("shutdown notification", 21);
    // Last-chance notification: nothing in Windows writes INFORM3 after this point.
    if (gRecoveryArmed != 0) {
        S10eWritePmu(S10E_PMU_INFORM3, S10E_REBOOT_RECOVERY);
        S10eWritePmu(S10E_PMU_RSTSTAT, 0);
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
    if (gBugcheckRegistered) {
        KeDeregisterBugCheckCallback(&gBugcheckRecord);
    }
    if (gMarks != NULL) {
        MmUnmapIoSpace((PVOID)gMarks, S10EDIAG_MARK_SIZE);
    }
    if (gProgress != NULL) {
        MmUnmapIoSpace((PVOID)gProgress, PAGE_SIZE);
    }
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

    // PRAM mark ring: the firmware zeroes the dumped 64 KiB after saving it, so a fresh boot starts empty.
    KeInitializeSpinLock(&gMarkLock);
    Physical.QuadPart = (LONGLONG)S10EDIAG_PROGRESS_PHYSICAL;
    gProgress = (volatile ULONG *)MmMapIoSpaceEx(Physical, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    Physical.QuadPart = (LONGLONG)S10EDIAG_MARK_PHYSICAL;
    gMarks = (volatile UCHAR *)MmMapIoSpaceEx(Physical, S10EDIAG_MARK_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (gMarks != NULL) {
        static const CHAR Magic[8] = { 'S', '1', '0', 'E', 'M', 'A', 'R', 'K' };
        ULONG Index;

        for (Index = 0; Index < 8; Index++) {
            gMarks[Index] = (UCHAR)Magic[Index];
        }
        S10eMark("S10eDiag loaded", 15);
        S10eMarkHex("INFORM3", (gPmu != NULL) ? READ_REGISTER_ULONG((volatile ULONG *)(gPmu + S10E_PMU_INFORM3)) : 0);
    }
    Status = AuxKlibInitialize();
    gAuxKlibReady = NT_SUCCESS(Status);
    if (gMarks != NULL) {
        S10eMarkHex("AuxKlib init", (ULONGLONG)(ULONG)Status);
    }
    KeInitializeCallbackRecord(&gBugcheckRecord);
    gBugcheckRegistered = KeRegisterBugCheckCallback(&gBugcheckRecord, S10eBugcheckCallback, NULL, 0,
                                                     (PUCHAR)"S10eDiag");

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
    if (gBugcheckRegistered) {
        KeDeregisterBugCheckCallback(&gBugcheckRecord);
        gBugcheckRegistered = FALSE;
    }
    if (gMarks != NULL) {
        MmUnmapIoSpace((PVOID)gMarks, S10EDIAG_MARK_SIZE);
        gMarks = NULL;
    }
    if (gProgress != NULL) {
        MmUnmapIoSpace((PVOID)gProgress, PAGE_SIZE);
        gProgress = NULL;
    }
    if (gPmu != NULL) {
        MmUnmapIoSpace((PVOID)gPmu, S10E_PMU_WINDOW);
        gPmu = NULL;
    }
    return Status;
}
