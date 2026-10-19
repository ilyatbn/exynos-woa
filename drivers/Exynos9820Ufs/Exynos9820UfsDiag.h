#pragma once

#define UFS_DIAG_DATA_SIGNATURE                    0x39465355UL

/*
 * V33 latency instrumentation switch.
 *
 * Set to 0 to compile the driver back to exactly the deployed v32 shape: the
 * diagnostic tail below disappears, sizeof(UFS_DIAGNOSTIC_DATA) and the adapter
 * extension return to their v32 values, and the version reverts. That is not a
 * convenience - it is the proof. Building this tree with the switch off must
 * reproduce v32's .text byte-for-byte, which is what demonstrates that the only
 * thing this revision adds is measurement, on the one driver in this project
 * whose regressions corrupt the Windows volume.
 */
#ifndef UFS_PERF_INSTRUMENTATION
#define UFS_PERF_INSTRUMENTATION                   1

/*
 * 2026-10-19 per-boot I/O mode (notes/ufs-async-plan.md stages 1-2, notes/app-launch-slowness.md):
 *   UFS_IO_FAST_ZERO   - clear only what a command uses (TRD slot, command UPIU, response UPIU, the PRDT entries
 *                        actually programmed) instead of the whole UTRL + UCD before AND after every command.
 *   UFS_IO_FULL_DUPLEX - StorSynchronizeFullDuplex: StartIo (and its completion poll) runs at DISPATCH under the
 *                        StartIo lock instead of DIRQL; HwStorTimer/HwResetBus still hold the StartIo lock
 *                        (MS Learn StorPortAcquireSpinLock lock table), the ISR is a no-op on the parked line.
 * Forced to 0 (the w38 behaviour) when the previous boot died: BootLogDxe's unexplained-death counter in progress
 * word [12] at 0x8FE0F000 ('DE' << 16 | n, cleared by a clean OS restart or a cold power-on).
 */
#ifndef UFS_IO_MODE_DEFAULT
#define UFS_IO_MODE_DEFAULT                        0x3UL
#endif
#define UFS_IO_FAST_ZERO                           0x1UL
#define UFS_IO_FULL_DUPLEX                         0x2UL
#define UFS_FAST_ZERO_COMMAND_BYTES                64UL    /* command UPIU is 32 B */
#define UFS_FAST_ZERO_RESPONSE_BYTES               128UL   /* response UPIU is 52 B */
#define UFS_DEATH_COUNTER_WORD                     12UL
#define UFS_DEATH_COUNTER_MAGIC                    0x4445UL
#endif

/*
 * V34 wide-uncached-access switch.
 *
 * The bounce buffer is MmNonCached, which on this platform is Device memory:
 * every access is its own bus transaction, nothing merges, nothing caches. The
 * transfer helpers move it a ULONG at a time, so a 64 KB read costs 16384
 * uncached loads to drain the bounce plus 16384 uncached stores to pre-zero it,
 * all on one 1.05 GHz core.
 *
 * This switch widens the aligned fast path from 32-bit to 64-bit accesses,
 * halving the transaction count. It is the same class of change this file
 * already documents making once before - byte-at-a-time to ULONG-at-a-time,
 * which it records as having "dominated boot" until fixed.
 *
 * A 64-bit access to Device memory is a single, naturally-atomic transaction
 * when the address is 8-byte aligned, so this preserves both the bytes written
 * and the order they are written in. Alignment is checked, never assumed: the
 * bounce is page aligned, but Storport only guarantees Srb->DataBuffer meets
 * the adapter's ULONG mask, and an unaligned access to uncached memory raises
 * the synchronous alignment abort that cost this project the V16/V20-V23
 * builds.
 *
 * Kept OFF by default. It is an optimisation, and the driver that owns the
 * Windows volume does not take optimisations on prediction - it takes them on
 * the measurement UFS_PERF_INSTRUMENTATION produces.
 */
#ifndef UFS_WIDE_UNCACHED_ACCESS
#define UFS_WIDE_UNCACHED_ACCESS                   1   /* m15 run 24 measurement: per read DEVICE 131 us,
                                                          ZERO 165 us, COPYBACK 286 us - the copies dominate */
#endif

/*
 * m15: pre-zeroing the bounce before every read cost 165 us per read (run 24, more than the device itself).
 * It only matters if the device returns fewer bytes than requested WITHOUT reporting a residual; residuals are
 * checked and counted (FAIL_BY_CAUSE RESID, 0 so far). 0 = skip it.
 */
/*
 * m15 w34: read copy-back through a CACHEABLE second view of the bounce pages (MmMapIoSpaceEx PAGE_READWRITE)
 * instead of the Device-memory view. Coherence by hand: the CPU never writes through this view, so its lines
 * are never dirty; after each read's DMA completes the transferred range gets DC CIVAC (= invalidate for clean
 * lines, line size from CTR_EL0) between DSBs, then a normal cached copy. Writes and command buffers keep the
 * Device-memory view, so a maintenance mistake can only return a stale READ, never write stale data to disk.
 * Encodings of DC CIVAC / CTR_EL0 for MSVC __sys/_ReadStatusReg verified by disassembly (notes/m15-perf.md).
 */
/* SYS #op1, Cn, Cm, #op2 operand for MSVC __sys(); (3,7,14,1) = DC CIVAC - verified by dumpbin. */
#define UFS_SYSINSTR(op1, crn, crm, op2)  (((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))

#ifndef UFS_CACHED_BOUNCE_READ
#define UFS_CACHED_BOUNCE_READ                     1
#endif

/*
 * m15 w35: with the firmware-reserved workspace (every SRB is copied through our own bounce, never DMA'd from
 * Srb->DataBuffer), tell StorPort the miniport takes 64-bit addresses. With Dma32BitAddresses only, StorPort
 * runs every SRB buffer above 4 GB (most of this phone's RAM) through HAL map registers and splits it: m15 u3
 * (boot #110 + w34 counters) saw 4152 x 64 KiB raw reads become 34057 device commands (~8 KiB each) and
 * ~3.6 ms per 64 KiB, while UEFI reads 64 KiB in 0.73 ms on the same controller. The fallback
 * StorPortGetUncachedExtension path keeps Dma32 (its allocation must stay below 4 GB).
 */
#ifndef UFS_DMA64_WITH_RESERVED_WORKSPACE
#define UFS_DMA64_WITH_RESERVED_WORKSPACE          1
#endif

/* m15 w36: zero the device's VPD 0xB0 OPTIMAL TRANSFER LENGTH (2 blocks) so classpnp stops splitting to 8 KiB. */
#ifndef UFS_CLEAR_VPD_OPTIMAL_TRANSFER
#define UFS_CLEAR_VPD_OPTIMAL_TRANSFER             1
#endif

/* m15 w37: UfsCaptureBounceEvidence only for the first N StartIo requests (0 = every command, the old behaviour). */
#ifndef UFS_BOUNCE_EVIDENCE_COMMANDS
#define UFS_BOUNCE_EVIDENCE_COMMANDS               256
#endif

#ifndef UFS_ZERO_BOUNCE_BEFORE_READ
#define UFS_ZERO_BOUNCE_BEFORE_READ                0
#endif

/*
 * V35 FMP DMA-window enforcement.
 *
 * Unlike the two switches above this is a CORRECTNESS fix, not measurement and
 * not an optimisation, and it is required before DRAM bank1 can be published as
 * conventional memory.
 *
 * The first MR1 boot hung on the Windows logo with no spinner. The firmware
 * side had worked perfectly - PRAM recorded "=MR1 published=000000001ef3c000
 * verdict=FULL", i.e. all 495.2 MiB added - but the driver declares only
 * Dma32BitAddresses=TRUE and takes its DMA workspace from
 * StorPortGetUncachedExtension. "Below 4 GB" was an accurate description of the
 * machine only while the sole conventional memory below 4 GB happened to be the
 * FMP-reachable window. Publishing bank1 made that false, Storport was free to
 * place the UTRL/UCD/bounce where the controller's DMA cannot reach, and
 * storage never initialised.
 *
 * It is a separate switch so it can be proven on its own boot WITHOUT bank1
 * (where it must be a no-op, because every allocation is already in-window),
 * before it is relied on to make MR1 safe.
 */
#ifndef UFS_DMA_WINDOW_ENFORCE
#define UFS_DMA_WINDOW_ENFORCE                     0
#endif

/*
 * Portable kernel contract: only documented kernel interfaces are used.
 * This switch changes neither the diagnostic wire layout nor its version.
 */
#ifndef UFS_PORTABLE_KERNEL_CONTRACT
#define UFS_PORTABLE_KERNEL_CONTRACT               1
#endif
#if (UFS_PORTABLE_KERNEL_CONTRACT != 0) && (UFS_PORTABLE_KERNEL_CONTRACT != 1)
#error UFS_PORTABLE_KERNEL_CONTRACT must be 0 or 1
#endif

#if UFS_PERF_INSTRUMENTATION
#define UFS_DIAG_DATA_VERSION                      0x00150000UL
#else
#define UFS_DIAG_DATA_VERSION                      0x00140000UL
#endif

/*
 * Vendor-specific data-in CDB that returns UFS_DIAGNOSTIC_DATA straight out of
 * the adapter extension. This exists because SRB_FUNCTION_IO_CONTROL proved to
 * be an unreliable diagnostic channel on this stack: the IOCTL round-trips with
 * BYTES=500 and a completely untouched payload, so the handler either never ran
 * or ran against a buffer the caller never sees. SRB_FUNCTION_EXECUTE_SCSI is
 * demonstrably delivered to the miniport, so it is used as the escape hatch.
 *
 * The opcode is in the SCSI vendor-specific range and is answered entirely from
 * memory - it is never encoded into a UPIU and never reaches the device.
 */
#define UFS_DIAG_VENDOR_CDB_OPCODE                 0xD0
#define UFS_DIAG_VENDOR_CDB_SUBCODE                0x9810UL

/*
 * Vendor-specific control CDB that asks the miniport to put the SoC back into
 * recovery (TWRP) using the PMU. Same escape-hatch reasoning as 0xD0: an SRB is
 * the only channel proven to reach this miniport.
 *
 * This is a reset, not an I/O, so it is deliberately harder to hit than the
 * read-only diagnostic: the CDB must carry the subcode AND the confirmation
 * word below, and the driver never issues it on its own. Nothing about it
 * touches the UFS device or its media - it writes two PMU registers only.
 */
#define UFS_DIAG_REBOOT_CDB_OPCODE                 0xD1
#define UFS_DIAG_REBOOT_CDB_SUBCODE                0x9810UL
#define UFS_DIAG_REBOOT_CDB_CONFIRM                0x5245424FUL   /* 'REBO' */

/* Local-only data-in control; never issues media I/O or a reset. */
#define UFS_CLEAN_RECOVERY_OPCODE                  0xD4U
#define UFS_CLEAN_RECOVERY_SUBCODE                 0x9810UL
#define UFS_CLEAN_RECOVERY_CONFIRM                 0x43525354UL   /* 'CRST' */
#define UFS_CLEAN_RECOVERY_SIGNATURE               0x31524355UL   /* 'UCR1' */
#define UFS_CLEAN_RECOVERY_VERSION                 1UL
#define UFS_CLEAN_RECOVERY_BYTES                   32UL
#define UFS_CLEAN_RECOVERY_SECONDS                 300UL
#define UFS_CLEAN_RECOVERY_QUERY                   0UL
#define UFS_CLEAN_RECOVERY_ARM                     1UL
#define UFS_CLEAN_RECOVERY_CANCEL                  2UL
#define UFS_CLEAN_RECOVERY_IDLE                    0UL
#define UFS_CLEAN_RECOVERY_ARMED                   1UL
#define UFS_CLEAN_RECOVERY_COMMITTED               2UL
#define UFS_CLEAN_RECOVERY_OK                      0UL
#define UFS_CLEAN_RECOVERY_NOT_READY               1UL
#define UFS_CLEAN_RECOVERY_BUSY                    2UL
#define UFS_CLEAN_RECOVERY_WRONG_TOKEN             3UL

#pragma pack(push, 4)
typedef struct _UFS_CLEAN_RECOVERY_REPLY {
    ULONG Signature;
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG State;
    ULONG Token;
    ULONG LifetimeSeconds;
    ULONG Ready;
} UFS_CLEAN_RECOVERY_REPLY;
#pragma pack(pop)

/*
 * Vendor-specific note CDB: user mode hands the driver a short reason code plus
 * a 32-bit auxiliary value, and the driver writes it into PRAM.
 *
 * WHY THIS EXISTS. UfsDiag.exe reports through stdout, which the loader
 * redirects to X: - the WinPE RAM disk. The unattended cycle ends in a hard PMU
 * reset, so every one of those logs is destroyed before the host can read it.
 * A helper step that FAILS is therefore completely unobservable, which is
 * exactly the step whose failure reason matters.
 *
 * PRAM survives the reset and is pulled from TWRP, but by the time the helper
 * runs the ring is already full (UFS_PRAM_CAPACITY 0x3F00, ~16 KiB, and the
 * driver's own records fill it), so an appended record is silently dropped.
 * The driver therefore RESERVES a small block of note slots at ring offset 0
 * during UfsPramInitialize and rewrites them in place - see UfsPramNote.
 *
 * Guarded exactly like the reboot and arm CDBs: opcode + subcode + a distinct
 * confirmation word. It touches no register and no media; it only stores bytes
 * into the already-mapped PRAM window. A 16-byte CDB is required because the
 * payload does not fit in 10 (opcode + subcode + confirm already consume 7).
 */
#define UFS_DIAG_NOTE_CDB_OPCODE                   0xD3U
#define UFS_DIAG_NOTE_CDB_SUBCODE                  0x9810UL
#define UFS_DIAG_NOTE_CDB_CONFIRM                  0x4E4F5445UL   /* 'NOTE' */

/*
 * 16, not 13. The tag occupies Cdb[12..15] big-endian, so a 13-byte CDB would
 * satisfy the gate and then be decoded out of three uninitialised bytes.
 * Raising this is a precondition of the four-byte tag, not a separate tidy-up:
 * the two must change together or the driver reads past what the caller set.
 */
#define UFS_DIAG_NOTE_CDB_MIN_LENGTH               16UL

/*
 * Note reason codes. START is emitted on entry so that "the helper never ran at
 * all" is distinguishable from "the helper ran and failed"; its Aux carries
 * GetLogicalDrives(), which answers whether the UFS volume got a drive letter
 * without needing a second channel.
 */
#define UFS_NOTE_REASON_NONE                       0UL
#define UFS_NOTE_REASON_FSWRITE_START              1UL
#define UFS_NOTE_REASON_FSWRITE_NO_VOLUME          2UL
#define UFS_NOTE_REASON_FSWRITE_NO_MEMORY          3UL
#define UFS_NOTE_REASON_FSWRITE_FORMAT             4UL
#define UFS_NOTE_REASON_FSWRITE_CREATE             5UL
#define UFS_NOTE_REASON_FSWRITE_WRITE              6UL
#define UFS_NOTE_REASON_FSWRITE_FLUSH              7UL
#define UFS_NOTE_REASON_FSWRITE_OK                 8UL

/*
 * Disk-survey codes, emitted by UfsDiskSurvey immediately before the volume
 * search. FSWRITE_NO_VOLUME told us only that GetLogicalDrives() returned X:
 * alone; it could not distinguish "partmgr never parsed the table" from
 * "partitions exist but no volume device was created" from "a volume exists
 * with no drive letter". These three notes separate those cases in one boot,
 * which is why the survey runs even when the fs-write itself cannot proceed.
 *
 *   DISK_OPEN_FAIL      Aux = Win32 error from CreateFileW(PhysicalDrive0)
 *   DISK_LAYOUT         Aux = PartitionCount, Tag = PartitionStyle
 *                             (0 = MBR, 1 = GPT, 2 = RAW)
 *   DISK_LAYOUT_FAIL    Aux = Win32 error from GET_DRIVE_LAYOUT_EX
 *   DISK_RESCAN         Aux = PartitionCount after UPDATE_PROPERTIES,
 *                       Tag = Win32 rc of the rescan (0 = it succeeded)
 *   VOLUMES             Aux = volumes enumerated, Tag = how many carry a path
 *   VOLUME_MOUNTED      Aux = drive letter assigned, Tag = volume ordinal
 *   VOLUME_MOUNT_FAIL   Aux = Win32 error, Tag = volume ordinal
 */
#define UFS_NOTE_REASON_DISK_OPEN_FAIL             9UL
#define UFS_NOTE_REASON_DISK_LAYOUT               10UL
#define UFS_NOTE_REASON_DISK_LAYOUT_FAIL          11UL
#define UFS_NOTE_REASON_DISK_RESCAN               12UL
#define UFS_NOTE_REASON_VOLUMES                   13UL
#define UFS_NOTE_REASON_VOLUME_MOUNTED            14UL
#define UFS_NOTE_REASON_VOLUME_MOUNT_FAIL         15UL

/*
 * Stack-locator codes, added after DISK_LAYOUT_FAIL returned ERROR_INVALID_FUNCTION
 * on a boot where 113 SCSI commands completed and the vendor note CDB itself
 * round-tripped through \\.\PhysicalDrive0. That combination proves the disk is
 * ours and the command path is healthy, so the break is somewhere in the disk
 * device stack above us - and a single "the layout IOCTL failed" note cannot say
 * where.
 *
 * These two ask the question the layout IOCTL cannot: which layer still answers.
 * GET_DEVICE_NUMBER is classpnp's, GET_DRIVE_GEOMETRY_EX and GET_LENGTH_INFO are
 * disk.sys's, GET_DRIVE_LAYOUT_EX is partmgr's, and ReadFile traverses the whole
 * stack down into us. Whichever is the first to fail names the missing layer.
 *
 * Fields are packed on nibble/byte/halfword boundaries so the host decoder needs
 * only trivial shifts; this project has a long history of decode defects and
 * cheap unpacking is worth more than the spare bits.
 *
 *   DISK_PROBE   Aux = (Flags << 24) | (Win32 of first failure & 0xFFFF)
 *                      Flags b0 GET_DEVICE_NUMBER ok
 *                            b1 GET_DRIVE_GEOMETRY_EX ok
 *                            b2 GET_LENGTH_INFO ok
 *                            b3 reported size matches this UFS device
 *                Tag = (SizeMiB & 0xFFFF) << 16 | (BytesPerSector & 0xFFFF)
 *
 *   DISK_READ    Aux = (Flags << 24) | (Win32 of the failing step & 0xFFFF)
 *                      Flags b0 SetFilePointerEx ok
 *                            b1 ReadFile ok
 *                            b2 "EFI PART" present at LBA 1
 *                            b3 LBA 0 carries a 0x55AA boot signature
 *                Tag = bytes actually returned by ReadFile
 *
 *   DISK_MODULES Aux = bitmask of kernel modules found resident
 *                      b0 partmgr.sys   b1 disk.sys     b2 classpnp.sys
 *                      b3 volmgr.sys    b4 volsnap.sys  b5 mountmgr.sys
 *                      b6 storport.sys  b7 Exynos9820Ufs.sys
 *                Tag = total loaded module count, or 0 if the query failed
 *
 * DISK_MODULES is the discriminator the offline analysis could not supply.
 * partmgr is registered correctly (Services\partmgr Start=0, the DiskDrive
 * class key carries UpperFilters=partmgr, and DriverDatabase binds GenDisk to
 * disk.inf), and partmgr's own GET_DRIVE_LAYOUT_EX handler can only fail with
 * STATUS_BUFFER_TOO_SMALL or STATUS_INTEGER_OVERFLOW - never the observed
 * ERROR_INVALID_FUNCTION. So the IOCTL is being answered by the classpnp/
 * disk.sys default case and partmgr is not in the stack. Whether that is
 * because the driver never loaded at all or because it loaded and failed to
 * attach is not answerable from the binary or the hive, only from the running
 * kernel.
 */
#define UFS_NOTE_REASON_DISK_PROBE                16UL
#define UFS_NOTE_REASON_DISK_READ                 17UL
#define UFS_NOTE_REASON_DISK_MODULES              18UL

/*
 * 19 - DISK_STACK. partmgr is resident (18 measured mask 0xFF) yet every
 * partmgr IOCTL returns ERROR_INVALID_FUNCTION, so it loaded without attaching
 * to our devnode. Only two things decide whether PnP inserts a class upper
 * filter: the class key's UpperFilters value, and the devnode's own ClassGUID.
 * This note reports both, measured live rather than from the offline hive, plus
 * the devnode's CM problem code.
 *
 * Aux packs the flag set (low 16), partmgr's Start value (bits 16-23) and the
 * number of disk devnodes found (bits 24-31). Tag packs DN_ status in the high
 * half and the CM_PROB_ code in the low half.
 */
#define UFS_NOTE_REASON_DISK_STACK                19UL

/*
 * NT object-manager view of \Device\Harddisk0.  This is the only measurement
 * that answers "is partmgr attached to this devnode" directly rather than by
 * inference: disk.sys creates \DR0, partmgr creates \Partition0.  Static
 * analysis of the storage drivers cannot answer it - their IRP_MJ_DEVICE_CONTROL
 * handlers switch on the IOCTL *function number* through a compiler-generated
 * jump table, so the full 32-bit CTL_CODE is never stored as a constant and
 * never materialises in a register.
 *
 * Aux bits 0-7 flags: 0x01 ntdll resolved, 0x02 directory opened,
 * 0x04 query returned at least one entry, 0x08 DR0 present,
 * 0x10 Partition0 present, 0x20 Partition1+ present, 0x40 enumeration
 * truncated at the iteration bound.  Aux bits 8-15 hold the total entry count
 * (clamped to 255) and bits 16-23 the highest PartitionN index seen (clamped
 * to 255).  Tag is the NTSTATUS of the failing open/query, 0 on success.
 */
#define UFS_NOTE_REASON_DISK_OBJDIR               20UL

/*
 * Sweep of 14 disk/storage IOCTLs on one already-open PhysicalDrive0 handle,
 * so a single note gives complete per-IOCTL accounting instead of one
 * first-failure code.  The legacy-vs-EX pairs (bits 5/6 GET_PARTITION_INFO and
 * bits 7/8 GET_DRIVE_LAYOUT) are the discriminator: a split there localises the
 * failure to one layer.  Aux is the success bitmask; tag's low half is the
 * "returned exactly ERROR_INVALID_FUNCTION" bitmask and its high half the
 * "returned some other error" bitmask, so success|invfunc|other covers every
 * bit.  IOCTL_DISK_ARE_VOLUMES_READY is deliberately excluded - it can block.
 */
#define UFS_NOTE_REASON_DISK_IOCTLS               21UL

/*
 * STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR.  4Kn is the one dimension of the
 * no-volume failure never investigated.  Both sector sizes are powers of two
 * <= 65536 so they fit in 16 bits each: aux is
 * (BytesPerPhysicalSector << 16) | BytesPerLogicalSector, tag is
 * (Win32Error << 16) | BytesOffsetForSectorAlignment with Win32Error 0 on
 * success.
 */
#define UFS_NOTE_REASON_DISK_ALIGN                22UL

/*
 * The payload reached disk but the post-payload diagnostic snapshot was not
 * valid, so no durable command-volume verdict can be trusted. This was added
 * after the disk-survey ABI; use the next free code rather than inserting it
 * into the historical 1..8 fs-write range and reattributing archived notes.
 */
#define UFS_NOTE_REASON_FSWRITE_DIAG              23UL

/*
 * Filesystem-semantics volume closure. NAUX is the exact Win32 error and NTAG
 * packs drive-letter ASCII in bits 0..7, attempted in bit 8, and succeeded in
 * bit 9. Four distinct reasons prevent one failed stage from overwriting the
 * state of another.
 */
#define UFS_NOTE_REASON_FSCLOSE_VOLUME_OPEN       24UL
#define UFS_NOTE_REASON_FSCLOSE_VOLUME_FLUSH      25UL
#define UFS_NOTE_REASON_FSCLOSE_VOLUME_LOCK       26UL
#define UFS_NOTE_REASON_FSCLOSE_VOLUME_DISMOUNT   27UL


/* Which UfsExecuteScsi early return fired last. */
#define UFS_EXEC_REJECT_NONE                       0UL
#define UFS_EXEC_REJECT_DIAGNOSTIC_ONLY            1UL
#define UFS_EXEC_REJECT_NOT_STARTED                2UL
#define UFS_EXEC_REJECT_FATAL_ERROR                3UL
#define UFS_EXEC_REJECT_BAD_NEXUS                  4UL
#define UFS_EXEC_REJECT_CLASSIFY                   5UL
#define UFS_EXEC_REJECT_REARM_EXHAUSTED            6UL
#define UFS_EXEC_REJECT_REARM_FAILED               7UL
#define UFS_EXEC_REJECT_PRECONDITION               8UL
#define UFS_EXEC_REJECT_LOCAL_COMPLETE             9UL
#define UFS_EXEC_REJECT_ISSUED                     10UL
#define UFS_EXEC_REJECT_REARM_BUSY                 11UL
#define UFS_EXEC_REJECT_WRITE_DISARMED             12UL
#define UFS_EXEC_REJECT_WRITE_FENCE                13UL
#define UFS_EXEC_REJECT_WRITE_DRY_RUN              14UL
#define UFS_EXEC_REJECT_WRITE_VERIFY               15UL
#define UFS_EXEC_REJECT_WRITE_CRASH_LOCKOUT        16UL

/*
 * WRITE-PATH PHASE CODES.
 *
 * WriteCrashAttempt names which write was in flight; this names where in the
 * write path it was. Written to a dedicated PRAM word on entry to each candidate
 * region, so a boot that bugchecks reports its own location on the next boot.
 *
 * Gaps between groups are intentional: they leave room to subdivide a region
 * without renumbering codes a previous capture already used.
 */
#define UFS_PHASE_IDLE                     0UL   /* never entered the write path */
#define UFS_PHASE_GATE_ENTERED             1UL   /* data-out gate, before the latch */
#define UFS_PHASE_CRASH_LOCKOUT            2UL   /* refused by the crash lockout */
#define UFS_PHASE_LATCH_SET                3UL   /* latch armed, before mode dispatch */
#define UFS_PHASE_DISARMED_PRE             4UL   /* DISARMED reject, before snapshot */
#define UFS_PHASE_DISARMED_POST            5UL   /* DISARMED reject, after snapshot */
#define UFS_PHASE_DRY_RUN_PRE              6UL   /* DRY_RUN, before snapshot */
#define UFS_PHASE_DRY_RUN_POST             7UL   /* DRY_RUN, after snapshot */
#define UFS_PHASE_LIVE_ENTERED             8UL   /* LIVE path (not reachable yet) */
#define UFS_PHASE_ARM_ENTER                10UL  /* vendor arm CDB, on entry */
#define UFS_PHASE_ARM_MODE_SET             11UL  /* arm CDB, mode assigned */
#define UFS_PHASE_ARM_PRE_SNAPSHOT         12UL  /* arm CDB, before snapshot */
#define UFS_PHASE_ARM_RETURNING            13UL  /* arm CDB, about to return */
#define UFS_PHASE_CLASSIFY_REJECT          20UL  /* write opcode refused by classify */
#define UFS_PHASE_VENDOR_DIAG              30UL  /* vendor diag CDB 0xD0 */
/*
 * Fine subdivision of the LIVE write path, added for the full-Windows build.
 *
 * UFS_PHASE_LIVE_ENTERED alone was too coarse to be useful the first time a
 * write actually reached hardware: it says the gate was passed and nothing
 * more, so a bugcheck anywhere between the gate and the completion looks
 * identical. These codes bracket every step that a write executes for the
 * first time, so the boot AFTER a crash names the step rather than the region.
 *
 * Only the data-out path sets them, so the proven read path is unchanged and
 * pays nothing. Each is a single naturally aligned 32-bit store to the same
 * PRAM word the coarse codes already use.
 */
#define UFS_PHASE_LIVE_PRECHECK            40UL  /* preconditions passed, pre-build */
#define UFS_PHASE_LIVE_BUILT               41UL  /* descriptor + payload staged */
#define UFS_PHASE_LIVE_NEXUS               42UL  /* vendor nexus programmed */
#define UFS_PHASE_LIVE_DOORBELL            43UL  /* doorbell rung, DMA in flight */
#define UFS_PHASE_LIVE_POLLED              44UL  /* doorbell cleared */
#define UFS_PHASE_LIVE_ACKED               45UL  /* interrupt status acknowledged */
#define UFS_PHASE_LIVE_FINISHED            46UL  /* response UPIU parsed */
#define UFS_PHASE_LIVE_VERIFIED            47UL  /* read-back verify returned */
#define UFS_PHASE_LIVE_COMPLETE            48UL  /* write path returning cleanly */
#define UFS_PHASE_VENDOR_REBOOT            31UL  /* vendor reboot CDB */
#define UFS_PHASE_VENDOR_NOTE              32UL  /* vendor note CDB 0xD3 */

/*
 * Write arming escape hatch, guarded exactly like the reboot CDB: a dedicated
 * opcode, a subcode, and a distinct confirmation word per mode. Arming is
 * deliberately a runtime act rather than a build-time flag so that a flashed
 * image is inert until something explicitly asks for writes, and so that the
 * dry-run and live steps are two separate, individually auditable decisions.
 */
#define UFS_DIAG_WRITE_ARM_CDB_OPCODE              0xD2U
#define UFS_DIAG_WRITE_ARM_CDB_SUBCODE             0x9810UL
#define UFS_DIAG_WRITE_ARM_CONFIRM_DRY_RUN         0x44525955UL   /* 'DRYU' */
#define UFS_DIAG_WRITE_ARM_CONFIRM_LIVE            0x4C495645UL   /* 'LIVE' */
#define UFS_DIAG_WRITE_ARM_CONFIRM_DISARM          0x53414645UL   /* 'SAFE' */

#define UFS_WRITE_MODE_DISARMED                    0UL
#define UFS_WRITE_MODE_DRY_RUN                     1UL
#define UFS_WRITE_MODE_LIVE                        2UL

/*
 * Why a command that reached hardware did not return SRB_STATUS_SUCCESS.
 * UfsFinishScsiCommand used to fold the first five of these into one opaque
 * RESPONSE_FRAMING bit, which is why V14 could say a response was malformed but
 * not in which field.
 */
#define UFS_FAIL_CAUSE_NONE                        0UL
#define UFS_FAIL_CAUSE_OCS                         1UL
#define UFS_FAIL_CAUSE_TRANSACTION_CODE            2UL
#define UFS_FAIL_CAUSE_LUN                         3UL
#define UFS_FAIL_CAUSE_TASK_TAG                    4UL
#define UFS_FAIL_CAUSE_RESPONSE                    5UL
#define UFS_FAIL_CAUSE_RESIDUAL                    6UL
#define UFS_FAIL_CAUSE_CHECK_CONDITION             7UL
#define UFS_FAIL_CAUSE_SCSI_STATUS                 8UL
#define UFS_FAIL_CAUSE_DATA_OVERRUN                9UL

#define UFS_DIAG_SRB_SIGNATURE                     "EXYNUFS9"
#define UFS_DIAG_SRB_SIGNATURE_LENGTH              8UL
#define UFS_DIAG_CONTROL_CODE                      0xE9810009UL
#define UFS_DIAG_RETURN_SUCCESS                     0UL
#define UFS_DIAG_RETURN_INVALID_REQUEST             1UL
#define UFS_DIAG_MAX_ACCESS_RANGES                 8UL

/*
 * Bytes of the last data-in transfer mirrored into telemetry. Part of the
 * wire format, so it lives here rather than in the driver-private header:
 * UfsDiag.exe decodes the same struct.
 */
#define UFS_LAST_DATA_PREFIX                       16UL

/*
 * How many DISTINCT refused opcodes to remember. Eight covers the whole
 * bring-up probe sequence classpnp/disk/partmgr issue against a new disk, and
 * packs into exactly one 64-bit PRAM field.
 */
#define UFS_REJECTED_OPCODE_SLOTS                  8UL

/* Fixed-format sense data: response code + 17 bytes, ASC/ASCQ at 12/13. */
#define UFS_FIXED_SENSE_LENGTH                     18UL
#define UFS_SENSE_KEY_ILLEGAL_REQUEST              0x05
#define UFS_SENSE_KEY_DATA_PROTECT                 0x07
#define UFS_ASC_INVALID_COMMAND_OPERATION_CODE     0x20
#define UFS_ASC_WRITE_PROTECTED                    0x27

#define UFS_DIAG_CONFIG_SCATTER_GATHER             (1UL << 0)
#define UFS_DIAG_CONFIG_MASTER                     (1UL << 1)
#define UFS_DIAG_CONFIG_NEED_PHYSICAL              (1UL << 2)
#define UFS_DIAG_CONFIG_TAGGED_QUEUING              (1UL << 3)
#define UFS_DIAG_CONFIG_AUTO_REQUEST_SENSE          (1UL << 4)
#define UFS_DIAG_CONFIG_MULTIPLE_REQUESTS           (1UL << 5)

#define UFS_DIAG_INIT_HW_INTERRUPT                 (1UL << 0)
#define UFS_DIAG_INIT_NEED_PHYSICAL                (1UL << 1)
#define UFS_DIAG_INIT_TAGGED_QUEUING               (1UL << 2)
#define UFS_DIAG_INIT_AUTO_REQUEST_SENSE           (1UL << 3)
#define UFS_DIAG_INIT_MULTIPLE_REQUESTS            (1UL << 4)

#define UFS_DIAG_STAGE_NONE                        0UL
#define UFS_DIAG_STAGE_FIND_ADAPTER_ENTERED        1UL
#define UFS_DIAG_STAGE_RESOURCES_CAPTURED          2UL
#define UFS_DIAG_STAGE_RESOURCES_MAPPED            3UL
#define UFS_DIAG_STAGE_WORKSPACE_ALLOCATED         4UL
#define UFS_DIAG_STAGE_FIND_WARM_VALIDATED         5UL
#define UFS_DIAG_STAGE_FIND_ADAPTER_COMPLETE       6UL
#define UFS_DIAG_STAGE_HW_INITIALIZE_ENTERED       7UL
#define UFS_DIAG_STAGE_INIT_WARM_VALIDATED         8UL
#define UFS_DIAG_STAGE_TRANSFER_LIST_PROGRAMMED    9UL
#define UFS_DIAG_STAGE_OPERATIONAL                 10UL
#define UFS_DIAG_STAGE_DIAGNOSTIC_ONLY             11UL
#define UFS_DIAG_STAGE_UNEXPECTED_INTERRUPT        12UL

#define UFS_DIAG_FAILURE_ACCESS_RANGES_NULL        (1ULL << 0)
#define UFS_DIAG_FAILURE_HCI_RANGE_MISSING         (1ULL << 1)
#define UFS_DIAG_FAILURE_UNIPRO_RANGE_MISSING      (1ULL << 2)
#define UFS_DIAG_FAILURE_PMA_RANGE_MISSING         (1ULL << 3)
#define UFS_DIAG_FAILURE_UFSP_RANGE_MISSING        (1ULL << 4)
#define UFS_DIAG_FAILURE_HCI_MAP                   (1ULL << 5)
#define UFS_DIAG_FAILURE_WORKSPACE_ALLOCATION      (1ULL << 6)
#define UFS_DIAG_FAILURE_WORKSPACE_LAYOUT          (1ULL << 7)
#define UFS_DIAG_FAILURE_UTRL_PHYSICAL             (1ULL << 8)
#define UFS_DIAG_FAILURE_UCD_PHYSICAL              (1ULL << 9)
#define UFS_DIAG_FAILURE_BOUNCE_PHYSICAL           (1ULL << 10)
#define UFS_DIAG_FAILURE_HOST_DISABLED             (1ULL << 11)
#define UFS_DIAG_FAILURE_HOST_STATUS               (1ULL << 12)
#define UFS_DIAG_FAILURE_DOORBELL_BUSY              (1ULL << 13)
#define UFS_DIAG_FAILURE_INTERRUPT_AGGREGATION     (1ULL << 14)
#define UFS_DIAG_FAILURE_INTERRUPT_ENABLE          (1ULL << 15)
#define UFS_DIAG_FAILURE_CAPABILITIES              (1ULL << 16)
#define UFS_DIAG_FAILURE_TX_PRDT_SIZE               (1ULL << 17)
#define UFS_DIAG_FAILURE_RX_PRDT_SIZE               (1ULL << 18)
#define UFS_DIAG_FAILURE_DATA_REORDER               (1ULL << 19)
#define UFS_DIAG_FAILURE_AXI_DMA_BURST              (1ULL << 20)
#define UFS_DIAG_FAILURE_DMA_ADDRESS_WIDTH          (1ULL << 21)
#define UFS_DIAG_FAILURE_PROGRAM_PRECONDITION       (1ULL << 22)
#define UFS_DIAG_FAILURE_PROGRAM_DOORBELL_BUSY      (1ULL << 23)
#define UFS_DIAG_FAILURE_PROGRAM_BASE_LOW           (1ULL << 24)
#define UFS_DIAG_FAILURE_PROGRAM_BASE_HIGH          (1ULL << 25)
#define UFS_DIAG_FAILURE_PROGRAM_RUN_STOP           (1ULL << 26)
#define UFS_DIAG_FAILURE_UNEXPECTED_INTERRUPT        (1ULL << 27)
#define UFS_DIAG_FAILURE_INTERRUPT_MASK              (1ULL << 28)
#define UFS_DIAG_FAILURE_UCD_VIRTUAL_ALIGNMENT       (1ULL << 29)
#define UFS_DIAG_FAILURE_DMA_ABOVE_4G                 (1ULL << 30)
#define UFS_DIAG_FAILURE_HCI_VERSION                  (1ULL << 31)
#define UFS_DIAG_FAILURE_INTERRUPT_GATE               (1ULL << 32)
#define UFS_DIAG_FAILURE_INTERRUPT_STATUS             (1ULL << 33)
#define UFS_DIAG_FAILURE_NEXUS_PROGRAM                (1ULL << 34)
#define UFS_DIAG_FAILURE_NEXUS_READBACK               (1ULL << 35)
#define UFS_DIAG_FAILURE_STOP                         (1ULL << 36)
#define UFS_DIAG_FAILURE_RESTORE                      (1ULL << 37)
#define UFS_DIAG_FAILURE_TRANSFER_LENGTH              (1ULL << 38)
#define UFS_DIAG_FAILURE_RESPONSE_FRAMING             (1ULL << 39)
#define UFS_DIAG_FAILURE_TIMEOUT_CONTAINMENT          (1ULL << 40)
#define UFS_DIAG_FAILURE_REARM_FAILED                 (1ULL << 41)
#define UFS_DIAG_FAILURE_REARM_EXHAUSTED              (1ULL << 42)

/*
 * Write-path failures. Each names the layer that refused, so a rejection is
 * attributable to a single guard rather than to "the write failed".
 */
#define UFS_DIAG_FAILURE_WRITE_DISARMED               (1ULL << 43)
#define UFS_DIAG_FAILURE_WRITE_FENCE                  (1ULL << 44)
#define UFS_DIAG_FAILURE_WRITE_GPT_GUARD              (1ULL << 45)
#define UFS_DIAG_FAILURE_WRITE_VERIFY                 (1ULL << 46)

/*
 * A capacity response was refused: wrong transfer length, an unsupported block
 * size, or a block size disagreeing with the one already established for this
 * medium. Any of those means the reply did not land intact, and accepting it
 * would hand Windows a bogus geometry - which on this 4K-native volume presents
 * as UNMOUNTABLE_BOOT_VOLUME rather than as an I/O error.
 */
#define UFS_DIAG_FAILURE_CAPACITY                     (1ULL << 51)

/*
 * A previous boot in this warm-reset chain entered the write path and never
 * reached the closing disarm, i.e. it died mid-probe. Set from the persistent
 * PRAM latch at initialization, and it refuses every write for the whole boot.
 *
 * This is what keeps the unattended loop alive: without it a write that
 * bugchecks WinPE simply repeats forever, the phone never reaches TWRP, and the
 * cycle stalls until a human holds the power button.
 */
#define UFS_DIAG_FAILURE_WRITE_CRASH_LOCKOUT          (1ULL << 47)

/*
 * A refused command was reported as CHECK CONDITION but the sense data could
 * NOT be delivered, so the caller received a bare SRB_STATUS_ERROR instead.
 *
 * This matters because the sense bytes are the whole point of the refusal: they
 * are what tells classpnp/disk.sys/partmgr "this device does not implement that
 * command" rather than "this adapter is broken". Without them the upper stack
 * only sees a failure, and the ERROR_INVALID_FUNCTION that stops partition
 * enumeration can come back.
 *
 * It lives in the failure mask rather than in its own counter deliberately:
 * FMASK is already emitted in every PRAM record, so this costs zero ring bytes
 * on a channel where the final record of every boot is already truncated.
 */
#define UFS_DIAG_FAILURE_SENSE_UNAVAILABLE            (1ULL << 48)

/*
 * The controller declared the transfer failed (OCS != success) but the device
 * nevertheless returned a structurally valid response UPIU carrying its own
 * non-GOOD SCSI status, and that answer was handed to the caller instead of a
 * bare adapter error.
 *
 * The observed case is MODE SENSE(6) for the caching page: the device replies
 * CHECK CONDITION / ILLEGAL REQUEST / INVALID FIELD IN CDB - "ask me with MODE
 * SENSE(10)" - while the controller simultaneously reports OCS=0x07. Before
 * this bit existed the OCS gate ran first and threw the entire response away,
 * so classpnp saw an unclassifiable device error, retried, and failed the IRP
 * with STATUS_IO_DEVICE_ERROR. That is the ERROR_IO_DEVICE 1117 on every layout
 * IOCTL, and it is why partmgr produced zero partitions for a disk whose media
 * and GPT both verified perfectly.
 *
 * The bit marks the BRANCH, not a fault: it is the difference between "the fix
 * ran and did not help" and "the fix never ran". Without it those two outcomes
 * are indistinguishable on the next boot, which is the exact defect class this
 * project keeps paying for.
 *
 * Like bit 48 it lives in the failure mask rather than in its own counter or
 * PRAM field: FMASK is one fixed-width field already emitted in every record,
 * so this costs zero ring bytes on a channel whose final record is already
 * truncated.
 */
#define UFS_DIAG_FAILURE_OCS_WITH_DEVICE_STATUS       (1ULL << 49)

/*
 * A write was aimed at a real partition other than the one Windows owns -
 * sda19 VENDOR through sda25 USERDATA. Distinct from WRITE_FENCE because the
 * two answer different questions: FENCE means "outside the window we allow at
 * all", PROTECTED_PARTITION means "inside that window, but on top of Samsung
 * data". The fence was widened to reach the unallocated tail past the last
 * partition, and this bit is what keeps that widening from being a licence to
 * write over everything in between.
 *
 * Adding a bit value does not change sizeof(UFS_DIAGNOSTIC_DATA), so this is
 * not an ABI break and UFS_DIAG_DATA_VERSION is unchanged.
 */
#define UFS_DIAG_FAILURE_WRITE_PROTECTED_PARTITION    (1ULL << 50)

/*
 * PRIVATE_TELEMETRY_DISABLED and BUGCHECK_PRAM_UNAVAILABLE are informational,
 * not storage-fatal conditions. Historical exact/zero-mask admission gates
 * intentionally do not qualify the portable candidate.
 */
#define UFS_DIAG_FAILURE_PRIVATE_TELEMETRY_DISABLED    (1ULL << 52)
#define UFS_DIAG_FAILURE_BUGCHECK_INIT                 (1ULL << 53)
#define UFS_DIAG_FAILURE_BUGCHECK_DATA                 (1ULL << 54)
#define UFS_DIAG_FAILURE_BUGCHECK_REGISTER             (1ULL << 55)
#define UFS_DIAG_FAILURE_BUGCHECK_PRAM_UNAVAILABLE      (1ULL << 56)

/*
 * The S10e's UFS HCI belongs to a newer revision class than the 9810's: its
 * device tree declares `hw-rev = UFS_VER_0006` (include/dt-bindings/ufs/ufs.h
 * defines UFS_VER_0004 = 4, UFS_VER_0005 = 5, UFS_VER_0006 = 6), while the
 * 9810 kernel accepted 4 and 5. The HCI register encoding that class implies
 * has never been read on Windows, so the version check is REPORTED here and
 * deliberately not folded into UFS_DIAG_FAILURE_HCI_VERSION, which invalidates
 * the warm state and would refuse storage on a value we have not measured.
 */
#define UFS_DIAG_FAILURE_HCI_VERSION_UNCONFIRMED         (1ULL << 57)

#pragma pack(push, 1)

typedef struct _UFS_DIAGNOSTIC_RANGE {
    ULONGLONG Start;
    ULONG Length;
    ULONG InMemory;
} UFS_DIAGNOSTIC_RANGE, *PUFS_DIAGNOSTIC_RANGE;

typedef struct _UFS_DIAGNOSTIC_DATA {
    ULONG Signature;
    ULONG Version;
    ULONG Size;
    ULONG Stage;
    ULONG FailureStage;
    ULONGLONG FailureMask;

    ULONG AdapterInterfaceType;
    ULONG SystemIoBusNumber;
    ULONG NumberOfAccessRanges;
    ULONG CapturedAccessRanges;
    UFS_DIAGNOSTIC_RANGE AccessRanges[UFS_DIAG_MAX_ACCESS_RANGES];

    ULONGLONG HciVirtual;
    ULONGLONG WorkspaceVirtual;
    ULONG WorkspaceSize;
    ULONGLONG UtrlVirtual;
    ULONGLONG UtrlPhysical;
    ULONGLONG UcdVirtual;
    ULONGLONG UcdPhysical;
    ULONGLONG BounceVirtual;
    ULONGLONG BouncePhysical;

    ULONG Dma64BitAddresses;
    ULONG Dma32BitAddresses;
    ULONG MaximumTransferLength;
    ULONG NumberOfPhysicalBreaks;
    ULONG InitializationCapabilities;
    ULONG IncomingConfigCapabilities;
    ULONG FinalConfigCapabilities;
    ULONG MaximumNumberOfTargets;
    ULONG MaximumNumberOfLogicalUnits;
    ULONG InitiatorBusId;
    ULONG MaxNumberOfIo;
    ULONG MaxIosPerLun;
    ULONG InitialLunQueueDepth;
    ULONG InterruptSynchronizationMode;
    ULONG InterruptCallbacks;
    ULONG UtrdInterruptRequested;
    ULONG InterruptStatus;
    ULONG InterruptEnableAfterIsr;

    ULONG HostEnable;
    ULONG Capabilities;
    ULONG HostStatus;
    ULONG InterruptEnable;
    ULONG InterruptAggregation;
    ULONG Doorbell;
    ULONG UtrlBaseLow;
    ULONG UtrlBaseHigh;
    ULONG UtrlRunStop;
    ULONG NexusType;
    ULONG TxPrdtSize;
    ULONG RxPrdtSize;
    ULONG DataReorder;
    ULONG AxiDmaBurst;

    ULONG ProgramBaseLowExpected;
    ULONG ProgramBaseHighExpected;
    ULONG ProgramBaseLowReadback;
    ULONG ProgramBaseHighReadback;
    ULONG ProgramRunStopReadback;

    ULONG ResourcesMapped;
    ULONG WorkspaceAllocated;
    ULONG WarmStateValid;
    ULONG DiagnosticOnly;
    ULONG Started;
    ULONG Operational;
    ULONG CompletedCommands;
    ULONG RejectedCommands;
    ULONG FailedCommands;
    ULONG ContainedCommands;
    ULONG WriteProtectedResponses;
    ULONG LastOpcode;
    ULONG LastOcs;
    ULONGLONG LastLogicalBlock;
    ULONG LogicalBlockSize;
    ULONG CapacityValid;
    ULONG OwnsTransferList;
    ULONG InterruptsGated;
    ULONG DiagnosticEndpointPreserved;
    ULONG FatalError;
    /*
     * V14 observability tail. Appended, so every offset above is unchanged;
     * only Size and Version move. Driver and UfsDiag.exe are always built and
     * shipped together from this header, and StatusBoard parses UfsDiag's text
     * output rather than this struct, so growing the tail is safe.
     */
    ULONG RearmAttempts;
    ULONG StartIoRequests;
    ULONG LastSrbFunction;
    ULONG ExecuteScsiRequests;
    ULONG LastExecuteReject;
    ULONG IoControlRequests;
    ULONG IoControlRejects;
    ULONG LastIoControlBufferPresent;
    ULONG LastIoControlLength;
    ULONG LastIoControlHeaderLength;
    ULONG LastIoControlCode;
    ULONG LastIoControlSignatureOk;
    ULONG VendorDiagRequests;
    ULONG HwInitializeCalls;

    //
    // V15: persistent-RAM breadcrumb channel. PramMapped is 1 only when the
    // 0xFED14000 window mapped AND read back the magic it was given, so a 0
    // here means the boot's telemetry exists only on screen.
    //
    ULONG PramMapped;
    ULONG PramRecords;
    ULONGLONG PramVirtual;

    /*
     * V15 failure forensics.
     *
     * V14 proved the command path works - 68 commands round-tripped through the
     * controller and returned the device's real 4096 x 15,616,000 geometry - and
     * that the adapter dies from error handling rather than from I/O. But it
     * could only report the LAST event, and by then sixteen containments had
     * already latched the adapter fatal, so the failure that started the cascade
     * was invisible.
     *
     * So freeze the state at the FIRST failed command and never overwrite it,
     * and tally each cause separately. One repeated cause and one isolated
     * cause look identical in a LAST_* field and completely different here.
     */
    ULONG FramingOcsFailures;
    ULONG FramingTransactionFailures;
    ULONG FramingLunFailures;
    ULONG FramingTagFailures;
    ULONG FramingResponseFailures;
    ULONG FramingResidualFailures;
    ULONG CheckConditionFailures;
    ULONG ScsiStatusFailures;
    ULONG DataOverrunFailures;
    ULONG RearmSuccesses;
    ULONG RearmBusyRejects;
    ULONG ConsecutiveFailures;
    ULONG MaxConsecutiveFailures;

    ULONG FirstFailureValid;
    ULONG FirstFailureCause;
    ULONG FirstFailureCommandIndex;
    ULONG FirstFailureOpcode;
    ULONG FirstFailureDataLength;
    ULONG FirstFailureResidual;
    ULONG FirstFailureOcs;
    ULONG FirstFailureTransactionCode;
    ULONG FirstFailureLun;
    ULONG FirstFailureTaskTag;
    ULONG FirstFailureResponse;
    ULONG FirstFailureStatus;
    ULONG FirstFailureSenseKey;
    ULONG FirstFailureAsc;
    ULONG FirstFailureAscq;
    ULONG FirstFailureInterruptStatus;
    ULONG FirstFailureHostStatus;
    ULONG FirstFailureDoorbell;
    UCHAR FirstFailureCdb[16];

    ULONG LastFailureCause;
    ULONG LastResidual;
    ULONG LastResponseStatus;
    ULONG LastTransactionCode;

    /*
     * V17 unattended-loop tail.
     *
     * PmuMapped/RebootRequests describe the PMU window the driver uses to put
     * the phone back in TWRP without a human holding buttons.
     *
     * LastDataIn* is the reason this build exists at all: V15 and V16 both
     * reported READ(10) 4096 completing with OCS=0 and residual 0 while the
     * buffer stayed zero, and that fact was only ever visible in a photograph
     * of the screen. Mirroring the prefix here (and into PRAM) makes "did the
     * DMA land" answerable from the host.
     */
    ULONG PmuMapped;
    ULONG RebootRequests;
    ULONG LastDataInLength;
    ULONG LastDataInNonZero;
    UCHAR LastDataIn[UFS_LAST_DATA_PREFIX];

    /*
     * V19 write-enablement observability.
     *
     * Writes are the one operation on this controller that can destroy the
     * device, so every attempt is counted by outcome rather than folded into
     * the generic command counters. WritesFenced/WritesGuarded being non-zero
     * with WritesIssued at zero is the signature of the fence doing its job.
     *
     * The fence bounds are reported rather than assumed so a flashed image can
     * be checked against the partition table it was built for.
     */
    ULONG WritesArmed;
    ULONG WriteMode;
    ULONG WriteFenceFirstLba;
    ULONG WriteFenceLastLba;
    ULONG WriteArmRequests;
    ULONG WritesAttempted;
    ULONG WritesFenced;
    ULONG WritesGuarded;
    ULONG WritesDisarmedRejects;
    ULONG WritesDryRun;
    ULONG WritesIssued;
    ULONG WritesVerified;
    ULONG WriteVerifyFailures;
    ULONG LastWriteLba;
    ULONG LastWriteBlocks;
    ULONG LastWriteResult;

    /*
     * Warm-reset crash latch (see UFS_DIAG_FAILURE_WRITE_CRASH_LOCKOUT).
     *
     * PRAM ring records do NOT survive a reboot - UEFI rewinds the cursor and
     * overwrites the first ~13.8 KB with its own breadcrumbs long before this
     * driver loads. The PRAM *header* word at +0x04 does survive, because UEFI
     * only clears it when the magic is absent, i.e. on a cold boot. These four
     * fields are that word, decoded, and they are the only evidence that
     * outlives a bugcheck.
     *
     * WriteCrashAttempt is the 1-based index of the write the previous boot was
     * on when it died, so a crash names its own trigger without a debugger.
     */
    ULONG WriteCrashLockout;
    ULONG WriteCrashAttempt;
    ULONG WriteBootEpoch;
    ULONG WriteProbeCompleted;

    /*
     * Where in the write path the previous boot was when it died, decoded from
     * the PRAM phase breadcrumb (UFS_PHASE_* in Exynos9820Ufs.h).
     *
     * WriteCrashAttempt says which write; this says which line. Together they
     * identify a bugcheck without a debugger, which is the only option here
     * because a crashing boot never reaches TWRP to be attached to.
     */
    ULONG WriteCrashPhase;

    /*
     * Value of the firmware's boot-attempt counter as this driver found it, before
     * clearing it (see UfsPramAcknowledgeBoot). 1 is the healthy steady state: this boot
     * was the first attempt since the last acknowledgement. 2 or more means that many
     * preceding boots never reached miniport initialisation, and the firmware was that
     * many short of diverting the phone to TWRP on its own.
     *
     * 0 means the firmware does not implement the counter, so nothing was acknowledged.
     */
    ULONG BootAttemptsAcknowledged;

    /*
     * Hardware-watchdog servicing state.
     *
     * WdtArmed is set when Windows inherits an armed firmware bridge or on the
     * first runtime servicing tick.  The bridge path leaves it set after the
     * hardware is stopped so the retained diagnostic proves that the bridge was
     * live and Windows retired it; the adapter's private WdtArmed flag carries
     * the current post-disarm state.
     *
     * WdtTicksPerSecond is measured on device rather than assumed. The firmware's
     * own register programming implies roughly 30 s to expiry, but its comment
     * explaining why the watchdog is disabled says an unserviced watchdog "resets
     * a perfectly healthy Windows boot a few seconds after handoff". Those two
     * disagree by an order of magnitude, and the difference decides whether a
     * firmware-side arm across ExitBootServices would be a safety net or a boot
     * loop. Reloading to a known value on every pet makes the residual count a
     * direct reading of the tick rate.
     */
    ULONG WdtArmed;
    ULONG WdtPets;
    ULONG WdtLastCount;
    ULONG WdtTicksPerSecond;
    /*
     * Set when the first measurement came back below UFS_WDT_MIN_TIMEOUT_SEC and
     * the watchdog was disarmed rather than left running with a period too short
     * to pet reliably. Distinguishes "never armed" from "armed, measured, and
     * deliberately stood down" - which are the same WdtArmed=0 otherwise.
     */
    ULONG WdtDisarmedTooFast;
    /*
     * Vendor note CDB (0xD3) accounting. NoteRequests counts every accepted
     * note; LastNoteCode/LastNoteAux mirror the most recent one so that a boot
     * whose PRAM capture is lost still reports the helper's verdict through the
     * ordinary diagnostic read.
     *
     * PramNotesWritten is the driver-side slot cursor and is deliberately
     * separate from NoteRequests: once every slot is spent the driver keeps
     * rewriting the last one, so the two diverge exactly when notes were
     * dropped, which is the only way to tell a truncated sequence from a
     * complete one.
     */
    ULONG NoteRequests;
    ULONG LastNoteCode;
    ULONG LastNoteAux;
    ULONG PramNotesWritten;
    /*
     * MODE SENSE replies that would have carried the write-protect bit under
     * the old always-assert behaviour. Non-zero proves the suppression is live,
     * which distinguishes "Windows never attempted a write" from "Windows was
     * told the disk was read-only" - two identical-looking failures with
     * opposite fixes.
     */
    ULONG WriteProtectSuppressed;
    /*
     * Unsupported-opcode accounting.
     *
     * Every CDB the allowlist refuses used to return SRB_STATUS_INVALID_REQUEST
     * and nothing recorded WHICH opcode it was: LastExecuteReject is the last
     * outcome only, and LastOpcode is the last opcode overall, so a rejection
     * during enumeration was unrecoverable from telemetry after the fact.
     *
     * RejectedOpcodes keeps the first UFS_REJECTED_OPCODE_SLOTS DISTINCT
     * opcodes in first-seen order, which is exactly the probe sequence Windows
     * issues while bringing a disk up. Stored as bytes rather than a bitmap or
     * a 64-bit log because this struct is #pragma pack(1), and a byte store is
     * the only width that is unconditionally safe at an arbitrary offset.
     */
    ULONG SyntheticCheckConditions;
    ULONG RejectedOpcodeCount;
    ULONG RejectedOpcodeOverflow;
    UCHAR RejectedOpcodes[UFS_REJECTED_OPCODE_SLOTS];

    /*
     * Context for the first refused NON-WRITE request.
     *
     * RejectedOpcodes names WHICH opcodes were refused but not WHY, and for an
     * opcode that is on the allowlist - 0x12 INQUIRY is - "why" can only be a
     * sub-validator inside its accepted case. Naming that sub-validator needs
     * the operands, so this captures them.
     *
     * Writes are deliberately skipped: the GPT guard refuses Windows' own
     * writes into LBA 0-5 before the probe ever arms (seven of them, every
     * boot), so "first refusal" without that filter would reliably capture
     * WRITE(10) and never the enumeration command being investigated.
     *
     * Packed into one ULONGLONG rather than separate members because the ring
     * is already 71 bytes over UFS_PRAM_CAPACITY and every added per-record
     * field costs a record off the tail:
     *
     *   bits  0..7   Cdb[0]  opcode
     *   bits  8..15  Cdb[1]  (INQUIRY: EVPD in bit 0)
     *   bits 16..23  Cdb[2]  (INQUIRY: VPD page code)
     *   bits 24..31  CdbLength
     *   bits 32..63  DataTransferLength
     *
     * Zero means "nothing refused yet", which is unambiguous because a real
     * capture always carries a non-zero CdbLength.
     */
    ULONGLONG FirstRejectContext;

#if UFS_PERF_INSTRUMENTATION
    /*
     * V33 latency instrumentation tail.
     *
     * Appended, so every offset above is unchanged - the header's own rule for
     * growing this struct. UFS_DIAG_DATA_VERSION is bumped so a mismatched
     * UfsDiag.exe reports the tail as absent rather than printing garbage.
     *
     * These are raw CNTVCT_EL0 ticks, not microseconds: converting in kernel
     * mode would need a 64-bit divide per I/O on the hot path for no benefit,
     * and CounterFrequency is reported alongside so the host does it once.
     *
     * What each answers:
     *   DeviceTicks   - time the controller/device actually owned the command.
     *                   This is the floor; no driver change can beat it.
     *   ZeroTicks     - time spent zeroing the uncached bounce before reads.
     *                   Pure diagnostic cost inherited from bring-up.
     *   BounceInTicks - time draining the uncached bounce into the caller.
     *   BounceOutTicks- time staging the caller into the uncached bounce.
     *   PollIterations- number of 10 us stalls paid; times UFS_TRANSFER_POLL_US
     *                   this is how much of DeviceTicks is polling granularity
     *                   rather than device time.
     *
     * If Zero+BounceIn is a large fraction of DeviceTicks, the win is in the
     * copies (wider accesses / dropping the zero-fill) and not in the device.
     */
    ULONGLONG PerfCounterFrequency;
    ULONGLONG PerfDeviceTicks;
    ULONGLONG PerfZeroTicks;
    ULONGLONG PerfBounceInTicks;
    ULONGLONG PerfBounceOutTicks;
    ULONGLONG PerfPollIterations;
    ULONGLONG PerfReadBytes;
    ULONGLONG PerfWriteBytes;
    ULONGLONG PerfMaxDeviceTicks;
    ULONG PerfReadCount;
    ULONG PerfWriteCount;
    /*
     * The driver's own poll granularity, reported rather than assumed. UfsDiag
     * does not include the kernel header, and hardcoding the value there would
     * silently start lying the moment this interval is tuned - which is one of
     * the very optimisations these counters exist to evaluate.
     */
    ULONG PerfPollIntervalUs;
#endif /* UFS_PERF_INSTRUMENTATION */

#if UFS_DMA_WINDOW_ENFORCE
    /*
     * DMA-window accounting. Appended, per this header's rule that the tail
     * grows and nothing above it moves.
     *
     * WorkspaceBounded is 1 when the UTRL/UCD/bounce workspace was allocated
     * with explicit FMP-window physical bounds rather than from
     * StorPortGetUncachedExtension. A 0 here on a machine where DRAM bank1 has
     * been published as conventional memory is the exact condition that made
     * the first MR1 boot hang before storage came up: Storport was free to
     * place DMA memory somewhere the controller cannot reach.
     *
     * The three physical addresses are reported so a boot can be checked
     * against the window instead of assumed to be inside it.
     */
    ULONG WorkspaceBounded;
    ULONGLONG DmaWindowFirst;
    ULONGLONG DmaWindowLast;
#endif /* UFS_DMA_WINDOW_ENFORCE */
} UFS_DIAGNOSTIC_DATA, *PUFS_DIAGNOSTIC_DATA;
#pragma pack(pop)

/*
 * Separate, read-only PE build identity, retained by /INCLUDE in both portable
 * binaries. It is not part of the IOCTL/CDB structure or any retained-RAM ABI.
 * "UFSKABI1", byte length, record version, then the actual compiled ABI/flags.
 */
#define UFS_DIAG_BINARY_CONTRACT_INITIALIZER { \
    0x4B534655UL, 0x31494241UL, 44UL, 1UL, \
    UFS_DIAG_DATA_SIGNATURE, UFS_DIAG_DATA_VERSION, \
    sizeof(UFS_DIAGNOSTIC_DATA), UFS_PERF_INSTRUMENTATION, \
    UFS_WIDE_UNCACHED_ACCESS, UFS_DMA_WINDOW_ENFORCE, \
    UFS_PORTABLE_KERNEL_CONTRACT \
}
