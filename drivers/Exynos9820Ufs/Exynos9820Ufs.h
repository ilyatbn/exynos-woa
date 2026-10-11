#pragma once

#include <ntddk.h>
#include <storport.h>
#include <ntddscsi.h>
#include "Exynos9820UfsDiag.h"
#include "../include/Rwd1Constants.h"

#include <aux_klib.h>

#ifndef SCSISTAT_GOOD
#define SCSISTAT_GOOD                       0x00U
#endif

#define EXYNOS_UFS_HCI_PHYSICAL_BASE       0x13D60000ULL   // VERIFIED (9820)
#define EXYNOS_UFS_HCI_LENGTH              0x00002000UL
#define EXYNOS_UFS_UNIPRO_PHYSICAL_BASE    0x13D50000ULL   // VERIFIED (9820)
#define EXYNOS_UFS_UNIPRO_LENGTH           0x00008000UL
#define EXYNOS_UFS_PMA_PHYSICAL_BASE       0x13D64000ULL   // VERIFIED (9820)
#define EXYNOS_UFS_PMA_LENGTH              0x00000800UL   // VERIFIED: stock DT ufs-phy reg 0x13D64000+0x800 = the DSDT window (W26: 0x1000 rejected it, FAIL PMA_RANGE_MISSING); presence-checked only, never mapped
#define EXYNOS_UFS_UFSP_PHYSICAL_BASE      0x13D70000ULL   // VERIFIED (9820)
#define EXYNOS_UFS_UFSP_LENGTH             0x00001000UL

//
// ---------------------------------------------------------------------------
// PORTING NOTE — this package targets the Exynos 9820 (Galaxy S10e, `beyond0lte`)
// and was copied from the validated Exynos 9810 package. Nothing in it has run
// on a 9820 device, so under this repo's golden rule no stage is `Ready`.
//
// The four register windows above are the 9810 values and are kept so the file
// still compiles. The 9820 values, VERIFIED twice (the shipping device tree in
// notes/dock-desktop.md §5, and the device's own fstab, which names
// /dev/block/platform/13d60000.ufs/by-name/keydata), are:
//
//   HCI      0x13D60000 + 0x2000
//   UNIPRO   0x13D50000 + 0x8000
//   PMA      0x13D64000 + 0x1000   (the DT declares the window as 0x800; 0x1000
//                                   is kept so no access is outside the mapping)
//   UFSP     0x13D70000 + 0x1000
//   vendor   HCI + 0x1100 = 0x13D61100 — the SAME intra-window offset as the 9810,
//            so UFS_VENDOR_OFFSET below does not change
//
// Also verified on the 9820: the PMU ("system-controller") moves to 0x15860000,
// while the watchdog cl0 stays at 0x10050000.
//
// UNVERIFIED, deliberately left at the 9810 value rather than guessed:
//   * UFS_PRAM_PHYSICAL_BASE and UFS_LOW_WDT_PRAM_PHYSICAL_BASE — these are OUR
//     firmware's reserved DRAM, chosen against the 9810's memory map. The S10e's
//     DRAM banks differ, so they must be re-picked with the 9820 map (M8).
//   * UFS_DMA_WINDOW_FIRST/LAST — re-picked to the free band inside the LIVE bank0
//     (0xA7400000..0xBAB00000): the live DTB ends bank0 at 0x80000000+0x3AB00000,
//     so the 9810's 0x90000000..0xBC800000 overran it. Still a CHOSEN band, UNVERIFIED.
//   * every write-fence and protected-partition LBA — the S10e's partition table is
//     a different list (notes/dock-desktop.md §13); the map derived there is
//     arithmetic, not a GPT read, so it may not be written here as a constant.
//   * the PMU register offsets below — the base moves, the +0x400/+0x80C layout
//     inside it has not been confirmed on 9820.
//   * UFS_HCI_VERSION_21 — the S10e reports hw-rev UFS_VER_0006 (Samsung's
//     include/dt-bindings/ufs/ufs.h: UFS_VER_0004 = 4, 0005 = 5, 0006 = 6), a
//     newer revision class than the 9810's 4/5. The HCI encoding it implies is
//     NOT guessed anywhere: Exynos9820Ufs.c reports a mismatching version with
//     the non-fatal UFS_DIAG_FAILURE_HCI_VERSION_UNCONFIRMED bit instead of
//     invalidating the warm state, so the first run on Windows tells us the
//     real value and this line can become a read.
//
// The 0x9810 values that appear as CDB subcodes and as UFS_DIAG_CONTROL_CODE in
// Exynos9820UfsDiag.h are wire-format constants, not chip names: they are what
// the host-side decoder already expects, so they were deliberately left alone.
// ---------------------------------------------------------------------------
//

//
// Persistent-RAM breadcrumb console. The firmware already writes boot markers
// here (=NTPATCH, =CFQSCAN) and TWRP exposes the same physical bytes as
// /sys/fs/pstore/pmsg-ramoops-0, so anything this driver appends survives the
// warm reset and can be pulled over adb without a human reading the screen.
//
// The region sits at 0xFED14000, which is ABOVE the end of EXYNOS_DRAM
// (0xBC800000) and is published by PlatformMemoryMapLib as a Device/non-cached
// window, so it is not memory Windows owns or allocates.
//
// Layout, matching Star2LteResetSystemLib exactly:
//     +0x00  ULONG magic 0x43474244
//     +0x04  ULONG reserved
//     +0x08  ULONG position, byte cursor into the data area
//     +0x0C  data, UFS_PRAM_CAPACITY bytes
//
// Every append is a single-byte store, so this channel cannot reproduce the
// unaligned-STRH fault class that cost V16-V23.
//
//
// UFS_PRAM_ENABLE (default 0, notes/m10-windows-deploy-plan.md audit): both PRAM windows below
// (0xFED14000 ring, 0xFED13000 low-WDT record) are 9810 ramoops choices. On the 9820 they fall
// inside the vendor's debug_snapshot carve-out (0xFD900000+), which the UEFI memory map does not
// publish and Samsung's hypervisor may protect. With 0 the driver never maps them: every PRAM
// user already treats a NULL mapping as "channel absent".
//
//
// W23: ON, at the 9820 firmware's reserved ring (Beyond0LteBootLog.h BEYOND0LTE_WINPRAM_BASE,
// EfiReservedMemoryType in the UEFI map). The boot after a Windows run copies it to \WINPRAM.BIN
// on the ESP. The low-WDT page (UFS_LOW_WDT_PRAM_PHYSICAL_BASE, still a 9810 address) stays
// unmapped: UFS_LOW_WDT_PRAM_ENABLE.
//
#define UFS_PRAM_ENABLE                    1
#define UFS_LOW_WDT_PRAM_ENABLE            0
#define UFS_PRAM_PHYSICAL_BASE             0x8FE00000ULL   // firmware choice: must equal BEYOND0LTE_WINPRAM_BASE
#define UFS_PRAM_MAGIC                     0x43474244UL
#define UFS_PRAM_HEADER_SIZE               0x0000000CUL
#define UFS_PRAM_CAPACITY                  0x00003F00UL
#define UFS_PRAM_RUNTIME_RESERVE           0x00000400UL
#define UFS_PRAM_DRIVER_CAPACITY           (UFS_PRAM_CAPACITY - UFS_PRAM_RUNTIME_RESERVE)

//
// The window must cover the whole ring, not just its first page. The firmware
// ring is linear and caps at UFS_PRAM_CAPACITY with no wrap (see
// PlatformBootManagerLib.c), and the cursor it hands us is already several
// hundred bytes in by the time Windows starts, so a short mapping would put
// UfsPramByte's very first store past the end of the mapped range.
//
// 0x4000 is also the reserved region's real extent, not a guess: the firmware
// keeps its own scratch words at 0xFED17F80 and 0xFED17FB0, i.e. above the
// 0x3F00 ring cap but still inside 0xFED14000..0xFED18000.
//
#define UFS_PRAM_WINDOW \
    ((UFS_PRAM_HEADER_SIZE + UFS_PRAM_CAPACITY + 0xFFFUL) & ~0xFFFUL)

C_ASSERT(UFS_PRAM_WINDOW >= UFS_PRAM_HEADER_SIZE + UFS_PRAM_CAPACITY);
C_ASSERT(UFS_PRAM_RUNTIME_RESERVE < UFS_PRAM_CAPACITY);
C_ASSERT(UFS_PRAM_DRIVER_CAPACITY > UFS_PRAM_CAPACITY / 2UL);

//
// RESERVED NOTE SLOTS.
//
// A note emitted late in the boot cannot be appended: by the time UfsDiag.exe
// runs its fs-write step the ring is already full (measured RING_BYTES=16195
// against the 0x3F00 = 16128 cap), so UfsPramByte's bounds check drops the
// record silently. The slots are therefore RESERVED at ring offset 0 during
// UfsPramInitialize - immediately after the cursor reset, which is why they are
// always at offset 0 and always inside capacity - and rewritten IN PLACE by
// UfsPramNote without moving the cursor.
//
// Each slot begins with '\n' because that is the record delimiter the host
// decoder splits on; the remainder is padded with '.' so a slot that is never
// written is inert text rather than a partial record. 80 bytes is the smallest
// size that holds the tag plus four 8-hex-digit fields:
//
//   "=UFSNOTE NSEQ=xxxxxxxx NCODE=xxxxxxxx NAUX=xxxxxxxx NTAG=xxxxxxxx" = 65
//
// Eight hex digits is not cosmetic: collect-pram.ps1 matches field values with
// {8,}, a deliberate truncation guard, so a shorter value is discarded.
//
// Eight slots, not four. The fs-write step now surveys the disk before it
// writes - drive layout, an optional rescan, the volume enumeration and a mount
// attempt - and each of those is a separate note. Six are emitted on the busiest
// path (START, LAYOUT, RESCAN, VOLUMES, MOUNT, outcome), so four would silently
// drop the outcome: UfsPramNote clamps to the last slot and overwrites it, which
// loses exactly the record that answers the question. The ceiling is the
// C_ASSERT below (UFS_PRAM_CAPACITY / 4 = 4032 bytes = 50 slots), so 640 bytes
// is comfortably inside it; the cost is ~640 bytes of ring tail, i.e. roughly
// one UFSDIAG record, and the tail is what truncates first anyway.
//
#define UFS_PRAM_NOTE_SLOTS                8UL
#define UFS_PRAM_NOTE_SLOT_SIZE            80UL
#define UFS_PRAM_NOTE_RESERVED             (UFS_PRAM_NOTE_SLOTS * UFS_PRAM_NOTE_SLOT_SIZE)
#define UFS_PRAM_BUGCHECK_SLOT_SIZE        128UL

C_ASSERT(UFS_PRAM_NOTE_RESERVED < UFS_PRAM_CAPACITY / 4UL);
C_ASSERT(
    UFS_PRAM_NOTE_RESERVED + UFS_PRAM_BUGCHECK_SLOT_SIZE <
    UFS_PRAM_CAPACITY / 4UL
    );


//
// Warm-reset crash latch.
//
// This is the ONLY state that outlives a bugcheck. The ring itself does not:
// PlatformBootManagerLib rewinds the cursor and spends ~13.8 KB on its own
// breadcrumbs every boot, so by the time this driver loads, the previous boot's
// records are already gone.
//
// The header word at +0x04 is NOT usable either, even though every ring writer
// guards it behind "if (magic absent)". Star2LteTraceResetLog() zeroes all three
// header words unconditionally on every boot (PlatformBootManagerLib.c:4996).
//
// Instead the latch goes where the firmware already puts its own survivable
// state: ABOVE the 0x3F00 flood cap. PlatformBootManagerLib.c:193-196 spells the
// rule out - the ring data ends at 0xFED17F0B, so anything past it "is inside
// the mapped ring PAGE ... but the firmware never overwrites it". The firmware's
// own latches live at 0xFED17F60 (RamManager), 0xFED17F80/90/98/A0/A8 (winload
// TTBR latch) and 0xFED17FB0 (Windows handoff marker); 0x3F40/0x3F44 sit in the
// untouched gap between the cap and 0xFED17F60.
//
// Because nothing clears these words - not even a cold boot - the latch is made
// self-healing rather than relying on power-cycle semantics: initialization
// always rewrites it as "complete", so a lockout lasts exactly one boot and then
// clears itself.
//
//     guard  : 'WATL', distinguishes a real latch from uninitialized DRAM
//     latch  : [31:24] tag  [23:16] boots (saturating)
//              [15:8]  last write attempt index  [0] probe completed
//
#define UFS_PRAM_LATCH_OFFSET              0x00003F40UL
#define UFS_PRAM_LATCH_GUARD_OFFSET        0x00003F44UL
#define UFS_PRAM_LATCH_GUARD               0x5741544CUL
#define UFS_PRAM_LATCH_TAG                 0xA7UL
#define UFS_PRAM_LATCH_TAG_SHIFT           24
#define UFS_PRAM_LATCH_BOOTS_SHIFT         16
#define UFS_PRAM_LATCH_ATTEMPT_SHIFT       8
#define UFS_PRAM_LATCH_BYTE_MASK           0xFFUL
#define UFS_PRAM_LATCH_COMPLETED           0x00000001UL

//
// Both words must clear the ring's flood cap, stay dword aligned, sit below the
// firmware's own first latch at 0xFED17F60, and remain inside the mapping.
//
C_ASSERT(UFS_PRAM_LATCH_OFFSET >= UFS_PRAM_HEADER_SIZE + UFS_PRAM_CAPACITY);
C_ASSERT(UFS_PRAM_LATCH_GUARD_OFFSET == UFS_PRAM_LATCH_OFFSET + sizeof(ULONG));
C_ASSERT((UFS_PRAM_LATCH_OFFSET % sizeof(ULONG)) == 0);
C_ASSERT(UFS_PRAM_LATCH_GUARD_OFFSET + sizeof(ULONG) <= 0x00003F60UL);
C_ASSERT(UFS_PRAM_LATCH_GUARD_OFFSET + sizeof(ULONG) <= UFS_PRAM_WINDOW);

//
// WRITE-PATH PHASE BREADCRUMB.
//
// The crash latch records WHICH write attempt was in flight, but not WHERE in
// the write path it died. That window spans the data-out gate, two reject
// branches, the dry-run branch and the vendor-diag CDB that the helper issues
// after every probe entry - far too wide to attribute a bugcheck.
//
// This word narrows it to a single code point. It is written on entry to each
// candidate region and read back on the next boot, so a crash names its own
// location the same way the latch names its own attempt.
//
// Deliberately a separate word rather than the latch's seven free low bits: it
// keeps the latch encoding that collect-pram.ps1 already decodes untouched, and
// it is one naturally aligned 32-bit store, structurally immune to the
// unaligned-halfword class that caused the V16/V20-V23 bugchecks.
//
// The tag guards against uninitialized DRAM inventing a phase, exactly as
// UFS_PRAM_LATCH_GUARD does for the latch.
//
#define UFS_PRAM_PHASE_OFFSET              0x00003F48UL
#define UFS_PRAM_PHASE_TAG                 0xB3UL
#define UFS_PRAM_PHASE_TAG_SHIFT           24
#define UFS_PRAM_PHASE_MASK                0xFFUL

//
// The UFS_PHASE_* codes themselves live in Exynos9820UfsDiag.h: they cross the
// driver/helper boundary (UfsDiag.exe names them) and so belong with the rest of
// the shared telemetry ABI, alongside UFS_EXEC_REJECT_* and UFS_DIAG_FAILURE_*.
// Only the PRAM placement of the word is driver-private and stays here.
//

//
// Same invariants as the latch: dword aligned, clear of the ring's flood cap,
// below the firmware's own first latch, and inside the mapping.
//
C_ASSERT(UFS_PRAM_PHASE_OFFSET >= UFS_PRAM_HEADER_SIZE + UFS_PRAM_CAPACITY);
C_ASSERT(UFS_PRAM_PHASE_OFFSET == UFS_PRAM_LATCH_GUARD_OFFSET + sizeof(ULONG));
C_ASSERT((UFS_PRAM_PHASE_OFFSET % sizeof(ULONG)) == 0);
C_ASSERT(UFS_PRAM_PHASE_OFFSET + sizeof(ULONG) <= 0x00003F60UL);
C_ASSERT(UFS_PRAM_PHASE_OFFSET + sizeof(ULONG) <= UFS_PRAM_WINDOW);

//
// CACHE-FLUSH WITNESS.
//
// SYNCHRONIZE CACHE has no payload, so the ordinary data-transfer telemetry
// cannot prove that it reached the device. This fixed word survives ring
// exhaustion and records the current boot epoch, the number of hardware flush
// attempts, whether any completed successfully, and the state of the last one.
//
#define UFS_PRAM_FLUSH_OFFSET              0x00003F4CUL
#define UFS_PRAM_FLUSH_GUARD_OFFSET        0x00003F50UL
#define UFS_PRAM_FLUSH_GUARD               0x48534C46UL
#define UFS_PRAM_FLUSH_TAG                 0xF1UL
#define UFS_PRAM_FLUSH_TAG_SHIFT           24
#define UFS_PRAM_FLUSH_BOOT_SHIFT          16
#define UFS_PRAM_FLUSH_ATTEMPT_SHIFT       8
#define UFS_PRAM_FLUSH_BYTE_MASK           0xFFUL
#define UFS_PRAM_FLUSH_EVER_COMPLETED      0x00000001UL
#define UFS_PRAM_FLUSH_LAST_COMPLETED      0x00000002UL
#define UFS_PRAM_FLUSH_IN_FLIGHT           0x00000004UL
#define UFS_PRAM_FLUSH_LAST_FAILED         0x00000008UL

C_ASSERT(UFS_PRAM_FLUSH_OFFSET == UFS_PRAM_PHASE_OFFSET + sizeof(ULONG));
C_ASSERT(UFS_PRAM_FLUSH_GUARD_OFFSET == UFS_PRAM_FLUSH_OFFSET + sizeof(ULONG));
C_ASSERT((UFS_PRAM_FLUSH_OFFSET % sizeof(ULONG)) == 0);
C_ASSERT(UFS_PRAM_FLUSH_GUARD_OFFSET + sizeof(ULONG) <= 0x00003F60UL);
C_ASSERT(UFS_PRAM_FLUSH_GUARD_OFFSET + sizeof(ULONG) <= UFS_PRAM_WINDOW);

/*
 * Firmware-reserved Windows UFS DMA workspace.
 *
 * Early-RAM builds reserve UFS_WORKSPACE_SIZE bytes as EfiReservedMemoryType
 * inside Samsung's FMP aperture and publish its physical base here. This avoids
 * relying on Storport's below-4-GB allocator after additional low DRAM becomes
 * conventional memory.
 *
 * The driver changes MAGIC to CONSUMED before using the record. That makes a
 * record one-shot and prevents an older firmware image from reusing a stale
 * address that it did not reserve on the current boot.
 */
#define UFS_PRAM_DMA_POOL_MAGIC_OFFSET      0x00003F54UL
#define UFS_PRAM_DMA_POOL_BASE_OFFSET       0x00003F58UL
#define UFS_PRAM_DMA_POOL_CHECK_OFFSET      0x00003F5CUL
#define UFS_PRAM_DMA_POOL_MAGIC             0x31504D44UL   /* 'DMP1' */
#define UFS_PRAM_DMA_POOL_CONSUMED          0x31434D44UL   /* 'DMC1' */
#define UFS_PRAM_DMA_POOL_GUARD             0x4B4F5044UL   /* 'DPOK' */

C_ASSERT(
    UFS_PRAM_DMA_POOL_MAGIC_OFFSET ==
    UFS_PRAM_FLUSH_GUARD_OFFSET + sizeof(ULONG)
    );
C_ASSERT(
    UFS_PRAM_DMA_POOL_BASE_OFFSET ==
    UFS_PRAM_DMA_POOL_MAGIC_OFFSET + sizeof(ULONG)
    );
C_ASSERT(
    UFS_PRAM_DMA_POOL_CHECK_OFFSET ==
    UFS_PRAM_DMA_POOL_BASE_OFFSET + sizeof(ULONG)
    );
C_ASSERT(UFS_PRAM_DMA_POOL_CHECK_OFFSET + sizeof(ULONG) <= 0x00003F60UL);

//
// FIRMWARE WINDOWS-HANDOFF MARKER (owned by UEFI, cleared here).
//
// PlatformBootManagerLib.c:329-330 stamps STAR2LTE_WIN_ATTEMPT_MAGIC into
// 0xFED17FB0 immediately before ExitBootServices, and
// Star2LteCheckWindowsHandoffRecovery() re-reads it on the NEXT UEFI entry. A
// marker that is still set means the previous attempt handed off to Windows and
// something reset the SoC without Windows ever getting far enough to matter, so
// UEFI diverts to TWRP via the device-verified INFORM3 + PMU SWRESET path.
//
// That mechanism is what makes an ExitBootServices-side wedge self-recovering,
// but it was disabled (STAR2LTE_AUTO_RECOVERY_ON_HANG 0) for one specific
// reason, recorded at PlatformBootManagerLib.c:323-324: "The handoff marker
// cannot tell a legitimate Windows restart from a hang, so with this on every
// successful Windows session diverts the NEXT boot to TWRP."
//
// Clearing the marker from here supplies exactly the missing signal. Reaching
// the first Storport timer tick proves the kernel is alive, scheduling, and
// running our driver - i.e. the handoff did NOT hang - so the next UEFI entry
// must boot Windows normally. A boot that never reaches that tick leaves the
// marker set and is correctly treated as a hang.
//
// The address is the firmware's, not ours, so it is derived from the same
// absolute constant rather than restated: offset = 0xFED17FB0 - 0xFED14000.
// It lands above the ring cap, clear of the latch/phase words, dword aligned,
// and inside the existing mapping - so this is one naturally aligned 32-bit
// store, structurally immune to the unaligned-halfword class that caused the
// V16/V20-V23 bugchecks.
//
#define UFS_PRAM_HANDOFF_MARKER_ADDR       (UFS_PRAM_PHYSICAL_BASE + 0x3FB0ULL)   // W23: follows the ring base
#define UFS_PRAM_HANDOFF_MARKER_OFFSET \
    ((ULONG)(UFS_PRAM_HANDOFF_MARKER_ADDR - UFS_PRAM_PHYSICAL_BASE))
#define UFS_PRAM_HANDOFF_MARKER_MAGIC      0x7A5E2C01UL

C_ASSERT(UFS_PRAM_HANDOFF_MARKER_OFFSET == 0x00003FB0UL);
C_ASSERT(UFS_PRAM_HANDOFF_MARKER_OFFSET >= UFS_PRAM_HEADER_SIZE + UFS_PRAM_CAPACITY);
C_ASSERT(UFS_PRAM_HANDOFF_MARKER_OFFSET > UFS_PRAM_PHASE_OFFSET);
C_ASSERT((UFS_PRAM_HANDOFF_MARKER_OFFSET % sizeof(ULONG)) == 0);
C_ASSERT(UFS_PRAM_HANDOFF_MARKER_OFFSET + sizeof(ULONG) <= UFS_PRAM_WINDOW);

/*
 * v47: the low-level watchdog handoff record.
 *
 * BdsDxe arms a hardware watchdog epoch and publishes ARM1/FWA1 here. It counts
 * every boot that fails to acknowledge and, on the THIRD unacknowledged epoch,
 * clears the state, writes REC1 and diverts the next boot to recovery.
 *
 * v46 mapped nothing here and never wrote the record, so a perfectly healthy
 * Windows boot was scored as a failed epoch. Measured on rate-g17-20260826 the
 * count ran 0, 1, 2 across three SUCCESSFUL boots and the fourth was diverted -
 * a guaranteed failure every third attempt. Acknowledging in HwInitialize is
 * what stops that, and it is the correct point because Storport has by then
 * committed to this adapter.
 */
#define UFS_LOW_WDT_PRAM_PHYSICAL_BASE      0xFED13000ULL
#define UFS_LOW_WDT_PRAM_WINDOW             0x00001000UL
#define UFS_LOW_WDT_RECORD_OFFSET           0x00000800UL
#define UFS_LOW_WDT_RECORD_BYTES            0x0000000CUL
#define UFS_LOW_WDT_STATE_OFFSET            0x00000000UL
#define UFS_LOW_WDT_COUNT_OFFSET            0x00000004UL
#define UFS_LOW_WDT_STATUS_OFFSET           0x00000008UL
#define UFS_LOW_WDT_ARM_MAGIC               0x314D5241UL   /* 'ARM1' */
#define UFS_LOW_WDT_WINDOWS_ARMED           0x31524157UL   /* 'WAR1' */
#define UFS_LOW_WDT_WINDOWS_STOP            0x31545357UL   /* 'WST1' */

C_ASSERT(
    UFS_LOW_WDT_PRAM_PHYSICAL_BASE + UFS_LOW_WDT_RECORD_OFFSET ==
    0xFED13800ULL);
C_ASSERT(
    UFS_LOW_WDT_RECORD_OFFSET + UFS_LOW_WDT_RECORD_BYTES <=
    UFS_LOW_WDT_PRAM_WINDOW);
C_ASSERT(UFS_LOW_WDT_COUNT_OFFSET ==
        UFS_LOW_WDT_STATE_OFFSET + sizeof(ULONG));
C_ASSERT(UFS_LOW_WDT_STATUS_OFFSET ==
        UFS_LOW_WDT_COUNT_OFFSET + sizeof(ULONG));
C_ASSERT(UFS_LOW_WDT_STATUS_OFFSET + sizeof(ULONG) ==
        UFS_LOW_WDT_RECORD_BYTES);
/*
 * The two PRAM windows must not overlap: this record lives one 4 KiB page below
 * the firmware trace ring, and both are mapped independently.
 */
#if UFS_LOW_WDT_PRAM_ENABLE
C_ASSERT(
    UFS_LOW_WDT_PRAM_PHYSICAL_BASE + UFS_LOW_WDT_PRAM_WINDOW <=
    UFS_PRAM_PHYSICAL_BASE);
#endif

/*
 * RWD1 and SMP1 share the already-mapped 0xFED13000 PRAM page. RWD1 is the
 * authoritative watchdog owner; the 12-byte low-WDT record remains a
 * compatibility mirror only.
 */
#define UFS_RWD1_RECORD_OFFSET \
    ((ULONG)(RWD1_PHYSICAL_BASE - UFS_LOW_WDT_PRAM_PHYSICAL_BASE))
#define UFS_RWD1_ENTRY_COOKIE_OFFSET \
    ((ULONG)(RWD1_ENTRY_COOKIE_ADDR - UFS_LOW_WDT_PRAM_PHYSICAL_BASE))
#define UFS_SMP_RECORD_OFFSET                0x00000E80UL
#define UFS_SMP_RECORD_DWORDS                32UL
#define UFS_SMP_MAGIC_VALID                  0x31504D53UL
#define UFS_SMP_MAGIC_COMMIT                 0x21504D53UL
#define UFS_SMP_VERSION_LENGTH               0x00800001UL
#define UFS_SMP_G19_GENERATION               0xA55E3013UL
#define UFS_SMP_W_MAGIC                      0UL
#define UFS_SMP_W_VERSION                    1UL
#define UFS_SMP_W_GENERATION                 2UL
#define UFS_SMP_W_SEQUENCE                   3UL
#define UFS_SMP_W_CHECKSUM                   4UL
#define UFS_SMP_W_STATEWORD                  5UL
#define UFS_SMP_STATE_PREBRANCH              0x60UL
#define UFS_SMP_STATE_WINDOWS_ACK            0x70UL
#define UFS_SMP_FLAG_ONE_SHOT_ARMED          0x00010000UL

C_ASSERT(UFS_RWD1_RECORD_OFFSET == 0x00000D80UL);
C_ASSERT(UFS_RWD1_ENTRY_COOKIE_OFFSET == 0x00000DC0UL);
C_ASSERT(
    UFS_RWD1_RECORD_OFFSET + RWD1_RECORD_BYTES <=
    UFS_RWD1_ENTRY_COOKIE_OFFSET);
C_ASSERT(
    UFS_RWD1_ENTRY_COOKIE_OFFSET + sizeof(ULONG) <=
    UFS_SMP_RECORD_OFFSET);
C_ASSERT(
    UFS_SMP_RECORD_OFFSET +
        (UFS_SMP_RECORD_DWORDS * sizeof(ULONG)) <=
    UFS_LOW_WDT_PRAM_WINDOW);

#define UFS_NO_PET_NONE                      0UL
#define UFS_NO_PET_BUGCHECK                  1UL
#define UFS_NO_PET_STORAGE_FATAL             2UL
#define UFS_NO_PET_RESTART_FAILED            3UL
#define UFS_NO_PET_SHUTDOWN_FINAL            4UL
#define UFS_NO_PET_WATCHDOG_INVALID          5UL
#define UFS_NO_PET_RECOVERY_REQUEST          6UL

//
// UNATTENDED SESSION CONTROL and WINDOWS WITNESS.
//
// Two things blocked running bring-up cycles without a human present:
//
//   1. Nothing inside Windows ever reported what Windows itself SEES. Active
//      processor count and usable physical memory are the two numbers that every
//      SMP and every memory-map change exists to move, and both were being read
//      off a screen by a person. A firmware-side claim that N cores were parked
//      is not evidence that Windows scheduled on them.
//   2. Returning to TWRP after a measurement needed somebody to hold buttons.
//
// Neither needs a new mechanism. This driver already maps PRAM and the PMU, and
// UfsUnattendedTimer already knows the device-verified route to recovery. All
// that was missing was a place to put the answer and a way to ask for the
// return.
//
// The division between the two blocks below is deliberate and is the whole
// safety argument:
//
//   witness    pure observation, written unconditionally on every boot. It
//              changes no behaviour, so it cannot make a boot worse.
//   arm token  written by TWRP, never by this driver, and STRICTLY OPT-IN. With
//              no valid token the driver behaves exactly as it did before and a
//              normal Windows session is never reset out from under the user.
//
// The token carries a magic, a bounded timeout and a checksum over both, so
// uninitialized DRAM has to reproduce roughly 2^-90 worth of coincidence to
// invent an unattended reset. The driver acknowledges the token by overwriting
// the magic, which makes arming ONE-SHOT: a boot that hangs after the token is
// consumed cannot leave the next boot armed, and a TWRP script that forgets to
// clear it cannot start a reboot loop.
//
// Both blocks live in the free fixed-word gap between the flush guard (0x3F50)
// and the firmware's handoff marker (0x3FB0). Every word is naturally aligned
// and sits above the ring's flood cap, so neither the firmware trace nor this
// driver's telemetry can reach them.
//
#define UFS_PRAM_UNATTEND_ARM_OFFSET       0x00003F60UL
#define UFS_PRAM_UNATTEND_SEC_OFFSET       0x00003F64UL
#define UFS_PRAM_UNATTEND_CHK_OFFSET       0x00003F68UL
#define UFS_PRAM_UNATTEND_ACK_OFFSET       0x00003F6CUL
#define UFS_PRAM_UNATTEND_ARM_MAGIC        0x31524155UL   /* 'UAR1' */
#define UFS_PRAM_UNATTEND_GUARD            0x4E55414BUL   /* 'KAUN' */
#define UFS_PRAM_UNATTEND_ACK_MAGIC        0x31414B41UL   /* 'AKA1' */
#define UFS_PRAM_UNATTEND_SEC_MIN          20UL
#define UFS_PRAM_UNATTEND_SEC_MAX          3600UL

C_ASSERT(UFS_PRAM_UNATTEND_ARM_OFFSET >= UFS_PRAM_FLUSH_GUARD_OFFSET + sizeof(ULONG));
C_ASSERT(UFS_PRAM_UNATTEND_SEC_OFFSET == UFS_PRAM_UNATTEND_ARM_OFFSET + sizeof(ULONG));
C_ASSERT(UFS_PRAM_UNATTEND_CHK_OFFSET == UFS_PRAM_UNATTEND_SEC_OFFSET + sizeof(ULONG));
C_ASSERT(UFS_PRAM_UNATTEND_ACK_OFFSET == UFS_PRAM_UNATTEND_CHK_OFFSET + sizeof(ULONG));
C_ASSERT((UFS_PRAM_UNATTEND_ARM_OFFSET % sizeof(ULONG)) == 0);
C_ASSERT(UFS_PRAM_UNATTEND_SEC_MIN < UFS_PRAM_UNATTEND_SEC_MAX);

//
// Windows witness record. Ten dwords at 0x3F70..0x3F98, ending clear of the
// firmware handoff marker at 0x3FB0.
//
//   [0] magic          [5] physical pages, high 32
//   [1] active procs   [6] physical range count
//   [2] maximum procs  [7] flags
//   [3] active groups  [8] tick at which it was last refreshed
//   [4] pages, low 32  [9] checksum
//
// Written magic-last, exactly like the PMU normal-boot record, so a reader can
// never observe a half-built record: every other word is already committed
// before the magic makes it findable.
//
#define UFS_PRAM_WITNESS_OFFSET            0x00003F70UL
#define UFS_PRAM_WITNESS_DWORDS            10UL
#define UFS_PRAM_WITNESS_MAGIC             0x31525757UL   /* 'WWR1' */
#define UFS_PRAM_WITNESS_GUARD             0x314B4F57UL   /* 'WOK1' */
#define UFS_PRAM_WITNESS_FLAG_EARLY        0x00000001UL   /* captured in DriverEntry */
#define UFS_PRAM_WITNESS_FLAG_INIT         0x00000002UL   /* refreshed in HwInitialize */
#define UFS_PRAM_WITNESS_FLAG_TICK         0x00000004UL   /* refreshed from a timer tick */
#define UFS_PRAM_WITNESS_FLAG_MEMOK        0x00000008UL   /* physical ranges enumerated */
#define UFS_PRAM_WITNESS_FLAG_ARMED        0x00000010UL   /* a valid arm token was consumed */

C_ASSERT(UFS_PRAM_WITNESS_OFFSET == UFS_PRAM_UNATTEND_ACK_OFFSET + sizeof(ULONG));
C_ASSERT((UFS_PRAM_WITNESS_OFFSET % sizeof(ULONG)) == 0);
C_ASSERT(
    UFS_PRAM_WITNESS_OFFSET + (UFS_PRAM_WITNESS_DWORDS * sizeof(ULONG)) <=
    UFS_PRAM_HANDOFF_MARKER_OFFSET
    );
C_ASSERT(UFS_PRAM_WITNESS_OFFSET >= UFS_PRAM_HEADER_SIZE + UFS_PRAM_CAPACITY);

//
// The firmware's BOOT-ATTEMPT COUNTER (Star2LteBootAttemptWatchdog in
// PlatformBootManagerLib.c). UEFI increments this on every Windows-path boot and, once it
// reaches its limit without being acknowledged, diverts the phone to TWRP so an unattended
// bring-up cycle can never be stranded in a reboot loop. This driver clears it to say
// "Windows got far enough to be worth keeping".
//
// Division of labour with the crash latch above, which is deliberate:
//
//     crash latch          crashes in the WRITE path, i.e. after the adapter is already
//                          initialized and serving commands. Self-limits to one boot.
//     boot-attempt count   crashes BEFORE the adapter finishes initializing, where nothing
//                          in Windows ever runs and so nothing in Windows can help itself.
//
// Because the acknowledgement happens at the end of a successful HwInitialize, a crash after
// that point still clears the counter - that window is the crash latch's job, not this one's.
//
// Sits below the crash latch and still above the ring's flood cap, so neither the firmware
// trace nor this driver's telemetry can ever reach it.
//
#define UFS_PRAM_BOOTATTEMPT_MAGIC_OFFSET  0x00003F10UL
#define UFS_PRAM_BOOTATTEMPT_COUNT_OFFSET  0x00003F14UL
#define UFS_PRAM_BOOTATTEMPT_MAGIC         0x31544142UL   // 'BAT1', little endian

C_ASSERT(UFS_PRAM_BOOTATTEMPT_MAGIC_OFFSET >= UFS_PRAM_HEADER_SIZE + UFS_PRAM_CAPACITY);
C_ASSERT(UFS_PRAM_BOOTATTEMPT_COUNT_OFFSET == UFS_PRAM_BOOTATTEMPT_MAGIC_OFFSET + sizeof(ULONG));
C_ASSERT((UFS_PRAM_BOOTATTEMPT_MAGIC_OFFSET % sizeof(ULONG)) == 0);
C_ASSERT(UFS_PRAM_BOOTATTEMPT_COUNT_OFFSET + sizeof(ULONG) <= UFS_PRAM_LATCH_OFFSET);

/*
 * Fixed PMU normal-boot record. The stop-path record overwrites the init record
 * immediately before ResetSystem, so it survives even when the text ring caps.
 */
#define UFS_PRAM_NORMAL_RECORD_OFFSET      0x00003F18UL
#define UFS_PRAM_NORMAL_RECORD_DWORDS      10UL
#define UFS_PRAM_NORMAL_RECORD_MAGIC       0x314D524EUL   /* 'NRM1' */
#define UFS_PRAM_NORMAL_RECORD_GUARD       0x4B4F4D50UL   /* 'PMOK' */
#define UFS_PRAM_NORMAL_PHASE_INIT         1UL
#define UFS_PRAM_NORMAL_PHASE_STOP         2UL

C_ASSERT(
    UFS_PRAM_NORMAL_RECORD_OFFSET ==
    UFS_PRAM_BOOTATTEMPT_COUNT_OFFSET + sizeof(ULONG)
    );
C_ASSERT((UFS_PRAM_NORMAL_RECORD_OFFSET % sizeof(ULONG)) == 0);
C_ASSERT(
    UFS_PRAM_NORMAL_RECORD_OFFSET +
    (UFS_PRAM_NORMAL_RECORD_DWORDS * sizeof(ULONG)) <=
    UFS_PRAM_LATCH_OFFSET
    );

//
// Exynos9820 PMU - the reset-to-recovery path.
//
// PlatformBootManagerLib.c documents this sequence as DEVICE-VERIFIED: sboot
// reads the Samsung SEC reset reason from PMU INFORM3 (+0x080C) and boots the
// RECOVERY partition when it holds 0x12345670 | 4, and a SoC software reset is
// PMU SWRESET (+0x0400) = 1. The same file records that a WDT-origin reset does
// NOT honour the reason, so this exact pair is the only path that lands in TWRP.
//
// That file also asserts the sequence "requires UEFI code to be running -
// impossible once the kernel has the CPU". That is true of UEFI's own writes,
// but not of a kernel-mode driver: MmMapIoSpace reaches the same registers. The
// PMU is at 0x14060000, far below the start of DRAM (0x80000000), so it is
// device MMIO that Windows never has in its PFN database - the same argument
// that makes the 0xFED14000 breadcrumb window mappable.
//
// This is what makes an unattended build/flash/measure loop possible: Windows
// can put the phone back into TWRP by itself, with no button presses.
//
#define UFS_PMU_PHYSICAL_BASE               0x15860000ULL   // VERIFIED (9820 system-controller; 9810 was 0x14060000)
#define UFS_PMU_WINDOW                      0x00004000UL
#define UFS_PMU_SWRESET                     0x00000400UL    // UNVERIFIED on 9820 (9810 offset)
#define UFS_PMU_RST_STAT                    0x00000404UL    // UNVERIFIED on 9820 (9810 offset)
#define UFS_PMU_INFORM2                     0x00000808UL    // UNVERIFIED on 9820 (9810 offset)
#define UFS_PMU_INFORM3                     0x0000080CUL    // UNVERIFIED on 9820 (9810 offset)
#define UFS_PMU_SYSIP_DAT0                  0x00000810UL    // UNVERIFIED on 9820 (9810 offset)
#define UFS_PMU_PS_HOLD_CONTROL             0x0000330CUL
#define UFS_PMU_PS_HOLD_DATA                0x00000100UL
#define UFS_PMU_SEC_POWER_RESET             0x12345678UL
#define UFS_PMU_REBOOT_REASON_NORMAL        0x12345670UL
#define UFS_PMU_REBOOT_REASON_RECOVERY      0x12345674UL

/*
 * Unattended-return watchdog.
 *
 * Re-armed once per tick rather than requested as one long interval, because a
 * single outstanding RequestTimerCall is all Storport guarantees and short
 * intervals are the portable case. UFS_UNATTENDED_REBOOT_SEC is therefore a
 * tick count, not a microsecond value.
 *
 * The deadline is deliberately longer than BOTH user-mode mechanisms - the
 * dashboard countdown (worst case (AUTO_REBOOT_MAX_DEFERRALS + 1) * 90 = 360 s)
 * and the helper watchdog (600 s) - so on a boot where either of those works
 * this timer never reaches its deadline and changes nothing.
 */
#define UFS_UNATTENDED_TICK_US              1000000UL
/*
 * How long the witness keeps refreshing on a boot that did NOT ask to be
 * returned. Long enough for the kernel to finish bringing secondary processors
 * online and settle, short enough that an idle machine is not woken forever.
 */
#define UFS_WITNESS_OBSERVE_SEC             90UL
#define UFS_UNATTENDED_REBOOT_SEC           900UL

/*
 * BOOT DIAGNOSTIC MODE
 *
 * The installed OS reaches the Windows logo from sda25 and stops. Every channel
 * that diagnosed the WinPE boots is unavailable there:
 *
 *   - no script runs, so there is no --log-lba and no UfsDiag at all
 *   - Windows brings up no USB gadget, so there is no ADB
 *   - the device is UEFI/GOP, so sos and bootlog text are not rendered - which
 *     is why boot 27 showed the logo and wrote no ntbtlog.txt despite both
 *     flags being set
 *
 * PRAM is the one channel that does work: boot 20 returned STARTIO=0x50D3,
 * MS10=0x16BB, DONE=0x50B5 and 71 write records from the installed OS. But it
 * only survives a WARM reboot, and a hang has to be ended by holding the power
 * button, which loses DRAM and with it the evidence.
 *
 * This mode closes that gap. The driver takes a numbered PRAM snapshot every
 * UFS_BOOT_DIAG_SNAP_SEC seconds and reboots itself to recovery after
 * UFS_BOOT_DIAG_REBOOT_SEC, so the phone returns on its own with the ring
 * intact. The snapshots are the actual measurement: comparing STARTIO across
 * them separates a driver that has stopped issuing I/O from one spinning in a
 * retry loop, and those two have opposite causes.
 *
 * Automatic resets are forbidden in the installation build. Keep the master
 * switch at zero even when temporarily enabling diagnostic snapshots.
 */
#define UFS_AUTOMATIC_RECOVERY_RESET         0
//
// W23 (S10e first boot): both OFF. They program PMU+0x408/0x40C (the 9810's WDT disable/mask) and
// the cl0 watchdog; the 9820 has no such PMU registers (exynos9-v1-wdt uses NONCPU_INT_EN 0x1244,
// notes/s10e-reboot-wdt-constants.md). Re-port before re-enabling.
//
#define UFS_FIRMWARE_WDT_BRIDGE              0
#define UFS_CONTINUOUS_HARDWARE_WATCHDOG     0
#define UFS_BOOT_DIAG_MODE                  0
#if UFS_CONTINUOUS_HARDWARE_WATCHDOG && !UFS_FIRMWARE_WDT_BRIDGE
#error UFS_CONTINUOUS_HARDWARE_WATCHDOG requires UFS_FIRMWARE_WDT_BRIDGE
#endif
#if !UFS_AUTOMATIC_RECOVERY_RESET && UFS_BOOT_DIAG_MODE
#error UFS_BOOT_DIAG_MODE requires UFS_AUTOMATIC_RECOVERY_RESET
#endif
#define UFS_BOOT_DIAG_SNAP_SEC              10UL
#define UFS_BOOT_DIAG_REBOOT_SEC            150UL
/* Seconds of write quiescence required before the diagnostic reboot may fire. */
#define UFS_BOOT_DIAG_WRITE_IDLE_SEC        5UL

/*
 * Storport's timer callback does not run in the installed-OS stall: the first
 * diagnostic build remained on the logo beyond UFS_BOOT_DIAG_REBOOT_SEC and
 * never returned to TWRP. The dispatch path does run - boot 20 recorded 20,691
 * StartIo calls - so mirror the sampler there and make it the primary return
 * mechanism. Four progress records plus the final record fit comfortably in
 * the 16 KB PRAM ring.
 */
#define UFS_BOOT_DIAG_IO_SNAP_INTERVAL      4096UL
#define UFS_BOOT_DIAG_IO_REBOOT_THRESHOLD  16384UL

/*
 * Exynos9820 cluster-0 hardware watchdog.
 *
 * The firmware deliberately compiles its own EBS watchdog out
 * (STAR2LTE_EBS_HW_WATCHDOG 0) for a documented and correct reason: "Windows has
 * no driver that services this firmware-owned watchdog, so it resets a perfectly
 * healthy Windows boot a few seconds after handoff."
 *
 * This driver is that missing servicer. It arms the watchdog once the adapter is
 * up and pets it from the 1 Hz unattended timer, which converts the watchdog from
 * a hazard into the last line of unattended recovery: anything that stops the
 * timer - a bugcheck, a wedged DPC, a hung adapter - stops the pets, so the SoC
 * resets into TWRP by itself. Today every one of those cases strands the phone
 * until somebody holds the buttons, which is the single largest obstacle to an
 * unattended build/flash/measure loop.
 *
 * Note the ordering guarantee this depends on: UfsPmuRebootToRecovery already
 * proves INFORM3 survives a warm reset, so arming is only safe once INFORM3 has
 * been set to the recovery reason - otherwise a timeout would reset into Windows
 * and loop invisibly instead of landing somewhere adb can reach.
 *
 * Prescaler 0xFF with divider 128 is the LONGEST period this block can produce.
 * The pet interval is 1 s, so the margin is enormous; the point of maximising it
 * is that the same value is what a future firmware-side arm would inherit, where
 * the window is "EBS until the storage driver loads" rather than one tick.
 */
#define UFS_WDT_PHYSICAL_BASE               0x10050000ULL   // VERIFIED (watchdog_cl0 is at the same base on 9820)
#define UFS_WDT_WINDOW                      0x00001000UL
#define UFS_WDT_WTCON                       0x00000000UL
#define UFS_WDT_WTDAT                       0x00000004UL
#define UFS_WDT_WTCNT                       0x00000008UL
#define UFS_WDT_CON_RESET_ENABLE            0x00000001UL
#define UFS_WDT_CON_ENABLE                  0x00000020UL
#define UFS_WDT_CON_DIVIDER_128             0x00000018UL
#define UFS_WDT_CON_PRESCALER_MAX           0x0000FF00UL
#define UFS_WDT_CON_ARM                     (UFS_WDT_CON_PRESCALER_MAX | \
                                             UFS_WDT_CON_DIVIDER_128 | \
                                             UFS_WDT_CON_ENABLE | \
                                             UFS_WDT_CON_RESET_ENABLE)
/* Exact alternate control used by the M3 probe firmware lineage. */
#define UFS_WDT_CON_FIRMWARE_ARM            0x00005C39UL
#define UFS_WDT_COUNT                       0x0000FFF5UL

/*
 * Safety floor for the measured watchdog period.
 *
 * The arm above uses the maximum prescaler and divider, which by the register
 * math is ~30s even at the slowest plausible input clock. But the firmware
 * disabled its own EBS arm because an unserviced watchdog "resets a perfectly
 * healthy Windows boot a few seconds after handoff" - an order of magnitude
 * shorter than that math predicts. One of the two is wrong, and until the
 * on-device measurement says which, the possibility that the real period is
 * only a few seconds has to be treated as live.
 *
 * That matters because pets ride a 1s Storport timer whose DPC can be delayed
 * by a poll loop at DISPATCH_LEVEL. A few seconds of margin is not enough, and
 * a missed pet on a healthy boot is a reset loop - strictly worse than the hang
 * this is meant to fix. So the first pet, which is also the first real
 * measurement, disarms the watchdog outright if the period it just measured is
 * below this floor. The measurement is still recorded, which is the part that
 * unblocks the firmware-side decision.
 */
#define UFS_WDT_MIN_TIMEOUT_SEC             10UL

/*
 * The watchdog can only reset the SoC if the PMU is not masking cluster-0's
 * reset request. Both registers live inside the PMU window this driver already
 * maps, so no second mapping is needed for them.
 */
#define UFS_PMU_WDT_DISABLE                 0x00000408UL
#define UFS_PMU_WDT_MASK_RESET              0x0000040CUL
#define UFS_WDT_CLUSTER0_RESET_BIT          0x01000000UL

C_ASSERT(UFS_PMU_SWRESET < UFS_PMU_WINDOW);
C_ASSERT(UFS_PMU_RST_STAT < UFS_PMU_WINDOW);
C_ASSERT(UFS_PMU_INFORM2 < UFS_PMU_WINDOW);
C_ASSERT(UFS_PMU_INFORM3 < UFS_PMU_WINDOW);
C_ASSERT(UFS_PMU_SYSIP_DAT0 < UFS_PMU_WINDOW);
C_ASSERT(UFS_PMU_WDT_DISABLE < UFS_PMU_WINDOW);
C_ASSERT(UFS_PMU_WDT_MASK_RESET < UFS_PMU_WINDOW);
C_ASSERT(UFS_PMU_PS_HOLD_CONTROL < UFS_PMU_WINDOW);
C_ASSERT(UFS_WDT_WTCNT < UFS_WDT_WINDOW);

//
// Bytes of the last data-in transfer mirrored into telemetry. The V15/V16 boots
// both reported a 4096-byte READ(10) completing with OCS=0 and residual 0 while
// the buffer stayed all zeros, and the only way to see that was a photograph of
// the screen. Capturing a prefix here puts the answer in the PRAM record, so
// "did the DMA actually land" is decidable from the host over adb.
//
// UFS_LAST_DATA_PREFIX itself is part of the diagnostic wire format and is
// defined in Exynos9820UfsDiag.h, which this header includes above.
//

//
// Granularity of the per-block delivery mask. Deliberately the fixed 4096-byte
// UFS logical block of this device rather than Adapter->LogicalBlockSize: the
// mask is evidence about the transfer, and pinning it to a constant means a
// recorded mask still means the same thing if the learned block size is ever
// wrong - which is precisely the state this telemetry exists to detect.
//
// The cap is the widest transfer the bounce can describe (UFS_BOUNCE_SIZE /
// 4096 = 32) and is also the width of the ULONG that carries it, so the shift
// below can never be undefined.
//
#define UFS_MULTI_BLOCK_GRANULE             4096UL
#define UFS_MULTI_BLOCK_MAX_GRANULES        32UL

#define UFS_REG_CAP                         0x0000UL
#define UFS_REG_VERSION                     0x0008UL
#define UFS_REG_INTERRUPT_STATUS            0x0020UL
#define UFS_REG_INTERRUPT_ENABLE            0x0024UL
#define UFS_REG_HOST_STATUS                 0x0030UL
#define UFS_REG_HOST_ENABLE                 0x0034UL
#define UFS_REG_INTERRUPT_AGGREGATION       0x004CUL
#define UFS_REG_UTRL_BASE_LOW               0x0050UL
#define UFS_REG_UTRL_BASE_HIGH              0x0054UL
#define UFS_REG_UTRL_DOORBELL               0x0058UL
#define UFS_REG_UTRL_LIST_CLEAR             0x005CUL   // W27: UTRLCLR; Exynos writes the slot BIT (inverted polarity), as Beyond0LteUfsHcDxe does
#define UFS_REG_UTRL_RUN_STOP               0x0060UL
#define UFS_REG_UTRL_CLEAR_NOT_READY         0x0064UL

#define UFS_VENDOR_OFFSET                   0x1100UL
#define UFS_VENDOR_TX_PRDT_SIZE             (UFS_VENDOR_OFFSET + 0x00UL)
#define UFS_VENDOR_RX_PRDT_SIZE             (UFS_VENDOR_OFFSET + 0x04UL)
#define UFS_VENDOR_NEXUS_TYPE               (UFS_VENDOR_OFFSET + 0x40UL)
#define UFS_VENDOR_DATA_REORDER              (UFS_VENDOR_OFFSET + 0x60UL)
#define UFS_VENDOR_AXI_DMA_BURST             (UFS_VENDOR_OFFSET + 0x6CUL)

#define UFS_HOST_ENABLE                     0x00000001UL
#define UFS_HCI_VERSION_21                  0x00000210UL
#define UFS_HOST_STATUS_DEVICE_PRESENT      0x00000001UL
#define UFS_HOST_STATUS_UTRL_READY           0x00000002UL
#define UFS_HOST_STATUS_UIC_READY            0x00000008UL
#define UFS_HOST_STATUS_WARM_REQUIRED        \
    (UFS_HOST_STATUS_DEVICE_PRESENT | UFS_HOST_STATUS_UTRL_READY | UFS_HOST_STATUS_UIC_READY)
#define UFS_INTERRUPT_ENABLE_REQUIRED        0x00030EF5UL
#define UFS_INTERRUPT_TRANSFER_COMPLETION    0x00000001UL
//
// UFSHCI 2.1 Interrupt Status (IS, offset 0x20) is a volatile RW1C latch. Most
// of its bits are informational and are re-latched autonomously by the link --
// auto-hibernate enter/exit in particular fires while the driver is polling.
// Only the fatal classes below mean the controller can no longer be trusted.
//
#define UFS_INTERRUPT_STATUS_UTP_ERROR       0x00000200UL
#define UFS_INTERRUPT_STATUS_DEVICE_FATAL    0x00010000UL
#define UFS_INTERRUPT_STATUS_HOST_FATAL      0x00020000UL
#define UFS_INTERRUPT_STATUS_SYSTEM_BUS      0x00040000UL
#define UFS_INTERRUPT_STATUS_CRYPTO_ENGINE   0x00080000UL
#define UFS_INTERRUPT_STATUS_FATAL_MASK      \
    (UFS_INTERRUPT_STATUS_UTP_ERROR | \
     UFS_INTERRUPT_STATUS_DEVICE_FATAL | \
     UFS_INTERRUPT_STATUS_HOST_FATAL | \
     UFS_INTERRUPT_STATUS_SYSTEM_BUS | \
     UFS_INTERRUPT_STATUS_CRYPTO_ENGINE)
#define UFS_CAP_64_BIT_ADDRESSING            0x01000000UL
#define UFS_CAP_NUTRS_MASK                   0x0000001FUL
#define UFS_LIST_RUN_STOP                    0x00000001UL

#define UFS_UTRL_ALIGNMENT                   1024UL
#define UFS_UTRL_REGION_SIZE                 1024UL
#define UFS_UCD_ALIGNMENT                    128UL
#define UFS_UCD_COMMAND_OFFSET               0UL
#define UFS_UCD_RESPONSE_OFFSET              1024UL
#define UFS_UCD_PRDT_OFFSET                  2048UL
#define UFS_UCD_REGION_SIZE                  4096UL
#define UFS_PRDT_ENTRY_SIZE                  128UL
#define UFS_FMP_PRDT_DATA_BYTE_COUNT_MASK    0x0003FFFFUL
/*
 * The controller advances the FMP crypto data-unit number once per 4096 bytes
 * and requires ONE PRDT ENTRY PER DATA UNIT. A single entry spanning two units
 * transfers only the first one and then raises Host Controller Fatal Error
 * Status.
 *
 * Measured on device, run pram-20260729-201048: an 8192-byte READ(10) described
 * by one 8192-byte entry returned IS=0x00020001 - bit 17 HOST_FATAL, with bits
 * 9 UTP_ERROR, 16 DEVICE_FATAL, 18 SYSTEM_BUS and 19 CRYPTO_ENGINE all clear,
 * so neither the device nor the AXI fabric nor the crypto engine objected - and
 * NZBLK=0x1 with DIN2 all zero, meaning the first 4096 bytes landed ("EFI PART"
 * at LBA 1) and nothing after them did. The 16384-byte case behaved identically:
 * one granule, same bit. Splitting a transfer into one entry per segment is what
 * makes it legal, and is what Linux does - ufs-exynos.c sets
 * UFSHCD_QUIRK_PRDT_BYTE_GRAN, under which prd_table_length is a BYTE count
 * rather than an entry count.
 *
 * The PRDT lives in the tail of the command descriptor, so its region size is
 * what caps a single transfer: 2048 bytes / 128 per entry = 16 entries = 64 KB.
 */
#define UFS_PRDT_SEGMENT_SIZE                4096UL
#define UFS_PRDT_REGION_SIZE                 (UFS_UCD_REGION_SIZE - UFS_UCD_PRDT_OFFSET)
#define UFS_PRDT_MAX_ENTRIES                 (UFS_PRDT_REGION_SIZE / UFS_PRDT_ENTRY_SIZE)
#define UFS_MAX_TRANSFER_LENGTH              (UFS_PRDT_MAX_ENTRIES * UFS_PRDT_SEGMENT_SIZE)
#define UFS_BOUNCE_SIZE                      (128UL * 1024UL)
/*
 * The Exynos FMP PRDT descriptor carries a crypto/bypass data-unit number that
 * the controller advances once per 4096 bytes from the START of the entry. When
 * the buffer's physical base is not 4 KB aligned those unit boundaries no longer
 * coincide with the device's real 4 KB blocks, and a data-in transfer that spans
 * a page boundary is silently dropped instead of faulting: V15 measured
 * BOUNCE_PA=0xBBCD8400 (1 KB aligned), so every transfer <= 0xC00 bytes landed
 * (INQUIRY, READ CAPACITY, REQUEST SENSE, MODE SENSE) while the 4096-byte
 * READ_10 reported OCS=0 with a residual of 0 and returned the pre-zeroed
 * buffer. Aligning the bounce to a page removes the whole failure class.
 */
#define UFS_BOUNCE_ALIGNMENT                 4096UL
#define UFS_MAX_PHYSICAL_BREAKS              ((UFS_BOUNCE_SIZE / PAGE_SIZE) + 1UL)
#define UFS_WORKSPACE_SIZE                   0x00024000UL

#define UFS_TRANSFER_SLOT                    0UL
#define UFS_TRANSFER_SLOT_MASK               0x00000001UL
#define UFS_TRANSFER_TIMEOUT_US             5000000UL
#define UFS_TRANSFER_POLL_US                 10UL

/*
 * Samsung FMP DMA window.
 *
 * The UFS controller's DMA is restricted to a low-memory region; addresses
 * outside it are not reachable. The 9810's aperture (0x90000000..0xBC800000)
 * is ILLEGAL on this SoC: the live DTB declares bank0 as 0x80000000 + 0x3AB00000
 * (ends 0xBAB00000 - notes/K10-rkp.md T18, confirmed by /proc/iomem "System RAM
 * 80000000-baafffff"), so the old upper bound overran the declared bank by
 * 0x1D00000.
 *
 * The window below is the free band inside bank0, above every reserved-memory
 * carveout read from the live DTB (notes/s10e-dtb-mmio.md S02): the camera
 * reservation ends at 0x99000000 + 0xE400000 = 0xA7400000, and bank0 ends at
 * 0xBAB00000. It is a CHOSEN band, not a hardware-read aperture, so it stays
 * UNVERIFIED until a device validation says otherwise.
 */
#define UFS_DMA_WINDOW_FIRST                0x00000000A7400000ULL   // UNVERIFIED: chosen band, above the camera reservation (live DTB)
#define UFS_DMA_WINDOW_LAST                 0x00000000BAB00000ULL   // UNVERIFIED: bank0 end (DTB reg + /proc/iomem)
C_ASSERT(UFS_DMA_WINDOW_FIRST < UFS_DMA_WINDOW_LAST);
#define UFS_STOP_TIMEOUT_US                  100000UL

/*
 * How many times a quiesced transfer list may be re-armed before the adapter
 * is declared genuinely dead. Bounded so a controller that really is wedged
 * still ends up latched off instead of re-arming on every request forever.
 */
#define UFS_MAX_REARM_ATTEMPTS               16UL
#define UFS_LOGICAL_BLOCK_SIZE               4096UL
#define UFS_MIN_LOGICAL_BLOCK_SIZE            512UL
#define UFS_MAX_LOGICAL_BLOCK_SIZE           8192UL
#define UFS_READ_CAPACITY10_LENGTH              8UL
#define UFS_READ_CAPACITY16_LENGTH             12UL
#define UFS_MAX_CAPACITY_RETRY_REQUESTS          3UL

/*
 * ---------------------------------------------------------------------------
 * WRITE FENCE
 * ---------------------------------------------------------------------------
 *
 * Read from the live device, not assumed. The S10e's own GPT was captured in
 * TWRP as root (dd of LBA 1 plus the entry array, parsed in
 * notes/s10e-root-reads.md §2) and its starts and ends equal Samsung's own
 * BEYOND0LTE_EUR_OPEN.pit exactly. This disk's GPT addresses partitions in
 * 4096-byte LBAs, which is the unit this driver speaks - there is no divide-by-8
 * step as there was for the 9810's sysfs read.
 *
 *   sda28  CACHE / ESP     LBA 1983488 .. 2137087    bootmgfw.efi + the BCD
 *   sda31  USERDATA        LBA 2151168 .. 31234815   the Windows volume
 *   tail                   LBA 31234816 .. 31236090  claimed by nothing
 *
 * The role of the Windows volume MOVES between the two phones: on the 9810 it
 * lived in the SYSTEM slot (sda18) with USERDATA protected, and here it IS
 * USERDATA, the last partition on the disk, with the ESP (CACHE) sitting
 * immediately below it and two real partitions in between (omr, cp_debug).
 * So the fence spans ESP..tail and the two partitions inside it are carved
 * back out by the protected ranges below.
 *
 * Excluded by the fence's lower bound, which is where the unrecoverable things
 * live:
 *
 *   sda3   EFS        LBA  4096 ..   9215   IMEI + RF calibration, UNRECOVERABLE
 *   sda14  BOOT       LBA 27264 ..  41343   UEFI
 *   sda15  RECOVERY   LBA 41344 ..  57855   TWRP - the way back
 *   GPT              LBA     0 ..      5    (no backup copy on this disk)
 *
 * A write that escapes this window can end the project, so the bound is a
 * compile-time constant checked on every command rather than a policy applied
 * somewhere upstream.
 *
 * The tail was measured before it was admitted, not assumed free: every LBA
 * from 31234812 to 31236095 - the end of the media - reads back zero, and the
 * only non-zero LBA in the measured run is 31234811, inside USERDATA
 * (notes/s10e-root-reads.md §9, two dd reads pulled and parsed locally). So
 * this disk carries NO backup GPT at all, even though its primary header
 * declares AltLBA = 31236095. The five LBAs the header reserves for that copy
 * are still refused, so a future reflash that does mirror the table is not
 * silently trampled.
 */
/*
 * 9820 PORTING - done. Every LBA in this block is the S10e's own table, read
 * from the live GPT (notes/s10e-root-reads.md §2) and cross-checked against
 * Samsung's PIT. The C_ASSERTs below were re-derived in the same step; a
 * half-edit still fails the compile.
 */
#define UFS_WRITE_FENCE_FIRST_LBA            1983488ULL   // VERIFIED (sda28 CACHE/ESP start, live GPT)
#define UFS_WRITE_FENCE_LAST_LBA             31236090ULL  // VERIFIED (this disk's LastUsableLBA, header +0x30)

/*
 * PROTECTED PARTITIONS - sda29 OMR and sda30 CP_DEBUG, the two real
 * partitions that fall INSIDE the widened fence, between the ESP and USERDATA.
 *
 * Deliberately expressed as its own bound rather than folded into the fence,
 * for the same reason the GPT guard is: it must still hold if a fence constant
 * is ever edited incorrectly. It runs alongside the GPT guard, BEFORE the
 * fence, so a request landing in a real partition is refused by a check that
 * does not depend on the fence being right.
 *
 * The layout it describes is printed next to the constants below. What matters
 * here is that the run is contiguous: sda26 begins exactly where sda25 (system)
 * ends and sda31 ends exactly where the unallocated tail begins, both verified
 * from the live GPT.
 */
/*
 * FULL WINDOWS MODE
 *
 * Everything above describes a driver whose job was to prove a read path and
 * never touch the media. That job is done. This build has a different one: it
 * is the boot-start disk driver for a real Windows installation living on
 * sda31 USERDATA, and a disk that refuses writes is not a disk Windows can run
 * from.
 *
 * The safeguards are therefore re-aimed, not removed. What must never be
 * writable is what cannot be rebuilt from the host - and that set is unchanged
 * in kind, only in number:
 *
 *   sda3  EFS       IMEI + RF calibration, UNRECOVERABLE
 *   sda14 BOOT      UEFI
 *   sda15 RECOVERY  TWRP, the way back
 *   GPT   LBA 0..5  (this disk has no backup copy; the last 5 LBAs are refused)
 *
 * all of which sit below the fence's lower bound (1983488) or outside its
 * upper bound, so the fence alone still refuses them, exactly as before.
 */
#define UFS_FULL_WINDOWS_MODE                1

/*
 * Readout boot.  [HISTORY - every paragraph below, up to the last one, is the
 * 9810's readout experiment: the LBAs and boot numbers in them are the 9810's,
 * kept because they are the evidence for why this switch exists. The last
 * paragraph and the switch itself speak of this device.]
 *
 * Comes up ARMED but confined to sda18 alone, so this boot cannot be the one
 * that crashes yet can still write down what it found.
 *
 * Why it exists: the crash phase is latched at PRAM +0x3F48, which survives a
 * bugcheck because it sits above the ring. But PRAM cannot carry it to TWRP -
 * measured directly on the v18 readout boot, whose ring came back as 579 B of
 * pure EDK2 single-byte BDS trace with no driver record in it at all. The
 * reboot re-runs Star2LteTraceResetLog, which re-seeds the magic and zeroes the
 * cursor, so every UEFI pass wipes whatever Windows appended. PRAM is a
 * within-boot channel only.
 *
 * The only channel that survives a reboot is the media itself. sda18 is the
 * right target: it is FAT, TWRP reads it directly, its entire contents are
 * reproducible from the host, and it is already inside the write fence -
 * UFS_WRITE_FENCE_FIRST_LBA is literally sda18's first LBA (68736, confirmed
 * against the on-device partition table), and it is below every protected
 * range. So writing there needs no new permission, only that nothing else is
 * permitted to crash the boot first.
 *
 * Hence the window: writes to sda18 proceed, writes to the ESP (sda21) and the
 * Windows volume (sda25) are refused at the gate before a descriptor is built.
 * Those two are the ones a real boot performs and the ones currently ending in
 * KMODE_EXCEPTION_NOT_HANDLED. Boot 1 is the proof the refused state is
 * survivable: all 28 of bcdboot's writes were refused (WTRIED=28 WPROT=28
 * WISSUED=0) and the boot still completed with its telemetry intact.
 *
 * Refusals land on the fence counters (WFENCE) rather than the protected-
 * partition ones, so a readout refusal stays distinguishable from the ordinary
 * guard's.
 *
 * Set to 0 to restore normal full-Windows arming.
 *
 * Now 0, and this is the whole point of the exercise.
 *
 * The readout boot did its job: it proved the miniport binds, arms, and writes
 * verified data back on this hardware. But this window confines writes to sda18
 * and refuses the ESP and sda25 by design, so it can never let bcdboot finish.
 * Boot 16 is that in one line - WMODE=2 (armed), WTRIED=28, WFENCE=28,
 * WISSUED=0. Every one of bcdboot's writes was refused by this window, not by
 * the arm state and not by the hardware.
 *
 * Turning it off does not widen the writable set beyond what FULL_WINDOWS_MODE
 * already sanctions: on THIS device the fence bounds writes to 1983488..31236090
 * (the ESP through the GPT's LastUsableLBA), the two protected ranges still
 * refuse sda29 OMR and sda30 CP_DEBUG, and EFS, BOOT (UEFI), RECOVERY, the
 * whole Android run below the fence and the five LBAs the header reserves for
 * the backup GPT remain outside it entirely. What this restores is exactly the
 * ESP (sda28), the Windows volume (sda31) and the verified-zero tail - the
 * things an installation must be able to write.
 */
#define UFS_READOUT_BOOT                     0

/*
 * READOUT WINDOW - scratch region ONLY.
 *
 * [HISTORY - the LBAs quoted in the three paragraphs below are the 9810's own
 * partition table, kept because they are the evidence for the SHAPE of this
 * window, not its numbers. The constants after it are this device's.]
 *
 * v19 opened this to sda18 (68736..1195135) so WinPE could persist a log to
 * FAT. That boot died: it returned in under 128 s against the DISARMED v18
 * control's 147.3 s, wrote nothing to sda18, and never reached the write
 * ladder. Permitting kernel-issued writes to a mounted volume kills the boot
 * even when the volume is FAT and the LBAs are the lowest on the disk - so the
 * crash is not about WHICH region is written, it is about the write being
 * issued by the filesystem stack at all.
 *
 * So the window is now the scratch region and nothing else. Scratch is one
 * block past the last partition's last LBA and below the backup GPT: no
 * partition contains it, no volume mounts it, and no filesystem ever touches
 * it. Every mount-time and bcdboot write is therefore refused before a
 * descriptor is built - the configuration v18 proved survivable - while
 * usermode pass-through writes to scratch, the exact path that passed the
 * 2026-07-31 ladder at 4/16/32/64 KB, remain permitted.
 *
 * That makes the next boot a clean single-variable test: same rebuilt binary,
 * same hardware, only the issuer differs.
 */
#define UFS_READOUT_WINDOW_FIRST_LBA         31234816UL   // S10e tail start, one LBA past USERDATA
#define UFS_READOUT_WINDOW_LAST_LBA          31235071UL   // 256 LBAs, verified zero (notes §9)

/*
 * The scratch region, mirrored here because UfsDiag.c's UFS_SCRATCH_LBA is a
 * usermode definition the driver cannot see. 256 blocks starting one LBA past
 * USERDATA's last (31234815), i.e. the same run UfsDiag.c calls its scratch
 * region. Verified free on THIS device: the whole run reads back zero
 * (notes/s10e-root-reads.md §9). The "UFSSCRATCHBLK" signature UfsDiag.c
 * looks for was stamped on the 9810's tail and nothing has been written here
 * yet, so that check fails on the S10e until the scratch pass runs - a
 * device-validation gap, not a wrong constant.
 */
#define UFS_READOUT_SCRATCH_FIRST_LBA        31234816UL
#define UFS_READOUT_SCRATCH_LAST_LBA         31235071UL

/*
 * PROTECTED PARTITIONS - the real partitions that fall INSIDE the fence, i.e.
 * between the ESP and the Windows volume. On this device that is sda29 OMR and
 * sda30 CP_DEBUG; the rest of the Android run (sda26 VENDOR, sda27 PRODUCT) is
 * already below the fence's lower bound and is refused by the fence itself.
 *
 * Deliberately expressed as its own bound rather than folded into the fence,
 * for the same reason the GPT guard is: it must still hold if a fence constant
 * is ever edited incorrectly. It runs alongside the GPT guard, BEFORE the
 * fence, so a request landing in a real partition is refused by a check that
 * does not depend on the fence being right.
 *
 *   sda26  VENDOR     LBA 1543168 .. 1824767   below the fence, refused by it
 *   sda27  PRODUCT    LBA 1824768 .. 1983487   below the fence, refused by it
 *   sda28  CACHE      LBA 1983488 .. 2137087   WRITABLE  <- the ESP
 *   sda29  OMR        LBA 2137088 .. 2149887   refused   (range A)
 *   sda30  CP_DEBUG   LBA 2149888 .. 2151167   refused   (range B)
 *   sda31  USERDATA   LBA 2151168 .. 31234815  WRITABLE  <- Windows
 *   tail             LBA 31234816 .. 31236090  WRITABLE  <- scratch, verified zero
 *
 * The two refused partitions are contiguous here, so either range alone would
 * cover both; they are kept split so a half-edited pair still fails the C_ASSERTs
 * below rather than silently admitting one of them.
 */
#if UFS_FULL_WINDOWS_MODE
#define UFS_PROTECTED_PARTITION_FIRST_LBA    2137088ULL   // VERIFIED (sda29 OMR start, live GPT)
/*
 * The writable set is no longer contiguous, so the guard is two ranges.
 *
 * sda28 CACHE is this device's ESP - it holds bootmgfw.efi and the BCD, and
 * bcdboot has to write the boot store there or Windows has nothing to boot
 * from. sda31 USERDATA carries the Windows volume itself. Those two are
 * released; every other partition inside the fence stays refused.
 *
 * This is the mirror image of the 9810's layout: there the Windows volume was
 * the SYSTEM slot (sda18) and USERDATA (sda25) had to be carved out as
 * protected, whereas here USERDATA IS the Windows volume and the partitions
 * that must stay refused sit between it and the ESP.
 */
#define UFS_PROTECTED_PARTITION_LAST_LBA     2149887ULL  // VERIFIED (sda29 OMR end)
#define UFS_PROTECTED_PARTITION2_FIRST_LBA   2149888ULL  // VERIFIED (sda30 CP_DEBUG start)
#define UFS_PROTECTED_PARTITION2_LAST_LBA    2151167ULL  // VERIFIED (sda30 CP_DEBUG end)
C_ASSERT(UFS_PROTECTED_PARTITION2_FIRST_LBA > UFS_PROTECTED_PARTITION_LAST_LBA);
C_ASSERT(UFS_PROTECTED_PARTITION2_FIRST_LBA < UFS_PROTECTED_PARTITION2_LAST_LBA);
C_ASSERT(UFS_PROTECTED_PARTITION2_LAST_LBA < UFS_WRITE_FENCE_LAST_LBA);
/* The ESP must stay writable: range A begins after the CACHE partition ends. */
C_ASSERT(UFS_PROTECTED_PARTITION_FIRST_LBA > 2137087ULL);
/* USERDATA must stay writable: range B ends one LBA before it starts. */
C_ASSERT(UFS_PROTECTED_PARTITION2_LAST_LBA < 2151168ULL);
/* The tail must stay writable: range B ends well before it starts. */
C_ASSERT(UFS_PROTECTED_PARTITION2_LAST_LBA < 31234816ULL);
#else
/*
 * A build that refuses writes does not need the split: the whole Android run,
 * VENDOR through CP_DEBUG inclusive, is refused as one range.
 */
#define UFS_PROTECTED_PARTITION_FIRST_LBA    1543168ULL  // VERIFIED (sda26 VENDOR start)
#define UFS_PROTECTED_PARTITION_LAST_LBA     2151167ULL  // VERIFIED (sda30 CP_DEBUG end)
#endif

/*
 * Independent of the fence, so a mis-set fence constant still cannot destroy
 * the partition table. The geometry comes from this device's own GPT header,
 * read at LBA 1 (notes/s10e-root-reads.md §8): the media is 127 943 049 216
 * bytes = 31 236 096 LBAs, so LastLba is 31 236 095, the header declares the
 * backup copy at AltLBA = 31 236 095 and carries LastUsableLBA = 31 236 090.
 * The backup region is the five LBAs 31 236 091 .. 31 236 095 - and on the
 * stock media it is empty (§9 read it back all zero), but it is refused
 * regardless so a reflashed disk that does mirror the table stays protected.
 */
#define UFS_GPT_PRIMARY_LAST_LBA             5ULL        // VERIFIED (128 entries x 128 B at LBA 2..5, live GPT)
#define UFS_GPT_BACKUP_FIRST_LBA             31236091ULL
#define UFS_EXPECTED_LAST_LOGICAL_BLOCK       31236095ULL
C_ASSERT(UFS_EXPECTED_LAST_LOGICAL_BLOCK == (UFS_GPT_BACKUP_FIRST_LBA + 4ULL));

/*
 * One command may never span more than the bounce buffer, which also caps how
 * much a single malformed request can touch.
 */
#define UFS_WRITE_MAX_BLOCKS                 (UFS_MAX_TRANSFER_LENGTH / UFS_LOGICAL_BLOCK_SIZE)

C_ASSERT(UFS_WRITE_FENCE_FIRST_LBA > UFS_GPT_PRIMARY_LAST_LBA);
C_ASSERT(UFS_WRITE_FENCE_LAST_LBA < UFS_GPT_BACKUP_FIRST_LBA);
C_ASSERT(UFS_WRITE_FENCE_FIRST_LBA < UFS_WRITE_FENCE_LAST_LBA);
/* The fence must not overlap BOOT (sda14 ends at LBA 41343). */
C_ASSERT(UFS_WRITE_FENCE_FIRST_LBA > 41343ULL);
/* The fence must not overlap RECOVERY (sda15 ends at LBA 57855). */
C_ASSERT(UFS_WRITE_FENCE_FIRST_LBA > 57855ULL);
/* The fence must not overlap EFS (sda3 ends at LBA 9215). */
C_ASSERT(UFS_WRITE_FENCE_FIRST_LBA > 9215ULL);

/*
 * The protected block must be consistent with the fence. The two modes differ
 * here, and the difference is deliberate: in FULL_WINDOWS_MODE the refused
 * partitions sit BETWEEN the ESP (at the fence's lower edge) and USERDATA, so
 * the block is strictly INSIDE the fence. In a refusing build the run starts at
 * VENDOR, below the fence's lower bound, because VENDOR and PRODUCT are already
 * refused by the fence itself and this guard has to cover the rest. Asserting
 * "strictly inside" in that mode would be a claim about a layout that does not
 * exist there, and it is exactly the assert that broke when the fence moved.
 */
C_ASSERT(UFS_PROTECTED_PARTITION_FIRST_LBA < UFS_PROTECTED_PARTITION_LAST_LBA);
C_ASSERT(UFS_PROTECTED_PARTITION_LAST_LBA < UFS_WRITE_FENCE_LAST_LBA);
C_ASSERT(UFS_PROTECTED_PARTITION_FIRST_LBA > UFS_GPT_PRIMARY_LAST_LBA);
#if UFS_FULL_WINDOWS_MODE
C_ASSERT(UFS_PROTECTED_PARTITION_FIRST_LBA > UFS_WRITE_FENCE_FIRST_LBA);
#else
C_ASSERT(UFS_PROTECTED_PARTITION_FIRST_LBA < UFS_WRITE_FENCE_FIRST_LBA);
#endif

/*
 * IN-FLIGHT GPT HEADER TRANSLATION.
 *
 * Windows only accepts a GPT header whose HeaderSize is exactly 92; any other
 * value makes the disk read as corrupt (STATUS_DISK_CORRUPT_ERROR) before the
 * header CRC is checked. Both the primary (LBA 1) and the backup (last LBA) of
 * this disk declare HeaderSize = 512, so the disk cannot be classified and
 * partmgr creates ZERO partitions - which is exactly the observed
 * PARTITION_N = 0.
 *
 * UEFI 2.10 Table 5-5 permits HeaderSize anywhere in [92, logical block size],
 * so the disk is spec-correct and Windows is stricter than the spec. Rewriting
 * the media is therefore not an option worth taking: it would mutate a
 * spec-valid partition table permanently, and it would have to defeat this
 * driver's own GPT guard to do it. Instead the header is translated IN FLIGHT,
 * in the read buffer only, so the media is never touched and reverting is just
 * reflashing the WIM.
 *
 * The translation is only ever applied to a header that has FIRST been proven
 * spec-valid: correct signature, revision 0x00010000, 92 < HeaderSize <= the
 * bytes actually present, and - decisively - the stored CRC32 over HeaderSize
 * bytes must validate. A block that merely happens to begin with 'EFI PART'
 * cannot pass that, which is what makes it safe to scan every logical-block
 * boundary in the transfer rather than only offset 0. (partmgr/ClassPnP may
 * issue multi-block reads, so the header is not always at offset 0.)
 *
 * Fixing up is six bytes: HeaderSize <- 92 and the CRC recomputed over those
 * 92 bytes. HeaderSize lives at offset 12, INSIDE the CRC'd region, so the CRC
 * must be recomputed AFTER the size is changed. For this disk that yields
 * 0x5E2BF25B, and Windows then accepts the header.
 *
 * The partition ENTRY ARRAY (LBA 2..5) is deliberately left alone - its CRC
 * (0x67F976EE) already validates and the array is not size-gated.
 */
#define UFS_GPT_HEADER_SIGNATURE_0           0x20494645UL   /* 'EFI ' */
#define UFS_GPT_HEADER_SIGNATURE_1           0x54524150UL   /* 'PART' */
#define UFS_GPT_HEADER_REVISION_1_0          0x00010000UL
#define UFS_GPT_HEADER_SIZE_WINDOWS          92UL
#define UFS_GPT_HEADER_OFFSET_REVISION       8UL
#define UFS_GPT_HEADER_OFFSET_HEADER_SIZE    12UL
#define UFS_GPT_HEADER_OFFSET_HEADER_CRC     16UL
#define UFS_GPT_CRC32_POLYNOMIAL             0xEDB88320UL
#define UFS_GPT_HEADER_CRC_LENGTH            4UL

/*
 * Outcome of examining one candidate block. IGNORED covers both "this is not a
 * GPT header" and "this header already declares 92", neither of which is worth
 * counting. REJECTED is reserved for the one case that warrants investigation:
 * a block that carries the GPT signature and revision and would have needed a
 * rewrite, but whose stored CRC32 did not validate - so we refused to touch it.
 */
#define UFS_GPT_XLATE_IGNORED                0UL
#define UFS_GPT_XLATE_TRANSLATED             1UL
#define UFS_GPT_XLATE_REJECTED               2UL

C_ASSERT(UFS_GPT_HEADER_SIZE_WINDOWS == 92UL);
C_ASSERT(UFS_GPT_HEADER_OFFSET_HEADER_SIZE < UFS_GPT_HEADER_SIZE_WINDOWS);
C_ASSERT(UFS_GPT_HEADER_OFFSET_HEADER_CRC < UFS_GPT_HEADER_SIZE_WINDOWS);
C_ASSERT((UFS_GPT_HEADER_OFFSET_HEADER_CRC + UFS_GPT_HEADER_CRC_LENGTH) <=
         UFS_GPT_HEADER_SIZE_WINDOWS);

/*
 * WRITE-PROTECT ADVERTISEMENT.
 *
 * The miniport used to OR MODE_DSP_WRITE_PROTECT into every MODE SENSE reply
 * while disarmed. That is now off, and the reason is a timing fact rather than
 * a change of policy about safety.
 *
 * Boot order is: drvload -> pnputil /scan-devices -> volume mount (which is
 * where MODE SENSE happens) -> --arm-live -> --fs-write. The mount therefore
 * always sees the disarmed answer, so FAT mounts the volume read-only for the
 * whole boot and classpnp/disk.sys latch FILE_READ_ONLY_DEVICE on the device
 * object. Arming afterwards cannot retroactively make an already-mounted volume
 * writable, so CreateFileW(..., GENERIC_WRITE) on it can never succeed - which
 * makes it impossible to prove that Windows' OWN filesystem stack can write
 * through this driver. There is no ordering escape: a vendor CDB needs
 * \\.\PhysicalDrive0, which needs the device started, which is the same PnP
 * cascade that performs the mount.
 *
 * The bit never provided protection in the first place. Enforcement is the
 * seven-layer stack - opcode allowlist, LBA fence, independent GPT guard, arm
 * latch, dry-run default, verify-after-write, block cap - and the arm latch is
 * what actually refuses writes while disarmed. Windows already ignores this bit
 * for LBA 0-5, which is precisely what the seven observed WGUARD refusals are.
 * Removing the advertisement only lets Windows ATTEMPT writes the latch still
 * refuses.
 *
 * Kept as a switch rather than deleted so the old behaviour can be restored for
 * a read-only-only build without re-deriving any of the above.
 */
#define UFS_ADVERTISE_WRITE_PROTECT          0

#define UFS_TRD_INTERRUPT                    0x01000000UL
#define UFS_TRD_DATA_OUT                     0x02000000UL
#define UFS_TRD_DATA_IN                      0x04000000UL
#define UFS_TRD_COMMAND_TYPE_STORAGE         0x10000000UL
#define UFS_TRD_OCS_INITIAL                  0x0000000FUL
#define UFS_TRD_OCS_SUCCESS                  0x00U
#define UFS_TRD_OCS_RESPONSE_SIZE_MISMATCH   0x04U

#define UFS_UPIU_TRANSACTION_COMMAND         0x01U
#define UFS_UPIU_TRANSACTION_RESPONSE        0x21U
#define UFS_UPIU_TRANSACTION_MASK            0x3FU
#define UFS_UPIU_FLAG_READ                   0x40U
#define UFS_UPIU_FLAG_WRITE                  0x20U
#define UFS_UPIU_COMMAND_NO_DATA_HEADER      0x00000001UL
#define UFS_UPIU_COMMAND_READ_HEADER         0x00004001UL
/*
 * Same aligned 32-bit header store as the read path, with the WRITE flag in
 * place of READ: byte 0 transaction=COMMAND, byte 1 flags, bytes 2-3 LUN/tag.
 * Writing this as one naturally aligned word is mandatory - an unaligned
 * halfword store into the uncached workspace is what bugchecked V16 and
 * V20-V23 before the descriptor ever reached the controller.
 */
#define UFS_UPIU_COMMAND_WRITE_HEADER        0x00002001UL
#define UFS_UTRD_RESPONSE_WORD               0x04000400UL
#define UFS_UTRD_PRDT_WORD                   0x08000080UL
/*
 * TRD DW7 packs PrdtLength in the low halfword and PrdtOffset in the high one,
 * both in BYTES on this controller rather than in the dwords/entries the base
 * UFSHCI layout specifies. UFS_UTRD_RESPONSE_WORD is the proof: it encodes 1024,
 * which is UFS_UCD_RESPONSE_OFFSET in bytes, and the controller really does
 * write the response UPIU there - UfsFinishScsiCommand validates the transaction
 * code at that address and every completed command has passed that check.
 *
 * For a single entry this reduces to exactly the proven UFS_UTRD_PRDT_WORD, so
 * the one-segment path keeps bit-for-bit the descriptor that has been working;
 * the C_ASSERT below pins that equivalence.
 */
#define UFS_UTRD_PRDT_WORD_FOR(Entries)                                        \
    (((ULONG)(Entries) * UFS_PRDT_ENTRY_SIZE) | (UFS_UCD_PRDT_OFFSET << 16))
#define UFS_UPIU_FLAG_UNDERFLOW              0x20U
#define UFS_UPIU_FLAG_OVERFLOW               0x40U
#define UFS_SCSI_STATUS_CHECK_CONDITION      0x02U

#define UFS_SERVICE_ACTION_READ_CAPACITY_16  0x10U

#pragma pack(push, 1)

typedef struct _UFS_TRANSFER_REQUEST_DESCRIPTOR {
    ULONG Dw0;
    ULONG Dw1;
    ULONG Dw2;
    ULONG Dw3;
    ULONG CommandDescriptorBaseLow;
    ULONG CommandDescriptorBaseHigh;
    USHORT ResponseLength;
    USHORT ResponseOffset;
    USHORT PrdtLength;
    USHORT PrdtOffset;
} UFS_TRANSFER_REQUEST_DESCRIPTOR, *PUFS_TRANSFER_REQUEST_DESCRIPTOR;

typedef struct _UFS_COMMAND_UPIU {
    UCHAR TransactionCode;
    UCHAR Flags;
    UCHAR Lun;
    UCHAR TaskTag;
    UCHAR CommandSet;
    UCHAR Reserved1[3];
    UCHAR EhsLength;
    UCHAR Reserved2;
    UCHAR DataSegmentLength[2];
    UCHAR ExpectedDataTransferLength[4];
    UCHAR Cdb[16];
} UFS_COMMAND_UPIU, *PUFS_COMMAND_UPIU;

/*
 * Width of the CDB field the command UPIU carries. Tied to the struct rather
 * than spelled 16 twice, because UfsBuildWireCdb builds into a stack buffer of
 * this size and a drift between the two would be a stack overrun, not a
 * mismatch the compiler would mention.
 */
#define UFS_WIRE_CDB_LENGTH 16UL
C_ASSERT(UFS_WIRE_CDB_LENGTH == RTL_FIELD_SIZE(UFS_COMMAND_UPIU, Cdb));

typedef struct _UFS_RESPONSE_UPIU {
    UCHAR TransactionCode;
    UCHAR Flags;
    UCHAR Lun;
    UCHAR TaskTag;
    UCHAR CommandSet;
    UCHAR Reserved1;
    UCHAR Response;
    UCHAR Status;
    UCHAR EhsLength;
    UCHAR DeviceInformation;
    UCHAR DataSegmentLength[2];
    UCHAR ResidualTransferCount[4];
    UCHAR Reserved2[16];
    UCHAR SenseDataLength[2];
    UCHAR SenseData[18];
} UFS_RESPONSE_UPIU, *PUFS_RESPONSE_UPIU;

typedef struct _UFS_FMP_PRDT_ENTRY {
    ULONG DataBaseAddressLow;
    ULONG DataBaseAddressHigh;
    ULONG Reserved;
    ULONG DataByteCount;
    ULONG FmpDescriptor[28];
} UFS_FMP_PRDT_ENTRY, *PUFS_FMP_PRDT_ENTRY;

#pragma pack(pop)

C_ASSERT(sizeof(UFS_TRANSFER_REQUEST_DESCRIPTOR) == 32);
C_ASSERT(sizeof(UFS_COMMAND_UPIU) == 32);
C_ASSERT(sizeof(UFS_RESPONSE_UPIU) == 52);
C_ASSERT(sizeof(UFS_FMP_PRDT_ENTRY) == UFS_PRDT_ENTRY_SIZE);
C_ASSERT(FIELD_OFFSET(UFS_COMMAND_UPIU, ExpectedDataTransferLength) == 12);
C_ASSERT(FIELD_OFFSET(UFS_COMMAND_UPIU, Cdb) == 16);
C_ASSERT(FIELD_OFFSET(UFS_TRANSFER_REQUEST_DESCRIPTOR, CommandDescriptorBaseHigh) == 20);
C_ASSERT(FIELD_OFFSET(UFS_TRANSFER_REQUEST_DESCRIPTOR, ResponseLength) == 24);
C_ASSERT(FIELD_OFFSET(UFS_TRANSFER_REQUEST_DESCRIPTOR, PrdtLength) == 28);
C_ASSERT(FIELD_OFFSET(UFS_FMP_PRDT_ENTRY, DataByteCount) == 12);
C_ASSERT(UFS_BOUNCE_SIZE <= (UFS_FMP_PRDT_DATA_BYTE_COUNT_MASK + 1UL));
C_ASSERT(UFS_BOUNCE_ALIGNMENT >= PAGE_SIZE);
/* One entry describes one FMP data unit, and the two must stay the same size. */
C_ASSERT(UFS_PRDT_SEGMENT_SIZE == UFS_MULTI_BLOCK_GRANULE);
C_ASSERT(UFS_PRDT_SEGMENT_SIZE == UFS_BOUNCE_ALIGNMENT);
C_ASSERT(UFS_PRDT_SEGMENT_SIZE <= (UFS_FMP_PRDT_DATA_BYTE_COUNT_MASK + 1UL));
/* The entries must fit the PRDT region, which is the tail of the command descriptor. */
C_ASSERT(UFS_PRDT_MAX_ENTRIES == 16);
C_ASSERT((UFS_PRDT_MAX_ENTRIES * UFS_PRDT_ENTRY_SIZE) <= UFS_PRDT_REGION_SIZE);
C_ASSERT((UFS_UCD_PRDT_OFFSET + UFS_PRDT_REGION_SIZE) == UFS_UCD_REGION_SIZE);
/* A transfer can never outrun either the PRDT or the bounce that backs it. */
C_ASSERT(UFS_MAX_TRANSFER_LENGTH == (64UL * 1024UL));
C_ASSERT(UFS_MAX_TRANSFER_LENGTH <= UFS_BOUNCE_SIZE);
/* The multi-entry descriptor must collapse to the proven single-entry constant. */
C_ASSERT(UFS_UTRD_PRDT_WORD_FOR(1) == UFS_UTRD_PRDT_WORD);
C_ASSERT(UFS_UTRD_PRDT_WORD_FOR(UFS_PRDT_MAX_ENTRIES) == 0x08000800UL);
/*
 * Worst-case workspace consumption when StorPortGetUncachedExtension hands back
 * a base that is maximally misaligned for every region in turn. Keeps the
 * page-aligned bounce from silently overrunning the uncached extension.
 */
C_ASSERT(UFS_WORKSPACE_SIZE >=
         ((UFS_UTRL_ALIGNMENT - 1UL) + UFS_UTRL_REGION_SIZE +
          (UFS_UCD_ALIGNMENT - 1UL) + UFS_UCD_REGION_SIZE +
          (UFS_BOUNCE_ALIGNMENT - 1UL) + UFS_BOUNCE_SIZE));

typedef enum _UFS_COMMAND_DIRECTION {
    UfsCommandNoData = 0,
    /*
     * These are the UTRD DW0 DD field encodings (bits [26:25]), not arbitrary
     * tags: 01b = data-out (host to device), 10b = data-in.
     */
    UfsCommandDataOut = 1,
    UfsCommandDataIn = 2
} UFS_COMMAND_DIRECTION;

/*
 * Everything a UFSDIAG record reports that does NOT move merely because the
 * record was taken. Two snapshots with equal signatures carry identical
 * information, so the second one can be dropped without losing evidence.
 *
 * This exists because comparing UFS_DIAGNOSTIC_DATA was wrong twice over:
 *
 *   - The vendor diagnostic is delivered as an ordinary SRB_FUNCTION_EXECUTE_SCSI
 *     (opcode 0xD0), so StartIoRequests and ExecuteScsiRequests are incremented
 *     BY THE READ ITSELF. Two consecutive snapshots could therefore never
 *     compare equal and suppression could never engage - measured on the
 *     2026-07-29 boot as PSUP=0 across all 19 records while the ring exhausted.
 *
 *   - More than half the printed fields (CompletedCommands, LastOpcode, the
 *     write counters, and the LastDataIn prefix) live directly on the adapter
 *     extension and are not inside UFS_DIAGNOSTIC_DATA at all, so that compare
 *     was blind to them. Had it ever matched, it would have suppressed records
 *     carrying brand-new data-in bytes.
 *
 * Anything genuinely observation-driven is deliberately absent: PramRecords,
 * PramDuplicatesSuppressed, VendorDiagRequests, StartIoRequests and
 * ExecuteScsiRequests. They are re-emitted verbatim in every record, so the
 * host still sees the true counts.
 */
typedef struct _UFS_PRAM_SIGNATURE {
    ULONGLONG FailureMask;
    ULONGLONG LastLogicalBlock;
    ULONGLONG LastRejectedLogicalBlock;
    ULONGLONG BouncePhysical;
    ULONGLONG RejectedOpcodes;
    ULONGLONG FirstRejectContext;
    ULONGLONG FailureHistogram;
    ULONGLONG FirstFailure;
    ULONG Stage;
    ULONG FailureStage;
    ULONG HwInitializeCalls;
    ULONG LastSrbFunction;
    ULONG LastExecuteReject;
    ULONG RearmAttempts;
    ULONG IoControlRequests;
    ULONG IoControlRejects;
    ULONG CompletedCommands;
    ULONG FailedCommands;
    ULONG RejectedCommands;
    ULONG RejectedOpcodeCount;
    ULONG ModeSenseTranslated;
    ULONG DataOverrunFailures;
    ULONG LastTrdDw2;
    ULONG GptTranslation;
    ULONG ContainedCommands;
    ULONG LastOpcode;
    ULONG LastOcs;
    ULONG InterruptStatus;
    ULONG AckFailureOcs;
    ULONG LastRequestLength;
    ULONG LastPrdtEntries;
    ULONG LogicalBlockSize;
    ULONG CapacityShortTransfers;
    ULONG CapacityRejected;
    ULONG CapacityMismatches;
    ULONG CapacityRetryRequests;
    ULONG CapacityRetriesExhausted;
    ULONG LastRejectedBlockSize;
    ULONG WriteMode;
    ULONG WriteCrashLockout;
    ULONG WriteCrashAttempt;
    ULONG WriteCrashPhase;
    ULONG WriteBootEpoch;
    ULONG WritesAttempted;
    ULONG WritesFenced;
    ULONG WritesGuarded;
    ULONG WritesProtected;
    ULONG WritesDisarmedRejects;
    ULONG WritesDryRun;
    ULONG WritesIssued;
    ULONG WritesVerified;
    ULONG BootAttemptsAcknowledged;
    ULONG LastDataInLength;
    ULONG DiagnosticOnly;
    ULONG Started;
    ULONG FatalError;
    ULONG OwnsTransferList;
    ULONG CapacityValid;
    ULONG LastDataInBlockMask;
    UCHAR LastDataIn[UFS_LAST_DATA_PREFIX];
    UCHAR LastDataInBlock1[UFS_LAST_DATA_PREFIX];
} UFS_PRAM_SIGNATURE, *PUFS_PRAM_SIGNATURE;

typedef struct _UFS_ADAPTER_EXTENSION {
    PUCHAR Hci;
    ULONG HciLength;
    PUCHAR Pram;
    PUCHAR LowWdtPram;
    BOOLEAN Rwd1Owned;
    volatile LONG Rwd1Lock;
    volatile LONG NoPetReason;
    BOOLEAN ShutdownPending;
    BOOLEAN WatchdogStopComplete;
    ULONG PramRecords;
    PUCHAR Pmu;
    ULONG RebootRequests;
    ULONG LastDataInLength;
    UCHAR LastDataIn[UFS_LAST_DATA_PREFIX];
    //
    // Per-block evidence for multi-block transfers. DIN alone is the head of the
    // bounce, i.e. block 0 - it can never say whether a 2- or 4-block read
    // actually deposited its later blocks, and the reported length cannot either
    // (a data-in command can report OCS=0 with residual 0 and still deliver
    // nothing, which is why DIN exists at all).
    //
    // LastDataInBlockMask sets bit N when 4096-byte block N of the transfer holds
    // at least one non-zero byte. The bounce is zeroed before every command, so a
    // clear bit means that block was genuinely never written. NONZERO - not the
    // leading bytes - is the discriminator, because a block may legitimately
    // begin with zeros: reading a 16-byte zero prefix at LBA 0 is the CORRECT
    // result for a protective MBR, and misreading it as a failed DMA is exactly
    // what cost V15/V16/V17.
    //
    // LastDataInBlock1 is 16 bytes from bounce + 4096. For the probe's 2- and
    // 4-block reads based at LBA 1 its expected value is known independently:
    // LBA 2 is the GPT entry-0 type GUID EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,
    // already observed on this device. That turns "the controller claims it moved
    // 8192 bytes" into "block 1 contains the right bytes for its LBA".
    //
    ULONG LastDataInBlockMask;
    UCHAR LastDataInBlock1[UFS_LAST_DATA_PREFIX];
    PVOID Workspace;
    STOR_PHYSICAL_ADDRESS WorkspacePhysical;
    PUFS_TRANSFER_REQUEST_DESCRIPTOR Utrl;
    STOR_PHYSICAL_ADDRESS UtrlPhysical;
    PUCHAR Ucd;
    STOR_PHYSICAL_ADDRESS UcdPhysical;
    PUCHAR Bounce;
    STOR_PHYSICAL_ADDRESS BouncePhysical;
    PUCHAR BounceCached;      /* m15 w34: cacheable view of the bounce pages, READ copy-back only (or NULL) */
    ULONG DcacheLineSize;     /* from CTR_EL0.DminLine */
    ULONG Capabilities;
    ULONG InitialHostStatus;
    ULONG InitialInterruptStatus;
    ULONG InitialInterruptEnable;
    ULONG InitialInterruptAggregation;
    ULONG InitialUtrlBaseLow;
    ULONG InitialUtrlBaseHigh;
    ULONG InitialUtrlRunStop;
    ULONG InitialNexusType;
    ULONG InitialTxPrdtSize;
    ULONG InitialRxPrdtSize;
    ULONG InitialDataReorder;
    ULONG InitialAxiDmaBurst;
    ULONG CompletedCommands;
    ULONG RejectedCommands;
    ULONG FailedCommands;
    ULONG ContainedCommands;
    ULONG RearmAttempts;
    /*
     * OCS frozen at the instant UfsAcknowledgeInterruptStatus refused a command.
     * LastOcs cannot serve here because UfsFinishScsiCommand is what writes it,
     * and that function is skipped on exactly this path - so the record would
     * otherwise carry the previous command's OCS and look healthy.
     */
    ULONG AckFailureOcs;
    ULONG LastRequestLength;
    /*
     * PRDT entries programmed for the last transfer. This is the field that
     * distinguishes "the multi-entry path ran" from "the build shipped but the
     * old single-entry descriptor was still being emitted", which no other
     * counter can tell apart: REQLEN reports what was asked for, not how it was
     * described to the controller.
     */
    ULONG LastPrdtEntries;
    ULONG WriteProtectedResponses;
    /*
     * Counts MODE SENSE replies that WOULD have carried the write-protect bit
     * under the old always-assert behaviour. See UFS_ADVERTISE_WRITE_PROTECT:
     * suppression has to be observable, otherwise "Windows never tried to
     * write" and "Windows was told it could not" look identical on device.
     */
    ULONG WriteProtectSuppressed;
    /*
     * Capacity-response rejections, split by reason so a boot failure can be
     * attributed without guessing:
     *   CapacityShortTransfers - the reply did not carry the expected byte
     *                            count, i.e. the command did not land
     *   CapacityRejected       - the block size or last LBA did not match this
     *                            device's fixed geometry
     *   CapacityMismatches     - a valid response disagreed with geometry
     *                            already latched for this medium
     * CapacityRetryRequests counts bounded SRB_STATUS_BUSY completions used to
     * make Storport issue a fresh capacity command. Once the bound is reached,
     * CapacityRetriesExhausted counts the hard failures.
     */
    ULONG CapacityShortTransfers;
    ULONG CapacityRejected;
    ULONG CapacityMismatches;
    ULONG CapacityRetryAttempts;
    ULONG CapacityRetryRequests;
    ULONG CapacityRetriesExhausted;
    ULONG LastRejectedBlockSize;
    ULONGLONG LastRejectedLogicalBlock;
    /*
     * Counts MODE SENSE(6) commands rewritten onto the wire as MODE SENSE(10)
     * by UfsBuildWireCdb. Emitted as MS10.
     *
     * Needed because the translation is invisible from anywhere else: the
     * caller's CDB still reads 0x1A on completion, so without a counter
     * "translation ran and the device was happy" and "translation never ran"
     * produce identical telemetry.
     */
    ULONG ModeSenseTranslated;
    /*
     * Write state. WriteMode is the arm latch and defaults to DISARMED, so a
     * freshly started adapter refuses every write regardless of what the fence
     * would have allowed.
     */
    ULONG WriteMode;
    ULONG WriteArmRequests;
    ULONG WritesAttempted;
    ULONG WritesFenced;
    ULONG WritesGuarded;
    /*
     * Refusals from the protected-partition guard. Adapter-only on purpose:
     * UFS_DIAGNOSTIC_DATA is an ABI surface shared with UfsDiag.exe and adding
     * a field there would change its size, so this is reported through PRAM
     * instead and UFS_DIAG_DATA_VERSION stays put.
     */
    ULONG WritesProtected;
    ULONG WritesDisarmedRejects;
    ULONG WritesDryRun;
    ULONG WritesIssued;
    ULONG WritesVerified;
    ULONG WriteVerifyFailures;
    ULONG LastWriteLba;
    ULONG LastWriteBlocks;
    ULONG LastWriteResult;
    ULONG LastOpcode;
    ULONG LastOcs;
    /*
     * The RAW, unmasked UTRD DW2 that LastOcs was derived from.
     *
     * Every OCS read in this driver masks with 0xFF, but UFSHCI defines OCS as
     * DW2 bits [3:0] and Linux's ufshcd_get_tr_ocs masks with MASK_OCS = 0x0F.
     * If this controller ever sets a bit in [7:4] - plausible, since UFSHCI 3.0
     * puts a crypto-error indicator in DW2 and this part runs an FMP crypto
     * descriptor - then a genuine OCS_SUCCESS reads back as a non-zero unknown
     * and the command is repudiated by the OCS gate.
     *
     * Captured raw rather than "fixed" by narrowing the mask, because changing
     * the mask on suspicion would alter the classification of every command in
     * the same build that is supposed to measure it. One variable at a time.
     */
    ULONG LastTrdDw2;
    ULONGLONG LastLogicalBlock;
    ULONG LogicalBlockSize;
    /*
     * IN-FLIGHT GPT HEADER TRANSLATION COUNTERS.
     *
     * These two distinguish the only two outcomes that otherwise look identical
     * from outside: a GPT header we rewrote so Windows would accept it, versus
     * a header we REFUSED to rewrite because its stored CRC32 did not validate.
     * Both leave partmgr's view unchanged when they are zero, so a single
     * "translation ran" flag would be unreadable - Rejected > 0 means a block
     * that looked like a GPT header failed its own integrity check and must be
     * investigated rather than assumed benign, while Translated > 0 with
     * Rejected == 0 is the intended path.
     *
     * They live here on the adapter extension rather than in
     * UFS_DIAGNOSTIC_DATA and are emitted only into PRAM, exactly like
     * LastTrdDw2 above. Appending to the diagnostic struct would change its
     * size, force UFS_DIAG_DATA_VERSION 0x00140000 -> 0x00150000, and ripple
     * into UfsDiag.exe's header validation, StatusBoard's BYTES= expectation,
     * the runtime validators and every decoder fixture - for two counters that
     * nothing outside the PRAM record needs to read.
     */
    ULONG GptHeadersTranslated;
    ULONG GptHeadersRejected;
    volatile LONG InterruptCallbacks;
    UFS_DIAGNOSTIC_DATA Diagnostic;

    /*
     * Signature of the previous snapshot as it was last written to PRAM. Used
     * to suppress a UFSDIAG record that would carry no new information.
     *
     * The PRAM ring is linear, caps at UFS_PRAM_CAPACITY and does NOT wrap, so
     * every wasted byte is permanently lost for the rest of the boot. Emitting
     * a full ~800-byte snapshot on every vendor-diag READ made observation
     * self-defeating: StatusBoard polling alone filled the ring mid-boot, after
     * which UfsPramByte silently discarded everything - including whatever the
     * driver was doing when the machine died. The 2026-07-29 write-probe boot
     * exhausted the ring at 16128 bytes across 20 UFSDIAG records, 13 of them
     * byte-identical, and the crash that followed left no ring evidence at all.
     * Only the crash latch, which lives at a fixed address outside the ring,
     * survived to name the attempt.
     *
     * See UFS_PRAM_SIGNATURE for why comparing UFS_DIAGNOSTIC_DATA instead
     * could never work.
     */
    UFS_PRAM_SIGNATURE LastPramDiagnostic;
    ULONG PramDuplicatesSuppressed;
    /*
     * Reserved note-slot state. PramNoteBase is the ring offset of slot 0,
     * captured at init rather than assumed to be zero so that the accessor stays
     * correct if the reservation ever moves. PramNotesWritten is the slot
     * cursor; once it reaches UFS_PRAM_NOTE_SLOTS the driver keeps rewriting the
     * LAST slot, because the newest note is the one that names the failure.
     */
    ULONG PramNoteBase;
    ULONG PramNotesWritten;
    BOOLEAN PramNotesReserved;
    /*
     * Vendor note CDB (0xD3) accounting, mirrored into Diagnostic.* so the note
     * survives even when its PRAM capture does not.
     */
    ULONG NoteRequests;
    ULONG LastNoteCode;
    ULONG LastNoteAux;
    /*
     * Unattended-return watchdog state. Ticks is seconds elapsed since
     * HwInitialize armed the timer; Armed is cleared once the reboot is
     * committed so a re-entrant tick cannot reset twice.
     */
    ULONG UnattendedTicks;
    BOOLEAN UnattendedArmed;
    /*
     * Unattended-session state derived from the TWRP-written PRAM arm token.
     *
     * UnattendReturnSec is 0 unless a valid token was consumed, and the timer
     * treats 0 as "observe only": it still refreshes the witness, but it never
     * arms the hardware watchdog and never resets. That is what keeps the
     * witness safe to compile in unconditionally.
     */
    ULONG UnattendReturnSec;
    BOOLEAN UnattendReturnRequested;
    /*
     * What Windows itself reports. Captured rather than assumed - firmware can
     * only claim it parked a core, and only the running kernel can say whether
     * the core was actually taken up and how much memory the map yielded.
     *
     * Processor counts are re-read on every tick because a boot-start driver
     * loads while the kernel is still bringing secondary processors online, so
     * the value seen in DriverEntry is a lower bound, not the answer.
     */
    ULONG WitnessActiveProcessors;
    ULONG WitnessMaximumProcessors;
    ULONG WitnessActiveGroups;
    ULONG WitnessFlags;
    /*
     * Tick at which the last write was issued, for the boot-diagnostic reboot.
     *
     * The unattended timer must never reset the phone out from under a live
     * write, but it cannot use WriteMode to decide that: in full-Windows mode
     * WriteMode is LIVE from HwInitialize to power-off, so an arm-state test
     * blocks the reboot forever and the backstop never fires. What matters is
     * whether a write is actually in flight, which this records directly.
     */
    ULONG LastWriteTick;
    /*
     * Prevent a returning PMU reset request from being issued on every
     * subsequent SRB if the reset register write unexpectedly does not take.
     */
    BOOLEAN IoDiagRebootIssued;
    /*
     * Hardware-watchdog servicing state. WdtTicksPerSecond is derived on device
     * from two WTCNT samples one pet apart, because the watchdog input clock is
     * not documented anywhere in this tree and the firmware's own estimate
     * ("resets a healthy boot a few seconds after handoff") disagrees with the
     * 30 s implied by its register programming. Measuring it is what makes a
     * later firmware-side arm safe to size.
     */
    PUCHAR Wdt;
    ULONG WdtPets;
    ULONG WdtLastCount;
    ULONG WdtTicksPerSecond;
#if UFS_PERF_INSTRUMENTATION
    /*
     * V33 latency instrumentation. Behaviour-neutral: nothing here is ever read
     * by a decision, only accumulated and reported, so an instrumented driver
     * issues byte-identical commands to an uninstrumented one.
     *
     * WHY. Task Manager showed one 1.05 GHz core and 711 MB of RAM with heavy
     * paging, so storage latency is on the critical path of every stall the user
     * sees - but nothing in this driver could say WHERE that latency goes. The
     * three candidates have completely different fixes:
     *
     *   - the device itself (doorbell -> doorbell-clear)   -> nothing we can do
     *   - the uncached zero-fill before every read          -> drop or narrow it
     *   - the uncached bounce copies                        -> widen the accesses
     *
     * The bounce is MmNonCached, so every access is its own bus transaction and
     * the copies are ULONG-at-a-time (this file already records that BYTE-at-a-
     * time "dominated boot"). A 64 KB read therefore currently costs 16384
     * uncached stores to zero the bounce plus 16384 uncached loads to drain it,
     * on top of the transfer. Whether that dwarfs the device time is exactly the
     * question these counters answer, and the answer decides the next change.
     *
     * Ticks are raw CNTVCT_EL0. CounterFrequency is captured once from
     * CNTFRQ_EL0 so the host converts to microseconds instead of guessing the
     * (undocumented on this SoC) tick rate. ULONGLONG throughout because at a
     * 26 MHz counter a ULONG of ticks wraps in about 165 seconds of boot.
     */
    ULONGLONG PerfCounterFrequency;
    ULONGLONG PerfDeviceTicks;      /* doorbell -> doorbell-clear (real device) */
    ULONGLONG PerfZeroTicks;        /* uncached bounce zero-fill before reads   */
    ULONGLONG PerfBounceInTicks;    /* uncached bounce -> caller (reads)        */
    ULONGLONG PerfBounceOutTicks;   /* caller -> uncached bounce (writes)       */
    ULONGLONG PerfPollIterations;   /* poll-loop passes, i.e. 10 us stalls paid */
    ULONGLONG PerfReadBytes;
    ULONGLONG PerfWriteBytes;
    ULONGLONG PerfMaxDeviceTicks;
    ULONG PerfReadCount;
    ULONG PerfWriteCount;
#endif /* UFS_PERF_INSTRUMENTATION */
#if UFS_DMA_WINDOW_ENFORCE
    /*
     * Set when the DMA workspace was obtained with explicit FMP-window bounds
     * rather than from StorPortGetUncachedExtension. Reported so a boot can be
     * told apart from one that fell back, which matters the moment bank1 is
     * published and "below 4 GB" stops being a sufficient constraint.
     */
    BOOLEAN WorkspaceBounded;
    BOOLEAN WorkspaceFirmwareReserved;
#endif /* UFS_DMA_WINDOW_ENFORCE */
    BOOLEAN WdtArmed;
    BOOLEAN LastPramDiagnosticValid;
    BOOLEAN ResourcesMapped;
    BOOLEAN WarmStateValid;
    BOOLEAN DiagnosticOnly;
    BOOLEAN Started;
    BOOLEAN FatalError;
    BOOLEAN OwnsTransferList;
    BOOLEAN InterruptsGated;
    BOOLEAN CapacityValid;
    BOOLEAN DiagnosticEndpointPreserved;
    /* Serialized by the existing half-duplex Storport callback contract. */
    ULONG CleanRecoveryState;
    ULONG CleanRecoveryToken;
    ULONGLONG CleanRecoveryDeadline;
} UFS_ADAPTER_EXTENSION, *PUFS_ADAPTER_EXTENSION;

C_ASSERT(sizeof(UFS_CLEAN_RECOVERY_REPLY) == UFS_CLEAN_RECOVERY_BYTES);

ULONG
DriverEntry(
    _In_ PVOID Argument1,
    _In_ PVOID Argument2
    );

ULONG
UfsHwFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_z_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _In_ PBOOLEAN Again
    );

BOOLEAN
UfsHwInitialize(
    _In_ PVOID DeviceExtension
    );

BOOLEAN
UfsHwStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb
    );

BOOLEAN
UfsHwInterrupt(
    _In_ PVOID DeviceExtension
    );

BOOLEAN
UfsHwResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId
    );

SCSI_ADAPTER_CONTROL_STATUS
UfsHwAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters
    );
