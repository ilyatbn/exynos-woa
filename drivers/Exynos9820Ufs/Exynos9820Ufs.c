#include "Exynos9820Ufs.h"

const ULONG UfsDiagBinaryContract[] = UFS_DIAG_BINARY_CONTRACT_INITIALIZER;
static NTSTATUS UfsBugcheckProviderStatus = STATUS_DEVICE_NOT_READY;
static NTSTATUS UfsBugcheckDataStatus = STATUS_DEVICE_NOT_READY;
static NTSTATUS UfsBugcheckRegistrationStatus = STATUS_DEVICE_NOT_READY;
static BOOLEAN UfsBugcheckSlotAvailable = FALSE;
static PUFS_ADAPTER_EXTENSION UfsBugcheckAdapter = NULL;
static ULONG UfsBugcheckSlot = 0;
static KBUGCHECK_CALLBACK_RECORD UfsBugcheckCallbackRecord;
static BOOLEAN UfsBugcheckCallbackRegistered = FALSE;
static UCHAR UfsBugcheckComponent[] = "Exynos9820Ufs";

static
VOID
UfsRwd1SetFatalNoPet(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG NoPetReason,
    _In_ ULONG RecordReason,
    _In_ ULONG Phase,
    _In_ ULONG Detail
    );

static
VOID
UfsRwd1PrepareRecovery(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG NoPetReason,
    _In_ ULONG RecordReason,
    _In_ ULONG Phase,
    _In_ ULONG Detail
    );


static
VOID
UfsInitializeKernelContract(
    VOID
    )
{
    /* PASSIVE_LEVEL, before Storport can register or invoke the callback. */
    UfsBugcheckProviderStatus = AuxKlibInitialize();
}

static
ULONG
UfsGetConfigCapabilities(
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo
    )
{
    ULONG Capabilities = 0;

    if (ConfigInfo->ScatterGather) {
        Capabilities |= UFS_DIAG_CONFIG_SCATTER_GATHER;
    }
    if (ConfigInfo->Master) {
        Capabilities |= UFS_DIAG_CONFIG_MASTER;
    }
    if (ConfigInfo->NeedPhysicalAddresses) {
        Capabilities |= UFS_DIAG_CONFIG_NEED_PHYSICAL;
    }
    if (ConfigInfo->TaggedQueuing) {
        Capabilities |= UFS_DIAG_CONFIG_TAGGED_QUEUING;
    }
    if (ConfigInfo->AutoRequestSense) {
        Capabilities |= UFS_DIAG_CONFIG_AUTO_REQUEST_SENSE;
    }
    if (ConfigInfo->MultipleRequestPerLu) {
        Capabilities |= UFS_DIAG_CONFIG_MULTIPLE_REQUESTS;
    }

    return Capabilities;
}

static
ULONG
UfsReadRegister(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Offset
    )
{
    return StorPortReadRegisterUlong(
        Adapter,
        (PULONG)(Adapter->Hci + Offset)
        );
}

static
VOID
UfsWriteRegister(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    StorPortWriteRegisterUlong(
        Adapter,
        (PULONG)(Adapter->Hci + Offset),
        Value
        );
}

static
BOOLEAN
UfsPollRegister(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Expected,
    _In_ ULONG TimeoutUs
    )
{
    ULONG Elapsed;

    for (Elapsed = 0; ; Elapsed += UFS_TRANSFER_POLL_US) {
        if ((UfsReadRegister(Adapter, Offset) & Mask) ==
            (Expected & Mask)) {
            return TRUE;
        }
        if (Elapsed >= TimeoutUs) {
            return FALSE;
        }
        StorPortStallExecution(UFS_TRANSFER_POLL_US);
    }
}

static
ULONG_PTR
UfsAlignUp(
    _In_ ULONG_PTR Value,
    _In_ ULONG Alignment
    )
{
    return (Value + Alignment - 1U) & ~((ULONG_PTR)Alignment - 1U);
}

static
ULONG
UfsReadBigEndian32(
    _In_reads_(4) const UCHAR *Bytes
    )
{
    return ((ULONG)Bytes[0] << 24) |
           ((ULONG)Bytes[1] << 16) |
           ((ULONG)Bytes[2] << 8) |
           (ULONG)Bytes[3];
}

static
USHORT
UfsReadBigEndian16(
    _In_reads_(2) const UCHAR *Bytes
    )
{
    return (USHORT)(((USHORT)Bytes[0] << 8) | Bytes[1]);
}

static
ULONGLONG
UfsReadBigEndian64(
    _In_reads_(8) const UCHAR *Bytes
    )
{
    return ((ULONGLONG)UfsReadBigEndian32(Bytes) << 32) |
           UfsReadBigEndian32(Bytes + 4);
}

static
VOID
UfsWriteBigEndian32(
    _Out_writes_(4) UCHAR *Bytes,
    _In_ ULONG Value
    )
{
    volatile UCHAR *Output;

    Output = (volatile UCHAR *)Bytes;
    Output[0] = (UCHAR)(Value >> 24);
    Output[1] = (UCHAR)(Value >> 16);
    Output[2] = (UCHAR)(Value >> 8);
    Output[3] = (UCHAR)Value;
}

/*
 * V33 latency instrumentation primitive.
 *
 * CNTVCT_EL0 is the ARM generic timer's virtual counter: a free-running,
 * system-wide, monotonic tick source readable with a single MRS at any IRQL.
 * That matters because these call sites run inside the Storport dispatch path
 * with interrupts gated, where a KeQueryPerformanceCounter round-trip would be
 * both heavier than the thing being measured and harder to justify.
 *
 * Deliberately NOT serialised with an ISB. An ISB here would cost more than the
 * counter read and would change the timing it is supposed to observe; the
 * quantities being measured are thousands of ticks, so a few ticks of read
 * skew is irrelevant. Nothing branches on these values.
 */
#if UFS_PERF_INSTRUMENTATION
static
__forceinline
ULONGLONG
UfsPerfTick(
    VOID
    )
{
    return (ULONGLONG)_ReadStatusReg(ARM64_CNTVCT);
}
#endif

static
VOID
UfsZeroUncached(
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    volatile ULONG *Words;
    volatile UCHAR *Bytes;

    NT_ASSERT((((ULONG_PTR)Buffer) & (sizeof(ULONG) - 1U)) == 0);
    Words = (volatile ULONG *)Buffer;

#if UFS_WIDE_UNCACHED_ACCESS
    /*
     * Halve the bus transactions when the buffer is 8-byte aligned, which the
     * page-aligned bounce always is. Falls through to the ULONG loop for the
     * tail and for any caller that is only ULONG aligned.
     */
    if ((((ULONG_PTR)Buffer) & (sizeof(ULONGLONG) - 1U)) == 0) {
        volatile ULONGLONG *Quads = (volatile ULONGLONG *)Buffer;

        while (Length >= sizeof(ULONGLONG)) {
            *Quads++ = 0;
            Length -= sizeof(ULONGLONG);
        }
        Words = (volatile ULONG *)Quads;
    }
#endif

    while (Length >= sizeof(ULONG)) {
        *Words++ = 0;
        Length -= sizeof(ULONG);
    }

    Bytes = (volatile UCHAR *)Words;
    while (Length != 0) {
        *Bytes++ = 0;
        --Length;
    }
}

static
VOID
UfsCopyToUncached(
    _Out_writes_bytes_(Length) PVOID Destination,
    _In_reads_bytes_(Length) const VOID *Source,
    _In_ ULONG Length
    )
{
    volatile ULONG *OutputWords;
    const ULONG *InputWords;
    volatile UCHAR *OutputBytes;
    const UCHAR *InputBytes;

    /*
     * Storport enforces the adapter's ULONG alignment mask for data buffers,
     * while the bounce is page aligned. Transfer those buffers a word at a
     * time: volatile byte stores against Normal-NC memory serialize one bus
     * transaction per byte and dominate boot. Keep the byte path for small
     * protocol fields whose cached source is not naturally aligned.
     */
    if ((((ULONG_PTR)Destination | (ULONG_PTR)Source) &
         (sizeof(ULONG) - 1U)) == 0) {
        OutputWords = (volatile ULONG *)Destination;
        InputWords = (const ULONG *)Source;

#if UFS_WIDE_UNCACHED_ACCESS
        /*
         * Both pointers 8-byte aligned: move 8 bytes per bus transaction
         * instead of 4. Storport only guarantees the adapter's ULONG mask on
         * Srb->DataBuffer, so this is checked rather than assumed.
         */
        if ((((ULONG_PTR)Destination | (ULONG_PTR)Source) &
             (sizeof(ULONGLONG) - 1U)) == 0) {
            volatile ULONGLONG *OutputQuads = (volatile ULONGLONG *)Destination;
            const ULONGLONG *InputQuads = (const ULONGLONG *)Source;

            while (Length >= sizeof(ULONGLONG)) {
                *OutputQuads++ = *InputQuads++;
                Length -= sizeof(ULONGLONG);
            }
            OutputWords = (volatile ULONG *)OutputQuads;
            InputWords = (const ULONG *)InputQuads;
        }
#endif

        while (Length >= sizeof(ULONG)) {
            *OutputWords++ = *InputWords++;
            Length -= sizeof(ULONG);
        }
        OutputBytes = (volatile UCHAR *)OutputWords;
        InputBytes = (const UCHAR *)InputWords;
    } else {
        OutputBytes = (volatile UCHAR *)Destination;
        InputBytes = (const UCHAR *)Source;
    }

    while (Length != 0) {
        *OutputBytes++ = *InputBytes++;
        --Length;
    }
}

static
VOID
UfsCopyFromUncached(
    _Out_writes_bytes_(Length) PVOID Destination,
    _In_reads_bytes_(Length) const VOID *Source,
    _In_ ULONG Length
    )
{
    ULONG *OutputWords;
    const volatile ULONG *InputWords;
    UCHAR *OutputBytes;
    const volatile UCHAR *InputBytes;

    if ((((ULONG_PTR)Destination | (ULONG_PTR)Source) &
         (sizeof(ULONG) - 1U)) == 0) {
        OutputWords = (ULONG *)Destination;
        InputWords = (const volatile ULONG *)Source;

#if UFS_WIDE_UNCACHED_ACCESS
        /* Same widening as the write path; see UfsCopyToUncached. */
        if ((((ULONG_PTR)Destination | (ULONG_PTR)Source) &
             (sizeof(ULONGLONG) - 1U)) == 0) {
            ULONGLONG *OutputQuads = (ULONGLONG *)Destination;
            const volatile ULONGLONG *InputQuads =
                (const volatile ULONGLONG *)Source;

            while (Length >= sizeof(ULONGLONG)) {
                *OutputQuads++ = *InputQuads++;
                Length -= sizeof(ULONGLONG);
            }
            OutputWords = (ULONG *)OutputQuads;
            InputWords = (const volatile ULONG *)InputQuads;
        }
#endif

        while (Length >= sizeof(ULONG)) {
            *OutputWords++ = *InputWords++;
            Length -= sizeof(ULONG);
        }
        OutputBytes = (UCHAR *)OutputWords;
        InputBytes = (const volatile UCHAR *)InputWords;
    } else {
        OutputBytes = (UCHAR *)Destination;
        InputBytes = (const volatile UCHAR *)Source;
    }

    while (Length != 0) {
        *OutputBytes++ = *InputBytes++;
        --Length;
    }
}

/*
 * ===========================================================================
 * GPT HEADER TRANSLATION
 * ===========================================================================
 *
 * ROOT CAUSE. Windows only accepts a GPT header whose HeaderSize field is
 * exactly 92; any other value makes the disk read as corrupt
 * (STATUS_DISK_CORRUPT_ERROR) and no partitions are created. This disk
 * declares HeaderSize = 512 on BOTH the primary (LBA 1) and the backup (last
 * LBA), so both are rejected before the CRC is ever checked. UEFI 2.10
 * Table 5-5 permits 92..logical-block-size, so the media is spec-correct and
 * Windows is simply stricter than the spec.
 *
 * FIX. Translate the header in flight, in the buffer handed back to Windows.
 * The media is never touched, so reverting is just reflashing the WIM. Six
 * bytes change: HeaderSize at offset 12 becomes 92, and the header CRC at
 * offset 16 is recomputed. HeaderSize lives INSIDE the CRC'd region, so the CRC
 * must be recomputed AFTER setting it - for this disk the correct value is
 * 0x5E2BF25B, verified against the real LBA 1 bytes. The entry array at LBA 2..5 is deliberately
 * left alone; its CRC covers only the entries and stays valid.
 *
 * SAFETY. A header is only rewritten once it has been proven valid on its own
 * terms: signature, revision 1.0, 92 < HeaderSize <= the block, and a stored
 * CRC32 over HeaderSize bytes that validates. Anything else is counted as a
 * rejection and left byte-untouched. Random data cannot pass a CRC32, so
 * scanning every logical-block boundary of the transfer is self-limiting.
 *
 * WHERE. This runs on Srb->DataBuffer, which is ordinary cached memory (see
 * UfsCopyFromUncached's non-volatile destination pointer above), NOT on the
 * bounce. The bounce is MmNonCached, and on Windows ARM64 an unaligned access
 * to uncached memory raises a synchronous alignment Data Abort - exactly the
 * SYSTEM_SERVICE_EXCEPTION that cost this project the V16/V20-V23 builds. Every
 * access below is bytewise anyway, so the edit is alignment-safe regardless of
 * how Srb->DataBuffer happens to be aligned. Leaving the bounce pristine also
 * preserves UfsCaptureBounceEvidence's sample and the verify-after-write source.
 */

static
ULONG
UfsGptReadLittleEndian32(
    _In_reads_bytes_(4) const UCHAR *Input
    )
{
    return ((ULONG)Input[0]) |
           (((ULONG)Input[1]) << 8) |
           (((ULONG)Input[2]) << 16) |
           (((ULONG)Input[3]) << 24);
}

static
VOID
UfsGptWriteLittleEndian32(
    _Out_writes_bytes_(4) PUCHAR Output,
    _In_ ULONG Value
    )
{
    Output[0] = (UCHAR)(Value & 0xFFUL);
    Output[1] = (UCHAR)((Value >> 8) & 0xFFUL);
    Output[2] = (UCHAR)((Value >> 16) & 0xFFUL);
    Output[3] = (UCHAR)((Value >> 24) & 0xFFUL);
}

static
ULONG
UfsGptCrc32(
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length,
    _In_ ULONG SkipOffset,
    _In_ ULONG SkipLength
    )
{
    ULONG Crc;
    ULONG Index;
    ULONG Bit;
    ULONG Byte;

    /*
     * Bytes in [SkipOffset, SkipOffset + SkipLength) are read as zero rather
     * than being zeroed in place, so a header that later fails validation is
     * left byte-untouched. SkipLength == 0 degenerates correctly to "no skip":
     * (Index - SkipOffset) < 0 is never true in unsigned arithmetic.
     */
    Crc = 0xFFFFFFFFUL;
    for (Index = 0; Index < Length; ++Index) {
        Byte = (ULONG)Data[Index];
        if ((Index >= SkipOffset) && ((Index - SkipOffset) < SkipLength)) {
            Byte = 0;
        }

        Crc ^= Byte;
        for (Bit = 0; Bit < 8; ++Bit) {
            /*
             * 0UL - (Crc & 1) is 0xFFFFFFFF when the low bit is set and 0
             * otherwise, so the polynomial is applied branchlessly and without
             * a 1 KiB table.
             */
            Crc = (Crc >> 1) ^ (UFS_GPT_CRC32_POLYNOMIAL & (0UL - (Crc & 1UL)));
        }
    }

    return Crc ^ 0xFFFFFFFFUL;
}

static
ULONG
UfsTranslateGptHeaderBlock(
    _Inout_updates_bytes_(Available) PUCHAR Block,
    _In_ ULONG Available
    )
{
    ULONG HeaderSize;
    ULONG StoredCrc;
    ULONG ComputedCrc;

    if (Available < UFS_GPT_HEADER_SIZE_WINDOWS) {
        return UFS_GPT_XLATE_IGNORED;
    }

    if ((UfsGptReadLittleEndian32(Block) != UFS_GPT_HEADER_SIGNATURE_0) ||
        (UfsGptReadLittleEndian32(Block + 4) != UFS_GPT_HEADER_SIGNATURE_1)) {
        return UFS_GPT_XLATE_IGNORED;
    }

    if (UfsGptReadLittleEndian32(Block + UFS_GPT_HEADER_OFFSET_REVISION) !=
        UFS_GPT_HEADER_REVISION_1_0) {
        return UFS_GPT_XLATE_IGNORED;
    }

    HeaderSize = UfsGptReadLittleEndian32(
        Block + UFS_GPT_HEADER_OFFSET_HEADER_SIZE
        );
    if (HeaderSize == UFS_GPT_HEADER_SIZE_WINDOWS) {
        /* Already compatible - nothing to do, and nothing to count. */
        return UFS_GPT_XLATE_IGNORED;
    }

    if ((HeaderSize < UFS_GPT_HEADER_SIZE_WINDOWS) || (HeaderSize > Available)) {
        return UFS_GPT_XLATE_REJECTED;
    }

    /*
     * Validate the header on its own terms before rewriting a single byte. The
     * stored CRC covers HeaderSize bytes with the CRC field itself read as
     * zero.
     */
    StoredCrc = UfsGptReadLittleEndian32(
        Block + UFS_GPT_HEADER_OFFSET_HEADER_CRC
        );
    ComputedCrc = UfsGptCrc32(
        Block,
        HeaderSize,
        UFS_GPT_HEADER_OFFSET_HEADER_CRC,
        UFS_GPT_HEADER_CRC_LENGTH
        );
    if (ComputedCrc != StoredCrc) {
        return UFS_GPT_XLATE_REJECTED;
    }

    UfsGptWriteLittleEndian32(
        Block + UFS_GPT_HEADER_OFFSET_HEADER_SIZE,
        UFS_GPT_HEADER_SIZE_WINDOWS
        );
    UfsGptWriteLittleEndian32(
        Block + UFS_GPT_HEADER_OFFSET_HEADER_CRC,
        0UL
        );

    /*
     * HeaderSize is inside the CRC'd region, so this must happen AFTER the
     * store above. Computing it from the pre-translation bytes yields a value
     * Windows will reject.
     */
    ComputedCrc = UfsGptCrc32(Block, UFS_GPT_HEADER_SIZE_WINDOWS, 0UL, 0UL);
    UfsGptWriteLittleEndian32(
        Block + UFS_GPT_HEADER_OFFSET_HEADER_CRC,
        ComputedCrc
        );

    return UFS_GPT_XLATE_TRANSLATED;
}

static
VOID
UfsTranslateGptHeaders(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _Inout_updates_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length
    )
{
    ULONG Stride;
    ULONG Offset;
    ULONG Available;
    ULONG Result;

    if ((Buffer == NULL) || (Length < UFS_GPT_HEADER_SIZE_WINDOWS)) {
        return;
    }

    /*
     * Capacity may not have been read yet when the first GPT read arrives, so
     * fall back to the architectural block size rather than scanning at a
     * nonsense stride.
     */
    Stride = Adapter->LogicalBlockSize;
    if ((Stride == 0) || (Stride < UFS_MIN_LOGICAL_BLOCK_SIZE)) {
        Stride = UFS_LOGICAL_BLOCK_SIZE;
    }

    /*
     * ClassPnP/partmgr may issue multi-block reads, so scan every logical-block
     * boundary rather than only offset 0. A header is confined to its own
     * block, hence the clamp of Available to Stride.
     */
    for (Offset = 0;
         (Offset + UFS_GPT_HEADER_SIZE_WINDOWS) <= Length;
         Offset += Stride) {

        Available = Length - Offset;
        if (Available > Stride) {
            Available = Stride;
        }

        Result = UfsTranslateGptHeaderBlock(Buffer + Offset, Available);
        if (Result == UFS_GPT_XLATE_TRANSLATED) {
            Adapter->GptHeadersTranslated += 1;
        } else if (Result == UFS_GPT_XLATE_REJECTED) {
            Adapter->GptHeadersRejected += 1;
        }
    }
}

static
VOID
UfsInitializeDiagnostic(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    RtlZeroMemory(&Adapter->Diagnostic, sizeof(Adapter->Diagnostic));
    Adapter->Diagnostic.Signature = UFS_DIAG_DATA_SIGNATURE;
    Adapter->Diagnostic.Version = UFS_DIAG_DATA_VERSION;
    Adapter->Diagnostic.Size = sizeof(Adapter->Diagnostic);
    Adapter->Diagnostic.InitializationCapabilities =
        UFS_DIAG_INIT_HW_INTERRUPT |
        UFS_DIAG_INIT_NEED_PHYSICAL |
        UFS_DIAG_INIT_AUTO_REQUEST_SENSE;
    Adapter->Diagnostic.UtrdInterruptRequested = 0;
    Adapter->Diagnostic.FailureMask |=
        UFS_DIAG_FAILURE_PRIVATE_TELEMETRY_DISABLED;
    if (!NT_SUCCESS(UfsBugcheckProviderStatus)) {
        Adapter->Diagnostic.FailureMask |= UFS_DIAG_FAILURE_BUGCHECK_INIT;
    }
}

static
VOID
UfsSetDiagnosticStage(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Stage
    )
{
    Adapter->Diagnostic.Stage = Stage;
}

static
VOID
UfsSetDiagnosticFailure(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONGLONG Failure
    )
{
    Adapter->Diagnostic.FailureMask |= Failure;
}

static
BOOLEAN
UfsAcknowledgeInterruptStatus(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    ULONG Status;

    Status = UfsReadRegister(Adapter, UFS_REG_INTERRUPT_STATUS);
    Adapter->Diagnostic.InterruptStatus = Status;
    if (Status == 0) {
        return TRUE;
    }

    //
    // Acknowledge exactly the bits that were observed. IS is RW1C, so writing
    // the sampled value clears precisely those bits and cannot disturb one that
    // the controller latched afterwards.
    //
    // Do NOT require IS to read back as zero. Completion is already established
    // by the doorbell, so IS is advisory here, and on this warm-handoff
    // controller the link re-latches informational bits (auto-hibernate
    // enter/exit above all) faster than any poll can drain them. Demanding zero
    // makes an otherwise healthy command fail and, because the caller contains
    // the controller on failure, permanently disables the adapter for the rest
    // of the boot.
    //
    UfsWriteRegister(Adapter, UFS_REG_INTERRUPT_STATUS, Status);
    KeMemoryBarrier();

    if ((Status & UFS_INTERRUPT_STATUS_FATAL_MASK) != 0) {
        UfsSetDiagnosticFailure(
            Adapter,
            UFS_DIAG_FAILURE_INTERRUPT_STATUS
            );
        return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
UfsStopTransferList(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    UfsWriteRegister(Adapter, UFS_REG_UTRL_RUN_STOP, 0);
    KeMemoryBarrier();
    if (!UfsPollRegister(
            Adapter,
            UFS_REG_UTRL_RUN_STOP,
            UFS_LIST_RUN_STOP,
            0,
            UFS_STOP_TIMEOUT_US
            )) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_STOP);
        return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
UfsStartTransferList(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    UfsWriteRegister(
        Adapter,
        UFS_REG_UTRL_RUN_STOP,
        UFS_LIST_RUN_STOP
        );
    KeMemoryBarrier();
    return UfsPollRegister(
        Adapter,
        UFS_REG_UTRL_RUN_STOP,
        UFS_LIST_RUN_STOP,
        UFS_LIST_RUN_STOP,
        UFS_STOP_TIMEOUT_US
        );
}

/*
 * Persistent-RAM telemetry.
 *
 * Every artifact ufs-load-v9.cmd produces lands on X:, the WinPE ramdisk, and
 * dies with the reboot. Nothing can be written back to WINSETUP either, because
 * reaching the disk from Windows requires the very driver under test. The net
 * effect is that each diagnostic cycle needs a human to read values off the
 * phone screen and re-type them, which is the rate limiter on this whole
 * investigation.
 *
 * The firmware's breadcrumb console does not have that problem: it lives in a
 * Device-mapped window above the end of DRAM, survives the PMU warm reset, and
 * TWRP re-exports the same physical bytes as /sys/fs/pstore/pmsg-ramoops-0. So
 * append the adapter state there and the next TWRP session - which the deploy
 * script already establishes - can pull it with no extra user action.
 *
 * Safety properties, in order of importance:
 *   - Every store is a single byte or an aligned ULONG. There is no 16-bit
 *     store anywhere in this path, so it cannot reproduce the unaligned-STRH
 *     fault that cost V16-V23.
 *   - The mapping is probed once (write magic, read it back). A window that
 *     does not read back is abandoned for the rest of the boot, so a bad or
 *     absent mapping degrades to silence rather than to a bugcheck.
 *   - Every entry point is guarded on Adapter->Pram being non-NULL.
 *   - The cursor is bounds-checked against UFS_PRAM_CAPACITY on every byte.
 */
static
VOID
UfsPramByte(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ UCHAR Value
    )
{
    volatile ULONG *Header;
    ULONG Position;

    if (Adapter->Pram == NULL) {
        return;
    }

    Header = (volatile ULONG *)Adapter->Pram;
    Position = Header[2];
    /*
     * Leave the final 1 KB for ResetSystemRuntimeDxe. Its warm/cold/shutdown
     * markers use the same cursor at runtime, after this miniport has stopped.
     */
    if (Position >= UFS_PRAM_DRIVER_CAPACITY) {
        return;
    }

    Adapter->Pram[UFS_PRAM_HEADER_SIZE + Position] = Value;
    Header[2] = Position + 1UL;
}

static
VOID
UfsPramString(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_z_ const char *Text
    )
{
    while (*Text != '\0') {
        UfsPramByte(Adapter, (UCHAR)*Text);
        Text++;
    }
}

static
VOID
UfsPramHex32(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Value
    )
{
    static const char Digits[] = "0123456789abcdef";
    ULONG Index;

    for (Index = 0; Index < 8; Index++) {
        UfsPramByte(
            Adapter,
            (UCHAR)Digits[(Value >> (28U - (Index * 4U))) & 0xFU]
            );
    }
}

static
VOID
UfsPramHex64(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONGLONG Value
    )
{
    UfsPramHex32(Adapter, (ULONG)(Value >> 32));
    UfsPramHex32(Adapter, (ULONG)(Value & 0xFFFFFFFFULL));
}

/*
 * Two nibbles, for byte dumps. The ring is ~16 KB and a record is ~450 bytes,
 * so spending eight characters per byte via UfsPramHex32 would cost more of the
 * ring than the rest of the record put together.
 */
static
VOID
UfsPramHex8(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ UCHAR Value
    )
{
    static const char Digits[] = "0123456789abcdef";

    UfsPramByte(Adapter, (UCHAR)Digits[(Value >> 4) & 0xFU]);
    UfsPramByte(Adapter, (UCHAR)Digits[Value & 0xFU]);
}

static
VOID
UfsPramField(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_z_ const char *Tag,
    _In_ ULONG Value
    )
{
    UfsPramByte(Adapter, (UCHAR)' ');
    UfsPramString(Adapter, Tag);
    UfsPramByte(Adapter, (UCHAR)'=');
    UfsPramHex32(Adapter, Value);
}

/*
 * RESERVED-SLOT NOTE CHANNEL.
 *
 * Everything above appends and moves the cursor. These two do not: they write
 * at an absolute ring offset and leave the cursor alone, which is the whole
 * point. A note is emitted at the very end of the boot, when the ring is
 * already full and UfsPramByte would discard it, so the space has to be claimed
 * up front and overwritten later.
 *
 * The same single-byte-store discipline applies. There is deliberately no
 * 16-bit or wider store here, so this path is structurally incapable of
 * reproducing the unaligned-STRH bugcheck class.
 */
static
VOID
UfsPramNoteByte(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Offset,
    _In_ UCHAR Value
    )
{
    if (Adapter->Pram == NULL) {
        return;
    }

    if (Offset >= UFS_PRAM_CAPACITY) {
        return;
    }

    Adapter->Pram[UFS_PRAM_HEADER_SIZE + Offset] = Value;
}

static
VOID
UfsBugcheckSlotByte(
    _In_ ULONG Index,
    _In_ UCHAR Value
    )
{
    if (!UfsBugcheckSlotAvailable) {
        return;
    }
    if ((UfsBugcheckAdapter == NULL) ||
        (UfsBugcheckAdapter->Pram == NULL) ||
        (Index >= UFS_PRAM_BUGCHECK_SLOT_SIZE)) {
        return;
    }

    UfsPramNoteByte(
        UfsBugcheckAdapter,
        UfsBugcheckSlot + Index,
        Value
        );
}

static
ULONG
UfsBugcheckSlotString(
    _In_ ULONG Cursor,
    _In_z_ const char *Text
    )
{
    while ((*Text != '\0') && (Cursor < UFS_PRAM_BUGCHECK_SLOT_SIZE)) {
        UfsBugcheckSlotByte(Cursor, (UCHAR)*Text);
        Cursor++;
        Text++;
    }

    return Cursor;
}

static
ULONG
UfsBugcheckSlotHex64(
    _In_ ULONG Cursor,
    _In_ ULONGLONG Value
    )
{
    static const char Digits[] = "0123456789abcdef";
    ULONG Index;

    for (Index = 0;
         (Index < 16UL) && (Cursor < UFS_PRAM_BUGCHECK_SLOT_SIZE);
         Index++) {
        UfsBugcheckSlotByte(
            Cursor,
            (UCHAR)Digits[(Value >> (60U - (Index * 4U))) & 0xFU]
            );
        Cursor++;
    }

    return Cursor;
}

static
VOID
UfsBugcheckSlotReset(
    VOID
    )
{
    ULONG Index;

    for (Index = 0; Index < UFS_PRAM_BUGCHECK_SLOT_SIZE; Index++) {
        UfsBugcheckSlotByte(Index, (Index == 0) ? (UCHAR)'\n' : (UCHAR)'.');
    }
}

static
ULONG
UfsBugcheckPlatformPowerFallback(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_reads_(5) const ULONGLONG *BugcheckData,
    _In_ BOOLEAN CleanShutdown,
    _In_ ULONG Cursor
    )
{
    volatile ULONG *Inform2;
    volatile ULONG *Inform3;
    volatile ULONG *Sysip;
    volatile ULONG *SoftReset;
    volatile ULONG *PsHold;
    ULONG Value;
    ULONG ExpectedInform2;
    ULONG ExpectedInform3;

    if ((Adapter->Pmu == NULL) ||
        (BugcheckData[0] != 0xA0ULL) ||
        (BugcheckData[1] != 0xBEEFULL) ||
        (BugcheckData[2] != 0x8004ULL) ||
        (BugcheckData[4] != 0xFFFFFFFFULL)) {
        return Cursor;
    }

    if (BugcheckData[3] == 0x84000009ULL) {
        Inform2 = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_INFORM2);
        Inform3 = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_INFORM3);
        Sysip = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_SYSIP_DAT0);
        SoftReset = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_SWRESET);

        /* Preserve a completed clean handoff on the existing OS reset path. */
        ExpectedInform3 = CleanShutdown ?
            UFS_PMU_REBOOT_REASON_RECOVERY : UFS_PMU_REBOOT_REASON_NORMAL;
        ExpectedInform2 = (ExpectedInform3 == UFS_PMU_REBOOT_REASON_RECOVERY) ?
            0UL : UFS_PMU_SEC_POWER_RESET;
        *Inform2 = ExpectedInform2;
        *Sysip = 0UL;
        KeMemoryBarrier();
        *Inform3 = ExpectedInform3;
        KeMemoryBarrier();

        if ((*Inform2 != ExpectedInform2) ||
            (*Inform3 != ExpectedInform3) ||
            (*Sysip != 0UL)) {
            return UfsBugcheckSlotString(Cursor, " ACT=ARMFAIL");
        }

        Cursor = UfsBugcheckSlotString(Cursor, " ACT=RESET");
        UfsBugcheckSlotByte(Cursor, (UCHAR)'\n');
        KeMemoryBarrier();
        *SoftReset = 1UL;
        KeMemoryBarrier();
        return Cursor;
    }

    if (BugcheckData[3] == 0x84000008ULL) {
        PsHold = (volatile ULONG *)(
            Adapter->Pmu + UFS_PMU_PS_HOLD_CONTROL
            );
        Value = *PsHold;
        Cursor = UfsBugcheckSlotString(Cursor, " ACT=SHUTDOWN");
        UfsBugcheckSlotByte(Cursor, (UCHAR)'\n');
        KeMemoryBarrier();
        *PsHold = Value & ~UFS_PMU_PS_HOLD_DATA;
        KeMemoryBarrier();
    }

    return Cursor;
}

static
BOOLEAN
UfsBugcheckIsPlatformPowerRequest(
    _In_reads_(5) const ULONGLONG *BugcheckData
    )
{
    return (BOOLEAN)(
        (BugcheckData[0] == 0xA0ULL) &&
        (BugcheckData[1] == 0xBEEFULL) &&
        (BugcheckData[2] == 0x8004ULL) &&
        (BugcheckData[4] == 0xFFFFFFFFULL) &&
        ((BugcheckData[3] == 0x84000008ULL) ||
         (BugcheckData[3] == 0x84000009ULL))
        );
}

static
VOID
UfsBugcheckRecoveryReset(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_reads_(5) const ULONGLONG *BugcheckData
    )
{
    volatile ULONG *Inform2;
    volatile ULONG *Inform3;
    volatile ULONG *Sysip;
    volatile ULONG *SoftReset;

    if ((Adapter == NULL) ||
        (Adapter->Pmu == NULL) ||
        UfsBugcheckIsPlatformPowerRequest(BugcheckData)) {
        return;
    }

    UfsRwd1PrepareRecovery(
        Adapter,
        UFS_NO_PET_BUGCHECK,
        RWD1_REASON_WINDOWS_BUGCHECK,
        RWD1_PHASE_WINDOWS_BUGCHECK,
        (ULONG)BugcheckData[0]
        );

    Inform2 = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_INFORM2);
    Inform3 = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_INFORM3);
    Sysip = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_SYSIP_DAT0);
    SoftReset = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_SWRESET);

    *Inform2 = 0UL;
    *Sysip = 0UL;
    *Inform3 = UFS_PMU_REBOOT_REASON_RECOVERY;
    KeMemoryBarrier();
    if ((*Inform2 == 0UL) &&
        (*Inform3 == UFS_PMU_REBOOT_REASON_RECOVERY) &&
        (*Sysip == 0UL)) {
        *SoftReset = 1UL;
        KeMemoryBarrier();
    }
}

static
VOID
UfsBugcheckCapture(
    _In_ PVOID Buffer,
    _In_ ULONG Length
    )
{
    KBUGCHECK_DATA PublicData = {0};
    NTSTATUS Status;
    ULONGLONG BugcheckData[5];
    /* Fatal-no-pet bookkeeping below deliberately clears WatchdogStopComplete. */
    BOOLEAN CleanShutdown = (BOOLEAN)(
        UfsBugcheckAdapter != NULL &&
        UfsBugcheckAdapter->CleanRecoveryState == UFS_CLEAN_RECOVERY_COMMITTED &&
        UfsBugcheckAdapter->ShutdownPending &&
        UfsBugcheckAdapter->WatchdogStopComplete
        );
    static const char * const Tags[5] = {
        "=UFSBUGCHECK BC=",
        " B1=",
        " B2=",
        " B3=",
        " B4="
    };
    ULONG Cursor;
    ULONG Index;

    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);

    if (UfsBugcheckAdapter == NULL) {
        /* No mapped adapter can be touched; retain the failure for a debugger. */
        UfsBugcheckDataStatus = STATUS_DEVICE_NOT_READY;
        KeMemoryBarrier();
        return;
    }

    Status = UfsBugcheckProviderStatus;
    if (NT_SUCCESS(Status)) {
        PublicData.BugCheckDataSize = sizeof(PublicData);
        Status = AuxKlibGetBugCheckData(&PublicData);
        if (NT_SUCCESS(Status) &&
            (PublicData.BugCheckDataSize != sizeof(PublicData))) {
            Status = STATUS_INFO_LENGTH_MISMATCH;
        }
    }
    UfsBugcheckDataStatus = Status;
    if (!NT_SUCCESS(Status)) {
        UfsSetDiagnosticFailure(
            UfsBugcheckAdapter,
            UFS_DIAG_FAILURE_BUGCHECK_DATA
            );
        UfsBugcheckAdapter->FatalError = TRUE;
        UfsBugcheckAdapter->Diagnostic.FatalError = 1UL;
        UfsRwd1SetFatalNoPet(
            UfsBugcheckAdapter,
            UFS_NO_PET_BUGCHECK,
            RWD1_REASON_WINDOWS_BUGCHECK,
            RWD1_PHASE_WINDOWS_BUGCHECK,
            (ULONG)Status
            );
        UfsBugcheckSlotReset();
        Cursor = UfsBugcheckSlotString(
            1, "=UFSBUGCHECK DATA=UNAVAILABLE STATUS=");
        Cursor = UfsBugcheckSlotHex64(Cursor, (ULONG)Status);
        Cursor = UfsBugcheckSlotString(Cursor, " ACT=NO_PET");
        UfsBugcheckSlotByte(Cursor, (UCHAR)'\n');
        KeMemoryBarrier();
        /* Do not guess a power/recovery tuple. Leave the watchdog untouched. */
        return;
    }

    BugcheckData[0] = PublicData.BugCheckCode;
    BugcheckData[1] = PublicData.Parameter1;
    BugcheckData[2] = PublicData.Parameter2;
    BugcheckData[3] = PublicData.Parameter3;
    BugcheckData[4] = PublicData.Parameter4;
    UfsBugcheckAdapter->FatalError = TRUE;
    UfsBugcheckAdapter->Diagnostic.FatalError = 1UL;
    UfsRwd1SetFatalNoPet(
        UfsBugcheckAdapter,
        UFS_NO_PET_BUGCHECK,
        RWD1_REASON_WINDOWS_BUGCHECK,
        RWD1_PHASE_WINDOWS_BUGCHECK,
        (ULONG)BugcheckData[0]
        );

    UfsBugcheckSlotReset();
    Cursor = 1;
    for (Index = 0; Index < RTL_NUMBER_OF(Tags); Index++) {
        Cursor = UfsBugcheckSlotString(Cursor, Tags[Index]);
        Cursor = UfsBugcheckSlotHex64(Cursor, BugcheckData[Index]);
    }
    Cursor = UfsBugcheckPlatformPowerFallback(
        UfsBugcheckAdapter,
        BugcheckData,
        CleanShutdown,
        Cursor
        );
    UfsBugcheckSlotByte(Cursor, (UCHAR)'\n');
    KeMemoryBarrier();
    UfsBugcheckRecoveryReset(UfsBugcheckAdapter, BugcheckData);
}

static
VOID
UfsRegisterBugcheckCapture(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Header;
    ULONG Cursor;
    ULONG Index;
    BOOLEAN Registered;

    if (UfsBugcheckCallbackRegistered) {
        if (UfsBugcheckAdapter != Adapter) {
            UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_BUGCHECK_REGISTER);
        }
        return;
    }

    UfsBugcheckSlotAvailable = FALSE;
    if (Adapter->Pram != NULL) {
        Header = (volatile ULONG *)Adapter->Pram;
        UfsBugcheckSlot = Header[2];
        if (UfsBugcheckSlot <=
            UFS_PRAM_DRIVER_CAPACITY - UFS_PRAM_BUGCHECK_SLOT_SIZE) {
            for (Index = 0; Index < UFS_PRAM_BUGCHECK_SLOT_SIZE; Index++) {
                UfsPramByte(Adapter, (Index == 0) ? (UCHAR)'\n' : (UCHAR)'.');
            }
            UfsBugcheckSlotAvailable = TRUE;
        }
    }
    if (!UfsBugcheckSlotAvailable) {
        UfsSetDiagnosticFailure(
            Adapter, UFS_DIAG_FAILURE_BUGCHECK_PRAM_UNAVAILABLE);
    }

    UfsBugcheckAdapter = Adapter;
    KeMemoryBarrier();

    KeInitializeCallbackRecord(&UfsBugcheckCallbackRecord);
    Registered = KeRegisterBugCheckCallback(
        &UfsBugcheckCallbackRecord,
        UfsBugcheckCapture,
        NULL,
        0,
        UfsBugcheckComponent
        );
    UfsBugcheckCallbackRegistered = Registered;
    UfsBugcheckRegistrationStatus = Registered ?
        STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    if (!Registered) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_BUGCHECK_REGISTER);
    }

    UfsBugcheckSlotReset();
    Cursor = UfsBugcheckSlotString(1, "=UFSBUGCHECK ARMED=");
    Cursor = UfsBugcheckSlotHex64(Cursor, Registered ? 1ULL : 0ULL);
    Cursor = UfsBugcheckSlotString(Cursor, " AUXINIT=");
    Cursor = UfsBugcheckSlotHex64(Cursor, (ULONG)UfsBugcheckProviderStatus);
    Cursor = UfsBugcheckSlotString(Cursor, " REG=");
    Cursor = UfsBugcheckSlotHex64(Cursor, (ULONG)UfsBugcheckRegistrationStatus);
    UfsBugcheckSlotByte(Cursor, (UCHAR)'\n');
    KeMemoryBarrier();

}

static
BOOLEAN
UfsBugcheckContractReady(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    NTSTATUS Status = UfsBugcheckProviderStatus;

    if (!UfsBugcheckCallbackRegistered || UfsBugcheckAdapter != Adapter) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_BUGCHECK_REGISTER);
        if (NT_SUCCESS(Status)) {
            Status = NT_SUCCESS(UfsBugcheckRegistrationStatus) ?
                STATUS_DEVICE_NOT_READY : UfsBugcheckRegistrationStatus;
        }
    }
    if (!NT_SUCCESS(Status)) {
        Adapter->FatalError = TRUE;
        Adapter->Diagnostic.FatalError = 1UL;
        UfsRwd1SetFatalNoPet(
            Adapter,
            UFS_NO_PET_WATCHDOG_INVALID,
            RWD1_REASON_WINDOWS_NO_PET,
            RWD1_PHASE_WINDOWS_INITIALIZING,
            (ULONG)Status
            );
        return FALSE;
    }
    return TRUE;
}

/*
 * Reserve the note block. Must be called with the cursor at a known position -
 * in practice immediately after UfsPramInitialize resets it to zero, so the
 * slots land at ring offset 0 and are therefore always inside capacity no
 * matter how full the ring later becomes.
 *
 * Each slot is pre-filled '\n' followed by dots so that an unwritten slot is a
 * line the host decoder skips (it only accepts lines beginning "=UFS"), rather
 * than a partial record that could be mistaken for a truncated one.
 */
static
VOID
UfsPramReserveNotes(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Header;
    ULONG Slot;
    ULONG Index;

    if (Adapter->Pram == NULL) {
        return;
    }

    Header = (volatile ULONG *)Adapter->Pram;
    Adapter->PramNoteBase = Header[2];
    Adapter->PramNotesWritten = 0;

    if ((Adapter->PramNoteBase + UFS_PRAM_NOTE_RESERVED) > UFS_PRAM_CAPACITY) {
        Adapter->PramNotesReserved = FALSE;
        return;
    }

    for (Slot = 0; Slot < UFS_PRAM_NOTE_SLOTS; Slot++) {
        UfsPramByte(Adapter, (UCHAR)'\n');
        for (Index = 1; Index < UFS_PRAM_NOTE_SLOT_SIZE; Index++) {
            UfsPramByte(Adapter, (UCHAR)'.');
        }
    }

    Adapter->PramNotesReserved = TRUE;
}

/*
 * Rewrite one reserved slot in place:
 *
 *   =UFSNOTE NSEQ=xxxxxxxx NCODE=xxxxxxxx NAUX=xxxxxxxx NTAG=xxxxxxxx......
 *
 * All four values are eight hex digits because collect-pram.ps1's field regex
 * requires {8,} as a truncation guard - a shorter value is silently dropped.
 * The leading '\n' written by UfsPramReserveNotes is preserved, since it is the
 * delimiter the decoder splits records on.
 *
 * Slot-exhaustion policy: keep rewriting the last slot. The newest note names
 * the failure, and Diagnostic.NoteRequests diverging from PramNotesWritten is
 * what reveals that earlier ones were dropped.
 */
static
VOID
UfsPramNote(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Code,
    _In_ ULONG Aux,
    _In_ ULONG Tag
    )
{
    static const char Digits[] = "0123456789abcdef";
    static const char Prefix[] = "=UFSNOTE NSEQ=";
    ULONG Slot;
    ULONG Cursor;
    ULONG Index;
    ULONG Values[3];
    ULONG Value;

    if ((Adapter->Pram == NULL) || (!Adapter->PramNotesReserved)) {
        return;
    }

    Slot = Adapter->PramNotesWritten;
    if (Slot >= UFS_PRAM_NOTE_SLOTS) {
        Slot = UFS_PRAM_NOTE_SLOTS - 1UL;
    } else {
        Adapter->PramNotesWritten = Slot + 1UL;
    }

    /* +1 keeps the delimiter this slot starts with. */
    Cursor = Adapter->PramNoteBase + (Slot * UFS_PRAM_NOTE_SLOT_SIZE) + 1UL;

    for (Index = 0; Prefix[Index] != '\0'; Index++) {
        UfsPramNoteByte(Adapter, Cursor, (UCHAR)Prefix[Index]);
        Cursor++;
    }

    for (Index = 0; Index < 8; Index++) {
        UfsPramNoteByte(
            Adapter,
            Cursor,
            (UCHAR)Digits[(Adapter->NoteRequests >> (28U - (Index * 4U))) & 0xFU]
            );
        Cursor++;
    }

    Values[0] = Code;
    Values[1] = Aux;
    Values[2] = Tag;

    for (Value = 0; Value < 3; Value++) {
        static const char * const Tags[3] = { " NCODE=", " NAUX=", " NTAG=" };
        const char *Name = Tags[Value];

        for (Index = 0; Name[Index] != '\0'; Index++) {
            UfsPramNoteByte(Adapter, Cursor, (UCHAR)Name[Index]);
            Cursor++;
        }

        for (Index = 0; Index < 8; Index++) {
            UfsPramNoteByte(
                Adapter,
                Cursor,
                (UCHAR)Digits[(Values[Value] >> (28U - (Index * 4U))) & 0xFU]
                );
            Cursor++;
        }
    }
}

/*
 * Map and validate the breadcrumb window. Called once, from HwFindAdapter,
 * after the device resources are mapped. Failure is never fatal: the driver
 * simply loses the persistent channel and behaves exactly as V14 did.
 *
 * V17: this used StorPortGetDeviceBase, which validates the requested range
 * against the adapter's TRANSLATED RESOURCE LIST. The DSDT publishes only the
 * four UFS windows (HCI/UniPro/PMA/UFSP), so 0xFED14000 was refused every time
 * and V15/V16 both booted with PRAM_MAPPED=0 - i.e. with no channel except a
 * photograph of the screen.
 *
 * MmMapIoSpace performs no resource-list validation, and it is legal on this
 * address for the same reason the window exists at all: PlatformMemoryMapLib
 * places it ABOVE the end of EXYNOS_DRAM (0xBC800000) and describes it as a
 * Device region, so Windows never has these pages in its PFN database and is
 * not being asked to alias memory it owns. HwFindAdapter runs at PASSIVE_LEVEL,
 * which is MmMapIoSpace's requirement.
 */
static
VOID
UfsPramLatchInitialize(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    );

static
VOID
UfsPramLatchSetAttempt(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Attempt
    );

static
VOID
UfsPramLatchComplete(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    );

static
VOID
UfsPramFlushInitialize(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    );

static
VOID
UfsPramFlushBegin(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    );

static
VOID
UfsPramFlushComplete(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ BOOLEAN Success
    );

static
VOID
UfsLowWdtPramInitialize(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    PHYSICAL_ADDRESS Physical;

    Adapter->LowWdtPram = NULL;
    Physical.QuadPart = (LONGLONG)UFS_LOW_WDT_PRAM_PHYSICAL_BASE;
    Adapter->LowWdtPram = (PUCHAR)MmMapIoSpace(
        Physical,
        UFS_LOW_WDT_PRAM_WINDOW,
        MmNonCached
        );
}

static
BOOLEAN
UfsRwd1Acquire(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    ULONG Spin;

    for (Spin = 0; Spin < 1024UL; Spin++) {
        if (InterlockedCompareExchange(&Adapter->Rwd1Lock, 1, 0) == 0) {
            return TRUE;
        }
        KeMemoryBarrier();
    }
    return FALSE;
}

static
VOID
UfsRwd1Release(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    KeMemoryBarrier();
    InterlockedExchange(&Adapter->Rwd1Lock, 0);
}

static
VOID
UfsRwd1Snapshot(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _Out_writes_(RWD1_RECORD_DWORDS) PULONG Shadow
    )
{
    volatile ULONG *Record;
    ULONG Index;

    Record = (volatile ULONG *)(
        Adapter->LowWdtPram + UFS_RWD1_RECORD_OFFSET
        );
    for (Index = 0; Index < RWD1_RECORD_DWORDS; Index++) {
        Shadow[Index] = Record[Index];
    }
}

static
ULONG
UfsRwd1Checksum(
    _In_reads_(RWD1_RECORD_DWORDS) const ULONG *Shadow
    )
{
    ULONG Acc;
    ULONG Index;

    Acc = RWD1_CHECKSUM_SEED;
    for (Index = 0; Index < RWD1_W_CHECKSUM; Index++) {
        Acc ^= Shadow[Index];
    }
    return Acc;
}

static
BOOLEAN
UfsRwd1StructurallyValid(
    _In_reads_(RWD1_RECORD_DWORDS) const ULONG *Shadow
    )
{
    return (BOOLEAN)(
        (Shadow[RWD1_W_MAGIC] == RWD1_MAGIC) &&
        (Shadow[RWD1_W_VERSION_LENGTH] == RWD1_VERSION_LENGTH) &&
        ((Shadow[RWD1_W_GENERATION] ^
          Shadow[RWD1_W_GENERATION_INV]) == MAXULONG) &&
        ((Shadow[RWD1_W_HEARTBEAT] ^
          Shadow[RWD1_W_HEARTBEAT_INV]) == MAXULONG) &&
        ((Shadow[RWD1_W_CHECKSUM] ^
          Shadow[RWD1_W_CHECKSUM_INV]) == MAXULONG) &&
        (Shadow[RWD1_W_COMMIT] == RWD1_COMMIT_MAGIC) &&
        (Shadow[RWD1_W_COMMIT_INV] == ~RWD1_COMMIT_MAGIC) &&
        (Shadow[RWD1_W_CHECKSUM] == UfsRwd1Checksum(Shadow))
        );
}

static
BOOLEAN
UfsRwd1StableRead(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _Out_writes_(RWD1_RECORD_DWORDS) PULONG Shadow
    )
{
    ULONG First[RWD1_RECORD_DWORDS];
    ULONG Index;

    if (Adapter->LowWdtPram == NULL) {
        return FALSE;
    }

    UfsRwd1Snapshot(Adapter, First);
    KeMemoryBarrier();
    UfsRwd1Snapshot(Adapter, Shadow);
    KeMemoryBarrier();
    for (Index = 0; Index < RWD1_RECORD_DWORDS; Index++) {
        if (First[Index] != Shadow[Index]) {
            return FALSE;
        }
    }
    return UfsRwd1StructurallyValid(Shadow);
}

static
VOID
UfsRwd1CommitLocked(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _Inout_updates_(RWD1_RECORD_DWORDS) PULONG Shadow
    )
{
    volatile ULONG *Record;
    ULONG Index;

    Shadow[RWD1_W_MAGIC] = RWD1_MAGIC;
    Shadow[RWD1_W_VERSION_LENGTH] = RWD1_VERSION_LENGTH;
    Shadow[RWD1_W_GENERATION_INV] = ~Shadow[RWD1_W_GENERATION];
    Shadow[RWD1_W_HEARTBEAT_INV] = ~Shadow[RWD1_W_HEARTBEAT];
    Shadow[RWD1_W_CHECKSUM] = 0UL;
    Shadow[RWD1_W_CHECKSUM_INV] = 0UL;
    Shadow[RWD1_W_COMMIT] = RWD1_COMMIT_MAGIC;
    Shadow[RWD1_W_COMMIT_INV] = ~RWD1_COMMIT_MAGIC;
    Shadow[RWD1_W_CHECKSUM] = UfsRwd1Checksum(Shadow);
    Shadow[RWD1_W_CHECKSUM_INV] = ~Shadow[RWD1_W_CHECKSUM];

    Record = (volatile ULONG *)(
        Adapter->LowWdtPram + UFS_RWD1_RECORD_OFFSET
        );
    Record[RWD1_W_COMMIT] = 0UL;
    KeMemoryBarrier();
    for (Index = 0; Index <= RWD1_W_CHECKSUM_INV; Index++) {
        Record[Index] = Shadow[Index];
    }
    KeMemoryBarrier();
    Record[RWD1_W_COMMIT_INV] = Shadow[RWD1_W_COMMIT_INV];
    KeMemoryBarrier();
    Record[RWD1_W_COMMIT] = Shadow[RWD1_W_COMMIT];
    KeMemoryBarrier();
}

static
BOOLEAN
UfsRwd1ReadbackMatches(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_reads_(RWD1_RECORD_DWORDS) const ULONG *Expected
    )
{
    ULONG Fresh[RWD1_RECORD_DWORDS];
    ULONG Index;

    if (!UfsRwd1StableRead(Adapter, Fresh)) {
        return FALSE;
    }
    for (Index = 0; Index < RWD1_RECORD_DWORDS; Index++) {
        if (Fresh[Index] != Expected[Index]) {
            return FALSE;
        }
    }
    return TRUE;
}

static
BOOLEAN
UfsRwd1TransitionAllowed(
    _In_ ULONG From,
    _In_ ULONG To
    )
{
    if (To == RWD1_STATE_WINDOWS_OWNED) {
        return (BOOLEAN)(
            (From == RWD1_STATE_SEC_ACTIVE) ||
            (From == RWD1_STATE_DXE_ACTIVE) ||
            (From == RWD1_STATE_BDS_ACTIVE) ||
            (From == RWD1_STATE_EBS_ARMED) ||
            (From == RWD1_STATE_P3_POST_HANDOFF) ||
            (From == RWD1_STATE_WINDOWS_OWNED)
            );
    }
    if (To == RWD1_STATE_SHUTDOWN_PENDING) {
        return (BOOLEAN)(From == RWD1_STATE_WINDOWS_OWNED);
    }
    if (To == RWD1_STATE_CONTROLLED_STOP) {
        return (BOOLEAN)(From == RWD1_STATE_SHUTDOWN_PENDING);
    }
    if (To == RWD1_STATE_FATAL_NO_PET) {
        return (BOOLEAN)(
            (From == RWD1_STATE_WINDOWS_OWNED) ||
            (From == RWD1_STATE_SHUTDOWN_PENDING) ||
            (From == RWD1_STATE_CONTROLLED_STOP) ||
            (From == RWD1_STATE_FATAL_NO_PET)
            );
    }
    if (To == RWD1_STATE_RECOVERY_PENDING) {
        return (BOOLEAN)(
            (From == RWD1_STATE_WINDOWS_OWNED) ||
            (From == RWD1_STATE_SHUTDOWN_PENDING) ||
            (From == RWD1_STATE_FATAL_NO_PET) ||
            (From == RWD1_STATE_RECOVERY_PENDING)
            );
    }
    return FALSE;
}

static
BOOLEAN
UfsRwd1Transition(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG State,
    _In_ ULONG Phase,
    _In_ ULONG Reason,
    _In_ ULONG Detail,
    _In_ BOOLEAN IncrementHeartbeat
    )
{
    ULONG Shadow[RWD1_RECORD_DWORDS];
    BOOLEAN Success;

    if ((Adapter == NULL) || !UfsRwd1Acquire(Adapter)) {
        return FALSE;
    }

    Success = FALSE;
    if (UfsRwd1StableRead(Adapter, Shadow) &&
        (!Adapter->Rwd1Owned ||
         (Shadow[RWD1_W_OWNER] == RWD1_OWNER_WINDOWS_UFS)) &&
        UfsRwd1TransitionAllowed(Shadow[RWD1_W_STATE], State)) {
        Shadow[RWD1_W_STATE] = State;
        Shadow[RWD1_W_OWNER] = RWD1_OWNER_WINDOWS_UFS;
        Shadow[RWD1_W_PHASE] = Phase;
        Shadow[RWD1_W_REASON] = Reason;
        Shadow[RWD1_W_DETAIL] = Detail;
        if (Adapter->Pmu != NULL) {
            Shadow[RWD1_W_RESET_STATUS] =
                *(volatile ULONG *)(Adapter->Pmu + UFS_PMU_RST_STAT);
        }
        if (IncrementHeartbeat) {
            Shadow[RWD1_W_HEARTBEAT]++;
        }
        UfsRwd1CommitLocked(Adapter, Shadow);
        Success = UfsRwd1ReadbackMatches(Adapter, Shadow);
    }

    UfsRwd1Release(Adapter);
    return Success;
}

static
BOOLEAN
UfsRwd1TakeOwnership(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    BOOLEAN Success;

    Adapter->Rwd1Owned = FALSE;
    Adapter->Rwd1Lock = 0;
    Adapter->NoPetReason = UFS_NO_PET_NONE;
    Adapter->ShutdownPending = FALSE;
    Adapter->WatchdogStopComplete = FALSE;

    Success = UfsRwd1Transition(
        Adapter,
        RWD1_STATE_WINDOWS_OWNED,
        RWD1_PHASE_WINDOWS_INITIALIZING,
        RWD1_REASON_NONE,
        0UL,
        FALSE
        );
    Adapter->Rwd1Owned = Success;
    return Success;
}

static
VOID
UfsRwd1Heartbeat(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    ULONG Shadow[RWD1_RECORD_DWORDS];
    BOOLEAN Success = FALSE;

    if (!Adapter->Rwd1Owned ||
        (Adapter->NoPetReason != UFS_NO_PET_NONE)) {
        return;
    }

    if (UfsRwd1Acquire(Adapter)) {
        if ((Adapter->NoPetReason == UFS_NO_PET_NONE) &&
            Adapter->Rwd1Owned &&
            UfsRwd1StableRead(Adapter, Shadow) &&
            (Shadow[RWD1_W_OWNER] == RWD1_OWNER_WINDOWS_UFS)) {
            if ((Shadow[RWD1_W_STATE] == RWD1_STATE_WINDOWS_OWNED) &&
                (Shadow[RWD1_W_REASON] == RWD1_REASON_NONE) &&
                ((Shadow[RWD1_W_PHASE] == RWD1_PHASE_WINDOWS_INITIALIZING) ||
                 (Shadow[RWD1_W_PHASE] == RWD1_PHASE_WINDOWS_RUNNING))) {
                Shadow[RWD1_W_PHASE] = RWD1_PHASE_WINDOWS_RUNNING;
                Shadow[RWD1_W_DETAIL] = Adapter->WdtPets;
                Success = TRUE;
            } else if (
                (Shadow[RWD1_W_STATE] == RWD1_STATE_SHUTDOWN_PENDING) &&
                (Shadow[RWD1_W_PHASE] == RWD1_PHASE_WINDOWS_SHUTDOWN) &&
                (Shadow[RWD1_W_REASON] == RWD1_REASON_CONTROLLED_SHUTDOWN)) {
                /* Servicing a shutdown must not undo its ownership boundary. */
                Success = TRUE;
            }
            if (Success) {
                Shadow[RWD1_W_HEARTBEAT]++;
                if (Adapter->Pmu != NULL) {
                    Shadow[RWD1_W_RESET_STATUS] =
                        *(volatile ULONG *)(Adapter->Pmu + UFS_PMU_RST_STAT);
                }
                UfsRwd1CommitLocked(Adapter, Shadow);
                Success = UfsRwd1ReadbackMatches(Adapter, Shadow);
            }
        }
        UfsRwd1Release(Adapter);
    }
    if (!Success) {
        (VOID)InterlockedCompareExchange(
            &Adapter->NoPetReason,
            UFS_NO_PET_WATCHDOG_INVALID,
            UFS_NO_PET_NONE
            );
        KeMemoryBarrier();
    }
}

/*
 * Only ScsiRestartAdapter may undo this adapter's completed controlled stop.
 * Validate before hardware work, but publish ownership and clear the sentinel
 * only after that work succeeds. This is not a fresh firmware-ownership epoch.
 */
static
BOOLEAN
UfsRwd1Restart(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ BOOLEAN Complete
    )
{
    ULONG Shadow[RWD1_RECORD_DWORDS];
    BOOLEAN Success = FALSE;

    if (!UfsRwd1Acquire(Adapter)) {
        return FALSE;
    }
    if (!Adapter->FatalError && Adapter->Rwd1Owned &&
        UfsRwd1StableRead(Adapter, Shadow) &&
        (Shadow[RWD1_W_OWNER] == RWD1_OWNER_WINDOWS_UFS)) {
        if ((Adapter->NoPetReason == UFS_NO_PET_NONE) &&
            !Adapter->ShutdownPending && !Adapter->WatchdogStopComplete &&
            (Shadow[RWD1_W_STATE] == RWD1_STATE_WINDOWS_OWNED) &&
            (Shadow[RWD1_W_PHASE] == RWD1_PHASE_WINDOWS_RUNNING) &&
            (Shadow[RWD1_W_REASON] == RWD1_REASON_NONE)) {
            Success = TRUE;
        } else if (
            (Adapter->NoPetReason == UFS_NO_PET_SHUTDOWN_FINAL) &&
            Adapter->WatchdogStopComplete && Adapter->ShutdownPending &&
            !Adapter->UnattendedArmed && !Adapter->WdtArmed &&
            (Shadow[RWD1_W_STATE] == RWD1_STATE_CONTROLLED_STOP) &&
            (Shadow[RWD1_W_PHASE] == RWD1_PHASE_WINDOWS_SHUTDOWN) &&
            (Shadow[RWD1_W_REASON] == RWD1_REASON_CONTROLLED_SHUTDOWN)) {
            Success = TRUE;
            if (Complete) {
                Success = Adapter->Started;
                if (Success) {
                    Shadow[RWD1_W_STATE] = RWD1_STATE_WINDOWS_OWNED;
                    Shadow[RWD1_W_PHASE] = RWD1_PHASE_WINDOWS_RUNNING;
                    Shadow[RWD1_W_REASON] = RWD1_REASON_NONE;
                    Shadow[RWD1_W_DETAIL] = 0UL;
                    UfsRwd1CommitLocked(Adapter, Shadow);
                    Success = UfsRwd1ReadbackMatches(Adapter, Shadow);
                    if (Success) {
                        /*
                         * A fatal callback that could not acquire Rwd1Lock
                         * can still latch NoPetReason. Never erase that latch.
                         */
                        Success = (BOOLEAN)(InterlockedCompareExchange(
                            &Adapter->NoPetReason,
                            UFS_NO_PET_NONE,
                            UFS_NO_PET_SHUTDOWN_FINAL
                            ) == UFS_NO_PET_SHUTDOWN_FINAL);
                        if (Success) {
                            Adapter->ShutdownPending = FALSE;
                            Adapter->WatchdogStopComplete = FALSE;
                        }
                    }
                }
            }
        }
    }
    UfsRwd1Release(Adapter);
    return Success;
}

static
VOID
UfsRwd1SetFatalNoPet(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG NoPetReason,
    _In_ ULONG RecordReason,
    _In_ ULONG Phase,
    _In_ ULONG Detail
    )
{
    if (Adapter == NULL ||
        ((Adapter->NoPetReason != UFS_NO_PET_NONE) &&
         !((Adapter->NoPetReason == UFS_NO_PET_SHUTDOWN_FINAL) &&
           Adapter->WatchdogStopComplete))) {
        return;
    }

    if (Adapter->Rwd1Owned) {
        (VOID)UfsRwd1Transition(
            Adapter,
            RWD1_STATE_FATAL_NO_PET,
            Phase,
            RecordReason,
            Detail,
            FALSE
            );
    }
    KeMemoryBarrier();
    Adapter->WatchdogStopComplete = FALSE;
    Adapter->NoPetReason = NoPetReason;
    KeMemoryBarrier();
}

static
VOID
UfsRwd1PrepareRecovery(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG NoPetReason,
    _In_ ULONG RecordReason,
    _In_ ULONG Phase,
    _In_ ULONG Detail
    )
{
    if (Adapter == NULL) {
        return;
    }

    if (Adapter->Rwd1Owned) {
        (VOID)UfsRwd1Transition(
            Adapter,
            RWD1_STATE_RECOVERY_PENDING,
            Phase,
            RecordReason,
            Detail,
            FALSE
            );
    }
    KeMemoryBarrier();
    Adapter->NoPetReason = NoPetReason;
    KeMemoryBarrier();
}

static
ULONG
UfsSmpChecksum(
    _In_reads_(UFS_SMP_RECORD_DWORDS) const ULONG *Shadow
    )
{
    ULONG Acc;
    ULONG Index;

    Acc = UFS_SMP_MAGIC_VALID;
    for (Index = 1; Index < UFS_SMP_RECORD_DWORDS; Index++) {
        if (Index != UFS_SMP_W_CHECKSUM) {
            Acc ^= Shadow[Index];
        }
    }
    return Acc;
}

static
BOOLEAN
UfsP3AcknowledgePostHandoff(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Record;
    ULONG First[UFS_SMP_RECORD_DWORDS];
    ULONG Shadow[UFS_SMP_RECORD_DWORDS];
    ULONG Index;
    ULONG State;

    if (Adapter->LowWdtPram == NULL) {
        return FALSE;
    }

    Record = (volatile ULONG *)(
        Adapter->LowWdtPram + UFS_SMP_RECORD_OFFSET
        );
    for (Index = 0; Index < UFS_SMP_RECORD_DWORDS; Index++) {
        First[Index] = Record[Index];
    }
    KeMemoryBarrier();
    for (Index = 0; Index < UFS_SMP_RECORD_DWORDS; Index++) {
        Shadow[Index] = Record[Index];
        if (Shadow[Index] != First[Index]) {
            return FALSE;
        }
    }
    KeMemoryBarrier();

    if (Shadow[UFS_SMP_W_MAGIC] == 0UL) {
        for (Index = 1; Index < UFS_SMP_RECORD_DWORDS; Index++) {
            if (Shadow[Index] != 0UL) {
                return FALSE;
            }
        }
        return TRUE;
    }
    if ((Shadow[UFS_SMP_W_MAGIC] != UFS_SMP_MAGIC_VALID) ||
        (Shadow[UFS_SMP_W_VERSION] != UFS_SMP_VERSION_LENGTH) ||
        (Shadow[UFS_SMP_W_GENERATION] != UFS_SMP_G19_GENERATION) ||
        (Shadow[UFS_SMP_W_CHECKSUM] != UfsSmpChecksum(Shadow))) {
        return FALSE;
    }

    State = Shadow[UFS_SMP_W_STATEWORD] & 0xFFUL;
    if (State == UFS_SMP_STATE_WINDOWS_ACK) {
        return TRUE;
    }
    if (State != UFS_SMP_STATE_PREBRANCH) {
        return FALSE;
    }

    Shadow[UFS_SMP_W_STATEWORD] &=
        ~(0xFFUL | UFS_SMP_FLAG_ONE_SHOT_ARMED);
    Shadow[UFS_SMP_W_STATEWORD] |= UFS_SMP_STATE_WINDOWS_ACK;
    Shadow[UFS_SMP_W_SEQUENCE]++;
    Shadow[UFS_SMP_W_CHECKSUM] = 0UL;
    Shadow[UFS_SMP_W_CHECKSUM] = UfsSmpChecksum(Shadow);

    Record[UFS_SMP_W_MAGIC] = UFS_SMP_MAGIC_COMMIT;
    KeMemoryBarrier();
    for (Index = 1; Index < UFS_SMP_RECORD_DWORDS; Index++) {
        Record[Index] =
            (Index == UFS_SMP_W_CHECKSUM) ? 0UL : Shadow[Index];
    }
    Record[UFS_SMP_W_CHECKSUM] = Shadow[UFS_SMP_W_CHECKSUM];
    KeMemoryBarrier();
    Record[UFS_SMP_W_MAGIC] = UFS_SMP_MAGIC_VALID;
    KeMemoryBarrier();

    for (Index = 0; Index < UFS_SMP_RECORD_DWORDS; Index++) {
        if (Record[Index] != Shadow[Index]) {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * Acknowledge the watchdog epoch BdsDxe handed to Windows.
 *
 * State is cleared FIRST so that any reset occurring after this store cannot be
 * mistaken by the next firmware boot for a failed firmware/winload epoch. Count
 * is then reset, because arriving here proves the boot that firmware was
 * counting actually succeeded. Status is written last and is purely
 * diagnostic. RWD1 owns live acknowledgement; this compatibility record is
 * written only for a final controlled stop (WST1).
 *
 * Ordering matters more than atomicity: there is no lock shared with firmware,
 * but firmware only ever inspects state and count, and clearing state before
 * count means the worst case is an already-acknowledged epoch being
 * acknowledged twice.
 */
static
VOID
UfsPramAcknowledgeLowLevelWatchdog(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Status
    )
{
    volatile ULONG *State;

    if (Adapter->LowWdtPram == NULL) {
        return;
    }

    State = (volatile ULONG *)(
        Adapter->LowWdtPram + UFS_LOW_WDT_RECORD_OFFSET
        );
    State[0] = 0UL;
    KeMemoryBarrier();
    State[1] = 0UL;
    State[2] = Status;
    KeMemoryBarrier();
}

/*
 * Publish Windows as the current hardware-watchdog owner.
 *
 * Status is committed before ARM1 so a firmware boot can never observe an
 * armed record still carrying the previous owner.
 */
static
VOID
UfsPramArmLowLevelWatchdog(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *State;

    if (Adapter->LowWdtPram == NULL) {
        return;
    }

    State = (volatile ULONG *)(
        Adapter->LowWdtPram + UFS_LOW_WDT_RECORD_OFFSET
        );
    State[2] = UFS_LOW_WDT_WINDOWS_ARMED;
    KeMemoryBarrier();
    State[0] = UFS_LOW_WDT_ARM_MAGIC;
    KeMemoryBarrier();
}

static
VOID
UfsPramInitialize(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo
    )
{
    PHYSICAL_ADDRESS PramPhysical;
    volatile ULONG *Header;

    UNREFERENCED_PARAMETER(ConfigInfo);

    Adapter->Pram = NULL;
    Adapter->PramRecords = 0;
    Adapter->Diagnostic.PramMapped = 0;

    PramPhysical.QuadPart = (LONGLONG)UFS_PRAM_PHYSICAL_BASE;
    Adapter->Pram = (PUCHAR)MmMapIoSpace(
        PramPhysical,
        UFS_PRAM_WINDOW,
        MmNonCached
        );
    if (Adapter->Pram == NULL) {
        return;
    }

    //
    // Prove the window is really writable before trusting it. The firmware
    // leaves the magic behind, but a fresh region or a bad mapping would not
    // read back what we just stored.
    //
    Header = (volatile ULONG *)Adapter->Pram;
    if (Header[0] != UFS_PRAM_MAGIC) {
        Header[0] = UFS_PRAM_MAGIC;
        Header[1] = 0;
        Header[2] = 0;
    }
    if (Header[0] != UFS_PRAM_MAGIC) {
        MmUnmapIoSpace((PVOID)Adapter->Pram, UFS_PRAM_WINDOW);
        Adapter->Pram = NULL;
        return;
    }

    //
    // Take the whole ring. UEFI resets the cursor every boot and then spends
    // ~13.8 KB of the 16 KB on its own breadcrumbs, which leaves under 2.4 KB -
    // about five driver records - before the ring caps and the channel goes
    // silent. Resetting here self-selects correctly: if Windows got far enough
    // to load this driver, the UEFI trace has already served its purpose; if it
    // did not, this code never ran and those breadcrumbs survive untouched.
    //
    Header[2] = 0;
    if (Header[2] != 0) {
        MmUnmapIoSpace((PVOID)Adapter->Pram, UFS_PRAM_WINDOW);
        Adapter->Pram = NULL;
        return;
    }

    Adapter->Diagnostic.PramMapped = 1;
    Adapter->Diagnostic.PramVirtual = (ULONGLONG)(ULONG_PTR)Adapter->Pram;

    //
    // Claim the note slots FIRST, while the cursor is still zero. This has to
    // happen here rather than at the point a note is emitted, because notes are
    // written at the very end of the boot when the ring is already full and an
    // appended record would be discarded by UfsPramByte's capacity check.
    //
    UfsPramReserveNotes(Adapter);

    UfsPramLatchInitialize(Adapter);
    UfsPramFlushInitialize(Adapter);
}

/*
 * Acknowledge to the firmware that this boot got far enough to be worth keeping.
 *
 * UEFI increments a boot-attempt counter on every Windows-path boot and diverts the phone
 * to TWRP once it reaches its limit, so a bring-up cycle that bugchecks before the miniport
 * ever runs cannot loop forever with no way back. Clearing the counter here is what makes a
 * healthy boot cost nothing; a boot that never reaches this point leaves it standing.
 *
 * Only clear the count, never the magic: the magic is the firmware's proof that the word is
 * really its own rather than uninitialized DRAM, and clearing it would make the firmware
 * restart from scratch and lose an attempt.
 */
static
VOID
UfsPramAcknowledgeBoot(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Magic;
    volatile ULONG *Count;

    if (Adapter->Pram == NULL) {
        return;
    }

    Magic = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_BOOTATTEMPT_MAGIC_OFFSET);
    Count = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_BOOTATTEMPT_COUNT_OFFSET);

    if (*Magic != UFS_PRAM_BOOTATTEMPT_MAGIC) {
        //
        // Firmware without the watchdog, or a cold region. Nothing to acknowledge, and
        // claiming the word here would be this driver asserting a contract it does not own.
        //
        Adapter->Diagnostic.BootAttemptsAcknowledged = 0;
        return;
    }

    Adapter->Diagnostic.BootAttemptsAcknowledged = *Count;
    *Count = 0;
}

/*
 * Decode the warm-reset crash latch, then immediately rewrite it as "complete".
 *
 * The rewrite is what makes the lockout self-limiting. Nothing in the firmware
 * ever clears these words, so if the latch were left as-is a single crash would
 * lock writes out forever. Rewriting here means a crash costs exactly one boot:
 * the next boot refuses writes, runs to completion, reboots itself into TWRP
 * with the evidence, and the boot after that is clean again.
 *
 * The alternative - leaving the lockout latched until a human clears it - trades
 * one stalled unattended cycle for an indefinite one, which is the failure this
 * whole mechanism exists to prevent.
 */
static
VOID
UfsPramLatchInitialize(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Latch;
    volatile ULONG *Guard;
    volatile ULONG *Phase;
    ULONG Previous;
    ULONG PreviousPhase;
    ULONG Boots;

    Adapter->Diagnostic.WriteCrashLockout = 0;
    Adapter->Diagnostic.WriteCrashAttempt = 0;
    Adapter->Diagnostic.WriteBootEpoch = 0;
    Adapter->Diagnostic.WriteProbeCompleted = 0;
    Adapter->Diagnostic.WriteCrashPhase = UFS_PHASE_IDLE;

    //
    // Force the first vendor-diag read of the boot to emit a record rather than
    // compare against a zeroed LastPramDiagnostic that never came from a real
    // snapshot.
    //
    Adapter->LastPramDiagnosticValid = FALSE;
    Adapter->PramDuplicatesSuppressed = 0;

    if (Adapter->Pram == NULL) {
        return;
    }

    Latch = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_LATCH_OFFSET);
    Guard = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_LATCH_GUARD_OFFSET);
    Phase = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_PHASE_OFFSET);

    //
    // Capture where the previous boot was before resetting the word, so a crash
    // reports its own location exactly once.
    //
    PreviousPhase = *Phase;
    if (((PreviousPhase >> UFS_PRAM_PHASE_TAG_SHIFT) & UFS_PRAM_PHASE_MASK) ==
        UFS_PRAM_PHASE_TAG) {
        Adapter->Diagnostic.WriteCrashPhase = PreviousPhase & UFS_PRAM_PHASE_MASK;
    }

    //
    // Without the guard, uninitialized DRAM has a 1-in-256 chance of matching
    // the tag byte and inventing a lockout on a device that never crashed.
    //
    Previous = 0;
    Boots = 0;
    if (*Guard == UFS_PRAM_LATCH_GUARD) {
        Previous = *Latch;
        if (((Previous >> UFS_PRAM_LATCH_TAG_SHIFT) & UFS_PRAM_LATCH_BYTE_MASK) ==
            UFS_PRAM_LATCH_TAG) {
            Boots = (Previous >> UFS_PRAM_LATCH_BOOTS_SHIFT) &
                    UFS_PRAM_LATCH_BYTE_MASK;

            if (((Previous & UFS_PRAM_LATCH_COMPLETED) == 0) &&
                (Adapter->Diagnostic.WriteCrashPhase !=
                 UFS_PHASE_LIVE_COMPLETE)) {
                //
                // The previous boot entered the write path and never reached the
                // closing disarm or a clean full-Windows write completion. It
                // died mid-probe.
                //
                Adapter->Diagnostic.WriteCrashLockout = 1;
                Adapter->Diagnostic.WriteCrashAttempt =
                    (Previous >> UFS_PRAM_LATCH_ATTEMPT_SHIFT) &
                    UFS_PRAM_LATCH_BYTE_MASK;
                Adapter->Diagnostic.FailureMask |=
                    UFS_DIAG_FAILURE_WRITE_CRASH_LOCKOUT;
            }
        }
    }

    if (Boots < UFS_PRAM_LATCH_BYTE_MASK) {
        Boots++;
    }

    Adapter->Diagnostic.WriteBootEpoch = Boots;
    Adapter->Diagnostic.WriteProbeCompleted = 1;

    *Latch = (UFS_PRAM_LATCH_TAG << UFS_PRAM_LATCH_TAG_SHIFT) |
             (Boots << UFS_PRAM_LATCH_BOOTS_SHIFT) |
             UFS_PRAM_LATCH_COMPLETED;
    *Guard = UFS_PRAM_LATCH_GUARD;

    //
    // Re-arm the breadcrumb for this boot. The previous value has already been
    // captured above, so clearing it here is what keeps a phase reading scoped
    // to exactly one boot, the same way the latch self-heals.
    //
    *Phase = (UFS_PRAM_PHASE_TAG << UFS_PRAM_PHASE_TAG_SHIFT) | UFS_PHASE_IDLE;
}

/*
 * Record where in the write path execution currently is.
 *
 * One naturally aligned 32-bit store to a dead PRAM word. It cannot fail, cannot
 * fault on an unaligned access, and cannot perturb the ring the telemetry uses,
 * so it is safe to call from anywhere in the write path including the moments
 * immediately before a suspected bugcheck.
 */
static
VOID
UfsPramSetPhase(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Phase
    )
{
    volatile ULONG *Word;

    if (Adapter->Pram == NULL) {
        return;
    }

    Word = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_PHASE_OFFSET);
    *Word = (UFS_PRAM_PHASE_TAG << UFS_PRAM_PHASE_TAG_SHIFT) |
            (Phase & UFS_PRAM_PHASE_MASK);
}

/*
 * Mark a write attempt in flight. Anything that happens after this point and
 * before UfsPramLatchComplete is what the next boot will blame.
 */
static
VOID
UfsPramLatchSetAttempt(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Attempt
    )
{
    volatile ULONG *Latch;

    if (Adapter->Pram == NULL) {
        return;
    }

    Adapter->Diagnostic.WriteProbeCompleted = 0;

    Latch = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_LATCH_OFFSET);
    *Latch = (UFS_PRAM_LATCH_TAG << UFS_PRAM_LATCH_TAG_SHIFT) |
             ((Adapter->Diagnostic.WriteBootEpoch & UFS_PRAM_LATCH_BYTE_MASK) <<
              UFS_PRAM_LATCH_BOOTS_SHIFT) |
             ((Attempt & UFS_PRAM_LATCH_BYTE_MASK) <<
              UFS_PRAM_LATCH_ATTEMPT_SHIFT);
}

/*
 * Close the window opened by UfsPramLatchSetAttempt.
 *
 * Two call sites, deliberately: the DISARM arm-CDB (the loader's last step) and
 * the vendor reboot CDB. Relying on DISARM alone was a single point of failure -
 * a boot that stalled before it, or was rescued by the watchdog, left the latch
 * armed and locked out every subsequent boot with no way to clear it.
 */
static
VOID
UfsPramLatchComplete(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Latch;

    if (Adapter->Pram == NULL) {
        return;
    }

    Adapter->Diagnostic.WriteProbeCompleted = 1;

    Latch = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_LATCH_OFFSET);
    *Latch = *Latch | UFS_PRAM_LATCH_COMPLETED;
}

static
VOID
UfsPramFlushInitialize(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Word;
    volatile ULONG *Guard;

    if (Adapter->Pram == NULL) {
        return;
    }

    Word = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_FLUSH_OFFSET);
    Guard = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_FLUSH_GUARD_OFFSET);
    *Word = (UFS_PRAM_FLUSH_TAG << UFS_PRAM_FLUSH_TAG_SHIFT) |
            ((Adapter->Diagnostic.WriteBootEpoch & UFS_PRAM_FLUSH_BYTE_MASK) <<
             UFS_PRAM_FLUSH_BOOT_SHIFT);
    *Guard = UFS_PRAM_FLUSH_GUARD;
    KeMemoryBarrier();
}

static
VOID
UfsPramFlushBegin(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Word;
    volatile ULONG *Guard;
    ULONG Current;
    ULONG Attempts;
    ULONG Flags;

    if (Adapter->Pram == NULL) {
        return;
    }

    Word = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_FLUSH_OFFSET);
    Guard = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_FLUSH_GUARD_OFFSET);
    Current = *Word;
    Attempts = 0;
    Flags = 0;

    if ((*Guard == UFS_PRAM_FLUSH_GUARD) &&
        (((Current >> UFS_PRAM_FLUSH_TAG_SHIFT) &
          UFS_PRAM_FLUSH_BYTE_MASK) == UFS_PRAM_FLUSH_TAG) &&
        (((Current >> UFS_PRAM_FLUSH_BOOT_SHIFT) &
          UFS_PRAM_FLUSH_BYTE_MASK) ==
         (Adapter->Diagnostic.WriteBootEpoch & UFS_PRAM_FLUSH_BYTE_MASK))) {
        Attempts = (Current >> UFS_PRAM_FLUSH_ATTEMPT_SHIFT) &
                   UFS_PRAM_FLUSH_BYTE_MASK;
        Flags = Current & UFS_PRAM_FLUSH_EVER_COMPLETED;
    }

    if (Attempts < UFS_PRAM_FLUSH_BYTE_MASK) {
        Attempts++;
    }

    *Word = (UFS_PRAM_FLUSH_TAG << UFS_PRAM_FLUSH_TAG_SHIFT) |
            ((Adapter->Diagnostic.WriteBootEpoch & UFS_PRAM_FLUSH_BYTE_MASK) <<
             UFS_PRAM_FLUSH_BOOT_SHIFT) |
            (Attempts << UFS_PRAM_FLUSH_ATTEMPT_SHIFT) |
            Flags |
            UFS_PRAM_FLUSH_IN_FLIGHT;
    *Guard = UFS_PRAM_FLUSH_GUARD;
    KeMemoryBarrier();
}

static
VOID
UfsPramFlushComplete(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ BOOLEAN Success
    )
{
    volatile ULONG *Word;
    volatile ULONG *Guard;
    ULONG Current;
    ULONG Flags;

    if (Adapter->Pram == NULL) {
        return;
    }

    Word = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_FLUSH_OFFSET);
    Guard = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_FLUSH_GUARD_OFFSET);
    Current = *Word;

    if ((*Guard != UFS_PRAM_FLUSH_GUARD) ||
        (((Current >> UFS_PRAM_FLUSH_TAG_SHIFT) &
          UFS_PRAM_FLUSH_BYTE_MASK) != UFS_PRAM_FLUSH_TAG) ||
        (((Current >> UFS_PRAM_FLUSH_BOOT_SHIFT) &
          UFS_PRAM_FLUSH_BYTE_MASK) !=
         (Adapter->Diagnostic.WriteBootEpoch & UFS_PRAM_FLUSH_BYTE_MASK))) {
        return;
    }

    Flags = Current & UFS_PRAM_FLUSH_EVER_COMPLETED;
    if (Success) {
        Flags |= UFS_PRAM_FLUSH_EVER_COMPLETED |
                 UFS_PRAM_FLUSH_LAST_COMPLETED;
    } else {
        Flags |= UFS_PRAM_FLUSH_LAST_FAILED;
    }

    *Word = (Current & 0xFFFFFF00UL) | Flags;
    KeMemoryBarrier();
}

/*
 * Tell the firmware that this Windows handoff did NOT hang.
 *
 * UEFI stamps STAR2LTE_WIN_ATTEMPT_MAGIC into 0xFED17FB0 just before
 * ExitBootServices and, on the next entry, treats a marker that is still set as
 * proof the previous attempt wedged - diverting to TWRP so the device recovers
 * itself with no button presses. Without a clearer that is indiscriminate: a
 * perfectly healthy Windows session would also divert the following boot, which
 * is exactly why STAR2LTE_AUTO_RECOVERY_ON_HANG was left disabled.
 *
 * Reaching the first Storport timer tick is the strongest "Windows is healthy"
 * signal available to us - it proves the kernel is alive, scheduling, and
 * running this driver - so that is where the marker is cleared. A boot that
 * wedges at ExitBootServices never gets here, leaves the marker set, and is
 * correctly diverted.
 *
 * One naturally aligned 32-bit store to a word the ring never reaches. It
 * cannot fail, cannot fault on an unaligned access, and cannot perturb
 * telemetry. Deliberately unconditional: re-clearing an already-clear marker is
 * free, and skipping the read avoids caring what uninitialized DRAM contains.
 */
static
VOID
UfsPramClearHandoffMarker(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Marker;

    if (Adapter->Pram == NULL) {
        return;
    }

    Marker = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_HANDOFF_MARKER_OFFSET);
    *Marker = 0UL;
}

/*
 * Total physical memory as the Windows memory manager itself accounts for it.
 *
 * Captured once, in DriverEntry, for one reason: MmGetPhysicalMemoryRanges must
 * be called at IRQL <= APC_LEVEL and allocates from paged pool, and every later
 * point where the witness is written (HwInitialize, the Storport timer tick) is
 * DISPATCH_LEVEL. DriverEntry is the only PASSIVE_LEVEL moment this driver has.
 *
 * That ordering is fine for memory specifically, because unlike the processor
 * count the physical memory map does not grow after boot - it is whatever
 * winload handed the kernel, which is exactly the number the memory-map work is
 * trying to move.
 *
 * Deliberately uses the documented enumeration rather than the MmNumberOfPhysicalPages
 * global: the global is not a contractual export on every kernel, and a driver
 * that fails to link is a far worse outcome than four extra lines here.
 *
 * A NULL return means the enumeration failed; the caller leaves the MEMOK flag
 * clear and the reader reports "unknown" instead of "zero", which are very
 * different claims.
 */
static ULONG64 UfsWitnessPhysicalPages = 0;
static ULONG   UfsWitnessPhysicalRanges = 0;
static BOOLEAN UfsWitnessMemoryValid = FALSE;

static
VOID
UfsCaptureSystemMemory(
    VOID
    )
{
    PPHYSICAL_MEMORY_RANGE Ranges;
    ULONG64 TotalBytes;
    ULONG Count;

    Ranges = MmGetPhysicalMemoryRanges();
    if (Ranges == NULL) {
        return;
    }

    TotalBytes = 0;
    Count = 0;

    //
    // The array is terminated by an all-zero entry: both BaseAddress and
    // NumberOfBytes zero. A range with a zero length but a nonzero base is not
    // the terminator, so both halves are tested rather than just the length.
    //
    while ((Ranges[Count].BaseAddress.QuadPart != 0) ||
           (Ranges[Count].NumberOfBytes.QuadPart != 0)) {
        TotalBytes += (ULONG64)Ranges[Count].NumberOfBytes.QuadPart;
        Count++;
    }

    ExFreePool(Ranges);

    UfsWitnessPhysicalPages = TotalBytes / 4096ULL;
    UfsWitnessPhysicalRanges = Count;
    UfsWitnessMemoryValid = TRUE;
}

/*
 * Publish what Windows sees into PRAM.
 *
 * Processor counts are re-read on every call rather than cached, because a
 * boot-start miniport is loaded while the kernel is still bringing secondary
 * processors online: the count visible at HwInitialize is a lower bound, and
 * only a later tick sees the final topology. Taking the maximum across calls
 * would hide a core that came up and then went away, so the newest reading
 * simply wins and the tick number records when it was taken.
 *
 * ALL_PROCESSOR_GROUPS is used deliberately. The per-group form would silently
 * report only group 0, which is indistinguishable from "the other cores never
 * started" on a machine that is expected to grow past a single group.
 *
 * Written magic-last so a reader can never latch a half-built record.
 */
static
VOID
UfsWitnessRefresh(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Flag
    )
{
    volatile ULONG *Record;
    ULONG Checksum;
    ULONG Index;

    if (Adapter->Pram == NULL) {
        return;
    }

    Adapter->WitnessActiveProcessors =
        KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    Adapter->WitnessMaximumProcessors =
        KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
    Adapter->WitnessActiveGroups = (ULONG)KeQueryActiveGroupCount();
    Adapter->WitnessFlags |= Flag;
    if (UfsWitnessMemoryValid) {
        Adapter->WitnessFlags |= UFS_PRAM_WITNESS_FLAG_MEMOK;
    }

    Record = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_WITNESS_OFFSET);

    Record[0] = 0;
    KeMemoryBarrier();
    Record[1] = Adapter->WitnessActiveProcessors;
    Record[2] = Adapter->WitnessMaximumProcessors;
    Record[3] = Adapter->WitnessActiveGroups;
    Record[4] = (ULONG)(UfsWitnessPhysicalPages & 0xFFFFFFFFULL);
    Record[5] = (ULONG)(UfsWitnessPhysicalPages >> 32);
    Record[6] = UfsWitnessPhysicalRanges;
    Record[7] = Adapter->WitnessFlags;
    Record[8] = Adapter->UnattendedTicks;

    Checksum = UFS_PRAM_WITNESS_MAGIC ^ UFS_PRAM_WITNESS_GUARD;
    for (Index = 1; Index < (UFS_PRAM_WITNESS_DWORDS - 1UL); Index++) {
        Checksum ^= Record[Index];
    }
    Record[9] = Checksum;

    KeMemoryBarrier();
    Record[0] = UFS_PRAM_WITNESS_MAGIC;
    KeMemoryBarrier();
}

/*
 * Consume the TWRP-written unattended-return request, if there is one.
 *
 * Returns the requested timeout in seconds, or 0 for "no valid request", which
 * is also what every failure mode returns. Fail-closed is the only acceptable
 * direction here: a false positive resets a machine somebody may be using, a
 * false negative merely means a measurement cycle needs a human once.
 *
 * Three independent things must agree before the request is honoured - the
 * magic, a timeout inside a sane range, and a checksum binding the two - so
 * uninitialized DRAM cannot invent one. The bounds check is not cosmetic: a
 * garbage timeout of 0 would reset the phone on the first tick, and one of
 * 4 billion would never fire at all.
 *
 * The token is acknowledged by overwriting its magic before the caller acts on
 * it. That makes arming strictly one-shot, so neither a hang later in this boot
 * nor a TWRP script that forgets to clean up can produce a reboot loop.
 */
static
ULONG
UfsUnattendConsumeToken(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Arm;
    ULONG Magic;
    ULONG Seconds;
    ULONG Checksum;

    if (Adapter->Pram == NULL) {
        return 0;
    }

    Arm = (volatile ULONG *)(Adapter->Pram + UFS_PRAM_UNATTEND_ARM_OFFSET);

    Magic = Arm[0];
    Seconds = Arm[1];
    Checksum = Arm[2];

    if (Magic != UFS_PRAM_UNATTEND_ARM_MAGIC) {
        return 0;
    }
    if ((Seconds < UFS_PRAM_UNATTEND_SEC_MIN) ||
        (Seconds > UFS_PRAM_UNATTEND_SEC_MAX)) {
        return 0;
    }
    if (Checksum != (UFS_PRAM_UNATTEND_ARM_MAGIC ^ Seconds ^
                     UFS_PRAM_UNATTEND_GUARD)) {
        return 0;
    }

    //
    // Acknowledge before acting, not after: if anything below wedges, the next
    // boot must come up unarmed.
    //
    Arm[0] = 0;
    Arm[3] = UFS_PRAM_UNATTEND_ACK_MAGIC;
    KeMemoryBarrier();

    return Seconds;
}

/*
 * Map the PMU for the explicit recovery-reset command and the bugcheck-only
 * reset/shutdown fallback.
 *
 * Mapping is unconditional and inert. Register writes remain gated behind
 * those two deliberate paths, so a mapped PMU on its own cannot reset anything.
 */
static
VOID
UfsPmuInitialize(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    PHYSICAL_ADDRESS PmuPhysical;

    Adapter->Pmu = NULL;
    Adapter->Diagnostic.PmuMapped = 0;

    PmuPhysical.QuadPart = (LONGLONG)UFS_PMU_PHYSICAL_BASE;
    Adapter->Pmu = (PUCHAR)MmMapIoSpace(
        PmuPhysical,
        UFS_PMU_WINDOW,
        MmNonCached
        );
    if (Adapter->Pmu == NULL) {
        return;
    }

    Adapter->Diagnostic.PmuMapped = 1;
}

#if UFS_AUTOMATIC_RECOVERY_RESET || UFS_FIRMWARE_WDT_BRIDGE || UFS_CONTINUOUS_HARDWARE_WATCHDOG
/*
 * Map the cluster-0 watchdog window.
 *
 * Mapping alone is inert: nothing here writes, and the block is left in whatever
 * state sboot/UEFI handed over. Arming is a separate, deliberate step that only
 * happens once the servicing timer has proven it fires (see UfsUnattendedTimer),
 * so a boot where the timer is broken can never leave an unpetted watchdog
 * running.
 */
static
VOID
UfsWdtInitialize(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    PHYSICAL_ADDRESS WdtPhysical;

    Adapter->Wdt = NULL;
    Adapter->WdtArmed = FALSE;
    Adapter->WdtPets = 0;
    Adapter->WdtLastCount = 0;
    Adapter->WdtTicksPerSecond = 0;

    WdtPhysical.QuadPart = (LONGLONG)UFS_WDT_PHYSICAL_BASE;
    Adapter->Wdt = (PUCHAR)MmMapIoSpace(
        WdtPhysical,
        UFS_WDT_WINDOW,
        MmNonCached
        );
}

#if UFS_AUTOMATIC_RECOVERY_RESET || UFS_CONTINUOUS_HARDWARE_WATCHDOG
/*
 * Arm a fresh cluster-0 hardware watchdog.
 *
 * The inherited BdsDxe v9 bridge is preferred because it covers the
 * ExitBootServices-to-HwInitialize interval. A fresh arm is the fail-safe for a
 * baseline firmware that handed Windows an inactive or unrecognised counter.
 */
static
BOOLEAN
UfsWdtArm(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Disable;
    volatile ULONG *MaskReset;
    volatile ULONG *Wtcon;
    volatile ULONG *Wtdat;
    volatile ULONG *Wtcnt;

    if (Adapter->Wdt == NULL ||
        Adapter->Pmu == NULL ||
        Adapter->LowWdtPram == NULL ||
        !Adapter->Rwd1Owned ||
        (Adapter->NoPetReason != UFS_NO_PET_NONE)) {
        return FALSE;
    }
    if (Adapter->WdtArmed) {
        return TRUE;
    }

    Disable = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_WDT_DISABLE);
    MaskReset = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_WDT_MASK_RESET);
    Wtcon = (volatile ULONG *)(Adapter->Wdt + UFS_WDT_WTCON);
    Wtdat = (volatile ULONG *)(Adapter->Wdt + UFS_WDT_WTDAT);
    Wtcnt = (volatile ULONG *)(Adapter->Wdt + UFS_WDT_WTCNT);

    *Wtcon = 0UL;
    KeMemoryBarrier();

    *Disable &= ~UFS_WDT_CLUSTER0_RESET_BIT;
    *MaskReset &= ~UFS_WDT_CLUSTER0_RESET_BIT;
    KeMemoryBarrier();
    if (((*Disable | *MaskReset) & UFS_WDT_CLUSTER0_RESET_BIT) != 0UL) {
        return FALSE;
    }

    *Wtdat = UFS_WDT_COUNT;
    *Wtcnt = UFS_WDT_COUNT;
    *Wtcon = UFS_WDT_CON_ARM;
    KeMemoryBarrier();

    if ((*Wtdat != UFS_WDT_COUNT) || (*Wtcon != UFS_WDT_CON_ARM)) {
        *Wtcon = 0UL;
        KeMemoryBarrier();
        return FALSE;
    }

    Adapter->WdtLastCount = UFS_WDT_COUNT;
    Adapter->WdtArmed = TRUE;
    Adapter->Diagnostic.WdtArmed = 1UL;
    Adapter->Diagnostic.WdtDisarmedTooFast = 0UL;
    return TRUE;
}

/*
 * Stop the watchdog even if software lost its WdtArmed bookkeeping. Controlled
 * shutdown must leave hardware inert before publishing WST1.
 */
static
BOOLEAN
UfsWdtDisarm(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Wtcon;

    if (Adapter->Wdt == NULL) {
        return FALSE;
    }

    Wtcon = (volatile ULONG *)(Adapter->Wdt + UFS_WDT_WTCON);
    *Wtcon = 0UL;
    KeMemoryBarrier();

    Adapter->WdtArmed = FALSE;
    Adapter->Diagnostic.WdtArmed = 0UL;
    return (BOOLEAN)(*Wtcon == 0UL);
}

/*
 * Reload the watchdog and measure the live clock. A surprising short period is
 * treated as a configuration fault: disarm, publish WST1, and do not rearm.
 */
static
VOID
UfsWdtPet(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Wtcnt;
    ULONG Current;

    if (Adapter->Wdt == NULL ||
        !Adapter->WdtArmed ||
        (Adapter->NoPetReason != UFS_NO_PET_NONE)) {
        return;
    }

    Wtcnt = (volatile ULONG *)(Adapter->Wdt + UFS_WDT_WTCNT);
    Current = *Wtcnt;
    if (Current < UFS_WDT_COUNT) {
        Adapter->WdtTicksPerSecond = UFS_WDT_COUNT - Current;
    }
    Adapter->WdtLastCount = Current;
    Adapter->Diagnostic.WdtLastCount = Current;

    if (Adapter->WdtTicksPerSecond != 0UL &&
        (UFS_WDT_COUNT / Adapter->WdtTicksPerSecond) <
            UFS_WDT_MIN_TIMEOUT_SEC) {
        (VOID)UfsWdtDisarm(Adapter);
        Adapter->Diagnostic.WdtDisarmedTooFast = 1UL;
        UfsRwd1SetFatalNoPet(
            Adapter,
            UFS_NO_PET_WATCHDOG_INVALID,
            RWD1_REASON_WINDOWS_NO_PET,
            RWD1_PHASE_WINDOWS_RUNNING,
            Adapter->WdtTicksPerSecond
            );
        UfsPramAcknowledgeLowLevelWatchdog(
            Adapter,
            UFS_LOW_WDT_WINDOWS_STOP
            );
        return;
    }

    *Wtcnt = UFS_WDT_COUNT;
    KeMemoryBarrier();
    Adapter->WdtPets++;
    Adapter->Diagnostic.WdtPets = Adapter->WdtPets;
    Adapter->Diagnostic.WdtTicksPerSecond = Adapter->WdtTicksPerSecond;
    UfsRwd1Heartbeat(Adapter);
}
#endif

#if UFS_FIRMWARE_WDT_BRIDGE
/*
 * Acknowledge the completed firmware epoch, then take ownership of its live
 * watchdog before controller initialisation can stall.
 *
 * G17's binary-patched BdsDxe v8/v9 bridge uses 0xFF39. The M3 probe lineage
 * uses 0x5C39. Both are accepted explicitly; arbitrary armed control words are
 * rejected rather than silently inherited.
 */
static
VOID
UfsWdtTakeFirmwareBridgeOwnership(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Disable;
    volatile ULONG *MaskReset;
    volatile ULONG *Wtcon;
    volatile ULONG *Wtdat;
    volatile ULONG *Wtcnt;
    ULONG InheritedCon;
    BOOLEAN ValidControl;

    if (Adapter->Wdt == NULL || !Adapter->Rwd1Owned) {
        return;
    }
    if (Adapter->LowWdtPram == NULL) {
        (VOID)UfsWdtDisarm(Adapter);
        return;
    }

    Wtcon = (volatile ULONG *)(Adapter->Wdt + UFS_WDT_WTCON);
    Wtdat = (volatile ULONG *)(Adapter->Wdt + UFS_WDT_WTDAT);
    Wtcnt = (volatile ULONG *)(Adapter->Wdt + UFS_WDT_WTCNT);
    InheritedCon = *Wtcon;
    ValidControl =
        (InheritedCon == UFS_WDT_CON_ARM) ||
        (InheritedCon == UFS_WDT_CON_FIRMWARE_ARM);

    if (ValidControl && (*Wtdat == UFS_WDT_COUNT)) {
        Adapter->WdtLastCount = *Wtcnt;
        Adapter->Diagnostic.WdtLastCount = Adapter->WdtLastCount;

        if (Adapter->Pmu != NULL) {
            Disable =
                (volatile ULONG *)(Adapter->Pmu + UFS_PMU_WDT_DISABLE);
            MaskReset =
                (volatile ULONG *)(Adapter->Pmu + UFS_PMU_WDT_MASK_RESET);
            *Disable &= ~UFS_WDT_CLUSTER0_RESET_BIT;
            *MaskReset &= ~UFS_WDT_CLUSTER0_RESET_BIT;
            KeMemoryBarrier();
            if (((*Disable | *MaskReset) &
                 UFS_WDT_CLUSTER0_RESET_BIT) != 0UL) {
                *Wtcon = 0UL;
                KeMemoryBarrier();
                Adapter->WdtArmed = FALSE;
                Adapter->Diagnostic.WdtArmed = 0UL;
                return;
            }
        }

        *Wtcnt = UFS_WDT_COUNT;
        KeMemoryBarrier();
        Adapter->WdtArmed = TRUE;
        Adapter->Diagnostic.WdtArmed = 1UL;
    } else {
        *Wtcon = 0UL;
        KeMemoryBarrier();
#if UFS_CONTINUOUS_HARDWARE_WATCHDOG
        (VOID)UfsWdtArm(Adapter);
#endif
    }

    if (Adapter->WdtArmed) {
        UfsPramArmLowLevelWatchdog(Adapter);
    }
}

/*
 * First shutdown boundary. Keep servicing the watchdog while storage and
 * Storport are still tearing down; only ownership changes here.
 */
static
VOID
UfsRuntimeWatchdogBeginShutdown(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    if (Adapter == NULL) {
        return;
    }

    if (!Adapter->ShutdownPending && Adapter->Rwd1Owned) {
        Adapter->ShutdownPending = UfsRwd1Transition(
            Adapter,
            RWD1_STATE_SHUTDOWN_PENDING,
            RWD1_PHASE_WINDOWS_SHUTDOWN,
            RWD1_REASON_CONTROLLED_SHUTDOWN,
            0UL,
            FALSE
            );
    }
}

/*
 * Final shutdown boundary. P3 acknowledgement and WTCON=0 must both read back
 * before CONTROLLED_STOP or the compatibility WST1 mirror is published.
 */
static
BOOLEAN
UfsRuntimeWatchdogFinalStop(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    BOOLEAN P3Acknowledged;
    BOOLEAN WatchdogStopped;
    BOOLEAN Rwd1Stopped;

    if (Adapter == NULL ||
        (Adapter->NoPetReason != UFS_NO_PET_NONE)) {
        return FALSE;
    }

    UfsRuntimeWatchdogBeginShutdown(Adapter);
    P3Acknowledged = UfsP3AcknowledgePostHandoff(Adapter);
    if (!Adapter->ShutdownPending || !P3Acknowledged) {
        UfsRwd1SetFatalNoPet(
            Adapter,
            UFS_NO_PET_SHUTDOWN_FINAL,
            RWD1_REASON_WINDOWS_NO_PET,
            RWD1_PHASE_WINDOWS_SHUTDOWN,
            (!Adapter->ShutdownPending ? 1UL : 0UL) |
            (!P3Acknowledged ? 2UL : 0UL)
            );
        return FALSE;
    }

#if UFS_AUTOMATIC_RECOVERY_RESET || UFS_CONTINUOUS_HARDWARE_WATCHDOG
    WatchdogStopped = UfsWdtDisarm(Adapter);
#else
    WatchdogStopped = TRUE;
#endif
    if (!WatchdogStopped) {
        UfsRwd1SetFatalNoPet(
            Adapter,
            UFS_NO_PET_SHUTDOWN_FINAL,
            RWD1_REASON_WINDOWS_NO_PET,
            RWD1_PHASE_WINDOWS_SHUTDOWN,
            4UL
            );
        return FALSE;
    }

    Rwd1Stopped = Adapter->ShutdownPending &&
                  UfsRwd1Transition(
                    Adapter,
                    RWD1_STATE_CONTROLLED_STOP,
                    RWD1_PHASE_WINDOWS_SHUTDOWN,
                    RWD1_REASON_CONTROLLED_SHUTDOWN,
                    0UL,
                    FALSE
                    );

    if (!Rwd1Stopped) {
#if UFS_AUTOMATIC_RECOVERY_RESET || UFS_CONTINUOUS_HARDWARE_WATCHDOG
        (VOID)UfsWdtArm(Adapter);
#endif
        UfsRwd1SetFatalNoPet(
            Adapter,
            UFS_NO_PET_SHUTDOWN_FINAL,
            RWD1_REASON_WINDOWS_NO_PET,
            RWD1_PHASE_WINDOWS_SHUTDOWN,
            8UL
            );
        return FALSE;
    }

    Adapter->UnattendedArmed = FALSE;
    Adapter->WatchdogStopComplete = TRUE;
    KeMemoryBarrier();
    if (InterlockedCompareExchange(
            &Adapter->NoPetReason,
            UFS_NO_PET_SHUTDOWN_FINAL,
            UFS_NO_PET_NONE
            ) != UFS_NO_PET_NONE) {
        Adapter->WatchdogStopComplete = FALSE;
        return FALSE;
    }
    UfsPramAcknowledgeLowLevelWatchdog(
        Adapter,
        UFS_LOW_WDT_WINDOWS_STOP
        );
    return TRUE;
}
#endif
#endif

static
VOID
UfsPramSnapshot(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_z_ const char *Tag
    );

/*
 * Put the SoC into recovery. Does not return when it works.
 *
 * PlatformBootManagerLib.c records this exact pair as the one device-verified
 * route to TWRP: sboot reads the Samsung SEC reset reason from PMU INFORM3 and
 * boots RECOVERY for 0x12345674, and PMU SWRESET = 1 is the SoC software reset.
 * A watchdog-origin reset does NOT honour the reason and just re-enters UEFI,
 * which is why the watchdog is not used here.
 *
 * The reason word is written and read back before the reset is triggered. If
 * the readback disagrees the reset is abandoned, because resetting with a stale
 * reason would boot straight back into Windows and strand the phone in exactly
 * the state this exists to avoid.
 */
static
VOID
UfsPmuRebootToRecovery(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    volatile ULONG *Inform2;
    volatile ULONG *Inform3;
    volatile ULONG *Sysip;
    volatile ULONG *SoftReset;

    if (Adapter->Pmu == NULL) {
        return;
    }

    Adapter->RebootRequests++;
    Adapter->Diagnostic.RebootRequests = Adapter->RebootRequests;

    //
    // Flush the breadcrumb channel first: after the reset this record is the
    // only evidence the boot ever happened.
    //
    UfsPramSnapshot(Adapter, "UFSREBOOT");
    UfsRwd1PrepareRecovery(
        Adapter,
        UFS_NO_PET_RECOVERY_REQUEST,
        RWD1_REASON_WINDOWS_REQUESTED_RECOVERY,
        RWD1_PHASE_RECOVERY_ROUTE,
        Adapter->RebootRequests
        );

    Inform2 = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_INFORM2);
    Inform3 = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_INFORM3);
    Sysip = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_SYSIP_DAT0);
    SoftReset = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_SWRESET);

    //
    // INFORM3 is the device-proven one-shot sboot selector. Clear the two
    // legacy auxiliary fields first: sboot consumes INFORM3 on the recovery
    // landing but leaves INFORM2/SYSIP_DAT0 intact, which made later TWRP
    // power-off and cold starts re-enter recovery indefinitely.
    //
    *Inform2 = 0UL;
    *Sysip = 0UL;
    *Inform3 = UFS_PMU_REBOOT_REASON_RECOVERY;
    KeMemoryBarrier();

    if ((*Inform2 != 0UL) ||
        (*Inform3 != UFS_PMU_REBOOT_REASON_RECOVERY) ||
        (*Sysip != 0UL)) {
        return;
    }

    *SoftReset = 1UL;
    KeMemoryBarrier();
}

/*
 * Establish Samsung's normal software-reset contract.
 *
 * S-Boot consumes all three words after a warm reset. A raw SWRESET with stale
 * recovery state either enters TWRP or stalls at the Samsung logo. The values
 * below are the exact sequence used by the device-verified pmu_boot.ko path.
 */
static
VOID
UfsPramWriteNormalBootRecord(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Phase,
    _In_ ULONG BeforeInform2,
    _In_ ULONG BeforeInform3,
    _In_ ULONG BeforeSysip,
    _In_ ULONG AfterInform2,
    _In_ ULONG AfterInform3,
    _In_ ULONG AfterSysip,
    _In_ BOOLEAN Success
    )
{
    volatile ULONG *Record;
    ULONG Status;
    ULONG Checksum;

    if (Adapter->Pram == NULL) {
        return;
    }

    Status = (Phase << 16) | (Success ? 1UL : 0UL);
    Checksum = UFS_PRAM_NORMAL_RECORD_MAGIC ^
               Status ^
               BeforeInform2 ^
               BeforeInform3 ^
               BeforeSysip ^
               AfterInform2 ^
               AfterInform3 ^
               AfterSysip ^
               UFS_PRAM_NORMAL_RECORD_GUARD;

    Record = (volatile ULONG *)(
        Adapter->Pram + UFS_PRAM_NORMAL_RECORD_OFFSET
        );
    Record[0] = 0;
    Record[1] = Status;
    Record[2] = BeforeInform2;
    Record[3] = BeforeInform3;
    Record[4] = BeforeSysip;
    Record[5] = AfterInform2;
    Record[6] = AfterInform3;
    Record[7] = AfterSysip;
    Record[8] = Checksum;
    Record[9] = UFS_PRAM_NORMAL_RECORD_GUARD;
    KeMemoryBarrier();
    Record[0] = UFS_PRAM_NORMAL_RECORD_MAGIC;
    KeMemoryBarrier();
}

static
BOOLEAN
UfsPmuSetNormalBootReason(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_z_ const char *Tag,
    _In_ ULONG Phase
    )
{
    volatile ULONG *Inform2;
    volatile ULONG *Inform3;
    volatile ULONG *Sysip;
    ULONG BeforeInform2;
    ULONG BeforeInform3;
    ULONG BeforeSysip;
    ULONG AfterInform2;
    ULONG AfterInform3;
    ULONG AfterSysip;
    ULONG ExpectedInform2;
    ULONG ExpectedInform3;
    BOOLEAN Result;

    if (Adapter->Pmu == NULL) {
        UfsPramWriteNormalBootRecord(
            Adapter,
            Phase,
            0,
            0,
            0,
            0,
            0,
            0,
            FALSE
            );
        UfsPramString(Adapter, "\n=");
        UfsPramString(Adapter, Tag);
        UfsPramField(Adapter, "PMU", 0);
        UfsPramByte(Adapter, (UCHAR)'\n');
        return FALSE;
    }

    Inform2 = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_INFORM2);
    Inform3 = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_INFORM3);
    Sysip = (volatile ULONG *)(Adapter->Pmu + UFS_PMU_SYSIP_DAT0);
    BeforeInform2 = *Inform2;
    BeforeInform3 = *Inform3;
    BeforeSysip = *Sysip;

    ExpectedInform3 =
        (Phase == UFS_PRAM_NORMAL_PHASE_STOP &&
         Adapter->CleanRecoveryState == UFS_CLEAN_RECOVERY_COMMITTED &&
         Adapter->ShutdownPending) ?
        UFS_PMU_REBOOT_REASON_RECOVERY : UFS_PMU_REBOOT_REASON_NORMAL;
    ExpectedInform2 = (ExpectedInform3 == UFS_PMU_REBOOT_REASON_RECOVERY) ?
        0UL : UFS_PMU_SEC_POWER_RESET;
    *Inform2 = ExpectedInform2;
    *Sysip = 0UL;
    KeMemoryBarrier();
    *Inform3 = ExpectedInform3;
    KeMemoryBarrier();

    AfterInform2 = *Inform2;
    AfterInform3 = *Inform3;
    AfterSysip = *Sysip;
    Result = (BOOLEAN)(
        AfterInform2 == ExpectedInform2 &&
        AfterInform3 == ExpectedInform3 &&
        AfterSysip == 0UL
        );

    UfsPramWriteNormalBootRecord(
        Adapter,
        Phase,
        BeforeInform2,
        BeforeInform3,
        BeforeSysip,
        AfterInform2,
        AfterInform3,
        AfterSysip,
        Result
        );

    UfsPramString(Adapter, "\n=");
    UfsPramString(Adapter, Tag);
    UfsPramField(Adapter, "B2", BeforeInform2);
    UfsPramField(Adapter, "B3", BeforeInform3);
    UfsPramField(Adapter, "BS", BeforeSysip);
    UfsPramField(Adapter, "A2", AfterInform2);
    UfsPramField(Adapter, "A3", AfterInform3);
    UfsPramField(Adapter, "AS", AfterSysip);
    UfsPramField(Adapter, "OK", (ULONG)Result);
    UfsPramByte(Adapter, (UCHAR)'\n');
    KeMemoryBarrier();

    return Result;
}

static
VOID
UfsCleanRecoveryClear(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    Adapter->CleanRecoveryState = UFS_CLEAN_RECOVERY_IDLE;
    Adapter->CleanRecoveryToken = 0;
    Adapter->CleanRecoveryDeadline = 0;
}

static
VOID
UfsCleanRecoveryExpire(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONGLONG Now
    )
{
    if (Adapter->CleanRecoveryState == UFS_CLEAN_RECOVERY_ARMED &&
        Now >= Adapter->CleanRecoveryDeadline) {
        UfsCleanRecoveryClear(Adapter);
    }
}

static
BOOLEAN
UfsCleanRecoveryReady(
    _In_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    return (BOOLEAN)(
        Adapter->Pmu != NULL && Adapter->Started &&
        !Adapter->FatalError && !Adapter->DiagnosticOnly &&
        !Adapter->ShutdownPending && !Adapter->WatchdogStopComplete &&
        Adapter->NoPetReason == UFS_NO_PET_NONE &&
        !Adapter->UnattendReturnRequested && Adapter->UnattendReturnSec == 0 &&
        !UFS_BOOT_DIAG_MODE && !UFS_AUTOMATIC_RECOVERY_RESET
        );
}

static
VOID
UfsCleanRecoveryBeginShutdown(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    /* Only SRB_FUNCTION_SHUTDOWN consumes a live ticket, never a power stop. */
    UfsCleanRecoveryExpire(Adapter, KeQueryInterruptTime());
    if (Adapter->CleanRecoveryState == UFS_CLEAN_RECOVERY_ARMED) {
        if (UfsCleanRecoveryReady(Adapter)) {
            Adapter->CleanRecoveryState = UFS_CLEAN_RECOVERY_COMMITTED;
        } else {
            UfsCleanRecoveryClear(Adapter);
        }
    }
}

static
UCHAR
UfsCleanRecoveryControl(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _Inout_ PSCSI_REQUEST_BLOCK Srb
    )
{
    UFS_CLEAN_RECOVERY_REPLY Reply = {0};
    ULONGLONG Now;
    ULONG Action;
    ULONG Token;

    if (Srb->CdbLength != 16 ||
        Srb->Cdb[0] != UFS_CLEAN_RECOVERY_OPCODE ||
        (((ULONG)Srb->Cdb[1] << 8) | Srb->Cdb[2]) != UFS_CLEAN_RECOVERY_SUBCODE ||
        Srb->Cdb[3] != (UCHAR)(UFS_CLEAN_RECOVERY_CONFIRM >> 24) ||
        Srb->Cdb[4] != (UCHAR)((UFS_CLEAN_RECOVERY_CONFIRM >> 16) & 0xFFU) ||
        Srb->Cdb[5] != (UCHAR)((UFS_CLEAN_RECOVERY_CONFIRM >> 8) & 0xFFU) ||
        Srb->Cdb[6] != (UCHAR)(UFS_CLEAN_RECOVERY_CONFIRM & 0xFFU) ||
        Srb->Cdb[7] > UFS_CLEAN_RECOVERY_CANCEL ||
        Srb->Cdb[12] != 0 || Srb->Cdb[13] != 0 ||
        Srb->Cdb[14] != 0 || Srb->Cdb[15] != 0 ||
        Srb->PathId != 0 || Srb->TargetId != 0 || Srb->Lun != 0 ||
        (Srb->SrbFlags & (SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT)) != SRB_FLAGS_DATA_IN ||
        Srb->DataBuffer == NULL || Srb->DataTransferLength != sizeof(Reply)) {
        Srb->DataTransferLength = 0;
        return SRB_STATUS_INVALID_REQUEST;
    }
    Action = Srb->Cdb[7];
    Token = ((ULONG)Srb->Cdb[8] << 24) | ((ULONG)Srb->Cdb[9] << 16) |
        ((ULONG)Srb->Cdb[10] << 8) | Srb->Cdb[11];
    if ((Action == UFS_CLEAN_RECOVERY_QUERY) != (Token == 0)) {
        Srb->DataTransferLength = 0;
        return SRB_STATUS_INVALID_REQUEST;
    }

    Now = KeQueryInterruptTime();
    UfsCleanRecoveryExpire(Adapter, Now);
    Reply.Signature = UFS_CLEAN_RECOVERY_SIGNATURE;
    Reply.Version = UFS_CLEAN_RECOVERY_VERSION;
    Reply.Size = sizeof(Reply);
    Reply.LifetimeSeconds = UFS_CLEAN_RECOVERY_SECONDS;
    Reply.Ready = UfsCleanRecoveryReady(Adapter);
    if (Action == UFS_CLEAN_RECOVERY_ARM) {
        if (!Reply.Ready || Now > MAXULONGLONG -
                UFS_CLEAN_RECOVERY_SECONDS * 10000000ULL) {
            Reply.Status = UFS_CLEAN_RECOVERY_NOT_READY;
        } else if (Adapter->CleanRecoveryState != UFS_CLEAN_RECOVERY_IDLE) {
            Reply.Status = UFS_CLEAN_RECOVERY_BUSY;
        } else {
            Adapter->CleanRecoveryToken = Token;
            Adapter->CleanRecoveryDeadline = Now +
                UFS_CLEAN_RECOVERY_SECONDS * 10000000ULL;
            Adapter->CleanRecoveryState = UFS_CLEAN_RECOVERY_ARMED;
        }
    } else if (Action == UFS_CLEAN_RECOVERY_CANCEL) {
        if (Adapter->CleanRecoveryState == UFS_CLEAN_RECOVERY_COMMITTED) {
            Reply.Status = UFS_CLEAN_RECOVERY_BUSY;
        } else if (Adapter->CleanRecoveryState == UFS_CLEAN_RECOVERY_ARMED &&
                   Adapter->CleanRecoveryToken != Token) {
            Reply.Status = UFS_CLEAN_RECOVERY_WRONG_TOKEN;
        } else {
            UfsCleanRecoveryClear(Adapter);
        }
    }
    Reply.State = Adapter->CleanRecoveryState;
    Reply.Token = Adapter->CleanRecoveryToken;
    RtlCopyMemory(Srb->DataBuffer, &Reply, sizeof(Reply));
    Srb->DataTransferLength = sizeof(Reply);
    Srb->ScsiStatus = SCSISTAT_GOOD;
    return SRB_STATUS_SUCCESS;
}

static
ULONGLONG
UfsPramPackRejectedOpcodes(
    _In_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    ULONGLONG Packed = 0;
    ULONG Index;

    for (Index = 0; Index < UFS_REJECTED_OPCODE_SLOTS; Index++) {
        Packed |= ((ULONGLONG)Adapter->Diagnostic.RejectedOpcodes[Index])
                  << (Index * 8);
    }

    return Packed;
}

static
ULONGLONG
UfsPramPackFailureHistogram(
    _In_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    ULONG Counts[8];
    ULONGLONG Packed = 0;
    ULONG Index;

    Counts[0] = Adapter->Diagnostic.FramingOcsFailures;
    Counts[1] = Adapter->Diagnostic.FramingTransactionFailures;
    Counts[2] = Adapter->Diagnostic.FramingLunFailures;
    Counts[3] = Adapter->Diagnostic.FramingTagFailures;
    Counts[4] = Adapter->Diagnostic.FramingResponseFailures;
    Counts[5] = Adapter->Diagnostic.FramingResidualFailures;
    Counts[6] = Adapter->Diagnostic.CheckConditionFailures;
    Counts[7] = Adapter->Diagnostic.ScsiStatusFailures;

    for (Index = 0; Index < RTL_NUMBER_OF(Counts); Index++) {
        ULONG Value = Counts[Index];

        if (Value > 0xFFUL) {
            Value = 0xFFUL;
        }

        Packed |= ((ULONGLONG)Value) << (Index * 8);
    }

    return Packed;
}

static
ULONGLONG
UfsPramPackFirstFailure(
    _In_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    ULONGLONG Packed;

    Packed  = ((ULONGLONG)(Adapter->Diagnostic.FirstFailureValid & 0xFFUL)) << 56;
    Packed |= ((ULONGLONG)(Adapter->Diagnostic.FirstFailureCause & 0xFFUL)) << 48;
    Packed |= ((ULONGLONG)(Adapter->Diagnostic.FirstFailureOpcode & 0xFFUL)) << 40;
    Packed |= ((ULONGLONG)(Adapter->Diagnostic.FirstFailureOcs & 0xFFUL)) << 32;
    Packed |= ((ULONGLONG)(Adapter->Diagnostic.FirstFailureStatus & 0xFFUL)) << 24;
    Packed |= ((ULONGLONG)(Adapter->Diagnostic.FirstFailureSenseKey & 0xFFUL)) << 16;
    Packed |= ((ULONGLONG)(Adapter->Diagnostic.FirstFailureAsc & 0xFFUL)) << 8;
    Packed |= ((ULONGLONG)(Adapter->Diagnostic.FirstFailureAscq & 0xFFUL));

    return Packed;
}

static
ULONG
UfsPramPackGptTranslation(
    _In_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    ULONG Translated = Adapter->GptHeadersTranslated;
    ULONG Rejected = Adapter->GptHeadersRejected;

    if (Translated > 0xFFFFUL) {
        Translated = 0xFFFFUL;
    }
    if (Rejected > 0xFFFFUL) {
        Rejected = 0xFFFFUL;
    }

    return (Translated << 16) | Rejected;
}

/*
 * Append the full adapter snapshot.
 *
 * Two call sites, chosen so that the record set alone separates the two live
 * hypotheses without any other channel working:
 *   =UFSINIT  at the end of HwInitialize - proves the miniport initialised.
 *   =UFSDIAG  from the vendor CDB handler - proves an SRB reached the miniport.
 * =UFSINIT present with no =UFSDIAG means Storport never dispatched a command,
 * which is exactly the case the on-screen channel cannot distinguish today.
 */
static
VOID
UfsPramSnapshot(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_z_ const char *Tag
    )
{
    if (Adapter->Pram == NULL) {
        return;
    }

    UfsPramString(Adapter, "\n=");
    UfsPramString(Adapter, Tag);
    UfsPramField(Adapter, "SEQ", Adapter->PramRecords);
    UfsPramField(Adapter, "STAGE", Adapter->Diagnostic.Stage);
    UfsPramField(Adapter, "FSTAGE", Adapter->Diagnostic.FailureStage);
    UfsPramString(Adapter, " FMASK=");
    UfsPramHex64(Adapter, Adapter->Diagnostic.FailureMask);
    UfsPramField(Adapter, "DIAGONLY", (ULONG)Adapter->DiagnosticOnly);
    UfsPramField(Adapter, "STARTED", (ULONG)Adapter->Started);
    UfsPramField(Adapter, "FATAL", (ULONG)Adapter->FatalError);
    UfsPramField(Adapter, "OWNSLIST", (ULONG)Adapter->OwnsTransferList);
    UfsPramField(Adapter, "HWINIT", Adapter->Diagnostic.HwInitializeCalls);
    UfsPramField(Adapter, "STARTIO", Adapter->Diagnostic.StartIoRequests);
    UfsPramField(Adapter, "EXECSCSI", Adapter->Diagnostic.ExecuteScsiRequests);
    UfsPramField(Adapter, "LASTSRBFN", Adapter->Diagnostic.LastSrbFunction);
    UfsPramField(Adapter, "EXECREJ", Adapter->Diagnostic.LastExecuteReject);
    UfsPramField(Adapter, "REARM", Adapter->RearmAttempts);
    UfsPramField(Adapter, "IOCTLREQ", Adapter->Diagnostic.IoControlRequests);
    UfsPramField(Adapter, "IOCTLREJ", Adapter->Diagnostic.IoControlRejects);
    UfsPramField(Adapter, "VENDOR", Adapter->Diagnostic.VendorDiagRequests);
    UfsPramField(Adapter, "DONE", Adapter->CompletedCommands);
    UfsPramField(Adapter, "FAILED", Adapter->FailedCommands);
    UfsPramField(Adapter, "REJECTED", Adapter->RejectedCommands);
    //
    // ROPN/ROPS name the opcodes the allowlist actually refused, in first-seen
    // order, so a boot can be read for WHICH command Windows wanted rather than
    // only how many were turned away. Packed into one 64-bit field instead of
    // eight separate ones because the ring caps at UFS_PRAM_CAPACITY and the
    // final record of every boot is already truncated mid-field.
    //
    // Emitted early in the record, next to REJECTED, so it survives that
    // truncation rather than being the first thing lost.
    //
    UfsPramField(Adapter, "ROPN", Adapter->Diagnostic.RejectedOpcodeCount);
    UfsPramString(Adapter, " ROPS=");
    UfsPramHex64(Adapter, UfsPramPackRejectedOpcodes(Adapter));
    //
    // RCTX carries the operands of the first refused non-write request, so a
    // refusal inside an ACCEPTED allowlist case can be attributed to its
    // sub-validator instead of inferred. Placed next to ROPS for the same
    // truncation reason.
    //
    UfsPramString(Adapter, " RCTX=");
    UfsPramHex64(Adapter, Adapter->Diagnostic.FirstRejectContext);
    //
    // MS10 counts MODE SENSE(6) commands rewritten onto the wire as MODE
    // SENSE(10). V20 measured this device answering opcode 0x1A with CHECK
    // CONDITION 05/24/00 - ILLEGAL REQUEST, INVALID FIELD IN CDB - which is
    // what a UFS logical unit does with a command outside its set. Linux
    // avoids the 6-byte form for every UFS LU via use_10_for_ms.
    //
    // Read it against FF's opcode field: a non-zero MS10 proves the
    // translation ran, and MODE_SENSE_6 disappearing from FF proves it removed
    // that failure population rather than merely relabelling it.
    //
    // Emitted next to ROPS/RCTX for the same truncation reason - the final
    // record of every boot is already cut short mid-field.
    //
    UfsPramField(Adapter, "MS10", Adapter->ModeSenseTranslated);
    //
    // FHIST/FOVR/FF expose the per-cause failure histogram that
    // UfsRecordCommandFailure has ALWAYS maintained but that nothing ever
    // emitted. Without them FAILED is a single number covering two populations
    // that need opposite fixes:
    //
    //   benign - a well-formed response carrying a non-GOOD SCSI status. That
    //            is correct SCSI behaviour; classpnp interprets the sense and
    //            carries on. Shows up as CHECK_CONDITION(7) / SCSI_STATUS(8).
    //
    //   fatal  - a well-formed response with GOOD status but a non-success OCS.
    //            That falls through to the OCS gate and returns a bare
    //            SRB_STATUS_ERROR, which surfaces to user mode as
    //            ERROR_IO_DEVICE 1117 - the code every partition-table IOCTL is
    //            currently failing with. Shows up as OCS(1).
    //
    // Which one dominates decides whether the OCS gate is the blocker or the
    // 1117 originates somewhere else entirely, and no existing field can tell
    // them apart.
    //
    // Packed into 64-bit fields for the same reason as ROPS: the ring caps at
    // UFS_PRAM_CAPACITY and nine separate named counters would cost ~126 bytes
    // in every UFSDIAG record. Assembled in stack locals so the read-out adds
    // no wide access to the packed diagnostic struct.
    //
    // FHIST byte N holds cause N+1, i.e. LSB = OCS(1), then TRANSACTION_CODE,
    // LUN, TASK_TAG, RESPONSE, RESIDUAL, CHECK_CONDITION, SCSI_STATUS(8), each
    // saturating at 0xFF. DATA_OVERRUN(9) does not fit in eight bytes and gets
    // its own plain field rather than a ragged bit width the decoder could
    // misread.
    //
    UfsPramString(Adapter, " FHIST=");
    UfsPramHex64(Adapter, UfsPramPackFailureHistogram(Adapter));
    UfsPramField(Adapter, "FOVR", Adapter->Diagnostic.DataOverrunFailures);
    //
    // FF is the first failure of the boot - the command that STARTED the
    // cascade, which is the one worth fixing. The counters above are a
    // whole-boot distribution and the two must not be read as describing the
    // same command.
    //
    // Layout, most significant byte first:
    //   [63:56] FirstFailureValid   [55:48] Cause     [47:40] Opcode
    //   [39:32] Ocs                 [31:24] Status    [23:16] SenseKey
    //   [15:8]  Asc                 [7:0]   Ascq
    //
    UfsPramString(Adapter, " FF=");
    UfsPramHex64(Adapter, UfsPramPackFirstFailure(Adapter));
    //
    // DW2 is the RAW descriptor word LASTOCS was masked out of. LASTOCS alone
    // cannot answer whether the 0xFF mask this driver uses is wider than the
    // [3:0] UFSHCI defines and Linux masks with, and that difference would turn
    // a successful transfer into a repudiated one.
    //
    UfsPramField(Adapter, "DW2", Adapter->LastTrdDw2);
    //
    // GPTX is the in-flight GPT header translation tally: headers rewritten in
    // bits [31:16], headers refused in bits [15:0], each clamped at 0xFFFF.
    //
    // Windows requires HeaderSize == 92 EXACTLY and this
    // disk declares 512, so both the primary and the backup header are rejected
    // with STATUS_DISK_CORRUPT_ERROR before their CRC is even checked - which is
    // why partmgr enumerates zero partitions on a disk whose GPT is byte-perfect
    // and spec-legal (UEFI 2.10 Table 5-5 permits 92..block-size).
    //
    // TRANSLATED > 0 means the fix fired. REJECTED > 0 means a block carried the
    // GPT signature and revision and would have needed a rewrite, but its stored
    // CRC32 did not validate, so it was left byte-untouched - investigate that,
    // never assume it is benign.
    //
    // The two counters share one field because cost scales with the count of the
    // record type: this is a UFSDIAG field and there are 12 of those per boot, so
    // one 4-char tag is 14 B/record = 168 B against a ring already over its
    // UFS_PRAM_CAPACITY cap. Two separate fields would have cost double.
    //
    // Emitted here, adjacent to DW2 and ahead of CONTAINED, because truncation of
    // the final record is guaranteed every run and fields at the tail are the ones
    // that disappear.
    //
    UfsPramField(Adapter, "GPTX", UfsPramPackGptTranslation(Adapter));
    UfsPramField(Adapter, "CONTAINED", Adapter->ContainedCommands);
    UfsPramField(Adapter, "LASTOP", Adapter->LastOpcode);
    UfsPramField(Adapter, "LASTOCS", Adapter->LastOcs);
    //
    // IS is the sampled UFSHCI Interrupt Status from the command that just ran.
    // It is the ONLY field that says which fatal condition contained a command,
    // and its absence is why the 2-block/4-block reads could be shown to fail
    // but not why: FMASK narrows it to UFS_INTERRUPT_STATUS_FATAL_MASK, which is
    // five different bits with five different meanings - UTP_ERROR (0x200) and
    // DEVICE_FATAL (0x10000) mean the device rejected the request, HOST_FATAL
    // (0x20000) and SYSTEM_BUS (0x40000) mean the host controller or the AXI
    // fabric could not carry it, and CRYPTO_ENGINE (0x80000) means the Exynos
    // FMP inline-crypto block rejected the descriptor. Those point at completely
    // different fixes, so guessing between them is not an option.
    //
    UfsPramField(Adapter, "IS", Adapter->Diagnostic.InterruptStatus);
    UfsPramField(Adapter, "ACKOCS", Adapter->AckFailureOcs);
    UfsPramField(Adapter, "REQLEN", Adapter->LastRequestLength);
    UfsPramField(Adapter, "PRDTN", Adapter->LastPrdtEntries);
    UfsPramField(Adapter, "BLKSIZE", Adapter->LogicalBlockSize);
    UfsPramField(Adapter, "CAPVALID", (ULONG)Adapter->CapacityValid);
    UfsPramString(Adapter, " LASTLBA=");
    UfsPramHex64(Adapter, Adapter->LastLogicalBlock);
    if ((Adapter->CapacityShortTransfers != 0) ||
        (Adapter->CapacityRejected != 0) ||
        (Adapter->CapacityMismatches != 0) ||
        (Adapter->CapacityRetryRequests != 0) ||
        (Adapter->CapacityRetriesExhausted != 0)) {
        UfsPramField(Adapter, "CAPSHORT", Adapter->CapacityShortTransfers);
        UfsPramField(Adapter, "CAPREJ", Adapter->CapacityRejected);
        UfsPramField(Adapter, "CAPMM", Adapter->CapacityMismatches);
        UfsPramField(Adapter, "CAPTRY", Adapter->CapacityRetryRequests);
        UfsPramField(Adapter, "CAPEXH", Adapter->CapacityRetriesExhausted);
        UfsPramField(Adapter, "CAPBADBS", Adapter->LastRejectedBlockSize);
        UfsPramString(Adapter, " CAPBADLBA=");
        UfsPramHex64(Adapter, Adapter->LastRejectedLogicalBlock);
    }

    //
    // Write state. WLOCK/WATT are the whole point of the crash latch: a boot
    // that bugchecks mid-probe destroys its own PRAM ring, so the ONLY way the
    // host ever learns which attempt killed it is to read it back here on the
    // following boot.
    //
    UfsPramField(Adapter, "WMODE", Adapter->WriteMode);
    UfsPramField(Adapter, "WLOCK", Adapter->Diagnostic.WriteCrashLockout);
    UfsPramField(Adapter, "WATT", Adapter->Diagnostic.WriteCrashAttempt);
    UfsPramField(Adapter, "WPHASE", Adapter->Diagnostic.WriteCrashPhase);
    UfsPramField(Adapter, "WBOOT", Adapter->Diagnostic.WriteBootEpoch);
    UfsPramField(Adapter, "WTRIED", Adapter->WritesAttempted);
    UfsPramField(Adapter, "WFENCE", Adapter->WritesFenced);
    UfsPramField(Adapter, "WGUARD", Adapter->WritesGuarded);
    //
    // WPROT is emitted only when it is non-zero. The ring already runs over
    // UFS_PRAM_CAPACITY and the last record of every boot is truncated, so a
    // counter that reads 0 on every healthy boot does not get to spend 13 bytes
    // on every healthy boot. The tag literal is still linked into the binary
    // either way, which is what the deployment freshness scan keys on.
    //
    if (Adapter->WritesProtected != 0) {
        UfsPramField(Adapter, "WPROT", Adapter->WritesProtected);
    }
    UfsPramField(Adapter, "WDISARM", Adapter->WritesDisarmedRejects);
    UfsPramField(Adapter, "WDRY", Adapter->WritesDryRun);
    UfsPramField(Adapter, "WISSUED", Adapter->WritesIssued);
    UfsPramField(Adapter, "WVERIFY", Adapter->WritesVerified);

    //
    // SMP topology exactly as the Windows kernel sees it. This is the
    // discriminator for the long-standing "Windows reports one core" defect.
    //
    // Every user-mode channel tried so far has failed to leave anything behind:
    // StatusBoard's ?:\smp-report.txt never appears, and its raw PhysicalDrive0
    // report is absent from both target LBAs. The reason is visible in this very
    // ring - UfsPmuRebootToRecovery() resets the PMU from inside this driver, so
    // nothing user mode wrote is ever committed. This ring is flushed
    // synchronously immediately before that reset, which makes it the only
    // Windows-side channel that survives.
    //
    //   On a platform without processor hot-add, the maximum processor count
    //   reported by the kernel equals the active count, so NPMAX mirrors NPACT.
    //
    UfsPramField(Adapter, "NPACT", KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS));
    UfsPramField(Adapter, "NPMAX", KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS));
    UfsPramField(Adapter, "NPGRP", (ULONG)KeQueryActiveGroupCount());
    UfsPramField(Adapter, "NTOK", 0);
    UfsPramField(Adapter, "NTPOLICY", 1);
    UfsPramField(Adapter, "BCAUX", (ULONG)UfsBugcheckProviderStatus);
    UfsPramField(Adapter, "BCREG", (ULONG)UfsBugcheckRegistrationStatus);
    UfsPramField(Adapter, "BCDATA", (ULONG)UfsBugcheckDataStatus);

    //
    // How many boots the firmware had counted before this one acknowledged itself.
    // 1 is healthy; anything higher names boots that never reached this driver at all.
    //
    UfsPramField(Adapter, "BATTACK", Adapter->Diagnostic.BootAttemptsAcknowledged);

    //
    // How many identical snapshots were dropped before this one. The ring is
    // linear and does not wrap, so duplicates are not free: every one of them
    // permanently costs ~700 bytes of the only evidence channel that survives a
    // bugcheck. A non-zero PSUP means the state was genuinely static, not that
    // telemetry was lost.
    //
    UfsPramField(Adapter, "PSUP", Adapter->PramDuplicatesSuppressed);

    //
    // The open question this whole build exists to answer: a data-in command
    // can report OCS=0 with residual 0 and still deliver nothing. BOUNCE tells
    // us where the controller was asked to write, DINLEN how much it claimed,
    // and DIN the bytes that actually arrived.
    //
    UfsPramString(Adapter, " BOUNCE=");
    UfsPramHex64(Adapter, (ULONGLONG)Adapter->BouncePhysical.QuadPart);
    UfsPramField(Adapter, "DINLEN", Adapter->LastDataInLength);
    UfsPramString(Adapter, " DIN=");
    {
        ULONG Index;

        for (Index = 0; Index < UFS_LAST_DATA_PREFIX; Index++) {
            UfsPramHex8(Adapter, Adapter->LastDataIn[Index]);
        }
    }

    //
    // Multi-block transfers only. DIN describes block 0 and nothing else, so a
    // wider transfer needs NZBLK - one bit per delivered 4096-byte granule - and
    // DIN2, the head of block 1, whose expected value for the probe's LBA-1-based
    // reads is the independently observed GPT entry-0 type GUID.
    //
    // Emitted conditionally because the ring is the binding constraint on this
    // whole loop: it is linear, does not wrap, and drops the NEWEST records when
    // full. Charging every single-block record ~50 bytes for two fields that
    // could not vary would spend the evidence budget on saying nothing.
    //
    if (Adapter->LastDataInLength > UFS_MULTI_BLOCK_GRANULE) {
        ULONG Index;

        UfsPramField(Adapter, "NZBLK", Adapter->LastDataInBlockMask);
        UfsPramString(Adapter, " DIN2=");
        for (Index = 0; Index < UFS_LAST_DATA_PREFIX; Index++) {
            UfsPramHex8(Adapter, Adapter->LastDataInBlock1[Index]);
        }
    }
    UfsPramByte(Adapter, (UCHAR)'\n');

    Adapter->PramRecords++;
    KeMemoryBarrier();
}

/*
 * The write trail, deliberately NOT a full snapshot.
 *
 * The ring is linear, does not wrap, and drops the NEWEST records once the
 * 16128-byte cap is reached. Every write-path event used to append a complete
 * UfsPramSnapshot at ~869 bytes. That was affordable while a LIVE cycle issued
 * exactly two writes (four write records); the width ladder issues two writes
 * per rung across four rungs, i.e. sixteen, and sixteen full snapshots alone
 * come to ~13.9 KB. The ring would cap partway through - and because the ladder
 * ascends, the records lost would be precisely the wide rungs the experiment
 * exists to measure. Cost has to come down before the ladder can be flashed, or
 * the run produces no usable evidence about the widths that matter.
 *
 * The fields kept are the ones no other record can supply for THIS write:
 *   WMODE    - the mode in force at the instant the write was issued. The host
 *              verdict pairs every WISSUED increment with it; reading the final
 *              value instead reports a clean LIVE cycle as a contract violation,
 *              because the loader always disarms last.
 *   WISSUED  - the counter the increment is being attributed to.
 *   WVERIFY  - lets a write that landed but failed read-back be told apart from
 *              one that never reached the device.
 *   REQLEN   - the transfer width. This is what makes the ladder readable at
 *              all: the widest rung that reached hardware is the largest REQLEN
 *              carrying a WISSUED increment.
 *   PRDTN    - entries programmed for that width. One entry per 4096-byte data
 *              unit is the Exynos FMP contract, so REQLEN/4096 != PRDTN means
 *              the descriptor, not the device, is wrong.
 *   LASTOCS  - overall command status.
 *   IS       - which fatal interrupt bit, if any, contained it.
 *   FMASK    - the accumulated failure bits.
 *
 * Everything else that used to be repeated here (WTRIED/WGUARD/WFENCE/WDISARM/
 * WDRY/CONTAINED/FATAL/STARTED/...) is still emitted by the UFSDIAG snapshots
 * bracketing the cycle, and the host decoder resolves fields across the whole
 * ring rather than from a single record, so nothing is actually lost.
 */
static
VOID
UfsPramWriteTrail(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    if (Adapter->Pram == NULL) {
        return;
    }

    UfsPramString(Adapter, "\n=UFSWRIT");
    UfsPramField(Adapter, "SEQ", Adapter->PramRecords);
    UfsPramField(Adapter, "WMODE", Adapter->WriteMode);
    UfsPramField(Adapter, "WISSUED", Adapter->WritesIssued);
    UfsPramField(Adapter, "WVERIFY", Adapter->WritesVerified);
    UfsPramField(Adapter, "REQLEN", Adapter->LastRequestLength);
    UfsPramField(Adapter, "PRDTN", Adapter->LastPrdtEntries);
    UfsPramField(Adapter, "LASTOCS", Adapter->LastOcs);
    UfsPramField(Adapter, "IS", Adapter->Diagnostic.InterruptStatus);
    UfsPramString(Adapter, " FMASK=");
    UfsPramHex64(Adapter, Adapter->Diagnostic.FailureMask);
    UfsPramByte(Adapter, (UCHAR)'\n');

    Adapter->PramRecords++;
    KeMemoryBarrier();
}

static
VOID
UfsBuildPramSignature(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _Out_ PUFS_PRAM_SIGNATURE Signature
    )
{
    RtlZeroMemory(Signature, sizeof(*Signature));

    Signature->FailureMask = Adapter->Diagnostic.FailureMask;
    Signature->LastLogicalBlock = Adapter->LastLogicalBlock;
    Signature->LastRejectedLogicalBlock =
        Adapter->LastRejectedLogicalBlock;
    Signature->BouncePhysical = (ULONGLONG)Adapter->BouncePhysical.QuadPart;
    Signature->RejectedOpcodes = UfsPramPackRejectedOpcodes(Adapter);
    Signature->FirstRejectContext = Adapter->Diagnostic.FirstRejectContext;
    Signature->FailureHistogram = UfsPramPackFailureHistogram(Adapter);
    Signature->FirstFailure = UfsPramPackFirstFailure(Adapter);
    Signature->Stage = Adapter->Diagnostic.Stage;
    Signature->FailureStage = Adapter->Diagnostic.FailureStage;
    Signature->HwInitializeCalls = Adapter->Diagnostic.HwInitializeCalls;
    Signature->LastSrbFunction = Adapter->Diagnostic.LastSrbFunction;
    Signature->LastExecuteReject = Adapter->Diagnostic.LastExecuteReject;
    Signature->RearmAttempts = Adapter->RearmAttempts;
    Signature->IoControlRequests = Adapter->Diagnostic.IoControlRequests;
    Signature->IoControlRejects = Adapter->Diagnostic.IoControlRejects;
    Signature->CompletedCommands = Adapter->CompletedCommands;
    Signature->FailedCommands = Adapter->FailedCommands;
    Signature->RejectedCommands = Adapter->RejectedCommands;
    Signature->RejectedOpcodeCount = Adapter->Diagnostic.RejectedOpcodeCount;
    Signature->ModeSenseTranslated = Adapter->ModeSenseTranslated;
    Signature->DataOverrunFailures = Adapter->Diagnostic.DataOverrunFailures;
    Signature->LastTrdDw2 = Adapter->LastTrdDw2;
    Signature->GptTranslation = UfsPramPackGptTranslation(Adapter);
    Signature->ContainedCommands = Adapter->ContainedCommands;
    Signature->LastOpcode = Adapter->LastOpcode;
    Signature->LastOcs = Adapter->LastOcs;
    Signature->InterruptStatus = Adapter->Diagnostic.InterruptStatus;
    Signature->AckFailureOcs = Adapter->AckFailureOcs;
    Signature->LastRequestLength = Adapter->LastRequestLength;
    Signature->LastPrdtEntries = Adapter->LastPrdtEntries;
    Signature->LogicalBlockSize = Adapter->LogicalBlockSize;
    Signature->CapacityShortTransfers = Adapter->CapacityShortTransfers;
    Signature->CapacityRejected = Adapter->CapacityRejected;
    Signature->CapacityMismatches = Adapter->CapacityMismatches;
    Signature->CapacityRetryRequests = Adapter->CapacityRetryRequests;
    Signature->CapacityRetriesExhausted =
        Adapter->CapacityRetriesExhausted;
    Signature->LastRejectedBlockSize = Adapter->LastRejectedBlockSize;
    Signature->WriteMode = Adapter->WriteMode;
    Signature->WriteCrashLockout = Adapter->Diagnostic.WriteCrashLockout;
    Signature->WriteCrashAttempt = Adapter->Diagnostic.WriteCrashAttempt;
    Signature->WriteCrashPhase = Adapter->Diagnostic.WriteCrashPhase;
    Signature->WriteBootEpoch = Adapter->Diagnostic.WriteBootEpoch;
    Signature->WritesAttempted = Adapter->WritesAttempted;
    Signature->WritesFenced = Adapter->WritesFenced;
    Signature->WritesGuarded = Adapter->WritesGuarded;
    Signature->WritesProtected = Adapter->WritesProtected;
    Signature->WritesDisarmedRejects = Adapter->WritesDisarmedRejects;
    Signature->WritesDryRun = Adapter->WritesDryRun;
    Signature->WritesIssued = Adapter->WritesIssued;
    Signature->WritesVerified = Adapter->WritesVerified;
    Signature->BootAttemptsAcknowledged =
        Adapter->Diagnostic.BootAttemptsAcknowledged;
    Signature->LastDataInLength = Adapter->LastDataInLength;
    Signature->DiagnosticOnly = (ULONG)Adapter->DiagnosticOnly;
    Signature->Started = (ULONG)Adapter->Started;
    Signature->FatalError = (ULONG)Adapter->FatalError;
    Signature->OwnsTransferList = (ULONG)Adapter->OwnsTransferList;
    Signature->CapacityValid = (ULONG)Adapter->CapacityValid;

    RtlCopyMemory(
        Signature->LastDataIn,
        Adapter->LastDataIn,
        sizeof(Signature->LastDataIn)
        );

    if (Adapter->LastDataInLength > UFS_MULTI_BLOCK_GRANULE) {
        Signature->LastDataInBlockMask = Adapter->LastDataInBlockMask;
        RtlCopyMemory(
            Signature->LastDataInBlock1,
            Adapter->LastDataInBlock1,
            sizeof(Signature->LastDataInBlock1)
            );
    }
}

/*
 * Append only fields that changed since the previous UFSDIAG record.
 *
 * The first vendor diagnostic remains a full snapshot. Later records carry the
 * observation counters plus state deltas; the host decoder already carries
 * missing fields forward, so this preserves the same reconstructed state while
 * cutting the measured V24 diagnostic payload from ~11 KB to less than 3.5 KB.
 */
static
VOID
UfsPramDiagnosticDelta(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ const UFS_PRAM_SIGNATURE *Previous,
    _In_ const UFS_PRAM_SIGNATURE *Current
    )
{
    ULONG Index;

#define UFS_PRAM_DELTA_FIELD(Tag, Member)                                  \
    do {                                                                   \
        if (Current->Member != Previous->Member) {                         \
            UfsPramField(Adapter, Tag, Current->Member);                   \
        }                                                                  \
    } while (FALSE)

#define UFS_PRAM_DELTA_HEX64(Tag, Member)                                  \
    do {                                                                   \
        if (Current->Member != Previous->Member) {                         \
            UfsPramString(Adapter, " " Tag "=");                          \
            UfsPramHex64(Adapter, Current->Member);                        \
        }                                                                  \
    } while (FALSE)

    UfsPramString(Adapter, "\n=UFSDIAG");
    UfsPramField(Adapter, "SEQ", Adapter->PramRecords);
    UFS_PRAM_DELTA_FIELD("STAGE", Stage);
    UFS_PRAM_DELTA_FIELD("FSTAGE", FailureStage);
    UFS_PRAM_DELTA_HEX64("FMASK", FailureMask);
    UFS_PRAM_DELTA_FIELD("DIAGONLY", DiagnosticOnly);
    UFS_PRAM_DELTA_FIELD("STARTED", Started);
    UFS_PRAM_DELTA_FIELD("FATAL", FatalError);
    UFS_PRAM_DELTA_FIELD("OWNSLIST", OwnsTransferList);
    UFS_PRAM_DELTA_FIELD("HWINIT", HwInitializeCalls);

    //
    // These counters move because the vendor diagnostic itself is an ordinary
    // execute-SCSI request, so they are intentionally excluded from the
    // duplicate signature but included whenever a real state delta is emitted.
    //
    UfsPramField(Adapter, "STARTIO", Adapter->Diagnostic.StartIoRequests);
    UfsPramField(Adapter, "EXECSCSI", Adapter->Diagnostic.ExecuteScsiRequests);

    UFS_PRAM_DELTA_FIELD("LASTSRBFN", LastSrbFunction);
    UFS_PRAM_DELTA_FIELD("EXECREJ", LastExecuteReject);
    UFS_PRAM_DELTA_FIELD("REARM", RearmAttempts);
    UFS_PRAM_DELTA_FIELD("IOCTLREQ", IoControlRequests);
    UFS_PRAM_DELTA_FIELD("IOCTLREJ", IoControlRejects);
    UfsPramField(Adapter, "VENDOR", Adapter->Diagnostic.VendorDiagRequests);
    UFS_PRAM_DELTA_FIELD("DONE", CompletedCommands);
    UFS_PRAM_DELTA_FIELD("FAILED", FailedCommands);
    UFS_PRAM_DELTA_FIELD("REJECTED", RejectedCommands);
    UFS_PRAM_DELTA_FIELD("ROPN", RejectedOpcodeCount);
    UFS_PRAM_DELTA_HEX64("ROPS", RejectedOpcodes);
    UFS_PRAM_DELTA_HEX64("RCTX", FirstRejectContext);
    UFS_PRAM_DELTA_FIELD("MS10", ModeSenseTranslated);
    UFS_PRAM_DELTA_HEX64("FHIST", FailureHistogram);
    UFS_PRAM_DELTA_FIELD("FOVR", DataOverrunFailures);
    UFS_PRAM_DELTA_HEX64("FF", FirstFailure);
    UFS_PRAM_DELTA_FIELD("DW2", LastTrdDw2);
    UFS_PRAM_DELTA_FIELD("GPTX", GptTranslation);
    UFS_PRAM_DELTA_FIELD("CONTAINED", ContainedCommands);
    UFS_PRAM_DELTA_FIELD("LASTOP", LastOpcode);
    UFS_PRAM_DELTA_FIELD("LASTOCS", LastOcs);
    UFS_PRAM_DELTA_FIELD("IS", InterruptStatus);
    UFS_PRAM_DELTA_FIELD("ACKOCS", AckFailureOcs);
    UFS_PRAM_DELTA_FIELD("REQLEN", LastRequestLength);
    UFS_PRAM_DELTA_FIELD("PRDTN", LastPrdtEntries);
    UFS_PRAM_DELTA_FIELD("BLKSIZE", LogicalBlockSize);
    UFS_PRAM_DELTA_FIELD("CAPVALID", CapacityValid);
    UFS_PRAM_DELTA_HEX64("LASTLBA", LastLogicalBlock);
    UFS_PRAM_DELTA_FIELD("CAPSHORT", CapacityShortTransfers);
    UFS_PRAM_DELTA_FIELD("CAPREJ", CapacityRejected);
    UFS_PRAM_DELTA_FIELD("CAPMM", CapacityMismatches);
    UFS_PRAM_DELTA_FIELD("CAPTRY", CapacityRetryRequests);
    UFS_PRAM_DELTA_FIELD("CAPEXH", CapacityRetriesExhausted);
    UFS_PRAM_DELTA_FIELD("CAPBADBS", LastRejectedBlockSize);
    UFS_PRAM_DELTA_HEX64("CAPBADLBA", LastRejectedLogicalBlock);
    UFS_PRAM_DELTA_FIELD("WMODE", WriteMode);
    UFS_PRAM_DELTA_FIELD("WLOCK", WriteCrashLockout);
    UFS_PRAM_DELTA_FIELD("WATT", WriteCrashAttempt);
    UFS_PRAM_DELTA_FIELD("WPHASE", WriteCrashPhase);
    UFS_PRAM_DELTA_FIELD("WBOOT", WriteBootEpoch);
    UFS_PRAM_DELTA_FIELD("WTRIED", WritesAttempted);
    UFS_PRAM_DELTA_FIELD("WFENCE", WritesFenced);
    UFS_PRAM_DELTA_FIELD("WGUARD", WritesGuarded);
    UFS_PRAM_DELTA_FIELD("WPROT", WritesProtected);
    UFS_PRAM_DELTA_FIELD("WDISARM", WritesDisarmedRejects);
    UFS_PRAM_DELTA_FIELD("WDRY", WritesDryRun);
    UFS_PRAM_DELTA_FIELD("WISSUED", WritesIssued);
    UFS_PRAM_DELTA_FIELD("WVERIFY", WritesVerified);
    UFS_PRAM_DELTA_FIELD("BATTACK", BootAttemptsAcknowledged);
    UfsPramField(Adapter, "PSUP", Adapter->PramDuplicatesSuppressed);
    UFS_PRAM_DELTA_HEX64("BOUNCE", BouncePhysical);
    UFS_PRAM_DELTA_FIELD("DINLEN", LastDataInLength);

    if (RtlCompareMemory(
            Current->LastDataIn,
            Previous->LastDataIn,
            sizeof(Current->LastDataIn)) != sizeof(Current->LastDataIn)) {
        UfsPramString(Adapter, " DIN=");
        for (Index = 0; Index < UFS_LAST_DATA_PREFIX; Index++) {
            UfsPramHex8(Adapter, Current->LastDataIn[Index]);
        }
    }

    //
    // NZBLK and DIN2 describe only transfers wider than one 4096-byte granule.
    // Emit the pair when entering, changing within, or leaving that state. The
    // exit record deliberately carries zero tombstones so a sparse decoder can
    // clear the previous multi-block values. A length change between two
    // single-block transfers must not fabricate a zero-valued multi-block sample.
    //
    if (((Current->LastDataInLength > UFS_MULTI_BLOCK_GRANULE) ||
         (Previous->LastDataInLength > UFS_MULTI_BLOCK_GRANULE)) &&
        ((Current->LastDataInLength != Previous->LastDataInLength) ||
         (Current->LastDataInBlockMask != Previous->LastDataInBlockMask) ||
         (RtlCompareMemory(
              Current->LastDataInBlock1,
              Previous->LastDataInBlock1,
              sizeof(Current->LastDataInBlock1)) !=
          sizeof(Current->LastDataInBlock1)))) {
        UfsPramField(Adapter, "NZBLK", Current->LastDataInBlockMask);
        UfsPramString(Adapter, " DIN2=");
        for (Index = 0; Index < UFS_LAST_DATA_PREFIX; Index++) {
            UfsPramHex8(Adapter, Current->LastDataInBlock1[Index]);
        }
    }

    UfsPramByte(Adapter, (UCHAR)'\n');
    Adapter->PramRecords++;
    KeMemoryBarrier();

#undef UFS_PRAM_DELTA_FIELD
#undef UFS_PRAM_DELTA_HEX64
}

/*
 * Would a UFSDIAG snapshot taken right now carry anything the last one written
 * did not already say?
 *
 * The vendor diagnostic CDB is a pure READ, but it used to append a full
 * snapshot on every call. StatusBoard polls it to keep the dashboard live, so
 * simply watching the adapter consumed the buffer that exists to explain a
 * crash - the more closely it was observed, the less could be seen. On the
 * 2026-07-29 write-probe boot that cost the entire investigation: 20 UFSDIAG
 * records, 13 of them byte-identical, exhausted all 16128 bytes, and the
 * bugcheck that followed left nothing behind but the fixed-address crash latch.
 *
 * Suppressing a record that repeats the previous one discards no information by
 * definition, and the count of suppressed records is emitted as PSUP so a
 * static period is still distinguishable from a gap in telemetry.
 *
 * The comparison runs over UFS_PRAM_SIGNATURE rather than UFS_DIAGNOSTIC_DATA.
 * The first attempt compared the diagnostic struct and provably could never
 * fire, because the vendor read is itself an SRB_FUNCTION_EXECUTE_SCSI and so
 * bumps StartIoRequests/ExecuteScsiRequests before the compare ever happens;
 * the device returned PSUP=0 on all 19 records while the ring still filled.
 * That same struct also omits the majority of the printed fields, so a match
 * would have thrown away new data-in bytes. The signature is built from the
 * printed state and excludes only what the act of observing perturbs.
 */
static
BOOLEAN
UfsPramDiagnosticChanged(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _Out_ PUFS_PRAM_SIGNATURE Previous,
    _Out_ PUFS_PRAM_SIGNATURE Current
    )
{
    BOOLEAN Changed;

    UfsBuildPramSignature(Adapter, Current);
    RtlZeroMemory(Previous, sizeof(*Previous));

    if (!Adapter->LastPramDiagnosticValid) {
        Changed = TRUE;
    } else {
        RtlCopyMemory(
            Previous,
            &Adapter->LastPramDiagnostic,
            sizeof(*Previous)
            );
        Changed =
            (RtlCompareMemory(
                 Current,
                 &Adapter->LastPramDiagnostic,
                 sizeof(*Current)
                 ) != sizeof(*Current));
    }

    if (Changed) {
        //
        // RtlCopyMemory, not struct assignment: the signature has trailing
        // padding and a member-wise copy would leave the destination's padding
        // untouched, so a later memcmp could differ on bytes no field owns.
        //
        RtlCopyMemory(
            &Adapter->LastPramDiagnostic,
            Current,
            sizeof(*Current)
            );
        Adapter->LastPramDiagnosticValid = TRUE;
    }

    return Changed;
}

/*
 * Quiesce the transfer list after a command-level anomaly.
 *
 * This used to latch the whole adapter off (Started = FALSE, FatalError =
 * TRUE), which made UfsExecuteScsi return SRB_STATUS_ERROR for every later
 * request - including the ones completed locally, so even TEST UNIT READY
 * failed and the disk class driver was left with a zero-byte device it could
 * never re-interrogate. Every anomaly this controller actually produces
 * (interrupt-status re-latching, response framing after a warm handoff) is
 * transient, so a one-way kill switch turns a recoverable command failure
 * into a dead boot with no diagnosable state.
 *
 * The list is now stopped and marked un-owned; the next command re-arms it
 * (see UfsExecuteScsi). Only a controller that refuses to release the list is
 * treated as a real adapter fatality.
 */
static
VOID
UfsContainController(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONGLONG Failure
    )
{
    UfsSetDiagnosticFailure(Adapter, Failure);
    /*
     * UTRLRSR only stops the host from fetching another descriptor; it does
     * not prove that a doorbelled command stopped using slot 0 or the shared
     * bounce buffer. Quarantine before touching the controller so neither a
     * Storport reset nor ScsiRestartAdapter can race ahead and reuse them.
     */
    if (Failure == UFS_DIAG_FAILURE_TIMEOUT_CONTAINMENT) {
        Adapter->FatalError = TRUE;
        Adapter->Started = FALSE;
    }
    /*
     * Disarm on any containment. The arm flag is an operator's statement that
     * the adapter is in a known state and they accept the risk; a containment
     * is the driver saying that is no longer true. Re-arming must then be an
     * explicit, deliberate act rather than something that survives an anomaly
     * unnoticed - fail closed, always, in the direction of not writing.
     */
#if UFS_FULL_WINDOWS_MODE
    /*
     * ...but not when this adapter is carrying the running system volume.
     *
     * Everything above describes a bring-up experiment: an operator is
     * present, a write is a deliberate act, and refusing one costs nothing.
     * None of that survives Windows booting from this disk. There the arm
     * latch is not consent for one experiment, it is the only thing making
     * the system disk a disk.
     *
     * The containment performed here is RECOVERABLE by construction - the
     * header above says so, the list is merely stopped and marked un-owned,
     * and the next command re-arms it. Every caller on the ordinary I/O path
     * returns a status Storport retries: PROGRAM_PRECONDITION gives
     * SRB_STATUS_BUSY, TIMEOUT_CONTAINMENT gives SRB_STATUS_TIMEOUT, and
     * NEXUS_READBACK / INTERRUPT_STATUS give SRB_STATUS_ERROR. The anomalies
     * the header names - interrupt-status re-latching, response framing after
     * a warm handoff - are transient by its own account.
     *
     * Revoking the latch on that does not fail closed. It fails silently and
     * permanently: the controller recovers on the very next command, reads
     * resume, and writes alone stay dead for the rest of the session. The
     * signature is WTRIED climbing while WISSUED is frozen and WDISARM tracks
     * it - indistinguishable from the boot-1 bcdboot refusal this build exists
     * to fix, but arriving mid-session from an unrelated cause. One retryable
     * SRB_STATUS_BUSY is enough to trigger it.
     *
     * This is the same hazard already settled one call site over, in
     * ScsiRestartAdapter: "Disarming here would silently drop the system disk
     * back to read-only in the middle of a session, which Windows cannot
     * survive and would report only as unexplained I/O failures." A transient
     * command anomaly is a weaker reason to do that than a power transition,
     * not a stronger one.
     *
     * Holding the latch weakens nothing. A controller that genuinely will not
     * release the transfer list is still caught immediately below, where the
     * UfsPollRegister failure sets FatalError and clears Started - and that
     * stops the adapter outright regardless of WriteMode. The event stays
     * fully visible either way, because ContainedCommands is incremented on
     * the way out and reaches the ring as CONTAINED; a containment that kept
     * the latch now reads as CONTAINED non-zero with WMODE still live.
     */
#else
    Adapter->WriteMode = UFS_WRITE_MODE_DISARMED;
#endif
    UfsWriteRegister(Adapter, UFS_REG_INTERRUPT_ENABLE, 0);
    KeMemoryBarrier();
    UfsWriteRegister(Adapter, UFS_REG_UTRL_RUN_STOP, 0);
    KeMemoryBarrier();
    if (!UfsPollRegister(
            Adapter,
            UFS_REG_UTRL_RUN_STOP,
            UFS_LIST_RUN_STOP,
            0,
            UFS_STOP_TIMEOUT_US
            )) {
        UfsSetDiagnosticFailure(
            Adapter,
            UFS_DIAG_FAILURE_TIMEOUT_CONTAINMENT
            );
        Adapter->FatalError = TRUE;
        Adapter->Started = FALSE;
    }
    Adapter->InterruptsGated = TRUE;
    Adapter->OwnsTransferList = FALSE;
    Adapter->ContainedCommands++;
    if (Adapter->FatalError) {
        UfsRwd1SetFatalNoPet(
            Adapter,
            UFS_NO_PET_STORAGE_FATAL,
            RWD1_REASON_WINDOWS_STORAGE_FATAL,
            RWD1_PHASE_WINDOWS_STORAGE_FATAL,
            (ULONG)Failure
            );
    }
}

static
VOID
UfsEnterDiagnosticOnly(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG FailureStage
    )
{
    if (Adapter->Diagnostic.FailureStage == UFS_DIAG_STAGE_NONE) {
        Adapter->Diagnostic.FailureStage = FailureStage;
    }

    Adapter->DiagnosticOnly = TRUE;
    Adapter->Diagnostic.DiagnosticOnly = TRUE;
    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_DIAGNOSTIC_ONLY);
}

static
VOID
UfsCaptureResources(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo
    )
{
    ULONG Index;
    ULONG Count;

    Adapter->Diagnostic.AdapterInterfaceType =
        (ULONG)ConfigInfo->AdapterInterfaceType;
    Adapter->Diagnostic.SystemIoBusNumber = ConfigInfo->SystemIoBusNumber;
    Adapter->Diagnostic.NumberOfAccessRanges =
        ConfigInfo->NumberOfAccessRanges;

    if (ConfigInfo->AccessRanges == NULL) {
        UfsSetDiagnosticFailure(
            Adapter,
            UFS_DIAG_FAILURE_ACCESS_RANGES_NULL
            );
        UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_RESOURCES_CAPTURED);
        return;
    }

    Count = min(
        ConfigInfo->NumberOfAccessRanges,
        UFS_DIAG_MAX_ACCESS_RANGES
        );
    Adapter->Diagnostic.CapturedAccessRanges = Count;
    for (Index = 0; Index < Count; Index++) {
        PACCESS_RANGE Range = &(*ConfigInfo->AccessRanges)[Index];

        Adapter->Diagnostic.AccessRanges[Index].Start =
            (ULONGLONG)Range->RangeStart.QuadPart;
        Adapter->Diagnostic.AccessRanges[Index].Length = Range->RangeLength;
        Adapter->Diagnostic.AccessRanges[Index].InMemory =
            Range->RangeInMemory ? 1UL : 0UL;
    }

    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_RESOURCES_CAPTURED);
}

static
VOID
UfsRefreshDiagnostic(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    Adapter->Diagnostic.HciVirtual =
        (ULONGLONG)(ULONG_PTR)Adapter->Hci;
    Adapter->Diagnostic.WorkspaceVirtual =
        (ULONGLONG)(ULONG_PTR)Adapter->Workspace;
    Adapter->Diagnostic.WorkspaceSize = UFS_WORKSPACE_SIZE;
    Adapter->Diagnostic.UtrlVirtual =
        (ULONGLONG)(ULONG_PTR)Adapter->Utrl;
    Adapter->Diagnostic.UtrlPhysical =
        (ULONGLONG)Adapter->UtrlPhysical.QuadPart;
    Adapter->Diagnostic.UcdVirtual =
        (ULONGLONG)(ULONG_PTR)Adapter->Ucd;
    Adapter->Diagnostic.UcdPhysical =
        (ULONGLONG)Adapter->UcdPhysical.QuadPart;
    Adapter->Diagnostic.BounceVirtual =
        (ULONGLONG)(ULONG_PTR)Adapter->Bounce;
    Adapter->Diagnostic.BouncePhysical =
        (ULONGLONG)Adapter->BouncePhysical.QuadPart;
    Adapter->Diagnostic.ResourcesMapped =
        Adapter->ResourcesMapped ? 1UL : 0UL;
    Adapter->Diagnostic.WarmStateValid =
        Adapter->WarmStateValid ? 1UL : 0UL;
    Adapter->Diagnostic.DiagnosticOnly =
        Adapter->DiagnosticOnly ? 1UL : 0UL;
    Adapter->Diagnostic.Started = Adapter->Started ? 1UL : 0UL;
    Adapter->Diagnostic.Operational =
        (Adapter->Started &&
         !Adapter->DiagnosticOnly &&
         Adapter->WarmStateValid &&
         !Adapter->FatalError) ? 1UL : 0UL;
    Adapter->Diagnostic.CompletedCommands = Adapter->CompletedCommands;
    Adapter->Diagnostic.RejectedCommands = Adapter->RejectedCommands;
    Adapter->Diagnostic.FailedCommands = Adapter->FailedCommands;
    Adapter->Diagnostic.ContainedCommands = Adapter->ContainedCommands;
    Adapter->Diagnostic.WriteProtectedResponses =
        Adapter->WriteProtectedResponses;
    Adapter->Diagnostic.LastOpcode = Adapter->LastOpcode;
    Adapter->Diagnostic.LastOcs = Adapter->LastOcs;
    Adapter->Diagnostic.LastLogicalBlock = Adapter->LastLogicalBlock;
    Adapter->Diagnostic.LogicalBlockSize = Adapter->LogicalBlockSize;
    Adapter->Diagnostic.CapacityValid =
        Adapter->CapacityValid ? 1UL : 0UL;
    Adapter->Diagnostic.OwnsTransferList =
        Adapter->OwnsTransferList ? 1UL : 0UL;
    Adapter->Diagnostic.InterruptsGated =
        Adapter->InterruptsGated ? 1UL : 0UL;
    Adapter->Diagnostic.DiagnosticEndpointPreserved =
        Adapter->DiagnosticEndpointPreserved ? 1UL : 0UL;
    Adapter->Diagnostic.FatalError =
        Adapter->FatalError ? 1UL : 0UL;
    Adapter->Diagnostic.RearmAttempts = Adapter->RearmAttempts;
    Adapter->Diagnostic.InterruptCallbacks =
        (ULONG)Adapter->InterruptCallbacks;
    Adapter->Diagnostic.PramRecords = Adapter->PramRecords;
    Adapter->Diagnostic.RebootRequests = Adapter->RebootRequests;
    Adapter->Diagnostic.LastDataInLength = Adapter->LastDataInLength;
    /*
     * Write state. WritesArmed is derived rather than stored so the two can
     * never disagree: there is exactly one arm latch, and it is WriteMode.
     */
    Adapter->Diagnostic.WriteMode = Adapter->WriteMode;
    Adapter->Diagnostic.WritesArmed =
        (Adapter->WriteMode != UFS_WRITE_MODE_DISARMED) ? 1UL : 0UL;
    Adapter->Diagnostic.WriteFenceFirstLba = (ULONG)UFS_WRITE_FENCE_FIRST_LBA;
    Adapter->Diagnostic.WriteFenceLastLba = (ULONG)UFS_WRITE_FENCE_LAST_LBA;
    Adapter->Diagnostic.WriteArmRequests = Adapter->WriteArmRequests;
    Adapter->Diagnostic.WritesAttempted = Adapter->WritesAttempted;
    Adapter->Diagnostic.WritesFenced = Adapter->WritesFenced;
    Adapter->Diagnostic.WritesGuarded = Adapter->WritesGuarded;
    Adapter->Diagnostic.WritesDisarmedRejects = Adapter->WritesDisarmedRejects;
    Adapter->Diagnostic.WritesDryRun = Adapter->WritesDryRun;
    Adapter->Diagnostic.WritesIssued = Adapter->WritesIssued;
    Adapter->Diagnostic.WritesVerified = Adapter->WritesVerified;
    Adapter->Diagnostic.WriteVerifyFailures = Adapter->WriteVerifyFailures;
    Adapter->Diagnostic.LastWriteLba = Adapter->LastWriteLba;
    Adapter->Diagnostic.LastWriteBlocks = Adapter->LastWriteBlocks;
    Adapter->Diagnostic.LastWriteResult = Adapter->LastWriteResult;

    /*
     * Note channel. NoteRequests is what the helper asked for and
     * PramNotesWritten is what actually landed in a reserved slot; a divergence
     * is the only signal that a note was dropped for want of a slot, so both are
     * exported rather than just the one.
     */
    Adapter->Diagnostic.NoteRequests = Adapter->NoteRequests;
    Adapter->Diagnostic.LastNoteCode = Adapter->LastNoteCode;
    Adapter->Diagnostic.LastNoteAux = Adapter->LastNoteAux;
    Adapter->Diagnostic.PramNotesWritten = Adapter->PramNotesWritten;
    Adapter->Diagnostic.WriteProtectSuppressed = Adapter->WriteProtectSuppressed;

#if UFS_PERF_INSTRUMENTATION
    /*
     * V33 latency snapshot. Published here rather than accumulated directly
     * into Diagnostic so the hot path touches only the adapter's own cache
     * lines and never the (much larger) reported structure.
     *
     * The frequency is sampled here too. CNTFRQ_EL0 is a constant for the life
     * of the system, so reading it on the diagnostic path rather than per-I/O
     * costs nothing and keeps the measured path free of a second system-
     * register read.
     */
    Adapter->PerfCounterFrequency = (ULONGLONG)_ReadStatusReg(ARM64_CNTFRQ_EL0);
    Adapter->Diagnostic.PerfCounterFrequency = Adapter->PerfCounterFrequency;
    Adapter->Diagnostic.PerfDeviceTicks = Adapter->PerfDeviceTicks;
    Adapter->Diagnostic.PerfZeroTicks = Adapter->PerfZeroTicks;
    Adapter->Diagnostic.PerfBounceInTicks = Adapter->PerfBounceInTicks;
    Adapter->Diagnostic.PerfBounceOutTicks = Adapter->PerfBounceOutTicks;
    Adapter->Diagnostic.PerfPollIterations = Adapter->PerfPollIterations;
    Adapter->Diagnostic.PerfReadBytes = Adapter->PerfReadBytes;
    Adapter->Diagnostic.PerfWriteBytes = Adapter->PerfWriteBytes;
    Adapter->Diagnostic.PerfMaxDeviceTicks = Adapter->PerfMaxDeviceTicks;
    Adapter->Diagnostic.PerfReadCount = Adapter->PerfReadCount;
    Adapter->Diagnostic.PerfWriteCount = Adapter->PerfWriteCount;
    Adapter->Diagnostic.PerfPollIntervalUs = UFS_TRANSFER_POLL_US;
#endif /* UFS_PERF_INSTRUMENTATION */
    RtlCopyMemory(
        Adapter->Diagnostic.LastDataIn,
        Adapter->LastDataIn,
        sizeof(Adapter->Diagnostic.LastDataIn)
        );
    if (Adapter->Utrl != NULL) {
        Adapter->Diagnostic.UtrdInterruptRequested =
            (Adapter->Utrl[UFS_TRANSFER_SLOT].Dw0 & UFS_TRD_INTERRUPT) != 0;
    }
}

static
BOOLEAN
UfsResourcePresent(
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _In_ ULONGLONG PhysicalBase,
    _In_ ULONG Length,
    _Out_opt_ PACCESS_RANGE *MatchedRange
    )
{
    ULONG Index;

    if (ConfigInfo->AccessRanges == NULL) {
        return FALSE;
    }

    for (Index = 0; Index < ConfigInfo->NumberOfAccessRanges; Index++) {
        PACCESS_RANGE Range = &(*ConfigInfo->AccessRanges)[Index];

        if (!Range->RangeInMemory) {
            continue;
        }

        if (((ULONGLONG)Range->RangeStart.QuadPart == PhysicalBase) &&
            (Range->RangeLength >= Length)) {
            if (MatchedRange != NULL) {
                *MatchedRange = Range;
            }
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOLEAN
UfsMapResources(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo
    )
{
    PACCESS_RANGE HciRange = NULL;
    BOOLEAN HciPresent;
    BOOLEAN UniProPresent;
    BOOLEAN PmaPresent;
    BOOLEAN UfspPresent;

    HciPresent = UfsResourcePresent(
        ConfigInfo,
        EXYNOS_UFS_HCI_PHYSICAL_BASE,
        EXYNOS_UFS_HCI_LENGTH,
        &HciRange
        );
    UniProPresent = UfsResourcePresent(
        ConfigInfo,
        EXYNOS_UFS_UNIPRO_PHYSICAL_BASE,
        EXYNOS_UFS_UNIPRO_LENGTH,
        NULL
        );
    PmaPresent = UfsResourcePresent(
        ConfigInfo,
        EXYNOS_UFS_PMA_PHYSICAL_BASE,
        EXYNOS_UFS_PMA_LENGTH,
        NULL
        );
    UfspPresent = UfsResourcePresent(
        ConfigInfo,
        EXYNOS_UFS_UFSP_PHYSICAL_BASE,
        EXYNOS_UFS_UFSP_LENGTH,
        NULL
        );

    if (!HciPresent) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_HCI_RANGE_MISSING);
    }
    if (!UniProPresent) {
        UfsSetDiagnosticFailure(
            Adapter,
            UFS_DIAG_FAILURE_UNIPRO_RANGE_MISSING
            );
    }
    if (!PmaPresent) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_PMA_RANGE_MISSING);
    }
    if (!UfspPresent) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_UFSP_RANGE_MISSING);
    }
    if (!HciPresent || !UniProPresent || !PmaPresent || !UfspPresent) {
        return FALSE;
    }

    Adapter->Hci = (PUCHAR)StorPortGetDeviceBase(
        Adapter,
        ConfigInfo->AdapterInterfaceType,
        ConfigInfo->SystemIoBusNumber,
        HciRange->RangeStart,
        EXYNOS_UFS_HCI_LENGTH,
        FALSE
        );
    if (Adapter->Hci == NULL) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_HCI_MAP);
        return FALSE;
    }

    Adapter->HciLength = EXYNOS_UFS_HCI_LENGTH;
    Adapter->ResourcesMapped = TRUE;
    Adapter->Diagnostic.HciVirtual =
        (ULONGLONG)(ULONG_PTR)Adapter->Hci;
    Adapter->Diagnostic.ResourcesMapped = 1;

    //
    // Best-effort, and deliberately after the device resources are mapped so a
    // failure here can never affect whether the adapter comes up.
    //
    UfsPramInitialize(Adapter, ConfigInfo);
    /* Safety is independent of whether the optional breadcrumb mapping exists. */
    UfsRegisterBugcheckCapture(Adapter);
    UfsPmuInitialize(Adapter);
    UfsLowWdtPramInitialize(Adapter);
#if UFS_AUTOMATIC_RECOVERY_RESET || UFS_FIRMWARE_WDT_BRIDGE || UFS_CONTINUOUS_HARDWARE_WATCHDOG
    UfsWdtInitialize(Adapter);
#endif
    return TRUE;
}

static
BOOLEAN
UfsMapFirmwareReservedWorkspace(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
#if UFS_DMA_WINDOW_ENFORCE
    volatile ULONG *Record;
    ULONG Magic;
    ULONG Base;
    ULONG Check;
    ULONGLONG End;
    PHYSICAL_ADDRESS Physical;
    PVOID Workspace;

    if (Adapter->Pram == NULL) {
        return FALSE;
    }

    Record = (volatile ULONG *)(
        Adapter->Pram + UFS_PRAM_DMA_POOL_MAGIC_OFFSET
        );
    Magic = Record[0];
    if (Magic != UFS_PRAM_DMA_POOL_MAGIC) {
        return FALSE;
    }

    Base = Record[1];
    Check = Record[2];

    /*
     * Consume the record before mapping it. Firmware republishes MAGIC on every
     * early-RAM boot; an older firmware image therefore cannot reuse this base.
     */
    Record[0] = UFS_PRAM_DMA_POOL_CONSUMED;
    KeMemoryBarrier();

    End = (ULONGLONG)Base + UFS_WORKSPACE_SIZE;
    if ((Check != (UFS_PRAM_DMA_POOL_MAGIC ^
                   Base ^
                   UFS_WORKSPACE_SIZE ^
                   UFS_PRAM_DMA_POOL_GUARD)) ||
        (((ULONGLONG)Base & (PAGE_SIZE - 1ULL)) != 0) ||
        ((ULONGLONG)Base < UFS_DMA_WINDOW_FIRST) ||
        (End > UFS_DMA_WINDOW_LAST)) {
        return FALSE;
    }

    Physical.QuadPart = (LONGLONG)(ULONGLONG)Base;
    Workspace = MmMapIoSpace(
        Physical,
        UFS_WORKSPACE_SIZE,
        MmNonCached
        );
    if (Workspace == NULL) {
        return FALSE;
    }

    Adapter->Workspace = Workspace;
    Adapter->WorkspacePhysical = Physical;
    Adapter->WorkspaceBounded = TRUE;
    Adapter->WorkspaceFirmwareReserved = TRUE;
    return TRUE;
#else
    UNREFERENCED_PARAMETER(Adapter);
    return FALSE;
#endif
}

static
BOOLEAN
UfsGetContiguousPhysicalAddress(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PVOID VirtualAddress,
    _In_ ULONG RequiredLength,
    _In_ ULONG Alignment,
    _Out_ PSTOR_PHYSICAL_ADDRESS PhysicalAddress
    )
{
    ULONG ContiguousLength = RequiredLength;

    *PhysicalAddress = StorPortGetPhysicalAddress(
        Adapter,
        NULL,
        VirtualAddress,
        &ContiguousLength
        );

    if ((PhysicalAddress->QuadPart == 0) ||
        (ContiguousLength < RequiredLength) ||
        (PhysicalAddress->HighPart != 0) ||
        (((ULONGLONG)PhysicalAddress->QuadPart & (Alignment - 1U)) != 0)) {
        return FALSE;
    }

    /*
     * FMP DMA WINDOW. This check is why the MR1 memory-expansion boot failed.
     *
     * Samsung's FMP restricts UFS DMA to the low memory window. Until MR1, the
     * only conventional memory below 4 GB was 0x90000000..0xBC800000, so every
     * allocation Storport could possibly return was inside that window BY
     * ACCIDENT OF THERE BEING NOWHERE ELSE TO PUT IT. Dma32BitAddresses=TRUE
     * expresses "below 4 GB", which was an accurate description of the machine
     * but not of the hardware's actual constraint.
     *
     * MR1 published bank1 (0xC0080000..0xE0000000) as conventional memory. That
     * is also below 4 GB, so Storport became free to place the UTRL, the UCD or
     * the bounce there - addresses the controller's DMA cannot reach. Storage
     * then never initialises and Windows hangs on the logo with no spinner,
     * which is exactly what happened.
     *
     * So state the real constraint here, on every DMA address, rather than
     * relying on the memory map to keep being small. This is cheap, it runs
     * once per region at init, and it converts an unbootable machine into a
     * clean diagnostic-only failure with a named cause.
     */
#if UFS_DMA_WINDOW_ENFORCE
    if (((ULONGLONG)PhysicalAddress->QuadPart < UFS_DMA_WINDOW_FIRST) ||
        (((ULONGLONG)PhysicalAddress->QuadPart + RequiredLength) >
         UFS_DMA_WINDOW_LAST)) {
        return FALSE;
    }
#endif

    return TRUE;
}

static
BOOLEAN
UfsGetWorkspacePhysicalAddress(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PVOID VirtualAddress,
    _In_ ULONG RequiredLength,
    _In_ ULONG Alignment,
    _Out_ PSTOR_PHYSICAL_ADDRESS PhysicalAddress
    )
{
#if UFS_DMA_WINDOW_ENFORCE
    if (Adapter->WorkspaceFirmwareReserved) {
        ULONG_PTR WorkspaceStart;
        ULONG_PTR Address;
        ULONG_PTR Offset;
        ULONGLONG Physical;

        WorkspaceStart = (ULONG_PTR)Adapter->Workspace;
        Address = (ULONG_PTR)VirtualAddress;
        if (Address < WorkspaceStart) {
            return FALSE;
        }

        Offset = Address - WorkspaceStart;
        if ((Offset > UFS_WORKSPACE_SIZE) ||
            (RequiredLength > UFS_WORKSPACE_SIZE - Offset)) {
            return FALSE;
        }

        Physical =
            (ULONGLONG)Adapter->WorkspacePhysical.QuadPart + Offset;
        PhysicalAddress->QuadPart = (LONGLONG)Physical;
        if ((Physical < UFS_DMA_WINDOW_FIRST) ||
            ((Physical + RequiredLength) > UFS_DMA_WINDOW_LAST) ||
            ((Physical & (Alignment - 1U)) != 0)) {
            return FALSE;
        }

        return TRUE;
    }
#endif

    return UfsGetContiguousPhysicalAddress(
        Adapter,
        VirtualAddress,
        RequiredLength,
        Alignment,
        PhysicalAddress
        );
}

static
BOOLEAN
UfsAllocateWorkspace(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo
    )
{
    ULONG_PTR Cursor;
    ULONG_PTR WorkspaceEnd;
    BOOLEAN UtrlValid;
    BOOLEAN UcdValid;
    BOOLEAN BounceValid;

    /*
     * Allocate the DMA workspace INSIDE the FMP window rather than hoping for
     * it. StorPortGetUncachedExtension honours only Dma32BitAddresses, i.e.
     * "anywhere below 4 GB", which stopped being equivalent to "reachable by
     * this controller" the moment MR1 published bank1 as conventional memory
     * (see UFS_DMA_WINDOW_FIRST/LAST and UfsGetContiguousPhysicalAddress).
     *
     * Early-RAM firmware now reserves and publishes a low workspace before
     * ExitBootServices. Prefer that deterministic handoff. The original
     * uncached-extension path remains as a compatibility fallback, and all
     * three regions are still rejected unless their physical addresses fit the
     * FMP aperture.
     */
#if UFS_DMA_WINDOW_ENFORCE
    (VOID)UfsMapFirmwareReservedWorkspace(Adapter);

    if (Adapter->Workspace == NULL) {
        Adapter->Workspace = StorPortGetUncachedExtension(
            Adapter,
            ConfigInfo,
            UFS_WORKSPACE_SIZE
            );
    }
#else
    /*
     * Switch off: this must be the ORIGINAL statement, character for character.
     * The driver is built /GL /LTCG, so whole-program optimisation re-lays-out
     * the entire image in response to any change at all - even an extra NULL
     * test that can never be true. Keeping the disabled path textually
     * identical is what lets the all-switches-off build still reproduce the
     * deployed v32 .text byte for byte, which is the only evidence that these
     * switches are the sole difference.
     */
    Adapter->Workspace = StorPortGetUncachedExtension(
        Adapter,
        ConfigInfo,
        UFS_WORKSPACE_SIZE
        );
#endif

    if (Adapter->Workspace == NULL) {
        UfsSetDiagnosticFailure(
            Adapter,
            UFS_DIAG_FAILURE_WORKSPACE_ALLOCATION
            );
        return FALSE;
    }
    Adapter->Diagnostic.WorkspaceAllocated = 1;
    Adapter->Diagnostic.WorkspaceVirtual =
        (ULONGLONG)(ULONG_PTR)Adapter->Workspace;
    Adapter->Diagnostic.WorkspaceSize = UFS_WORKSPACE_SIZE;
#if UFS_DMA_WINDOW_ENFORCE
    Adapter->Diagnostic.WorkspaceBounded =
        Adapter->WorkspaceBounded ? 1UL : 0UL;
    Adapter->Diagnostic.DmaWindowFirst = UFS_DMA_WINDOW_FIRST;
    Adapter->Diagnostic.DmaWindowLast = UFS_DMA_WINDOW_LAST;
#endif

    Cursor = UfsAlignUp((ULONG_PTR)Adapter->Workspace, UFS_UTRL_ALIGNMENT);
    Adapter->Utrl = (PUFS_TRANSFER_REQUEST_DESCRIPTOR)Cursor;
    Cursor += UFS_UTRL_REGION_SIZE;

    Cursor = UfsAlignUp(Cursor, UFS_UCD_ALIGNMENT);
    Adapter->Ucd = (PUCHAR)Cursor;
    Cursor += UFS_UCD_REGION_SIZE;

    Cursor = UfsAlignUp(Cursor, UFS_BOUNCE_ALIGNMENT);
    Adapter->Bounce = (PUCHAR)Cursor;
    Cursor += UFS_BOUNCE_SIZE;

    WorkspaceEnd = (ULONG_PTR)Adapter->Workspace + UFS_WORKSPACE_SIZE;
    if (Cursor > WorkspaceEnd) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_WORKSPACE_LAYOUT);
        return FALSE;
    }

    UtrlValid = UfsGetWorkspacePhysicalAddress(
        Adapter,
        Adapter->Utrl,
        UFS_UTRL_REGION_SIZE,
        UFS_UTRL_ALIGNMENT,
        &Adapter->UtrlPhysical
        );
    UcdValid = UfsGetWorkspacePhysicalAddress(
        Adapter,
        Adapter->Ucd,
        UFS_UCD_REGION_SIZE,
        UFS_UCD_ALIGNMENT,
        &Adapter->UcdPhysical
        );
    BounceValid = UfsGetWorkspacePhysicalAddress(
        Adapter,
        Adapter->Bounce,
        UFS_BOUNCE_SIZE,
        UFS_BOUNCE_ALIGNMENT,
        &Adapter->BouncePhysical
        );

    Adapter->Diagnostic.UtrlVirtual =
        (ULONGLONG)(ULONG_PTR)Adapter->Utrl;
    Adapter->Diagnostic.UtrlPhysical =
        (ULONGLONG)Adapter->UtrlPhysical.QuadPart;
    Adapter->Diagnostic.UcdVirtual =
        (ULONGLONG)(ULONG_PTR)Adapter->Ucd;
    Adapter->Diagnostic.UcdPhysical =
        (ULONGLONG)Adapter->UcdPhysical.QuadPart;
    Adapter->Diagnostic.BounceVirtual =
        (ULONGLONG)(ULONG_PTR)Adapter->Bounce;
    Adapter->Diagnostic.BouncePhysical =
        (ULONGLONG)Adapter->BouncePhysical.QuadPart;

    if (!UtrlValid) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_UTRL_PHYSICAL);
    }
    if (!UcdValid) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_UCD_PHYSICAL);
    }
    if (!BounceValid) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_BOUNCE_PHYSICAL);
    }
    if (!UtrlValid || !UcdValid || !BounceValid) {
        return FALSE;
    }

    UfsZeroUncached(Adapter->Workspace, UFS_WORKSPACE_SIZE);
    return TRUE;
}

static
BOOLEAN
UfsValidateWarmState(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    ULONG HostEnable;
    ULONG Doorbell;
    ULONG HciVersion;
    ULONGLONG Failures = 0;

    HostEnable = UfsReadRegister(Adapter, UFS_REG_HOST_ENABLE);
    Adapter->Capabilities = UfsReadRegister(Adapter, UFS_REG_CAP);
    Adapter->InitialHostStatus = UfsReadRegister(Adapter, UFS_REG_HOST_STATUS);
    Adapter->InitialInterruptStatus = UfsReadRegister(Adapter, UFS_REG_INTERRUPT_STATUS);
    Adapter->InitialInterruptEnable = UfsReadRegister(Adapter, UFS_REG_INTERRUPT_ENABLE);
    Adapter->InitialInterruptAggregation = UfsReadRegister(Adapter, UFS_REG_INTERRUPT_AGGREGATION);
    Adapter->InitialUtrlBaseLow = UfsReadRegister(Adapter, UFS_REG_UTRL_BASE_LOW);
    Adapter->InitialUtrlBaseHigh = UfsReadRegister(Adapter, UFS_REG_UTRL_BASE_HIGH);
    Adapter->InitialUtrlRunStop = UfsReadRegister(Adapter, UFS_REG_UTRL_RUN_STOP);
    Adapter->InitialNexusType = UfsReadRegister(Adapter, UFS_VENDOR_NEXUS_TYPE);
    Doorbell = UfsReadRegister(Adapter, UFS_REG_UTRL_DOORBELL);
    Adapter->InitialTxPrdtSize = UfsReadRegister(Adapter, UFS_VENDOR_TX_PRDT_SIZE);
    Adapter->InitialRxPrdtSize = UfsReadRegister(Adapter, UFS_VENDOR_RX_PRDT_SIZE);
    Adapter->InitialDataReorder = UfsReadRegister(Adapter, UFS_VENDOR_DATA_REORDER);
    Adapter->InitialAxiDmaBurst = UfsReadRegister(Adapter, UFS_VENDOR_AXI_DMA_BURST);

    Adapter->Diagnostic.HostEnable = HostEnable;
    Adapter->Diagnostic.Capabilities = Adapter->Capabilities;
    Adapter->Diagnostic.HostStatus = Adapter->InitialHostStatus;
    Adapter->Diagnostic.InterruptStatus = Adapter->InitialInterruptStatus;
    Adapter->Diagnostic.InterruptEnable = Adapter->InitialInterruptEnable;
    Adapter->Diagnostic.InterruptAggregation =
        Adapter->InitialInterruptAggregation;
    Adapter->Diagnostic.Doorbell = Doorbell;
    Adapter->Diagnostic.UtrlBaseLow = Adapter->InitialUtrlBaseLow;
    Adapter->Diagnostic.UtrlBaseHigh = Adapter->InitialUtrlBaseHigh;
    Adapter->Diagnostic.UtrlRunStop = Adapter->InitialUtrlRunStop;
    Adapter->Diagnostic.NexusType = Adapter->InitialNexusType;
    Adapter->Diagnostic.TxPrdtSize = Adapter->InitialTxPrdtSize;
    Adapter->Diagnostic.RxPrdtSize = Adapter->InitialRxPrdtSize;
    Adapter->Diagnostic.DataReorder = Adapter->InitialDataReorder;
    Adapter->Diagnostic.AxiDmaBurst = Adapter->InitialAxiDmaBurst;

    if ((HostEnable & UFS_HOST_ENABLE) == 0) {
        Failures |= UFS_DIAG_FAILURE_HOST_DISABLED;
    }
    // The 9810 treated any HCI version other than 2.1 as fatal. This SoC's
    // device tree declares `hw-rev = UFS_VER_0006` (Samsung's ufs.h: 4, 5, 6),
    // a newer revision class, and the HCI encoding that class implies has never
    // been read on Windows. Refusing storage on a constant we have not measured
    // is exactly the failure mode the port's bar protects ("Windows sees disk"),
    // so an unexpected value is REPORTED and left to the remaining checks below,
    // which still have to agree before the adapter is considered warm.
    HciVersion = UfsReadRegister(Adapter, UFS_REG_VERSION);
    if (HciVersion != UFS_HCI_VERSION_21) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_HCI_VERSION_UNCONFIRMED);
    }
    if ((Adapter->InitialHostStatus & UFS_HOST_STATUS_WARM_REQUIRED) !=
        UFS_HOST_STATUS_WARM_REQUIRED) {
        Failures |= UFS_DIAG_FAILURE_HOST_STATUS;
    }
    if (Doorbell != 0) {
        Failures |= UFS_DIAG_FAILURE_DOORBELL_BUSY;
    }
    if (Adapter->InitialUtrlRunStop != UFS_LIST_RUN_STOP) {
        Failures |= UFS_DIAG_FAILURE_PROGRAM_PRECONDITION;
    }
    if (Adapter->InitialInterruptAggregation != 0) {
        Failures |= UFS_DIAG_FAILURE_INTERRUPT_AGGREGATION;
    }
    if ((Adapter->InitialInterruptEnable & UFS_INTERRUPT_ENABLE_REQUIRED) !=
        UFS_INTERRUPT_ENABLE_REQUIRED) {
        Failures |= UFS_DIAG_FAILURE_INTERRUPT_ENABLE;
    }
    if ((Adapter->Capabilities & UFS_CAP_NUTRS_MASK) == 0) {
        Failures |= UFS_DIAG_FAILURE_CAPABILITIES;
    }
    if ((Adapter->InitialTxPrdtSize & 0x800000FFUL) != 0x8000000CUL) {
        Failures |= UFS_DIAG_FAILURE_TX_PRDT_SIZE;
    }
    if ((Adapter->InitialRxPrdtSize & 0x000000FFUL) != 0x0000000CUL) {
        Failures |= UFS_DIAG_FAILURE_RX_PRDT_SIZE;
    }
    if (Adapter->InitialDataReorder != 0x0000000AUL) {
        Failures |= UFS_DIAG_FAILURE_DATA_REORDER;
    }
    if ((Adapter->InitialAxiDmaBurst & 0x80000000UL) == 0) {
        Failures |= UFS_DIAG_FAILURE_AXI_DMA_BURST;
    }

    if ((((ULONG_PTR)Adapter->Ucd & (sizeof(ULONG) - 1U)) != 0)) {
        Failures |= UFS_DIAG_FAILURE_UCD_VIRTUAL_ALIGNMENT;
    }
    if ((Adapter->UtrlPhysical.HighPart != 0) ||
        (Adapter->UcdPhysical.HighPart != 0) ||
        (Adapter->BouncePhysical.HighPart != 0)) {
        Failures |= UFS_DIAG_FAILURE_DMA_ABOVE_4G;
    }

    if (Failures != 0) {
        UfsSetDiagnosticFailure(Adapter, Failures);
        Adapter->WarmStateValid = FALSE;
        Adapter->Diagnostic.WarmStateValid = 0;
        return FALSE;
    }

    Adapter->WarmStateValid = TRUE;
    Adapter->Diagnostic.WarmStateValid = 1;
    return TRUE;
}

static
BOOLEAN
UfsRestoreWarmHandoff(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    );

static
BOOLEAN
UfsProgramTransferList(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    ULONG LowAddress;
    ULONG ReadbackLow;
    ULONG ReadbackHigh;
    ULONG ExpectedNexus;
    ULONGLONG Failures = 0;

    if (!Adapter->WarmStateValid) {
        UfsSetDiagnosticFailure(
            Adapter,
            UFS_DIAG_FAILURE_PROGRAM_PRECONDITION
            );
        return FALSE;
    }
    if (UfsReadRegister(Adapter, UFS_REG_UTRL_DOORBELL) != 0) {
        UfsSetDiagnosticFailure(
            Adapter,
            UFS_DIAG_FAILURE_PROGRAM_DOORBELL_BUSY
            );
        return FALSE;
    }

    UfsZeroUncached(Adapter->Utrl, UFS_UTRL_REGION_SIZE);
    UfsZeroUncached(Adapter->Ucd, UFS_UCD_REGION_SIZE);
    UfsZeroUncached(Adapter->Bounce, UFS_BOUNCE_SIZE);
    KeMemoryBarrier();

    UfsWriteRegister(Adapter, UFS_REG_INTERRUPT_ENABLE, 0);
    KeMemoryBarrier();
    if (!UfsPollRegister(
            Adapter,
            UFS_REG_INTERRUPT_ENABLE,
            0xFFFFFFFFUL,
            0,
            UFS_STOP_TIMEOUT_US
            )) {
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_INTERRUPT_GATE);
        UfsContainController(Adapter, UFS_DIAG_FAILURE_INTERRUPT_GATE);
        return FALSE;
    }
    Adapter->InterruptsGated = TRUE;
    if (!UfsAcknowledgeInterruptStatus(Adapter)) {
        (VOID)UfsRestoreWarmHandoff(Adapter);
        return FALSE;
    }
    if (!UfsStopTransferList(Adapter)) {
        (VOID)UfsRestoreWarmHandoff(Adapter);
        return FALSE;
    }

    LowAddress = Adapter->UtrlPhysical.LowPart & ~(UFS_UTRL_ALIGNMENT - 1U);
    Adapter->Diagnostic.ProgramBaseLowExpected = LowAddress;
    Adapter->Diagnostic.ProgramBaseHighExpected =
        Adapter->UtrlPhysical.HighPart;
    UfsWriteRegister(Adapter, UFS_REG_UTRL_BASE_LOW, LowAddress);
    UfsWriteRegister(Adapter, UFS_REG_UTRL_BASE_HIGH, 0);
    KeMemoryBarrier();

    ReadbackLow = UfsReadRegister(Adapter, UFS_REG_UTRL_BASE_LOW);
    ReadbackHigh = UfsReadRegister(Adapter, UFS_REG_UTRL_BASE_HIGH);
    Adapter->Diagnostic.ProgramBaseLowReadback = ReadbackLow;
    Adapter->Diagnostic.ProgramBaseHighReadback = ReadbackHigh;

    if (ReadbackLow != LowAddress) {
        Failures |= UFS_DIAG_FAILURE_PROGRAM_BASE_LOW;
    }
    if (ReadbackHigh != 0) {
        Failures |= UFS_DIAG_FAILURE_PROGRAM_BASE_HIGH;
    }
    if (Failures != 0) {
        UfsSetDiagnosticFailure(Adapter, Failures);
        (VOID)UfsRestoreWarmHandoff(Adapter);
        return FALSE;
    }

    if (!UfsStartTransferList(Adapter) ||
        ((UfsReadRegister(Adapter, UFS_REG_HOST_STATUS) &
          UFS_HOST_STATUS_UTRL_READY) == 0) ||
        (UfsReadRegister(Adapter, UFS_REG_UTRL_DOORBELL) != 0)) {
        UfsSetDiagnosticFailure(
            Adapter,
            UFS_DIAG_FAILURE_PROGRAM_RUN_STOP
            );
        (VOID)UfsRestoreWarmHandoff(Adapter);
        return FALSE;
    }
    Adapter->Diagnostic.ProgramRunStopReadback =
        UfsReadRegister(Adapter, UFS_REG_UTRL_RUN_STOP);

    ExpectedNexus = Adapter->InitialNexusType | UFS_TRANSFER_SLOT_MASK;
    UfsWriteRegister(Adapter, UFS_VENDOR_NEXUS_TYPE, ExpectedNexus);
    KeMemoryBarrier();
    if (UfsReadRegister(Adapter, UFS_VENDOR_NEXUS_TYPE) != ExpectedNexus) {
        UfsSetDiagnosticFailure(
            Adapter,
            UFS_DIAG_FAILURE_NEXUS_PROGRAM |
                UFS_DIAG_FAILURE_NEXUS_READBACK
            );
        (VOID)UfsRestoreWarmHandoff(Adapter);
        return FALSE;
    }

    Adapter->OwnsTransferList = TRUE;
    return TRUE;
}

static
BOOLEAN
UfsRestoreWarmHandoff(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    BOOLEAN Restored;

    Restored = FALSE;
    if (UfsReadRegister(Adapter, UFS_REG_UTRL_DOORBELL) != 0) {
        goto Exit;
    }
    if (!UfsStopTransferList(Adapter) ||
        !UfsAcknowledgeInterruptStatus(Adapter)) {
        goto Exit;
    }

    UfsWriteRegister(
        Adapter,
        UFS_VENDOR_NEXUS_TYPE,
        Adapter->InitialNexusType
        );
    UfsWriteRegister(
        Adapter,
        UFS_REG_UTRL_BASE_LOW,
        Adapter->InitialUtrlBaseLow
        );
    UfsWriteRegister(
        Adapter,
        UFS_REG_UTRL_BASE_HIGH,
        Adapter->InitialUtrlBaseHigh
        );
    KeMemoryBarrier();

    if ((UfsReadRegister(Adapter, UFS_VENDOR_NEXUS_TYPE) !=
         Adapter->InitialNexusType) ||
        (UfsReadRegister(Adapter, UFS_REG_UTRL_BASE_LOW) !=
         Adapter->InitialUtrlBaseLow) ||
        (UfsReadRegister(Adapter, UFS_REG_UTRL_BASE_HIGH) !=
         Adapter->InitialUtrlBaseHigh)) {
        goto Exit;
    }

    if (Adapter->InitialUtrlRunStop == UFS_LIST_RUN_STOP) {
        if (!UfsStartTransferList(Adapter)) {
            goto Exit;
        }
    }

    UfsWriteRegister(
        Adapter,
        UFS_REG_INTERRUPT_ENABLE,
        Adapter->InitialInterruptEnable
        );
    KeMemoryBarrier();
    if (!UfsPollRegister(
            Adapter,
            UFS_REG_INTERRUPT_ENABLE,
            0xFFFFFFFFUL,
            Adapter->InitialInterruptEnable,
            UFS_STOP_TIMEOUT_US
            ) ||
        (UfsReadRegister(Adapter, UFS_REG_UTRL_RUN_STOP) !=
         Adapter->InitialUtrlRunStop) ||
        (UfsReadRegister(Adapter, UFS_REG_UTRL_DOORBELL) != 0)) {
        goto Exit;
    }

    Adapter->OwnsTransferList = FALSE;
    Adapter->InterruptsGated = FALSE;
    Restored = TRUE;

Exit:
    if (!Restored) {
        UfsContainController(Adapter, UFS_DIAG_FAILURE_RESTORE);
    }
    return Restored;
}

/*
 * Bound a data-in command's transfer against the buffer it was given.
 *
 * This used to also require DataLength <= AllocationLength, which inverts SCSI
 * semantics. SPC says the target transfers min(AllocationLength, available);
 * the initiator's buffer is free to be LARGER than the CDB allocation length,
 * and Windows issues INQUIRY exactly that way when storport collects the VPD
 * pages it builds StorageAccessAlignmentProperty from. Refusing that legal
 * shape is why 0x12 INQUIRY appeared in the refused-opcode log while a plain
 * INQUIRY demonstrably succeeds, and why QUERY_PROPERTY/alignment was the one
 * surviving ERROR_INVALID_FUNCTION after the CHECK CONDITION change.
 *
 * Removing it cannot widen what the device sees:
 *   - AllocationLength has no other consumer in this driver. It is never
 *     written into a descriptor.
 *   - The Command UPIU's ExpectedDataTransferLength and the PRDT are both
 *     sized from DataLength, so DataLength remains the hard DMA bound.
 *   - Read-only enforcement lives in the UfsClassifyCdb allowlist, not here;
 *     this validator is only reachable from opcodes already admitted, all of
 *     which are data-in. No write opcode routes through it.
 *
 * AllocationLength is still bounded by the controller ceiling so a nonsense
 * CDB field is refused rather than forwarded.
 */
static
BOOLEAN
UfsValidateAllocationLength(
    _In_ ULONG DataLength,
    _In_ ULONG AllocationLength,
    _In_ ULONG MinimumLength
    )
{
    return (DataLength >= MinimumLength) &&
           (DataLength <= UFS_MAX_TRANSFER_LENGTH) &&
           (AllocationLength <= UFS_MAX_TRANSFER_LENGTH);
}

//
// The logical block size is whatever LU 0 actually reports. Real UFS units use
// 4096, but a 512-byte unit is equally legal and must not disable the adapter.
//
static
BOOLEAN
UfsIsSupportedBlockSize(
    _In_ ULONG BlockSize
    )
{
    return (BlockSize >= UFS_MIN_LOGICAL_BLOCK_SIZE) &&
           (BlockSize <= UFS_MAX_LOGICAL_BLOCK_SIZE) &&
           ((BlockSize & (BlockSize - 1UL)) == 0UL);
}

static
UCHAR
UfsCapacityFailureStatus(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    if (Adapter->CapacityRetryAttempts < UFS_MAX_CAPACITY_RETRY_REQUESTS) {
        Adapter->CapacityRetryAttempts++;
        Adapter->CapacityRetryRequests++;
        return SRB_STATUS_BUSY;
    }

    Adapter->CapacityRetriesExhausted++;
    return SRB_STATUS_ERROR;
}

static
BOOLEAN
UfsValidateReadTransfer(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _In_ ULONGLONG LogicalBlock,
    _In_ ULONG BlockCount
    )
{
    ULONGLONG LastBlock;
    ULONG ExpectedLength;
    ULONG BlockSize = Adapter->LogicalBlockSize;

    if (!Adapter->CapacityValid ||
        !UfsIsSupportedBlockSize(BlockSize) ||
        (BlockCount == 0) ||
        (BlockCount > (UFS_MAX_TRANSFER_LENGTH / BlockSize))) {
        return FALSE;
    }

    ExpectedLength = BlockCount * BlockSize;
    if (Srb->DataTransferLength != ExpectedLength) {
        return FALSE;
    }

    LastBlock = LogicalBlock + BlockCount - 1ULL;
    if ((LastBlock < LogicalBlock) ||
        (LastBlock > Adapter->LastLogicalBlock)) {
        return FALSE;
    }

    return TRUE;
}

/*
 * The write opcode allowlist, expressed once so the classifier, the data-out
 * admission gate and the telemetry cannot drift apart. Every other write-class
 * opcode - FORMAT UNIT, UNMAP, WRITE SAME, SANITIZE, WRITE BUFFER, MODE SELECT,
 * SECURITY PROTOCOL OUT - is absent on purpose: only these two carry an
 * explicit LBA and block count, which is precisely what makes them fenceable.
 */
static
BOOLEAN
UfsIsWriteOpcode(
    _In_ UCHAR Opcode
    )
{
    return (BOOLEAN)((Opcode == SCSIOP_WRITE) || (Opcode == SCSIOP_WRITE16));
}

static
ULONGLONG
UfsExtractWriteLba(
    _In_reads_bytes_(16) const UCHAR *Cdb
    )
{
    if (Cdb[0] == SCSIOP_WRITE16) {
        return UfsReadBigEndian64(&Cdb[2]);
    }

    return (ULONGLONG)UfsReadBigEndian32(&Cdb[2]);
}

/*
 * Layers 2, 3 and 7 of the write safeguard stack. Deliberately a separate
 * function from the read validator rather than a shared one with a direction
 * flag: the read path is proven and must not acquire new branches, and the
 * extra guards here have no meaning for reads.
 *
 * Ordering matters. Geometry is checked first so LastBlock is computed from
 * values already known to be sane, then the GPT guard, then the fence. The GPT
 * guard runs before the fence on purpose - it is the check that must survive a
 * wrong fence constant, so it may not depend on one.
 */
static
BOOLEAN
UfsValidateWriteTransfer(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _In_ ULONGLONG LogicalBlock,
    _In_ ULONG BlockCount,
    _Out_ PULONG RejectCode,
    _Out_ PULONGLONG FailureBit
    )
{
    ULONGLONG LastBlock;
    ULONG ExpectedLength;
    ULONG BlockSize = Adapter->LogicalBlockSize;

    *RejectCode = UFS_EXEC_REJECT_CLASSIFY;
    *FailureBit = UFS_DIAG_FAILURE_TRANSFER_LENGTH;

    if (!Adapter->CapacityValid ||
        !UfsIsSupportedBlockSize(BlockSize) ||
        (BlockCount == 0) ||
        (BlockCount > UFS_WRITE_MAX_BLOCKS) ||
        (BlockCount > (UFS_MAX_TRANSFER_LENGTH / BlockSize))) {
        return FALSE;
    }

    ExpectedLength = BlockCount * BlockSize;
    if ((Srb->DataTransferLength != ExpectedLength) ||
        (Srb->DataBuffer == NULL)) {
        return FALSE;
    }

    LastBlock = LogicalBlock + BlockCount - 1ULL;
    if ((LastBlock < LogicalBlock) ||
        (LastBlock > Adapter->LastLogicalBlock)) {
        return FALSE;
    }

    /*
     * GPT guard. Independent of the fence by construction - it is stated in
     * terms of the partition table's own geometry, so it still holds if the
     * fence bounds are ever edited incorrectly.
     */
    if ((LogicalBlock <= UFS_GPT_PRIMARY_LAST_LBA) ||
        (LastBlock >= UFS_GPT_BACKUP_FIRST_LBA)) {
        *RejectCode = UFS_EXEC_REJECT_WRITE_FENCE;
        *FailureBit = UFS_DIAG_FAILURE_WRITE_GPT_GUARD;
        return FALSE;
    }

    /*
     * Protected-partition guard. Like the GPT guard above and for the same
     * reason, this is stated in terms of the device's own partition layout
     * rather than in terms of the fence, so it still holds if a fence constant
     * is ever edited incorrectly - and it deliberately runs BEFORE the fence so
     * that a request landing on Samsung data is refused by a check that does
     * not depend on the fence being right.
     *
     * The fence spans from sda18 all the way to the GPT's LastUsableLBA so that
     * the unallocated tail past the last partition is reachable. Everything
     * real in between - sda19 VENDOR through sda25 USERDATA - is carved back
     * out here. Any overlap at all is refused, not merely full containment:
     * a write that clips one block of USERDATA is exactly as unacceptable as
     * one that lands squarely in it.
     */
    if ((LogicalBlock <= UFS_PROTECTED_PARTITION_LAST_LBA) &&
        (LastBlock >= UFS_PROTECTED_PARTITION_FIRST_LBA)) {
        *RejectCode = UFS_EXEC_REJECT_WRITE_FENCE;
        *FailureBit = UFS_DIAG_FAILURE_WRITE_PROTECTED_PARTITION;
        return FALSE;
    }

#if UFS_FULL_WINDOWS_MODE
    /*
     * Second range, carrying the same overlap semantics as the first. It exists
     * because sda21 the ESP sits between the two groups and has to be writable;
     * splitting the guard is what keeps sda22..sda24 refused instead of letting
     * them through as a side effect of reaching sda25.
     */
    if ((LogicalBlock <= UFS_PROTECTED_PARTITION2_LAST_LBA) &&
        (LastBlock >= UFS_PROTECTED_PARTITION2_FIRST_LBA)) {
        *RejectCode = UFS_EXEC_REJECT_WRITE_FENCE;
        *FailureBit = UFS_DIAG_FAILURE_WRITE_PROTECTED_PARTITION;
        return FALSE;
    }
#endif

    /* Fence. The whole extent must be inside, not merely overlap it. */
    if ((LogicalBlock < UFS_WRITE_FENCE_FIRST_LBA) ||
        (LastBlock > UFS_WRITE_FENCE_LAST_LBA)) {
        *RejectCode = UFS_EXEC_REJECT_WRITE_FENCE;
        *FailureBit = UFS_DIAG_FAILURE_WRITE_FENCE;
        return FALSE;
    }

    *RejectCode = UFS_EXEC_REJECT_NONE;
    *FailureBit = 0ULL;
    return TRUE;
}

static
BOOLEAN
UfsClassifyCdb(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _Out_ UFS_COMMAND_DIRECTION *Direction,
    _Out_ PBOOLEAN CompleteLocally,
    _Out_ PULONG RejectCode,
    _Out_ PULONGLONG FailureBit
    )
{
    const UCHAR *Cdb;
    ULONG AllocationLength;
    ULONG BlockCount;
    ULONGLONG LogicalBlock;

    Cdb = Srb->Cdb;
    *Direction = UfsCommandDataIn;
    *CompleteLocally = FALSE;
    /*
     * Defaults describe the read path, which has no write-specific reason to
     * fail; the write cases overwrite them with the exact guard that refused.
     */
    *RejectCode = UFS_EXEC_REJECT_CLASSIFY;
    *FailureBit = 0ULL;

    switch (Cdb[0]) {
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_MEDIUM_REMOVAL:
        *Direction = UfsCommandNoData;
        *CompleteLocally = TRUE;
        return (Srb->CdbLength == 6) &&
               (Srb->DataTransferLength == 0);

    case SCSIOP_SYNCHRONIZE_CACHE:
        *Direction = UfsCommandNoData;
        *CompleteLocally = FALSE;
        return (Srb->CdbLength == 10) &&
               (Srb->DataTransferLength == 0);

    case SCSIOP_SYNCHRONIZE_CACHE16:
        *Direction = UfsCommandNoData;
        *CompleteLocally = FALSE;
        return (Srb->CdbLength == 16) &&
               (Srb->DataTransferLength == 0);

    case SCSIOP_REQUEST_SENSE:
        return (Srb->CdbLength == 6) &&
               UfsValidateAllocationLength(
                   Srb->DataTransferLength,
                   Cdb[4],
                   1
                   );

    case SCSIOP_INQUIRY:
        AllocationLength = UfsReadBigEndian16(Cdb + 3);
        return (Srb->CdbLength == 6) &&
               UfsValidateAllocationLength(
                   Srb->DataTransferLength,
                   AllocationLength,
                   1
                   );

    case SCSIOP_MODE_SENSE:
        return (Srb->CdbLength == 6) &&
               UfsValidateAllocationLength(
                   Srb->DataTransferLength,
                   Cdb[4],
                   sizeof(MODE_PARAMETER_HEADER)
                   );

    case SCSIOP_READ6:
        if (Srb->CdbLength != 6) {
            return FALSE;
        }
        LogicalBlock = ((ULONGLONG)(Cdb[1] & 0x1FU) << 16) |
                       ((ULONGLONG)Cdb[2] << 8) |
                       Cdb[3];
        BlockCount = (Cdb[4] == 0) ? 256UL : Cdb[4];
        return UfsValidateReadTransfer(
            Adapter,
            Srb,
            LogicalBlock,
            BlockCount
            );

    case SCSIOP_READ_CAPACITY:
        return (Srb->CdbLength == 10) &&
               (Srb->DataTransferLength == UFS_READ_CAPACITY10_LENGTH);

    case SCSIOP_READ:
        if (Srb->CdbLength != 10) {
            return FALSE;
        }
        LogicalBlock = UfsReadBigEndian32(Cdb + 2);
        BlockCount = UfsReadBigEndian16(Cdb + 7);
        return UfsValidateReadTransfer(
            Adapter,
            Srb,
            LogicalBlock,
            BlockCount
            );

    case SCSIOP_MODE_SENSE10:
        AllocationLength = UfsReadBigEndian16(Cdb + 7);
        return (Srb->CdbLength == 10) &&
               UfsValidateAllocationLength(
                   Srb->DataTransferLength,
                   AllocationLength,
                   sizeof(MODE_PARAMETER_HEADER10)
                   );

    case SCSIOP_REPORT_LUNS:
        AllocationLength = UfsReadBigEndian32(Cdb + 6);
        return (Srb->CdbLength == 12) &&
               UfsValidateAllocationLength(
                   Srb->DataTransferLength,
                   AllocationLength,
                   8
                   );

    case SCSIOP_READ12:
        if (Srb->CdbLength != 12) {
            return FALSE;
        }
        LogicalBlock = UfsReadBigEndian32(Cdb + 2);
        BlockCount = UfsReadBigEndian32(Cdb + 6);
        return UfsValidateReadTransfer(
            Adapter,
            Srb,
            LogicalBlock,
            BlockCount
            );

    case SCSIOP_READ16:
        if (Srb->CdbLength != 16) {
            return FALSE;
        }
        LogicalBlock = UfsReadBigEndian64(Cdb + 2);
        BlockCount = UfsReadBigEndian32(Cdb + 10);
        return UfsValidateReadTransfer(
            Adapter,
            Srb,
            LogicalBlock,
            BlockCount
            );

    case SCSIOP_SERVICE_ACTION_IN16:
        AllocationLength = UfsReadBigEndian32(Cdb + 10);
        return (Srb->CdbLength == 16) &&
               ((Cdb[1] & 0x1FU) ==
                UFS_SERVICE_ACTION_READ_CAPACITY_16) &&
               (AllocationLength >= UFS_READ_CAPACITY16_LENGTH) &&
               (AllocationLength <= UFS_MAX_TRANSFER_LENGTH) &&
               (Srb->DataTransferLength == AllocationLength);

    /*
     * The only two write opcodes this driver will ever admit.
     *
     * Both carry an explicit LBA and an explicit block count, which is exactly
     * what makes them fenceable. Everything else that can destroy data is left
     * to the default reject below, and the danger is not proportional to CDB
     * size: FORMAT UNIT (0x04), UNMAP (0x42), WRITE SAME 10/16 (0x41/0x93),
     * SANITIZE (0x48), MODE SELECT 6/10 (0x15/0x55), SECURITY PROTOCOL OUT
     * (0xB5), and above all WRITE BUFFER (0x3B), which can reflash the UFS
     * device's own firmware and permanently brick the chip.
     */
    case SCSIOP_WRITE:
        if (Srb->CdbLength != 10) {
            return FALSE;
        }
        *Direction = UfsCommandDataOut;
        LogicalBlock = UfsReadBigEndian32(Cdb + 2);
        BlockCount = UfsReadBigEndian16(Cdb + 7);
        return UfsValidateWriteTransfer(
            Adapter,
            Srb,
            LogicalBlock,
            BlockCount,
            RejectCode,
            FailureBit
            );

    case SCSIOP_WRITE16:
        if (Srb->CdbLength != 16) {
            return FALSE;
        }
        *Direction = UfsCommandDataOut;
        LogicalBlock = UfsReadBigEndian64(Cdb + 2);
        BlockCount = UfsReadBigEndian32(Cdb + 10);
        return UfsValidateWriteTransfer(
            Adapter,
            Srb,
            LogicalBlock,
            BlockCount,
            RejectCode,
            FailureBit
            );

    default:
        return FALSE;
    }
}

/*
 * Remember a refused opcode, keeping the first UFS_REJECTED_OPCODE_SLOTS
 * DISTINCT ones in first-seen order.
 *
 * First-seen order is the point: it reproduces the sequence Windows probes a
 * new disk with, which a bitmap could not convey. Distinctness keeps a single
 * opcode that classpnp retries from filling every slot.
 */
static
VOID
UfsRecordRejectedOpcode(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ UCHAR Opcode
    )
{
    ULONG Index;

    for (Index = 0; Index < Adapter->Diagnostic.RejectedOpcodeCount; Index++) {
        if (Adapter->Diagnostic.RejectedOpcodes[Index] == Opcode) {
            return;
        }
    }

    if (Adapter->Diagnostic.RejectedOpcodeCount >= UFS_REJECTED_OPCODE_SLOTS) {
        Adapter->Diagnostic.RejectedOpcodeOverflow++;
        return;
    }

    Adapter->Diagnostic.RejectedOpcodes[Adapter->Diagnostic.RejectedOpcodeCount] =
        Opcode;
    Adapter->Diagnostic.RejectedOpcodeCount++;
}

/*
 * Capture the operands of the first refused NON-WRITE request.
 *
 * See UFS_DIAGNOSTIC_DATA::FirstRejectContext for the packing and for why
 * writes are excluded. Assembled in a naturally aligned stack local and stored
 * once, so the packed diagnostic struct never sees a partial wide write.
 */
static
VOID
UfsRecordRejectedCdb(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PSCSI_REQUEST_BLOCK Srb
    )
{
    ULONGLONG Context;

    if (Adapter->Diagnostic.FirstRejectContext != 0ULL) {
        return;
    }

    if (UfsIsWriteOpcode(Srb->Cdb[0])) {
        return;
    }

    Context  = (ULONGLONG)Srb->Cdb[0];
    Context |= ((ULONGLONG)((Srb->CdbLength > 1) ? Srb->Cdb[1] : 0)) << 8;
    Context |= ((ULONGLONG)((Srb->CdbLength > 2) ? Srb->Cdb[2] : 0)) << 16;
    Context |= ((ULONGLONG)Srb->CdbLength) << 24;
    Context |= ((ULONGLONG)Srb->DataTransferLength) << 32;

    Adapter->Diagnostic.FirstRejectContext = Context;
}

/*
 * Answer a refused CDB the way a SCSI target does, rather than the way a broken
 * HBA does.
 *
 * SRB_STATUS_INVALID_REQUEST means "this adapter cannot express that request".
 * classpnp maps it to STATUS_INVALID_DEVICE_REQUEST, which surfaces as Win32
 * ERROR_INVALID_FUNCTION and aborts the caller outright - which is exactly what
 * IOCTL_DISK_GET_DRIVE_LAYOUT_EX, GET_PARTITION_INFO_EX and the alignment
 * property query were all reporting while the media itself read back perfectly.
 * That is why the disk enumerated with correct geometry and still produced no
 * partitions: partmgr attached, asked, was told the adapter was broken, and
 * gave up.
 *
 * A real target answers an opcode it does not implement with CHECK CONDITION /
 * ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE, and the storage stack is
 * built to read that as "feature unsupported, carry on". A refused write gets
 * DATA PROTECT / WRITE PROTECTED, which is the honest answer for a read-only
 * volume.
 *
 * This changes only what the caller is TOLD. The command is still never built
 * into a UPIU and never reaches the device, so UfsClassifyCdb's allowlist
 * remains the sole arbiter of what the media ever sees - this is strictly more
 * SCSI-conformant, not more permissive.
 */
static
UCHAR
UfsCompleteWithSense(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _Inout_ PSCSI_REQUEST_BLOCK Srb,
    _In_ UCHAR SenseKey,
    _In_ UCHAR AdditionalSenseCode,
    _In_ UCHAR AdditionalSenseCodeQualifier
    )
{
    PUCHAR Sense;

    Adapter->Diagnostic.SyntheticCheckConditions++;

    Srb->ScsiStatus = UFS_SCSI_STATUS_CHECK_CONDITION;
    Srb->DataTransferLength = 0;

    if ((Srb->SenseInfoBuffer == NULL) ||
        (Srb->SenseInfoBufferLength < UFS_FIXED_SENSE_LENGTH) ||
        ((Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE) != 0)) {
        /*
         * No autosense channel. The SCSI status alone still says "the device
         * refused this command", which is what the stack needs to keep going.
         *
         * Flagged because it is the one way this fix can be in place and still
         * behave like the old adapter-level rejection - if partitions still do
         * not appear, bit 48 in FMASK says whether the sense ever reached the
         * caller at all, which is the difference between "the theory is wrong"
         * and "the theory was never tested".
         */
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_SENSE_UNAVAILABLE);
        return SRB_STATUS_ERROR;
    }

    Sense = (PUCHAR)Srb->SenseInfoBuffer;
    RtlZeroMemory(Sense, UFS_FIXED_SENSE_LENGTH);
    Sense[0] = 0x70;                        /* current error, fixed format   */
    Sense[2] = (UCHAR)(SenseKey & 0x0F);
    Sense[7] = 0x0A;                        /* additional sense length       */
    Sense[12] = AdditionalSenseCode;
    Sense[13] = AdditionalSenseCodeQualifier;

    return SRB_STATUS_ERROR | SRB_STATUS_AUTOSENSE_VALID;
}

/*
 * Program the PRDT for a DataLength-byte transfer and return the number of
 * entries written, or 0 if the length cannot be described.
 *
 * One entry per UFS_PRDT_SEGMENT_SIZE bytes, because that is what the
 * controller's FMP data-unit machinery requires - see the measurement recorded
 * at UFS_PRDT_SEGMENT_SIZE for why a single wide entry silently truncates to
 * one segment and then raises HOST_FATAL.
 *
 * Exists so that the command path and the write read-back path cannot drift
 * apart: they previously carried two near-identical copies of this descriptor
 * code, which is exactly the shape of bug that ships in one path and not the
 * other. Every store is a naturally aligned 32-bit write into the uncached
 * workspace - entries are 128 bytes apart so every offset stays 4-byte aligned.
 * See UFS_UPIU_COMMAND_WRITE_HEADER for why that alignment is mandatory.
 *
 * The caller must have zeroed the command descriptor first: the crypto/bypass
 * tail of each entry has to read as zero, and unused entries must stay zero.
 */
static
ULONG
UfsProgramPrdt(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG DataLength
    )
{
    PUCHAR Prdt;
    ULONG Base;
    ULONG Entries;
    ULONG Offset;
    ULONG Segment;

    if ((DataLength == 0) ||
        (DataLength > UFS_MAX_TRANSFER_LENGTH) ||
        (DataLength > UFS_BOUNCE_SIZE) ||
        (DataLength > (UFS_FMP_PRDT_DATA_BYTE_COUNT_MASK + 1UL)) ||
        (Adapter->BouncePhysical.HighPart != 0)) {
        return 0;
    }

    Base = Adapter->BouncePhysical.LowPart & ~(UFS_BOUNCE_ALIGNMENT - 1U);
    Prdt = Adapter->Ucd + UFS_UCD_PRDT_OFFSET;
    Entries = 0;

    for (Offset = 0; Offset < DataLength; Offset += UFS_PRDT_SEGMENT_SIZE) {
        if (Entries >= UFS_PRDT_MAX_ENTRIES) {
            return 0;
        }

        Segment = DataLength - Offset;
        if (Segment > UFS_PRDT_SEGMENT_SIZE) {
            Segment = UFS_PRDT_SEGMENT_SIZE;
        }

        *(volatile ULONG *)(Prdt + (Entries * UFS_PRDT_ENTRY_SIZE) + 0) =
            Base + Offset;
        *(volatile ULONG *)(Prdt + (Entries * UFS_PRDT_ENTRY_SIZE) + 4) = 0;
        *(volatile ULONG *)(Prdt + (Entries * UFS_PRDT_ENTRY_SIZE) + 12) =
            (Segment - 1U) & UFS_FMP_PRDT_DATA_BYTE_COUNT_MASK;
        Entries += 1;
    }

    Adapter->LastPrdtEntries = Entries;
    return Entries;
}

/*
 * Build the CDB that actually goes on the wire.
 *
 * Every opcode is passed through byte-for-byte except MODE SENSE(6), which is
 * rewritten as MODE SENSE(10).
 *
 * UFS logical units do not implement the 6-byte MODE commands - the JEDEC UFS
 * command set carries MODE SELECT(10)/MODE SENSE(10) only. This is measured on
 * this device, not assumed: the V20 failure histogram attributed ALL 20
 * failures of a boot to CHECK_CONDITION and named MODE_SENSE_6 as the first of
 * them, with sense 05/24/00 (ILLEGAL REQUEST, INVALID FIELD IN CDB). Linux
 * carries the identical workaround for every UFS logical unit as
 * scsi_device::use_10_for_ms, set unconditionally in ufshcd_slave_configure.
 *
 * The translation is deliberately unconditional, so completion needs no
 * per-command state to undo it: Srb->Cdb[0] still reads SCSIOP_MODE_SENSE
 * there, and that alone proves the reply carries a 10-byte header. The
 * caller's CDB is never modified - classpnp owns that buffer and reuses it on
 * retry.
 */
static
ULONG
UfsBuildWireCdb(
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _Out_writes_bytes_all_(UFS_WIRE_CDB_LENGTH) PUCHAR WireCdb
    )
{
    ULONG Index;
    ULONG Length;
    ULONG AllocationLength;

    for (Index = 0; Index < UFS_WIRE_CDB_LENGTH; Index++) {
        WireCdb[Index] = 0;
    }

    if ((Srb->Cdb[0] != SCSIOP_MODE_SENSE) || (Srb->CdbLength != 6)) {
        Length = Srb->CdbLength;
        if (Length > UFS_WIRE_CDB_LENGTH) {
            Length = UFS_WIRE_CDB_LENGTH;
        }
        for (Index = 0; Index < Length; Index++) {
            WireCdb[Index] = Srb->Cdb[Index];
        }
        return Length;
    }

    /*
     * Ask for four bytes more than the caller did, because the 10-byte reply
     * spends four extra bytes on its header and the payload would otherwise be
     * truncated by exactly that much.
     *
     * Clamped to DataTransferLength so the device can never be invited to send
     * more than ExpectedDataTransferLength and the PRDT were programmed for.
     * That clamp, not the arithmetic, is what keeps an overlong reply inside
     * the bounce.
     */
    AllocationLength = (ULONG)Srb->Cdb[4] +
                       (ULONG)(sizeof(MODE_PARAMETER_HEADER10) -
                               sizeof(MODE_PARAMETER_HEADER));
    if (AllocationLength > Srb->DataTransferLength) {
        AllocationLength = Srb->DataTransferLength;
    }
    if (AllocationLength > MAXUSHORT) {
        AllocationLength = MAXUSHORT;
    }

    WireCdb[0] = SCSIOP_MODE_SENSE10;
    /* DBD is bit 3 in both forms; reserved bits are deliberately not carried. */
    WireCdb[1] = Srb->Cdb[1] & 0x08U;
    /* Page control and page code share byte 2, subpage code byte 3. */
    WireCdb[2] = Srb->Cdb[2];
    WireCdb[3] = Srb->Cdb[3];
    WireCdb[7] = (UCHAR)((AllocationLength >> 8) & 0xFFU);
    WireCdb[8] = (UCHAR)(AllocationLength & 0xFFU);
    WireCdb[9] = Srb->Cdb[5];

    return 10;
}

static
BOOLEAN
UfsBuildCommand(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _In_ UFS_COMMAND_DIRECTION Direction
    )
{
    PUFS_TRANSFER_REQUEST_DESCRIPTOR Trd;
    PUFS_COMMAND_UPIU Command;
    ULONG DataLength;
    ULONG Entries;

    UfsZeroUncached(Adapter->Utrl, UFS_UTRL_REGION_SIZE);
    UfsZeroUncached(Adapter->Ucd, UFS_UCD_REGION_SIZE);

    DataLength = Srb->DataTransferLength;
    if (((Direction != UfsCommandNoData) &&
         (Direction != UfsCommandDataIn) &&
         (Direction != UfsCommandDataOut)) ||
        ((Direction == UfsCommandNoData) && (DataLength != 0)) ||
        ((Direction != UfsCommandNoData) &&
         ((DataLength == 0) ||
          (DataLength > UFS_MAX_TRANSFER_LENGTH) ||
          (DataLength > UFS_BOUNCE_SIZE) ||
          (DataLength > (UFS_FMP_PRDT_DATA_BYTE_COUNT_MASK + 1UL)))) ||
        (((ULONG_PTR)Adapter->Ucd & (sizeof(ULONG) - 1U)) != 0) ||
        (Adapter->UcdPhysical.HighPart != 0) ||
        (Adapter->BouncePhysical.HighPart != 0)) {
        return FALSE;
    }

    /*
     * A read leaves the bounce zeroed so a DMA that never lands is visible as
     * zeros rather than as stale data; a write instead stages the caller's
     * payload, and must therefore fill the buffer completely. Zeroing first
     * would only give a window in which a partially staged buffer could be
     * submitted, so the copy replaces the zero rather than following it.
     */
    if (Direction == UfsCommandDataOut) {
        if (Srb->DataBuffer == NULL) {
            return FALSE;
        }
#if UFS_PERF_INSTRUMENTATION
        {
            ULONGLONG Start = UfsPerfTick();
            UfsCopyToUncached(Adapter->Bounce, Srb->DataBuffer, DataLength);
            Adapter->PerfBounceOutTicks += UfsPerfTick() - Start;
            Adapter->PerfWriteBytes += DataLength;
            Adapter->PerfWriteCount++;
        }
#else
        UfsCopyToUncached(Adapter->Bounce, Srb->DataBuffer, DataLength);
#endif
    } else if (Direction == UfsCommandDataIn) {
#if UFS_PERF_INSTRUMENTATION
        ULONGLONG Start = UfsPerfTick();
        UfsZeroUncached(Adapter->Bounce, DataLength);
        Adapter->PerfZeroTicks += UfsPerfTick() - Start;
#else
        UfsZeroUncached(Adapter->Bounce, DataLength);
#endif
    }

    if (Direction == UfsCommandNoData) {
        Entries = 0;
        Adapter->LastPrdtEntries = 0;
    } else {
        Entries = UfsProgramPrdt(Adapter, DataLength);
        if (Entries == 0) {
            return FALSE;
        }
    }

    Trd = &Adapter->Utrl[UFS_TRANSFER_SLOT];
    Command = (PUFS_COMMAND_UPIU)(Adapter->Ucd + UFS_UCD_COMMAND_OFFSET);

    *(volatile ULONG *)((PUCHAR)Trd + 0) =
        UFS_TRD_COMMAND_TYPE_STORAGE |
        ((Direction == UfsCommandDataOut) ? UFS_TRD_DATA_OUT :
         ((Direction == UfsCommandDataIn) ? UFS_TRD_DATA_IN : 0));
    *(volatile ULONG *)((PUCHAR)Trd + 8) = UFS_TRD_OCS_INITIAL;
    *(volatile ULONG *)((PUCHAR)Trd + 16) =
        Adapter->UcdPhysical.LowPart & ~(UFS_UCD_ALIGNMENT - 1U);
    *(volatile ULONG *)((PUCHAR)Trd + 20) = 0;
    *(volatile ULONG *)((PUCHAR)Trd + 24) = UFS_UTRD_RESPONSE_WORD;
    *(volatile ULONG *)((PUCHAR)Trd + 28) = UFS_UTRD_PRDT_WORD_FOR(Entries);

    /* One naturally aligned 32-bit store - see UFS_UPIU_COMMAND_WRITE_HEADER. */
    *(volatile ULONG *)(void *)Command =
        (Direction == UfsCommandDataOut) ? UFS_UPIU_COMMAND_WRITE_HEADER
        : ((Direction == UfsCommandDataIn) ? UFS_UPIU_COMMAND_READ_HEADER
                                           : UFS_UPIU_COMMAND_NO_DATA_HEADER);
    UfsWriteBigEndian32(Command->ExpectedDataTransferLength, DataLength);
    {
        UCHAR WireCdb[UFS_WIRE_CDB_LENGTH];
        ULONG WireCdbLength;

        WireCdbLength = UfsBuildWireCdb(Srb, WireCdb);
        if (WireCdb[0] != Srb->Cdb[0]) {
            Adapter->ModeSenseTranslated++;
        }
        UfsCopyToUncached(Command->Cdb, WireCdb, WireCdbLength);
    }

    return TRUE;
}

static
BOOLEAN
UfsPollForCompletion(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    ULONG Elapsed;

    for (Elapsed = 0; Elapsed < UFS_TRANSFER_TIMEOUT_US;
         Elapsed += UFS_TRANSFER_POLL_US) {
        if ((UfsReadRegister(Adapter, UFS_REG_UTRL_DOORBELL) &
             UFS_TRANSFER_SLOT_MASK) == 0) {
            KeMemoryBarrier();
            return TRUE;
        }

        /*
         * V33: count the stalls, not the time. Multiplied by
         * UFS_TRANSFER_POLL_US this gives the polling granularity component of
         * the measured device latency, which is what decides whether shrinking
         * the poll interval is worth anything. Counted before the stall so a
         * command that completes on the first look records zero.
         */
#if UFS_PERF_INSTRUMENTATION
        Adapter->PerfPollIterations++;
#endif
        StorPortStallExecution(UFS_TRANSFER_POLL_US);
    }

    return FALSE;
}

/*
 * Layer 6 of the write safeguard stack: read the blocks back and compare.
 *
 * Compiled out of the full-Windows build - see the call site for why - so it is
 * guarded here as well to keep /WX clean of C4505 rather than left as a
 * defined-but-unreferenced static.
 */
#if !UFS_FULL_WINDOWS_MODE
/*
 * Every other layer is preventative - they decide whether a write is allowed to
 * be issued. None of them can detect a write that was allowed, was correct on
 * paper, and still landed somewhere else, which is exactly the failure mode a
 * misprogrammed PRDT or an inherited FMP address window would produce, and
 * exactly the failure mode that would be unrecoverable on this device. Only a
 * read-back can see that, so only a read-back closes the loop.
 *
 * Deliberately self-contained rather than routed through UfsBuildCommand and
 * UfsFinishScsiCommand: this must not perturb the caller's SRB, must not copy
 * anything back into the caller's buffer, and must not touch the proven read
 * path's own accounting. It reuses only the primitives - descriptor layout,
 * nexus, doorbell, poll, IS ack - and reads into the bounce, which the caller
 * is about to zero anyway.
 *
 * Returns TRUE only if the read completed cleanly AND every byte matches.
 */
static
BOOLEAN
UfsVerifyWrittenBlocks(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONGLONG LogicalBlock,
    _In_ ULONG BlockCount,
    _In_reads_bytes_(DataLength) const void *Source,
    _In_ ULONG DataLength
    )
{
    PUFS_TRANSFER_REQUEST_DESCRIPTOR Trd;
    PUFS_COMMAND_UPIU Command;
    const volatile UCHAR *Read;
    const UCHAR *Expected;
    UCHAR Cdb[16];
    ULONG Nexus;
    ULONG Index;
    ULONG Entries;
    UCHAR Ocs;

    if ((DataLength == 0) || (DataLength > UFS_MAX_TRANSFER_LENGTH) ||
        (DataLength > UFS_BOUNCE_SIZE) ||
        (DataLength > (UFS_FMP_PRDT_DATA_BYTE_COUNT_MASK + 1UL)) ||
        (Source == NULL) ||
        (LogicalBlock > 0xFFFFFFFFULL) || (BlockCount > 0xFFFFU) ||
        (Adapter->UcdPhysical.HighPart != 0) ||
        (Adapter->BouncePhysical.HighPart != 0)) {
        return FALSE;
    }

    UfsZeroUncached(Adapter->Utrl, UFS_UTRL_REGION_SIZE);
    UfsZeroUncached(Adapter->Ucd, UFS_UCD_REGION_SIZE);
    UfsZeroUncached(Adapter->Bounce, DataLength);

    Entries = UfsProgramPrdt(Adapter, DataLength);
    if (Entries == 0) {
        return FALSE;
    }

    Trd = &Adapter->Utrl[UFS_TRANSFER_SLOT];
    Command = (PUFS_COMMAND_UPIU)(Adapter->Ucd + UFS_UCD_COMMAND_OFFSET);

    *(volatile ULONG *)((PUCHAR)Trd + 0) =
        UFS_TRD_COMMAND_TYPE_STORAGE | UFS_TRD_DATA_IN;
    *(volatile ULONG *)((PUCHAR)Trd + 8) = UFS_TRD_OCS_INITIAL;
    *(volatile ULONG *)((PUCHAR)Trd + 16) =
        Adapter->UcdPhysical.LowPart & ~(UFS_UCD_ALIGNMENT - 1U);
    *(volatile ULONG *)((PUCHAR)Trd + 20) = 0;
    *(volatile ULONG *)((PUCHAR)Trd + 24) = UFS_UTRD_RESPONSE_WORD;
    *(volatile ULONG *)((PUCHAR)Trd + 28) = UFS_UTRD_PRDT_WORD_FOR(Entries);

    /* One naturally aligned 32-bit store - see UFS_UPIU_COMMAND_WRITE_HEADER. */
    *(volatile ULONG *)(void *)Command = UFS_UPIU_COMMAND_READ_HEADER;
    UfsWriteBigEndian32(Command->ExpectedDataTransferLength, DataLength);

    /*
     * Assemble the READ(10) in ordinary cached memory and copy it across with
     * the same primitive the proven path uses. Writing the CDB bytes straight
     * into the uncached UCD would let the compiler merge adjacent stores into a
     * wide unaligned access, which is precisely the fault that bugchecked
     * V16 and V20-V23.
     */
    RtlZeroMemory(Cdb, sizeof(Cdb));
    Cdb[0] = SCSIOP_READ;
    Cdb[2] = (UCHAR)((LogicalBlock >> 24) & 0xFFU);
    Cdb[3] = (UCHAR)((LogicalBlock >> 16) & 0xFFU);
    Cdb[4] = (UCHAR)((LogicalBlock >> 8) & 0xFFU);
    Cdb[5] = (UCHAR)(LogicalBlock & 0xFFU);
    Cdb[7] = (UCHAR)((BlockCount >> 8) & 0xFFU);
    Cdb[8] = (UCHAR)(BlockCount & 0xFFU);
    UfsCopyToUncached(Command->Cdb, Cdb, sizeof(Cdb));
    KeMemoryBarrier();

    Nexus = Adapter->InitialNexusType | UFS_TRANSFER_SLOT_MASK;
    UfsWriteRegister(Adapter, UFS_VENDOR_NEXUS_TYPE, Nexus);
    KeMemoryBarrier();
    if (UfsReadRegister(Adapter, UFS_VENDOR_NEXUS_TYPE) != Nexus) {
        return FALSE;
    }

    UfsWriteRegister(Adapter, UFS_REG_UTRL_DOORBELL, UFS_TRANSFER_SLOT_MASK);
    KeMemoryBarrier();

    if (!UfsPollForCompletion(Adapter)) {
        return FALSE;
    }
    if (!UfsAcknowledgeInterruptStatus(Adapter)) {
        return FALSE;
    }

    Ocs = (UCHAR)(Trd->Dw2 & 0xFFU);
    if (Ocs != UFS_TRD_OCS_SUCCESS) {
        return FALSE;
    }

    Read = (const volatile UCHAR *)Adapter->Bounce;
    Expected = (const UCHAR *)Source;
    for (Index = 0; Index < DataLength; Index++) {
        if (Read[Index] != Expected[Index]) {
            return FALSE;
        }
    }

    return TRUE;
}
#endif /* !UFS_FULL_WINDOWS_MODE */

/*
 * Freeze the state of the FIRST command that failed after reaching hardware.
 *
 * V14 could only report LAST_OPCODE/LAST_OCS, and by the time the adapter was
 * readable it had already been killed by the sixteenth containment - so the
 * failure that started the cascade had been overwritten many times over. The
 * first failure is the only one that happened while the controller was still
 * in its known-good post-handoff state; everything after it is contaminated by
 * the recovery attempts. Capture it once and never overwrite it.
 */
static
VOID
UfsRecordCommandFailure(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _In_ PUFS_RESPONSE_UPIU Response,
    _In_ UCHAR Ocs,
    _In_ ULONG Residual,
    _In_ ULONG Cause
    )
{
    ULONG Index;

    Adapter->Diagnostic.LastFailureCause = Cause;
    Adapter->Diagnostic.LastResidual = Residual;
    Adapter->Diagnostic.LastResponseStatus = (ULONG)Response->Status;
    Adapter->Diagnostic.LastTransactionCode = (ULONG)Response->TransactionCode;

    switch (Cause) {
    case UFS_FAIL_CAUSE_OCS:
        Adapter->Diagnostic.FramingOcsFailures++;
        break;
    case UFS_FAIL_CAUSE_TRANSACTION_CODE:
        Adapter->Diagnostic.FramingTransactionFailures++;
        break;
    case UFS_FAIL_CAUSE_LUN:
        Adapter->Diagnostic.FramingLunFailures++;
        break;
    case UFS_FAIL_CAUSE_TASK_TAG:
        Adapter->Diagnostic.FramingTagFailures++;
        break;
    case UFS_FAIL_CAUSE_RESPONSE:
        Adapter->Diagnostic.FramingResponseFailures++;
        break;
    case UFS_FAIL_CAUSE_RESIDUAL:
        Adapter->Diagnostic.FramingResidualFailures++;
        break;
    case UFS_FAIL_CAUSE_CHECK_CONDITION:
        Adapter->Diagnostic.CheckConditionFailures++;
        break;
    case UFS_FAIL_CAUSE_SCSI_STATUS:
        Adapter->Diagnostic.ScsiStatusFailures++;
        break;
    case UFS_FAIL_CAUSE_DATA_OVERRUN:
        Adapter->Diagnostic.DataOverrunFailures++;
        break;
    default:
        break;
    }

    if (Adapter->Diagnostic.FirstFailureValid != 0) {
        return;
    }

    Adapter->Diagnostic.FirstFailureValid = 1UL;
    Adapter->Diagnostic.FirstFailureCause = Cause;
    Adapter->Diagnostic.FirstFailureCommandIndex = Adapter->CompletedCommands;
    Adapter->Diagnostic.FirstFailureOpcode = (ULONG)Srb->Cdb[0];
    Adapter->Diagnostic.FirstFailureDataLength = Srb->DataTransferLength;
    Adapter->Diagnostic.FirstFailureResidual = Residual;
    Adapter->Diagnostic.FirstFailureOcs = (ULONG)Ocs;
    Adapter->Diagnostic.FirstFailureTransactionCode =
        (ULONG)Response->TransactionCode;
    Adapter->Diagnostic.FirstFailureLun = (ULONG)Response->Lun;
    Adapter->Diagnostic.FirstFailureTaskTag = (ULONG)Response->TaskTag;
    Adapter->Diagnostic.FirstFailureResponse = (ULONG)Response->Response;
    Adapter->Diagnostic.FirstFailureStatus = (ULONG)Response->Status;

    //
    // Fixed-format sense: key in byte 2, ASC/ASCQ in 12/13. Read bytewise out
    // of the uncached descriptor window - no wide access is legal there.
    //
    if (sizeof(Response->SenseData) > 13) {
        Adapter->Diagnostic.FirstFailureSenseKey =
            (ULONG)(Response->SenseData[2] & 0x0FU);
        Adapter->Diagnostic.FirstFailureAsc = (ULONG)Response->SenseData[12];
        Adapter->Diagnostic.FirstFailureAscq = (ULONG)Response->SenseData[13];
    }

    //
    // Registers as they stood at the first failure. If the controller really is
    // in a fatal state these name it; if they are clean, an OCS of 0x07 is the
    // controller complaining about the descriptor rather than about itself.
    //
    Adapter->Diagnostic.FirstFailureInterruptStatus =
        UfsReadRegister(Adapter, UFS_REG_INTERRUPT_STATUS);
    Adapter->Diagnostic.FirstFailureHostStatus =
        UfsReadRegister(Adapter, UFS_REG_HOST_STATUS);
    Adapter->Diagnostic.FirstFailureDoorbell =
        UfsReadRegister(Adapter, UFS_REG_UTRL_DOORBELL);

    for (Index = 0;
         Index < sizeof(Adapter->Diagnostic.FirstFailureCdb);
         Index += 1) {
        Adapter->Diagnostic.FirstFailureCdb[Index] =
            (Index < sizeof(Srb->Cdb)) ? Srb->Cdb[Index] : 0;
    }
}

/*
 * Snapshot what the controller actually deposited in the bounce.
 *
 * Called from two places, deliberately sharing one implementation:
 *
 *   1. UfsFinishScsiCommand, on the normal success path.
 *   2. The interrupt-status acknowledge-failure path in UfsExecuteScsi.
 *
 * (2) is the reason this is a function rather than inline code. Every "last X"
 * field this driver publishes is written by UfsFinishScsiCommand, which is
 * exactly the function that does NOT run when the acknowledge rejects a
 * command - so a failing multi-block read used to report the PREVIOUS command's
 * DIN/DINLEN and looked like a healthy 4096-byte transfer. Capturing here too
 * answers, in the same flash cycle, whether the DMA landed before the driver
 * rejected it: if NZBLK is non-zero on a failed command then the controller did
 * deliver the data and only the interrupt-status handling is wrong.
 *
 * Every access to the uncached bounce is a plain byte read through a volatile
 * pointer. No wide access is synthesised against the uncached window, which is
 * the V16/V20-V23 unaligned-strh fault class.
 */
static
VOID
UfsCaptureBounceEvidence(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG ActualLength
    )
{
    ULONG Index;
    ULONG PrefixLength;

    if (ActualLength == 0) {
        return;
    }

    //
    // Snapshot the head of the transfer straight out of the bounce, before
    // the copy-back, so the record reflects what the controller delivered
    // rather than anything this driver did afterwards. This is the evidence
    // that separates "the DMA never landed" from "the copy-back is wrong" -
    // V15 and V16 could not tell those apart from the screen alone.
    //
    RtlZeroMemory(Adapter->LastDataIn, sizeof(Adapter->LastDataIn));
    RtlZeroMemory(Adapter->LastDataInBlock1, sizeof(Adapter->LastDataInBlock1));
    Adapter->LastDataInBlockMask = 0;
    PrefixLength = ActualLength;
    if (PrefixLength > UFS_LAST_DATA_PREFIX) {
        PrefixLength = UFS_LAST_DATA_PREFIX;
    }
    Adapter->LastDataInLength = ActualLength;
    Adapter->Diagnostic.LastDataInNonZero = 0;
    for (Index = 0; Index < PrefixLength; Index++) {
        Adapter->LastDataIn[Index] =
            ((volatile UCHAR *)Adapter->Bounce)[Index];
        if (Adapter->LastDataIn[Index] != 0) {
            Adapter->Diagnostic.LastDataInNonZero = 1;
        }
    }

    //
    // Multi-block evidence. DIN above is the head of the bounce and can only
    // ever describe block 0, so on its own it cannot distinguish a transfer
    // that delivered every block from one that delivered the first and
    // silently dropped the rest - and the reported length cannot either.
    //
    // Walk the transfer in 4096-byte granules and set one bit per granule
    // that holds any non-zero byte. The bounce is zeroed before every command
    // (UfsZeroUncached), so a clear bit means that granule was genuinely
    // never written by the controller.
    //
    // Skipped entirely for single-block transfers: they are already fully
    // described by DIN, and the PRAM ring is the scarcest resource in this
    // whole loop.
    //
    if (ActualLength > UFS_MULTI_BLOCK_GRANULE) {
        ULONG Granule;
        ULONG Granules;

        Granules = ActualLength / UFS_MULTI_BLOCK_GRANULE;
        if ((ActualLength % UFS_MULTI_BLOCK_GRANULE) != 0) {
            Granules++;
        }
        if (Granules > UFS_MULTI_BLOCK_MAX_GRANULES) {
            Granules = UFS_MULTI_BLOCK_MAX_GRANULES;
        }

        for (Granule = 0; Granule < Granules; Granule++) {
            ULONG Base;
            ULONG Limit;

            Base = Granule * UFS_MULTI_BLOCK_GRANULE;
            Limit = Base + UFS_MULTI_BLOCK_GRANULE;
            if (Limit > ActualLength) {
                Limit = ActualLength;
            }
            for (Index = Base; Index < Limit; Index++) {
                if (((volatile UCHAR *)Adapter->Bounce)[Index] != 0) {
                    Adapter->LastDataInBlockMask |= (1UL << Granule);
                    break;
                }
            }
        }

        for (Index = 0; Index < UFS_LAST_DATA_PREFIX; Index++) {
            Adapter->LastDataInBlock1[Index] =
                ((volatile UCHAR *)Adapter->Bounce)
                    [UFS_MULTI_BLOCK_GRANULE + Index];
        }
    }
}

/*
 * Is this response UPIU structurally THIS command's answer?
 *
 * Transaction code, LUN and task tag are the three fields that bind a response
 * to its request. If all three agree then whatever status the UPIU carries came
 * from the device in reply to the CDB we sent, regardless of what the
 * controller thinks of the transfer.
 *
 * Response->Response is deliberately NOT part of this test. Linux's
 * ufshcd_transfer_rsp_status() reads MASK_RSP_UPIU_RESULT - the low 16 bits of
 * response DWORD 1, i.e. (Response << 8) | Status - and then acts on
 * `result & MASK_SCSI_STATUS` only, ignoring the Response byte. The observed
 * failing UPIU carries RESPONSE=0x01 alongside valid sense, so testing it would
 * discard the same answer the OCS gate was discarding, four gates later.
 */
static
BOOLEAN
UfsResponseIsWellFormed(
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _In_ PUFS_RESPONSE_UPIU Response
    )
{
    if ((Response->TransactionCode & UFS_UPIU_TRANSACTION_MASK) !=
        UFS_UPIU_TRANSACTION_RESPONSE) {
        return FALSE;
    }

    if (Response->Lun != Srb->Lun) {
        return FALSE;
    }

    if (Response->TaskTag != (UCHAR)UFS_TRANSFER_SLOT) {
        return FALSE;
    }

    return TRUE;
}

/*
 * Hand the device's own non-GOOD SCSI status - and its sense, when there is
 * any - back to the caller.
 *
 * This is the single implementation of that decision. Both the OCS-anomaly
 * branch and the ordinary status branch delegate here, so the two cannot drift
 * apart; one copy of a decision silently disagreeing with another is the exact
 * failure mode this change exists to fix.
 *
 * DataTransferLength is zeroed unconditionally. A non-GOOD status means no data
 * was delivered, UfsCompleteWithSense already zeroes it on the synthetic path,
 * and when the controller has repudiated the transfer the length is meaningless
 * anyway - so this converges the real and synthetic paths rather than adding a
 * new divergence.
 */
static
UCHAR
UfsCompleteWithDeviceStatus(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _Inout_ PSCSI_REQUEST_BLOCK Srb,
    _In_ PUFS_RESPONSE_UPIU Response,
    _In_ UCHAR Ocs
    )
{
    USHORT SenseLength;

    Srb->ScsiStatus = Response->Status;
    Srb->DataTransferLength = 0;

    SenseLength = UfsReadBigEndian16(Response->SenseDataLength);
    if ((Response->Status == UFS_SCSI_STATUS_CHECK_CONDITION) &&
        (SenseLength != 0)) {
        if ((Srb->SenseInfoBuffer == NULL) ||
            (Srb->SenseInfoBufferLength == 0) ||
            ((Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE) != 0)) {
            /*
             * The device explained itself and we have nowhere to put the
             * explanation, so the caller sees a bare error - behaviourally the
             * old adapter-level rejection. Flagged for the same reason bit 48
             * exists on the synthetic path: without it, a boot where partitions
             * still do not appear cannot distinguish "the sense never reached
             * the caller" from "the sense reached the caller and did not help".
             */
            UfsSetDiagnosticFailure(
                Adapter, UFS_DIAG_FAILURE_SENSE_UNAVAILABLE);
        } else {
            ULONG CopyLength = min(
                (ULONG)SenseLength,
                min((ULONG)Srb->SenseInfoBufferLength,
                    (ULONG)sizeof(Response->SenseData))
                );

            UfsCopyFromUncached(
                Srb->SenseInfoBuffer,
                Response->SenseData,
                CopyLength
                );
            UfsRecordCommandFailure(
                Adapter, Srb, Response, Ocs, 0,
                UFS_FAIL_CAUSE_CHECK_CONDITION);
            return SRB_STATUS_ERROR | SRB_STATUS_AUTOSENSE_VALID;
        }
    }

    UfsRecordCommandFailure(
        Adapter, Srb, Response, Ocs, 0, UFS_FAIL_CAUSE_SCSI_STATUS);
    return SRB_STATUS_ERROR;
}

static
UCHAR
UfsFinishScsiCommand(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _Inout_ PSCSI_REQUEST_BLOCK Srb,
    _Out_ PBOOLEAN ProtocolFailure
    )
{
    PUFS_TRANSFER_REQUEST_DESCRIPTOR Trd;
    PUFS_RESPONSE_UPIU Response;
    ULONG ActualLength;
    ULONG Residual;
    UCHAR Ocs;

    *ProtocolFailure = FALSE;
    Trd = &Adapter->Utrl[UFS_TRANSFER_SLOT];
    Response = (PUFS_RESPONSE_UPIU)(Adapter->Ucd + UFS_UCD_RESPONSE_OFFSET);
    Ocs = (UCHAR)(Trd->Dw2 & 0xFFU);
    Adapter->LastOcs = Ocs;
    Adapter->LastTrdDw2 = Trd->Dw2;

    //
    // The device's own SCSI answer outranks the controller's opinion of the
    // transfer, so it is claimed BEFORE any framing gate can discard it.
    //
    // This gate used to be reachable only after the OCS test, which meant a
    // well-formed response carrying real sense was thrown away whenever OCS was
    // non-success. classpnp then had no sense to interpret, treated the failure
    // as an unclassifiable device error, retried, and failed the IRP with
    // STATUS_IO_DEVICE_ERROR - the ERROR_IO_DEVICE 1117 seen on every layout
    // IOCTL, and the reason partmgr produced zero partitions for a disk whose
    // media and GPT both verified perfectly.
    //
    // Ordering also rescues the Response->Response gate below, which trips on
    // the same UPIU (RESPONSE=0x01). Fixing only the OCS test would have moved
    // the loss four gates later and changed nothing observable.
    //
    // Whether OCS=0x07 is real or stale here is unresolved - ACKOCS is already
    // documented as capable of freezing a previous command's value - but it
    // does not change the reading: a response UPIU that names the right
    // transaction code, LUN and task tag IS this command's answer either way.
    //
    // A GOOD status with a non-success OCS is deliberately NOT claimed here. It
    // falls through to the OCS gate, because reporting success on a transfer
    // the controller repudiated would be a data-integrity claim we cannot make.
    //
    // *ProtocolFailure and RESPONSE_FRAMING still fire, so the bit 39 evidence
    // present since V15 survives unchanged; ProtocolFailure is diagnostic-only,
    // the caller UNREFERENCED_PARAMETERs it. Bit 49 marks this branch itself so
    // the next boot can tell "the fix ran and did not help" from "the fix never
    // ran".
    //
    if (UfsResponseIsWellFormed(Srb, Response) &&
        (Response->Status != SCSISTAT_GOOD)) {
        if ((Ocs != UFS_TRD_OCS_SUCCESS) &&
            (Ocs != UFS_TRD_OCS_RESPONSE_SIZE_MISMATCH)) {
            *ProtocolFailure = TRUE;
            UfsSetDiagnosticFailure(
                Adapter, UFS_DIAG_FAILURE_RESPONSE_FRAMING);
            UfsSetDiagnosticFailure(
                Adapter, UFS_DIAG_FAILURE_OCS_WITH_DEVICE_STATUS);
        }

        return UfsCompleteWithDeviceStatus(Adapter, Srb, Response, Ocs);
    }

    //
    // Split what used to be one compound test. Folding five independent
    // conditions into a single RESPONSE_FRAMING bit is why V14 could say a
    // response was malformed but not say which field was wrong, and the answer
    // decides whether the controller, the descriptor, or the device is at
    // fault. Each cause is now counted separately and the first one is frozen.
    //
    if ((Ocs != UFS_TRD_OCS_SUCCESS) &&
        (Ocs != UFS_TRD_OCS_RESPONSE_SIZE_MISMATCH)) {
        *ProtocolFailure = TRUE;
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_RESPONSE_FRAMING);
        UfsRecordCommandFailure(
            Adapter, Srb, Response, Ocs, 0, UFS_FAIL_CAUSE_OCS);
        return SRB_STATUS_ERROR;
    }

    if ((Response->TransactionCode & UFS_UPIU_TRANSACTION_MASK) !=
        UFS_UPIU_TRANSACTION_RESPONSE) {
        *ProtocolFailure = TRUE;
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_RESPONSE_FRAMING);
        UfsRecordCommandFailure(
            Adapter, Srb, Response, Ocs, 0,
            UFS_FAIL_CAUSE_TRANSACTION_CODE);
        return SRB_STATUS_ERROR;
    }

    if (Response->Lun != Srb->Lun) {
        *ProtocolFailure = TRUE;
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_RESPONSE_FRAMING);
        UfsRecordCommandFailure(
            Adapter, Srb, Response, Ocs, 0, UFS_FAIL_CAUSE_LUN);
        return SRB_STATUS_ERROR;
    }

    if (Response->TaskTag != (UCHAR)UFS_TRANSFER_SLOT) {
        *ProtocolFailure = TRUE;
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_RESPONSE_FRAMING);
        UfsRecordCommandFailure(
            Adapter, Srb, Response, Ocs, 0, UFS_FAIL_CAUSE_TASK_TAG);
        return SRB_STATUS_ERROR;
    }

    if (Response->Response != 0) {
        *ProtocolFailure = TRUE;
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_RESPONSE_FRAMING);
        UfsRecordCommandFailure(
            Adapter, Srb, Response, Ocs, 0, UFS_FAIL_CAUSE_RESPONSE);
        return SRB_STATUS_ERROR;
    }

    Srb->ScsiStatus = Response->Status;
    if (Response->Status != SCSISTAT_GOOD) {
        /*
         * Unreachable in practice - the well-formedness branch above claims
         * every non-GOOD status on a response that matches this command, and
         * the framing gates in between reject everything that does not.
         *
         * Kept as a delegation rather than deleted so the two paths cannot
         * diverge if the ordering above is ever changed. Duplicating the
         * sense-copy logic here is precisely what this change removed.
         */
        return UfsCompleteWithDeviceStatus(Adapter, Srb, Response, Ocs);
    }

    ActualLength = Srb->DataTransferLength;
    Residual = UfsReadBigEndian32(Response->ResidualTransferCount);
    if (Residual > ActualLength) {
        *ProtocolFailure = TRUE;
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_RESPONSE_FRAMING);
        UfsRecordCommandFailure(
            Adapter, Srb, Response, Ocs, Residual,
            UFS_FAIL_CAUSE_RESIDUAL);
        return SRB_STATUS_ERROR;
    }
    ActualLength -= Residual;

    if (((Srb->Cdb[0] == SCSIOP_READ6) ||
         (Srb->Cdb[0] == SCSIOP_READ) ||
         (Srb->Cdb[0] == SCSIOP_READ12) ||
         (Srb->Cdb[0] == SCSIOP_READ16)) &&
        (Residual != 0)) {
        UfsRecordCommandFailure(
            Adapter, Srb, Response, Ocs, Residual,
            UFS_FAIL_CAUSE_DATA_OVERRUN);
        return SRB_STATUS_DATA_OVERRUN;
    }

    /*
     * Undo the MODE SENSE(6) -> MODE SENSE(10) translation that UfsBuildWireCdb
     * applied, in place in the bounce, so everything downstream - the
     * write-protect block below, the copy-back, and classpnp itself - sees the
     * 4-byte header the caller's CDB asked for.
     *
     * ModeDataLength counts the bytes FOLLOWING that field, so it counts two
     * fewer than the field's own width suggests: total10 = Length10 + 2 and
     * total6 = Length6 + 1, and the two headers differ by four bytes, hence
     * Length6 = Length10 - 3.
     */
    if (Srb->Cdb[0] == SCSIOP_MODE_SENSE) {
        volatile UCHAR *Header;
        ULONG ModeDataLength;
        ULONG BlockDescriptorLength;
        ULONG Index;

        if (ActualLength < sizeof(MODE_PARAMETER_HEADER10)) {
            /*
             * A reply too short to hold the header it was asked for is a
             * framing violation, not a short read: there is no honest 6-byte
             * header to synthesise from it, and fabricating one would tell
             * classpnp the device answered when it did not.
             */
            *ProtocolFailure = TRUE;
            UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_RESPONSE_FRAMING);
            UfsRecordCommandFailure(
                Adapter, Srb, Response, Ocs, 0, UFS_FAIL_CAUSE_RESPONSE);
            return SRB_STATUS_ERROR;
        }

        Header = (volatile UCHAR *)Adapter->Bounce;
        ModeDataLength = ((ULONG)Header[0] << 8) | (ULONG)Header[1];
        BlockDescriptorLength = ((ULONG)Header[6] << 8) | (ULONG)Header[7];

        ModeDataLength = (ModeDataLength >= 3UL) ? (ModeDataLength - 3UL) : 0UL;
        if (ModeDataLength > MAXUCHAR) {
            ModeDataLength = MAXUCHAR;
        }
        if (BlockDescriptorLength > MAXUCHAR) {
            BlockDescriptorLength = MAXUCHAR;
        }

        /*
         * Ordered so each source byte is read before it is overwritten:
         * MediumType at [2] is consumed before [2] is rewritten, and
         * DeviceSpecificParameter at [3] before [3] is rewritten.
         */
        Header[0] = (UCHAR)ModeDataLength;
        Header[1] = Header[2];
        Header[2] = Header[3];
        Header[3] = (UCHAR)BlockDescriptorLength;

        /*
         * Forward walk: the destination is four bytes BELOW the source, so an
         * ascending copy can never read a byte it has already overwritten.
         */
        for (Index = (ULONG)sizeof(MODE_PARAMETER_HEADER10);
             Index < ActualLength;
             Index++) {
            Header[Index - 4U] = Header[Index];
        }

        ActualLength -= 4U;
    }

    /*
     * Write-protect advertisement, gated on UFS_ADVERTISE_WRITE_PROTECT (see
     * Exynos9820Ufs.h for why it is off). Counting the suppressed replies keeps
     * the change observable: without a counter, "Windows never attempted a
     * write" is indistinguishable on device from "Windows was told it could
     * not", and those two demand opposite next steps.
     */
    if (Adapter->WriteMode == UFS_WRITE_MODE_DISARMED) {
        if (((Srb->Cdb[0] == SCSIOP_MODE_SENSE) &&
             (ActualLength >= sizeof(MODE_PARAMETER_HEADER))) ||
            ((Srb->Cdb[0] == SCSIOP_MODE_SENSE10) &&
             (ActualLength >= sizeof(MODE_PARAMETER_HEADER10)))) {
#if UFS_ADVERTISE_WRITE_PROTECT
            if (Srb->Cdb[0] == SCSIOP_MODE_SENSE) {
                ((PMODE_PARAMETER_HEADER)Adapter->Bounce)->
                    DeviceSpecificParameter |= MODE_DSP_WRITE_PROTECT;
            } else {
                ((PMODE_PARAMETER_HEADER10)Adapter->Bounce)->
                    DeviceSpecificParameter |= MODE_DSP_WRITE_PROTECT;
            }
            Adapter->WriteProtectedResponses++;
#else
            Adapter->WriteProtectSuppressed++;
#endif
        }
    }

    /*
     * Capacity handling. This is the boot-critical parse: Windows derives the
     * volume geometry from it, and NTFS on this device is 4K-native, so a wrong
     * block size does not degrade - it makes the boot volume unmountable
     * (UNMOUNTABLE_BOOT_VOLUME, 0xED) with no other symptom.
     *
     * The previous version had two holes on this queue-depth-1, polled,
     * bounce-copied path, where a partially-landed response is a real failure
     * mode:
     *   - it PUBLISHED the reported block size into LogicalBlockSize BEFORE
     *     validating it, so a rejected command still left the adapter carrying
     *     a bogus geometry, and
     *   - it accepted any power of two in [512, 8192], so a corrupt reply
     *     decoding as 512 was indistinguishable from a real 512-byte device.
     *
     * Four rules close that for this fixed, soldered LU 0:
     *   1. Validate BEFORE publishing. A rejected value never reaches a field
     *      that an I/O path consumes; it is recorded separately instead.
     *   2. Require the known 4096-byte block size and last LBA 15615999. A 512
     *      response is corruption on this phone, not an alternate geometry.
     *   3. Require the entire requested response to land. Any residual on these
     *      tiny boot-critical commands is a partial reply and cannot be parsed.
     *   4. Return SRB_STATUS_BUSY for three invalid replies so Storport issues a
     *      fresh hardware command before the driver finally reports failure.
     */
    if (Srb->Cdb[0] == SCSIOP_READ_CAPACITY) {
        ULONG LastBlock;
        ULONG BlockSize;

        if (ActualLength != Srb->DataTransferLength) {
            Adapter->CapacityShortTransfers++;
            Adapter->LastRejectedBlockSize = 0;
            Adapter->LastRejectedLogicalBlock = 0;
            UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_CAPACITY);
            return UfsCapacityFailureStatus(Adapter);
        }

        LastBlock = UfsReadBigEndian32(Adapter->Bounce);
        BlockSize = UfsReadBigEndian32(Adapter->Bounce + 4);

        if ((BlockSize != UFS_LOGICAL_BLOCK_SIZE) ||
            (LastBlock != (ULONG)UFS_EXPECTED_LAST_LOGICAL_BLOCK)) {
            Adapter->CapacityRejected++;
            Adapter->LastRejectedBlockSize = BlockSize;
            Adapter->LastRejectedLogicalBlock = LastBlock;
            UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_CAPACITY);
            return UfsCapacityFailureStatus(Adapter);
        }
        if (Adapter->CapacityValid &&
            ((Adapter->LogicalBlockSize != BlockSize) ||
             (Adapter->LastLogicalBlock != LastBlock))) {
            Adapter->CapacityMismatches++;
            Adapter->LastRejectedBlockSize = BlockSize;
            Adapter->LastRejectedLogicalBlock = LastBlock;
            UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_CAPACITY);
            return UfsCapacityFailureStatus(Adapter);
        }

        Adapter->LogicalBlockSize = BlockSize;
        Adapter->LastLogicalBlock = LastBlock;
        Adapter->CapacityValid = TRUE;
        Adapter->CapacityRetryAttempts = 0;
        Adapter->LastRejectedBlockSize = 0;
        Adapter->LastRejectedLogicalBlock = 0;
    } else if (Srb->Cdb[0] == SCSIOP_SERVICE_ACTION_IN16) {
        ULONG BlockSize;
        ULONGLONG LastBlock;

        if (ActualLength != Srb->DataTransferLength) {
            Adapter->CapacityShortTransfers++;
            Adapter->LastRejectedBlockSize = 0;
            Adapter->LastRejectedLogicalBlock = 0;
            UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_CAPACITY);
            return UfsCapacityFailureStatus(Adapter);
        }

        LastBlock = UfsReadBigEndian64(Adapter->Bounce);
        BlockSize = UfsReadBigEndian32(Adapter->Bounce + 8);

        if ((BlockSize != UFS_LOGICAL_BLOCK_SIZE) ||
            (LastBlock != UFS_EXPECTED_LAST_LOGICAL_BLOCK)) {
            Adapter->CapacityRejected++;
            Adapter->LastRejectedBlockSize = BlockSize;
            Adapter->LastRejectedLogicalBlock = LastBlock;
            UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_CAPACITY);
            return UfsCapacityFailureStatus(Adapter);
        }
        if (Adapter->CapacityValid &&
            ((Adapter->LogicalBlockSize != BlockSize) ||
             (Adapter->LastLogicalBlock != LastBlock))) {
            Adapter->CapacityMismatches++;
            Adapter->LastRejectedBlockSize = BlockSize;
            Adapter->LastRejectedLogicalBlock = LastBlock;
            UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_CAPACITY);
            return UfsCapacityFailureStatus(Adapter);
        }

        Adapter->LogicalBlockSize = BlockSize;
        Adapter->LastLogicalBlock = LastBlock;
        Adapter->CapacityValid = TRUE;
        Adapter->CapacityRetryAttempts = 0;
        Adapter->LastRejectedBlockSize = 0;
        Adapter->LastRejectedLogicalBlock = 0;
    }

    if (ActualLength != 0) {
        UfsCaptureBounceEvidence(Adapter, ActualLength);

        /*
         * Copy back only for data-in. On a write the bounce holds a copy of the
         * caller's own payload, so copying it back would at best be a no-op and
         * at worst push a corrupted bounce into the caller's buffer - and it
         * would destroy the pristine source that verify-after-write compares
         * against, turning the read-back into a self-fulfilling check.
         */
        if (!UfsIsWriteOpcode(Srb->Cdb[0])) {
#if UFS_PERF_INSTRUMENTATION
            ULONGLONG Start = UfsPerfTick();
#endif

            UfsCopyFromUncached(
                Srb->DataBuffer,
                Adapter->Bounce,
                ActualLength
                );
#if UFS_PERF_INSTRUMENTATION
            Adapter->PerfBounceInTicks += UfsPerfTick() - Start;
            Adapter->PerfReadBytes += ActualLength;
            Adapter->PerfReadCount++;
#endif
            /*
             * Translate GPT headers in flight, but only for media reads. The
             * same four-opcode test the READ-overrun check above uses: a GPT
             * header can only ever arrive through READ(6/10/12/16), so gating
             * on them keeps the scan off INQUIRY/MODE SENSE/REPORT LUNS
             * payloads that could otherwise contain the signature by accident.
             *
             * Runs on Srb->DataBuffer - ordinary cached memory - never on the
             * MmNonCached bounce. See the design note on UfsTranslateGptHeaders.
             */
            if ((Srb->Cdb[0] == SCSIOP_READ6) ||
                (Srb->Cdb[0] == SCSIOP_READ) ||
                (Srb->Cdb[0] == SCSIOP_READ12) ||
                (Srb->Cdb[0] == SCSIOP_READ16)) {
                UfsTranslateGptHeaders(
                    Adapter,
                    (PUCHAR)Srb->DataBuffer,
                    ActualLength
                    );
            }
        }
    }
    Srb->DataTransferLength = ActualLength;
    return SRB_STATUS_SUCCESS;
}

static
UCHAR
UfsExecuteScsi(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _Inout_ PSCSI_REQUEST_BLOCK Srb
    )
{
    UFS_COMMAND_DIRECTION Direction;
    BOOLEAN CompleteLocally;
    BOOLEAN ProtocolFailure;
    ULONG Nexus;
    ULONG DataLength;
    /*
     * Initialized because the CDB-length and data-out tests below short-circuit
     * ahead of UfsClassifyCdb, so the reject path can be reached without either
     * out-parameter ever being written.
     */
    ULONG ClassifyReject = UFS_EXEC_REJECT_CLASSIFY;
    ULONGLONG ClassifyFailure = 0ULL;
    UCHAR SrbStatus;
    /*
     * V33: doorbell timestamp. Zero-initialised because the many early returns
     * above the doorbell never reach the paired read, and /W4 /WX would
     * otherwise reject a conditionally-initialised local.
     */
#if UFS_PERF_INSTRUMENTATION
    ULONGLONG PerfDoorbell = 0ULL;
#endif

    Adapter->Diagnostic.ExecuteScsiRequests++;

    if (Srb->CdbLength != 0 && Srb->Cdb[0] == UFS_CLEAN_RECOVERY_OPCODE) {
        return UfsCleanRecoveryControl(Adapter, Srb);
    }

    /*
     * Vendor diagnostic escape hatch. Answered before every precondition so the
     * adapter's own state is readable precisely when the adapter is refusing
     * work - which is the only time it matters. It is served from memory and is
     * never encoded into a UPIU, so it cannot touch the device or the media.
     */
    if ((Srb->CdbLength >= 6) &&
        (Srb->Cdb[0] == UFS_DIAG_VENDOR_CDB_OPCODE) &&
        ((Srb->SrbFlags & SRB_FLAGS_DATA_OUT) == 0) &&
        (Srb->DataBuffer != NULL) &&
        (Srb->DataTransferLength >= sizeof(UFS_DIAGNOSTIC_DATA))) {
        UFS_PRAM_SIGNATURE Previous;
        UFS_PRAM_SIGNATURE Current;
        BOOLEAN HadPrevious;

        HadPrevious = Adapter->LastPramDiagnosticValid;
        Adapter->Diagnostic.VendorDiagRequests++;
        UfsPramSetPhase(Adapter, UFS_PHASE_VENDOR_DIAG);
        UfsRefreshDiagnostic(Adapter);
        Adapter->Diagnostic.PramRecords = Adapter->PramRecords;
        if (UfsPramDiagnosticChanged(Adapter, &Previous, &Current)) {
            if (HadPrevious) {
                UfsPramDiagnosticDelta(Adapter, &Previous, &Current);
            } else {
                UfsPramSnapshot(Adapter, "UFSDIAG");
            }
            Adapter->PramDuplicatesSuppressed = 0;
        } else {
            Adapter->PramDuplicatesSuppressed++;
        }
        Adapter->Diagnostic.PramRecords = Adapter->PramRecords;
        RtlCopyMemory(
            Srb->DataBuffer,
            &Adapter->Diagnostic,
            sizeof(Adapter->Diagnostic)
            );
        Srb->DataTransferLength = sizeof(Adapter->Diagnostic);
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return SRB_STATUS_SUCCESS;
    }

    /*
     * Vendor reboot-to-recovery escape hatch.
     *
     * This is what makes the bring-up loop unattended: without it, every
     * measurement ends with the phone sitting in WinPE, invisible to adb, until
     * somebody holds three buttons. Windows cannot reboot this device the normal
     * way - the EL3 monitor implements no PSCI SYSTEM_RESET - so the driver
     * drives the PMU directly, exactly as UEFI and Linux both do.
     *
     * Guarded three ways: a dedicated opcode, the subcode, and a confirmation
     * word, all of which must be present in the CDB. It writes only PMU
     * registers, never a UPIU, so it cannot reach the UFS device or its media -
     * this stays within the read-only contract.
     *
     * Answered before the preconditions for the same reason as 0xD0: a boot
     * whose adapter refused to start is precisely the boot that most needs to
     * be able to hand the phone back.
     */
    if ((Srb->CdbLength >= 10) &&
        (Srb->Cdb[0] == UFS_DIAG_REBOOT_CDB_OPCODE) &&
        (((ULONG)Srb->Cdb[1] << 8 | (ULONG)Srb->Cdb[2]) ==
             UFS_DIAG_REBOOT_CDB_SUBCODE) &&
        (Srb->Cdb[3] == (UCHAR)((UFS_DIAG_REBOOT_CDB_CONFIRM >> 24) & 0xFFU)) &&
        (Srb->Cdb[4] == (UCHAR)((UFS_DIAG_REBOOT_CDB_CONFIRM >> 16) & 0xFFU)) &&
        (Srb->Cdb[5] == (UCHAR)((UFS_DIAG_REBOOT_CDB_CONFIRM >> 8) & 0xFFU)) &&
        (Srb->Cdb[6] == (UCHAR)(UFS_DIAG_REBOOT_CDB_CONFIRM & 0xFFU))) {
        if (Adapter->Pmu == NULL) {
            return SRB_STATUS_INVALID_REQUEST;
        }

        UfsPramSetPhase(Adapter, UFS_PHASE_VENDOR_REBOOT);

        /*
         * Reaching this CDB is proof the boot survived the write path - a boot
         * that bugchecks inside it never gets here - so the crash lockout still
         * catches every case it was built for.
         *
         * Closing the window here as well as in DISARM is what stops the
         * unattended loop wedging. The loader's --disarm is its LAST step, so
         * anything that stops the loader short of it, or a watchdog reboot
         * (which issues this same CDB), used to leave the latch armed. The next
         * boot then refused the probe, never reached --disarm either, and
         * re-armed the latch - a lockout with no exit. Handing the phone back
         * is the honest end-of-boot signal; make it close the window too.
         *
         * Disarm as well, so the phone never resets with writes still armed.
         */
        Adapter->WriteMode = UFS_WRITE_MODE_DISARMED;
        UfsPramLatchComplete(Adapter);

        UfsRefreshDiagnostic(Adapter);
        UfsPmuRebootToRecovery(Adapter);

        //
        // Only reached if the reset did not take. Report it rather than
        // pretending, so the caller can fall back to asking for a human.
        //
        Srb->DataTransferLength = 0;
        return SRB_STATUS_ERROR;
    }

    /*
     * Write arming escape hatch. Layer 4 of the write safeguard stack, and the
     * one that makes every other layer meaningful: a flashed image is inert
     * until something explicitly asks for writes, so no boot can lose data by
     * accident, and an image that is never armed is exactly as safe as the
     * read-only builds that preceded it.
     *
     * Guarded like the reboot CDB - dedicated opcode, subcode, and a distinct
     * four-byte confirmation word per mode - and answered before the
     * preconditions so the arm state is settable and reportable even when the
     * adapter is refusing work.
     *
     * DRY-RUN and LIVE are two separate confirmation words rather than one arm
     * plus a flag, because the difference between them is the difference
     * between simulating a write and destroying a partition. Requesting LIVE
     * must be a deliberate, individually auditable act.
     */
    if ((Srb->CdbLength >= 10) &&
        (Srb->Cdb[0] == UFS_DIAG_WRITE_ARM_CDB_OPCODE) &&
        (((ULONG)Srb->Cdb[1] << 8 | (ULONG)Srb->Cdb[2]) ==
             UFS_DIAG_WRITE_ARM_CDB_SUBCODE)) {
        ULONG Confirm =
            ((ULONG)Srb->Cdb[3] << 24) | ((ULONG)Srb->Cdb[4] << 16) |
            ((ULONG)Srb->Cdb[5] << 8) | (ULONG)Srb->Cdb[6];

        Adapter->WriteArmRequests++;
        UfsPramSetPhase(Adapter, UFS_PHASE_ARM_ENTER);
        switch (Confirm) {
        case UFS_DIAG_WRITE_ARM_CONFIRM_DRY_RUN:
            Adapter->WriteMode = UFS_WRITE_MODE_DRY_RUN;
            break;
        case UFS_DIAG_WRITE_ARM_CONFIRM_LIVE:
            Adapter->WriteMode = UFS_WRITE_MODE_LIVE;
            break;
        case UFS_DIAG_WRITE_ARM_CONFIRM_DISARM:
            Adapter->WriteMode = UFS_WRITE_MODE_DISARMED;
            //
            // Disarming is the loader's last write-related act, so it is the
            // only point at which this boot can be declared to have survived the
            // write path. Close the crash window here.
            //
            UfsPramLatchComplete(Adapter);
            break;
        default:
            /* Unrecognised word disarms - the safe direction to fail. */
            Adapter->WriteMode = UFS_WRITE_MODE_DISARMED;
            UfsRefreshDiagnostic(Adapter);
            Srb->DataTransferLength = 0;
            return SRB_STATUS_INVALID_REQUEST;
        }

        UfsPramSetPhase(Adapter, UFS_PHASE_ARM_MODE_SET);
        UfsRefreshDiagnostic(Adapter);
        UfsPramSetPhase(Adapter, UFS_PHASE_ARM_PRE_SNAPSHOT);
        UfsPramSnapshot(Adapter, "UFSARM");
        UfsPramSetPhase(Adapter, UFS_PHASE_ARM_RETURNING);
        Srb->DataTransferLength = 0;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return SRB_STATUS_SUCCESS;
    }

    /*
     * Note escape hatch. The helper's only way to say something durable.
     *
     * RunFilesystemWrite reports exclusively to stdout, which lands on X: - the
     * WinPE RAM disk - and the handback reboot destroys it. Its intended second
     * channel is a file on the UFS volume, i.e. precisely the thing that fails
     * when the filesystem write fails. A failing fs-write is therefore currently
     * unobservable, and that is what this CDB fixes: the reason code, the last
     * Win32 error and the drive letter go into PRAM, which survives the reset
     * and is readable from TWRP.
     *
     * The note lands in slots reserved at ring offset 0 during init, not
     * appended, because by the time the helper runs the ring is already full
     * (~16.1 KiB against the 0x3F00 cap) and an appended record would simply be
     * dropped - which is the exact failure this is meant to escape.
     *
     * Carries no data phase at all: DATA_OUT is refused outright, so the
     * read-only opcode allowlist is never even consulted and this cannot become
     * a back door to the media. Like 0xD0/0xD1/0xD2 it is answered ahead of the
     * preconditions, so the helper can still report when the adapter is
     * refusing work - the case where an explanation matters most.
     */
    if ((Srb->CdbLength >= UFS_DIAG_NOTE_CDB_MIN_LENGTH) &&
        (Srb->Cdb[0] == UFS_DIAG_NOTE_CDB_OPCODE) &&
        (((ULONG)Srb->Cdb[1] << 8 | (ULONG)Srb->Cdb[2]) ==
             UFS_DIAG_NOTE_CDB_SUBCODE) &&
        (Srb->Cdb[3] == (UCHAR)((UFS_DIAG_NOTE_CDB_CONFIRM >> 24) & 0xFFU)) &&
        (Srb->Cdb[4] == (UCHAR)((UFS_DIAG_NOTE_CDB_CONFIRM >> 16) & 0xFFU)) &&
        (Srb->Cdb[5] == (UCHAR)((UFS_DIAG_NOTE_CDB_CONFIRM >> 8) & 0xFFU)) &&
        (Srb->Cdb[6] == (UCHAR)(UFS_DIAG_NOTE_CDB_CONFIRM & 0xFFU))) {
        ULONG NoteCode;
        ULONG NoteAux;
        ULONG NoteTag;

        if ((Srb->SrbFlags & SRB_FLAGS_DATA_OUT) != 0) {
            Srb->DataTransferLength = 0;
            return SRB_STATUS_INVALID_REQUEST;
        }

        NoteCode = (ULONG)Srb->Cdb[7];
        NoteAux =
            ((ULONG)Srb->Cdb[8] << 24) | ((ULONG)Srb->Cdb[9] << 16) |
            ((ULONG)Srb->Cdb[10] << 8) | (ULONG)Srb->Cdb[11];
        NoteTag =
            ((ULONG)Srb->Cdb[12] << 24) | ((ULONG)Srb->Cdb[13] << 16) |
            ((ULONG)Srb->Cdb[14] << 8) | (ULONG)Srb->Cdb[15];

        UfsPramSetPhase(Adapter, UFS_PHASE_VENDOR_NOTE);

        /*
         * Emit before incrementing: UfsPramNote reads NoteRequests for NSEQ, so
         * this order makes the first note NSEQ=0 and leaves NoteRequests as an
         * honest count of notes ASKED for. NoteRequests > PramNotesWritten is
         * then the only way to see that a note was dropped for want of a slot.
         */
        UfsPramNote(Adapter, NoteCode, NoteAux, NoteTag);
        Adapter->NoteRequests++;
        Adapter->LastNoteCode = NoteCode;
        Adapter->LastNoteAux = NoteAux;

        UfsRefreshDiagnostic(Adapter);
        Srb->DataTransferLength = 0;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return SRB_STATUS_SUCCESS;
    }

    if (Adapter->DiagnosticOnly) {
        Adapter->Diagnostic.LastExecuteReject =
            UFS_EXEC_REJECT_DIAGNOSTIC_ONLY;
        return SRB_STATUS_NO_DEVICE;
    }

    if (!Adapter->Started) {
        Adapter->Diagnostic.LastExecuteReject = UFS_EXEC_REJECT_NOT_STARTED;
        return SRB_STATUS_ERROR;
    }

    if (Adapter->FatalError) {
        Adapter->Diagnostic.LastExecuteReject = UFS_EXEC_REJECT_FATAL_ERROR;
        return SRB_STATUS_ERROR;
    }

    if ((Srb->PathId != 0) || (Srb->TargetId != 0) || (Srb->Lun != 0)) {
        Adapter->Diagnostic.LastExecuteReject = UFS_EXEC_REJECT_BAD_NEXUS;
        return SRB_STATUS_NO_DEVICE;
    }

    /*
     * Layer 1: the opcode allowlist. Data-out is admitted only for a CDB the
     * allowlist itself accepts, so a request that merely claims to be a write
     * still has to name WRITE(10) or WRITE(16) and survive the fence.
     */
    /*
     * A malformed CDB length genuinely is a request this adapter cannot
     * express, so it keeps the adapter-level status. Splitting it out also
     * holds the CHECK CONDITION change below to exactly one variable: opcodes
     * the allowlist refuses, and nothing else.
     */
    if ((Srb->CdbLength == 0) ||
        (Srb->CdbLength > sizeof(((PUFS_COMMAND_UPIU)0)->Cdb))) {
        Adapter->RejectedCommands++;
        Adapter->Diagnostic.LastExecuteReject = UFS_EXEC_REJECT_CLASSIFY;
        UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_TRANSFER_LENGTH);
        return SRB_STATUS_INVALID_REQUEST;
    }

    if ((((Srb->SrbFlags & SRB_FLAGS_DATA_OUT) != 0) &&
         !UfsIsWriteOpcode(Srb->Cdb[0])) ||
        !UfsClassifyCdb(
            Adapter,
            Srb,
            &Direction,
            &CompleteLocally,
            &ClassifyReject,
            &ClassifyFailure
            )) {
        Adapter->RejectedCommands++;
        UfsRecordRejectedOpcode(Adapter, Srb->Cdb[0]);
        UfsRecordRejectedCdb(Adapter, Srb);
        if (UfsIsWriteOpcode(Srb->Cdb[0])) {
            Adapter->WritesAttempted++;
            if (ClassifyFailure == UFS_DIAG_FAILURE_WRITE_FENCE) {
                Adapter->WritesFenced++;
            } else if (ClassifyFailure == UFS_DIAG_FAILURE_WRITE_GPT_GUARD) {
                Adapter->WritesGuarded++;
            } else if (ClassifyFailure ==
                       UFS_DIAG_FAILURE_WRITE_PROTECTED_PARTITION) {
                Adapter->WritesProtected++;
            }
            Adapter->LastWriteResult = ClassifyReject;
            UfsPramSetPhase(Adapter, UFS_PHASE_CLASSIFY_REJECT);
        }
        Adapter->Diagnostic.LastExecuteReject = ClassifyReject;
        UfsSetDiagnosticFailure(
            Adapter,
            (ClassifyFailure != 0ULL) ? ClassifyFailure
                                      : UFS_DIAG_FAILURE_TRANSFER_LENGTH
            );
        /*
         * Report the refusal as the DEVICE refusing, not as the adapter being
         * broken - see UfsCompleteWithSense for why that distinction decides
         * whether partmgr enumerates partitions or gives up. The reject codes,
         * failure bits and write counters above are deliberately unchanged;
         * only the status handed back to Windows differs.
         */
        if (UfsIsWriteOpcode(Srb->Cdb[0])) {
            return UfsCompleteWithSense(
                Adapter,
                Srb,
                UFS_SENSE_KEY_DATA_PROTECT,
                UFS_ASC_WRITE_PROTECTED,
                0x00
                );
        }

        return UfsCompleteWithSense(
            Adapter,
            Srb,
            UFS_SENSE_KEY_ILLEGAL_REQUEST,
            UFS_ASC_INVALID_COMMAND_OPERATION_CODE,
            0x00
            );
    }

    /*
     * Layers 4 and 5, applied only once the request is known to be a legal,
     * fenced write. Disarmed refuses outright; dry-run reports success without
     * ever building a descriptor, which proves classification, the fence and
     * the journal against real Windows I/O at zero risk to the media.
     */
    if (Direction == UfsCommandDataOut) {
        UfsPramSetPhase(Adapter, UFS_PHASE_GATE_ENTERED);
        Adapter->WritesAttempted++;
        Adapter->LastWriteLba = (ULONG)UfsExtractWriteLba(Srb->Cdb);
        Adapter->LastWriteBlocks =
            Srb->DataTransferLength / Adapter->LogicalBlockSize;

        /*
         * Crash lockout, ahead of every other write gate.
         *
         * A previous boot in this warm-reset chain entered the write path and
         * never came back out. Whatever it was doing is, by definition, the last
         * thing that ran before the machine died, so repeating it would repeat
         * the crash - and because a bugcheck reboots straight back into WinPE,
         * that loop never reaches TWRP and strands an unattended cycle until
         * somebody holds the power button.
         *
         * Refusing writes for exactly one boot breaks the loop: this boot still
         * completes normally, still arms the self-reboot, and still lands in
         * TWRP carrying WriteCrashAttempt, which names the write that killed its
         * predecessor.
         */
        /*
         * Kept ON in the full-Windows build, deliberately.
         *
         * V16 disabled this and that removed the only channel by which a
         * bugchecked write can ever be diagnosed. The PRAM ring cannot serve:
         * a bugcheck reboots through UEFI, and the firmware's own ACPI trace
         * refills all 16 KB of the ring before Linux snapshots it, so the
         * driver's telemetry from the crashed boot is destroyed. Measured on
         * boot 2 - the capture came back as =ACPIINST/=DSDT/=PPTT with not one
         * UFS record in it.
         *
         * The lockout is what turns an undiagnosable reboot loop into a single
         * readable boot: the crashed attempt is latched in PRAM, the next boot
         * refuses writes for exactly one cycle, completes, and returns to TWRP
         * carrying the attempt number and the phase word that names the step.
         */
        if (Adapter->Diagnostic.WriteCrashLockout != 0) {
            UfsPramSetPhase(Adapter, UFS_PHASE_CRASH_LOCKOUT);
            Adapter->WritesDisarmedRejects++;
            Adapter->RejectedCommands++;
            Adapter->LastWriteResult = UFS_EXEC_REJECT_WRITE_CRASH_LOCKOUT;
            Adapter->Diagnostic.LastExecuteReject =
                UFS_EXEC_REJECT_WRITE_CRASH_LOCKOUT;
            UfsSetDiagnosticFailure(
                Adapter,
                UFS_DIAG_FAILURE_WRITE_CRASH_LOCKOUT
                );
            UfsPramWriteTrail(Adapter);
            return SRB_STATUS_INVALID_REQUEST;
        }

#if UFS_READOUT_BOOT
        /*
         * Readout window. Ahead of the latch, because a refusal here must not
         * look like a boot that entered the crash window and died in it.
         *
         * Confines this build to sda18 and refuses the ESP and the Windows
         * volume outright - the two regions a real boot writes and the two
         * currently ending in KMODE_EXCEPTION_NOT_HANDLED. Refusing costs the
         * boot nothing: boot 1 refused all 28 of bcdboot's writes and still
         * completed with its telemetry intact.
         *
         * Refused before a descriptor is built, before the bounce copy, before
         * any DMA - the same place the DISARMED gate refuses - so nothing on
         * the write path can fault. What survives is the log on sda18, which
         * TWRP can read after the reboot that PRAM does not survive.
         */
        {
            ULONG ReadoutFirst = Adapter->LastWriteLba;
            ULONG ReadoutLast = Adapter->LastWriteLba +
                                ((Adapter->LastWriteBlocks != 0)
                                     ? (Adapter->LastWriteBlocks - 1UL)
                                     : 0UL);
            BOOLEAN ReadoutOnLog =
                (ReadoutFirst >= UFS_READOUT_WINDOW_FIRST_LBA) &&
                (ReadoutLast <= UFS_READOUT_WINDOW_LAST_LBA);
            /*
             * The scratch region is permitted too, so the write ladder can run
             * in situ in this same boot. It is the exact target the 2026-07-31
             * capture wrote and read back at 4/16/32/64 KB with LASTOCS=0, so a
             * pass or a fail here is a direct comparison against a known-good
             * result on the same hardware - which is what decides whether the
             * rebuilt binary regressed. It lies past sda25's last LBA in
             * unallocated space, so nothing on the device depends on it.
             */
            BOOLEAN ReadoutOnScratch =
                (ReadoutFirst >= UFS_READOUT_SCRATCH_FIRST_LBA) &&
                (ReadoutLast <= UFS_READOUT_SCRATCH_LAST_LBA);

            if (!ReadoutOnLog && !ReadoutOnScratch) {
                UfsPramSetPhase(Adapter, UFS_PHASE_CLASSIFY_REJECT);
                Adapter->WritesFenced++;
                Adapter->RejectedCommands++;
                Adapter->LastWriteResult = UFS_EXEC_REJECT_WRITE_FENCE;
                Adapter->Diagnostic.LastExecuteReject =
                    UFS_EXEC_REJECT_WRITE_FENCE;
                UfsSetDiagnosticFailure(
                    Adapter,
                    UFS_DIAG_FAILURE_WRITE_FENCE
                    );
                UfsPramWriteTrail(Adapter);
                /*
                 * Match the proven-survivable DISARMED path exactly.
                 *
                 * v19 and v20 were the first builds to reject early boot writes
                 * through UfsCompleteWithSense. Both bugchecked before
                 * readout.cmd; v20's first raw-LBA slot remained all zero. The
                 * v18 control rejected the same class of writes with
                 * SRB_STATUS_INVALID_REQUEST and returned to recovery cleanly.
                 *
                 * The only contract difference was synthetic autosense:
                 * UfsCompleteWithSense writes 18 bytes through
                 * Srb->SenseInfoBuffer. Do not touch that caller-owned pointer
                 * on this pre-shell path until a controlled build proves it is
                 * valid for filesystem write SRBs on this platform.
                 */
                return SRB_STATUS_INVALID_REQUEST;
            }
        }
#endif

        //
        // Past this point the boot is inside the crash window. Record which
        // attempt it is before anything can fault.
        //
        UfsPramLatchSetAttempt(Adapter, Adapter->WritesAttempted);
        UfsPramSetPhase(Adapter, UFS_PHASE_LATCH_SET);

        if (Adapter->WriteMode == UFS_WRITE_MODE_DISARMED) {
            UfsPramSetPhase(Adapter, UFS_PHASE_DISARMED_PRE);
            Adapter->WritesDisarmedRejects++;
            Adapter->RejectedCommands++;
            Adapter->LastWriteResult = UFS_EXEC_REJECT_WRITE_DISARMED;
            Adapter->Diagnostic.LastExecuteReject =
                UFS_EXEC_REJECT_WRITE_DISARMED;
            UfsSetDiagnosticFailure(
                Adapter,
                UFS_DIAG_FAILURE_WRITE_DISARMED
                );
            UfsPramWriteTrail(Adapter);
            UfsPramSetPhase(Adapter, UFS_PHASE_DISARMED_POST);
            return SRB_STATUS_INVALID_REQUEST;
        }

        if (Adapter->WriteMode == UFS_WRITE_MODE_DRY_RUN) {
            UfsPramSetPhase(Adapter, UFS_PHASE_DRY_RUN_PRE);
            Adapter->WritesDryRun++;
            Adapter->LastWriteResult = UFS_EXEC_REJECT_WRITE_DRY_RUN;
            Adapter->Diagnostic.LastExecuteReject =
                UFS_EXEC_REJECT_WRITE_DRY_RUN;
            UfsPramWriteTrail(Adapter);
            UfsPramSetPhase(Adapter, UFS_PHASE_DRY_RUN_POST);
            /*
             * A dry run moves no data, so it must not claim it did. Every other
             * early return in this dispatcher zeroes this; omitting it here made
             * a DATA_OUT SRB report a full transfer that never happened, which is
             * indistinguishable to the caller from a real write having landed.
             */
            Srb->DataTransferLength = 0;
            Srb->ScsiStatus = SCSISTAT_GOOD;
            return SRB_STATUS_SUCCESS;
        }

        UfsPramSetPhase(Adapter, UFS_PHASE_LIVE_ENTERED);
    }

    Adapter->LastOpcode = Srb->Cdb[0];
    //
    // The length the command ASKED for, recorded before anything can fail.
    // DINLEN is LastDataInLength, which is only written once a transfer has
    // completed and been copied back - so on a failed command it still holds
    // the previous command's value and reads as a healthy short transfer. That
    // is precisely what made the 2-block and 4-block probes look like they had
    // never run: their records carried the stale 8-byte READ CAPACITY length.
    // REQLEN is the only field that distinguishes "this probe was never issued"
    // from "this probe was issued and did not come back".
    //
    Adapter->LastRequestLength = Srb->DataTransferLength;
    Srb->ScsiStatus = SCSISTAT_GOOD;
    if (CompleteLocally) {
        Adapter->Diagnostic.LastExecuteReject =
            UFS_EXEC_REJECT_LOCAL_COMPLETE;
        Srb->DataTransferLength = 0;
        return SRB_STATUS_SUCCESS;
    }

    /*
     * A previous command may have quiesced the transfer list. Re-arm it here
     * instead of refusing everything that follows. UfsProgramTransferList is
     * the same primitive HwInitialize uses: it re-zeroes the workspace, gates
     * interrupts, drains the interrupt-status latch, reprograms the list base
     * and the vendor nexus, and only reports success once the controller has
     * read all of it back. Read-only safety is unaffected - it is the CDB
     * allowlist in UfsClassifyCdb that rejects data-out, not this quiesce.
     */
    if (!Adapter->OwnsTransferList ||
        (UfsReadRegister(Adapter, UFS_REG_UTRL_RUN_STOP) !=
         UFS_LIST_RUN_STOP)) {
        /*
         * The budget counts CONSECUTIVE re-arms, not lifetime ones.
         *
         * V14 died here: RearmAttempts was monotonic for the whole boot, so 16
         * failures scattered across ~100 commands - with 68 successful
         * round-trips among them - were enough to trip a bound meant to catch a
         * controller that will not come back. Reset on success (see the
         * completion path below) so the bound measures a controller that is
         * genuinely stuck rather than one that is merely imperfect.
         */
        if (Adapter->RearmAttempts >= UFS_MAX_REARM_ATTEMPTS) {
            Adapter->FailedCommands++;
            Adapter->Diagnostic.RearmBusyRejects++;
            UfsSetDiagnosticFailure(
                Adapter,
                UFS_DIAG_FAILURE_REARM_EXHAUSTED
                );
            /*
             * Deliberately NOT fatal.
             *
             * Latching FatalError/Started here is what made V11-V14
             * undiagnosable: one exhausted budget turned every later command -
             * including the locally-completed TEST_UNIT_READY - into
             * STATUS_IO_DEVICE_ERROR, so the adapter looked dead from outside
             * and no evidence could be gathered from it. SRB_STATUS_BUSY lets
             * Storport retry, keeps the endpoint queryable, and leaves recovery
             * possible if the condition clears.
             */
            Adapter->Diagnostic.LastExecuteReject =
                UFS_EXEC_REJECT_REARM_BUSY;
            return SRB_STATUS_BUSY;
        }
        Adapter->RearmAttempts++;
        if (!UfsProgramTransferList(Adapter)) {
            Adapter->FailedCommands++;
            UfsSetDiagnosticFailure(
                Adapter,
                UFS_DIAG_FAILURE_REARM_FAILED
                );
            Adapter->Diagnostic.LastExecuteReject =
                UFS_EXEC_REJECT_REARM_FAILED;
            return SRB_STATUS_ERROR;
        }
        Adapter->Diagnostic.RearmSuccesses++;
        Adapter->InterruptsGated = TRUE;
    }

    if (((Direction != UfsCommandNoData) &&
         (Direction != UfsCommandDataIn) &&
         (Direction != UfsCommandDataOut)) ||
        ((Direction == UfsCommandNoData) &&
         ((Srb->DataTransferLength != 0) ||
          ((Srb->SrbFlags &
            (SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT)) != 0))) ||
        ((Direction != UfsCommandNoData) &&
         ((Srb->DataTransferLength == 0) ||
          (Srb->DataTransferLength > UFS_MAX_TRANSFER_LENGTH) ||
          (Srb->DataBuffer == NULL))) ||
        ((Direction == UfsCommandDataIn) &&
         (((Srb->SrbFlags & SRB_FLAGS_DATA_IN) == 0) ||
          ((Srb->SrbFlags & SRB_FLAGS_DATA_OUT) != 0))) ||
        ((Direction == UfsCommandDataOut) &&
         (((Srb->SrbFlags & SRB_FLAGS_DATA_OUT) == 0) ||
          ((Srb->SrbFlags & SRB_FLAGS_DATA_IN) != 0))) ||
        !Adapter->OwnsTransferList ||
        !Adapter->InterruptsGated ||
        (UfsReadRegister(Adapter, UFS_REG_INTERRUPT_ENABLE) != 0) ||
        (UfsReadRegister(Adapter, UFS_REG_UTRL_BASE_LOW) !=
         (Adapter->UtrlPhysical.LowPart &
          ~(UFS_UTRL_ALIGNMENT - 1U))) ||
        (UfsReadRegister(Adapter, UFS_REG_UTRL_BASE_HIGH) != 0) ||
        (UfsReadRegister(Adapter, UFS_REG_UTRL_RUN_STOP) !=
         UFS_LIST_RUN_STOP) ||
        (UfsReadRegister(Adapter, UFS_REG_UTRL_DOORBELL) != 0)) {
        Adapter->FailedCommands++;
        UfsContainController(
            Adapter,
            UFS_DIAG_FAILURE_PROGRAM_PRECONDITION
            );
        Adapter->Diagnostic.LastExecuteReject = UFS_EXEC_REJECT_PRECONDITION;
        return SRB_STATUS_BUSY;
    }

    Adapter->Diagnostic.LastExecuteReject = UFS_EXEC_REJECT_ISSUED;
    DataLength = Srb->DataTransferLength;
    if (Direction == UfsCommandDataOut) {
        UfsPramSetPhase(Adapter, UFS_PHASE_LIVE_PRECHECK);
    }
    if (!UfsBuildCommand(Adapter, Srb, Direction)) {
        Adapter->RejectedCommands++;
        UfsSetDiagnosticFailure(
            Adapter,
            UFS_DIAG_FAILURE_UCD_VIRTUAL_ALIGNMENT
            );
        return SRB_STATUS_INVALID_REQUEST;
    }
    KeMemoryBarrier();

    if (Direction == UfsCommandDataOut) {
        UfsPramSetPhase(Adapter, UFS_PHASE_LIVE_BUILT);
    }

    Nexus = Adapter->InitialNexusType | UFS_TRANSFER_SLOT_MASK;
    UfsWriteRegister(
        Adapter,
        UFS_VENDOR_NEXUS_TYPE,
        Nexus
        );
    KeMemoryBarrier();
    if (UfsReadRegister(Adapter, UFS_VENDOR_NEXUS_TYPE) != Nexus) {
        Adapter->FailedCommands++;
        UfsContainController(
            Adapter,
            UFS_DIAG_FAILURE_NEXUS_READBACK
            );
        return SRB_STATUS_ERROR;
    }

    if (Direction == UfsCommandDataOut) {
        UfsPramSetPhase(Adapter, UFS_PHASE_LIVE_NEXUS);
        /*
         * Past this line the payload is committed to the device. Count it here
         * rather than after completion so a write that is issued and then times
         * out is still visible as issued - "we rang the doorbell and do not
         * know what happened" is the single most important state to be able to
         * see from TWRP after a bad write.
         */
        Adapter->WritesIssued++;
        Adapter->LastWriteTick = Adapter->UnattendedTicks;
        UfsPramWriteTrail(Adapter);
    }

    if (Direction == UfsCommandNoData) {
        UfsPramFlushBegin(Adapter);
    }

    UfsWriteRegister(
        Adapter,
        UFS_REG_UTRL_DOORBELL,
        UFS_TRANSFER_SLOT_MASK
        );
    KeMemoryBarrier();

    if (Direction == UfsCommandDataOut) {
        UfsPramSetPhase(Adapter, UFS_PHASE_LIVE_DOORBELL);
    }

    /*
     * V33: bracket exactly the window the controller and device own. Started
     * after the doorbell store and the barrier so it excludes our own
     * programming, and stopped the instant the poll observes the doorbell
     * clear. This is the irreducible floor that the bounce-copy counters are
     * compared against.
     */
#if UFS_PERF_INSTRUMENTATION
    PerfDoorbell = UfsPerfTick();
#endif
    if (!UfsPollForCompletion(Adapter)) {
        Adapter->FailedCommands++;
        if (Direction == UfsCommandNoData) {
            UfsPramFlushComplete(Adapter, FALSE);
        }
        UfsContainController(
            Adapter,
            UFS_DIAG_FAILURE_TIMEOUT_CONTAINMENT
            );
        return SRB_STATUS_TIMEOUT;
    }

    if (Direction == UfsCommandDataOut) {
        UfsPramSetPhase(Adapter, UFS_PHASE_LIVE_POLLED);
    }

#if UFS_PERF_INSTRUMENTATION
    {
        ULONGLONG Elapsed = UfsPerfTick() - PerfDoorbell;

        Adapter->PerfDeviceTicks += Elapsed;
        if (Elapsed > Adapter->PerfMaxDeviceTicks) {
            Adapter->PerfMaxDeviceTicks = Elapsed;
        }
    }
#endif

    if (!UfsAcknowledgeInterruptStatus(Adapter)) {
        Adapter->FailedCommands++;
        if (Direction == UfsCommandNoData) {
            UfsPramFlushComplete(Adapter, FALSE);
        }
        //
        // Freeze the transfer descriptor's OCS at the moment the acknowledge
        // rejected the command. UfsFinishScsiCommand is what normally updates
        // LastOcs, and it is precisely the function that does NOT run on this
        // path - so without this the record carries the OCS of the PREVIOUS,
        // successful command and reads as a healthy transfer. That stale value
        // is what made the 2-block and 4-block reads look like they returned
        // OCS=0: the doorbell had already cleared, so the controller really did
        // finish, and the only unknown left is whether the descriptor agreed.
        //
        Adapter->AckFailureOcs =
            (ULONG)(Adapter->Utrl[UFS_TRANSFER_SLOT].Dw2 & 0xFFU);
        //
        // Capture what the controller actually deposited BEFORE giving up on
        // this command. UfsFinishScsiCommand normally does this and is skipped
        // here, so without it DIN/DINLEN/NZBLK still describe the previous,
        // successful command and the failed transfer reads as healthy.
        //
        // This is the measurement that decides the next move in one flash cycle
        // instead of two: if NZBLK shows every granule delivered, the DMA
        // worked and only the interrupt-status handling is wrong; if it shows
        // nothing landed, the transfer genuinely failed. DataTransferLength is
        // the requested length rather than a residual-adjusted one, which is
        // correct here precisely because the response UPIU was never parsed.
        //
        UfsCaptureBounceEvidence(Adapter, Srb->DataTransferLength);
        UfsContainController(
            Adapter,
            UFS_DIAG_FAILURE_INTERRUPT_STATUS
            );
        return SRB_STATUS_ERROR;
    }

    Adapter->CompletedCommands++;
    if (Direction == UfsCommandDataOut) {
        UfsPramSetPhase(Adapter, UFS_PHASE_LIVE_ACKED);
    }
    SrbStatus = UfsFinishScsiCommand(
        Adapter,
        Srb,
        &ProtocolFailure
        );
    if (Direction == UfsCommandDataOut) {
        UfsPramSetPhase(Adapter, UFS_PHASE_LIVE_FINISHED);
    }
    if (Direction == UfsCommandNoData) {
        UfsPramFlushComplete(
            Adapter,
            (BOOLEAN)(SrbStatus == SRB_STATUS_SUCCESS)
            );
    }
    if (SrbStatus != SRB_STATUS_SUCCESS) {
        Adapter->FailedCommands++;
        Adapter->Diagnostic.ConsecutiveFailures++;
        if (Adapter->Diagnostic.ConsecutiveFailures >
            Adapter->Diagnostic.MaxConsecutiveFailures) {
            Adapter->Diagnostic.MaxConsecutiveFailures =
                Adapter->Diagnostic.ConsecutiveFailures;
        }
    } else {
        /*
         * A completed command proves the list, the descriptor, the doorbell and
         * the device are all working right now, so forgive the re-arm budget.
         * Without this the budget is a lifetime allowance and any long-running
         * boot eventually spends it.
         */
        Adapter->Diagnostic.ConsecutiveFailures = 0;
        Adapter->RearmAttempts = 0;

        /*
         * Layer 6: a live write is not finished until it has been read back.
         *
         * The device reporting GOOD only says it accepted the command; it says
         * nothing about where the DMA actually landed. On this controller that
         * distinction is the difference between a recoverable mistake and an
         * unrecoverable one, so pay for a second command and check.
         *
         * A verify failure is reported to Windows as an error even though the
         * write itself "succeeded", because from the caller's point of view the
         * data is not where it asked for it to be - and silently reporting
         * success would be the one outcome that could corrupt a filesystem
         * without leaving a trace.
         */
        if (Direction == UfsCommandDataOut) {
#if UFS_FULL_WINDOWS_MODE
            /*
             * Layer 6 is OFF for a real installation, for two reasons.
             *
             * Correctness first: this is the largest block of code in the
             * driver that had never executed on hardware. It issues a SECOND
             * complete SCSI command from inside the dispatcher - reprogramming
             * the UTRL, the UCD and the PRDT, re-ringing the doorbell and
             * re-polling - while the caller's write is still in flight. Every
             * write Windows issues would run it, so any fault in it is a fault
             * on the very first write. Boot 2 bugchecked on exactly that write,
             * and this is the only untested code on the path.
             *
             * Cost second: it doubles every write on a miniport that is already
             * single-slot, half-duplex, MaxNumberOfIO=1 and polling-completion,
             * and it compares the read-back a byte at a time through an uncached
             * mapping. A full Windows boot cannot afford that.
             *
             * The protection it provided is not lost, only relocated: the write
             * fence and the two protected-partition ranges still refuse every
             * LBA outside the Windows volume and the ESP, and a misplaced write
             * inside those two is recoverable by re-staging the image. Landing
             * is confirmed out-of-band instead - the finalize script's probe
             * file and bcdboot's own output are read back from TWRP, which
             * proves placement without a kernel-mode read-back on every I/O.
             */
            Adapter->WritesVerified++;
            Adapter->LastWriteResult = UFS_EXEC_REJECT_NONE;
            UfsPramSetPhase(Adapter, UFS_PHASE_LIVE_VERIFIED);
#else
            if (UfsVerifyWrittenBlocks(
                    Adapter,
                    Adapter->LastWriteLba,
                    Adapter->LastWriteBlocks,
                    Srb->DataBuffer,
                    DataLength)) {
                Adapter->WritesVerified++;
                Adapter->LastWriteResult = UFS_EXEC_REJECT_NONE;
            } else {
                Adapter->WriteVerifyFailures++;
                Adapter->LastWriteResult = UFS_EXEC_REJECT_WRITE_VERIFY;
                UfsSetDiagnosticFailure(Adapter, UFS_DIAG_FAILURE_WRITE_VERIFY);
                SrbStatus = SRB_STATUS_ERROR;
            }
#endif
            UfsPramWriteTrail(Adapter);
            UfsPramSetPhase(Adapter, UFS_PHASE_LIVE_COMPLETE);
#if UFS_FULL_WINDOWS_MODE
            /*
             * Windows keeps the miniport armed for the lifetime of the OS, so it
             * never sends the diagnostic DISARM CDB that closes the probe latch.
             * A successful write is the full-Windows completion boundary: close
             * it here, and let the next write re-arm it before touching hardware.
             * Keep LIVE_COMPLETE adjacent to this close; initialization relies on
             * that phase to recover old builds that omitted the completion bit.
             */
            UfsPramLatchComplete(Adapter);
#endif
        }
    }

    /*
     * ProtocolFailure deliberately no longer contains the controller.
     *
     * Containment stops our list and hands the base register back to the
     * firmware's; the next command then stops, rebases, restarts and rings
     * again. V14 showed that churn is not a recovery, it is the failure mode:
     * across the whole boot the ONLY failure bits set were TRANSFER_LENGTH,
     * RESPONSE_FRAMING and REARM_EXHAUSTED - no timeout, no precondition
     * mismatch, no nexus mismatch, no fatal interrupt status - so every
     * containment was triggered by a malformed response and every one of them
     * succeeded in re-arming afterwards. Seventeen containments against a
     * sixteen-attempt budget is what killed the adapter.
     *
     * A malformed response UPIU says nothing about the state of the transfer
     * list, so treat it as what it is: this command failed. List integrity is
     * still enforced independently, and more strictly, by the precondition
     * block above, which re-reads UTRLBA/RUNSTOP/DOORBELL/IE before every
     * single command and does contain on genuine drift. Read-only enforcement
     * is unaffected either way - that lives in UfsClassifyCdb's allowlist.
     */
    UNREFERENCED_PARAMETER(ProtocolFailure);

    UfsZeroUncached(Adapter->Utrl, UFS_UTRL_REGION_SIZE);
    UfsZeroUncached(Adapter->Ucd, UFS_UCD_REGION_SIZE);
    /*
     * Do not clear the bounce here. The next data-in command zeroes its entire
     * submitted range before DMA, and the next data-out command overwrites its
     * entire submitted range before the doorbell. Clearing it after completion
     * therefore adds uncached writes to every I/O without changing what the
     * controller or caller can observe.
     */
    KeMemoryBarrier();
    return SrbStatus;
}

static
VOID
UfsCompleteRequest(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _Inout_ PSCSI_REQUEST_BLOCK Srb,
    _In_ UCHAR SrbStatus
    )
{
    Srb->SrbStatus = SrbStatus;
    StorPortNotification(RequestComplete, Adapter, Srb);
    StorPortNotification(NextRequest, Adapter);
}

static
BOOLEAN
UfsDiagnosticSignatureMatches(
    _In_reads_(UFS_DIAG_SRB_SIGNATURE_LENGTH) const UCHAR *Signature
    )
{
    ULONG Index;

    for (Index = 0; Index < UFS_DIAG_SRB_SIGNATURE_LENGTH; Index++) {
        if (Signature[Index] !=
            (UCHAR)UFS_DIAG_SRB_SIGNATURE[Index]) {
            return FALSE;
        }
    }

    return TRUE;
}

static
UCHAR
UfsHandleIoControl(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter,
    _Inout_ PSCSI_REQUEST_BLOCK Srb
    )
{
    PSRB_IO_CONTROL Control;
    PUFS_DIAGNOSTIC_DATA Output;

    Adapter->Diagnostic.IoControlRequests++;
    Adapter->Diagnostic.LastIoControlBufferPresent =
        (Srb->DataBuffer != NULL) ? 1UL : 0UL;
    Adapter->Diagnostic.LastIoControlLength = Srb->DataTransferLength;

    if ((Srb->DataBuffer == NULL) ||
        (Srb->DataTransferLength < sizeof(SRB_IO_CONTROL))) {
        Adapter->Diagnostic.IoControlRejects++;
        return SRB_STATUS_INVALID_REQUEST;
    }

    Control = (PSRB_IO_CONTROL)Srb->DataBuffer;
    Control->ReturnCode = UFS_DIAG_RETURN_INVALID_REQUEST;
    Adapter->Diagnostic.LastIoControlHeaderLength = Control->HeaderLength;
    Adapter->Diagnostic.LastIoControlCode = Control->ControlCode;
    Adapter->Diagnostic.LastIoControlSignatureOk =
        UfsDiagnosticSignatureMatches(Control->Signature) ? 1UL : 0UL;
    if ((Control->HeaderLength < sizeof(SRB_IO_CONTROL)) ||
        (Control->HeaderLength > Srb->DataTransferLength) ||
        (Control->Length < sizeof(UFS_DIAGNOSTIC_DATA)) ||
        (sizeof(UFS_DIAGNOSTIC_DATA) >
            (Srb->DataTransferLength - Control->HeaderLength)) ||
        (Control->ControlCode != UFS_DIAG_CONTROL_CODE) ||
        !UfsDiagnosticSignatureMatches(Control->Signature)) {
        Adapter->Diagnostic.IoControlRejects++;
        return SRB_STATUS_INVALID_REQUEST;
    }

    UfsRefreshDiagnostic(Adapter);
    Output = (PUFS_DIAGNOSTIC_DATA)(
        (PUCHAR)Control + Control->HeaderLength
        );
    RtlCopyMemory(Output, &Adapter->Diagnostic, sizeof(*Output));
    Control->Length = sizeof(*Output);
    Control->ReturnCode = UFS_DIAG_RETURN_SUCCESS;
    Srb->DataTransferLength = Control->HeaderLength + sizeof(*Output);
    return SRB_STATUS_SUCCESS;
}

ULONG
DriverEntry(
    _In_ PVOID Argument1,
    _In_ PVOID Argument2
    )
{
    HW_INITIALIZATION_DATA InitializationData = {0};

    UfsInitializeKernelContract();

    //
    // Total physical memory, captured here for the same reason: this is the only
    // PASSIVE_LEVEL moment the driver gets, and MmGetPhysicalMemoryRanges needs
    // one. The result is stable for the life of the boot, so capturing it early
    // costs nothing in accuracy.
    //
    UfsCaptureSystemMemory();

    InitializationData.HwInitializationDataSize = sizeof(InitializationData);
    InitializationData.AdapterInterfaceType = InterfaceTypeUndefined;
    InitializationData.HwInitialize = UfsHwInitialize;
    InitializationData.HwStartIo = UfsHwStartIo;
    InitializationData.HwInterrupt = UfsHwInterrupt;
#pragma warning(suppress: 4152)
    InitializationData.HwFindAdapter = UfsHwFindAdapter;
    InitializationData.HwResetBus = UfsHwResetBus;
    InitializationData.HwAdapterControl = UfsHwAdapterControl;
    InitializationData.DeviceExtensionSize = sizeof(UFS_ADAPTER_EXTENSION);
    InitializationData.NumberOfAccessRanges = 4;
    InitializationData.MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    InitializationData.NeedPhysicalAddresses = TRUE;
    InitializationData.TaggedQueuing = TRUE;
    InitializationData.AutoRequestSense = TRUE;
    InitializationData.MultipleRequestPerLu = TRUE;
    InitializationData.FeatureSupport =
        STOR_FEATURE_SET_ADAPTER_INTERFACE_TYPE |
        STOR_FEATURE_ADAPTER_NOT_REQUIRE_IO_PORT;
    InitializationData.SrbTypeFlags = SRB_TYPE_FLAG_SCSI_REQUEST_BLOCK;
    InitializationData.AddressTypeFlags = ADDRESS_TYPE_FLAG_BTL8;

    return StorPortInitialize(
        Argument1,
        Argument2,
        &InitializationData,
        NULL
        );
}

ULONG
UfsHwFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_z_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _In_ PBOOLEAN Again
    )
{
    PUFS_ADAPTER_EXTENSION Adapter = (PUFS_ADAPTER_EXTENSION)DeviceExtension;

    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);

    *Again = FALSE;
    RtlZeroMemory(Adapter, sizeof(*Adapter));
    UfsInitializeDiagnostic(Adapter);
    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_FIND_ADAPTER_ENTERED);
    Adapter->Diagnostic.IncomingConfigCapabilities =
        UfsGetConfigCapabilities(ConfigInfo);

    /*
     * The PRDT region caps a single transfer at UFS_PRDT_MAX_ENTRIES segments.
     * Advertising more than that would let Storport hand down an SRB this
     * adapter cannot describe, which the controller answers with HOST_FATAL
     * rather than a clean error - so the cap has to be the descriptor's real
     * capacity, not the bounce buffer's size.
     */
    ConfigInfo->MaximumTransferLength = UFS_MAX_TRANSFER_LENGTH;
    ConfigInfo->NumberOfPhysicalBreaks = UFS_MAX_PHYSICAL_BREAKS;
    ConfigInfo->AlignmentMask = sizeof(ULONG) - 1U;
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->CachesData = FALSE;
    ConfigInfo->MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    ConfigInfo->Dma64BitAddresses = 0;
    ConfigInfo->Dma32BitAddresses = TRUE;
    ConfigInfo->MaximumNumberOfTargets = 2;
    ConfigInfo->MaximumNumberOfLogicalUnits = 1;
    if ((ConfigInfo->InitiatorBusId[0] <= 0) ||
        ((UCHAR)ConfigInfo->InitiatorBusId[0] >=
         ConfigInfo->MaximumNumberOfTargets)) {
        ConfigInfo->InitiatorBusId[0] = 1;
    }
    ConfigInfo->SynchronizationModel = StorSynchronizeHalfDuplex;
    ConfigInfo->InterruptSynchronizationMode = InterruptSupportNone;
    ConfigInfo->MaxNumberOfIO = 1;
    ConfigInfo->MaxIOsPerLun = 1;
    ConfigInfo->InitialLunQueueDepth = 1;
    Adapter->Diagnostic.FinalConfigCapabilities =
        UfsGetConfigCapabilities(ConfigInfo);
    Adapter->Diagnostic.MaximumNumberOfTargets =
        ConfigInfo->MaximumNumberOfTargets;
    Adapter->Diagnostic.MaximumNumberOfLogicalUnits =
        ConfigInfo->MaximumNumberOfLogicalUnits;
    Adapter->Diagnostic.InitiatorBusId =
        (ULONG)(UCHAR)ConfigInfo->InitiatorBusId[0];
    Adapter->Diagnostic.MaxNumberOfIo = ConfigInfo->MaxNumberOfIO;
    Adapter->Diagnostic.MaxIosPerLun = ConfigInfo->MaxIOsPerLun;
    Adapter->Diagnostic.InitialLunQueueDepth =
        ConfigInfo->InitialLunQueueDepth;
    Adapter->Diagnostic.InterruptSynchronizationMode =
        (ULONG)ConfigInfo->InterruptSynchronizationMode;

    UfsCaptureResources(Adapter, ConfigInfo);
    Adapter->Diagnostic.Dma64BitAddresses = ConfigInfo->Dma64BitAddresses;
    Adapter->Diagnostic.Dma32BitAddresses =
        ConfigInfo->Dma32BitAddresses ? 1UL : 0UL;
    Adapter->Diagnostic.MaximumTransferLength =
        ConfigInfo->MaximumTransferLength;
    Adapter->Diagnostic.NumberOfPhysicalBreaks =
        ConfigInfo->NumberOfPhysicalBreaks;

    if (!UfsMapResources(Adapter, ConfigInfo)) {
        UfsEnterDiagnosticOnly(
            Adapter,
            UFS_DIAG_STAGE_RESOURCES_MAPPED
            );
        return SP_RETURN_FOUND;
    }
    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_RESOURCES_MAPPED);

    Adapter->Capabilities = UfsReadRegister(Adapter, UFS_REG_CAP);
    ConfigInfo->Dma64BitAddresses = 0;
    ConfigInfo->Dma32BitAddresses = TRUE;
    Adapter->Diagnostic.Dma64BitAddresses = ConfigInfo->Dma64BitAddresses;
    Adapter->Diagnostic.Dma32BitAddresses =
        ConfigInfo->Dma32BitAddresses ? 1UL : 0UL;

    if (!UfsAllocateWorkspace(Adapter, ConfigInfo)) {
        UfsEnterDiagnosticOnly(
            Adapter,
            UFS_DIAG_STAGE_WORKSPACE_ALLOCATED
            );
        return SP_RETURN_FOUND;
    }
    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_WORKSPACE_ALLOCATED);

    if (!UfsValidateWarmState(Adapter)) {
        UfsEnterDiagnosticOnly(
            Adapter,
            UFS_DIAG_STAGE_FIND_WARM_VALIDATED
            );
        return SP_RETURN_FOUND;
    }
    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_FIND_WARM_VALIDATED);
    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_FIND_ADAPTER_COMPLETE);
    return SP_RETURN_FOUND;
}

static
BOOLEAN
UfsHwInitializeWorker(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    Adapter->Diagnostic.HwInitializeCalls++;
    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_HW_INITIALIZE_ENTERED);
    if (Adapter->DiagnosticOnly) {
        Adapter->DiagnosticEndpointPreserved = TRUE;
        Adapter->Started = TRUE;
        UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_DIAGNOSTIC_ONLY);
        return TRUE;
    }

    if (!UfsValidateWarmState(Adapter)) {
        UfsEnterDiagnosticOnly(
            Adapter,
            UFS_DIAG_STAGE_INIT_WARM_VALIDATED
            );
        Adapter->DiagnosticEndpointPreserved = TRUE;
        Adapter->Started = TRUE;
        return TRUE;
    }
    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_INIT_WARM_VALIDATED);

    if (!UfsProgramTransferList(Adapter)) {
        UfsEnterDiagnosticOnly(
            Adapter,
            UFS_DIAG_STAGE_TRANSFER_LIST_PROGRAMMED
            );
        Adapter->DiagnosticEndpointPreserved = TRUE;
        Adapter->Started = TRUE;
        return TRUE;
    }
    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_TRANSFER_LIST_PROGRAMMED);

    Adapter->DiagnosticOnly = FALSE;
    Adapter->DiagnosticEndpointPreserved = FALSE;
    Adapter->Started = TRUE;
    UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_OPERATIONAL);
    return TRUE;
}

#if UFS_AUTOMATIC_RECOVERY_RESET
/*
 * Legacy compile-time unattended watchdog. Superseded by UfsWitnessTimer below,
 * which subsumes it and is armed at runtime instead. Kept switched off.
 */
#endif

/*
 * Witness timer, and the runtime-armed unattended return.
 *
 * This replaces the old compile-time UFS_AUTOMATIC_RECOVERY_RESET watchdog with
 * something that can be compiled in permanently, because what it does by
 * default is nothing but observe.
 *
 * Two jobs, and the split between them is the safety argument:
 *
 *   ALWAYS   refresh the Windows witness. Processor counts are re-read every
 *            tick because a boot-start driver is loaded while secondaries are
 *            still coming online, so the first reading is a lower bound rather
 *            than an answer. This is pure observation and cannot make a boot
 *            worse.
 *
 *   ARMED    only when TWRP left a valid arm token in PRAM: pet the hardware
 *            watchdog and, once the requested time has elapsed and the media is
 *            idle, return to recovery over the device-verified INFORM3 + PMU
 *            SWRESET path.
 *
 * The old design armed the Samsung WDT on the first tick unconditionally. That
 * is deliberately NOT done here: an armed watchdog on an unarmed boot is a new
 * way to lose the phone for no measurement benefit, so the WDT is only ever
 * armed on a boot that has already asked to be reset.
 *
 * The reboot is gated on write quiescence rather than on WriteMode. In full
 * Windows WriteMode is LIVE from HwInitialize to power-off, so a WriteMode test
 * can never pass and the return would never fire - which is precisely the
 * situation this exists to escape. Reads are safe to interrupt; a read that
 * never completes leaves the device untouched. Writes are not, so the timer
 * keeps ticking until the media has been quiet, and only then resets.
 */
static
VOID
UfsWitnessTimer(
    _In_ PVOID DeviceExtension
    )
{
    PUFS_ADAPTER_EXTENSION Adapter = (PUFS_ADAPTER_EXTENSION)DeviceExtension;

    if (Adapter == NULL ||
        !Adapter->UnattendedArmed ||
        (Adapter->NoPetReason != UFS_NO_PET_NONE)) {
        return;
    }

    Adapter->UnattendedTicks++;
    (VOID)UfsP3AcknowledgePostHandoff(Adapter);

#if UFS_CONTINUOUS_HARDWARE_WATCHDOG
    if (Adapter->WdtArmed) {
        UfsWdtPet(Adapter);
    } else if (!Adapter->Diagnostic.WdtDisarmedTooFast) {
        if (UfsWdtArm(Adapter)) {
            UfsPramArmLowLevelWatchdog(Adapter);
        } else {
            UfsRwd1SetFatalNoPet(
                Adapter,
                UFS_NO_PET_WATCHDOG_INVALID,
                RWD1_REASON_WINDOWS_NO_PET,
                RWD1_PHASE_WINDOWS_RUNNING,
                0UL
                );
        }
    }
    if (Adapter->NoPetReason != UFS_NO_PET_NONE) {
        return;
    }
#endif

    /*
     * Clearing the firmware handoff marker is idempotent and already done in
     * HwInitialize; repeating it here costs one aligned store and keeps the
     * liveness claim true for the whole session.
     */
    UfsPramClearHandoffMarker(Adapter);
    UfsWitnessRefresh(Adapter, UFS_PRAM_WITNESS_FLAG_TICK);

    if (!Adapter->UnattendReturnRequested) {
#if UFS_CONTINUOUS_HARDWARE_WATCHDOG
        /* The 1 Hz callback is now the hardware watchdog's service loop. */
        StorPortNotification(RequestTimerCall, Adapter, UfsWitnessTimer,
                             UFS_UNATTENDED_TICK_US);
#else
        if (Adapter->UnattendedTicks < UFS_WITNESS_OBSERVE_SEC) {
            StorPortNotification(RequestTimerCall, Adapter, UfsWitnessTimer,
                                 UFS_UNATTENDED_TICK_US);
        }
#endif
        return;
    }

    /*
     * Armed from here down.
     *
     * The old design also armed the Samsung hardware watchdog on the first tick.
     * That is deliberately not done, for two reasons. It is a new way to lose the
     * phone if the timer ever stops being delivered, and it buys nothing: a
     * watchdog-origin reset does NOT honour the INFORM3 boot reason, so it
     * re-enters UEFI rather than reaching TWRP. Hangs are already covered
     * upstream - the firmware's boot-attempt counter diverts to TWRP when Windows
     * repeatedly fails to get anywhere, and the handoff marker catches a wedge at
     * ExitBootServices. Both are strictly better backstops than a reset this
     * driver cannot aim.
     */
    if (Adapter->UnattendedTicks < Adapter->UnattendReturnSec) {
        StorPortNotification(RequestTimerCall, Adapter, UfsWitnessTimer,
                             UFS_UNATTENDED_TICK_US);
        return;
    }

    if ((Adapter->WritesIssued != 0UL) &&
        ((Adapter->UnattendedTicks - Adapter->LastWriteTick) <
             UFS_BOOT_DIAG_WRITE_IDLE_SEC)) {
        StorPortNotification(RequestTimerCall, Adapter, UfsWitnessTimer,
                             UFS_UNATTENDED_TICK_US);
        return;
    }

    /*
     * Same close-out the vendor reboot CDB performs: disarm writes and close the
     * crash latch, so this boot is not misread as a bugcheck and does not lock
     * the next boot out of its write probe.
     */
    Adapter->UnattendedArmed = FALSE;
    Adapter->WriteMode = UFS_WRITE_MODE_DISARMED;
    UfsPramLatchComplete(Adapter);

    /*
     * Final witness write before the reset. Everything above may have changed
     * since the last tick, and after the reset this record is the only evidence
     * the boot happened at all.
     */
    UfsWitnessRefresh(Adapter, UFS_PRAM_WITNESS_FLAG_TICK);
    UfsRefreshDiagnostic(Adapter);

    UfsPramSnapshot(Adapter, "UFSAUTO");
    UfsPmuRebootToRecovery(Adapter);
}

/*
 * Start or resume continuous service. Hardware ownership is established before
 * the timer is requested, so a DPC stall in the scheduling window remains
 * bounded by the hardware counter.
 */
static
VOID
UfsRuntimeWatchdogStart(
    _Inout_ PUFS_ADAPTER_EXTENSION Adapter
    )
{
    if (Adapter == NULL ||
        (Adapter->NoPetReason != UFS_NO_PET_NONE)) {
        return;
    }

#if UFS_CONTINUOUS_HARDWARE_WATCHDOG
    if (!Adapter->WdtArmed &&
        !Adapter->Diagnostic.WdtDisarmedTooFast &&
        UfsWdtArm(Adapter)) {
        UfsPramArmLowLevelWatchdog(Adapter);
    }
#endif

    if (!Adapter->UnattendedArmed) {
        Adapter->UnattendedArmed = TRUE;
        Adapter->UnattendedTicks = 0UL;
        Adapter->LastWriteTick = 0UL;
    }
    StorPortNotification(
        RequestTimerCall,
        Adapter,
        UfsWitnessTimer,
        UFS_UNATTENDED_TICK_US
        );
}

#if UFS_AUTOMATIC_RECOVERY_RESET
static
VOID
UfsUnattendedTimer(
    _In_ PVOID DeviceExtension
    )
{
    PUFS_ADAPTER_EXTENSION Adapter = (PUFS_ADAPTER_EXTENSION)DeviceExtension;

    if (Adapter == NULL || !Adapter->UnattendedArmed) {
        return;
    }

    /*
     * Arm on the FIRST tick rather than at initialisation. Reaching this line is
     * proof that Storport actually delivers the timer, so a boot where the timer
     * never fires can never leave a running watchdog with nothing servicing it.
     * Every later tick just reloads it.
     *
     * The same proof retires the firmware's Windows-handoff marker: a kernel
     * that is alive enough to schedule our timer did not wedge at
     * ExitBootServices, so the next UEFI entry must boot Windows rather than
     * divert to TWRP. Clearing it here - and only here - is what makes
     * STAR2LTE_AUTO_RECOVERY_ON_HANG safe to enable.
     */
    if (!Adapter->WdtArmed) {
        UfsPramClearHandoffMarker(Adapter);
        UfsWdtArm(Adapter);
    } else {
        UfsWdtPet(Adapter);
    }

    Adapter->UnattendedTicks++;

#if UFS_BOOT_DIAG_MODE
    /*
     * Numbered progress snapshot. This is the measurement the installed-OS hang
     * needs: one sample says what the storage stack had done by the time the
     * machine stopped, but a series says WHEN it stopped and whether it stopped
     * at all. STARTIO frozen between two snapshots means the class driver is no
     * longer issuing anything - a wait. STARTIO still climbing means a retry
     * loop, and MS10 climbing with it names MODE SENSE as the command being
     * retried.
     *
     * Cheap enough to run every few seconds: it is one PRAM record, written to
     * a ring the firmware has already handed over, with no allocation and no
     * device I/O.
     */
    if ((Adapter->UnattendedTicks % UFS_BOOT_DIAG_SNAP_SEC) == 0UL) {
        UfsRefreshDiagnostic(Adapter);
        UfsPramSnapshot(Adapter, "UFSTICK");
    }

    if (Adapter->UnattendedTicks < UFS_BOOT_DIAG_REBOOT_SEC) {
        StorPortNotification(RequestTimerCall, Adapter, UfsUnattendedTimer,
                             UFS_UNATTENDED_TICK_US);
        return;
    }
#else
    if (Adapter->UnattendedTicks < UFS_UNATTENDED_REBOOT_SEC) {
        StorPortNotification(RequestTimerCall, Adapter, UfsUnattendedTimer,
                             UFS_UNATTENDED_TICK_US);
        return;
    }
#endif

#if UFS_BOOT_DIAG_MODE
    /*
     * Write-quiescence guard.
     *
     * Same intent as the arm-state test below - never reset part-way through a
     * write - but expressed as a fact about I/O rather than about configuration.
     * In full-Windows mode WriteMode is LIVE for the entire session, so the
     * arm-state test can never pass and the backstop would never fire, which is
     * exactly the situation this mode exists to escape. LastWriteTick is stamped
     * where WritesIssued is incremented, so this waits for the media to be
     * genuinely idle instead of waiting for a flag that will not change.
     */
    if ((Adapter->WritesIssued != 0UL) &&
        ((Adapter->UnattendedTicks - Adapter->LastWriteTick) <
             UFS_BOOT_DIAG_WRITE_IDLE_SEC)) {
        StorPortNotification(RequestTimerCall, Adapter, UfsUnattendedTimer,
                             UFS_UNATTENDED_TICK_US);
        return;
    }
#else
    /*
     * Never reset out from under a live write. WritesIssued has been 0 on every
     * boot ever measured, so this guard has not yet been load-bearing - but a
     * reset part-way through a write is precisely the unrecoverable-corruption
     * case the write safeguards exist to prevent, so it is checked rather than
     * assumed. Reads are safe to interrupt: a read that never completes leaves
     * the device untouched. Keep ticking so the phone still returns once the
     * write finishes and disarms.
     */
    if (Adapter->WriteMode == UFS_WRITE_MODE_LIVE) {
        StorPortNotification(RequestTimerCall, Adapter, UfsUnattendedTimer,
                             UFS_UNATTENDED_TICK_US);
        return;
    }
#endif

    /*
     * Same close-out the vendor reboot CDB performs: disarm writes and close the
     * crash latch, so this boot is not misread as a bugcheck and does not lock
     * the next boot out of its write probe.
     */
    Adapter->UnattendedArmed = FALSE;
    Adapter->WriteMode = UFS_WRITE_MODE_DISARMED;
    UfsPramLatchComplete(Adapter);

    UfsRefreshDiagnostic(Adapter);

    /*
     * Distinct tag so PRAM alone attributes the return: =UFSAUTO means the
     * user-mode chain failed and this backstop fired, =UFSREBOOT alone means the
     * normal path worked.
     */
    UfsPramSnapshot(Adapter, "UFSAUTO");
    UfsPmuRebootToRecovery(Adapter);
}
#endif

BOOLEAN
UfsHwInitialize(
    _In_ PVOID DeviceExtension
    )
{
    PUFS_ADAPTER_EXTENSION Adapter = (PUFS_ADAPTER_EXTENSION)DeviceExtension;
    BOOLEAN Result;

    UfsCleanRecoveryClear(Adapter);

    /*
     * RWD1 ownership is authoritative. The legacy ARM1/WAR1 record is mirrored
     * only after this transaction and live watchdog takeover both succeed.
     */
#if UFS_FIRMWARE_WDT_BRIDGE
    if (UfsRwd1TakeOwnership(Adapter)) {
        UfsWdtTakeFirmwareBridgeOwnership(Adapter);
    } else {
        /*
         * Do not stop the inherited firmware watchdog. With no authoritative
         * ownership transaction there is no safe petter; expiry is the
         * recovery mechanism.
         */
        Adapter->NoPetReason = UFS_NO_PET_WATCHDOG_INVALID;
    }
    Adapter->UnattendReturnSec = UfsUnattendConsumeToken(Adapter);
    Adapter->UnattendReturnRequested =
        (Adapter->UnattendReturnSec != 0UL) && (Adapter->Pmu != NULL);
#endif

    /*
     * Entry marker, written BEFORE any work.
     *
     * =UFSINIT is only emitted at the END of this routine, so its absence has
     * been ambiguous: it cannot distinguish "Storport never called the
     * miniport" from "the miniport was called and hung inside initialisation".
     * That ambiguity is what the intermittent logo hang keeps running into -
     * pmsg carries the full firmware trace and no UFS marker at all, which is
     * consistent with both.
     *
     * Emitting a marker here makes the next failure self-locating:
     *   neither marker -> the driver never ran; look at winload/Storport
     *   =UFSENTRY only -> the driver ran and hung inside initialisation
     *   both markers   -> initialisation completed; look later
     *
     * Diagnostic only: one PRAM record, no behavioural change.
     */
    UfsPramSnapshot(Adapter, "UFSENTRY");

    if (!UfsBugcheckContractReady(Adapter)) {
        /*
         * Keep the diagnostic endpoint/lifetime, not an operational disk.
         * Ownership was attempted above; never clear its fatal/no-pet latch.
         */
        UfsEnterDiagnosticOnly(Adapter, UFS_DIAG_STAGE_HW_INITIALIZE_ENTERED);
    }
    Result = UfsHwInitializeWorker(Adapter);

    if (!Result || Adapter->FatalError) {
        UfsRwd1SetFatalNoPet(
            Adapter,
            UFS_NO_PET_STORAGE_FATAL,
            RWD1_REASON_WINDOWS_STORAGE_FATAL,
            RWD1_PHASE_WINDOWS_STORAGE_FATAL,
            Adapter->Diagnostic.FailureStage
            );
    } else if (Adapter->Rwd1Owned) {
        if (!UfsRwd1Transition(
                Adapter,
                RWD1_STATE_WINDOWS_OWNED,
                RWD1_PHASE_WINDOWS_RUNNING,
                RWD1_REASON_NONE,
                0UL,
                TRUE)) {
            UfsRwd1SetFatalNoPet(
                Adapter,
                UFS_NO_PET_WATCHDOG_INVALID,
                RWD1_REASON_WINDOWS_NO_PET,
                RWD1_PHASE_WINDOWS_INITIALIZING,
                0UL
                );
            Result = FALSE;
        } else {
            (VOID)UfsP3AcknowledgePostHandoff(Adapter);
        }
    }

    //
    // Tell the firmware this boot is worth keeping, BEFORE writing the record: reaching
    // here means initialisation ran to completion on some path, which is exactly the
    // liveness the firmware's boot-attempt counter is asking about. Ordering it first also
    // means the value lands in the record below.
    //
    UfsPramAcknowledgeBoot(Adapter);

    //
    // One record per initialisation, on every path, so the persistent log
    // always shows whether the miniport got this far. A boot whose PRAM has
    // =UFSINIT but no =UFSDIAG proves Storport never dispatched a command.
    //
    Adapter->Diagnostic.PramRecords = Adapter->PramRecords;
    UfsPramSnapshot(Adapter, "UFSINIT");
    Adapter->Diagnostic.PramRecords = Adapter->PramRecords;

    /*
     * Arm the unattended-return backstop on EVERY path, including the
     * diagnostic-only one. A boot whose adapter came up degraded is exactly the
     * boot most likely to strand the phone, so it is the one that most needs a
     * guaranteed way back to TWRP. Requires only the PMU mapping, which is
     * independent of adapter health.
     */
#if UFS_FULL_WINDOWS_MODE
    /*
     * Both backstops are off in this build, and they have to go together.
     *
     * The unattended timer exists to guarantee a stranded experiment finds its
     * way back to TWRP after 900 s. Under a real installation that same
     * guarantee is a fault: it would reset the phone a quarter of an hour into
     * every session, while Windows is running, with dirty metadata in flight.
     *
     * Not arming it also settles the hardware watchdog, because UfsWdtArm is
     * only ever reached from inside this timer's first tick. If the timer never
     * runs, the Samsung WDT is never armed and so never needs petting - there
     * is no window in which an unpetted counter can reset the SoC.
     *
     * Writes are armed here instead. UFS_WRITE_MODE_DISARMED is 0 and Storport
     * zeroes the extension, so read-only is what this driver is by default;
     * making it a disk takes an explicit statement, and this is it. The arming
     * IOCTL cannot serve here - nothing runs UfsDiag.exe during a real boot,
     * which is precisely why boot 1 refused all 28 of bcdboot's writes.
     */
    Adapter->WriteMode = UFS_WRITE_MODE_LIVE;

    /*
     * Clearing the firmware handoff marker was a side effect of the unattended
     * timer's first tick, so switching that timer off silently took it with it.
     * It has to be done explicitly here or the marker stays set for the whole
     * session, and every Windows boot looks to UEFI like a hang that never got
     * anywhere - which, the moment STAR2LTE_AUTO_RECOVERY_ON_HANG is turned on,
     * diverts the NEXT boot to TWRP and makes booting Windows twice in a row
     * impossible.
     *
     * Reaching HwInitialize is the same liveness proof the timer tick was
     * standing in for: the kernel is up, Storport is dispatching, and this
     * driver is running, so the handoff plainly did not wedge. One naturally
     * aligned 32-bit store to a word outside the ring.
     */
    UfsPramClearHandoffMarker(Adapter);
    (VOID)UfsPmuSetNormalBootReason(
        Adapter,
        "UFSNORMAL",
        UFS_PRAM_NORMAL_PHASE_INIT
        );

    /*
     * Start the witness, and honour an unattended-return request if TWRP left
     * one.
     *
     * Order matters. The token is consumed BEFORE the first witness write so the
     * ARMED flag is visible in the very first record: if this boot then wedges
     * before any tick, the retained record still says whether the return had
     * been requested, which is the difference between "the arming failed" and
     * "the boot failed".
     *
     * The timer itself is started unconditionally. Unarmed it only observes, so
     * there is no path by which starting it can reset anything.
     */
    if (Adapter->UnattendReturnSec == 0UL) {
        Adapter->UnattendReturnSec = UfsUnattendConsumeToken(Adapter);
    }
    Adapter->UnattendReturnRequested =
        (Adapter->UnattendReturnSec != 0UL) && (Adapter->Pmu != NULL);
    if (Adapter->UnattendReturnRequested) {
        Adapter->WitnessFlags |= UFS_PRAM_WITNESS_FLAG_ARMED;
    }

    UfsWitnessRefresh(Adapter, UFS_PRAM_WITNESS_FLAG_INIT);

    UfsRuntimeWatchdogStart(Adapter);

#if UFS_AUTOMATIC_RECOVERY_RESET && UFS_BOOT_DIAG_MODE
    /*
     * Arm the timer anyway in diagnostic mode.
     *
     * The comment above is right that an unattended reset is a fault under a
     * real installation - but there is no installation to protect yet: the
     * installed OS reaches the logo and stops, and the only reason that hang is
     * undiagnosable is that ending it needs the power button, which loses the
     * PRAM ring along with the evidence.
     *
     * With the timer armed the driver ends the hang itself, on a warm path, so
     * the ring survives and carries the snapshot series that shows where I/O
     * stopped. The hardware watchdog it also arms is petted by this same timer,
     * so it introduces no new way to lose the phone.
     */
    if (Adapter->Pmu != NULL && !Adapter->UnattendedArmed) {
        Adapter->UnattendedArmed = TRUE;
        Adapter->UnattendedTicks = 0;
        Adapter->LastWriteTick = 0;
        StorPortNotification(RequestTimerCall, Adapter, UfsUnattendedTimer,
                             UFS_UNATTENDED_TICK_US);
    }
#endif
#else
#if UFS_AUTOMATIC_RECOVERY_RESET
    if (Adapter->Pmu != NULL && !Adapter->UnattendedArmed) {
        Adapter->UnattendedArmed = TRUE;
        Adapter->UnattendedTicks = 0;
        StorPortNotification(RequestTimerCall, Adapter, UfsUnattendedTimer,
                             UFS_UNATTENDED_TICK_US);
    }
#endif
#endif

    return Result;
}

BOOLEAN
UfsHwStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb
    )
{
    PUFS_ADAPTER_EXTENSION Adapter = (PUFS_ADAPTER_EXTENSION)DeviceExtension;
    UCHAR SrbStatus;

    Adapter->Diagnostic.StartIoRequests++;
    Adapter->Diagnostic.LastSrbFunction = (ULONG)Srb->Function;

#if UFS_AUTOMATIC_RECOVERY_RESET && UFS_BOOT_DIAG_MODE
    /*
     * Installed-OS progress sampler and warm-return trigger.
     *
     * The Storport timer callback is never delivered in the logo stall, but
     * StartIo is: boot 20 reached 0x50D3 requests. Sampling here therefore
     * cannot be starved by the condition being measured. The final reboot is
     * issued only while handling a non-write EXECUTE_SCSI request. This adapter
     * owns one transfer slot, so entry into the next StartIo also proves the
     * previous command has completed; no media write can be in flight.
     */
    if ((Adapter->Diagnostic.StartIoRequests %
             UFS_BOOT_DIAG_IO_SNAP_INTERVAL) == 0UL) {
        UfsRefreshDiagnostic(Adapter);
        UfsPramSnapshot(Adapter, "UFSIO");
    }

    if (!Adapter->IoDiagRebootIssued &&
        (Adapter->Diagnostic.StartIoRequests >=
             UFS_BOOT_DIAG_IO_REBOOT_THRESHOLD) &&
        (Srb->Function == SRB_FUNCTION_EXECUTE_SCSI) &&
        (Srb->CdbLength != 0U) &&
        !UfsIsWriteOpcode(Srb->Cdb[0])) {
        Adapter->IoDiagRebootIssued = TRUE;
        UfsRefreshDiagnostic(Adapter);
        UfsPramSnapshot(Adapter, "UFSIOEND");
        UfsPmuRebootToRecovery(Adapter);
    }
#endif

    switch (Srb->Function) {
    case SRB_FUNCTION_IO_CONTROL:
        SrbStatus = UfsHandleIoControl(Adapter, Srb);
        break;

    case SRB_FUNCTION_EXECUTE_SCSI:
        SrbStatus = UfsExecuteScsi(Adapter, Srb);
        break;

    case SRB_FUNCTION_SHUTDOWN:
        UfsCleanRecoveryBeginShutdown(Adapter);
        UfsRuntimeWatchdogBeginShutdown(Adapter);
        (VOID)UfsPmuSetNormalBootReason(
            Adapter,
            "UFSSRBSHUT",
            UFS_PRAM_NORMAL_PHASE_STOP
            );
        SrbStatus = SRB_STATUS_SUCCESS;
        break;

    case SRB_FUNCTION_FLUSH:
    case SRB_FUNCTION_PNP:
    case SRB_FUNCTION_POWER:
        SrbStatus = SRB_STATUS_SUCCESS;
        break;

    default:
        Adapter->RejectedCommands++;
        SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    UfsCompleteRequest(Adapter, Srb, SrbStatus);
    return TRUE;
}

BOOLEAN
UfsHwInterrupt(
    _In_ PVOID DeviceExtension
    )
{
    PUFS_ADAPTER_EXTENSION Adapter =
        (PUFS_ADAPTER_EXTENSION)DeviceExtension;

    InterlockedIncrement(&Adapter->InterruptCallbacks);
    return FALSE;
}

BOOLEAN
UfsHwResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId
    )
{
    PUFS_ADAPTER_EXTENSION Adapter = (PUFS_ADAPTER_EXTENSION)DeviceExtension;

    UNREFERENCED_PARAMETER(PathId);
    if (Adapter->DiagnosticOnly) {
        Adapter->Started = TRUE;
        return TRUE;
    }

    /*
     * Quiesce the list and report success unless the controller refused to
     * release it. Reporting failure here would contradict the recovery model:
     * storport would tear the adapter down for a condition the next command
     * re-arms out of.
     */
    UfsContainController(Adapter, UFS_DIAG_FAILURE_TIMEOUT_CONTAINMENT);
    return (BOOLEAN)(!Adapter->FatalError);
}

SCSI_ADAPTER_CONTROL_STATUS
UfsHwAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters
    )
{
    PUFS_ADAPTER_EXTENSION Adapter = (PUFS_ADAPTER_EXTENSION)DeviceExtension;

    switch (ControlType) {
    case ScsiQuerySupportedControlTypes:
    {
        PSCSI_SUPPORTED_CONTROL_TYPE_LIST ControlList =
            (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        ULONG Index;

        for (Index = 0; Index < ControlList->MaxControlType; Index++) {
            ControlList->SupportedTypeList[Index] = FALSE;
        }
        if (ScsiQuerySupportedControlTypes < ControlList->MaxControlType) {
            ControlList->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE;
        }
        if (ScsiStopAdapter < ControlList->MaxControlType) {
            ControlList->SupportedTypeList[ScsiStopAdapter] = TRUE;
        }
        if (ScsiRestartAdapter < ControlList->MaxControlType) {
            ControlList->SupportedTypeList[ScsiRestartAdapter] = TRUE;
        }
        return ScsiAdapterControlSuccess;
    }

    case ScsiStopAdapter:
        if (Adapter->CleanRecoveryState != UFS_CLEAN_RECOVERY_COMMITTED) {
            UfsCleanRecoveryClear(Adapter);
        }
        UfsRuntimeWatchdogBeginShutdown(Adapter);

        /*
         * Best effort only. A missing PMU mapping must never skip the existing
         * stop sequence or turn a reset fix into a failed device power IRP.
         */
        (VOID)UfsPmuSetNormalBootReason(
            Adapter,
            "UFSSTOPNORM",
            UFS_PRAM_NORMAL_PHASE_STOP
            );

        /*
         * Power transitions and surprise removal both land here. Whatever the
         * operator armed, they armed it for the adapter that is about to stop -
         * carrying that consent across a stop/start cycle would let a write go
         * live against a controller nobody has looked at since.
         */
        Adapter->WriteMode = UFS_WRITE_MODE_DISARMED;
        if (!Adapter->DiagnosticOnly && Adapter->OwnsTransferList) {
            if (!UfsRestoreWarmHandoff(Adapter)) {
                UfsRwd1SetFatalNoPet(
                    Adapter,
                    UFS_NO_PET_RESTART_FAILED,
                    RWD1_REASON_WINDOWS_STORAGE_FATAL,
                    RWD1_PHASE_WINDOWS_SHUTDOWN,
                    0x52535452UL
                    );
                return ScsiAdapterControlUnsuccessful;
            }
            UfsZeroUncached(Adapter->Utrl, UFS_UTRL_REGION_SIZE);
            UfsZeroUncached(Adapter->Ucd, UFS_UCD_REGION_SIZE);
            UfsZeroUncached(Adapter->Bounce, UFS_BOUNCE_SIZE);
            KeMemoryBarrier();
        }
        if (!UfsRuntimeWatchdogFinalStop(Adapter)) {
            return ScsiAdapterControlUnsuccessful;
        }
        Adapter->Started = FALSE;
        return ScsiAdapterControlSuccess;

    case ScsiRestartAdapter:
        if (Adapter->CleanRecoveryState != UFS_CLEAN_RECOVERY_IDLE) {
            UfsCleanRecoveryClear(Adapter);
            (VOID)UfsPmuSetNormalBootReason(
                Adapter, "UFSCLEANRESUME", UFS_PRAM_NORMAL_PHASE_INIT);
        }

        /*
         * A timed-out command was never aborted device-side. Reprogramming the
         * transfer list would reuse its UCD, PRDT and bounce memory while the
         * old command may still drain, which is the observed silent-write
         * corruption mechanism. Only a new boot may establish a clean handoff.
         */
        if (Adapter->FatalError || !UfsRwd1Restart(Adapter, FALSE)) {
            return ScsiAdapterControlUnsuccessful;
        }
#if UFS_FULL_WINDOWS_MODE
        /*
         * A restart is a power transition, not a change of intent. Disarming
         * here would silently drop the system disk back to read-only in the
         * middle of a session, which Windows cannot survive and would report
         * only as unexplained I/O failures.
         */
        Adapter->WriteMode = UFS_WRITE_MODE_LIVE;
#else
        Adapter->WriteMode = UFS_WRITE_MODE_DISARMED;
#endif
        if (Adapter->DiagnosticOnly) {
            Adapter->DiagnosticEndpointPreserved = TRUE;
            Adapter->Started = TRUE;
            UfsSetDiagnosticStage(
                Adapter,
                UFS_DIAG_STAGE_DIAGNOSTIC_ONLY
                );
            if (!UfsRwd1Restart(Adapter, TRUE)) {
                Adapter->Started = FALSE;
                UfsRwd1SetFatalNoPet(
                    Adapter,
                    UFS_NO_PET_RESTART_FAILED,
                    RWD1_REASON_WINDOWS_NO_PET,
                    RWD1_PHASE_WINDOWS_INITIALIZING,
                    0UL
                    );
                return ScsiAdapterControlUnsuccessful;
            }
            UfsRuntimeWatchdogStart(Adapter);
            return ScsiAdapterControlSuccess;
        }
        if (!UfsValidateWarmState(Adapter)) {
            UfsEnterDiagnosticOnly(
                Adapter,
                UFS_DIAG_STAGE_INIT_WARM_VALIDATED
                );
            Adapter->DiagnosticEndpointPreserved = TRUE;
            Adapter->Started = FALSE;
            UfsRwd1SetFatalNoPet(
                Adapter,
                UFS_NO_PET_RESTART_FAILED,
                RWD1_REASON_WINDOWS_STORAGE_FATAL,
                RWD1_PHASE_WINDOWS_STORAGE_FATAL,
                UFS_DIAG_STAGE_INIT_WARM_VALIDATED
                );
            return ScsiAdapterControlUnsuccessful;
        }
        if (!UfsProgramTransferList(Adapter)) {
            UfsEnterDiagnosticOnly(
                Adapter,
                UFS_DIAG_STAGE_TRANSFER_LIST_PROGRAMMED
                );
            Adapter->DiagnosticEndpointPreserved = TRUE;
            Adapter->Started = FALSE;
            UfsRwd1SetFatalNoPet(
                Adapter,
                UFS_NO_PET_RESTART_FAILED,
                RWD1_REASON_WINDOWS_STORAGE_FATAL,
                RWD1_PHASE_WINDOWS_STORAGE_FATAL,
                UFS_DIAG_STAGE_TRANSFER_LIST_PROGRAMMED
                );
            return ScsiAdapterControlUnsuccessful;
        }
        Adapter->FatalError = FALSE;
        Adapter->RearmAttempts = 0;
        Adapter->DiagnosticEndpointPreserved = FALSE;
        Adapter->Started = TRUE;
        UfsSetDiagnosticStage(Adapter, UFS_DIAG_STAGE_OPERATIONAL);
        if (!UfsRwd1Restart(Adapter, TRUE)) {
            Adapter->Started = FALSE;
            UfsRwd1SetFatalNoPet(
                Adapter,
                UFS_NO_PET_RESTART_FAILED,
                RWD1_REASON_WINDOWS_NO_PET,
                RWD1_PHASE_WINDOWS_INITIALIZING,
                0UL
                );
            return ScsiAdapterControlUnsuccessful;
        }
        UfsRuntimeWatchdogStart(Adapter);
        return ScsiAdapterControlSuccess;

    default:
        return ScsiAdapterControlUnsuccessful;
    }
}
