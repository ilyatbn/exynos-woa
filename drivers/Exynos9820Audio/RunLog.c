/*
 * RunLog.c - the driver's run log, the same idea as Exynos9820Wlan's ReportLine/ReportWriteFile
 * (drivers/Exynos9820Wlan/WlanProbe.c:69-107): every line goes to DbgPrintEx and into a text buffer, and the
 * whole buffer is rewritten to \SystemRoot\System32\LogFiles\Exynos9820Audio-run.txt (FILE_OVERWRITE_IF,
 * write-through) at every PASSIVE_LEVEL checkpoint. Difference: stream events arrive in timer DPCs, so
 * appending takes a spin lock and works up to DISPATCH_LEVEL; only AudLogFlush touches the file.
 * A full buffer drops further lines and counts them (the count is printed at the end of the file).
 */

#include "Exynos9820Audio.h"

PAUD_RUNLOG AudLogCreate(VOID)
{
    PAUD_RUNLOG Log;

    Log = (PAUD_RUNLOG)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(AUD_RUNLOG), AUD_POOL_TAG);
    if (Log == NULL) {
        return NULL;
    }

    KeInitializeSpinLock(&Log->Lock);
    Log->Start = KeQueryInterruptTime();
    return Log;
}

VOID AudLogFree(_In_opt_ PAUD_RUNLOG Log)
{
    if (Log != NULL) {
        ExFreePoolWithTag(Log, AUD_POOL_TAG);
    }
}

VOID AudLog(_In_opt_ PAUD_RUNLOG Log, _In_z_ _Printf_format_string_ PCSTR Fmt, ...)
{
    CHAR     Line[256];
    CHAR     Prefix[24];
    va_list  Args;
    size_t   PrefixLen;
    size_t   LineLen;
    ULONG    Ms;
    KIRQL    OldIrql;

    va_start(Args, Fmt);
    if (!NT_SUCCESS(RtlStringCbVPrintfA(Line, sizeof(Line), Fmt, Args))) {
        /* truncated lines are still useful: RtlStringCbVPrintfA leaves a terminated prefix */
        Line[sizeof(Line) - 1] = '\0';
    }
    va_end(Args);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Exynos9820Audio: %s", Line);
    if (Log == NULL) {
        return;
    }

    Ms = (ULONG)((KeQueryInterruptTime() - Log->Start) / 10000);
    (VOID)RtlStringCbPrintfA(Prefix, sizeof(Prefix), "[%lu.%03lu] ", Ms / 1000, Ms % 1000);
    PrefixLen = strlen(Prefix);
    LineLen   = strlen(Line);

    KeAcquireSpinLock(&Log->Lock, &OldIrql);
    if (Log->Used + PrefixLen + LineLen + 1 < sizeof(Log->Text)) {
        RtlCopyMemory(Log->Text + Log->Used, Prefix, PrefixLen);
        RtlCopyMemory(Log->Text + Log->Used + PrefixLen, Line, LineLen);
        Log->Used += (ULONG)(PrefixLen + LineLen);
    } else {
        Log->Dropped++;
    }
    KeReleaseSpinLock(&Log->Lock, OldIrql);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID AudLogFlush(_In_opt_ PAUD_RUNLOG Log)
{
    UNICODE_STRING    Name;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK   Iosb;
    HANDLE            File;
    NTSTATUS          Status;
    PCHAR             Copy;
    ULONG             Used;
    ULONG             Dropped;
    CHAR              Tail[64];
    size_t            TailLen;
    KIRQL             OldIrql;

    if (Log == NULL) {
        return;
    }

    /* Snapshot under the lock: the file write happens unlocked at PASSIVE_LEVEL. */
    Copy = (PCHAR)ExAllocatePool2(POOL_FLAG_PAGED, AUD_RUNLOG_MAX + sizeof(Tail), AUD_POOL_TAG);
    if (Copy == NULL) {
        return;
    }

    KeAcquireSpinLock(&Log->Lock, &OldIrql);
    Used    = Log->Used;
    Dropped = Log->Dropped;
    RtlCopyMemory(Copy, Log->Text, Used);
    KeReleaseSpinLock(&Log->Lock, OldIrql);

    TailLen = 0;
    if (Dropped != 0) {
        (VOID)RtlStringCbPrintfA(Tail, sizeof(Tail), "(log full: %lu lines dropped)\r\n", Dropped);
        TailLen = strlen(Tail);
        RtlCopyMemory(Copy + Used, Tail, TailLen);
    }

    RtlInitUnicodeString(&Name, AUD_RUNLOG_FILE);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_WRITE | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OVERWRITE_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, NULL, 0);
    if (NT_SUCCESS(Status)) {
        (VOID)ZwWriteFile(File, NULL, NULL, NULL, &Iosb, Copy, (ULONG)(Used + TailLen), NULL, NULL);
        ZwClose(File);
    } else {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Exynos9820Audio: run log file %08x\n", Status);
    }

    ExFreePoolWithTag(Copy, AUD_POOL_TAG);
}
