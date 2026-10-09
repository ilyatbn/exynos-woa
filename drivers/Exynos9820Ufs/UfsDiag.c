#include <Windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "Exynos9820UfsDiag.h"

const ULONG UfsDiagBinaryContract[] = UFS_DIAG_BINARY_CONTRACT_INITIALIZER;

typedef struct _UFS_DIAGNOSTIC_PACKET {
    SRB_IO_CONTROL Control;
    UFS_DIAGNOSTIC_DATA Data;
} UFS_DIAGNOSTIC_PACKET;

typedef struct _UFS_FAILURE_NAME {
    ULONGLONG Mask;
    const char *Name;
} UFS_FAILURE_NAME;

static const char *
ExecRejectName(
    ULONG Code
    )
{
    switch (Code) {
    case UFS_EXEC_REJECT_NONE:            return "NONE";
    case UFS_EXEC_REJECT_DIAGNOSTIC_ONLY: return "DIAGNOSTIC_ONLY";
    case UFS_EXEC_REJECT_NOT_STARTED:     return "NOT_STARTED";
    case UFS_EXEC_REJECT_FATAL_ERROR:     return "FATAL_ERROR";
    case UFS_EXEC_REJECT_BAD_NEXUS:       return "BAD_NEXUS";
    case UFS_EXEC_REJECT_CLASSIFY:        return "CLASSIFY";
    case UFS_EXEC_REJECT_REARM_EXHAUSTED: return "REARM_EXHAUSTED";
    case UFS_EXEC_REJECT_REARM_FAILED:    return "REARM_FAILED";
    case UFS_EXEC_REJECT_PRECONDITION:    return "PRECONDITION";
    case UFS_EXEC_REJECT_LOCAL_COMPLETE:  return "LOCAL_COMPLETE";
    case UFS_EXEC_REJECT_ISSUED:          return "ISSUED";
    case UFS_EXEC_REJECT_REARM_BUSY:      return "REARM_BUSY";
    case UFS_EXEC_REJECT_WRITE_DISARMED:  return "WRITE_DISARMED";
    case UFS_EXEC_REJECT_WRITE_FENCE:     return "WRITE_FENCE";
    case UFS_EXEC_REJECT_WRITE_DRY_RUN:   return "WRITE_DRY_RUN";
    case UFS_EXEC_REJECT_WRITE_VERIFY:    return "WRITE_VERIFY";
    case UFS_EXEC_REJECT_WRITE_CRASH_LOCKOUT:
                                          return "WRITE_CRASH_LOCKOUT";
    default:                              return "UNKNOWN";
    }
}

/*
 * Name the SCSI opcodes the allowlist can refuse.
 *
 * A bare hex byte is not evidence anybody can act on - the whole point of the
 * rejected-opcode log is to decide, per opcode, whether Windows genuinely needs
 * that command for partition enumeration. Rendering the value with its meaning
 * is the same lesson EXECREJ=10 taught: a number under a legend that does not
 * explain it survives for many boots as a false open item.
 *
 * Only opcodes this driver can actually refuse are listed. Anything on
 * UfsClassifyCdb's allowlist never reaches UfsRecordRejectedOpcode, so its
 * absence here is correct rather than an omission.
 */
static const char *
ScsiOpcodeName(
    UCHAR Opcode
    )
{
    switch (Opcode) {
    /*
     * Opcodes UfsClassifyCdb ADMITS. These reach the rejected-opcode log via the
     * allowlist-reject block, where an admitted opcode is refused because it
     * failed a PREDICATE (allocation length, transfer bounds, service action)
     * rather than because the opcode itself is forbidden.
     *
     * This half of the table was originally omitted on the reasoning that "an
     * allowlisted opcode never reaches the log". That stopped being true when
     * reject logging moved into the allowlist-reject path, and V16 proved it:
     * ROPS slot 0 came back 0x12, which rendered as UNKNOWN - the single opcode
     * the whole partition-enumeration investigation is about.
     */
    case 0x00: return "TEST_UNIT_READY";
    case 0x03: return "REQUEST_SENSE";
    case 0x08: return "READ_6";
    case 0x12: return "INQUIRY";
    case 0x1A: return "MODE_SENSE_6";
    case 0x1E: return "PREVENT_ALLOW_MEDIUM_REMOVAL";
    case 0x25: return "READ_CAPACITY_10";
    case 0x28: return "READ_10";
    case 0x5A: return "MODE_SENSE_10";
    case 0xA0: return "REPORT_LUNS";
    case 0xA8: return "READ_12";

    /* Opcodes UfsClassifyCdb never admits - refused on the opcode alone. */
    case 0x04: return "FORMAT_UNIT";
    case 0x15: return "MODE_SELECT_6";
    case 0x16: return "RESERVE_6";
    case 0x17: return "RELEASE_6";
    case 0x1B: return "START_STOP_UNIT";
    case 0x1D: return "SEND_DIAGNOSTIC";
    case 0x2A: return "WRITE_10";
    case 0x2F: return "VERIFY_10";
    case 0x35: return "SYNCHRONIZE_CACHE_10";
    case 0x37: return "READ_DEFECT_DATA_10";
    case 0x3B: return "WRITE_BUFFER";
    case 0x3C: return "READ_BUFFER";
    case 0x41: return "WRITE_SAME_10";
    case 0x42: return "UNMAP";
    case 0x48: return "SANITIZE";
    case 0x4D: return "LOG_SENSE";
    case 0x55: return "MODE_SELECT_10";
    case 0x5E: return "PERSISTENT_RESERVE_IN";
    case 0x5F: return "PERSISTENT_RESERVE_OUT";
    case 0x85: return "ATA_PASS_THROUGH_16";
    case 0x88: return "READ_16";
    case 0x8A: return "WRITE_16";
    case 0x8F: return "VERIFY_16";
    case 0x91: return "SYNCHRONIZE_CACHE_16";
    case 0x93: return "WRITE_SAME_16";
    case 0x9E: return "SERVICE_ACTION_IN_16";
    case 0xA1: return "ATA_PASS_THROUGH_12";
    case 0xA2: return "SECURITY_PROTOCOL_IN";
    case 0xA3: return "MAINTENANCE_IN";
    case 0xA4: return "MAINTENANCE_OUT";
    case 0xB5: return "SECURITY_PROTOCOL_OUT";
    default:   return "UNKNOWN";
    }
}

static const char *
WritePhaseName(
    ULONG Code
    )
{
    switch (Code) {
    case UFS_PHASE_IDLE:             return "IDLE";
    case UFS_PHASE_GATE_ENTERED:     return "GATE_ENTERED";
    case UFS_PHASE_CRASH_LOCKOUT:    return "CRASH_LOCKOUT";
    case UFS_PHASE_LATCH_SET:        return "LATCH_SET";
    case UFS_PHASE_DISARMED_PRE:     return "DISARMED_PRE";
    case UFS_PHASE_DISARMED_POST:    return "DISARMED_POST";
    case UFS_PHASE_DRY_RUN_PRE:      return "DRY_RUN_PRE";
    case UFS_PHASE_DRY_RUN_POST:     return "DRY_RUN_POST";
    case UFS_PHASE_LIVE_ENTERED:     return "LIVE_ENTERED";
    case UFS_PHASE_ARM_ENTER:        return "ARM_ENTER";
    case UFS_PHASE_ARM_MODE_SET:     return "ARM_MODE_SET";
    case UFS_PHASE_ARM_PRE_SNAPSHOT: return "ARM_PRE_SNAPSHOT";
    case UFS_PHASE_ARM_RETURNING:    return "ARM_RETURNING";
    case UFS_PHASE_CLASSIFY_REJECT:  return "CLASSIFY_REJECT";
    case UFS_PHASE_VENDOR_DIAG:      return "VENDOR_DIAG";
    case UFS_PHASE_VENDOR_REBOOT:    return "VENDOR_REBOOT";
    default:                         return "UNKNOWN";
    }
}

static const UFS_FAILURE_NAME FailureNames[] = {
    {UFS_DIAG_FAILURE_ACCESS_RANGES_NULL, "ACCESS_RANGES_NULL"},
    {UFS_DIAG_FAILURE_HCI_RANGE_MISSING, "HCI_RANGE_MISSING"},
    {UFS_DIAG_FAILURE_UNIPRO_RANGE_MISSING, "UNIPRO_RANGE_MISSING"},
    {UFS_DIAG_FAILURE_PMA_RANGE_MISSING, "PMA_RANGE_MISSING"},
    {UFS_DIAG_FAILURE_UFSP_RANGE_MISSING, "UFSP_RANGE_MISSING"},
    {UFS_DIAG_FAILURE_HCI_MAP, "HCI_MAP"},
    {UFS_DIAG_FAILURE_WORKSPACE_ALLOCATION, "WORKSPACE_ALLOCATION"},
    {UFS_DIAG_FAILURE_WORKSPACE_LAYOUT, "WORKSPACE_LAYOUT"},
    {UFS_DIAG_FAILURE_UTRL_PHYSICAL, "UTRL_PHYSICAL"},
    {UFS_DIAG_FAILURE_UCD_PHYSICAL, "UCD_PHYSICAL"},
    {UFS_DIAG_FAILURE_BOUNCE_PHYSICAL, "BOUNCE_PHYSICAL"},
    {UFS_DIAG_FAILURE_HOST_DISABLED, "HOST_DISABLED"},
    {UFS_DIAG_FAILURE_HOST_STATUS, "HOST_STATUS"},
    {UFS_DIAG_FAILURE_DOORBELL_BUSY, "DOORBELL_BUSY"},
    {UFS_DIAG_FAILURE_INTERRUPT_AGGREGATION, "INTERRUPT_AGGREGATION"},
    {UFS_DIAG_FAILURE_INTERRUPT_ENABLE, "INTERRUPT_ENABLE"},
    {UFS_DIAG_FAILURE_CAPABILITIES, "CAPABILITIES"},
    {UFS_DIAG_FAILURE_TX_PRDT_SIZE, "TX_PRDT_SIZE"},
    {UFS_DIAG_FAILURE_RX_PRDT_SIZE, "RX_PRDT_SIZE"},
    {UFS_DIAG_FAILURE_DATA_REORDER, "DATA_REORDER"},
    {UFS_DIAG_FAILURE_AXI_DMA_BURST, "AXI_DMA_BURST"},
    {UFS_DIAG_FAILURE_DMA_ADDRESS_WIDTH, "DMA_ADDRESS_WIDTH"},
    {UFS_DIAG_FAILURE_PROGRAM_PRECONDITION, "PROGRAM_PRECONDITION"},
    {UFS_DIAG_FAILURE_PROGRAM_DOORBELL_BUSY, "PROGRAM_DOORBELL_BUSY"},
    {UFS_DIAG_FAILURE_PROGRAM_BASE_LOW, "PROGRAM_BASE_LOW"},
    {UFS_DIAG_FAILURE_PROGRAM_BASE_HIGH, "PROGRAM_BASE_HIGH"},
    {UFS_DIAG_FAILURE_PROGRAM_RUN_STOP, "PROGRAM_RUN_STOP"},
    {UFS_DIAG_FAILURE_UNEXPECTED_INTERRUPT, "UNEXPECTED_INTERRUPT"},
    {UFS_DIAG_FAILURE_INTERRUPT_MASK, "INTERRUPT_MASK"},
    {UFS_DIAG_FAILURE_UCD_VIRTUAL_ALIGNMENT, "UCD_VIRTUAL_ALIGNMENT"},
    {UFS_DIAG_FAILURE_DMA_ABOVE_4G, "DMA_ABOVE_4G"},
    {UFS_DIAG_FAILURE_HCI_VERSION, "HCI_VERSION"},
    {UFS_DIAG_FAILURE_HCI_VERSION_UNCONFIRMED, "HCI_VERSION_UNCONFIRMED"},
    {UFS_DIAG_FAILURE_INTERRUPT_GATE, "INTERRUPT_GATE"},
    {UFS_DIAG_FAILURE_INTERRUPT_STATUS, "INTERRUPT_STATUS"},
    {UFS_DIAG_FAILURE_NEXUS_PROGRAM, "NEXUS_PROGRAM"},
    {UFS_DIAG_FAILURE_NEXUS_READBACK, "NEXUS_READBACK"},
    {UFS_DIAG_FAILURE_STOP, "STOP"},
    {UFS_DIAG_FAILURE_RESTORE, "RESTORE"},
    {UFS_DIAG_FAILURE_TRANSFER_LENGTH, "TRANSFER_LENGTH"},
    {UFS_DIAG_FAILURE_RESPONSE_FRAMING, "RESPONSE_FRAMING"},
    {UFS_DIAG_FAILURE_TIMEOUT_CONTAINMENT, "TIMEOUT_CONTAINMENT"},
    {UFS_DIAG_FAILURE_WRITE_DISARMED, "WRITE_DISARMED"},
    {UFS_DIAG_FAILURE_WRITE_FENCE, "WRITE_FENCE"},
    {UFS_DIAG_FAILURE_WRITE_GPT_GUARD, "WRITE_GPT_GUARD"},
    {UFS_DIAG_FAILURE_WRITE_VERIFY, "WRITE_VERIFY"},
    {UFS_DIAG_FAILURE_WRITE_CRASH_LOCKOUT, "WRITE_CRASH_LOCKOUT"},
    {UFS_DIAG_FAILURE_PRIVATE_TELEMETRY_DISABLED, "PRIVATE_TELEMETRY_DISABLED (informational)"},
    {UFS_DIAG_FAILURE_BUGCHECK_INIT, "BUGCHECK_INIT"},
    {UFS_DIAG_FAILURE_BUGCHECK_DATA, "BUGCHECK_DATA"},
    {UFS_DIAG_FAILURE_BUGCHECK_REGISTER, "BUGCHECK_REGISTER"},
    {UFS_DIAG_FAILURE_BUGCHECK_PRAM_UNAVAILABLE, "BUGCHECK_PRAM_UNAVAILABLE (informational)"}
};

static const char *
StageName(
    ULONG Stage
    )
{
    switch (Stage) {
    case UFS_DIAG_STAGE_NONE:
        return "NONE";
    case UFS_DIAG_STAGE_FIND_ADAPTER_ENTERED:
        return "FIND_ADAPTER_ENTERED";
    case UFS_DIAG_STAGE_RESOURCES_CAPTURED:
        return "RESOURCES_CAPTURED";
    case UFS_DIAG_STAGE_RESOURCES_MAPPED:
        return "RESOURCES_MAPPED";
    case UFS_DIAG_STAGE_WORKSPACE_ALLOCATED:
        return "WORKSPACE_ALLOCATED";
    case UFS_DIAG_STAGE_FIND_WARM_VALIDATED:
        return "FIND_WARM_VALIDATED";
    case UFS_DIAG_STAGE_FIND_ADAPTER_COMPLETE:
        return "FIND_ADAPTER_COMPLETE";
    case UFS_DIAG_STAGE_HW_INITIALIZE_ENTERED:
        return "HW_INITIALIZE_ENTERED";
    case UFS_DIAG_STAGE_INIT_WARM_VALIDATED:
        return "INIT_WARM_VALIDATED";
    case UFS_DIAG_STAGE_TRANSFER_LIST_PROGRAMMED:
        return "TRANSFER_LIST_PROGRAMMED";
    case UFS_DIAG_STAGE_OPERATIONAL:
        return "OPERATIONAL";
    case UFS_DIAG_STAGE_DIAGNOSTIC_ONLY:
        return "DIAGNOSTIC_ONLY";
    case UFS_DIAG_STAGE_UNEXPECTED_INTERRUPT:
        return "UNEXPECTED_INTERRUPT";
    default:
        return "UNKNOWN";
    }
}

static const char *
FailCauseName(
    ULONG Cause
    )
{
    switch (Cause) {
    case UFS_FAIL_CAUSE_NONE:             return "NONE";
    case UFS_FAIL_CAUSE_OCS:              return "OCS";
    case UFS_FAIL_CAUSE_TRANSACTION_CODE: return "TRANSACTION_CODE";
    case UFS_FAIL_CAUSE_LUN:              return "LUN";
    case UFS_FAIL_CAUSE_TASK_TAG:         return "TASK_TAG";
    case UFS_FAIL_CAUSE_RESPONSE:         return "RESPONSE";
    case UFS_FAIL_CAUSE_RESIDUAL:         return "RESIDUAL";
    case UFS_FAIL_CAUSE_CHECK_CONDITION:  return "CHECK_CONDITION";
    case UFS_FAIL_CAUSE_SCSI_STATUS:      return "SCSI_STATUS";
    case UFS_FAIL_CAUSE_DATA_OVERRUN:     return "DATA_OVERRUN";
    default:                              return "UNKNOWN";
    }
}

static const char *
WriteModeName(
    ULONG Mode
    )
{
    switch (Mode) {
    case UFS_WRITE_MODE_DISARMED: return "DISARMED";
    case UFS_WRITE_MODE_DRY_RUN:  return "DRY_RUN";
    case UFS_WRITE_MODE_LIVE:     return "LIVE";
    default:                      return "UNKNOWN";
    }
}

static void
PrintFailureForensics(
    const UFS_DIAGNOSTIC_DATA *Data
    )
{
    ULONG Index;

    printf("FAIL_BY_CAUSE OCS=%lu TXN=%lu LUN=%lu TAG=%lu RESP=%lu "
           "RESID=%lu CHECKCOND=%lu STATUS=%lu OVERRUN=%lu\n",
        Data->FramingOcsFailures, Data->FramingTransactionFailures,
        Data->FramingLunFailures, Data->FramingTagFailures,
        Data->FramingResponseFailures, Data->FramingResidualFailures,
        Data->CheckConditionFailures, Data->ScsiStatusFailures,
        Data->DataOverrunFailures);
    printf("REARM_SUCCESSES=%lu REARM_BUSY=%lu CONSEC_FAIL=%lu "
           "MAX_CONSEC_FAIL=%lu LAST_CAUSE=%lu(%s)\n",
        Data->RearmSuccesses, Data->RearmBusyRejects,
        Data->ConsecutiveFailures, Data->MaxConsecutiveFailures,
        Data->LastFailureCause, FailCauseName(Data->LastFailureCause));
    printf("LAST_RESIDUAL=%lu LAST_SCSI_STATUS=0x%02lX LAST_TXN=0x%02lX\n",
        Data->LastResidual, Data->LastResponseStatus,
        Data->LastTransactionCode);

    if (Data->FirstFailureValid == 0) {
        printf("FIRST_FAILURE=NONE\n");
        return;
    }

    printf("FIRST_FAILURE=%lu(%s) AT_COMMAND=%lu OPCODE=0x%02lX "
           "LENGTH=%lu RESIDUAL=%lu\n",
        Data->FirstFailureCause, FailCauseName(Data->FirstFailureCause),
        Data->FirstFailureCommandIndex, Data->FirstFailureOpcode,
        Data->FirstFailureDataLength, Data->FirstFailureResidual);
    printf("FIRST_FAILURE_UPIU OCS=0x%02lX TXN=0x%02lX LUN=%lu TAG=%lu "
           "RESPONSE=0x%02lX STATUS=0x%02lX\n",
        Data->FirstFailureOcs, Data->FirstFailureTransactionCode,
        Data->FirstFailureLun, Data->FirstFailureTaskTag,
        Data->FirstFailureResponse, Data->FirstFailureStatus);
    printf("FIRST_FAILURE_SENSE KEY=0x%02lX ASC=0x%02lX ASCQ=0x%02lX\n",
        Data->FirstFailureSenseKey, Data->FirstFailureAsc,
        Data->FirstFailureAscq);
    printf("FIRST_FAILURE_REGS IS=0x%08lX HCS=0x%08lX DBR=0x%08lX\n",
        Data->FirstFailureInterruptStatus, Data->FirstFailureHostStatus,
        Data->FirstFailureDoorbell);
    printf("FIRST_FAILURE_CDB=");
    for (Index = 0; Index < sizeof(Data->FirstFailureCdb); Index += 1) {
        printf("%02X ", Data->FirstFailureCdb[Index]);
    }
    printf("\n");
}

static void
PrintLatency(
    const UFS_DIAGNOSTIC_DATA *Data
    )
{
#if UFS_PERF_INSTRUMENTATION
    ULONGLONG Freq;
    ULONGLONG Total;

    /*
     * A driver older than this build has no latency tail, and reading past its
     * Size would print whatever follows the structure in memory. Refuse rather
     * than mislead: a wrong microsecond figure here would send the next
     * optimisation at the wrong bottleneck.
     */
    if ((Data->Size < sizeof(UFS_DIAGNOSTIC_DATA)) ||
        (Data->Version < UFS_DIAG_DATA_VERSION)) {
        printf("PERF=UNAVAILABLE (driver predates the latency tail)\n");
        return;
    }

    Freq = Data->PerfCounterFrequency;
    if (Freq == 0) {
        printf("PERF=UNAVAILABLE (counter frequency not reported)\n");
        return;
    }

    printf("PERF_COUNTER_HZ=%llu READS=%lu (%llu bytes) WRITES=%lu (%llu bytes)\n",
        Freq, Data->PerfReadCount, Data->PerfReadBytes,
        Data->PerfWriteCount, Data->PerfWriteBytes);

    /*
     * Microseconds, computed here rather than in the driver: the kernel path
     * would need a 64-bit divide per I/O for a number nothing in the driver
     * consumes.
     */
    printf("PERF_US DEVICE=%llu ZERO=%llu BOUNCE_IN=%llu BOUNCE_OUT=%llu MAX_DEVICE=%llu\n",
        (Data->PerfDeviceTicks    * 1000000ULL) / Freq,
        (Data->PerfZeroTicks      * 1000000ULL) / Freq,
        (Data->PerfBounceInTicks  * 1000000ULL) / Freq,
        (Data->PerfBounceOutTicks * 1000000ULL) / Freq,
        (Data->PerfMaxDeviceTicks * 1000000ULL) / Freq);

    /*
     * The whole point of the exercise: how much of storage time is the device
     * and how much is this driver shuffling bytes through an uncached bounce.
     * COPY_PCT_OF_DEVICE over 100 means the copies cost more than the device.
     */
    Total = Data->PerfZeroTicks + Data->PerfBounceInTicks +
            Data->PerfBounceOutTicks;
    if (Data->PerfDeviceTicks != 0) {
        printf("PERF_COPY_PCT_OF_DEVICE=%llu\n",
            (Total * 100ULL) / Data->PerfDeviceTicks);
    }
    printf("PERF_POLL_ITERATIONS=%llu (approx %llu us at %lu us granularity)\n",
        Data->PerfPollIterations,
        Data->PerfPollIterations * (ULONGLONG)Data->PerfPollIntervalUs,
        Data->PerfPollIntervalUs);

    if (Data->PerfReadCount != 0) {
        printf("PERF_PER_READ_US DEVICE=%llu ZERO=%llu COPYBACK=%llu\n",
            (Data->PerfDeviceTicks   * 1000000ULL) / Freq / Data->PerfReadCount,
            (Data->PerfZeroTicks     * 1000000ULL) / Freq / Data->PerfReadCount,
            (Data->PerfBounceInTicks * 1000000ULL) / Freq / Data->PerfReadCount);
    }
#else
    (void)Data;
    printf("PERF=DISABLED (built with UFS_PERF_INSTRUMENTATION=0)\n");
#endif
}

static void
PrintDiagnostic(
    ULONG AdapterNumber,
    const UFS_DIAGNOSTIC_DATA *Data,
    BOOL Detailed
    )
{
    ULONG Index;

    printf("UFS_V9_DIAG_FOUND=1 ADAPTER=%lu\n", AdapterNumber);
    printf("SIGNATURE=0x%08lX VERSION=0x%08lX SIZE=%lu\n",
        Data->Signature, Data->Version, Data->Size);
    printf("STAGE=%lu(%s) FAILURE_STAGE=%lu(%s)\n",
        Data->Stage, StageName(Data->Stage),
        Data->FailureStage, StageName(Data->FailureStage));
    printf("FAILURE_MASK=0x%016I64X\n", Data->FailureMask);

    for (Index = 0; Index < ARRAYSIZE(FailureNames); Index++) {
        if ((Data->FailureMask & FailureNames[Index].Mask) != 0) {
            printf("FAILURE=%s\n", FailureNames[Index].Name);
        }
    }

    printf("INIT_CAPS=0x%08lX CONFIG_IN=0x%08lX CONFIG_FINAL=0x%08lX\n",
        Data->InitializationCapabilities,
        Data->IncomingConfigCapabilities,
        Data->FinalConfigCapabilities);
    printf("TARGETS=%lu LUNS=%lu INITIATOR=%lu IO=%lu IO_PER_LUN=%lu QUEUE=%lu\n",
        Data->MaximumNumberOfTargets,
        Data->MaximumNumberOfLogicalUnits,
        Data->InitiatorBusId,
        Data->MaxNumberOfIo,
        Data->MaxIosPerLun,
        Data->InitialLunQueueDepth);
    printf("MAPPED=%lu WORKSPACE=%lu WARM=%lu DIAGNOSTIC_ONLY=%lu "
           "STARTED=%lu OPERATIONAL=%lu\n",
        Data->ResourcesMapped, Data->WorkspaceAllocated,
        Data->WarmStateValid, Data->DiagnosticOnly,
        Data->Started, Data->Operational);
    printf("INTERRUPT_MODE=%lu CALLBACKS=%lu UTRD_INTERRUPT=%lu "
           "IS=0x%08lX IE_AFTER_ISR=0x%08lX\n",
        Data->InterruptSynchronizationMode,
        Data->InterruptCallbacks,
        Data->UtrdInterruptRequested,
        Data->InterruptStatus,
        Data->InterruptEnableAfterIsr);
    printf("HCE=0x%08lX CAP=0x%08lX HCS=0x%08lX IE=0x%08lX\n",
        Data->HostEnable, Data->Capabilities, Data->HostStatus,
        Data->InterruptEnable);
    printf("IAGC=0x%08lX DBR=0x%08lX TXPRDT=0x%08lX RXPRDT=0x%08lX\n",
        Data->InterruptAggregation, Data->Doorbell,
        Data->TxPrdtSize, Data->RxPrdtSize);
    printf("UTRL_BASE=0x%08lX:%08lX RUNSTOP=0x%08lX NEXUS=0x%08lX\n",
        Data->UtrlBaseHigh, Data->UtrlBaseLow,
        Data->UtrlRunStop, Data->NexusType);
    printf("COMMANDS_COMPLETED=%lu REJECTED=%lu FAILED=%lu\n",
        Data->CompletedCommands, Data->RejectedCommands,
        Data->FailedCommands);
    printf("CONTAINED=%lu WRITE_PROTECTED=%lu LAST_OPCODE=0x%02lX "
           "LAST_OCS=0x%02lX\n",
        Data->ContainedCommands, Data->WriteProtectedResponses,
        Data->LastOpcode, Data->LastOcs);
    printf("CAPACITY_VALID=%lu LAST_LBA=%I64u BLOCK_SIZE=%lu "
           "OWNS_UTRL=%lu IE_GATED=%lu\n",
        Data->CapacityValid, Data->LastLogicalBlock,
        Data->LogicalBlockSize, Data->OwnsTransferList,
        Data->InterruptsGated);
    printf("DIAG_ENDPOINT_PRESERVED=%lu FATAL_ERROR=%lu\n",
        Data->DiagnosticEndpointPreserved, Data->FatalError);
    /*
     * Write telemetry. WRITE_MODE is the single most important field on the
     * whole dashboard: DISARMED means the image physically cannot write, which
     * is the state every flash must boot into. FENCED/GUARDED counting up while
     * ISSUED stays at zero is the safeguard stack visibly working, not a fault.
     */
    printf("WRITE_MODE=%lu(%s) WRITES_ARMED=%lu FENCE=[%lu..%lu]\n",
        Data->WriteMode, WriteModeName(Data->WriteMode),
        Data->WritesArmed,
        Data->WriteFenceFirstLba, Data->WriteFenceLastLba);
    printf("WRITES ATTEMPTED=%lu FENCED=%lu GUARDED=%lu DISARMED_REJECTS=%lu\n",
        Data->WritesAttempted, Data->WritesFenced,
        Data->WritesGuarded, Data->WritesDisarmedRejects);
    printf("WRITES DRY_RUN=%lu ISSUED=%lu VERIFIED=%lu VERIFY_FAILURES=%lu\n",
        Data->WritesDryRun, Data->WritesIssued,
        Data->WritesVerified, Data->WriteVerifyFailures);
    printf("LAST_WRITE LBA=%lu BLOCKS=%lu RESULT=%lu\n",
        Data->LastWriteLba, Data->LastWriteBlocks, Data->LastWriteResult);
    /*
     * The crash latch. LOCKOUT=1 is the one line that explains an otherwise
     * inexplicable boot: the previous boot in this warm-reset chain died inside
     * the write path, so this boot refuses writes and reports which attempt
     * killed it. ATTEMPT is 1-based against the probe table, so ATTEMPT=4 means
     * WRITE_LAST_FENCED_BLOCK.
     *
     * COMPLETED=0 read from a live boot simply means a write is in flight right
     * now; it only becomes evidence when the NEXT boot reads it out of PRAM.
     */
    printf("WRITE_CRASH LOCKOUT=%lu ATTEMPT=%lu BOOT_EPOCH=%lu COMPLETED=%lu "
           "PHASE=%lu(%s)\n",
        Data->WriteCrashLockout, Data->WriteCrashAttempt,
        Data->WriteBootEpoch, Data->WriteProbeCompleted,
        Data->WriteCrashPhase, WritePhaseName(Data->WriteCrashPhase));
    printf("REARM_ATTEMPTS=%lu STARTIO=%lu LAST_SRB_FUNCTION=0x%02lX "
           "EXECUTE_SCSI=%lu\n",
        Data->RearmAttempts, Data->StartIoRequests,
        Data->LastSrbFunction, Data->ExecuteScsiRequests);
    printf("LAST_EXEC_REJECT=%lu(%s) HW_INITIALIZE_CALLS=%lu "
           "VENDOR_DIAG=%lu\n",
        Data->LastExecuteReject, ExecRejectName(Data->LastExecuteReject),
        Data->HwInitializeCalls, Data->VendorDiagRequests);
    /*
     * The opcode log. Every CDB the allowlist refuses is recorded here once,
     * so this names exactly which commands Windows sends that this miniport
     * does not implement - information no earlier build could produce, because
     * REJECTED only ever gave a count.
     *
     * SYNTHETIC_SENSE counts the refusals answered as CHECK CONDITION rather
     * than as an adapter-level failure, so a non-zero value is also the
     * cheapest proof on the device that this binary, and not a stale one, is
     * the one that ran.
     */
    printf("SYNTHETIC_SENSE=%lu REJECTED_OPCODES=%lu OVERFLOW=%lu\n",
        Data->SyntheticCheckConditions, Data->RejectedOpcodeCount,
        Data->RejectedOpcodeOverflow);
    {
        ULONG Slot;

        printf("REJECTED_OPCODE_LIST=");
        if (Data->RejectedOpcodeCount == 0) {
            printf("(none)");
        } else {
            for (Slot = 0;
                 (Slot < Data->RejectedOpcodeCount) &&
                     (Slot < UFS_REJECTED_OPCODE_SLOTS);
                 Slot++) {
                printf("%s0x%02X(%s)",
                    (Slot == 0) ? "" : " ",
                    Data->RejectedOpcodes[Slot],
                    ScsiOpcodeName(Data->RejectedOpcodes[Slot]));
            }
        }
        printf("\n");
    }
    /*
     * FIRST_REJECT decodes UFS_DIAGNOSTIC_DATA::FirstRejectContext. It names the
     * sub-validator behind a refusal inside an ACCEPTED allowlist case, which
     * REJECTED_OPCODE_LIST cannot: for 0x12 INQUIRY, CDB1 bit 0 is EVPD and CDB2
     * is the VPD page code, so a VPD query is distinguishable from a standard
     * one, and XFER against the CDB's own allocation length shows whether the
     * buffer was legitimately larger than the allocation.
     */
    if (Data->FirstRejectContext == 0ULL) {
        printf("FIRST_REJECT=(none)\n");
    } else {
        unsigned char RejOpcode = (unsigned char)(Data->FirstRejectContext & 0xFFULL);

        printf("FIRST_REJECT=0x%02X(%s) CDB1=0x%02X CDB2=0x%02X CDBLEN=%lu XFER=%lu\n",
            RejOpcode,
            ScsiOpcodeName(RejOpcode),
            (unsigned char)((Data->FirstRejectContext >> 8) & 0xFFULL),
            (unsigned char)((Data->FirstRejectContext >> 16) & 0xFFULL),
            (unsigned long)((Data->FirstRejectContext >> 24) & 0xFFULL),
            (unsigned long)((Data->FirstRejectContext >> 32) & 0xFFFFFFFFULL));
    }
    printf("PRAM_MAPPED=%lu PRAM_RECORDS=%lu PRAM_VIRTUAL=0x%016llX\n",
        Data->PramMapped, Data->PramRecords,
        (unsigned long long)Data->PramVirtual);
    /*
     * The line that decides the current investigation. A data-in command can
     * report OCS=0 with residual 0 and still deliver nothing, and until now
     * that was only visible by photographing the screen.
     */
    printf("PMU_MAPPED=%lu REBOOT_REQUESTS=%lu DATA_IN_LEN=%lu "
           "DATA_IN_NONZERO=%lu\n",
        Data->PmuMapped, Data->RebootRequests,
        Data->LastDataInLength, Data->LastDataInNonZero);
    /*
     * Hardware watchdog. WDT_ARMED=1 with PETS climbing is the unattended safety
     * net running: anything that stops this driver's timer - a bugcheck, a wedged
     * DPC - stops the pets and the SoC resets itself instead of sitting on the
     * boot logo waiting for somebody to hold the buttons.
     *
     * TIMEOUT_S is derived from the measured tick rate, not assumed. It is the
     * number that decides whether the firmware could safely arm this across
     * ExitBootServices to cover the window before this driver even loads.
     */
    printf("WDT_ARMED=%lu WDT_PETS=%lu WDT_COUNT=%lu WDT_HZ=%lu WDT_TIMEOUT_S=%lu"
           " WDT_TOOFAST=%lu\n",
        Data->WdtArmed, Data->WdtPets, Data->WdtLastCount,
        Data->WdtTicksPerSecond,
        (Data->WdtTicksPerSecond != 0)
            ? (unsigned long)(0xFFF5UL / Data->WdtTicksPerSecond)
            : 0UL,
        Data->WdtDisarmedTooFast);
    printf("DATA_IN_PREFIX=");
    for (Index = 0; Index < UFS_LAST_DATA_PREFIX; Index++) {
        printf("%02X", (unsigned int)Data->LastDataIn[Index]);
    }
    printf("\n");
    PrintFailureForensics(Data);
    PrintLatency(Data);
    printf("IOCTL_REQUESTS=%lu IOCTL_REJECTS=%lu IOCTL_BUFFER=%lu "
           "IOCTL_LENGTH=%lu\n",
        Data->IoControlRequests, Data->IoControlRejects,
        Data->LastIoControlBufferPresent, Data->LastIoControlLength);
    printf("IOCTL_HEADER=%lu IOCTL_CODE=0x%08lX IOCTL_SIGNATURE_OK=%lu\n",
        Data->LastIoControlHeaderLength, Data->LastIoControlCode,
        Data->LastIoControlSignatureOk);
    if (!Detailed) {
        return;
    }

    printf("INTERFACE_TYPE=%lu SYSTEM_BUS=%lu ACCESS_RANGES=%lu CAPTURED=%lu\n",
        Data->AdapterInterfaceType, Data->SystemIoBusNumber,
        Data->NumberOfAccessRanges, Data->CapturedAccessRanges);
    for (Index = 0; Index < Data->CapturedAccessRanges; Index++) {
        printf("RANGE[%lu]=0x%016I64X LENGTH=0x%08lX MEMORY=%lu\n",
            Index, Data->AccessRanges[Index].Start,
            Data->AccessRanges[Index].Length,
            Data->AccessRanges[Index].InMemory);
    }

    printf("HCI_VA=0x%016I64X WORKSPACE_VA=0x%016I64X WORKSPACE_SIZE=0x%08lX\n",
        Data->HciVirtual, Data->WorkspaceVirtual, Data->WorkspaceSize);
    printf("UTRL_VA=0x%016I64X UTRL_PA=0x%016I64X\n",
        Data->UtrlVirtual, Data->UtrlPhysical);
    printf("UCD_VA=0x%016I64X UCD_PA=0x%016I64X\n",
        Data->UcdVirtual, Data->UcdPhysical);
    printf("BOUNCE_VA=0x%016I64X BOUNCE_PA=0x%016I64X\n",
        Data->BounceVirtual, Data->BouncePhysical);
    printf("DMA64=0x%08lX DMA32=%lu MAX_TRANSFER=0x%08lX BREAKS=%lu\n",
        Data->Dma64BitAddresses, Data->Dma32BitAddresses,
        Data->MaximumTransferLength, Data->NumberOfPhysicalBreaks);
    printf("REORDER=0x%08lX AXIDMA=0x%08lX\n",
        Data->DataReorder, Data->AxiDmaBurst);
    printf("UTRL_EXPECTED=0x%08lX:%08lX READBACK=0x%08lX:%08lX RUNSTOP=0x%08lX\n",
        Data->ProgramBaseHighExpected, Data->ProgramBaseLowExpected,
        Data->ProgramBaseHighReadback, Data->ProgramBaseLowReadback,
        Data->ProgramRunStopReadback);
}

//
// Read-only SCSI pass-through probe.
//
// This deliberately carries a fixed, compiled-in command table. There is no
// way to feed it a caller-supplied CDB, and every entry is a data-in or
// no-data command, so the probe cannot write to the media.
//

typedef struct _UFS_PROBE_COMMAND {
    const char *Name;
    UCHAR Cdb[16];
    UCHAR CdbLength;
    ULONG TransferLength;
} UFS_PROBE_COMMAND;

static const UFS_PROBE_COMMAND ProbeCommands[] = {
    {"TEST_UNIT_READY",   {0x00}, 6, 0},
    {"INQUIRY",           {0x12, 0x00, 0x00, 0x00, 0x24}, 6, 36},
    {"READ_CAPACITY_10",  {0x25}, 10, 8},
    //
    // Multi-block DMA. Every read this project has ever executed - the KMDF
    // probes and all the single-block entries below - carried block count 1, so
    // a transfer described by a single PRDT entry spanning more than one
    // 4096-byte block has NEVER reached this controller. The V20/V21 five-block
    // attempts do not count: they all faulted pre-doorbell on the unaligned STRH
    // that V24 fixed, so the descriptor never got as far as the hardware.
    //
    // That gap is on the critical path. The write safeguards already admit up to
    // UFS_WRITE_MAX_BLOCKS per command, so whether a wider fenced write costs one
    // command or N sequential ones depends entirely on this answer - and it must
    // be settled on the READ path, because making the first-ever multi-block
    // transfer a WRITE would be exactly the kind of unhedged step the RW
    // directive forbids.
    //
    // LBA 1 is the right base because the blocks either side of it have distinct,
    // already-observed contents: LBA 1 = "EFI PART", LBA 2 = the GPT entry-0 type
    // GUID EBD0A0A2-B9E5-4433-87C0-68B6B72699C7. A transfer that silently
    // delivers only its first block is therefore visible in the data itself,
    // not merely inferred from the reported length.
    //
    // 2 blocks first (the minimal delta from the proven shape), then 4 (the
    // orphan-tail width the next write milestone needs). The driver validates
    // BlockCount against UFS_BOUNCE_SIZE / BlockSize = 32, so both are in range.
    //
    // ORDERING IS LOAD-BEARING. These run third and fourth, not last. The PRAM
    // ring is linear and non-wrapping: when it fills, the NEWEST records are the
    // ones lost. On the 2026-07-29 boot both entries executed and the machine
    // survived, but they sat at the end of the table and the ring truncated
    // mid-record at SEQ 0x15, so the answer was destroyed on the way out. They
    // are placed after READ_CAPACITY_10 so the driver has learned the logical
    // block size before a multi-block count is validated, and early enough that
    // their records are written while the ring still has room. What now falls off
    // the tail is the LBA sweep, whose values are already recorded and known.
    //
    {"READ_10_LBA1_2BLK",    {0x28, 0x00, 0, 0, 0, 0x01, 0x00, 0x00, 0x02}, 10, 8192},
    {"READ_10_LBA1_4BLK",    {0x28, 0x00, 0, 0, 0, 0x01, 0x00, 0x00, 0x04}, 10, 16384},
    {"READ_CAPACITY_16",  {0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0,
                           0, 0, 0, 0x20}, 16, 32},
    {"REQUEST_SENSE",     {0x03, 0x00, 0x00, 0x00, 0x12}, 6, 18},
    {"MODE_SENSE_10",     {0x5A, 0x00, 0x3F, 0, 0, 0, 0, 0x00, 0xC0}, 10, 192},
    {"READ_10_LBA0_4096", {0x28, 0x00, 0, 0, 0, 0, 0x00, 0x00, 0x01}, 10, 4096},
    //
    // LBA sweep. LBA 0 alone cannot answer whether the media path works: on a
    // GPT disk LBA 0 is the protective MBR, and on Samsung/Android images its
    // 446-byte bootstrap area is genuinely all zeros. Reading zeros out of the
    // first 16/32 bytes there is the EXPECTED result, not evidence of a failed
    // DMA - which is exactly how V15/V16/V17 were misread.
    //
    // LBA 1 is the discriminator: a GPT primary header starts with the ASCII
    // signature "EFI PART" (45 46 49 20 50 41 52 54) in its first 8 bytes, so a
    // correct read is unmistakable in even a 16-byte prefix. The KMDF probe read
    // exactly this block on this controller in July and got that signature, so a
    // zero here would be a real regression rather than an ambiguous sample.
    //
    // LBA 2 (first partition-entry block) and a deep LBA discriminate a
    // protected-region theory: if only low/bootloader LBAs are zero-filled by
    // the Exynos UFS Protector, a high LBA still returns data.
    //
    {"READ_10_LBA1_GPTHDR",  {0x28, 0x00, 0, 0, 0, 0x01, 0x00, 0x00, 0x01}, 10, 4096},
    {"READ_10_LBA2_GPTENT",  {0x28, 0x00, 0, 0, 0, 0x02, 0x00, 0x00, 0x01}, 10, 4096},
    {"READ_10_LBA6_FIRSTUSE",{0x28, 0x00, 0, 0, 0, 0x06, 0x00, 0x00, 0x01}, 10, 4096},
    {"READ_10_LBA1M_DEEP",   {0x28, 0x00, 0, 0x10, 0, 0x00, 0x00, 0x00, 0x01}, 10, 4096},
    {"READ_10_LBA0_512",  {0x28, 0x00, 0, 0, 0, 0, 0x00, 0x00, 0x01}, 10, 512}
};

//
// Multi-block proof.
//
// PrintHexPrefix caps at 32 bytes, so on a multi-block transfer it only ever
// shows block 0 - it cannot tell a full transfer apart from one where the
// controller delivered the first block and stopped. The reported XFER length is
// no better: that is what the port driver was asked to move, not what the device
// actually deposited. This is the same trap that made "LBA 0 reads all zeros"
// look like a failed DMA when it was a correct read of a protective MBR - a
// value read at the wrong place and then trusted.
//
// So report each block directly. The caller zeroes the whole buffer before every
// command, which is what makes this conclusive: a block that comes back entirely
// zero was genuinely never written. NONZERO is carried alongside the signature
// because a block may legitimately begin with zeros, so the leading bytes alone
// are not a sound emptiness test.
//
static void
PrintBlockSignatures(
    const UCHAR *Data,
    ULONG Length,
    ULONG BlockSize
    )
{
    ULONG Blocks;
    ULONG Block;
    ULONG Limit;

    if ((BlockSize == 0UL) || (Length <= BlockSize)) {
        return;
    }

    Blocks = Length / BlockSize;
    Limit = (Blocks > 8UL) ? 8UL : Blocks;

    for (Block = 0; Block < Limit; Block++) {
        const UCHAR *Start = Data + ((size_t)Block * (size_t)BlockSize);
        ULONG NonZero = 0;
        ULONG Index;

        for (Index = 0; Index < BlockSize; Index++) {
            if (Start[Index] != 0) {
                NonZero++;
            }
        }

        printf("  BLK[%lu] NONZERO=%lu SIG=", Block, NonZero);
        for (Index = 0; Index < 16UL; Index++) {
            printf("%02X", Start[Index]);
        }
        printf("\n");
    }

    if (Blocks > Limit) {
        printf("  BLK_TRUNCATED BLOCKS=%lu SHOWN=%lu\n", Blocks, Limit);
    }
}

typedef struct _UFS_SPTD_WITH_SENSE {
    SCSI_PASS_THROUGH_DIRECT Sptd;
    ULONG Filler;
    UCHAR Sense[32];
} UFS_SPTD_WITH_SENSE;

static void
PrintHexPrefix(
    const UCHAR *Data,
    ULONG Length
    )
{
    ULONG Index;
    ULONG Limit = (Length > 32UL) ? 32UL : Length;

    if (Limit == 0) {
        return;
    }
    printf("  DATA=");
    for (Index = 0; Index < Limit; Index++) {
        printf("%02X", Data[Index]);
    }
    printf("\n");
}

/*
 * Summarise the WHOLE transfer, not just its prefix.
 *
 * This exists because a 16/32-byte prefix is not evidence about a 4096-byte
 * block. Scanning every byte turns "did the DMA land?" into a single number
 * (NONZERO) that cannot be misread, and FIRST_NZ says where the content starts
 * so a legitimately-zero leading region is obvious.
 *
 * SIG55AA reports the MBR boot signature at offset 510, which is the only
 * reliably non-zero field of a protective MBR, and PART0 dumps the 16-byte
 * partition entry at offset 446. Together they classify LBA 0 correctly even
 * though its first 446 bytes are supposed to be zero.
 */
static void
PrintDataSummary(
    const UCHAR *Data,
    ULONG Length
    )
{
    ULONG Index;
    ULONG NonZero = 0;
    ULONG FirstNonZero = 0xFFFFFFFFUL;

    if (Length == 0) {
        return;
    }

    for (Index = 0; Index < Length; Index++) {
        if (Data[Index] != 0) {
            NonZero++;
            if (FirstNonZero == 0xFFFFFFFFUL) {
                FirstNonZero = Index;
            }
        }
    }

    printf(
        "  NONZERO=%lu/%lu FIRST_NZ=%ld\n",
        NonZero,
        Length,
        (FirstNonZero == 0xFFFFFFFFUL) ? -1L : (LONG)FirstNonZero
        );

    if (Length >= 512UL) {
        printf(
            "  SIG55AA=%02X%02X PART0=",
            (ULONG)Data[510],
            (ULONG)Data[511]
            );
        for (Index = 446; Index < 462; Index++) {
            printf("%02X", Data[Index]);
        }
        printf("\n");
    }
}

static void
ReportDiskAddress(
    void
    )
{
    HANDLE Handle;
    SCSI_ADDRESS Address;
    DWORD Returned = 0;

    Handle = CreateFileW(
        L"\\\\.\\PhysicalDrive0",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Handle == INVALID_HANDLE_VALUE) {
        printf("UFS_DISK0_OPEN=0 ERROR=%lu\n", GetLastError());
        return;
    }

    ZeroMemory(&Address, sizeof(Address));
    Address.Length = sizeof(Address);
    if (DeviceIoControl(
            Handle,
            IOCTL_SCSI_GET_ADDRESS,
            NULL,
            0,
            &Address,
            sizeof(Address),
            &Returned,
            NULL
            )) {
        printf(
            "UFS_DISK0_OPEN=1 PORT=%lu PATH=%lu TARGET=%lu LUN=%lu\n",
            (ULONG)Address.PortNumber,
            (ULONG)Address.PathId,
            (ULONG)Address.TargetId,
            (ULONG)Address.Lun
            );
    } else {
        printf("UFS_DISK0_OPEN=1 ADDRESS_ERROR=%lu\n", GetLastError());
    }

    CloseHandle(Handle);
}

static BOOL
RunVendorDiagSnapshot(
    UFS_DIAGNOSTIC_DATA *Snapshot
    )
{
    HANDLE Handle;
    PUCHAR Buffer;
    UFS_SPTD_WITH_SENSE Request;
    const UFS_DIAGNOSTIC_DATA *Data;
    DWORD Returned = 0;
    DWORD Error;
    BOOL Result;
    BOOL Valid = FALSE;

    if (Snapshot != NULL) {
        ZeroMemory(Snapshot, sizeof(*Snapshot));
    }

    printf(
        "UFS_VENDOR_DIAG_LAYOUT PACKET=%lu CONTROL=%lu DATA=%lu OFFSET=%lu\n",
        (ULONG)sizeof(UFS_DIAGNOSTIC_PACKET),
        (ULONG)sizeof(SRB_IO_CONTROL),
        (ULONG)sizeof(UFS_DIAGNOSTIC_DATA),
        (ULONG)FIELD_OFFSET(UFS_DIAGNOSTIC_PACKET, Data)
        );

    Buffer = (PUCHAR)VirtualAlloc(
        NULL,
        4096,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
        );
    if (Buffer == NULL) {
        printf("UFS_VENDOR_DIAG=0 REASON=ALLOC\n");
        return FALSE;
    }

    Handle = CreateFileW(
        L"\\\\.\\PhysicalDrive0",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Handle == INVALID_HANDLE_VALUE) {
        printf("UFS_VENDOR_DIAG=0 REASON=OPEN ERROR=%lu\n", GetLastError());
        VirtualFree(Buffer, 0, MEM_RELEASE);
        return FALSE;
    }

    ZeroMemory(&Request, sizeof(Request));
    ZeroMemory(Buffer, 4096);
    Request.Sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    Request.Sptd.CdbLength = 6;
    Request.Sptd.SenseInfoLength = sizeof(Request.Sense);
    Request.Sptd.DataIn = SCSI_IOCTL_DATA_IN;
    Request.Sptd.DataTransferLength = 1024;
    Request.Sptd.TimeOutValue = 10;
    Request.Sptd.DataBuffer = Buffer;
    Request.Sptd.SenseInfoOffset = FIELD_OFFSET(UFS_SPTD_WITH_SENSE, Sense);
    Request.Sptd.Cdb[0] = UFS_DIAG_VENDOR_CDB_OPCODE;
    Request.Sptd.Cdb[1] = 0;
    Request.Sptd.Cdb[2] = 0;
    Request.Sptd.Cdb[3] = 0;
    Request.Sptd.Cdb[4] = 0;
    Request.Sptd.Cdb[5] = 0;

    Result = DeviceIoControl(
        Handle,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &Request,
        sizeof(Request),
        &Request,
        sizeof(Request),
        &Returned,
        NULL
        );
    Error = Result ? ERROR_SUCCESS : GetLastError();
    CloseHandle(Handle);

    printf(
        "UFS_VENDOR_DIAG=%lu WIN32=%lu SCSI_STATUS=0x%02X XFER=%lu\n",
        Result ? 1UL : 0UL,
        Error,
        (ULONG)Request.Sptd.ScsiStatus,
        Request.Sptd.DataTransferLength
        );

    Data = (const UFS_DIAGNOSTIC_DATA *)Buffer;
    printf(
        "UFS_VENDOR_DIAG_HEADER SIGNATURE=0x%08lX VERSION=0x%08lX SIZE=%lu "
        "EXPECT_SIGNATURE=0x%08lX EXPECT_VERSION=0x%08lX EXPECT_SIZE=%lu\n",
        Data->Signature,
        Data->Version,
        Data->Size,
        (ULONG)UFS_DIAG_DATA_SIGNATURE,
        (ULONG)UFS_DIAG_DATA_VERSION,
        (ULONG)sizeof(UFS_DIAGNOSTIC_DATA)
        );
    PrintHexPrefix(Buffer, 32);

    if (Result &&
        (Data->Signature == UFS_DIAG_DATA_SIGNATURE) &&
        (Data->Version == UFS_DIAG_DATA_VERSION) &&
        (Data->Size == sizeof(UFS_DIAGNOSTIC_DATA))) {
        Valid = TRUE;
        if (Snapshot != NULL) {
            CopyMemory(Snapshot, Data, sizeof(*Snapshot));
        }
        PrintDiagnostic(0, Data, TRUE);
    } else {
        printf("UFS_VENDOR_DIAG_VALID=0\n");
    }

    VirtualFree(Buffer, 0, MEM_RELEASE);
    return Valid;
}

static BOOL
RunVendorDiag(
    void
    )
{
    return RunVendorDiagSnapshot(NULL);
}

static void
RunScsiProbe(
    void
    )
{
    HANDLE Handle;
    PUCHAR Buffer;
    ULONG Index;

    ReportDiskAddress();

    Buffer = (PUCHAR)VirtualAlloc(
        NULL,
        65536,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
        );
    if (Buffer == NULL) {
        printf("UFS_PROBE=0 REASON=ALLOC\n");
        return;
    }

    Handle = CreateFileW(
        L"\\\\.\\PhysicalDrive0",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Handle == INVALID_HANDLE_VALUE) {
        printf("UFS_PROBE=0 REASON=OPEN ERROR=%lu\n", GetLastError());
        VirtualFree(Buffer, 0, MEM_RELEASE);
        return;
    }

    printf("UFS_PROBE=1 COMMANDS=%lu\n", (ULONG)ARRAYSIZE(ProbeCommands));

    for (Index = 0; Index < ARRAYSIZE(ProbeCommands); Index++) {
        const UFS_PROBE_COMMAND *Command = &ProbeCommands[Index];
        UFS_SPTD_WITH_SENSE Request;
        DWORD Returned = 0;
        DWORD Error;
        BOOL Result;
        ULONG CdbIndex;

        ZeroMemory(&Request, sizeof(Request));
        ZeroMemory(Buffer, 65536);

        Request.Sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
        Request.Sptd.CdbLength = Command->CdbLength;
        Request.Sptd.SenseInfoLength = sizeof(Request.Sense);
        Request.Sptd.DataIn = (Command->TransferLength != 0) ?
            SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_UNSPECIFIED;
        Request.Sptd.DataTransferLength = Command->TransferLength;
        Request.Sptd.TimeOutValue = 10;
        Request.Sptd.DataBuffer =
            (Command->TransferLength != 0) ? Buffer : NULL;
        Request.Sptd.SenseInfoOffset =
            FIELD_OFFSET(UFS_SPTD_WITH_SENSE, Sense);
        CopyMemory(Request.Sptd.Cdb, Command->Cdb, Command->CdbLength);

        Result = DeviceIoControl(
            Handle,
            IOCTL_SCSI_PASS_THROUGH_DIRECT,
            &Request,
            sizeof(Request),
            &Request,
            sizeof(Request),
            &Returned,
            NULL
            );
        Error = Result ? ERROR_SUCCESS : GetLastError();

        printf("CMD=%s CDB=", Command->Name);
        for (CdbIndex = 0; CdbIndex < Command->CdbLength; CdbIndex++) {
            printf("%02X", Command->Cdb[CdbIndex]);
        }
        printf(
            " OK=%lu WIN32=%lu SCSI_STATUS=0x%02X XFER=%lu\n",
            Result ? 1UL : 0UL,
            Error,
            (ULONG)Request.Sptd.ScsiStatus,
            Request.Sptd.DataTransferLength
            );

        if (Request.Sptd.ScsiStatus != 0) {
            printf(
                "  SENSE KEY=0x%02X ASC=0x%02X ASCQ=0x%02X RESPONSE=0x%02X\n",
                (ULONG)(Request.Sense[2] & 0x0FU),
                (ULONG)Request.Sense[12],
                (ULONG)Request.Sense[13],
                (ULONG)Request.Sense[0]
                );
        }

        if (Result && (Request.Sptd.DataTransferLength != 0)) {
            PrintHexPrefix(Buffer, Request.Sptd.DataTransferLength);
            PrintBlockSignatures(Buffer, Request.Sptd.DataTransferLength, 4096UL);
            PrintDataSummary(Buffer, Request.Sptd.DataTransferLength);
        }

        //
        // Snapshot this command's result into PRAM before the next one
        // overwrites it. The vendor 0xD0 read is answered from memory at the top
        // of UfsExecuteScsi and never builds a UPIU, so it cannot disturb the
        // bounce buffer or the DIN capture it is reporting - it just forces
        // UfsPramSnapshot("UFSDIAG") to run now.
        //
        // Without this, only the LAST data-in of the boot reaches the ramoops
        // ring and the whole sweep would be unreadable off-device.
        //
        if (Command->TransferLength != 0) {
            (void)RunVendorDiag();
        }
    }

    CloseHandle(Handle);
    VirtualFree(Buffer, 0, MEM_RELEASE);
    printf("UFS_PROBE_COMPLETE=1\n");}

/*
 * Read the live fence and arm latch straight out of the driver so the write
 * probe below can derive its LBAs instead of duplicating the fence constants.
 * Duplicated bounds are exactly how a safeguard silently stops matching the
 * thing it is supposed to be guarding.
 */
static BOOL
QueryWriteFence(
    ULONG *FirstLba,
    ULONG *LastLba,
    ULONG *WriteMode,
    ULONG *CrashLockout,
    ULONG *CrashAttempt
    )
{
    HANDLE Handle;
    PUCHAR Buffer;
    UFS_SPTD_WITH_SENSE Request;
    const UFS_DIAGNOSTIC_DATA *Data;
    DWORD Returned = 0;
    BOOL Result;
    BOOL Valid = FALSE;

    *FirstLba = 0;
    *LastLba = 0;
    *WriteMode = UFS_WRITE_MODE_DISARMED;
    *CrashLockout = 0;
    *CrashAttempt = 0;

    Buffer = (PUCHAR)VirtualAlloc(
        NULL,
        4096,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
        );
    if (Buffer == NULL) {
        return FALSE;
    }

    Handle = CreateFileW(
        L"\\\\.\\PhysicalDrive0",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Handle == INVALID_HANDLE_VALUE) {
        VirtualFree(Buffer, 0, MEM_RELEASE);
        return FALSE;
    }

    ZeroMemory(&Request, sizeof(Request));
    ZeroMemory(Buffer, 4096);
    Request.Sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    Request.Sptd.CdbLength = 6;
    Request.Sptd.SenseInfoLength = sizeof(Request.Sense);
    Request.Sptd.DataIn = SCSI_IOCTL_DATA_IN;
    Request.Sptd.DataTransferLength = 1024;
    Request.Sptd.TimeOutValue = 10;
    Request.Sptd.DataBuffer = Buffer;
    Request.Sptd.SenseInfoOffset = FIELD_OFFSET(UFS_SPTD_WITH_SENSE, Sense);
    Request.Sptd.Cdb[0] = UFS_DIAG_VENDOR_CDB_OPCODE;

    Result = DeviceIoControl(
        Handle,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &Request,
        sizeof(Request),
        &Request,
        sizeof(Request),
        &Returned,
        NULL
        );
    CloseHandle(Handle);

    Data = (const UFS_DIAGNOSTIC_DATA *)Buffer;
    if (Result &&
        (Data->Signature == UFS_DIAG_DATA_SIGNATURE) &&
        (Data->Version == UFS_DIAG_DATA_VERSION) &&
        (Data->Size == sizeof(UFS_DIAGNOSTIC_DATA)) &&
        (Data->WriteFenceLastLba > Data->WriteFenceFirstLba) &&
        (Data->WriteFenceFirstLba != 0)) {
        *FirstLba = Data->WriteFenceFirstLba;
        *LastLba = Data->WriteFenceLastLba;
        *WriteMode = Data->WriteMode;
        *CrashLockout = Data->WriteCrashLockout;
        *CrashAttempt = Data->WriteCrashAttempt;
        Valid = TRUE;
    }

    VirtualFree(Buffer, 0, MEM_RELEASE);
    return Valid;
}

/*
 * Layers 1-3 proven against real Windows I/O: a fixed set of WRITE(10) attempts
 * whose LBAs are DERIVED FROM THE DRIVER'S OWN FENCE rather than duplicated
 * here. As in the read probe there is no caller-supplied CDB and no
 * caller-supplied LBA, so this cannot be aimed at an arbitrary block.
 *
 * Three of the five attempts are deliberately illegal and MUST be refused in
 * every arm state, because the GPT guard and the fence run inside
 * UfsClassifyCdb - ahead of the arm latch. That ordering is the whole point: an
 * armed adapter is still not allowed anywhere near EFS, BOOT, RECOVERY or
 * either GPT copy. Only the two in-fence attempts depend on the latch:
 *
 *     DISARMED -> refused, LAST_EXEC_REJECT=12(WRITE_DISARMED)
 *     DRY_RUN  -> reported success, but the driver returns before
 *                 UfsBuildCommand, so no UPIU, no doorbell, no DMA, no media
 *     LIVE     -> a real write, verified by read-back inside the driver
 *
 * Running this while DISARMED or in DRY_RUN therefore carries no media risk at
 * all, which is why the first on-device flash is gated to dry-run only.
 */
#define UFS_WRITE_PROBE_FROM_ZERO   0
#define UFS_WRITE_PROBE_FROM_FIRST  1
#define UFS_WRITE_PROBE_FROM_LAST   2

typedef struct _UFS_WRITE_PROBE_ENTRY {
    const char *Name;
    ULONG Origin;
    LONGLONG Delta;
    const char *Expect;
} UFS_WRITE_PROBE_ENTRY;

/*
 * Ordering is load-bearing, not cosmetic. The two ARM_DEPENDENT entries run
 * FIRST because they are the only ones that reach the arm gate at all - the
 * other three are refused inside UfsClassifyCdb, which runs ahead of the latch.
 *
 * They used to be last, and the armed run never got to them: every capture ever
 * taken shows WDRY=0, so the DRY_RUN branch had never once executed on hardware.
 * Anything that cuts the run short now costs a redundant rejection rather than
 * the single measurement the whole milestone turns on.
 */
static const UFS_WRITE_PROBE_ENTRY WriteProbeEntries[] = {
    {"WRITE_LAST_FENCED_BLOCK",   UFS_WRITE_PROBE_FROM_LAST,  0,  "ARM_DEPENDENT"},
    {"WRITE_FIRST_FENCED_BLOCK",  UFS_WRITE_PROBE_FROM_FIRST, 0,  "ARM_DEPENDENT"},
    {"WRITE_LBA0_PROTECTIVE_MBR", UFS_WRITE_PROBE_FROM_ZERO,  0,  "REJECT_GPT_GUARD"},
    {"WRITE_ONE_BELOW_FENCE",     UFS_WRITE_PROBE_FROM_FIRST, -1, "REJECT_FENCE"},
    {"WRITE_ONE_ABOVE_FENCE",     UFS_WRITE_PROBE_FROM_LAST,  1,  "REJECT_FENCE"}
};

static void
RunWriteProbe(
    void
    )
{
    HANDLE Handle;
    PUCHAR Buffer;
    ULONG Index;
    ULONG FenceFirst = 0;
    ULONG FenceLast = 0;
    ULONG WriteMode = UFS_WRITE_MODE_DISARMED;
    ULONG CrashLockout = 0;
    ULONG CrashAttempt = 0;
    const ULONG BlockSize = 4096;

    if (!QueryWriteFence(&FenceFirst, &FenceLast, &WriteMode,
                         &CrashLockout, &CrashAttempt)) {
        printf("UFS_WRITE_PROBE=0 REASON=NO_FENCE\n");
        return;
    }

    /*
     * Stop before the table if the driver latched a crash lockout. The driver
     * would refuse every entry anyway, so running would only fill the PRAM ring
     * with five identical rejections and push the evidence that actually matters
     * - which attempt crashed the previous boot - out of a ring that has room
     * for about five records total.
     */
    if (CrashLockout != 0) {
        printf(
            "UFS_WRITE_PROBE=0 REASON=CRASH_LOCKOUT PREVIOUS_ATTEMPT=%lu\n",
            CrashAttempt
            );
        return;
    }

    printf(
        "UFS_WRITE_PROBE=1 MODE=%s FENCE=[%lu..%lu] BLOCK=%lu ATTEMPTS=%lu\n",
        WriteModeName(WriteMode),
        FenceFirst,
        FenceLast,
        BlockSize,
        (ULONG)ARRAYSIZE(WriteProbeEntries)
        );

    //
    // Refuse to run unless the driver is in a mode that cannot touch media. The
    // helper is not the safeguard - the driver is - but there is no reason for
    // this table to be the thing that first exercises a LIVE adapter, and a
    // separate deliberate step for that keeps the audit trail honest.
    //
    if (WriteMode == UFS_WRITE_MODE_LIVE) {
        printf("UFS_WRITE_PROBE_SKIPPED=1 REASON=LIVE_REQUIRES_EXPLICIT_RUN\n");
        return;
    }

    Buffer = (PUCHAR)VirtualAlloc(
        NULL,
        65536,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
        );
    if (Buffer == NULL) {
        printf("UFS_WRITE_PROBE=0 REASON=ALLOC\n");
        return;
    }

    Handle = CreateFileW(
        L"\\\\.\\PhysicalDrive0",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Handle == INVALID_HANDLE_VALUE) {
        printf("UFS_WRITE_PROBE=0 REASON=OPEN ERROR=%lu\n", GetLastError());
        VirtualFree(Buffer, 0, MEM_RELEASE);
        return;
    }

    for (Index = 0; Index < ARRAYSIZE(WriteProbeEntries); Index++) {
        const UFS_WRITE_PROBE_ENTRY *Entry = &WriteProbeEntries[Index];
        UFS_SPTD_WITH_SENSE Request;
        LONGLONG Origin;
        LONGLONG Target;
        ULONG Lba;
        DWORD Returned = 0;
        DWORD Error;
        BOOL Result;
        ULONG CdbIndex;

        if (Entry->Origin == UFS_WRITE_PROBE_FROM_FIRST) {
            Origin = (LONGLONG)FenceFirst;
        } else if (Entry->Origin == UFS_WRITE_PROBE_FROM_LAST) {
            Origin = (LONGLONG)FenceLast;
        } else {
            Origin = 0;
        }

        Target = Origin + Entry->Delta;
        if ((Target < 0) || (Target > 0xFFFFFFFFLL)) {
            continue;
        }
        Lba = (ULONG)Target;

        ZeroMemory(&Request, sizeof(Request));

        //
        // A recognisable, self-describing payload. In dry-run it is never read
        // by anyone, but once LIVE is authorised this is what read-back
        // verification compares against, and a constant pattern makes a partial
        // or misdirected DMA obvious rather than plausible.
        //
        FillMemory(Buffer, BlockSize, 0xA5);
        Buffer[0] = 'U';
        Buffer[1] = 'F';
        Buffer[2] = 'S';
        Buffer[3] = 'W';
        Buffer[4] = (UCHAR)(Lba & 0xFFU);
        Buffer[5] = (UCHAR)((Lba >> 8) & 0xFFU);
        Buffer[6] = (UCHAR)((Lba >> 16) & 0xFFU);
        Buffer[7] = (UCHAR)((Lba >> 24) & 0xFFU);

        Request.Sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
        Request.Sptd.CdbLength = 10;
        Request.Sptd.SenseInfoLength = sizeof(Request.Sense);
        Request.Sptd.DataIn = SCSI_IOCTL_DATA_OUT;
        Request.Sptd.DataTransferLength = BlockSize;
        Request.Sptd.TimeOutValue = 10;
        Request.Sptd.DataBuffer = Buffer;
        Request.Sptd.SenseInfoOffset =
            FIELD_OFFSET(UFS_SPTD_WITH_SENSE, Sense);
        Request.Sptd.Cdb[0] = 0x2A;
        Request.Sptd.Cdb[2] = (UCHAR)((Lba >> 24) & 0xFFU);
        Request.Sptd.Cdb[3] = (UCHAR)((Lba >> 16) & 0xFFU);
        Request.Sptd.Cdb[4] = (UCHAR)((Lba >> 8) & 0xFFU);
        Request.Sptd.Cdb[5] = (UCHAR)(Lba & 0xFFU);
        Request.Sptd.Cdb[8] = 0x01;

        Result = DeviceIoControl(
            Handle,
            IOCTL_SCSI_PASS_THROUGH_DIRECT,
            &Request,
            sizeof(Request),
            &Request,
            sizeof(Request),
            &Returned,
            NULL
            );
        Error = Result ? ERROR_SUCCESS : GetLastError();

        printf("WCMD=%s LBA=%lu EXPECT=%s CDB=", Entry->Name, Lba, Entry->Expect);
        for (CdbIndex = 0; CdbIndex < 10; CdbIndex++) {
            printf("%02X", Request.Sptd.Cdb[CdbIndex]);
        }
        printf(
            " OK=%lu WIN32=%lu SCSI_STATUS=0x%02X XFER=%lu\n",
            Result ? 1UL : 0UL,
            Error,
            (ULONG)Request.Sptd.ScsiStatus,
            Request.Sptd.DataTransferLength
            );

        //
        // Force the driver to journal this attempt into PRAM before the next
        // one overwrites LastWrite*, so the whole sweep is readable off-device
        // from TWRP rather than only the final entry.
        //
        (void)RunVendorDiag();
    }

    CloseHandle(Handle);
    VirtualFree(Buffer, 0, MEM_RELEASE);
    printf("UFS_WRITE_PROBE_COMPLETE=1\n");
}

/*
 * One block, one command each way, fully reversible.
 *
 * TARGET: the LAST fenced block, taken from the driver's own fence rather than
 * written down here - same discipline as the probe table. On this device that
 * resolves to disk LBA 1195135, and it was chosen because it is PROVABLY
 * unreachable by the filesystem rather than merely believed to be free:
 *
 *   WINSETUP (sda18) is FAT32 with 4096-byte sectors, spanning disk blocks
 *   68736..1195135. Reserved 32 + 2 FATs x 138 puts the data region at
 *   partition sector 308, leaving 1126092 data sectors. That is 140761 whole
 *   clusters of 8 sectors plus a remainder of FOUR. Cluster numbers therefore
 *   stop at 140762, which ends at partition sector 1126395 - so partition
 *   sectors 1126396..1126399, i.e. disk LBA 1195132..1195135, lie past the
 *   final cluster and no cluster number can address them. FAT32 cannot allocate
 *   them, so no file can ever occupy them.
 *
 * That was confirmed against the live device before this verb existed: those
 * tail blocks read back near-empty while the block immediately below (1195130,
 * inside cluster 140762) is full of data. "Nearly empty" is not the proof - the
 * cluster arithmetic is - but the boundary landing exactly where the arithmetic
 * puts it is a useful independent check. Note that free space in this volume is
 * NOT zeroed: it is full of remnants of previously written boot.wim images, so
 * content alone can never establish that a block is unallocated.
 *
 * WHY THE FENCE-RELATIVE TAIL IS NOT THE TARGET. Read "tail" here as the last
 * blocks of sda18, which is what the end of the fence meant at the time. The
 * proof above covers exactly FOUR blocks, and the target used to be derived as
 * FenceLast - (BLOCKS - 1). That coupling is a trap: raising the block count
 * walks the START of the write DOWNWARD, out of the four proven-orphan blocks
 * and into the ordinary data region, while every gate keeps passing because the
 * fence itself is unchanged. The proof silently stops applying to the thing it
 * is quoted as justifying. Measured on the live device: the volume is 94% full
 * (254 MB free of 4.3 GB) and every block below 1195132 sampled dense non-zero
 * data, so a 16-block write there would have landed in live boot.wim content.
 *
 * The rule that came out of that - THE TARGET IS AN ABSOLUTE ADDRESS, NEVER ONE
 * DERIVED FROM THE FENCE - still holds and still governs the constant below,
 * which is why widening the fence did not move the target by itself.
 *
 * TARGET, first revision: a dedicated scratch FILE. ufsscratch.bin was created
 * in the volume's free space and its extent resolved from the on-disk FAT32
 * structures (BPB: 4096 B/sector, 8 sectors/cluster, reserved 32, 2 FATs x 138
 * -> data at partition sector 308; root directory entry UFSSCR~1BIN -> cluster
 * 61856), so
 *   partition sector = 308 + (61856 - 2) * 8      = 495140
 *   disk LBA         = 68736 + 495140             = 563876
 * and the file is 256 blocks, i.e. 563876..564131. That span was then verified
 * BY MEASUREMENT, not by arithmetic alone: the raw device region was read back
 * and is byte-identical (SHA-256 D6FA54FD...) to the 1 MiB file that was
 * written, which simultaneously proves the extent is correct AND that all 256
 * blocks are contiguous.
 *
 * That was strictly safer than the fence-relative tail. The sda18 tail was
 * merely unreachable; those blocks were OWNED by a file we created and could
 * delete, so the worst case of a failed restore destroyed a scratch file rather
 * than the boot image. It was also 256 blocks rather than 4, which is 16x the
 * PRDT ceiling, so width stopped being bounded by the target.
 *
 * TARGET, current: the UNALLOCATED DISK TAIL past the last partition - a
 * different region entirely from the sda18 tail discussed above, and the reason
 * the fence was widened. See the constant below for why the target moved and
 * for the measurements that admitted it. Note that the argument for it does not
 * rest on content the way the sda18 analysis had to: free space inside a volume
 * is NOT zeroed and is full of remnants of previously written boot.wim images,
 * so content alone can never establish that a block there is unallocated - but
 * space outside every partition is unallocated according to the partition table
 * itself, which is a claim the GPT makes rather than one inferred from bytes.
 *
 * SIGNATURE GATE: because the target is now a fixed number rather than something
 * derived from the driver, a stale constant is the obvious new failure mode - if
 * the file were deleted, moved, or reallocated, that LBA would belong to someone
 * else. So the blocks must IDENTIFY THEMSELVES before anything is written: every
 * block read in step 1 must begin with UFS_SCRATCH_MAGIC followed by its own
 * little-endian index within the file. Any mismatch refuses the write outright.
 * A target that proves what it is at the moment of use cannot go stale the way a
 * proof written in a comment can.
 *
 * WIDTH LADDER: 1, 4, 8, then 16 blocks (16 x 4096 = 64 KiB is the PRDT ceiling:
 * the PRDT occupies UCD bytes 2048..4096 = 16 entries, one per 4096-byte FMP
 * data unit). Each rung is a complete independent cycle, and the ladder stops at
 * the first rung that does not restore. One boot therefore measures the whole
 * width curve instead of costing one flash per width, and a failure names the
 * exact width at which the multi-entry PRDT path breaks.
 *
 * CYCLE: read original -> check signature -> write pattern -> verify pattern ->
 * write original back -> verify restore.
 *
 * A pure write-the-same-bytes-back design was considered and rejected: it is
 * safe but vacuous, because the bytes match whether the write landed or never
 * happened at all, and the driver's own read-back verification would pass
 * either way. Writing a distinct pattern first is what makes the proof real.
 *
 * The restore is attempted even when the pattern fails to verify. If the write
 * never reached media the restore is a no-op writing identical bytes; if it did
 * reach media the restore is exactly what is needed. There is no failure path
 * on which skipping the restore is the better choice.
 */
#define UFS_WRITE_LIVE_BLOCK_SIZE 4096UL
#define UFS_WRITE_LIVE_MAX_BLOCKS 16UL
#define UFS_WRITE_LIVE_SPAN       (UFS_WRITE_LIVE_BLOCK_SIZE * UFS_WRITE_LIVE_MAX_BLOCKS)

/*
 * The ladder's target region.
 *
 * This used to be the extent of ufsscratch.bin on WINSETUP (LBA 563876) - the
 * 9810's. It is the unallocated tail past the last partition on THIS device,
 * which is what the driver's widened fence actually makes reachable.
 *
 * [HISTORY - the sda18/ufsscratch analysis above is the 9810's; the numbers in
 * the table below are this device's, read from the live GPT.]
 *
 *   sda31 USERDATA ends at LBA 31234815
 *   tail            LBA 31234816 .. 31236090, claimed by no partition
 *   GPT backup      LBA 31236091 .. 31236095 (declared by the header, empty on
 *                   the stock media - notes/s10e-root-reads.md §9 read it back
 *                   zero)
 *
 * The tail was verified empty on THIS device before being adopted: every LBA
 * from 31234812 to the end of the media reads back zero, the only non-zero one
 * in the measured run being 31234811 inside USERDATA. The 9810's equivalent
 * archive (backups\TAIL-lba15614336-1659blk-preWIDEN.bin, sha256
 * f5119dba...) is a different disk and is NOT this device's restore source.
 *
 * Because the blocks here are not a file, nothing stamps them implicitly; they
 * are stamped once from the host before the first run. On the stock S10e they
 * are ZERO, so the signature gate below refuses the ladder until that stamping
 * pass has run - the gate is what makes a stale or misplaced target address
 * refuse rather than write, and it is a device-validation step, not a constant
 * to fix here.
 */
#define UFS_SCRATCH_LBA           31234816UL   // S10e tail start, one LBA past USERDATA (live GPT)
#define UFS_SCRATCH_BLOCKS        256UL

/*
 * Black-box log channel.
 *
 * PRAM is proven unable to carry anything across a reboot: EDK2 calls
 * Star2LteTraceResetLog on every pass, which re-seeds the ring magic and zeroes
 * the cursor, and the firmware's own Windows-handoff trace fills all 16128 B
 * before winload even hands off. Measured twice - boot 5 came back with 579 B
 * of pure BDS trace and boot 6 with a 100%-full ring ending at =WLODE1.
 *
 * So the log goes to media instead, at a raw LBA inside the scratch region.
 * Scratch sits one block past USERDATA's (sda31) last LBA and below the backup
 * GPT, so no volume claims those sectors and Windows permits the raw write. The
 * first 16 blocks are the write ladder's working area; the log slots start well
 * past it and are read back from TWRP with a plain dd.
 */
#define UFS_LOGLBA_BLOCKS         16UL
#define UFS_LOGLBA_SPAN           (UFS_WRITE_LIVE_BLOCK_SIZE * UFS_LOGLBA_BLOCKS)
#define UFS_LOGLBA_HEADER         64UL
#define UFS_LOGLBA_MAGIC          "UFSLOGLBA"
#define UFS_SCRATCH_MAGIC         "UFSSCRATCHBLK"
#define UFS_SCRATCH_MAGIC_LEN     13UL
#define UFS_SCRATCH_INDEX_OFFSET  UFS_SCRATCH_MAGIC_LEN

/* The ladder must never exceed what one PRDT can describe. */
C_ASSERT(UFS_WRITE_LIVE_MAX_BLOCKS <= UFS_SCRATCH_BLOCKS);

/*
 * The whole target must clear the last partition and stop short of the backup
 * GPT. Stated against the device's partition geometry rather than against the
 * driver's fence, so an incorrectly edited fence cannot quietly move it onto
 * USERDATA.
 */
C_ASSERT(UFS_SCRATCH_LBA > 31234815UL);   // USERDATA (sda31) ends here
C_ASSERT((UFS_SCRATCH_LBA + UFS_SCRATCH_BLOCKS - 1UL) < 31236091UL); // backup GPT starts here

static BOOL
WriteLiveBlock(
    HANDLE Handle,
    ULONG Lba,
    ULONG BlockCount,
    PUCHAR Buffer,
    const char *Step
    )
{
    UFS_SPTD_WITH_SENSE Request;
    DWORD Returned = 0;
    ULONG Length = BlockCount * UFS_WRITE_LIVE_BLOCK_SIZE;
    BOOL Result;
    DWORD Error;

    ZeroMemory(&Request, sizeof(Request));
    Request.Sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    Request.Sptd.CdbLength = 10;
    Request.Sptd.SenseInfoLength = sizeof(Request.Sense);
    Request.Sptd.DataIn = SCSI_IOCTL_DATA_OUT;
    Request.Sptd.DataTransferLength = Length;
    Request.Sptd.TimeOutValue = 10;
    Request.Sptd.DataBuffer = Buffer;
    Request.Sptd.SenseInfoOffset = FIELD_OFFSET(UFS_SPTD_WITH_SENSE, Sense);
    Request.Sptd.Cdb[0] = 0x2A;
    Request.Sptd.Cdb[2] = (UCHAR)((Lba >> 24) & 0xFFU);
    Request.Sptd.Cdb[3] = (UCHAR)((Lba >> 16) & 0xFFU);
    Request.Sptd.Cdb[4] = (UCHAR)((Lba >> 8) & 0xFFU);
    Request.Sptd.Cdb[5] = (UCHAR)(Lba & 0xFFU);
    Request.Sptd.Cdb[7] = (UCHAR)((BlockCount >> 8) & 0xFFU);
    Request.Sptd.Cdb[8] = (UCHAR)(BlockCount & 0xFFU);

    Result = DeviceIoControl(
        Handle,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &Request,
        sizeof(Request),
        &Request,
        sizeof(Request),
        &Returned,
        NULL
        );
    Error = Result ? ERROR_SUCCESS : GetLastError();

    printf(
        "WLIVE_STEP=%s OP=WRITE LBA=%lu BLOCKS=%lu OK=%lu WIN32=%lu"
        " SCSI_STATUS=0x%02X XFER=%lu\n",
        Step,
        Lba,
        BlockCount,
        Result ? 1UL : 0UL,
        Error,
        (ULONG)Request.Sptd.ScsiStatus,
        Request.Sptd.DataTransferLength
        );

    /* Journal into PRAM before the next step overwrites LastWrite*. */
    (void)RunVendorDiag();
    return Result && (Request.Sptd.DataTransferLength == Length);
}

static BOOL
ReadLiveBlock(
    HANDLE Handle,
    ULONG Lba,
    ULONG BlockCount,
    PUCHAR Buffer,
    const char *Step
    )
{
    UFS_SPTD_WITH_SENSE Request;
    DWORD Returned = 0;
    ULONG Length = BlockCount * UFS_WRITE_LIVE_BLOCK_SIZE;
    BOOL Result;
    DWORD Error;

    ZeroMemory(&Request, sizeof(Request));
    ZeroMemory(Buffer, Length);
    Request.Sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    Request.Sptd.CdbLength = 10;
    Request.Sptd.SenseInfoLength = sizeof(Request.Sense);
    Request.Sptd.DataIn = SCSI_IOCTL_DATA_IN;
    Request.Sptd.DataTransferLength = Length;
    Request.Sptd.TimeOutValue = 10;
    Request.Sptd.DataBuffer = Buffer;
    Request.Sptd.SenseInfoOffset = FIELD_OFFSET(UFS_SPTD_WITH_SENSE, Sense);
    Request.Sptd.Cdb[0] = 0x28;
    Request.Sptd.Cdb[2] = (UCHAR)((Lba >> 24) & 0xFFU);
    Request.Sptd.Cdb[3] = (UCHAR)((Lba >> 16) & 0xFFU);
    Request.Sptd.Cdb[4] = (UCHAR)((Lba >> 8) & 0xFFU);
    Request.Sptd.Cdb[5] = (UCHAR)(Lba & 0xFFU);
    Request.Sptd.Cdb[7] = (UCHAR)((BlockCount >> 8) & 0xFFU);
    Request.Sptd.Cdb[8] = (UCHAR)(BlockCount & 0xFFU);

    Result = DeviceIoControl(
        Handle,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &Request,
        sizeof(Request),
        &Request,
        sizeof(Request),
        &Returned,
        NULL
        );
    Error = Result ? ERROR_SUCCESS : GetLastError();

    printf(
        "WLIVE_STEP=%s OP=READ LBA=%lu BLOCKS=%lu OK=%lu WIN32=%lu"
        " SCSI_STATUS=0x%02X XFER=%lu\n",
        Step,
        Lba,
        BlockCount,
        Result ? 1UL : 0UL,
        Error,
        (ULONG)Request.Sptd.ScsiStatus,
        Request.Sptd.DataTransferLength
        );
    if (Result) {
        PrintHexPrefix(Buffer, Length);
    }

    return Result && (Request.Sptd.DataTransferLength == Length);
}

/*
 * Layer 8 of the safeguard stack, and the one that makes a hardcoded target LBA
 * defensible: the blocks must identify themselves as our disposable scratch
 * region before a single byte is written to them.
 *
 * Every block of the region was stamped with UFS_SCRATCH_MAGIC followed by its
 * own little-endian index within the region, so the expected index for any disk
 * LBA is simply (Lba - UFS_SCRATCH_LBA). If the region were ever relocated, the
 * constants edited inconsistently, or the address simply wrong, the bytes there
 * would belong to something else and would not carry this stamp - so the write
 * is refused instead of landing on a stranger's data.
 *
 * This matters more now than it did when the region was a file. A file could be
 * deleted or defragmented underneath us, which is what this originally guarded
 * against; the current region is unallocated space, so what it guards against is
 * an arithmetic mistake pointing the ladder at USERDATA instead. Both failure
 * modes are caught by the same test, because it is a test of what is actually
 * on the media rather than of what anything here believes.
 *
 * Deliberately checked against the CONTENT read back from the device at the
 * moment of use, not against anything recorded here. A comment can go stale;
 * a self-identifying block cannot.
 */
static BOOL
ScratchSignatureValid(
    const UCHAR *Buffer,
    ULONG Lba,
    ULONG BlockCount
    )
{
    ULONG Index;

    if ((Lba < UFS_SCRATCH_LBA) ||
        ((Lba - UFS_SCRATCH_LBA) > (UFS_SCRATCH_BLOCKS - BlockCount))) {
        printf(
            "WLIVE_SCRATCH_BAD LBA=%lu BLOCKS=%lu REASON=OUTSIDE_SCRATCH"
            " SCRATCH=[%lu..%lu]\n",
            Lba,
            BlockCount,
            UFS_SCRATCH_LBA,
            UFS_SCRATCH_LBA + UFS_SCRATCH_BLOCKS - 1UL
            );
        return FALSE;
    }

    for (Index = 0; Index < BlockCount; Index++) {
        const UCHAR *Block = Buffer + (Index * UFS_WRITE_LIVE_BLOCK_SIZE);
        ULONG BlockLba = Lba + Index;
        ULONG Expected = BlockLba - UFS_SCRATCH_LBA;
        ULONG Actual;

        if (memcmp(Block, UFS_SCRATCH_MAGIC, UFS_SCRATCH_MAGIC_LEN) != 0) {
            printf(
                "WLIVE_SCRATCH_BAD LBA=%lu REASON=MAGIC"
                " FOUND=%02X%02X%02X%02X\n",
                BlockLba,
                (ULONG)Block[0],
                (ULONG)Block[1],
                (ULONG)Block[2],
                (ULONG)Block[3]
                );
            return FALSE;
        }

        Actual = (ULONG)Block[UFS_SCRATCH_INDEX_OFFSET] |
                 ((ULONG)Block[UFS_SCRATCH_INDEX_OFFSET + 1] << 8) |
                 ((ULONG)Block[UFS_SCRATCH_INDEX_OFFSET + 2] << 16) |
                 ((ULONG)Block[UFS_SCRATCH_INDEX_OFFSET + 3] << 24);
        if (Actual != Expected) {
            printf(
                "WLIVE_SCRATCH_BAD LBA=%lu REASON=INDEX EXPECTED=%lu ACTUAL=%lu\n",
                BlockLba,
                Expected,
                Actual
                );
            return FALSE;
        }
    }

    return TRUE;
}

typedef struct _UFS_WLIVE_CYCLE_RESULT {
    BOOL SignatureOk;
    BOOL PatternWritten;
    BOOL PatternVerified;
    BOOL Restored;
    BOOL RestoreVerified;
    BOOL RegionIntact;
} UFS_WLIVE_CYCLE_RESULT;

/*
 * One complete reversible cycle at a single width. Split out of RunWriteLive so
 * the ladder can run several widths in one boot, each independently restored and
 * independently reported.
 *
 * RegionIntact is the field that matters operationally: it is the only one that
 * says whether the device was left as it was found, and the ladder stops the
 * moment it goes false.
 */
static void
RunWriteLiveCycle(
    HANDLE Handle,
    ULONG Lba,
    ULONG Blocks,
    PUCHAR Original,
    PUCHAR Pattern,
    PUCHAR Readback,
    UFS_WLIVE_CYCLE_RESULT *Result
    )
{
    ULONG Span = Blocks * UFS_WRITE_LIVE_BLOCK_SIZE;
    ULONG Index;

    ZeroMemory(Result, sizeof(*Result));
    /* Nothing has been written yet, so the region is trivially still intact. */
    Result->RegionIntact = TRUE;

    printf(
        "WLIVE_CYCLE_BEGIN BLOCKS=%lu LBA=%lu LAST_LBA=%lu SPAN=%lu PRDT_ENTRIES=%lu\n",
        Blocks,
        Lba,
        Lba + Blocks - 1UL,
        Span,
        Blocks
        );

    /*
     * Step 1. Capture the original before anything is written. If this fails
     * there is nothing to restore from, so stop rather than write blind - a
     * write whose original content is unknown is exactly the situation the
     * whole reversible design exists to avoid.
     */
    if (!ReadLiveBlock(Handle, Lba, Blocks, Original, "READ_ORIGINAL")) {
        printf(
            "WLIVE_CYCLE_VERDICT BLOCKS=%lu RESULT=READ_ORIGINAL_FAILED\n",
            Blocks
            );
        return;
    }

    /* Step 2. Refuse unless these blocks are still the scratch file. */
    if (!ScratchSignatureValid(Original, Lba, Blocks)) {
        printf(
            "WLIVE_CYCLE_VERDICT BLOCKS=%lu RESULT=SCRATCH_SIGNATURE_MISMATCH\n",
            Blocks
            );
        return;
    }
    Result->SignatureOk = TRUE;
    printf("WLIVE_SCRATCH_OK BLOCKS=%lu LBA=%lu\n", Blocks, Lba);

    /*
     * Self-describing PER BLOCK, not merely per transfer. A uniform fill with a
     * single header would verify identically whether the data units landed at
     * their own addresses or the same unit was replayed N times - and per-unit
     * misdirection is precisely the failure mode a multi-entry PRDT introduces,
     * since the FMP advances its data-unit number once per entry. Stamping each
     * block with its own absolute LBA makes any reordering, duplication, or
     * off-by-one land as a mismatch naming the exact block.
     */
    FillMemory(Pattern, Span, 0x5A);
    for (Index = 0; Index < Blocks; Index++) {
        PUCHAR Block = Pattern + (Index * UFS_WRITE_LIVE_BLOCK_SIZE);
        ULONG BlockLba = Lba + Index;

        Block[0] = 'U';
        Block[1] = 'F';
        Block[2] = 'S';
        Block[3] = 'L';
        Block[4] = (UCHAR)(BlockLba & 0xFFU);
        Block[5] = (UCHAR)((BlockLba >> 8) & 0xFFU);
        Block[6] = (UCHAR)((BlockLba >> 16) & 0xFFU);
        Block[7] = (UCHAR)((BlockLba >> 24) & 0xFFU);
        Block[8] = (UCHAR)Index;
        Block[9] = (UCHAR)Blocks;
    }

    /* Step 3. One multi-block write across the whole width. */
    Result->PatternWritten = WriteLiveBlock(Handle, Lba, Blocks, Pattern,
                                            "WRITE_PATTERN");
    /*
     * From here the region may differ from what was found, and stays that way
     * until VERIFY_RESTORE proves otherwise. Set before the write is even
     * evaluated: a command that reported failure may still have reached media.
     */
    Result->RegionIntact = FALSE;

    /*
     * Step 4. Independent host-side confirmation. The driver already verified
     * this internally, but that check reads through the same descriptor path
     * that performed the write; re-reading through the ordinary SCSI path is a
     * genuinely separate observation.
     */
    if (ReadLiveBlock(Handle, Lba, Blocks, Readback, "VERIFY_PATTERN")) {
        Result->PatternVerified = TRUE;
        for (Index = 0; Index < Span; Index++) {
            if (Readback[Index] != Pattern[Index]) {
                printf(
                    "WLIVE_MISMATCH STEP=VERIFY_PATTERN BLOCKS=%lu OFFSET=%lu"
                    " BLOCK=%lu LBA=%lu EXPECTED=0x%02X ACTUAL=0x%02X\n",
                    Blocks,
                    Index,
                    Index / UFS_WRITE_LIVE_BLOCK_SIZE,
                    Lba + (Index / UFS_WRITE_LIVE_BLOCK_SIZE),
                    (ULONG)Pattern[Index],
                    (ULONG)Readback[Index]
                    );
                Result->PatternVerified = FALSE;
                break;
            }
        }
    }
    printf(
        "WLIVE_PATTERN_VERIFIED BLOCKS=%lu VALUE=%lu\n",
        Blocks,
        Result->PatternVerified ? 1UL : 0UL
        );

    /*
     * Step 5. Restore unconditionally - see the header comment. Even when the
     * write above did nothing, this writes back bytes identical to what is
     * already there.
     */
    Result->Restored = WriteLiveBlock(Handle, Lba, Blocks, Original,
                                      "RESTORE_ORIGINAL");

    /* Step 6. Prove the span is byte-for-byte what it was on entry. */
    if (ReadLiveBlock(Handle, Lba, Blocks, Readback, "VERIFY_RESTORE")) {
        Result->RestoreVerified = TRUE;
        for (Index = 0; Index < Span; Index++) {
            if (Readback[Index] != Original[Index]) {
                printf(
                    "WLIVE_MISMATCH STEP=VERIFY_RESTORE BLOCKS=%lu OFFSET=%lu"
                    " BLOCK=%lu LBA=%lu EXPECTED=0x%02X ACTUAL=0x%02X\n",
                    Blocks,
                    Index,
                    Index / UFS_WRITE_LIVE_BLOCK_SIZE,
                    Lba + (Index / UFS_WRITE_LIVE_BLOCK_SIZE),
                    (ULONG)Original[Index],
                    (ULONG)Readback[Index]
                    );
                Result->RestoreVerified = FALSE;
                break;
            }
        }
    }
    Result->RegionIntact = Result->RestoreVerified;
    printf(
        "WLIVE_RESTORE_VERIFIED BLOCKS=%lu VALUE=%lu\n",
        Blocks,
        Result->RestoreVerified ? 1UL : 0UL
        );

    /*
     * The verdict ordering puts restoration first because it is the only one
     * that decides whether the device was left as it was found. A run that
     * wrote nothing is a disappointment; a run that wrote and could not restore
     * is the one that needs a human.
     */
    if (!Result->RestoreVerified) {
        printf(
            "WLIVE_CYCLE_VERDICT BLOCKS=%lu RESULT=RESTORE_FAILED_NEEDS_ATTENTION\n",
            Blocks
            );
    } else if (Result->PatternWritten && Result->PatternVerified) {
        printf(
            "WLIVE_CYCLE_VERDICT BLOCKS=%lu RESULT=LIVE_WRITE_PROVEN\n",
            Blocks
            );
    } else if (Result->PatternWritten) {
        printf(
            "WLIVE_CYCLE_VERDICT BLOCKS=%lu RESULT=WRITE_ACCEPTED_NOT_OBSERVED\n",
            Blocks
            );
    } else {
        printf(
            "WLIVE_CYCLE_VERDICT BLOCKS=%lu RESULT=WRITE_REFUSED_BLOCK_INTACT\n",
            Blocks
            );
    }
}

static void
RunWriteLive(
    void
    )
{
    static const ULONG Ladder[] = { 1UL, 4UL, 8UL, UFS_WRITE_LIVE_MAX_BLOCKS };
    HANDLE Handle;
    PUCHAR Buffer;
    PUCHAR Original;
    PUCHAR Pattern;
    PUCHAR Readback;
    ULONG FenceFirst = 0;
    ULONG FenceLast = 0;
    ULONG WriteMode = UFS_WRITE_MODE_DISARMED;
    ULONG CrashLockout = 0;
    ULONG CrashAttempt = 0;
    ULONG Lba = UFS_SCRATCH_LBA;
    ULONG LastLba = UFS_SCRATCH_LBA + UFS_WRITE_LIVE_MAX_BLOCKS - 1UL;
    ULONG Rung;
    ULONG WidestProven = 0;
    BOOL RegionIntact = TRUE;
    UFS_WLIVE_CYCLE_RESULT Cycle;

    if (!QueryWriteFence(&FenceFirst, &FenceLast, &WriteMode,
                         &CrashLockout, &CrashAttempt)) {
        printf("UFS_WRITE_LIVE=0 REASON=NO_FENCE\n");
        return;
    }

    if (CrashLockout != 0) {
        printf(
            "UFS_WRITE_LIVE=0 REASON=CRASH_LOCKOUT PREVIOUS_ATTEMPT=%lu\n",
            CrashAttempt
            );
        return;
    }

    /*
     * Fail closed unless the adapter is already LIVE. This verb deliberately
     * cannot arm anything: reaching LIVE takes a separate --arm-live carrying
     * its own confirm word, so "the adapter was armed for real writes" and "a
     * real write was attempted" stay two independently auditable events.
     */
    if (WriteMode != UFS_WRITE_MODE_LIVE) {
        printf(
            "UFS_WRITE_LIVE=0 REASON=NOT_LIVE MODE=%s\n",
            WriteModeName(WriteMode)
            );
        return;
    }

    /*
     * The scratch region is a fixed address rather than something derived from
     * the fence, so it is checked AGAINST the fence rather than computed from
     * it. That keeps the fence authoritative: a target outside it is refused
     * here rather than left to the driver to reject downstream, because a
     * request that only survives because something else says no is not a
     * safeguard. What proves the address is still the RIGHT one is not this
     * check but the per-cycle signature gate, which reads the blocks and
     * requires them to identify themselves as the scratch file.
     */
    if ((Lba < FenceFirst) || (LastLba > FenceLast)) {
        printf(
            "UFS_WRITE_LIVE=0 REASON=SCRATCH_OUTSIDE_FENCE FENCE=[%lu..%lu]"
            " SCRATCH=[%lu..%lu]\n",
            FenceFirst,
            FenceLast,
            Lba,
            LastLba
            );
        return;
    }

    Buffer = (PUCHAR)VirtualAlloc(
        NULL,
        UFS_WRITE_LIVE_SPAN * 3,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
        );
    if (Buffer == NULL) {
        printf("UFS_WRITE_LIVE=0 REASON=ALLOC\n");
        return;
    }
    Original = Buffer;
    Pattern = Buffer + UFS_WRITE_LIVE_SPAN;
    Readback = Buffer + (UFS_WRITE_LIVE_SPAN * 2);

    Handle = CreateFileW(
        L"\\\\.\\PhysicalDrive0",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Handle == INVALID_HANDLE_VALUE) {
        printf("UFS_WRITE_LIVE=0 REASON=OPEN ERROR=%lu\n", GetLastError());
        VirtualFree(Buffer, 0, MEM_RELEASE);
        return;
    }

    printf(
        "UFS_WRITE_LIVE=1 MODE=%s FENCE=[%lu..%lu] SCRATCH=[%lu..%lu]"
        " TARGET_LBA=%lu MAX_BLOCKS=%lu LADDER=1,4,8,%lu\n",
        WriteModeName(WriteMode),
        FenceFirst,
        FenceLast,
        UFS_SCRATCH_LBA,
        UFS_SCRATCH_LBA + UFS_SCRATCH_BLOCKS - 1UL,
        Lba,
        UFS_WRITE_LIVE_MAX_BLOCKS,
        UFS_WRITE_LIVE_MAX_BLOCKS
        );

    /*
     * Walk the width ladder. Every rung is a complete, independently restored
     * cycle at the SAME base LBA, so a rung that fails leaves exactly the same
     * blocks to answer for as the rung before it - the failure is attributable
     * to the width alone rather than to where it landed.
     *
     * The ladder stops the instant a rung does not restore. Continuing after an
     * unrestored region would write over evidence, and the next rung's signature
     * gate would refuse anyway; stopping makes that explicit rather than
     * incidental.
     */
    for (Rung = 0; Rung < (sizeof(Ladder) / sizeof(Ladder[0])); Rung++) {
        ULONG Blocks = Ladder[Rung];

        RunWriteLiveCycle(Handle, Lba, Blocks, Original, Pattern, Readback,
                          &Cycle);

        if (Cycle.PatternWritten && Cycle.PatternVerified &&
            Cycle.RestoreVerified) {
            WidestProven = Blocks;
        }

        if (!Cycle.RegionIntact) {
            RegionIntact = FALSE;
            printf(
                "WLIVE_LADDER_STOPPED AT_BLOCKS=%lu REASON=REGION_NOT_RESTORED\n",
                Blocks
                );
            break;
        }

        if (!Cycle.SignatureOk) {
            printf(
                "WLIVE_LADDER_STOPPED AT_BLOCKS=%lu REASON=SIGNATURE\n",
                Blocks
                );
            break;
        }

        if (!Cycle.PatternWritten) {
            printf(
                "WLIVE_LADDER_STOPPED AT_BLOCKS=%lu REASON=WRITE_REFUSED\n",
                Blocks
                );
            break;
        }

        if (!Cycle.PatternVerified) {
            printf(
                "WLIVE_LADDER_STOPPED AT_BLOCKS=%lu REASON=PATTERN_NOT_OBSERVED\n",
                Blocks
                );
            break;
        }
    }

    /*
     * WIDEST_PROVEN is the number this whole exercise exists to produce: the
     * largest multi-block write that was issued, observed on a separate read,
     * and undone. REGION_INTACT is the one that decides whether a human is
     * needed, so it is reported alongside rather than buried in the verdict.
     */
    printf(
        "UFS_WRITE_LIVE_SUMMARY WIDEST_PROVEN_BLOCKS=%lu WIDEST_PROVEN_BYTES=%lu"
        " REGION_INTACT=%lu\n",
        WidestProven,
        WidestProven * UFS_WRITE_LIVE_BLOCK_SIZE,
        RegionIntact ? 1UL : 0UL
        );

    if (!RegionIntact) {
        printf("UFS_WRITE_LIVE_VERDICT=RESTORE_FAILED_NEEDS_ATTENTION\n");
        printf(
            "UFS_WRITE_LIVE_RECOVERY REGION=[%lu..%lu]"
            " SOURCE=zero-fill (this disk's stock tail reads back zero,"
            " notes/s10e-root-reads.md §9; the 9810's archive"
            " backups\\TAIL-lba15614336-1659blk-preWIDEN.bin is a different disk)\n",
            Lba,
            LastLba
            );
    } else if (WidestProven >= UFS_WRITE_LIVE_MAX_BLOCKS) {
        printf("UFS_WRITE_LIVE_VERDICT=WIDE_WRITE_PROVEN\n");
    } else if (WidestProven > 0) {
        printf("UFS_WRITE_LIVE_VERDICT=PARTIAL_WIDTH_PROVEN\n");
    } else {
        printf("UFS_WRITE_LIVE_VERDICT=WRITE_REFUSED_BLOCK_INTACT\n");
    }

    CloseHandle(Handle);
    VirtualFree(Buffer, 0, MEM_RELEASE);
    printf("UFS_WRITE_LIVE_COMPLETE=1\n");
}

/*
 * Arm, downgrade, or disarm the write path via vendor CDB 0xD2. This is layer 4
 * and layer 5 of the safeguard stack: the driver boots DISARMED and refuses
 * every write until this runs, and reaching LIVE needs a different confirm word
 * from DRY_RUN, so asking for real writes is individually auditable rather than
 * a side effect of asking for writes at all.
 *
 * Nothing here can move data. The CDB carries no payload and the driver answers
 * it from memory, so a mistyped verb can only change a latch.
 */
static BOOL
RequestWriteArm(
    ULONG Confirm,
    const char *ModeLabel
    )
{
    UFS_SPTD_WITH_SENSE Request;
    HANDLE Handle;
    DWORD Returned = 0;
    BOOL Result;
    DWORD Error;

    Handle = CreateFileW(
        L"\\\\.\\PhysicalDrive0",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Handle == INVALID_HANDLE_VALUE) {
        printf("UFS_WRITE_ARM=0 MODE=%s REASON=OPEN ERROR=%lu\n",
            ModeLabel, GetLastError());
        return FALSE;
    }

    ZeroMemory(&Request, sizeof(Request));
    Request.Sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    Request.Sptd.CdbLength = 10;
    Request.Sptd.SenseInfoLength = sizeof(Request.Sense);
    Request.Sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
    Request.Sptd.DataTransferLength = 0;
    Request.Sptd.TimeOutValue = 10;
    Request.Sptd.DataBuffer = NULL;
    Request.Sptd.SenseInfoOffset = FIELD_OFFSET(UFS_SPTD_WITH_SENSE, Sense);
    Request.Sptd.Cdb[0] = UFS_DIAG_WRITE_ARM_CDB_OPCODE;
    Request.Sptd.Cdb[1] = (UCHAR)((UFS_DIAG_WRITE_ARM_CDB_SUBCODE >> 8) & 0xFFU);
    Request.Sptd.Cdb[2] = (UCHAR)(UFS_DIAG_WRITE_ARM_CDB_SUBCODE & 0xFFU);
    Request.Sptd.Cdb[3] = (UCHAR)((Confirm >> 24) & 0xFFU);
    Request.Sptd.Cdb[4] = (UCHAR)((Confirm >> 16) & 0xFFU);
    Request.Sptd.Cdb[5] = (UCHAR)((Confirm >> 8) & 0xFFU);
    Request.Sptd.Cdb[6] = (UCHAR)(Confirm & 0xFFU);

    Result = DeviceIoControl(
        Handle,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &Request,
        sizeof(Request),
        &Request,
        sizeof(Request),
        &Returned,
        NULL
        );
    Error = Result ? ERROR_SUCCESS : GetLastError();
    CloseHandle(Handle);

    printf(
        "UFS_WRITE_ARM=%lu MODE=%s WIN32=%lu SCSI_STATUS=0x%02X\n",
        (ULONG)(Result ? 1 : 0),
        ModeLabel,
        Error,
        (ULONG)Request.Sptd.ScsiStatus
        );
    return Result;
}

/*
 * Put one short note into PRAM, where it survives the handback reset.
 *
 * This exists because RunFilesystemWrite has nowhere else to speak from. Its
 * stdout lands on X:, the WinPE RAM disk, which the PMU reset destroys, and its
 * only other channel is a file on the UFS volume - the very thing that fails
 * when the filesystem write fails. So the failure that most needs explaining is
 * the one that currently explains nothing.
 *
 * Cloned from RequestWriteArm because that shape is proven on this stack:
 * SCSI_IOCTL_DATA_UNSPECIFIED means no SRB_FLAGS_DATA_OUT, so the miniport's
 * read-only opcode allowlist is never consulted and this cannot become a write
 * path. The CDB is 16 bytes, which is legal because both Srb->Cdb and
 * SCSI_PASS_THROUGH_DIRECT.Cdb are 16 bytes.
 *
 * Failure is deliberately silent apart from the printf: a note that cannot be
 * delivered must never change the outcome of the operation it is describing.
 */
static BOOL
SendVendorNote(
    ULONG Code,
    ULONG Aux,
    ULONG Tag
    )
{
    UFS_SPTD_WITH_SENSE Request;
    HANDLE Handle;
    DWORD Returned = 0;
    BOOL Result;
    DWORD Error;

    Handle = CreateFileW(
        L"\\\\.\\PhysicalDrive0",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Handle == INVALID_HANDLE_VALUE) {
        printf("UFS_NOTE=0 CODE=%lu REASON=OPEN ERROR=%lu\n",
            Code, GetLastError());
        return FALSE;
    }

    ZeroMemory(&Request, sizeof(Request));
    Request.Sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    Request.Sptd.CdbLength = 16;
    Request.Sptd.SenseInfoLength = sizeof(Request.Sense);
    Request.Sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
    Request.Sptd.DataTransferLength = 0;
    Request.Sptd.TimeOutValue = 10;
    Request.Sptd.DataBuffer = NULL;
    Request.Sptd.SenseInfoOffset = FIELD_OFFSET(UFS_SPTD_WITH_SENSE, Sense);
    Request.Sptd.Cdb[0] = UFS_DIAG_NOTE_CDB_OPCODE;
    Request.Sptd.Cdb[1] = (UCHAR)((UFS_DIAG_NOTE_CDB_SUBCODE >> 8) & 0xFFU);
    Request.Sptd.Cdb[2] = (UCHAR)(UFS_DIAG_NOTE_CDB_SUBCODE & 0xFFU);
    Request.Sptd.Cdb[3] = (UCHAR)((UFS_DIAG_NOTE_CDB_CONFIRM >> 24) & 0xFFU);
    Request.Sptd.Cdb[4] = (UCHAR)((UFS_DIAG_NOTE_CDB_CONFIRM >> 16) & 0xFFU);
    Request.Sptd.Cdb[5] = (UCHAR)((UFS_DIAG_NOTE_CDB_CONFIRM >> 8) & 0xFFU);
    Request.Sptd.Cdb[6] = (UCHAR)(UFS_DIAG_NOTE_CDB_CONFIRM & 0xFFU);
    Request.Sptd.Cdb[7] = (UCHAR)(Code & 0xFFU);
    Request.Sptd.Cdb[8] = (UCHAR)((Aux >> 24) & 0xFFU);
    Request.Sptd.Cdb[9] = (UCHAR)((Aux >> 16) & 0xFFU);
    Request.Sptd.Cdb[10] = (UCHAR)((Aux >> 8) & 0xFFU);
    Request.Sptd.Cdb[11] = (UCHAR)(Aux & 0xFFU);
    Request.Sptd.Cdb[12] = (UCHAR)((Tag >> 24) & 0xFFU);
    Request.Sptd.Cdb[13] = (UCHAR)((Tag >> 16) & 0xFFU);
    Request.Sptd.Cdb[14] = (UCHAR)((Tag >> 8) & 0xFFU);
    Request.Sptd.Cdb[15] = (UCHAR)(Tag & 0xFFU);

    Result = DeviceIoControl(
        Handle,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &Request,
        sizeof(Request),
        &Request,
        sizeof(Request),
        &Returned,
        NULL
        );
    Error = Result ? ERROR_SUCCESS : GetLastError();
    CloseHandle(Handle);

    printf(
        "UFS_NOTE=%lu CODE=%lu AUX=%lu TAG=%lu WIN32=%lu\n",
        (ULONG)(Result ? 1 : 0),
        Code,
        Aux,
        Tag,
        Error
        );
    return Result;
}

/*
 * Ask the miniport to put the SoC back into recovery.
 *
 * The CDB carries the subcode and a confirmation word because this is a reset,
 * not an I/O. Success is indistinguishable from a hang from here - the SoC is
 * gone before DeviceIoControl returns - so the caller treats a returning call
 * as failure and says so.
 */
static
BOOL
RequestRebootToRecovery(
    VOID
    )
{
    UFS_SPTD_WITH_SENSE Request;
    HANDLE Handle;
    DWORD Returned = 0;
    BOOL Result;
    DWORD Error;

    Handle = CreateFileW(
        L"\\\\.\\PhysicalDrive0",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Handle == INVALID_HANDLE_VALUE) {
        printf("UFS_REBOOT=0 REASON=OPEN ERROR=%lu\n", GetLastError());
        return FALSE;
    }

    ZeroMemory(&Request, sizeof(Request));
    Request.Sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    Request.Sptd.CdbLength = 10;
    Request.Sptd.SenseInfoLength = sizeof(Request.Sense);
    Request.Sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
    Request.Sptd.DataTransferLength = 0;
    Request.Sptd.TimeOutValue = 10;
    Request.Sptd.DataBuffer = NULL;
    Request.Sptd.SenseInfoOffset = FIELD_OFFSET(UFS_SPTD_WITH_SENSE, Sense);
    Request.Sptd.Cdb[0] = UFS_DIAG_REBOOT_CDB_OPCODE;
    Request.Sptd.Cdb[1] = (UCHAR)((UFS_DIAG_REBOOT_CDB_SUBCODE >> 8) & 0xFFU);
    Request.Sptd.Cdb[2] = (UCHAR)(UFS_DIAG_REBOOT_CDB_SUBCODE & 0xFFU);
    Request.Sptd.Cdb[3] = (UCHAR)((UFS_DIAG_REBOOT_CDB_CONFIRM >> 24) & 0xFFU);
    Request.Sptd.Cdb[4] = (UCHAR)((UFS_DIAG_REBOOT_CDB_CONFIRM >> 16) & 0xFFU);
    Request.Sptd.Cdb[5] = (UCHAR)((UFS_DIAG_REBOOT_CDB_CONFIRM >> 8) & 0xFFU);
    Request.Sptd.Cdb[6] = (UCHAR)(UFS_DIAG_REBOOT_CDB_CONFIRM & 0xFFU);

    printf("UFS_REBOOT_REQUEST=1\n");
    fflush(stdout);

    Result = DeviceIoControl(
        Handle,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &Request,
        sizeof(Request),
        &Request,
        sizeof(Request),
        &Returned,
        NULL
        );
    Error = Result ? ERROR_SUCCESS : GetLastError();
    CloseHandle(Handle);

    //
    // Reaching this line at all means the reset did not happen.
    //
    printf(
        "UFS_REBOOT=0 REASON=RETURNED WIN32=%lu SCSI_STATUS=0x%02X\n",
        Error,
        (ULONG)Request.Sptd.ScsiStatus
        );
    return FALSE;
}

/*
 * Unattended-loop watchdog.
 *
 * winpeshl.ini runs the loader to COMPLETION before StatusBoard starts, and the
 * dashboard countdown is the only thing that fires the return-to-TWRP. So any
 * hang inside the loader takes the reboot down with it, and the host then has no
 * way back in at all, because WinPE exposes no adbd. That is not theoretical:
 * two consecutive write-probe boots stranded the phone while PRAM showed the
 * adapter perfectly healthy (STARTED=1, 94 commands, containment quiet).
 *
 * This verb is launched detached from the TOP of the loader, so it is already
 * counting before anything downstream can wedge, and it does not care whether
 * the loader or the dashboard ever get anywhere. It sleeps, then issues the same
 * vendor reset the dashboard would have issued.
 *
 * Sleep() is used rather than timeout.exe or ping.exe deliberately - this image
 * has already been bitten twice by assuming a stock Windows binary exists in
 * WinPE (findstr.exe in the V12 boot gate, sc.exe in the trust checks). The only
 * binary this can depend on is the one it is already running inside.
 *
 * The reset needs PhysicalDrive0, which may not be enumerated yet on a slow or
 * partly-failed boot, so a miss is retried rather than treated as terminal.
 */
#define UFS_DIAG_WATCHDOG_ATTEMPTS   6UL
#define UFS_DIAG_WATCHDOG_RETRY_MS   10000UL
#define UFS_DIAG_WATCHDOG_MAX_SEC    3600UL

static
int
RunRebootWatchdog(
    ULONG DelaySeconds
    )
{
    ULONG attempt;

    if (DelaySeconds > UFS_DIAG_WATCHDOG_MAX_SEC) {
        DelaySeconds = UFS_DIAG_WATCHDOG_MAX_SEC;
    }

    printf("UFS_WATCHDOG_ARMED=1 DELAY_SEC=%lu\n", DelaySeconds);
    fflush(stdout);

    Sleep(DelaySeconds * 1000UL);

    for (attempt = 0; attempt < UFS_DIAG_WATCHDOG_ATTEMPTS; attempt++) {
        printf("UFS_WATCHDOG_FIRE=%lu\n", attempt + 1UL);
        fflush(stdout);

        //
        // Returns only on failure; a successful reset never comes back.
        //
        (VOID)RequestRebootToRecovery();
        Sleep(UFS_DIAG_WATCHDOG_RETRY_MS);
    }

    printf("UFS_WATCHDOG=0 REASON=EXHAUSTED\n");
    return 1;
}

/*
 * FILESYSTEM WRITE PROOF
 *
 * Every write this project has proven so far was issued by this helper as a
 * hand-built WRITE(10)/WRITE(16) with an LBA taken from the driver's own fence.
 * That proves the descriptor, the PRDT and the DMA path, but it does not prove
 * the driver works as a *disk*: Windows' own filesystem has never written
 * through it. This verb closes that gap by asking the FAT driver to create an
 * ordinary file, which becomes directory-entry updates, FAT-table updates and
 * data-cluster writes at LBAs this helper never chose and cannot predict.
 *
 * The payload is deliberately 16 MiB. At the 64 KiB per-command hardware
 * ceiling Windows must split its data into at least 256 writes, and the driver
 * verifies every one with a second read command. That pushes the same real FAT
 * path past 500 hardware commands - well beyond the roughly 100-command regime
 * where the old lifetime re-arm budget used to kill an otherwise healthy boot.
 *
 * Three properties make it safe to run unattended:
 *
 *   - the volume is identified by CONTENT, never by a guessed drive letter: it
 *     must carry both our own ufsscratch.bin marker and the WINSETUP layout;
 *   - every resulting write still passes through the driver's fence and GPT
 *     guard, which run ahead of the arm latch, so even a misidentified volume
 *     cannot place a byte outside the fence (sda18 on the 9810; on this device
 *     the fence spans the ESP, the Windows volume and the tail);
 *   - it flushes and dismounts before returning. The phone is handed back to
 *     TWRP by a hard PMU reset, which is not an orderly shutdown - anything
 *     still in FAT's write-behind cache at that moment is lost, and a
 *     half-updated FAT is materially worse than no write at all.
 */
#define UFS_FSWRITE_RECORD_SIZE  64UL
#define UFS_FSWRITE_RECORDS      262144UL
#define UFS_FSWRITE_TOTAL        (UFS_FSWRITE_RECORD_SIZE * UFS_FSWRITE_RECORDS)
C_ASSERT(UFS_FSWRITE_TOTAL == (16UL * 1024UL * 1024UL));
C_ASSERT(UFS_FSWRITE_TOTAL <= MAXDWORD);

/*
 * The mount point the survey assigns when it finds a volume that Windows left
 * without a drive letter. W: is chosen because WinPE reserves X: for its RAM
 * disk and assigns letters from C: upward, so W: is free by construction here
 * and cannot collide with a volume the survey is about to enumerate.
 *
 * UfsFsWriteFindVolume needs no change to see it: it scans A-Z against
 * GetLogicalDrives(), and a successful SetVolumeMountPointW makes W: appear in
 * that mask immediately.
 */
#define UFS_SURVEY_MOUNT         L"W:\\"
#define UFS_SURVEY_MOUNT_LETTER  L'W'

/*
 * IOCTL_DISK_GET_DRIVE_LAYOUT_EX returns a variable-length array of
 * PARTITION_INFORMATION_EX, so the buffer is derived from the worst case rather
 * than picked: this disk's GPT header declares NumberOfEntries = 128, and
 * partmgr is free to report every one of them.
 *
 * Measured on the host (10 partitions -> 1488 bytes returned), the layout is a
 * 48-byte header followed by 144 bytes per entry, so a flat 8 KiB would have
 * held only 56 entries - less than half the declared maximum. Deriving the size
 * from the two sizeofs keeps the bound correct if either struct ever changes.
 * DRIVE_LAYOUT_INFORMATION_EX already carries one trailing entry, so this
 * over-allocates by exactly one, which is the safe direction.
 */
#define UFS_SURVEY_MAX_PARTITIONS  128UL
#define UFS_SURVEY_LAYOUT_BYTES                                 \
    ((DWORD)(sizeof(DRIVE_LAYOUT_INFORMATION_EX) +              \
             (UFS_SURVEY_MAX_PARTITIONS *                       \
              sizeof(PARTITION_INFORMATION_EX))))

/*
 * This device's capacity, from READ CAPACITY(10) on the very first boot that
 * reached hardware and reconfirmed on every boot since: 15,616,000 blocks of
 * 4096 bytes. Expressed in MiB because that is what the note carries, and it
 * divides exactly (63,963,136,000 / 1048576 = 61000) so there is no rounding to
 * reason about when reading the value back.
 *
 * Its only use is to let the probe assert that whatever disk.sys reports is the
 * disk this driver is servicing, rather than trusting the device path alone.
 */
#define UFS_SURVEY_EXPECT_SIZE_MIB  61000UL
#define UFS_SURVEY_EXPECT_SECTOR    4096UL

/*
 * Six 4096-byte blocks, LBA 0 through LBA 5: LBA 0 for the protective MBR
 * signature, LBA 1 for the GPT header, and LBA 2..5 for the whole 16384-byte
 * primary partition entry array. All of it has to come from one read, because a
 * physical drive handle only accepts offsets and lengths that are whole
 * multiples of the sector size, so LBA 1 cannot be reached without also reading
 * LBA 0.
 *
 * This is exactly the span partmgr must read to parse the table, so covering it
 * makes the probe a like-for-like stand-in for the consumer that is failing.
 *
 * It is also the first multi-block read this project has sent through Windows'
 * storage stack. Multi-block is proven at the driver level (the write ladder
 * re-reads 16 blocks in one command), but every read that has traversed
 * disk.sys so far was a single 8192-byte pair, so a failure here is itself the
 * finding rather than a setback.
 *
 * The driver's read validator admits 1 to 16 blocks per command, so six blocks
 * is comfortably inside what the CDB classifier will pass, and well under the
 * 64 KiB ceiling the 16-entry PRDT can express.
 */
#define UFS_PROBE_READ_BYTES        24576UL

#define UFS_PROBE_F_DEVICE_NUMBER   0x01UL
#define UFS_PROBE_F_GEOMETRY        0x02UL
#define UFS_PROBE_F_LENGTH          0x04UL
#define UFS_PROBE_F_SIZE_MATCHES    0x08UL
#define UFS_PROBE_F_PARTITION_INFO  0x10UL

#define UFS_READ_F_SEEK             0x01UL
#define UFS_READ_F_READ             0x02UL
#define UFS_READ_F_GPT_SIGNATURE    0x04UL
#define UFS_READ_F_MBR_SIGNATURE    0x08UL

/*
 * The upper nibble carries the GPT structural verdict.
 *
 * These ride in the existing DISK_READ note's spare flag bits rather than in a
 * note of their own. The note channel has eight slots shared by twenty-three
 * reason codes and allocated in call order, and the eight that fire on the
 * expected path are already spoken for, so an extra note would silently push
 * the settled fs-write answer out of the last slot. Spare bits cost nothing.
 *
 * HDR_CRC_92 and HDR_CRC_DECL are deliberately separate. This disk's header
 * declares HeaderSize 512, not the 92 the UEFI spec defines, and the bytes past
 * 92 are known to be non-zero, so the two CRCs cannot both match. Which one
 * matches identifies whose interpretation of HeaderSize is right.
 */
#define UFS_READ_F_HDR_CRC_92       0x10UL
#define UFS_READ_F_HDR_CRC_DECL     0x20UL
#define UFS_READ_F_ENTRY_CRC        0x40UL
#define UFS_READ_F_ENTRIES          0x80UL

/* Byte offsets inside the GPT header, which itself starts at LBA 1. */
#define UFS_GPT_OFF_HEADER_SIZE     12UL
#define UFS_GPT_OFF_HEADER_CRC      16UL
#define UFS_GPT_OFF_ENTRY_LBA       72UL
#define UFS_GPT_OFF_ENTRY_COUNT     80UL
#define UFS_GPT_OFF_ENTRY_SIZE      84UL
#define UFS_GPT_OFF_ENTRY_CRC       88UL
#define UFS_GPT_HEADER_MIN          92UL

/*
 * Walk the disk device stack from the bottom up and record which layer still
 * answers.
 *
 * The order is deliberate and is the whole point of the function: each request
 * is owned by a different driver, so the lowest one that fails names the layer
 * that is missing rather than merely restating that something is wrong.
 *
 *   IOCTL_STORAGE_GET_DEVICE_NUMBER   classpnp
 *   IOCTL_DISK_GET_DRIVE_GEOMETRY_EX  disk.sys
 *   IOCTL_DISK_GET_LENGTH_INFO        disk.sys
 *   IOCTL_DISK_GET_PARTITION_INFO_EX  partmgr
 *
 * The last one is the addition that matters. GET_DRIVE_LAYOUT_EX already
 * returns ERROR_INVALID_FUNCTION, but a single failing code proves nothing
 * about which driver produced it. GET_PARTITION_INFO_EX (0x00070048) is a
 * second, independent partmgr-only code, so if it fails identically the whole
 * partmgr IOCTL family is dark rather than one request being malformed.
 *
 * Only the first failure's Win32 code is kept. A later one would be a
 * consequence of the first, and reporting it instead would point at the wrong
 * layer - which is exactly the mistake the single DISK_LAYOUT_FAIL note made.
 */
static
VOID
UfsProbeDiskStack(
    HANDLE Disk
    )
{
    STORAGE_DEVICE_NUMBER Number;
    DISK_GEOMETRY_EX Geometry;
    GET_LENGTH_INFORMATION Length;
    PARTITION_INFORMATION_EX Partition;
    DWORD Bytes = 0;
    DWORD Flags = 0;
    DWORD FirstFail = 0;
    DWORD SectorSize = 0;
    ULONGLONG Size = 0;
    ULONGLONG SizeMib = 0;

    memset(&Number, 0, sizeof(Number));
    memset(&Geometry, 0, sizeof(Geometry));
    memset(&Length, 0, sizeof(Length));
    memset(&Partition, 0, sizeof(Partition));

    if (DeviceIoControl(Disk, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
            &Number, sizeof(Number), &Bytes, NULL)) {
        Flags |= UFS_PROBE_F_DEVICE_NUMBER;
    } else if (FirstFail == 0) {
        FirstFail = GetLastError();
    }

    if (DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
            &Geometry, sizeof(Geometry), &Bytes, NULL)) {
        Flags |= UFS_PROBE_F_GEOMETRY;
        SectorSize = Geometry.Geometry.BytesPerSector;
        Size = (ULONGLONG)Geometry.DiskSize.QuadPart;
    } else if (FirstFail == 0) {
        FirstFail = GetLastError();
    }

    if (DeviceIoControl(Disk, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
            &Length, sizeof(Length), &Bytes, NULL)) {
        Flags |= UFS_PROBE_F_LENGTH;
        if (Size == 0) {
            Size = (ULONGLONG)Length.Length.QuadPart;
        }
    } else if (FirstFail == 0) {
        FirstFail = GetLastError();
    }

    SizeMib = Size / (1024ULL * 1024ULL);
    if ((SizeMib == UFS_SURVEY_EXPECT_SIZE_MIB) &&
        (SectorSize == UFS_SURVEY_EXPECT_SECTOR)) {
        Flags |= UFS_PROBE_F_SIZE_MATCHES;
    }

    /*
     * Issued last because it sits highest in the stack. If everything below
     * succeeded and only this fails, FirstFail names partmgr's absence rather
     * than a lower layer, which is the whole point of the bottom-up order.
     */
    if (DeviceIoControl(Disk, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0,
            &Partition, sizeof(Partition), &Bytes, NULL)) {
        Flags |= UFS_PROBE_F_PARTITION_INFO;
    } else if (FirstFail == 0) {
        FirstFail = GetLastError();
    }

    /* Capped rather than truncated, so an absurd value reads as absurd. */
    if (SizeMib > 0xFFFFULL) {
        SizeMib = 0xFFFFULL;
    }
    if (SectorSize > 0xFFFFUL) {
        SectorSize = 0xFFFFUL;
    }

    printf("UFS_DISK_PROBE=1 DEVNUM=%lu GEOMETRY=%lu LENGTH=%lu MATCH=%lu "
           "PARTINFO=%lu WIN32=%lu DISK=%lu SIZEMIB=%llu SECTOR=%lu\n",
        (unsigned long)((Flags & UFS_PROBE_F_DEVICE_NUMBER) ? 1 : 0),
        (unsigned long)((Flags & UFS_PROBE_F_GEOMETRY) ? 1 : 0),
        (unsigned long)((Flags & UFS_PROBE_F_LENGTH) ? 1 : 0),
        (unsigned long)((Flags & UFS_PROBE_F_SIZE_MATCHES) ? 1 : 0),
        (unsigned long)((Flags & UFS_PROBE_F_PARTITION_INFO) ? 1 : 0),
        (unsigned long)FirstFail,
        (unsigned long)Number.DeviceNumber,
        (unsigned long long)SizeMib,
        (unsigned long)SectorSize);

    SendVendorNote(UFS_NOTE_REASON_DISK_PROBE,
        (Flags << 24) | (FirstFail & 0xFFFFUL),
        (DWORD)((SizeMib << 16) | SectorSize));
}

/*
 * Is partmgr.sys actually resident in the kernel?
 *
 * This is the one measurement that separates the two surviving explanations
 * for the missing volume, and neither the binaries nor the offline hive can
 * answer it:
 *
 *   partmgr NOT loaded -> PnP never started the driver. The fix is a WIM or
 *                         registry change and no driver work is needed.
 *   partmgr loaded     -> it loaded but did not attach to our disk's devnode,
 *                         so the fault is in AddDevice or in the devnode's
 *                         runtime ClassGUID.
 *
 * The neighbours are reported for context rather than decoration. disk.sys
 * must be present (PhysicalDrive0 exists and reads), so it doubles as a
 * positive control: if disk.sys came back absent the scan itself would be
 * wrong. volmgr/volsnap/mountmgr are the drivers that would build and letter a
 * volume once a partition exists, so their absence would move the problem
 * downstream of partmgr entirely.
 *
 * NtQuerySystemInformation is resolved at runtime rather than linked, so this
 * adds no import and cannot stop the helper loading on an image where the
 * export is missing - it just reports the query as failed.
 */
#define UFS_SYSTEM_MODULE_INFORMATION   11UL
#define UFS_STATUS_INFO_LENGTH_MISMATCH ((LONG)0xC0000004L)

#define UFS_MODULE_F_PARTMGR        0x01UL
#define UFS_MODULE_F_DISK           0x02UL
#define UFS_MODULE_F_CLASSPNP       0x04UL
#define UFS_MODULE_F_VOLMGR         0x08UL
#define UFS_MODULE_F_VOLSNAP        0x10UL
#define UFS_MODULE_F_MOUNTMGR       0x20UL
#define UFS_MODULE_F_STORPORT       0x40UL
#define UFS_MODULE_F_EXYNOSUFS      0x80UL

typedef struct _UFS_RTL_PROCESS_MODULE_INFORMATION {
    PVOID  Section;
    PVOID  MappedBase;
    PVOID  ImageBase;
    ULONG  ImageSize;
    ULONG  Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR  FullPathName[256];
} UFS_RTL_PROCESS_MODULE_INFORMATION;

typedef struct _UFS_RTL_PROCESS_MODULES {
    ULONG NumberOfModules;
    UFS_RTL_PROCESS_MODULE_INFORMATION Modules[1];
} UFS_RTL_PROCESS_MODULES;

typedef LONG (WINAPI *UFS_NT_QUERY_SYSTEM_INFORMATION)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength);

static
DWORD
UfsMatchKernelModule(
    const char *Name
    )
{
    if (_stricmp(Name, "partmgr.sys") == 0)         { return UFS_MODULE_F_PARTMGR; }
    if (_stricmp(Name, "disk.sys") == 0)            { return UFS_MODULE_F_DISK; }
    if (_stricmp(Name, "classpnp.sys") == 0)        { return UFS_MODULE_F_CLASSPNP; }
    if (_stricmp(Name, "volmgr.sys") == 0)          { return UFS_MODULE_F_VOLMGR; }
    if (_stricmp(Name, "volsnap.sys") == 0)         { return UFS_MODULE_F_VOLSNAP; }
    if (_stricmp(Name, "mountmgr.sys") == 0)        { return UFS_MODULE_F_MOUNTMGR; }
    if (_stricmp(Name, "storport.sys") == 0)        { return UFS_MODULE_F_STORPORT; }
    if (_stricmp(Name, "Exynos9820Ufs.sys") == 0)   { return UFS_MODULE_F_EXYNOSUFS; }
    return 0;
}

static
VOID
UfsProbeLoadedModules(
    VOID
    )
{
    UFS_NT_QUERY_SYSTEM_INFORMATION Query;
    UFS_RTL_PROCESS_MODULES *Modules = NULL;
    HMODULE Ntdll;
    ULONG Capacity = 0x20000UL;   /* ~440 modules before a single retry */
    ULONG Needed = 0;
    ULONG Index;
    ULONG Count = 0;
    DWORD Mask = 0;
    DWORD Win32 = 0;
    LONG Status = 0;
    int Attempt;

    Ntdll = GetModuleHandleW(L"ntdll.dll");
    if (Ntdll == NULL) {
        Win32 = GetLastError();
        goto report;
    }

    Query = (UFS_NT_QUERY_SYSTEM_INFORMATION)
        GetProcAddress(Ntdll, "NtQuerySystemInformation");
    if (Query == NULL) {
        Win32 = GetLastError();
        goto report;
    }

    /*
     * Bounded retry rather than a loop on the returned length: the module list
     * can grow between the sizing call and the fetch, and an unbounded retry
     * would be a livelock in a diagnostic that must never hang the boot.
     */
    for (Attempt = 0; Attempt < 4; Attempt++) {
        Modules = (UFS_RTL_PROCESS_MODULES *)malloc(Capacity);
        if (Modules == NULL) {
            Win32 = ERROR_NOT_ENOUGH_MEMORY;
            goto report;
        }

        memset(Modules, 0, Capacity);
        Status = Query(UFS_SYSTEM_MODULE_INFORMATION, Modules, Capacity, &Needed);
        if (Status >= 0) {
            break;
        }

        free(Modules);
        Modules = NULL;

        if (Status != UFS_STATUS_INFO_LENGTH_MISMATCH) {
            break;
        }

        /* Grow past what the kernel asked for, so a small race does not repeat. */
        Capacity = (Needed > Capacity) ? (Needed + 0x4000UL) : (Capacity * 2UL);
    }

    if ((Status < 0) || (Modules == NULL)) {
        Win32 = (DWORD)Status;
        goto report;
    }

    Count = Modules->NumberOfModules;
    for (Index = 0; Index < Count; Index++) {
        const UFS_RTL_PROCESS_MODULE_INFORMATION *Module = &Modules->Modules[Index];
        USHORT Offset = Module->OffsetToFileName;

        /*
         * Guard the offset before using it. It is kernel-supplied and indexes
         * into a fixed 256-byte array; a value at or past the end would walk
         * off the record. The array is also not guaranteed NUL-terminated at
         * the very last byte, so the compare is bounded by forcing one.
         */
        if (Offset < sizeof(Module->FullPathName)) {
            char Name[sizeof(Module->FullPathName)];
            size_t Available = sizeof(Module->FullPathName) - Offset;

            memcpy(Name, Module->FullPathName + Offset, Available);
            Name[Available - 1] = '\0';
            Mask |= UfsMatchKernelModule(Name);
        }
    }

    free(Modules);
    Modules = NULL;

report:

    printf("UFS_MODULES=%lu COUNT=%lu MASK=0x%02lX PARTMGR=%lu DISK=%lu "
           "CLASSPNP=%lu VOLMGR=%lu VOLSNAP=%lu MOUNTMGR=%lu STORPORT=%lu "
           "UFSMINIPORT=%lu WIN32=0x%08lX\n",
        (unsigned long)((Win32 == 0) ? 1 : 0),
        (unsigned long)Count,
        (unsigned long)Mask,
        (unsigned long)((Mask & UFS_MODULE_F_PARTMGR) ? 1 : 0),
        (unsigned long)((Mask & UFS_MODULE_F_DISK) ? 1 : 0),
        (unsigned long)((Mask & UFS_MODULE_F_CLASSPNP) ? 1 : 0),
        (unsigned long)((Mask & UFS_MODULE_F_VOLMGR) ? 1 : 0),
        (unsigned long)((Mask & UFS_MODULE_F_VOLSNAP) ? 1 : 0),
        (unsigned long)((Mask & UFS_MODULE_F_MOUNTMGR) ? 1 : 0),
        (unsigned long)((Mask & UFS_MODULE_F_STORPORT) ? 1 : 0),
        (unsigned long)((Mask & UFS_MODULE_F_EXYNOSUFS) ? 1 : 0),
        (unsigned long)Win32);

    /*
     * Note retired: the answer is settled and stable across runs (mask 0xFF,
     * 129 kernel modules), and the slot it occupied is needed by DISK_OBJDIR.
     * The printf stays, so the value is still on screen and in the log.
     */
}

/*
 * Whether partmgr is attached to this devnode - still an OPEN question.
 *
 * RETRACTION. This comment previously asserted that partmgr is loaded but NOT
 * attached, on the strength of a scan that found neither IOCTL_DISK_
 * GET_DRIVE_LAYOUT_EX (0x00070050) nor STATUS_INVALID_DEVICE_REQUEST
 * (0xC0000010) inside partmgr.sys. That scan is unsound and its negatives mean
 * nothing. The falsifying check is one line: disk.sys unquestionably implements
 * DiskIoctlGetDriveLayoutEx, yet the same scan reported zero hits for
 * 0x00070050 there too.
 *
 * The cause is structural, not a bug in the scan's search. These dispatchers
 * switch on the IOCTL code, which MSVC compiles to a jump table indexed by the
 * function number ((Code >> 2) & 0xFFF), so the full 32-bit CTL_CODE is never
 * stored as a literal and never materialises in a register. Both detectors used
 * - a raw little-endian DWORD byte search and a capstone immediate
 * reconstruction - are blind to that by construction. The sibling claim that
 * partmgr's layout handler "has exactly two error literals" is withdrawn for
 * the same reason.
 *
 * Cheap guard against repeating this: make any scanner find something you
 * already know is present before trusting a single one of its absences.
 *
 * This probe measures the two conditions PnP uses to insert a class upper
 * filter, which remain worth having even though they cannot settle attachment
 * on their own:
 *
 *   the class key's UpperFilters value  - read LIVE, not from the offline hive.
 *                                         Every previous check of this was made
 *                                         against the WIM's SYSTEM hive, which
 *                                         is not proof about the running system.
 *   the devnode's own ClassGUID         - the filter list is looked up by class,
 *                                         so a devnode enumerated under any other
 *                                         class never sees partmgr at all.
 *
 * Attachment itself is settled by UfsProbeDiskObjectDirectory below, which asks
 * the object manager instead of inferring from an error code.
 *
 * The devnode is found by interface (GUID_DEVINTERFACE_DISK) rather than by
 * class. Enumerating the DiskDrive class to discover whether the device is in
 * the DiskDrive class would assume the answer; going in through the interface
 * finds it whatever class it landed in, which is the whole point.
 *
 * setupapi/cfgmgr32 are resolved at runtime for the same reason ntdll is in the
 * module probe: it adds no import, so an image missing either DLL still loads
 * the helper and simply reports the query as unavailable.
 */
#define UFS_STACK_F_CLASS_KEY         0x0001UL
#define UFS_STACK_F_CLASS_UPPER       0x0002UL
#define UFS_STACK_F_CLASS_HAS_PARTMGR 0x0004UL
#define UFS_STACK_F_SERVICE_KEY       0x0008UL
#define UFS_STACK_F_SERVICE_BOOT      0x0010UL
#define UFS_STACK_F_SETUPAPI          0x0020UL
#define UFS_STACK_F_DEVNODE           0x0040UL
#define UFS_STACK_F_CLASSGUID_READ    0x0080UL
#define UFS_STACK_F_CLASSGUID_DISK    0x0100UL
#define UFS_STACK_F_SERVICE_READ      0x0200UL
#define UFS_STACK_F_SERVICE_DISK      0x0400UL
#define UFS_STACK_F_DEV_UPPER         0x0800UL
#define UFS_STACK_F_DEV_HAS_PARTMGR   0x1000UL
#define UFS_STACK_F_STATUS_READ       0x2000UL
#define UFS_STACK_F_HAS_PROBLEM       0x4000UL
#define UFS_STACK_F_MULTIPLE_DISKS    0x8000UL

#define UFS_STACK_START_UNREAD        0xFFUL

typedef HDEVINFO (WINAPI *UFS_SETUPDI_GET_CLASS_DEVS_W)(
    const GUID *, PCWSTR, HWND, DWORD);
typedef BOOL (WINAPI *UFS_SETUPDI_ENUM_INTERFACES)(
    HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI *UFS_SETUPDI_GET_INTERFACE_DETAIL_W)(
    HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W,
    DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *UFS_SETUPDI_GET_PROPERTY_W)(
    HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef BOOL (WINAPI *UFS_SETUPDI_DESTROY_LIST)(HDEVINFO);
typedef CONFIGRET (WINAPI *UFS_CM_GET_DEVNODE_STATUS)(
    PULONG, PULONG, DEVINST, ULONG);

/*
 * Bounded REG_MULTI_SZ membership test.
 *
 * The value is registry-supplied and is not guaranteed to be terminated inside
 * the bytes returned, so the walk is bounded by the reported length and a run
 * that reaches the end without a NUL is discarded rather than compared. That is
 * the same trap the helper's HardwareID check was fixed for: comparing the
 * first entry, or trusting termination, both silently mis-report.
 */
static
BOOL
UfsMultiSzContains(
    const wchar_t *Multi,
    DWORD Bytes,
    const wchar_t *Needle
    )
{
    DWORD Chars = Bytes / (DWORD)sizeof(wchar_t);
    DWORD Index = 0;

    if ((Multi == NULL) || (Chars == 0)) {
        return FALSE;
    }

    while (Index < Chars) {
        DWORD Start = Index;

        while ((Index < Chars) && (Multi[Index] != L'\0')) {
            Index++;
        }

        if (Index == Start) {
            break;      /* empty entry terminates a REG_MULTI_SZ */
        }

        if (Index < Chars) {
            /* Terminated in place by the run above, so this compare is bounded. */
            if (_wcsicmp(&Multi[Start], Needle) == 0) {
                return TRUE;
            }
        }

        Index++;
    }

    return FALSE;
}

static
VOID
UfsProbeDiskStackAttach(
    VOID
    )
{
    static const GUID UfsDiskInterfaceGuid =
        { 0x53f56307, 0xb6bf, 0x11d0,
          { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };
    static const wchar_t UfsDiskDriveClass[] =
        L"{4D36E967-E325-11CE-BFC1-08002BE10318}";

    UFS_SETUPDI_GET_CLASS_DEVS_W GetClassDevs = NULL;
    UFS_SETUPDI_ENUM_INTERFACES EnumInterfaces = NULL;
    UFS_SETUPDI_GET_INTERFACE_DETAIL_W GetDetail = NULL;
    UFS_SETUPDI_GET_PROPERTY_W GetProperty = NULL;
    UFS_SETUPDI_DESTROY_LIST DestroyList = NULL;
    UFS_CM_GET_DEVNODE_STATUS GetDevNodeStatus = NULL;

    HMODULE SetupApi;
    HMODULE CfgMgr;
    HDEVINFO Set = INVALID_HANDLE_VALUE;
    HKEY Key;
    DWORD Flags = 0;
    DWORD Win32 = 0;
    DWORD StartValue = UFS_STACK_START_UNREAD;
    DWORD DevNodes = 0;
    DWORD Status = 0;
    DWORD Problem = 0;
    DWORD Bytes;
    DWORD Type = 0;
    DWORD Index;
    ULONG Aux;
    ULONG Tag;
    wchar_t Multi[512];
    wchar_t ClassGuid[64];
    wchar_t Service[64];

    ClassGuid[0] = L'\0';
    Service[0] = L'\0';

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
            L"{4D36E967-E325-11CE-BFC1-08002BE10318}",
            0, KEY_READ, &Key) == ERROR_SUCCESS) {
        Flags |= UFS_STACK_F_CLASS_KEY;
        memset(Multi, 0, sizeof(Multi));
        Bytes = (DWORD)sizeof(Multi);
        if (RegQueryValueExW(Key, L"UpperFilters", NULL, &Type,
                (LPBYTE)Multi, &Bytes) == ERROR_SUCCESS) {
            Flags |= UFS_STACK_F_CLASS_UPPER;
            if (UfsMultiSzContains(Multi, Bytes, L"partmgr")) {
                Flags |= UFS_STACK_F_CLASS_HAS_PARTMGR;
            }
        }
        RegCloseKey(Key);
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Services\\partmgr",
            0, KEY_READ, &Key) == ERROR_SUCCESS) {
        DWORD Start = 0;

        Flags |= UFS_STACK_F_SERVICE_KEY;
        Bytes = (DWORD)sizeof(Start);
        if (RegQueryValueExW(Key, L"Start", NULL, &Type,
                (LPBYTE)&Start, &Bytes) == ERROR_SUCCESS) {
            StartValue = Start & 0xFFUL;
            if (Start == 0) {
                Flags |= UFS_STACK_F_SERVICE_BOOT;
            }
        }
        RegCloseKey(Key);
    }

    SetupApi = LoadLibraryW(L"setupapi.dll");
    CfgMgr = LoadLibraryW(L"cfgmgr32.dll");

    if (SetupApi != NULL) {
        GetClassDevs = (UFS_SETUPDI_GET_CLASS_DEVS_W)
            GetProcAddress(SetupApi, "SetupDiGetClassDevsW");
        EnumInterfaces = (UFS_SETUPDI_ENUM_INTERFACES)
            GetProcAddress(SetupApi, "SetupDiEnumDeviceInterfaces");
        GetDetail = (UFS_SETUPDI_GET_INTERFACE_DETAIL_W)
            GetProcAddress(SetupApi, "SetupDiGetDeviceInterfaceDetailW");
        GetProperty = (UFS_SETUPDI_GET_PROPERTY_W)
            GetProcAddress(SetupApi, "SetupDiGetDeviceRegistryPropertyW");
        DestroyList = (UFS_SETUPDI_DESTROY_LIST)
            GetProcAddress(SetupApi, "SetupDiDestroyDeviceInfoList");
    }

    if (CfgMgr != NULL) {
        GetDevNodeStatus = (UFS_CM_GET_DEVNODE_STATUS)
            GetProcAddress(CfgMgr, "CM_Get_DevNode_Status");
    }

    if ((GetClassDevs == NULL) || (EnumInterfaces == NULL) ||
        (GetDetail == NULL) || (GetProperty == NULL) || (DestroyList == NULL)) {
        Win32 = GetLastError();
        goto report;
    }

    Flags |= UFS_STACK_F_SETUPAPI;

    Set = GetClassDevs(&UfsDiskInterfaceGuid, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (Set == INVALID_HANDLE_VALUE) {
        Win32 = GetLastError();
        goto report;
    }

    for (Index = 0; ; Index++) {
        SP_DEVICE_INTERFACE_DATA Interface;
        SP_DEVINFO_DATA Node;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *Detail;
        DWORD Needed = 0;

        memset(&Interface, 0, sizeof(Interface));
        Interface.cbSize = (DWORD)sizeof(Interface);

        if (!EnumInterfaces(Set, NULL, &UfsDiskInterfaceGuid, Index, &Interface)) {
            break;
        }

        DevNodes++;
        if (DevNodes > 1) {
            /*
             * Only the first disk is inspected. A second one would mean the
             * whole premise - that PhysicalDrive0 is ours - needs revisiting,
             * so it is flagged rather than silently averaged in.
             */
            Flags |= UFS_STACK_F_MULTIPLE_DISKS;
            continue;
        }

        (void)GetDetail(Set, &Interface, NULL, 0, &Needed, NULL);
        if (Needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        Detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(Needed);
        if (Detail == NULL) {
            continue;
        }

        memset(Detail, 0, Needed);
        Detail->cbSize = (DWORD)sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        memset(&Node, 0, sizeof(Node));
        Node.cbSize = (DWORD)sizeof(Node);

        if (GetDetail(Set, &Interface, Detail, Needed, NULL, &Node)) {
            Flags |= UFS_STACK_F_DEVNODE;

            printf("UFS_DISK_DEVPATH=%ls\n", Detail->DevicePath);

            memset(ClassGuid, 0, sizeof(ClassGuid));
            if (GetProperty(Set, &Node, SPDRP_CLASSGUID, NULL,
                    (PBYTE)ClassGuid, (DWORD)sizeof(ClassGuid) - sizeof(wchar_t),
                    NULL)) {
                Flags |= UFS_STACK_F_CLASSGUID_READ;
                if (_wcsicmp(ClassGuid, UfsDiskDriveClass) == 0) {
                    Flags |= UFS_STACK_F_CLASSGUID_DISK;
                }
            }

            memset(Service, 0, sizeof(Service));
            if (GetProperty(Set, &Node, SPDRP_SERVICE, NULL,
                    (PBYTE)Service, (DWORD)sizeof(Service) - sizeof(wchar_t),
                    NULL)) {
                Flags |= UFS_STACK_F_SERVICE_READ;
                if (_wcsicmp(Service, L"disk") == 0) {
                    Flags |= UFS_STACK_F_SERVICE_DISK;
                }
            }

            memset(Multi, 0, sizeof(Multi));
            Bytes = 0;
            if (GetProperty(Set, &Node, SPDRP_UPPERFILTERS, NULL,
                    (PBYTE)Multi, (DWORD)sizeof(Multi), &Bytes)) {
                Flags |= UFS_STACK_F_DEV_UPPER;
                if (UfsMultiSzContains(Multi, Bytes, L"partmgr")) {
                    Flags |= UFS_STACK_F_DEV_HAS_PARTMGR;
                }
            }

            if (GetDevNodeStatus != NULL) {
                ULONG NodeStatus = 0;
                ULONG NodeProblem = 0;

                if (GetDevNodeStatus(&NodeStatus, &NodeProblem,
                        Node.DevInst, 0) == CR_SUCCESS) {
                    Flags |= UFS_STACK_F_STATUS_READ;
                    Status = NodeStatus;
                    Problem = NodeProblem;
                    if (NodeProblem != 0) {
                        Flags |= UFS_STACK_F_HAS_PROBLEM;
                    }
                }
            }
        }

        free(Detail);
    }

report:

    if ((Set != INVALID_HANDLE_VALUE) && (DestroyList != NULL)) {
        (void)DestroyList(Set);
    }

    if (SetupApi != NULL) {
        FreeLibrary(SetupApi);
    }

    if (CfgMgr != NULL) {
        FreeLibrary(CfgMgr);
    }

    printf("UFS_DISK_STACK=%lu FLAGS=0x%04lX CLASS_UPPER=%lu CLASS_PARTMGR=%lu "
           "SVC_START=%lu DEVNODES=%lu CLASSGUID_DISK=%lu SERVICE_DISK=%lu "
           "DEV_PARTMGR=%lu DN_STATUS=0x%08lX PROBLEM=%lu WIN32=%lu\n",
        (unsigned long)((Flags & UFS_STACK_F_DEVNODE) ? 1 : 0),
        (unsigned long)(Flags & 0xFFFFUL),
        (unsigned long)((Flags & UFS_STACK_F_CLASS_UPPER) ? 1 : 0),
        (unsigned long)((Flags & UFS_STACK_F_CLASS_HAS_PARTMGR) ? 1 : 0),
        (unsigned long)StartValue,
        (unsigned long)DevNodes,
        (unsigned long)((Flags & UFS_STACK_F_CLASSGUID_DISK) ? 1 : 0),
        (unsigned long)((Flags & UFS_STACK_F_SERVICE_DISK) ? 1 : 0),
        (unsigned long)((Flags & UFS_STACK_F_DEV_HAS_PARTMGR) ? 1 : 0),
        (unsigned long)Status,
        (unsigned long)Problem,
        (unsigned long)Win32);

    printf("UFS_DISK_STACK_ID CLASSGUID=%ls SERVICE=%ls\n", ClassGuid, Service);

    /*
     * Aux carries the flag set plus the two counts that would otherwise need a
     * second note, and the note budget is full at 8 slots. Tag carries the
     * devnode status and CM problem code - both of which the 4-byte tag
     * transport is required for, and which the previous run proved works.
     */
    Aux = (ULONG)((Flags & 0xFFFFUL)
        | ((StartValue & 0xFFUL) << 16)
        | (((DevNodes > 0xFFUL) ? 0xFFUL : DevNodes) << 24));
    Tag = (ULONG)(((Status & 0xFFFFUL) << 16) | (Problem & 0xFFFFUL));

    /*
     * Note retired: the previous run answered this outright and every gate came
     * back green - CLASS_HAS_PARTMGR set, partmgr Start=0 (SERVICE_BOOT_START),
     * DN_Status 0x200A, CM_PROB 0. Registration and AddDevice are not the fault,
     * so re-sending the same aux/tag every boot buys nothing and the slot is
     * needed by DISK_IOCTLS. The printfs above still carry the full detail.
     */
    (void)Aux;
    (void)Tag;
}

/*
 * \Device\Harddisk0 through the object manager - the decisive measurement.
 *
 * Two segments of static analysis failed to establish whether partmgr is
 * attached to this devnode, and the scan they rested on has since been
 * retracted (see the comment above UfsProbeDiskStackAttach). The object manager
 * answers it directly and unambiguously, because the two drivers in question
 * create differently-named objects in this directory:
 *
 *   DR0          created by disk.sys   (DiskCreateSymbolicLinks)
 *   Partition0   created by partmgr    - its presence IS attachment
 *   Partition1..N                      - partitions were actually enumerated,
 *                                        which would move the whole
 *                                        investigation up a layer to
 *                                        volmgr/mountmgr
 *
 * Resolved through GetModuleHandleW + GetProcAddress exactly like the
 * NtQuerySystemInformation probe above, so a missing export degrades to a
 * reported flag instead of a load failure. Every failure path still emits the
 * note; a silent probe would be indistinguishable from a passing one.
 */

#define UFS_OBJDIR_F_NTDLL          0x01UL
#define UFS_OBJDIR_F_OPENED         0x02UL
#define UFS_OBJDIR_F_QUERIED        0x04UL
#define UFS_OBJDIR_F_DR0            0x08UL
#define UFS_OBJDIR_F_PARTITION0     0x10UL
#define UFS_OBJDIR_F_PARTITION_N    0x20UL
#define UFS_OBJDIR_F_TRUNCATED      0x40UL

#define UFS_DIRECTORY_QUERY         0x0001UL
#define UFS_OBJ_CASE_INSENSITIVE    0x00000040UL
#define UFS_OBJDIR_MAX_ENTRIES      64UL

typedef struct _UFS_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    wchar_t *Buffer;
} UFS_UNICODE_STRING;

typedef struct _UFS_OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    UFS_UNICODE_STRING *ObjectName;
    ULONG Attributes;
    void *SecurityDescriptor;
    void *SecurityQualityOfService;
} UFS_OBJECT_ATTRIBUTES;

typedef struct _UFS_OBJECT_DIRECTORY_INFORMATION {
    UFS_UNICODE_STRING Name;
    UFS_UNICODE_STRING TypeName;
} UFS_OBJECT_DIRECTORY_INFORMATION;

typedef LONG (WINAPI *UFS_NT_OPEN_DIRECTORY_OBJECT)(
    HANDLE *DirectoryHandle,
    ULONG DesiredAccess,
    UFS_OBJECT_ATTRIBUTES *ObjectAttributes
    );

typedef LONG (WINAPI *UFS_NT_QUERY_DIRECTORY_OBJECT)(
    HANDLE DirectoryHandle,
    void *Buffer,
    ULONG Length,
    BOOLEAN ReturnSingleEntry,
    BOOLEAN RestartScan,
    ULONG *Context,
    ULONG *ReturnLength
    );

/*
 * Bounded, case-insensitive comparison over an explicit length. Written out
 * rather than calling _wcsnicmp so the probe carries no assumption about which
 * CRT declarations are visible, and so the length bound is the caller's rather
 * than a terminator the object manager never promised.
 */
static
BOOL
UfsWideEqualsAscii(
    const wchar_t *Text,
    ULONG Chars,
    const char *Ascii
    )
{
    ULONG Index;

    for (Index = 0; Index < Chars; Index++) {
        wchar_t Left = Text[Index];
        wchar_t Right = (wchar_t)(unsigned char)Ascii[Index];

        if (Ascii[Index] == '\0') {
            return FALSE;
        }
        if ((Left >= L'A') && (Left <= L'Z')) {
            Left = (wchar_t)(Left + (L'a' - L'A'));
        }
        if ((Right >= L'A') && (Right <= L'Z')) {
            Right = (wchar_t)(Right + (L'a' - L'A'));
        }
        if (Left != Right) {
            return FALSE;
        }
    }

    return (Ascii[Chars] == '\0') ? TRUE : FALSE;
}

static
VOID
UfsProbeDiskObjectDirectory(
    VOID
    )
{
    static wchar_t Path[] = L"\\Device\\Harddisk0";

    HMODULE Ntdll;
    UFS_NT_OPEN_DIRECTORY_OBJECT OpenDirectory = NULL;
    UFS_NT_QUERY_DIRECTORY_OBJECT QueryDirectory = NULL;
    UFS_OBJECT_ATTRIBUTES Attributes;
    UFS_UNICODE_STRING Name;
    HANDLE Directory = NULL;
    unsigned char Buffer[1024];
    ULONG Flags = 0;
    ULONG Entries = 0;
    ULONG Highest = 0;
    ULONG Context = 0;
    ULONG Iteration;
    LONG Status = 0;
    ULONG Aux;

    Ntdll = GetModuleHandleW(L"ntdll.dll");
    if (Ntdll != NULL) {
        OpenDirectory = (UFS_NT_OPEN_DIRECTORY_OBJECT)(void *)
            GetProcAddress(Ntdll, "NtOpenDirectoryObject");
        QueryDirectory = (UFS_NT_QUERY_DIRECTORY_OBJECT)(void *)
            GetProcAddress(Ntdll, "NtQueryDirectoryObject");
    }

    if ((OpenDirectory == NULL) || (QueryDirectory == NULL)) {
        printf("UFS_DISK_OBJDIR=0 REASON=NTDLL\n");
        goto report;
    }
    Flags |= UFS_OBJDIR_F_NTDLL;

    Name.Buffer = Path;
    Name.Length = (USHORT)(((sizeof(Path) / sizeof(Path[0])) - 1) * sizeof(wchar_t));
    Name.MaximumLength = (USHORT)sizeof(Path);

    memset(&Attributes, 0, sizeof(Attributes));
    Attributes.Length = (ULONG)sizeof(Attributes);
    Attributes.ObjectName = &Name;
    Attributes.Attributes = UFS_OBJ_CASE_INSENSITIVE;

    Status = OpenDirectory(&Directory, UFS_DIRECTORY_QUERY, &Attributes);
    if (Status < 0) {
        printf("UFS_DISK_OBJDIR=0 REASON=OPEN NTSTATUS=0x%08lX\n",
            (unsigned long)Status);
        goto report;
    }
    Flags |= UFS_OBJDIR_F_OPENED;

    /*
     * One entry per call. The multi-entry form returns a packed array whose
     * termination has to be inferred, and inferring is what this probe exists
     * to stop doing. The iteration bound is reported rather than assumed
     * sufficient - a truncated enumeration is a different answer from a short
     * directory.
     */
    for (Iteration = 0; Iteration < UFS_OBJDIR_MAX_ENTRIES; Iteration++) {
        const UFS_OBJECT_DIRECTORY_INFORMATION *Entry;
        const unsigned char *Text;
        ULONG Returned = 0;
        ULONG Chars;

        memset(Buffer, 0, sizeof(Buffer));

        Status = QueryDirectory(Directory, Buffer, (ULONG)sizeof(Buffer),
            TRUE, (Iteration == 0) ? TRUE : FALSE, &Context, &Returned);
        if (Status < 0) {
            break;
        }

        Entry = (const UFS_OBJECT_DIRECTORY_INFORMATION *)(const void *)Buffer;
        if ((Entry->Name.Buffer == NULL) || (Entry->Name.Length == 0)) {
            break;
        }

        /*
         * The name lives inside our own buffer, after the fixed part. Verify
         * that before dereferencing rather than trusting the returned pointer.
         */
        Text = (const unsigned char *)(const void *)Entry->Name.Buffer;
        if ((Text < Buffer) ||
            ((size_t)(Text - Buffer) + Entry->Name.Length > sizeof(Buffer))) {
            break;
        }

        Flags |= UFS_OBJDIR_F_QUERIED;
        Entries++;
        Chars = (ULONG)(Entry->Name.Length / sizeof(wchar_t));

        printf("UFS_DISK_OBJDIR_ENTRY=%.*ls\n", (int)Chars, Entry->Name.Buffer);

        if (UfsWideEqualsAscii(Entry->Name.Buffer, Chars, "DR0")) {
            Flags |= UFS_OBJDIR_F_DR0;
        } else if ((Chars > 9) &&
                   UfsWideEqualsAscii(Entry->Name.Buffer, 9, "Partition")) {
            ULONG Index = 0;
            ULONG Digit;
            BOOL Numeric = TRUE;

            for (Digit = 9; Digit < Chars; Digit++) {
                wchar_t Ch = Entry->Name.Buffer[Digit];

                if ((Ch < L'0') || (Ch > L'9')) {
                    Numeric = FALSE;
                    break;
                }
                if (Index > 0xFFFFUL) {
                    Numeric = FALSE;
                    break;
                }
                Index = (Index * 10UL) + (ULONG)(Ch - L'0');
            }

            if (Numeric) {
                if (Index == 0) {
                    Flags |= UFS_OBJDIR_F_PARTITION0;
                } else {
                    Flags |= UFS_OBJDIR_F_PARTITION_N;
                    if (Index > Highest) {
                        Highest = Index;
                    }
                }
            }
        }
    }

    if (Iteration >= UFS_OBJDIR_MAX_ENTRIES) {
        Flags |= UFS_OBJDIR_F_TRUNCATED;
    }

    /*
     * STATUS_NO_MORE_ENTRIES is the normal terminator, not a failure, so it must
     * not be reported as the note's tag - that would make a complete
     * enumeration look like a broken one.
     */
    if ((ULONG)Status == 0x8000001AUL) {
        Status = 0;
    }

    printf("UFS_DISK_OBJDIR=1 ENTRIES=%lu DR0=%lu PART0=%lu PARTN=%lu HIGHEST=%lu"
        " TRUNC=%lu NTSTATUS=0x%08lX\n",
        (unsigned long)Entries,
        (unsigned long)((Flags & UFS_OBJDIR_F_DR0) ? 1 : 0),
        (unsigned long)((Flags & UFS_OBJDIR_F_PARTITION0) ? 1 : 0),
        (unsigned long)((Flags & UFS_OBJDIR_F_PARTITION_N) ? 1 : 0),
        (unsigned long)Highest,
        (unsigned long)((Flags & UFS_OBJDIR_F_TRUNCATED) ? 1 : 0),
        (unsigned long)Status);

report:

    if (Directory != NULL) {
        CloseHandle(Directory);
    }

    Aux = (ULONG)((Flags & 0xFFUL)
        | (((Entries > 0xFFUL) ? 0xFFUL : Entries) << 8)
        | (((Highest > 0xFFUL) ? 0xFFUL : Highest) << 16));

    SendVendorNote(UFS_NOTE_REASON_DISK_OBJDIR, Aux, (ULONG)Status);
}

/*
 * Sweep every disk/storage IOCTL that matters, on one handle, in one pass.
 *
 * Slot 0's first-failure code told us a single IOCTL returned 1, which is not
 * enough to localise anything. This returns complete accounting instead: for
 * each of the 14 codes, exactly one of "succeeded", "returned
 * ERROR_INVALID_FUNCTION", or "returned some other error" is recorded, so
 * success|invfunc|other covers every bit and a bit missing from all three would
 * itself be a defect.
 *
 * The legacy-vs-EX pairs are the point. GET_PARTITION_INFO (bit 5) against
 * GET_PARTITION_INFO_EX (bit 6), and GET_DRIVE_LAYOUT (bit 7) against
 * GET_DRIVE_LAYOUT_EX (bit 8): the legacy forms cannot describe a GPT disk, so
 * a split across either pair says something specific about how this disk is
 * being interpreted rather than merely that something failed.
 *
 * Ordering is load-bearing. Every read-only query runs first and
 * UPDATE_PROPERTIES runs last, so the layout results are the pristine ones
 * rather than measurements of a re-read this probe itself provoked.
 *
 * IOCTL_DISK_ARE_VOLUMES_READY is deliberately absent: it can block, and this
 * runs on the boot path.
 */

#define UFS_IOCTL_SWEEP_COUNT       14UL
#define UFS_IOCTL_INPUT_NONE        0UL
#define UFS_IOCTL_INPUT_DEVICE      1UL
#define UFS_IOCTL_INPUT_ALIGNMENT   2UL

typedef struct _UFS_IOCTL_SWEEP_ENTRY {
    DWORD Code;
    ULONG InputKind;
    const char *Name;
} UFS_IOCTL_SWEEP_ENTRY;

static
VOID
UfsProbeDiskIoctlSweep(
    HANDLE Disk
    )
{
    static const UFS_IOCTL_SWEEP_ENTRY Table[UFS_IOCTL_SWEEP_COUNT] = {
        { IOCTL_STORAGE_GET_DEVICE_NUMBER,  UFS_IOCTL_INPUT_NONE,      "STORAGE_GET_DEVICE_NUMBER" },
        { IOCTL_DISK_GET_DRIVE_GEOMETRY,    UFS_IOCTL_INPUT_NONE,      "DISK_GET_DRIVE_GEOMETRY" },
        { IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, UFS_IOCTL_INPUT_NONE,      "DISK_GET_DRIVE_GEOMETRY_EX" },
        { IOCTL_DISK_GET_LENGTH_INFO,       UFS_IOCTL_INPUT_NONE,      "DISK_GET_LENGTH_INFO" },
        { IOCTL_DISK_IS_WRITABLE,           UFS_IOCTL_INPUT_NONE,      "DISK_IS_WRITABLE" },
        { IOCTL_DISK_GET_PARTITION_INFO,    UFS_IOCTL_INPUT_NONE,      "DISK_GET_PARTITION_INFO" },
        { IOCTL_DISK_GET_PARTITION_INFO_EX, UFS_IOCTL_INPUT_NONE,      "DISK_GET_PARTITION_INFO_EX" },
        { IOCTL_DISK_GET_DRIVE_LAYOUT,      UFS_IOCTL_INPUT_NONE,      "DISK_GET_DRIVE_LAYOUT" },
        { IOCTL_DISK_GET_DRIVE_LAYOUT_EX,   UFS_IOCTL_INPUT_NONE,      "DISK_GET_DRIVE_LAYOUT_EX" },
        { IOCTL_STORAGE_QUERY_PROPERTY,     UFS_IOCTL_INPUT_DEVICE,    "STORAGE_QUERY_DEVICE" },
        { IOCTL_STORAGE_QUERY_PROPERTY,     UFS_IOCTL_INPUT_ALIGNMENT, "STORAGE_QUERY_ALIGNMENT" },
        { IOCTL_STORAGE_GET_HOTPLUG_INFO,   UFS_IOCTL_INPUT_NONE,      "STORAGE_GET_HOTPLUG_INFO" },
        { IOCTL_DISK_GET_DISK_ATTRIBUTES,   UFS_IOCTL_INPUT_NONE,      "DISK_GET_DISK_ATTRIBUTES" },
        { IOCTL_DISK_UPDATE_PROPERTIES,     UFS_IOCTL_INPUT_NONE,      "DISK_UPDATE_PROPERTIES" }
    };

    unsigned char Out[4096];
    STORAGE_PROPERTY_QUERY Query;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR Alignment;
    ULONG SuccessMask = 0;
    ULONG InvalidFunctionMask = 0;
    ULONG OtherMask = 0;
    ULONG AlignmentWin32 = (ULONG)ERROR_NOT_SUPPORTED;
    BOOL AlignmentValid = FALSE;
    ULONG Index;
    ULONG Aux;
    ULONG Tag;

    memset(&Alignment, 0, sizeof(Alignment));

    for (Index = 0; Index < UFS_IOCTL_SWEEP_COUNT; Index++) {
        void *Input = NULL;
        DWORD InputBytes = 0;
        DWORD Bytes = 0;
        BOOL Ok;

        memset(Out, 0, sizeof(Out));

        if (Table[Index].InputKind != UFS_IOCTL_INPUT_NONE) {
            memset(&Query, 0, sizeof(Query));
            Query.PropertyId =
                (Table[Index].InputKind == UFS_IOCTL_INPUT_DEVICE)
                    ? StorageDeviceProperty
                    : StorageAccessAlignmentProperty;
            Query.QueryType = PropertyStandardQuery;
            Input = &Query;
            InputBytes = (DWORD)sizeof(Query);
        }

        Ok = DeviceIoControl(Disk, Table[Index].Code, Input, InputBytes,
            Out, (DWORD)sizeof(Out), &Bytes, NULL);

        if (Ok) {
            SuccessMask |= (1UL << Index);
            printf("UFS_IOCTL[%02lu] %-28s OK BYTES=%lu\n",
                (unsigned long)Index, Table[Index].Name,
                (unsigned long)Bytes);

            if ((Table[Index].InputKind == UFS_IOCTL_INPUT_ALIGNMENT) &&
                (Bytes >= (DWORD)sizeof(Alignment))) {
                memcpy(&Alignment, Out, sizeof(Alignment));
                AlignmentValid = TRUE;
                AlignmentWin32 = 0;
            }
        } else {
            DWORD Error = GetLastError();

            if (Error == (DWORD)ERROR_INVALID_FUNCTION) {
                InvalidFunctionMask |= (1UL << Index);
            } else {
                OtherMask |= (1UL << Index);
            }
            if (Table[Index].InputKind == UFS_IOCTL_INPUT_ALIGNMENT) {
                AlignmentWin32 = (ULONG)Error;
            }
            printf("UFS_IOCTL[%02lu] %-28s FAIL WIN32=%lu\n",
                (unsigned long)Index, Table[Index].Name,
                (unsigned long)Error);
        }
    }

    printf("UFS_IOCTL_SWEEP OK=0x%08lX INVFUNC=0x%08lX OTHER=0x%08lX\n",
        (unsigned long)SuccessMask,
        (unsigned long)InvalidFunctionMask,
        (unsigned long)OtherMask);

    SendVendorNote(UFS_NOTE_REASON_DISK_IOCTLS, SuccessMask,
        (ULONG)((InvalidFunctionMask & 0xFFFFUL)
            | ((OtherMask & 0xFFFFUL) << 16)));

    /*
     * 4Kn is the one dimension of this failure never investigated. Both sector
     * sizes are powers of two <= 65536, so each fits a 16-bit half with room to
     * spare; a value that did not fit would be a hardware impossibility rather
     * than a packing bug.
     */
    printf("UFS_DISK_ALIGN=%lu LOGICAL=%lu PHYSICAL=%lu OFFSET=%lu WIN32=%lu\n",
        (unsigned long)(AlignmentValid ? 1 : 0),
        (unsigned long)Alignment.BytesPerLogicalSector,
        (unsigned long)Alignment.BytesPerPhysicalSector,
        (unsigned long)Alignment.BytesOffsetForSectorAlignment,
        (unsigned long)AlignmentWin32);

    Aux = (ULONG)(((Alignment.BytesPerPhysicalSector & 0xFFFFUL) << 16)
        | (Alignment.BytesPerLogicalSector & 0xFFFFUL));
    Tag = (ULONG)(((AlignmentWin32 & 0xFFFFUL) << 16)
        | (Alignment.BytesOffsetForSectorAlignment & 0xFFFFUL));

    SendVendorNote(UFS_NOTE_REASON_DISK_ALIGN, Aux, Tag);
}

/*
 * CRC-32 as GPT uses it: reflected, polynomial 0xEDB88320, initialised to all
 * ones and finally inverted. Written incrementally so the header CRC can be
 * taken over a span with its own stored checksum treated as four zero bytes
 * without copying or mutating the buffer that is still being inspected.
 *
 * Table-free on purpose. The whole probe covers at most 24576 bytes, so the
 * bitwise form costs a couple of hundred thousand iterations once per boot, and
 * a table would be more code to get wrong than the loop it replaces.
 */
static
ULONG
UfsCrc32Update(
    ULONG Crc,
    const unsigned char *Data,
    ULONG Length
    )
{
    ULONG i;
    ULONG j;

    for (i = 0; i < Length; i++) {
        Crc ^= (ULONG)Data[i];
        for (j = 0; j < 8; j++) {
            /*
             * Branch-free reduction: (0 - (Crc & 1)) is all ones when the low
             * bit is set and zero otherwise, so the polynomial is applied
             * exactly on the cycles it should be.
             */
            Crc = (Crc >> 1) ^ (0xEDB88320UL & (0UL - (Crc & 1UL)));
        }
    }

    return Crc;
}

static
ULONG
UfsReadLe32(
    const unsigned char *Data
    )
{
    ULONG Value = 0;

    memcpy(&Value, Data, sizeof(Value));
    return Value;
}

static
ULONGLONG
UfsReadLe64(
    const unsigned char *Data
    )
{
    ULONGLONG Value = 0;

    memcpy(&Value, Data, sizeof(Value));
    return Value;
}

/*
 * Validate the GPT that was just read, and fold the verdict into the caller's
 * flag word.
 *
 * Every bound is taken against Read - the bytes the device actually returned -
 * rather than against the size requested, so a short read that still reported
 * success cannot walk this over untouched pages. That is the same discipline
 * the signature checks use, and it is the reason an earlier all-zero LBA 0 was
 * correctly read as an empty bootstrap area rather than a dead DMA path.
 *
 * Returns the number of non-empty partition entries found.
 */
static
ULONG
UfsProbeValidateGpt(
    const unsigned char *Buffer,
    DWORD Read,
    DWORD *Flags
    )
{
    static const unsigned char Zeros[4] = { 0, 0, 0, 0 };
    const unsigned char *Header;
    const unsigned char *Entries;
    ULONGLONG EntryLba;
    ULONGLONG EntryOffset;
    ULONGLONG ArrayBytes;
    ULONG HeaderSize;
    ULONG StoredHeaderCrc;
    ULONG StoredEntryCrc;
    ULONG EntryCount;
    ULONG EntrySize;
    ULONG NonEmpty = 0;
    ULONG Crc;
    ULONG i;
    ULONG j;

    if (Read < (UFS_SURVEY_EXPECT_SECTOR + UFS_GPT_HEADER_MIN + 4UL)) {
        return 0;
    }

    Header = Buffer + UFS_SURVEY_EXPECT_SECTOR;
    HeaderSize = UfsReadLe32(Header + UFS_GPT_OFF_HEADER_SIZE);
    StoredHeaderCrc = UfsReadLe32(Header + UFS_GPT_OFF_HEADER_CRC);

    /*
     * The 92-byte CRC is computed unconditionally because it is the spec-defined
     * one, and its failure here is the evidence that this header is not
     * spec-shaped rather than an error in the probe.
     */
    Crc = UfsCrc32Update(0xFFFFFFFFUL, Header, UFS_GPT_OFF_HEADER_CRC);
    Crc = UfsCrc32Update(Crc, Zeros, 4);
    Crc = UfsCrc32Update(Crc, Header + UFS_GPT_OFF_HEADER_CRC + 4UL,
        UFS_GPT_HEADER_MIN - (UFS_GPT_OFF_HEADER_CRC + 4UL));
    if ((Crc ^ 0xFFFFFFFFUL) == StoredHeaderCrc) {
        *Flags |= UFS_READ_F_HDR_CRC_92;
    }

    /*
     * The declared-size CRC only runs when the declaration is both sane and
     * fully covered by what was read. A header claiming more than one block
     * would not be reachable from LBA 1 alone, so refusing it is honest rather
     * than conservative.
     */
    if ((HeaderSize >= UFS_GPT_HEADER_MIN) &&
        (HeaderSize <= UFS_SURVEY_EXPECT_SECTOR) &&
        (Read >= (UFS_SURVEY_EXPECT_SECTOR + HeaderSize))) {
        Crc = UfsCrc32Update(0xFFFFFFFFUL, Header, UFS_GPT_OFF_HEADER_CRC);
        Crc = UfsCrc32Update(Crc, Zeros, 4);
        Crc = UfsCrc32Update(Crc, Header + UFS_GPT_OFF_HEADER_CRC + 4UL,
            HeaderSize - (UFS_GPT_OFF_HEADER_CRC + 4UL));
        if ((Crc ^ 0xFFFFFFFFUL) == StoredHeaderCrc) {
            *Flags |= UFS_READ_F_HDR_CRC_DECL;
        }
    }

    EntryLba = UfsReadLe64(Header + UFS_GPT_OFF_ENTRY_LBA);
    EntryCount = UfsReadLe32(Header + UFS_GPT_OFF_ENTRY_COUNT);
    EntrySize = UfsReadLe32(Header + UFS_GPT_OFF_ENTRY_SIZE);
    StoredEntryCrc = UfsReadLe32(Header + UFS_GPT_OFF_ENTRY_CRC);

    if ((EntryCount == 0) || (EntrySize < 128UL) ||
        (EntryCount > UFS_SURVEY_MAX_PARTITIONS)) {
        return 0;
    }

    /*
     * 64-bit throughout, then one bound against Read. Computing the offset in
     * 32 bits would let a hostile or merely corrupt EntryLba wrap into a small
     * value that passes the bound while pointing somewhere else entirely.
     */
    EntryOffset = EntryLba * (ULONGLONG)UFS_SURVEY_EXPECT_SECTOR;
    ArrayBytes = (ULONGLONG)EntryCount * (ULONGLONG)EntrySize;
    if ((EntryOffset + ArrayBytes) > (ULONGLONG)Read) {
        return 0;
    }

    Entries = Buffer + EntryOffset;

    Crc = UfsCrc32Update(0xFFFFFFFFUL, Entries, (ULONG)ArrayBytes);
    if ((Crc ^ 0xFFFFFFFFUL) == StoredEntryCrc) {
        *Flags |= UFS_READ_F_ENTRY_CRC;
    }

    /* An entry is in use when its type GUID is not all zeros. */
    for (i = 0; i < EntryCount; i++) {
        const unsigned char *Entry = Entries + ((ULONGLONG)i * EntrySize);

        for (j = 0; j < 16UL; j++) {
            if (Entry[j] != 0) {
                NonEmpty++;
                break;
            }
        }
    }

    if (NonEmpty != 0) {
        *Flags |= UFS_READ_F_ENTRIES;
    }

    return NonEmpty;
}

/*
 * Read LBA 0 through LBA 5 through Windows' own disk read path.
 *
 * This is the measurement the survey exists for. Every read this project has
 * proven so far went out as a SCSI pass-through, which hands a CDB straight to
 * the miniport and bypasses the entire stack above it. This one goes the other
 * way: ReadFile descends through disk.sys and classpnp and arrives as an
 * ordinary READ(10), which is precisely what partmgr issues when it parses a
 * partition table. If that path is broken, no amount of partition-table
 * correctness can produce a volume.
 *
 * The payload checks itself. "EFI PART" at LBA 1 and the 0x55AA protective-MBR
 * signature at the end of LBA 0 are both already proven present from TWRP, so a
 * correct result cannot come from zeroed or stale memory - the failure mode
 * that made an earlier all-zero LBA 0 read look like a dead DMA path when it
 * was really a legitimate empty bootstrap area. The two CRCs go further: they
 * cannot be satisfied by any buffer this probe could have produced by accident.
 *
 * The buffer comes from VirtualAlloc rather than malloc because a physical
 * drive handle performs unbuffered I/O, which requires the buffer to be aligned
 * to the volume sector size; VirtualAlloc's 64 KiB granularity satisfies that
 * for any sector size this device could report.
 */
static
VOID
UfsProbeDiskRead(
    HANDLE Disk
    )
{
    LARGE_INTEGER Offset;
    unsigned char *Buffer;
    DWORD Flags = 0;
    DWORD Error = 0;
    DWORD Read = 0;
    ULONG NonEmpty = 0;

    Buffer = (unsigned char *)VirtualAlloc(NULL, UFS_PROBE_READ_BYTES,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (Buffer == NULL) {
        /*
         * Captured once. printf can itself set a last-error, so calling
         * GetLastError() again for the note would report whatever the print
         * left behind rather than the allocation failure being described.
         */
        Error = GetLastError();
        printf("UFS_DISK_READ=0 REASON=ALLOC WIN32=%lu\n",
            (unsigned long)Error);
        SendVendorNote(UFS_NOTE_REASON_DISK_READ, Error & 0xFFFFUL, 0);
        return;
    }

    Offset.QuadPart = 0;
    if (SetFilePointerEx(Disk, Offset, NULL, FILE_BEGIN)) {
        Flags |= UFS_READ_F_SEEK;

        if (ReadFile(Disk, Buffer, UFS_PROBE_READ_BYTES, &Read, NULL)) {
            Flags |= UFS_READ_F_READ;

            /*
             * Guarded on the byte count actually returned rather than on the
             * count requested: a short read that still reports success would
             * otherwise have these checks run over untouched pages.
             */
            if ((Read >= 512) &&
                (Buffer[510] == 0x55) && (Buffer[511] == 0xAA)) {
                Flags |= UFS_READ_F_MBR_SIGNATURE;
            }

            if ((Read >= (UFS_SURVEY_EXPECT_SECTOR + 8)) &&
                (memcmp(Buffer + UFS_SURVEY_EXPECT_SECTOR, "EFI PART", 8) == 0)) {
                Flags |= UFS_READ_F_GPT_SIGNATURE;

                NonEmpty = UfsProbeValidateGpt(Buffer, Read, &Flags);
            }
        } else {
            Error = GetLastError();
        }
    } else {
        Error = GetLastError();
    }

    printf("UFS_DISK_READ=%lu SEEK=%lu GPT=%lu MBR=%lu WIN32=%lu BYTES=%lu\n",
        (unsigned long)((Flags & UFS_READ_F_READ) ? 1 : 0),
        (unsigned long)((Flags & UFS_READ_F_SEEK) ? 1 : 0),
        (unsigned long)((Flags & UFS_READ_F_GPT_SIGNATURE) ? 1 : 0),
        (unsigned long)((Flags & UFS_READ_F_MBR_SIGNATURE) ? 1 : 0),
        (unsigned long)Error,
        (unsigned long)Read);

    printf("UFS_GPTCRC HDR92=%lu HDRDECL=%lu ENTRY=%lu ENTRIES=%lu\n",
        (unsigned long)((Flags & UFS_READ_F_HDR_CRC_92) ? 1 : 0),
        (unsigned long)((Flags & UFS_READ_F_HDR_CRC_DECL) ? 1 : 0),
        (unsigned long)((Flags & UFS_READ_F_ENTRY_CRC) ? 1 : 0),
        (unsigned long)NonEmpty);

    /*
     * Bits 24..31 flags, bits 16..23 the non-empty entry count, bits 0..15 the
     * Win32 error. The count is clamped rather than allowed to alias into the
     * flag nibble; 128 entries is the declared maximum, so the clamp is
     * unreachable in practice and exists only so a corrupt header cannot
     * rewrite the verdict bits.
     */
    if (NonEmpty > 0xFFUL) {
        NonEmpty = 0xFFUL;
    }

    SendVendorNote(UFS_NOTE_REASON_DISK_READ,
        (Flags << 24) | (NonEmpty << 16) | (Error & 0xFFFFUL), Read);

    VirtualFree(Buffer, 0, MEM_RELEASE);
}

/*
 * Survey the disk exactly as Windows sees it, immediately before the fs-write
 * tries to use it.
 *
 * The first fs-write attempt returned FSWRITE_NO_VOLUME with
 * GetLogicalDrives() = 0x00800000, i.e. X: and nothing else, and the survey's
 * first version then returned DISK_LAYOUT_FAIL with ERROR_INVALID_FUNCTION.
 *
 * That second result rules out most of what it looks like it implicates. The
 * same boot completed 113 SCSI commands with no containment and no fatal bits,
 * and SendVendorNote delivers every one of these notes by issuing a vendor CDB
 * to \\.\PhysicalDrive0 with no fallback and no port scan - so the notes
 * arriving at all proves PhysicalDrive0 is this driver's disk and that its
 * command path works. The break is above us, in the disk device stack.
 *
 * So the survey now locates the break instead of only reporting its symptom.
 * The IOCTLs below are owned by different drivers, and the first one to fail
 * names the missing layer:
 *
 *   GET_DEVICE_NUMBER fails     -> classpnp/disk.sys is not really servicing us
 *   GEOMETRY/LENGTH fail        -> disk.sys answers no disk-class request
 *   only GET_DRIVE_LAYOUT_EX    -> partmgr never attached or refuses this disk,
 *     fails                        the remaining suspects being the 4096-byte
 *                                  logical sector size and PnP attachment
 *   ReadFile fails              -> Windows' own read path cannot reach the media
 *                                  even though pass-through can
 *
 * ReadFile is the highest-value of these: it is exactly what partmgr does to
 * parse a partition table, it traverses the whole stack down into this driver
 * rather than bypassing it the way pass-through does, and its payload is
 * self-checking - "EFI PART" at LBA 1 is already proven present from TWRP, so a
 * correct read cannot be faked by zeroed memory.
 *
 * The rescan has also been moved out of the layout-success branch. In v1 the
 * one remedy the survey implements was nested inside the success path and gated
 * on PartitionCount == 0, which made it unreachable on the branch that actually
 * fired. It is now attempted whenever the layout is missing for either reason.
 *
 * The survey is read-only with respect to the media. SetVolumeMountPointW
 * writes a mount-point registry entry, not a disk sector; the disk handle is
 * opened write-capable only so IOCTL_DISK_UPDATE_PROPERTIES will accept it; and
 * the driver's CDB allowlist still refuses every data-out command regardless of
 * how the caller opened the disk.
 */
static
VOID
UfsDiskSurvey(
    VOID
    )
{
    HANDLE Disk;
    HANDLE Find;
    wchar_t Volume[MAX_PATH];
    DWORD Volumes = 0;
    DWORD Lettered = 0;
    DWORD Mounted = 0;
    BOOL MountFailNoted = FALSE;

    /*
     * Write-capable, because IOCTL_DISK_UPDATE_PROPERTIES is not reliably
     * accepted on a read-only handle and the rescan now runs on the failure
     * path where it is most needed. Falls back to read-only so a refused
     * reopen degrades to v1's behaviour rather than losing the survey.
     */
    Disk = CreateFileW(L"\\\\.\\PhysicalDrive0", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Disk == INVALID_HANDLE_VALUE) {
        Disk = CreateFileW(L"\\\\.\\PhysicalDrive0", GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }

    if (Disk == INVALID_HANDLE_VALUE) {
        DWORD Error = GetLastError();
        printf("UFS_DISK_LAYOUT=0 REASON=OPEN WIN32=%lu\n", (unsigned long)Error);
        SendVendorNote(UFS_NOTE_REASON_DISK_OPEN_FAIL, Error, 0);
    } else {
        unsigned char *Layout = (unsigned char *)malloc(UFS_SURVEY_LAYOUT_BYTES);
        BOOL LayoutKnown = FALSE;
        DWORD LayoutCount = 0;

        UfsProbeDiskStack(Disk);
        UfsProbeDiskRead(Disk);
        UfsProbeLoadedModules();
        UfsProbeDiskStackAttach();

        /*
         * Emission order determines slot assignment, and the budget is exactly
         * 8. objdir -> ioctls -> align lands them in slots 2/3/4, ahead of the
         * layout/rescan pair and the settled FSWRITE_NO_VOLUME answer that has
         * to stay in the last slot.
         *
         * The sweep runs before the layout attempt below on purpose: it ends
         * with UPDATE_PROPERTIES, so running it after would make the layout
         * result a measurement of a re-read this probe provoked rather than of
         * the state Windows actually booted into.
         */
        UfsProbeDiskObjectDirectory();
        UfsProbeDiskIoctlSweep(Disk);

        if (Layout != NULL) {
            DWORD Bytes = 0;

            /*
             * Zeroed so a short or failed reply can never be read back as
             * uninitialised heap; every field examined below is then either
             * what partmgr wrote or a definite zero.
             */
            memset(Layout, 0, UFS_SURVEY_LAYOUT_BYTES);

            if (DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0,
                    Layout, UFS_SURVEY_LAYOUT_BYTES, &Bytes, NULL)) {
                DRIVE_LAYOUT_INFORMATION_EX *Info =
                    (DRIVE_LAYOUT_INFORMATION_EX *)Layout;
                DWORD Style = (DWORD)Info->PartitionStyle;
                DWORD Count = Info->PartitionCount;

                printf("UFS_DISK_LAYOUT=1 STYLE=%lu COUNT=%lu\n",
                    (unsigned long)Style, (unsigned long)Count);
                SendVendorNote(UFS_NOTE_REASON_DISK_LAYOUT, Count, Style);

                LayoutKnown = TRUE;
                LayoutCount = Count;
            } else {
                DWORD Error = GetLastError();
                printf("UFS_DISK_LAYOUT=0 REASON=IOCTL WIN32=%lu\n",
                    (unsigned long)Error);
                SendVendorNote(UFS_NOTE_REASON_DISK_LAYOUT_FAIL, Error, 0);
            }

            /*
             * Both outcomes worth acting on are "Windows has no partitions for
             * this disk": the layout call failed outright, or it succeeded and
             * reported none. Both are consistent with partmgr having read the
             * disk before the driver could answer, which a rescan repairs.
             *
             * In v1 this block lived inside the success branch and so could
             * never run on the branch that actually fired. A layout that
             * reports partitions still costs no note slot.
             */
            if (!LayoutKnown || (LayoutCount == 0)) {
                DWORD Rc = 0;

                if (!DeviceIoControl(Disk, IOCTL_DISK_UPDATE_PROPERTIES,
                        NULL, 0, NULL, 0, &Bytes, NULL)) {
                    Rc = GetLastError();
                }

                LayoutCount = 0;
                memset(Layout, 0, UFS_SURVEY_LAYOUT_BYTES);
                if (DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                        NULL, 0, Layout, UFS_SURVEY_LAYOUT_BYTES, &Bytes,
                        NULL)) {
                    LayoutCount =
                        ((DRIVE_LAYOUT_INFORMATION_EX *)Layout)->PartitionCount;
                }

                printf("UFS_DISK_RESCAN=1 WIN32=%lu COUNT=%lu\n",
                    (unsigned long)Rc, (unsigned long)LayoutCount);
                SendVendorNote(UFS_NOTE_REASON_DISK_RESCAN, LayoutCount, Rc);
            }

            free(Layout);
        }

        CloseHandle(Disk);
    }

    /*
     * Enumerate volume objects directly. GetLogicalDrives() only reports
     * volumes that already carry a letter, so it cannot see the case this
     * survey exists to catch - a volume device that exists but was never
     * assigned one, which is exactly what happens when a disk arrives after
     * WinPE's letter-assignment window.
     */
    Find = FindFirstVolumeW(Volume, MAX_PATH);
    if (Find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t Names[512];
            DWORD Length = 0;

            Volumes++;
            Names[0] = 0;

            if (GetVolumePathNamesForVolumeNameW(Volume, Names,
                    (DWORD)(sizeof(Names) / sizeof(Names[0])), &Length) &&
                (Names[0] != 0)) {
                Lettered++;
                continue;
            }

            printf("UFS_VOLUME_NOPATH=%ls\n", Volume);

            /*
             * Mount the first letterless volume and keep it only if it is the
             * one we want. Identifying it by content rather than by ordinal is
             * the same rule UfsFsWriteFindVolume follows, and it is what makes
             * the mount safe to attempt blind: a volume that is not ours is
             * unmounted again immediately.
             */
            if (Mounted != 0) {
                continue;
            }

            if (SetVolumeMountPointW(UFS_SURVEY_MOUNT, Volume)) {
                wchar_t Wim[] = UFS_SURVEY_MOUNT L"sources\\boot.wim";

                if (GetFileAttributesW(Wim) != INVALID_FILE_ATTRIBUTES) {
                    Mounted = (DWORD)UFS_SURVEY_MOUNT_LETTER;
                    printf("UFS_VOLUME_MOUNTED=%lc\n", UFS_SURVEY_MOUNT_LETTER);
                    SendVendorNote(UFS_NOTE_REASON_VOLUME_MOUNTED, Mounted,
                        Volumes);
                } else {
                    DeleteVolumeMountPointW(UFS_SURVEY_MOUNT);
                }
            } else if (!MountFailNoted) {
                DWORD Error = GetLastError();

                /*
                 * Bounded to the first failure. The loop is unbounded in
                 * principle and the note slots are not, so an unlucky disk
                 * could otherwise overwrite the fs-write outcome - the one
                 * record that must survive.
                 */
                MountFailNoted = TRUE;
                printf("UFS_VOLUME_MOUNT=0 WIN32=%lu\n", (unsigned long)Error);
                SendVendorNote(UFS_NOTE_REASON_VOLUME_MOUNT_FAIL, Error,
                    Volumes);
            }
        } while (FindNextVolumeW(Find, Volume, MAX_PATH));

        FindVolumeClose(Find);
    }

    printf("UFS_VOLUMES=%lu LETTERED=%lu MOUNTED=%lu\n",
        (unsigned long)Volumes, (unsigned long)Lettered,
        (unsigned long)Mounted);
    /*
     * Note retired: settled and invariant - one volume, one lettered (X:, the
     * WinPE ramdisk). FSWRITE_NO_VOLUME in the last slot already carries the
     * same answer as a drive mask, and that slot is the one UfsPramNote clamps
     * to, so it always survives. This slot goes to DISK_ALIGN instead.
     */
}

static
BOOL
UfsFsWriteFindVolume(
    wchar_t *LetterOut
    )
{
    DWORD Mask = GetLogicalDrives();
    wchar_t Letter;

    for (Letter = L'A'; Letter <= L'Z'; Letter++) {
        wchar_t Marker[] = L"?:\\ufsscratch.bin";
        wchar_t Wim[] = L"?:\\sources\\boot.wim";

        //
        // X: is the WinPE RAM disk. It always exists and is never the UFS
        // volume, so testing it can only produce a false positive.
        //
        if (Letter == L'X') {
            continue;
        }

        if ((Mask & (1UL << (Letter - L'A'))) == 0) {
            continue;
        }

        Marker[0] = Letter;
        Wim[0] = Letter;

        if ((GetFileAttributesW(Marker) != INVALID_FILE_ATTRIBUTES) &&
            (GetFileAttributesW(Wim) != INVALID_FILE_ATTRIBUTES)) {
            *LetterOut = Letter;
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOL
PersistFilesystemResult(
    const wchar_t *Path,
    const char *Line,
    DWORD Length,
    DWORD CreationDisposition,
    BOOL RequireExistingAllocation,
    DWORD *ErrorOut,
    ULONG *NoteReasonOut
    )
{
    HANDLE File;
    DWORD Bytes = 0;
    FILE_STANDARD_INFO StandardInfo;
    LARGE_INTEGER Origin;
    BOOL WriteOk;

    *ErrorOut = ERROR_SUCCESS;
    *NoteReasonOut = UFS_NOTE_REASON_FSWRITE_CREATE;
    Origin.QuadPart = 0;

    File = CreateFileW(
        Path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CreationDisposition,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL
        );
    if (File == INVALID_HANDLE_VALUE) {
        *ErrorOut = GetLastError();
        return FALSE;
    }

    if (RequireExistingAllocation) {
        ZeroMemory(&StandardInfo, sizeof(StandardInfo));
        if (!GetFileInformationByHandleEx(
                File,
                FileStandardInfo,
                &StandardInfo,
                sizeof(StandardInfo))) {
            *ErrorOut = GetLastError();
            *NoteReasonOut = UFS_NOTE_REASON_FSWRITE_WRITE;
            CloseHandle(File);
            return FALSE;
        }
        if ((StandardInfo.AllocationSize.QuadPart < 0) ||
            ((ULONGLONG)Length > (ULONGLONG)StandardInfo.AllocationSize.QuadPart)) {
            *ErrorOut = ERROR_DISK_FULL;
            *NoteReasonOut = UFS_NOTE_REASON_FSWRITE_WRITE;
            CloseHandle(File);
            return FALSE;
        }
    }

    if (!SetFilePointerEx(File, Origin, NULL, FILE_BEGIN)) {
        *ErrorOut = GetLastError();
        *NoteReasonOut = UFS_NOTE_REASON_FSWRITE_WRITE;
        CloseHandle(File);
        return FALSE;
    }

    WriteOk = WriteFile(File, Line, Length, &Bytes, NULL);
    if (!WriteOk || (Bytes != Length)) {
        *ErrorOut = WriteOk ? ERROR_WRITE_FAULT : GetLastError();
        *NoteReasonOut = UFS_NOTE_REASON_FSWRITE_WRITE;
        CloseHandle(File);
        return FALSE;
    }

    if (!SetEndOfFile(File)) {
        *ErrorOut = GetLastError();
        *NoteReasonOut = UFS_NOTE_REASON_FSWRITE_WRITE;
        CloseHandle(File);
        return FALSE;
    }

    if (!FlushFileBuffers(File)) {
        *ErrorOut = GetLastError();
        *NoteReasonOut = UFS_NOTE_REASON_FSWRITE_FLUSH;
        CloseHandle(File);
        return FALSE;
    }

    if (!CloseHandle(File)) {
        *ErrorOut = GetLastError();
        *NoteReasonOut = UFS_NOTE_REASON_FSWRITE_FLUSH;
        return FALSE;
    }

    return TRUE;
}

static
BOOL
CopyFilePreservingAllocation(
    const wchar_t *SourcePath,
    const wchar_t *DestinationPath,
    DWORD *CopiedBytes,
    DWORD *ErrorOut
    )
{
    BYTE Buffer[65536];
    HANDLE Destination;
    HANDLE Source;
    FILE_STANDARD_INFO DestinationInfo;
    FILE_STANDARD_INFO SourceInfo;
    LARGE_INTEGER Origin;
    DWORD ReadBytes;
    DWORD TotalBytes;
    DWORD WrittenBytes;
    BOOL Ok;

    if ((SourcePath == NULL) ||
        (DestinationPath == NULL) ||
        (CopiedBytes == NULL) ||
        (ErrorOut == NULL)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *CopiedBytes = 0;
    *ErrorOut = ERROR_SUCCESS;
    Origin.QuadPart = 0;
    TotalBytes = 0;
    Source = CreateFileW(
        SourcePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        NULL
        );
    if (Source == INVALID_HANDLE_VALUE) {
        *ErrorOut = GetLastError();
        return FALSE;
    }

    Destination = CreateFileW(
        DestinationPath,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL
        );
    if (Destination == INVALID_HANDLE_VALUE) {
        *ErrorOut = GetLastError();
        CloseHandle(Source);
        return FALSE;
    }

    ZeroMemory(&SourceInfo, sizeof(SourceInfo));
    ZeroMemory(&DestinationInfo, sizeof(DestinationInfo));
    if (!GetFileInformationByHandleEx(
            Source,
            FileStandardInfo,
            &SourceInfo,
            sizeof(SourceInfo)) ||
        !GetFileInformationByHandleEx(
            Destination,
            FileStandardInfo,
            &DestinationInfo,
            sizeof(DestinationInfo))) {
        *ErrorOut = GetLastError();
        CloseHandle(Destination);
        CloseHandle(Source);
        return FALSE;
    }
    if ((SourceInfo.EndOfFile.QuadPart < 0) ||
        (SourceInfo.EndOfFile.QuadPart > MAXDWORD) ||
        (DestinationInfo.AllocationSize.QuadPart < 0) ||
        ((ULONGLONG)SourceInfo.EndOfFile.QuadPart >
            (ULONGLONG)DestinationInfo.AllocationSize.QuadPart)) {
        *ErrorOut = ERROR_DISK_FULL;
        CloseHandle(Destination);
        CloseHandle(Source);
        return FALSE;
    }

    Ok = SetFilePointerEx(Destination, Origin, NULL, FILE_BEGIN);
    if (!Ok) {
        *ErrorOut = GetLastError();
    }

    while (Ok) {
        if (!ReadFile(Source, Buffer, sizeof(Buffer), &ReadBytes, NULL)) {
            *ErrorOut = GetLastError();
            Ok = FALSE;
            break;
        }
        if (ReadBytes == 0) {
            break;
        }
        if (MAXDWORD - TotalBytes < ReadBytes) {
            *ErrorOut = ERROR_ARITHMETIC_OVERFLOW;
            Ok = FALSE;
            break;
        }
        if (!WriteFile(Destination, Buffer, ReadBytes, &WrittenBytes, NULL)) {
            *ErrorOut = GetLastError();
            Ok = FALSE;
            break;
        }
        if (WrittenBytes != ReadBytes) {
            *ErrorOut = ERROR_WRITE_FAULT;
            Ok = FALSE;
            break;
        }
        TotalBytes += ReadBytes;
    }

    if (Ok && ((ULONGLONG)TotalBytes != (ULONGLONG)SourceInfo.EndOfFile.QuadPart)) {
        *ErrorOut = ERROR_HANDLE_EOF;
        Ok = FALSE;
    }
    if (Ok && !SetEndOfFile(Destination)) {
        *ErrorOut = GetLastError();
        Ok = FALSE;
    }
    if (Ok && !FlushFileBuffers(Destination)) {
        *ErrorOut = GetLastError();
        Ok = FALSE;
    }
    if (!CloseHandle(Destination) && Ok) {
        *ErrorOut = GetLastError();
        Ok = FALSE;
    }
    CloseHandle(Source);

    if (Ok) {
        *CopiedBytes = TotalBytes;
    }
    return Ok;
}

/*
 * One-boot filesystem-semantics probe.
 *
 * Every row is persisted and flushed before the next operation starts. The
 * before/after counters are absolute snapshots because the diagnostic queries
 * themselves can advance command counters. All disposable objects are confined
 * to the mounted WINSETUP volume and are removed before volume teardown.
 */
#define UFS_FSSEM_MATRIX_CAPACITY       32768UL
#define UFS_FSSEM_ROW_COUNT                 8UL
#define UFS_FSSEM_SCRATCH_SIZE        1048576ULL
#define UFS_FSSEM_PREPARED_SIZE          8192UL
#define UFS_FSSEM_RENAMED_SIZE           4096UL
#define UFS_FSSEM_TRUNCATED_SIZE         4096UL

#define UFS_FSSEM_STAGE_NONE                 0UL
#define UFS_FSSEM_STAGE_PREPARE              1UL
#define UFS_FSSEM_STAGE_OPEN                 2UL
#define UFS_FSSEM_STAGE_OPERATION            3UL
#define UFS_FSSEM_STAGE_FLUSH                4UL
#define UFS_FSSEM_STAGE_CLOSE                5UL
#define UFS_FSSEM_STAGE_VERIFY               6UL
#define UFS_FSSEM_STAGE_CLEANUP              7UL
#define UFS_FSSEM_STAGE_COUNTER              8UL
#define UFS_FSSEM_STAGE_PERSIST              9UL

#define UFS_FSSEM_CLOSE_ATTEMPTED        0x100UL
#define UFS_FSSEM_CLOSE_SUCCEEDED        0x200UL

typedef enum _UFS_FSSEM_OPERATION {
    UfsFsSemScratchMetadata = 0,
    UfsFsSemDeletePath,
    UfsFsSemDisposition,
    UfsFsSemRename,
    UfsFsSemDeleteOnClose,
    UfsFsSemTruncate,
    UfsFsSemDirectoryCreate,
    UfsFsSemDirectoryRemove
} UFS_FSSEM_OPERATION;

typedef struct _UFS_FSSEM_COUNTERS {
    BOOL Valid;
    ULONG WritesIssued;
    ULONG WritesVerified;
    ULONG FailedCommands;
    ULONG CompletedCommands;
    ULONG StartIoRequests;
    ULONG RearmAttempts;
    ULONG RearmSuccesses;
    ULONG ContainedCommands;
    ULONG FatalError;
} UFS_FSSEM_COUNTERS;

typedef struct _UFS_FSSEM_PATH_STATE {
    BOOL QueryOk;
    BOOL Exists;
    DWORD Error;
    DWORD Attributes;
    ULONGLONG Size;
} UFS_FSSEM_PATH_STATE;

typedef struct _UFS_FSSEM_ROW {
    ULONG Index;
    const char *Name;
    BOOL Attempted;
    BOOL OperationOk;
    BOOL VerifyOk;
    BOOL CleanupOk;
    ULONG FailureStage;
    DWORD OperationError;
    DWORD VerifyError;
    DWORD CleanupError;
    UFS_FSSEM_COUNTERS Before;
    UFS_FSSEM_COUNTERS After;
    UFS_FSSEM_PATH_STATE Source;
    UFS_FSSEM_PATH_STATE Destination;
} UFS_FSSEM_ROW;

typedef struct _UFS_FSSEM_CLOSURE_STAGE {
    BOOL Attempted;
    BOOL Succeeded;
    DWORD Error;
} UFS_FSSEM_CLOSURE_STAGE;

static
BOOL
UfsFsSemAppend(
    char *Buffer,
    ULONG Capacity,
    ULONG *Length,
    const char *Format,
    ...
    )
{
    va_list Arguments;
    int Added;

    if (*Length >= Capacity) {
        return FALSE;
    }

    va_start(Arguments, Format);
    Added = vsprintf_s(
        Buffer + *Length,
        (size_t)(Capacity - *Length),
        Format,
        Arguments
        );
    va_end(Arguments);

    if (Added < 0) {
        return FALSE;
    }

    *Length += (ULONG)Added;
    return TRUE;
}

static
BOOL
UfsFsSemCaptureCounters(
    UFS_FSSEM_COUNTERS *Counters
    )
{
    UFS_DIAGNOSTIC_DATA Diagnostic;

    ZeroMemory(Counters, sizeof(*Counters));
    ZeroMemory(&Diagnostic, sizeof(Diagnostic));
    if (!RunVendorDiagSnapshot(&Diagnostic)) {
        return FALSE;
    }

    Counters->Valid = TRUE;
    Counters->WritesIssued = Diagnostic.WritesIssued;
    Counters->WritesVerified = Diagnostic.WritesVerified;
    Counters->FailedCommands = Diagnostic.FailedCommands;
    Counters->CompletedCommands = Diagnostic.CompletedCommands;
    Counters->StartIoRequests = Diagnostic.StartIoRequests;
    Counters->RearmAttempts = Diagnostic.RearmAttempts;
    Counters->RearmSuccesses = Diagnostic.RearmSuccesses;
    Counters->ContainedCommands = Diagnostic.ContainedCommands;
    Counters->FatalError = Diagnostic.FatalError ? 1UL : 0UL;
    return TRUE;
}

static
VOID
UfsFsSemQueryPath(
    const wchar_t *Path,
    UFS_FSSEM_PATH_STATE *State
    )
{
    WIN32_FILE_ATTRIBUTE_DATA Data;
    DWORD Error;

    ZeroMemory(State, sizeof(*State));
    if (GetFileAttributesExW(Path, GetFileExInfoStandard, &Data)) {
        State->QueryOk = TRUE;
        State->Exists = TRUE;
        State->Attributes = Data.dwFileAttributes;
        State->Size =
            ((ULONGLONG)Data.nFileSizeHigh << 32) |
            (ULONGLONG)Data.nFileSizeLow;
        return;
    }

    Error = GetLastError();
    State->Error = Error;
    if ((Error == ERROR_FILE_NOT_FOUND) || (Error == ERROR_PATH_NOT_FOUND)) {
        State->QueryOk = TRUE;
        State->Exists = FALSE;
        State->Error = ERROR_SUCCESS;
    }
}

static
BOOL
UfsFsSemDeleteFileIfPresent(
    const wchar_t *Path,
    DWORD *ErrorOut
    )
{
    DWORD Attributes;
    DWORD Error;

    *ErrorOut = ERROR_SUCCESS;
    Attributes = GetFileAttributesW(Path);
    if (Attributes == INVALID_FILE_ATTRIBUTES) {
        Error = GetLastError();
        if ((Error == ERROR_FILE_NOT_FOUND) || (Error == ERROR_PATH_NOT_FOUND)) {
            return TRUE;
        }
        *ErrorOut = Error;
        return FALSE;
    }

    if ((Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        *ErrorOut = ERROR_DIRECTORY;
        return FALSE;
    }

    if (!DeleteFileW(Path)) {
        *ErrorOut = GetLastError();
        return FALSE;
    }

    return TRUE;
}

static
BOOL
UfsFsSemRemoveDirectoryIfPresent(
    const wchar_t *Path,
    DWORD *ErrorOut
    )
{
    DWORD Attributes;
    DWORD Error;

    *ErrorOut = ERROR_SUCCESS;
    Attributes = GetFileAttributesW(Path);
    if (Attributes == INVALID_FILE_ATTRIBUTES) {
        Error = GetLastError();
        if ((Error == ERROR_FILE_NOT_FOUND) || (Error == ERROR_PATH_NOT_FOUND)) {
            return TRUE;
        }
        *ErrorOut = Error;
        return FALSE;
    }

    if ((Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        *ErrorOut = ERROR_DIRECTORY;
        return FALSE;
    }

    if (!RemoveDirectoryW(Path)) {
        *ErrorOut = GetLastError();
        return FALSE;
    }

    return TRUE;
}

static
BOOL
UfsFsSemWritePreparedFile(
    const wchar_t *Path,
    ULONG Length,
    UCHAR Seed,
    DWORD *ErrorOut
    )
{
    HANDLE File;
    UCHAR *Buffer;
    ULONG Index;
    DWORD Written = 0;
    BOOL WriteOk;
    BOOL Ok = FALSE;

    *ErrorOut = ERROR_SUCCESS;
    Buffer = (UCHAR *)malloc(Length);
    if (Buffer == NULL) {
        *ErrorOut = ERROR_NOT_ENOUGH_MEMORY;
        return FALSE;
    }

    for (Index = 0; Index < Length; Index++) {
        Buffer[Index] = (UCHAR)(Seed + (UCHAR)Index);
    }

    File = CreateFileW(
        Path,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL
        );
    if (File == INVALID_HANDLE_VALUE) {
        *ErrorOut = GetLastError();
        free(Buffer);
        return FALSE;
    }

    WriteOk = WriteFile(File, Buffer, Length, &Written, NULL);
    if (!WriteOk || (Written != Length)) {
        *ErrorOut = WriteOk ? ERROR_WRITE_FAULT : GetLastError();
        goto Exit;
    }

    if (!FlushFileBuffers(File)) {
        *ErrorOut = GetLastError();
        goto Exit;
    }

    Ok = TRUE;

Exit:
    if (!CloseHandle(File) && Ok) {
        *ErrorOut = GetLastError();
        Ok = FALSE;
    }
    free(Buffer);
    return Ok;
}

static
BOOL
UfsFsSemPrepareRow(
    UFS_FSSEM_OPERATION Operation,
    const wchar_t *Source,
    const wchar_t *Destination,
    DWORD *ErrorOut
    )
{
    DWORD Error = ERROR_SUCCESS;

    *ErrorOut = ERROR_SUCCESS;
    switch (Operation) {
    case UfsFsSemScratchMetadata:
        return TRUE;

    case UfsFsSemDeletePath:
    case UfsFsSemDisposition:
    case UfsFsSemDeleteOnClose:
    case UfsFsSemTruncate:
        if (!UfsFsSemDeleteFileIfPresent(Source, &Error)) {
            *ErrorOut = Error;
            return FALSE;
        }
        if ((Operation == UfsFsSemDeleteOnClose) ||
            (Operation == UfsFsSemTruncate)) {
            if (Operation == UfsFsSemDeleteOnClose) {
                return TRUE;
            }
            return UfsFsSemWritePreparedFile(
                Source,
                UFS_FSSEM_PREPARED_SIZE,
                0x60U,
                ErrorOut
                );
        }
        return UfsFsSemWritePreparedFile(
            Source,
            UFS_FSSEM_RENAMED_SIZE,
            (Operation == UfsFsSemDeletePath) ? 0x20U : 0x40U,
            ErrorOut
            );

    case UfsFsSemRename:
        if (!UfsFsSemDeleteFileIfPresent(Source, &Error) ||
            !UfsFsSemDeleteFileIfPresent(Destination, &Error)) {
            *ErrorOut = Error;
            return FALSE;
        }
        return UfsFsSemWritePreparedFile(
            Source,
            UFS_FSSEM_RENAMED_SIZE,
            0x50U,
            ErrorOut
            );

    case UfsFsSemDirectoryCreate:
        return UfsFsSemRemoveDirectoryIfPresent(Source, ErrorOut);

    case UfsFsSemDirectoryRemove:
        if (!UfsFsSemRemoveDirectoryIfPresent(Source, ErrorOut)) {
            return FALSE;
        }
        if (!CreateDirectoryW(Source, NULL)) {
            *ErrorOut = GetLastError();
            return FALSE;
        }
        return TRUE;
    }

    *ErrorOut = ERROR_INVALID_FUNCTION;
    return FALSE;
}

static
VOID
UfsFsSemPerformRow(
    UFS_FSSEM_OPERATION Operation,
    const wchar_t *Source,
    const wchar_t *Destination,
    UFS_FSSEM_ROW *Row
    )
{
    HANDLE File;
    DWORD Written = 0;
    DWORD Error;
    BOOL WriteOk;

    Row->Attempted = TRUE;
    Row->OperationError = ERROR_SUCCESS;

    switch (Operation) {
    case UfsFsSemScratchMetadata:
        UfsFsSemQueryPath(Source, &Row->Source);
        Row->OperationOk = Row->Source.QueryOk;
        if (!Row->OperationOk) {
            Row->OperationError = Row->Source.Error;
            Row->FailureStage = UFS_FSSEM_STAGE_OPERATION;
        }
        return;

    case UfsFsSemDeletePath:
        Row->OperationOk = DeleteFileW(Source);
        if (!Row->OperationOk) {
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_OPERATION;
        }
        return;

    case UfsFsSemDisposition:
    {
        FILE_DISPOSITION_INFO Disposition;

        File = CreateFileW(
            Source,
            DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
            );
        if (File == INVALID_HANDLE_VALUE) {
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_OPEN;
            return;
        }

        ZeroMemory(&Disposition, sizeof(Disposition));
        Disposition.DeleteFile = TRUE;
        Row->OperationOk = SetFileInformationByHandle(
            File,
            FileDispositionInfo,
            &Disposition,
            sizeof(Disposition)
            );
        if (!Row->OperationOk) {
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_OPERATION;
        }
        if (!CloseHandle(File) && Row->OperationOk) {
            Row->OperationOk = FALSE;
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_CLOSE;
        }
        return;
    }

    case UfsFsSemRename:
        Row->OperationOk = MoveFileExW(
            Source,
            Destination,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            );
        if (!Row->OperationOk) {
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_OPERATION;
        }
        return;

    case UfsFsSemDeleteOnClose:
    {
        UCHAR Buffer[UFS_FSSEM_RENAMED_SIZE];

        memset(Buffer, 0xA5, sizeof(Buffer));
        File = CreateFileW(
            Source,
            DELETE | GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL |
                FILE_FLAG_DELETE_ON_CLOSE |
                FILE_FLAG_WRITE_THROUGH,
            NULL
            );
        if (File == INVALID_HANDLE_VALUE) {
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_OPEN;
            return;
        }

        WriteOk = WriteFile(
            File,
            Buffer,
            sizeof(Buffer),
            &Written,
            NULL
            );
        if (!WriteOk || (Written != sizeof(Buffer))) {
            Row->OperationOk = FALSE;
            Row->OperationError = WriteOk ?
                ERROR_WRITE_FAULT : GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_OPERATION;
        } else {
            Row->OperationOk = TRUE;
        }
        if (Row->OperationOk && !FlushFileBuffers(File)) {
            Row->OperationOk = FALSE;
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_FLUSH;
        }

        if (!CloseHandle(File) && Row->OperationOk) {
            Row->OperationOk = FALSE;
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_CLOSE;
        }
        return;
    }

    case UfsFsSemTruncate:
    {
        LARGE_INTEGER Position;

        File = CreateFileW(
            Source,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            NULL
            );
        if (File == INVALID_HANDLE_VALUE) {
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_OPEN;
            return;
        }

        Position.QuadPart = UFS_FSSEM_TRUNCATED_SIZE;
        Row->OperationOk = SetFilePointerEx(
            File,
            Position,
            NULL,
            FILE_BEGIN
            );
        if (Row->OperationOk) {
            Row->OperationOk = SetEndOfFile(File);
        }
        if (!Row->OperationOk) {
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_OPERATION;
        } else if (!FlushFileBuffers(File)) {
            Row->OperationOk = FALSE;
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_FLUSH;
        }

        if (!CloseHandle(File) && Row->OperationOk) {
            Row->OperationOk = FALSE;
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_CLOSE;
        }
        return;
    }

    case UfsFsSemDirectoryCreate:
        Row->OperationOk = CreateDirectoryW(Source, NULL);
        if (!Row->OperationOk) {
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_OPERATION;
        }
        return;

    case UfsFsSemDirectoryRemove:
        Row->OperationOk = RemoveDirectoryW(Source);
        if (!Row->OperationOk) {
            Row->OperationError = GetLastError();
            Row->FailureStage = UFS_FSSEM_STAGE_OPERATION;
        }
        return;
    }

    Error = ERROR_INVALID_FUNCTION;
    Row->OperationError = Error;
    Row->FailureStage = UFS_FSSEM_STAGE_OPERATION;
}

static
VOID
UfsFsSemVerifyRow(
    UFS_FSSEM_OPERATION Operation,
    UFS_FSSEM_ROW *Row
    )
{
    Row->VerifyError = ERROR_SUCCESS;
    switch (Operation) {
    case UfsFsSemScratchMetadata:
        Row->VerifyOk =
            Row->Source.QueryOk &&
            Row->Source.Exists &&
            ((Row->Source.Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) &&
            (Row->Source.Size == UFS_FSSEM_SCRATCH_SIZE);
        break;

    case UfsFsSemDeletePath:
    case UfsFsSemDisposition:
    case UfsFsSemDeleteOnClose:
    case UfsFsSemDirectoryRemove:
        Row->VerifyOk = Row->Source.QueryOk && !Row->Source.Exists;
        break;

    case UfsFsSemRename:
        Row->VerifyOk =
            Row->Source.QueryOk &&
            !Row->Source.Exists &&
            Row->Destination.QueryOk &&
            Row->Destination.Exists &&
            ((Row->Destination.Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) &&
            (Row->Destination.Size == UFS_FSSEM_RENAMED_SIZE);
        break;

    case UfsFsSemTruncate:
        Row->VerifyOk =
            Row->Source.QueryOk &&
            Row->Source.Exists &&
            ((Row->Source.Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) &&
            (Row->Source.Size == UFS_FSSEM_TRUNCATED_SIZE);
        break;

    case UfsFsSemDirectoryCreate:
        Row->VerifyOk =
            Row->Source.QueryOk &&
            Row->Source.Exists &&
            ((Row->Source.Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        break;
    }

    if (!Row->VerifyOk) {
        if (!Row->Source.QueryOk) {
            Row->VerifyError = Row->Source.Error;
        } else if ((Operation == UfsFsSemRename) &&
                   !Row->Destination.QueryOk) {
            Row->VerifyError = Row->Destination.Error;
        } else {
            Row->VerifyError = ERROR_INVALID_DATA;
        }
        if (Row->FailureStage == UFS_FSSEM_STAGE_NONE) {
            Row->FailureStage = UFS_FSSEM_STAGE_VERIFY;
        }
    }
}

static
VOID
UfsFsSemCleanupRow(
    UFS_FSSEM_OPERATION Operation,
    const wchar_t *Source,
    const wchar_t *Destination,
    UFS_FSSEM_ROW *Row
    )
{
    DWORD Error = ERROR_SUCCESS;
    BOOL Ok = TRUE;

    switch (Operation) {
    case UfsFsSemScratchMetadata:
        break;

    case UfsFsSemDeletePath:
    case UfsFsSemDisposition:
    case UfsFsSemDeleteOnClose:
        Ok = UfsFsSemDeleteFileIfPresent(Source, &Error);
        break;

    case UfsFsSemDirectoryRemove:
        Ok = UfsFsSemRemoveDirectoryIfPresent(Source, &Error);
        break;

    case UfsFsSemRename:
        Ok = UfsFsSemDeleteFileIfPresent(Source, &Error);
        if (Ok) {
            Ok = UfsFsSemDeleteFileIfPresent(Destination, &Error);
        }
        break;

    case UfsFsSemTruncate:
        Ok = UfsFsSemDeleteFileIfPresent(Source, &Error);
        break;

    case UfsFsSemDirectoryCreate:
        Ok = UfsFsSemRemoveDirectoryIfPresent(Source, &Error);
        break;
    }

    Row->CleanupOk = Ok;
    Row->CleanupError = Error;
    if (!Ok && (Row->FailureStage == UFS_FSSEM_STAGE_NONE)) {
        Row->FailureStage = UFS_FSSEM_STAGE_CLEANUP;
    }
}

static
VOID
UfsFsSemRunRow(
    ULONG Index,
    const char *Name,
    UFS_FSSEM_OPERATION Operation,
    const wchar_t *Source,
    const wchar_t *Destination,
    UFS_FSSEM_ROW *Row
    )
{
    DWORD PrepareError = ERROR_SUCCESS;

    ZeroMemory(Row, sizeof(*Row));
    Row->Index = Index;
    Row->Name = Name;
    Row->CleanupOk = TRUE;

    if (!UfsFsSemPrepareRow(
            Operation,
            Source,
            Destination,
            &PrepareError)) {
        Row->OperationError = PrepareError;
        Row->FailureStage = UFS_FSSEM_STAGE_PREPARE;
    }

    if (!UfsFsSemCaptureCounters(&Row->Before) &&
        (Row->FailureStage == UFS_FSSEM_STAGE_NONE)) {
        Row->FailureStage = UFS_FSSEM_STAGE_COUNTER;
    }

    if (Row->FailureStage != UFS_FSSEM_STAGE_PREPARE) {
        UfsFsSemPerformRow(Operation, Source, Destination, Row);
    }

    if (!UfsFsSemCaptureCounters(&Row->After) &&
        (Row->FailureStage == UFS_FSSEM_STAGE_NONE)) {
        Row->FailureStage = UFS_FSSEM_STAGE_COUNTER;
    }

    UfsFsSemQueryPath(Source, &Row->Source);
    if (Destination != NULL) {
        UfsFsSemQueryPath(Destination, &Row->Destination);
    }
    UfsFsSemVerifyRow(Operation, Row);
    UfsFsSemCleanupRow(Operation, Source, Destination, Row);
}

static
BOOL
UfsFsSemAppendRow(
    char *Matrix,
    ULONG Capacity,
    ULONG *Length,
    const UFS_FSSEM_ROW *Row
    )
{
    return UfsFsSemAppend(
        Matrix,
        Capacity,
        Length,
        "ROW=%lu NAME=%s ATTEMPTED=%lu OP=%lu VERIFY=%lu CLEANUP=%lu "
        "STAGE=%lu OPERR=%lu VERIFYERR=%lu CLEANUPERR=%lu "
        "SRC_Q=%lu SRC_EXISTS=%lu SRC_ERR=%lu SRC_ATTR=0x%08lx "
        "SRC_SIZE=%llu DST_Q=%lu DST_EXISTS=%lu DST_ERR=%lu "
        "DST_ATTR=0x%08lx DST_SIZE=%llu "
        "BVALID=%lu BWISSUED=%lu BWVERIFY=%lu BFAILED=%lu BDONE=%lu "
        "BSTARTIO=%lu BREARM=%lu BREARMSUCC=%lu BCONTAINED=%lu BFATAL=%lu "
        "AVALID=%lu AWISSUED=%lu AWVERIFY=%lu AFAILED=%lu ADONE=%lu "
        "ASTARTIO=%lu AREARM=%lu AREARMSUCC=%lu ACONTAINED=%lu AFATAL=%lu\n",
        Row->Index,
        Row->Name,
        Row->Attempted ? 1UL : 0UL,
        Row->OperationOk ? 1UL : 0UL,
        Row->VerifyOk ? 1UL : 0UL,
        Row->CleanupOk ? 1UL : 0UL,
        Row->FailureStage,
        Row->OperationError,
        Row->VerifyError,
        Row->CleanupError,
        Row->Source.QueryOk ? 1UL : 0UL,
        Row->Source.Exists ? 1UL : 0UL,
        Row->Source.Error,
        Row->Source.Attributes,
        (unsigned long long)Row->Source.Size,
        Row->Destination.QueryOk ? 1UL : 0UL,
        Row->Destination.Exists ? 1UL : 0UL,
        Row->Destination.Error,
        Row->Destination.Attributes,
        (unsigned long long)Row->Destination.Size,
        Row->Before.Valid ? 1UL : 0UL,
        Row->Before.WritesIssued,
        Row->Before.WritesVerified,
        Row->Before.FailedCommands,
        Row->Before.CompletedCommands,
        Row->Before.StartIoRequests,
        Row->Before.RearmAttempts,
        Row->Before.RearmSuccesses,
        Row->Before.ContainedCommands,
        Row->Before.FatalError,
        Row->After.Valid ? 1UL : 0UL,
        Row->After.WritesIssued,
        Row->After.WritesVerified,
        Row->After.FailedCommands,
        Row->After.CompletedCommands,
        Row->After.StartIoRequests,
        Row->After.RearmAttempts,
        Row->After.RearmSuccesses,
        Row->After.ContainedCommands,
        Row->After.FatalError
        );
}

static
BOOL
UfsFsSemPersistMatrix(
    const wchar_t *ResultPath,
    const char *Matrix,
    ULONG Length,
    DWORD *ErrorOut
    )
{
    ULONG NoteReason = UFS_NOTE_REASON_FSWRITE_CREATE;

    return PersistFilesystemResult(
        ResultPath,
        Matrix,
        Length,
        OPEN_EXISTING,
        TRUE,
        ErrorOut,
        &NoteReason
        );
}

static
BOOL
UfsFsSemRowPassed(
    const UFS_FSSEM_ROW *Row
    )
{
    return
        Row->Attempted &&
        Row->OperationOk &&
        Row->VerifyOk &&
        Row->CleanupOk &&
        Row->Before.Valid &&
        Row->After.Valid;
}

static
ULONG
UfsFsSemClosureTag(
    wchar_t Letter,
    const UFS_FSSEM_CLOSURE_STAGE *Stage
    )
{
    ULONG Tag = ((ULONG)Letter) & 0xFFUL;

    if (Stage->Attempted) {
        Tag |= UFS_FSSEM_CLOSE_ATTEMPTED;
    }
    if (Stage->Succeeded) {
        Tag |= UFS_FSSEM_CLOSE_SUCCEEDED;
    }
    return Tag;
}

static
int
RunFilesystemSemantics(
    VOID
    )
{
    static const char * const Names[UFS_FSSEM_ROW_COUNT] = {
        "SCRATCH_METADATA",
        "DELETE_PATH",
        "DISPOSITION",
        "RENAME",
        "DELETE_ON_CLOSE",
        "TRUNCATE",
        "DIRECTORY_CREATE",
        "DIRECTORY_REMOVE"
    };
    static const wchar_t * const Templates[UFS_FSSEM_ROW_COUNT] = {
        L"?:\\ufsscratch.bin",
        L"?:\\ufs-fssem-delete-path.tmp",
        L"?:\\ufs-fssem-disposition.tmp",
        L"?:\\ufs-fssem-rename-source.tmp",
        L"?:\\ufs-fssem-delete-on-close.tmp",
        L"?:\\ufs-fssem-truncate.tmp",
        L"?:\\ufs-fssem-dir-create",
        L"?:\\ufs-fssem-dir-remove"
    };
    wchar_t Paths[UFS_FSSEM_ROW_COUNT][MAX_PATH];
    wchar_t RenameDestination[] = L"?:\\ufs-fssem-rename-destination.tmp";
    wchar_t ResultPath[] = L"?:\\ufs-fssemantics-result.txt";
    wchar_t VolumePath[] = L"\\\\.\\?:";
    wchar_t Letter = 0;
    char *Matrix;
    ULONG MatrixLength = 0;
    ULONG RowsCompleted = 0;
    ULONG RowsPassed = 0;
    ULONG Index;
    DWORD Error = ERROR_SUCCESS;
    DWORD Bytes = 0;
    BOOL PersistenceOk = TRUE;
    BOOL NotesOk = TRUE;
    BOOL FinalPersisted = FALSE;
    BOOL ClosureOk;
    HANDLE Volume = INVALID_HANDLE_VALUE;
    UFS_FSSEM_ROW Row;
    UFS_FSSEM_CLOSURE_STAGE OpenStage;
    UFS_FSSEM_CLOSURE_STAGE FlushStage;
    UFS_FSSEM_CLOSURE_STAGE LockStage;
    UFS_FSSEM_CLOSURE_STAGE DismountStage;

    ZeroMemory(&OpenStage, sizeof(OpenStage));
    ZeroMemory(&FlushStage, sizeof(FlushStage));
    ZeroMemory(&LockStage, sizeof(LockStage));
    ZeroMemory(&DismountStage, sizeof(DismountStage));

    Matrix = (char *)malloc(UFS_FSSEM_MATRIX_CAPACITY);
    if (Matrix == NULL) {
        printf("UFS_FSSEMANTICS=0 STAGE=ALLOCATE WIN32=%lu\n",
            (ULONG)ERROR_NOT_ENOUGH_MEMORY);
        return 1;
    }
    ZeroMemory(Matrix, UFS_FSSEM_MATRIX_CAPACITY);

    if (!UfsFsWriteFindVolume(&Letter)) {
        printf("UFS_FSSEMANTICS=0 STAGE=FIND_VOLUME WIN32=%lu\n",
            (ULONG)ERROR_PATH_NOT_FOUND);
        goto EmitClosureNotes;
    }

    for (Index = 0; Index < UFS_FSSEM_ROW_COUNT; Index++) {
        wcscpy_s(Paths[Index], MAX_PATH, Templates[Index]);
        Paths[Index][0] = Letter;
    }
    RenameDestination[0] = Letter;
    ResultPath[0] = Letter;
    VolumePath[4] = Letter;

    if (!UfsFsSemAppend(
            Matrix,
            UFS_FSSEM_MATRIX_CAPACITY,
            &MatrixLength,
            "UFS_FSSEMANTICS=0 VERSION=1 STAGE=PENDING DRIVE=%lc ROWS=0\n",
            Letter)) {
        PersistenceOk = FALSE;
        Error = ERROR_INSUFFICIENT_BUFFER;
        goto CloseVolume;
    }
    if (!UfsFsSemPersistMatrix(
            ResultPath,
            Matrix,
            MatrixLength,
            &Error)) {
        PersistenceOk = FALSE;
        printf("UFS_FSSEMANTICS=0 STAGE=PERSIST_PENDING WIN32=%lu\n", Error);
        goto CloseVolume;
    }

    for (Index = 0; Index < UFS_FSSEM_ROW_COUNT; Index++) {
        const wchar_t *Destination =
            (Index == UfsFsSemRename) ? RenameDestination : NULL;

        UfsFsSemRunRow(
            Index,
            Names[Index],
            (UFS_FSSEM_OPERATION)Index,
            Paths[Index],
            Destination,
            &Row
            );
        RowsCompleted++;
        if (UfsFsSemRowPassed(&Row)) {
            RowsPassed++;
        }

        if (!UfsFsSemAppendRow(
                Matrix,
                UFS_FSSEM_MATRIX_CAPACITY,
                &MatrixLength,
                &Row)) {
            PersistenceOk = FALSE;
            Error = ERROR_INSUFFICIENT_BUFFER;
            break;
        }

        if (!UfsFsSemPersistMatrix(
                ResultPath,
                Matrix,
                MatrixLength,
                &Error)) {
            PersistenceOk = FALSE;
            Row.FailureStage = UFS_FSSEM_STAGE_PERSIST;
            break;
        }

        printf(
            "UFS_FSSEMANTICS_ROW=%lu NAME=%s OP=%lu VERIFY=%lu "
            "CLEANUP=%lu STAGE=%lu WIN32=%lu\n",
            Row.Index,
            Row.Name,
            Row.OperationOk ? 1UL : 0UL,
            Row.VerifyOk ? 1UL : 0UL,
            Row.CleanupOk ? 1UL : 0UL,
            Row.FailureStage,
            Row.OperationError
            );
    }

    if (UfsFsSemAppend(
            Matrix,
            UFS_FSSEM_MATRIX_CAPACITY,
            &MatrixLength,
            "UFS_FSSEMANTICS=%lu VERSION=1 STAGE=FINAL ROWS=%lu "
            "PASSED=%lu PERSIST=%lu CLOSURE=PRAM\n",
            ((RowsCompleted == UFS_FSSEM_ROW_COUNT) &&
             (RowsPassed == UFS_FSSEM_ROW_COUNT) &&
             PersistenceOk) ? 1UL : 0UL,
            RowsCompleted,
            RowsPassed,
            PersistenceOk ? 1UL : 0UL)) {
        if (UfsFsSemPersistMatrix(
                ResultPath,
                Matrix,
                MatrixLength,
                &Error)) {
            FinalPersisted = TRUE;
        } else {
            PersistenceOk = FALSE;
        }
    } else {
        PersistenceOk = FALSE;
        Error = ERROR_INSUFFICIENT_BUFFER;
    }

CloseVolume:
    OpenStage.Attempted = TRUE;
    Volume = CreateFileW(
        VolumePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Volume == INVALID_HANDLE_VALUE) {
        OpenStage.Error = GetLastError();
    } else {
        OpenStage.Succeeded = TRUE;
        OpenStage.Error = ERROR_SUCCESS;

        FlushStage.Attempted = TRUE;
        FlushStage.Succeeded = FlushFileBuffers(Volume);
        FlushStage.Error = FlushStage.Succeeded ?
            ERROR_SUCCESS : GetLastError();

        if (FlushStage.Succeeded) {
            LockStage.Attempted = TRUE;
            LockStage.Succeeded = DeviceIoControl(
                Volume,
                FSCTL_LOCK_VOLUME,
                NULL,
                0,
                NULL,
                0,
                &Bytes,
                NULL
                );
            LockStage.Error = LockStage.Succeeded ?
                ERROR_SUCCESS : GetLastError();
        }

        if (LockStage.Succeeded) {
            DismountStage.Attempted = TRUE;
            DismountStage.Succeeded = DeviceIoControl(
                Volume,
                FSCTL_DISMOUNT_VOLUME,
                NULL,
                0,
                NULL,
                0,
                &Bytes,
                NULL
                );
            DismountStage.Error = DismountStage.Succeeded ?
                ERROR_SUCCESS : GetLastError();
        }

        CloseHandle(Volume);
        Volume = INVALID_HANDLE_VALUE;
    }

EmitClosureNotes:
    NotesOk =
        SendVendorNote(
            UFS_NOTE_REASON_FSCLOSE_VOLUME_OPEN,
            OpenStage.Error,
            UfsFsSemClosureTag(Letter, &OpenStage)
            ) && NotesOk;
    NotesOk =
        SendVendorNote(
            UFS_NOTE_REASON_FSCLOSE_VOLUME_FLUSH,
            FlushStage.Error,
            UfsFsSemClosureTag(Letter, &FlushStage)
            ) && NotesOk;
    NotesOk =
        SendVendorNote(
            UFS_NOTE_REASON_FSCLOSE_VOLUME_LOCK,
            LockStage.Error,
            UfsFsSemClosureTag(Letter, &LockStage)
            ) && NotesOk;
    NotesOk =
        SendVendorNote(
            UFS_NOTE_REASON_FSCLOSE_VOLUME_DISMOUNT,
            DismountStage.Error,
            UfsFsSemClosureTag(Letter, &DismountStage)
            ) && NotesOk;

    ClosureOk =
        OpenStage.Succeeded &&
        FlushStage.Succeeded &&
        LockStage.Succeeded &&
        DismountStage.Succeeded;

    printf(
        "UFS_FSSEMANTICS=%lu ROWS=%lu PASSED=%lu PERSIST=%lu "
        "OPEN=%lu/%lu/%lu FLUSH=%lu/%lu/%lu LOCK=%lu/%lu/%lu "
        "DISMOUNT=%lu/%lu/%lu NOTES=%lu\n",
        ((RowsCompleted == UFS_FSSEM_ROW_COUNT) &&
         (RowsPassed == UFS_FSSEM_ROW_COUNT) &&
         PersistenceOk &&
         FinalPersisted &&
         ClosureOk &&
         NotesOk) ? 1UL : 0UL,
        RowsCompleted,
        RowsPassed,
        (PersistenceOk && FinalPersisted) ? 1UL : 0UL,
        OpenStage.Attempted ? 1UL : 0UL,
        OpenStage.Succeeded ? 1UL : 0UL,
        OpenStage.Error,
        FlushStage.Attempted ? 1UL : 0UL,
        FlushStage.Succeeded ? 1UL : 0UL,
        FlushStage.Error,
        LockStage.Attempted ? 1UL : 0UL,
        LockStage.Succeeded ? 1UL : 0UL,
        LockStage.Error,
        DismountStage.Attempted ? 1UL : 0UL,
        DismountStage.Succeeded ? 1UL : 0UL,
        DismountStage.Error,
        NotesOk ? 1UL : 0UL
        );

    free(Matrix);
    return
        ((RowsCompleted == UFS_FSSEM_ROW_COUNT) &&
         (RowsPassed == UFS_FSSEM_ROW_COUNT) &&
         PersistenceOk &&
         FinalPersisted &&
         ClosureOk &&
         NotesOk) ? 0 : 1;
}

static
int
RunFilesystemWrite(
    VOID
    )
{
    wchar_t Letter = 0;
    wchar_t Path[] = L"?:\\ufs-fswrite-proof.bin";
    wchar_t Result[] = L"?:\\ufs-fswrite-result.txt";
    wchar_t VolumePath[] = L"\\\\.\\?:";
    HANDLE File;
    HANDLE Volume;
    unsigned char *Buffer;
    UFS_DIAGNOSTIC_DATA Diagnostic;
    ULONG Index;
    DWORD Written = 0;
    DWORD Bytes = 0;
    DWORD ResultError = ERROR_SUCCESS;
    ULONG ResultNoteReason = UFS_NOTE_REASON_FSWRITE_CREATE;
    BOOL Dismounted = FALSE;
    BOOL DiagnosticValid = FALSE;
    BOOL Locked = FALSE;
    BOOL ResultPersisted = FALSE;
    static const char Pending[] =
        "UFS_FSWRITE=0 PROFILE=FSEND16M STAGE=STARTED\n";

    /*
     * RETIRED: the FSWRITE_START note used to sit here.
     *
     * The note ring is 8 slots and the survey now fills all 8, so a ninth note
     * would silently overwrite the last one under the exhaustion policy - and
     * the slots live at ring offset 0, where every extra slot would eat 80 more
     * bytes of the UEFI marker area (=NTPATCH, =CFQSCAN) that is the early
     * warning for the CNTFRQ regression. Enlarging the ring is therefore not an
     * option and one note had to go.
     *
     * This was the cheapest to lose: its aux value is GetLogicalDrives(), which
     * is the identical value FSWRITE_NO_VOLUME reports at the end of the same
     * function, and the "did we get this far" question it answered is now
     * answered better by the survey's own four notes.
     */
    printf("UFS_FSWRITE_START=1 DRIVES=0x%08lX\n",
        (unsigned long)GetLogicalDrives());

    /*
     * Measure and, where possible, repair the disk state before searching for
     * a volume. The survey may assign W: to a letterless volume, so it has to
     * run before UfsFsWriteFindVolume rather than after a failure - otherwise
     * proving the cause and fixing it would cost two boots instead of one.
     */
    UfsDiskSurvey();

    if (!UfsFsWriteFindVolume(&Letter)) {
        printf("UFS_FSWRITE=0 REASON=NO_VOLUME\n");
        SendVendorNote(UFS_NOTE_REASON_FSWRITE_NO_VOLUME, GetLogicalDrives(), 0);
        return 1;
    }

    Path[0] = Letter;
    Result[0] = Letter;
    VolumePath[4] = Letter;

    /*
     * The result file is the durable verdict. Truncate it to a flushed PENDING
     * record before touching the payload so a failed boot can never inherit an
     * older success. Do not use DeleteFileW here: the first FSEND16M phone boot
     * proved this FAT path returns ERROR_INVALID_FUNCTION for deletion while
     * CREATE_ALWAYS remains the already-proven create/truncate operation.
     */
    if (!PersistFilesystemResult(
            Result,
            Pending,
            (DWORD)(sizeof(Pending) - 1),
            CREATE_ALWAYS,
            FALSE,
            &ResultError,
            &ResultNoteReason
            )) {
        printf(
            "UFS_FSWRITE=0 REASON=RESULT_RESET VOL=%lc GLE=%lu\n",
            Letter,
            ResultError
            );
        SendVendorNote(ResultNoteReason, ResultError, (ULONG)Letter);
        return 1;
    }

    Buffer = (unsigned char *)malloc(UFS_FSWRITE_TOTAL);
    if (Buffer == NULL) {
        printf("UFS_FSWRITE=0 REASON=NO_MEMORY VOL=%lc\n", Letter);
        SendVendorNote(UFS_NOTE_REASON_FSWRITE_NO_MEMORY, UFS_FSWRITE_TOTAL,
            (ULONG)Letter);
        return 1;
    }

    //
    // Self-identifying payload: every 64-byte record carries its own index, so
    // a truncated, duplicated or misordered write is visible from TWRP by
    // reading the file rather than by trusting a hash alone.
    //
    for (Index = 0; Index < UFS_FSWRITE_RECORDS; Index++) {
        unsigned char *Record =
            Buffer + ((size_t)Index * (size_t)UFS_FSWRITE_RECORD_SIZE);
        char Header[32];
        int Length;
        int Pos;

        Length = sprintf_s(
            Header,
            sizeof(Header),
            "UFSFSWRITE-V1 %08lu ",
            Index
            );
        if (Length < 0) {
            printf("UFS_FSWRITE=0 REASON=FORMAT VOL=%lc\n", Letter);
            SendVendorNote(UFS_NOTE_REASON_FSWRITE_FORMAT, Index, (ULONG)Letter);
            free(Buffer);
            return 1;
        }

        for (Pos = 0; Pos < Length; Pos++) {
            Record[Pos] = (unsigned char)Header[Pos];
        }
        for (; Pos < (int)UFS_FSWRITE_RECORD_SIZE - 1; Pos++) {
            Record[Pos] = (unsigned char)'.';
        }
        Record[UFS_FSWRITE_RECORD_SIZE - 1] = (unsigned char)'\n';
    }

    File = CreateFileW(
        Path,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL
        );
    if (File == INVALID_HANDLE_VALUE) {
        DWORD CreateError = GetLastError();

        printf(
            "UFS_FSWRITE=0 REASON=CREATE VOL=%lc GLE=%lu\n",
            Letter,
            CreateError
            );
        /*
         * The decisive one. GLE=19 (ERROR_WRITE_PROTECT) means the volume
         * mounted read-only, i.e. the driver still advertised write protection
         * at MODE SENSE time; anything else points elsewhere entirely. Reading
         * the raw code is the whole reason this channel exists.
         */
        SendVendorNote(UFS_NOTE_REASON_FSWRITE_CREATE, CreateError, (ULONG)Letter);
        free(Buffer);
        return 1;
    }

    {
        BOOL WriteOk = WriteFile(
            File,
            Buffer,
            UFS_FSWRITE_TOTAL,
            &Written,
            NULL
            );

        if (!WriteOk || (Written != UFS_FSWRITE_TOTAL)) {
            DWORD WriteError = WriteOk ? ERROR_WRITE_FAULT : GetLastError();

            printf(
                "UFS_FSWRITE=0 REASON=WRITE VOL=%lc GLE=%lu WROTE=%lu\n",
                Letter,
                WriteError,
                Written
                );
            SendVendorNote(
                UFS_NOTE_REASON_FSWRITE_WRITE,
                WriteError,
                (ULONG)Letter
                );
            CloseHandle(File);
            free(Buffer);
            return 1;
        }
    }

    if (!FlushFileBuffers(File)) {
        DWORD FlushError = GetLastError();

        printf(
            "UFS_FSWRITE=0 REASON=FLUSH VOL=%lc GLE=%lu\n",
            Letter,
            FlushError
            );
        SendVendorNote(UFS_NOTE_REASON_FSWRITE_FLUSH, FlushError, (ULONG)Letter);
        CloseHandle(File);
        free(Buffer);
        return 1;
    }

    CloseHandle(File);
    free(Buffer);

    //
    // Snapshot the driver after the large payload, before the tiny verdict file
    // adds its own metadata writes. This is the command-volume measurement:
    // unlike PRAM, it cannot be truncated by hundreds of UFSWRIT records.
    //
    ZeroMemory(&Diagnostic, sizeof(Diagnostic));
    DiagnosticValid = RunVendorDiagSnapshot(&Diagnostic);

    //
    // Rewrite the PENDING file with the final verdict. If create, write, flush or
    // close fails, the helper returns failure and the host rejects either the
    // pending line or a partial final line.
    //
    {
        char Line[512];
        int Length = sprintf_s(
            Line,
            sizeof(Line),
            "UFS_FSWRITE=%lu PROFILE=FSEND16M BYTES=%lu RECORDS=%lu "
            "RECORD_SIZE=%lu DIAG=%lu WISSUED=%lu WVERIFY=%lu "
            "REARM=%lu REARMSUCC=%lu CONTAINED=%lu FATAL=%lu "
            "FAILED=%lu DONE=%lu STARTIO=%lu\n",
            DiagnosticValid ? 1UL : 0UL,
            UFS_FSWRITE_TOTAL,
            UFS_FSWRITE_RECORDS,
            UFS_FSWRITE_RECORD_SIZE,
            DiagnosticValid ? 1UL : 0UL,
            Diagnostic.WritesIssued,
            Diagnostic.WritesVerified,
            Diagnostic.RearmAttempts,
            Diagnostic.RearmSuccesses,
            Diagnostic.ContainedCommands,
            Diagnostic.FatalError,
            Diagnostic.FailedCommands,
            Diagnostic.CompletedCommands,
            Diagnostic.StartIoRequests
            );

        if (Length <= 0) {
            ResultError = ERROR_INVALID_DATA;
            ResultNoteReason = UFS_NOTE_REASON_FSWRITE_FORMAT;
        } else {
            ResultPersisted = PersistFilesystemResult(
                Result,
                Line,
                (DWORD)Length,
                CREATE_ALWAYS,
                FALSE,
                &ResultError,
                &ResultNoteReason
                );
        }
    }

    //
    // Force the filesystem's own metadata out. FlushFileBuffers above covers
    // each handle's data; only a dismount guarantees the directory entries and
    // both FAT copies have reached media before the PMU reset.
    //
    Volume = CreateFileW(
        VolumePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
        );
    if (Volume != INVALID_HANDLE_VALUE) {
        Locked = DeviceIoControl(
            Volume,
            FSCTL_LOCK_VOLUME,
            NULL, 0, NULL, 0,
            &Bytes,
            NULL
            );
        Dismounted = DeviceIoControl(
            Volume,
            FSCTL_DISMOUNT_VOLUME,
            NULL, 0, NULL, 0,
            &Bytes,
            NULL
            );
        CloseHandle(Volume);
    }

    //
    // The data write is reported separately from the dismount on purpose. If
    // the dismount failed the file may still be correct on media, and saying
    // "1" for both would hide exactly the case that needs a closer look.
    //
    printf(
        "UFS_FSWRITE=1 VOL=%lc PROFILE=FSEND16M BYTES=%lu RECORDS=%lu "
        "DIAG=%d RESULT=%d LOCK=%d DISMOUNT=%d\n",
        Letter,
        UFS_FSWRITE_TOTAL,
        UFS_FSWRITE_RECORDS,
        DiagnosticValid ? 1 : 0,
        ResultPersisted ? 1 : 0,
        Locked ? 1 : 0,
        Dismounted ? 1 : 0
        );

    if (!ResultPersisted) {
        printf(
            "UFS_FSWRITE_RESULT=0 GLE=%lu\n",
            ResultError
            );
        SendVendorNote(ResultNoteReason, ResultError, (ULONG)Letter);
        return 1;
    }

    if (!DiagnosticValid) {
        printf("UFS_FSWRITE=0 REASON=DIAGNOSTIC_SNAPSHOT\n");
        SendVendorNote(
            UFS_NOTE_REASON_FSWRITE_DIAG,
            ERROR_INVALID_DATA,
            (ULONG)Letter
            );
        return 1;
    }

    /*
     * Sent last so its presence means the whole sequence completed. Aux packs
     * lock and dismount, since a payload that reached media but was never
     * dismounted is a materially different result from a clean one.
     */
    SendVendorNote(
        UFS_NOTE_REASON_FSWRITE_OK,
        ((ULONG)(Locked ? 1 : 0) << 1) | (ULONG)(Dismounted ? 1 : 0),
        (ULONG)Letter
        );
    return 0;
}

int
wmain(
    int ArgumentCount,
    wchar_t **Arguments
    )
{    ULONG AdapterNumber;
    ULONG OpenedPorts = 0;
    ULONG IoctlAttempts = 0;
    DWORD LastOpenError = ERROR_SUCCESS;
    DWORD LastIoctlError = ERROR_SUCCESS;
    ULONG LastReturnCode = UFS_DIAG_RETURN_INVALID_REQUEST;
    BOOL Detailed = TRUE;

    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--summary") == 0)) {
        Detailed = FALSE;
    }

    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--probe") == 0)) {
        RunScsiProbe();
        RunVendorDiag();
        return 0;
    }

    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--vendor") == 0)) {
        ReportDiskAddress();
        RunVendorDiag();
        return 0;
    }

    /*
     * Write-path latch verbs. Each prints the resulting mode, then the caller
     * re-runs the plain helper to see WRITE_MODE reflected in telemetry.
     */    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--arm-dry-run") == 0)) {
        return RequestWriteArm(
            UFS_DIAG_WRITE_ARM_CONFIRM_DRY_RUN, "DRY_RUN") ? 0 : 1;
    }

    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--arm-live") == 0)) {
        return RequestWriteArm(
            UFS_DIAG_WRITE_ARM_CONFIRM_LIVE, "LIVE") ? 0 : 1;
    }

    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--disarm") == 0)) {
        return RequestWriteArm(
            UFS_DIAG_WRITE_ARM_CONFIRM_DISARM, "DISARMED") ? 0 : 1;
    }

    /*
     * Exercise the write path. Nothing else in the image can issue a WRITE(10),
     * so without this verb the whole safeguard stack is untested on hardware.
     * The table is compiled in and its LBAs come from the driver's own fence, so
     * this is not a general write tool.
     */
    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--write-probe") == 0)) {
        RunWriteProbe();
        RunVendorDiag();
        return 0;
    }

    /*
     * The real thing. Separate from --write-probe on purpose: the probe is a
     * safeguard test that must never touch media, this is the one verb that is
     * meant to. It refuses unless the adapter is already LIVE, so arming and
     * writing stay two deliberate steps.
     */
    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--write-live") == 0)) {
        RunWriteLive();
        RunVendorDiag();
        return 0;
    }

    /*
     * The first write in this project that Windows itself issues. Everything
     * above builds its own CDBs; this one hands the job to the FAT driver and
     * only observes the result, which is the difference between "the write path
     * works" and "this is a disk".
     *
     * Deliberately separate from --write-live: that verb is reversible by
     * construction (capture, write, restore, re-read), this one is not - a file
     * created through the filesystem stays created. It is safe on the 9810
     * because the fence confined it to sda18, whose entire contents are
     * reproducible from the host; on this device the fenced volume is sda31
     * USERDATA and the same argument has NOT been re-run, so this verb is
     * unvalidated here until Windows boots from this disk.
     */
    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--fs-write") == 0)) {
        return RunFilesystemWrite();
    }

    /*
     * Disposable FAT operation matrix plus strict volume closure. This path is
     * intentionally separate from --disk-survey: its four closure notes must
     * retain slots 0..3 instead of competing with the survey's eight notes.
     */
    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--fs-semantics") == 0)) {
        return RunFilesystemSemantics();
    }

    /*
     * Hand the phone back to TWRP without a human holding buttons. This is the
     * last link in the unattended loop: everything else can be driven from the
     * host over adb, but only while the device is in recovery, and WinPE has no
     * working reset of its own on this platform.
     */
    if ((ArgumentCount == 2) &&
        (lstrcmpiW(Arguments[1], L"--reboot-recovery") == 0)) {
        return RequestRebootToRecovery() ? 0 : 1;
    }

    /*
     * --log-lba <srcfile> <blockoffset>
     *
     * Copy a file to a raw LBA in the scratch region. This is the only channel
     * that survives to TWRP: FAT persistence needs a filesystem write, and it
     * is exactly the filesystem writes that have been bugchecking, so a log
     * that depends on one cannot report the failure it exists to explain.
     *
     * Deliberately not going through a volume: the target sectors sit one block
     * past USERDATA's (sda31) last LBA and below the backup GPT, so no mounted
     * volume owns them and no lock is needed. The 2026-07-31 ladder proved this
     * exact path on the 9810's tail; on this device the equivalent region was
     * read back all zero (notes/s10e-root-reads.md §9) but nothing has been
     * written to it yet.
     *
     * Each call takes an explicit slot so a boot leaves a progressive trail: if
     * slot 128 is present and 160 is not, the machine died between them.
     */
    if ((ArgumentCount == 4) &&
        (lstrcmpiW(Arguments[1], L"--log-lba") == 0)) {
        HANDLE Source;
        HANDLE Disk;
        LARGE_INTEGER Offset;
        unsigned char *Buffer;
        wchar_t *SlotEnd = NULL;
        unsigned long BlockOffset;
        DWORD ReadBytes = 0;
        DWORD Written = 0;
        DWORD Error = 0;

        BlockOffset = wcstoul(Arguments[3], &SlotEnd, 10);
        if ((SlotEnd == Arguments[3]) ||
            (BlockOffset < UFS_WRITE_LIVE_MAX_BLOCKS) ||
            ((BlockOffset + UFS_LOGLBA_BLOCKS) > UFS_SCRATCH_BLOCKS)) {
            printf(
                "UFS_LOGLBA=0 REASON=BAD_SLOT OFF=%lu VALID=[%lu..%lu]\n",
                BlockOffset,
                UFS_WRITE_LIVE_MAX_BLOCKS,
                UFS_SCRATCH_BLOCKS - UFS_LOGLBA_BLOCKS
                );
            return 1;
        }

        Buffer = (unsigned char *)VirtualAlloc(
            NULL,
            UFS_LOGLBA_SPAN,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE
            );
        if (Buffer == NULL) {
            printf("UFS_LOGLBA=0 REASON=ALLOC WIN32=%lu\n",
                (unsigned long)GetLastError());
            return 1;
        }

        /*
         * VirtualAlloc already zeroes, but the header is stamped explicitly so
         * a reader can tell "slot written this boot" from "slot never touched"
         * without having to trust that the region started clean.
         */
        memcpy(Buffer, UFS_LOGLBA_MAGIC, sizeof(UFS_LOGLBA_MAGIC) - 1);
        Buffer[16] = (unsigned char)(BlockOffset & 0xFFUL);
        Buffer[17] = (unsigned char)((BlockOffset >> 8) & 0xFFUL);

        Source = CreateFileW(
            Arguments[2],
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
            );
        if (Source == INVALID_HANDLE_VALUE) {
            Error = GetLastError();
            printf("UFS_LOGLBA=0 REASON=NO_SOURCE WIN32=%lu SRC=%ls\n",
                (unsigned long)Error, Arguments[2]);
            VirtualFree(Buffer, 0, MEM_RELEASE);
            return 1;
        }

        if (!ReadFile(Source, Buffer + UFS_LOGLBA_HEADER,
                      UFS_LOGLBA_SPAN - UFS_LOGLBA_HEADER, &ReadBytes, NULL)) {
            ReadBytes = 0;
        }
        CloseHandle(Source);

        Disk = CreateFileW(
            L"\\\\.\\PhysicalDrive0",
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
            );
        if (Disk == INVALID_HANDLE_VALUE) {
            Error = GetLastError();
            printf("UFS_LOGLBA=0 REASON=NO_DISK WIN32=%lu\n",
                (unsigned long)Error);
            VirtualFree(Buffer, 0, MEM_RELEASE);
            return 1;
        }

        Offset.QuadPart =
            (LONGLONG)(UFS_SCRATCH_LBA + BlockOffset) *
            (LONGLONG)UFS_WRITE_LIVE_BLOCK_SIZE;

        if (!SetFilePointerEx(Disk, Offset, NULL, FILE_BEGIN)) {
            Error = GetLastError();
            printf("UFS_LOGLBA=0 REASON=SEEK WIN32=%lu\n",
                (unsigned long)Error);
            CloseHandle(Disk);
            VirtualFree(Buffer, 0, MEM_RELEASE);
            return 1;
        }

        if (!WriteFile(Disk, Buffer, UFS_LOGLBA_SPAN, &Written, NULL)) {
            Error = GetLastError();
            printf("UFS_LOGLBA=0 REASON=WRITE WIN32=%lu BYTES=%lu\n",
                (unsigned long)Error, (unsigned long)Written);
            CloseHandle(Disk);
            VirtualFree(Buffer, 0, MEM_RELEASE);
            return (int)Error;
        }

        CloseHandle(Disk);
        VirtualFree(Buffer, 0, MEM_RELEASE);

        printf(
            "UFS_LOGLBA=1 SLOT=%lu LBA=%lu SRCBYTES=%lu WROTE=%lu\n",
            BlockOffset,
            UFS_SCRATCH_LBA + BlockOffset,
            (unsigned long)ReadBytes,
            (unsigned long)Written
            );
        return 0;
    }

    /*
     * Persist a WinPE-ramdisk log onto the UFS volume so it survives the
     * vendor-CDB self-reboot. --write-live's stdout is the ONLY place the
     * per-rung WLIVE_STEP=.../OK=/WIN32=/SCSI_STATUS=/XFER= detail exists, and
     * X: is destroyed by the reset, so without this the one measurement that
     * distinguishes READ_ORIGINAL_FAILED from SCRATCH_SIGNATURE_MISMATCH is
     * unreadable - which is exactly why the rung-8 verdict has stayed
     * REASON=SIGNATURE for two boots running.
     *
     * In-process on purpose. The loader's batch equivalent (for /if exist/copy)
     * shipped byte-identically in the deployed image, ran, and silently
     * delivered nothing on both boots; a batch copy cannot report WHY. The
     * allocation-preserving copy reports the Win32 error while retaining the
     * destination's existing FAT clusters for allocation-neutral closure.
     *
     * Volume identity is UfsFsWriteFindVolume - the same two-marker content
     * check --fs-write uses, never a guessed letter, with X: excluded.
     *
     * Deliberately NO UfsDiskSurvey() here: --fs-write already surveyed in this
     * same boot and the note ring is only 8 slots, so a second survey would
     * clobber DISK_PROBE..FSWRITE_OK.
     */
    if ((ArgumentCount == 4) &&
        (lstrcmpiW(Arguments[1], L"--copy-log") == 0)) {
        wchar_t Letter = 0;
        wchar_t Dest[MAX_PATH];
        DWORD CopyError;
        DWORD CopiedBytes;

        if (!UfsFsWriteFindVolume(&Letter)) {
            printf("UFS_COPYLOG=0 REASON=NO_VOLUME SRC=%ls\n", Arguments[2]);
            return 1;
        }

        _snwprintf_s(
            Dest,
            ARRAYSIZE(Dest),
            _TRUNCATE,
            L"%c:\\%s",
            Letter,
            Arguments[3]
            );

        if (!CopyFilePreservingAllocation(
                Arguments[2],
                Dest,
                &CopiedBytes,
                &CopyError)) {
            printf(
                "UFS_COPYLOG=0 REASON=COPY_FAILED WIN32=%lu SRC=%ls DEST=%ls\n",
                CopyError,
                Arguments[2],
                Dest
                );
            return (int)CopyError;
        }

        printf("UFS_COPYLOG=1 BYTES=%lu DEST=%ls\n", CopiedBytes, Dest);
        return 0;
    }

    /*
     * Loader-independent safety net for the unattended loop. See
     * RunRebootWatchdog: the dashboard countdown cannot rescue a boot that hangs
     * before StatusBoard is ever launched, and that is exactly the failure that
     * stranded the phone on both write-probe boots.
     */
    if ((ArgumentCount == 3) &&
        (lstrcmpiW(Arguments[1], L"--watchdog") == 0)) {
        wchar_t *end = NULL;
        unsigned long seconds = wcstoul(Arguments[2], &end, 10);

        if ((end == Arguments[2]) || (end != NULL && *end != L'\0')) {
            printf("UFS_WATCHDOG=0 REASON=BAD_DELAY\n");
            return 1;
        }
        return RunRebootWatchdog((ULONG)seconds);
    }

    ReportDiskAddress();
    for (AdapterNumber = 0; AdapterNumber < 64; AdapterNumber++) {
        WCHAR Path[32];
        HANDLE Handle;
        UFS_DIAGNOSTIC_PACKET Packet;
        DWORD BytesReturned = 0;
        BOOL Result;

        _snwprintf_s(
            Path,
            ARRAYSIZE(Path),
            _TRUNCATE,
            L"\\\\.\\Scsi%lu:",
            AdapterNumber
            );
        Handle = CreateFileW(
            Path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
            );
        if (Handle == INVALID_HANDLE_VALUE) {
            LastOpenError = GetLastError();
            continue;
        }
        OpenedPorts++;

        ZeroMemory(&Packet, sizeof(Packet));
        /*
         * Sentinel: UFS_DIAG_RETURN_SUCCESS is 0 and ZeroMemory already wrote
         * 0, so a packet the driver never touched is indistinguishable from a
         * successful one. Poison both fields first so "untouched" is provable.
         */
        Packet.Control.ReturnCode = 0xDEADBEEFUL;
        Packet.Data.Signature = 0xDEADBEEFUL;
        Packet.Control.HeaderLength = sizeof(Packet.Control);
        CopyMemory(
            Packet.Control.Signature,
            UFS_DIAG_SRB_SIGNATURE,
            UFS_DIAG_SRB_SIGNATURE_LENGTH
            );
        Packet.Control.Timeout = 10;
        Packet.Control.ControlCode = UFS_DIAG_CONTROL_CODE;
        Packet.Control.Length = sizeof(Packet.Data);

        Result = DeviceIoControl(
            Handle,
            IOCTL_SCSI_MINIPORT,
            &Packet,
            sizeof(Packet),
            &Packet,
            sizeof(Packet),
            &BytesReturned,
            NULL
            );
        LastIoctlError = Result ? ERROR_SUCCESS : GetLastError();
        LastReturnCode = Packet.Control.ReturnCode;
        IoctlAttempts++;
        CloseHandle(Handle);

        printf(
            "UFS_V9_PORT=%lu OPEN=1 IOCTL=%lu ERROR=%lu RETURN=%lu BYTES=%lu\n",
            AdapterNumber,
            Result ? 1UL : 0UL,
            LastIoctlError,
            LastReturnCode,
            BytesReturned
            );
        printf(
            "  SIGNATURE=0x%08lX VERSION=0x%08lX SIZE=%lu LENGTH=%lu\n",
            Packet.Data.Signature,
            Packet.Data.Version,
            Packet.Data.Size,
            Packet.Control.Length
            );
        PrintHexPrefix((const UCHAR *)&Packet, 32);

        if (Result &&
            (Packet.Control.ReturnCode == UFS_DIAG_RETURN_SUCCESS) &&
            (Packet.Data.Signature == UFS_DIAG_DATA_SIGNATURE) &&
            (Packet.Data.Version == UFS_DIAG_DATA_VERSION) &&
            (Packet.Data.Size == sizeof(Packet.Data))) {
            printf(
                "UFS_V9_SCAN_PORTS=64 OPENED=%lu IOCTLS=%lu LAST_OPEN=%lu\n",
                OpenedPorts,
                IoctlAttempts,
                LastOpenError
                );
            PrintDiagnostic(AdapterNumber, &Packet.Data, Detailed);
            return 0;
        }
    }

    printf(
        "UFS_V9_SCAN_PORTS=64 OPENED=%lu IOCTLS=%lu LAST_OPEN=%lu "
        "LAST_IOCTL=%lu LAST_RETURN=%lu\n",
        OpenedPorts,
        IoctlAttempts,
        LastOpenError,
        LastIoctlError,
        LastReturnCode
        );
    /*
     * The SRB_FUNCTION_IO_CONTROL channel is unreliable on this stack, so fall
     * back to the vendor data-in CDB before declaring the adapter unreadable.
     */
    if (RunVendorDiag()) {
        return 0;
    }
    printf("UFS_V9_DIAG_FOUND=0\n");
    return 2;
}
