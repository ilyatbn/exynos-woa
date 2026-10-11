/** @file
  PlatformMemoryMapLib — Exynos 9820 / Galaxy S10e (`beyond0lte`).

  Returns the ARM memory region descriptors used to build the UEFI page tables.
  Mirrors Star2LtePkg's library for the Exynos 9810 and pulls every base/size
  from the single source of truth: Silicon/Exynos9820Pkg/Include/Platform/
  Exynos9820.h.

  NOTHING HERE IS VALIDATED. This library is the port's floor: it turns the
  reads recorded in notes/dock-desktop.md §5/§13/§15 into page-table inputs.
  Under the repo's golden rule the boot it produces is Guided until it has been
  observed on an S10e.

  Rules (same as the 9810 library):
   - DRAM windows: ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK (cached).
   - MMIO blocks:  ARM_MEMORY_REGION_ATTRIBUTE_DEVICE (non-cached).
   - reserved-memory carveouts (ION heaps, el2_code, kaslr, TrustZone-style
     regions) must NOT be exposed as conventional RAM.
   - Device windows whose base OR size is still 0 (an unverified placeholder
     from Exynos9820.h) are SKIPPED, so a TODO value never maps physical
     address 0 by accident.
**/

#include <Library/ArmPlatformLib.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Platform/Exynos9820.h>

#include "Beyond0LteCheckpoint.h"
#include "../Beyond0LteFbSerialPortLib/Beyond0LteBootLog.h"

#define BEYOND0LTE_PUBLISH_HIGH_BANKS  0

//
// W2's bank1/bank2 RAM. 0 = A/B test (m10 W16): winload faults in its heap code with a
// callee-saved register zeroed across a plain call (MmHapAddToFreeList), deterministically -
// the bank0-only boot tells whether the new ranges (uH stage-2?) are the cause.
//
#define BEYOND0LTE_PUBLISH_BANK12      1   // W33: back on, and now actually published as RAM (resource HOBs below)

#define MMAP_DRAM   ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define MMAP_DEV    ARM_MEMORY_REGION_ATTRIBUTE_DEVICE

//
// The 9810 flow wrote single-byte boot breadcrumbs to 0xFED14000 and mapped a
// 64 KB Device window over it. That worked because on the S9+ DRAM ENDED at
// 0xBC800000, so 0xFED14000 was above every bank.
//
// On the 9820 that assumption is false, and it is now a read rather than an
// inference: bank0 is 0x80000000 + 0x3AB00000 (live DTB reg, and /proc/iomem
// says "System RAM 80000000-baafffff"), so it ends at 0xBAB00000 - which still
// leaves 0xFED14000 inside conventional DRAM territory of the SECOND bank
// (memory@C0000000 + 0x20000000, ends 0xE0000000) and the third
// (memory@E1900000 + 0x1E700000, ends 0x100000000). Writing there would scribble
// into memory the loader owns, and mapping it DEVICE would overlap a cached DRAM
// region with a different attribute.
//
// So the breadcrumb channel has to be re-picked before it can be used, and the
// only honest place to pick it from is the vendor DT's own reserved-memory
// node, which publishes el2_code, el2_earlymem, debug_snapshot, seclog_mem,
// cp_ram_logging, kaslr and abox-rmem. Until that choice is made, the channel
// is compiled out rather than pointed at a guess.
//
#define BEYOND0LTE_PRAM_DIAG      0

//
// Table of candidate MMIO device windows. Anything with Base == 0 or Size == 0
// is skipped until it is filled in (Exynos9820.h). Keep in sync with
// docs/map/ for this subsystem.
//
// Deliberate containments, all derived from the vendor DT's reg lists rather
// than restated:
//   * CMU_TOP (0x1A240000) sits INSIDE CMU_BANK0 (0x1A200000 + 0x8000), so only
//     the bank is mapped.
//   * UFS_VS (0x13D61100) sits INSIDE UFS_HCI (0x13D60000 + 0x2000), so only the
//     HCI window is mapped.
//   * GICD_ISPENDRN (0x10101200) sits INSIDE GICD, so it is not mapped either.
//   * The USB PHY is ONE DT node with three register windows that OVERLAP each
//     other (combo 0x10AE0000+0x2800 already reaches 0x10AE2800, and the main
//     PHY window at 0x10B00000 is above it). Mapping three overlapping Device
//     regions is avoided: one envelope is mapped instead, computed from the
//     verified pair so it cannot drift.
//
typedef struct {
  UINT64        Base;
  UINT64        Size;
  CONST CHAR8   *Name;
} MMIO_REGION;

//
// RAM that the sboot/uH monitor OWNS - all four are reads from the live
// reserved-memory node (notes/K10-rkp.md T16/T17, notes/s10e-dtb-mmio.md S01),
// and the `compatible` strings name the owner: "el2,uh" for el2_code /
// el2_earlymem, "kernel-kaslr" for kaslr. uH maps its own region read-only or
// unmapped in stage-2, so publishing it as conventional RAM lets Windows
// overwrite live EL2 code. MUST stay sorted ascending by Base - the bank loop
// below walks it once per bank.
//
typedef struct {
  UINT64  Base;
  UINT64  Size;
} MONITOR_RESERVATION;

STATIC CONST MONITOR_RESERVATION  mMonitorReservations[] = {
  { EXYNOS_RS_MAGIC_BASE,     EXYNOS_RS_MAGIC_SIZE     },  // sec_debug_magic
  { EXYNOS_RS_KASLR_BASE,     EXYNOS_RS_KASLR_SIZE     },  // kaslr
  { EXYNOS_RS_EL2_CODE_BASE,  EXYNOS_RS_EL2_CODE_SIZE  },  // el2_code
  //
  // Bank1/bank2 carve-outs (W2, notes/m10-windows-deploy-plan.md): with those banks published as
  // RAM, these stay out of it. seclog (live read), the boot framebuffer (mapped as DEVICE below,
  // page-rounded), the modem block modem_spare..s5100_msi (modem_spare is a live read; IF2/CP2CP/
  // MSI are vendor-DT values - carved conservatively), and everything from modem_if up to 4 GiB
  // (modem_if, cp_ram_logging, debug_snapshot, log_bcm - live reads).
  //
  { EXYNOS_RSVD_SECLOG_BASE,  EXYNOS_RSVD_SECLOG_SIZE  },  // seclog 0xC0000000
  { EXYNOS_FB_BASE,           ((EXYNOS_FB_SIZE + 0xFFFULL) & ~0xFFFULL) },  // boot framebuffer
  { EXYNOS_RSVD_MODEM_SPARE_BASE,
      (EXYNOS_RSVD_S5100_MSI_BASE + EXYNOS_RSVD_S5100_MSI_SIZE - EXYNOS_RSVD_MODEM_SPARE_BASE) },  // modem block
  { EXYNOS_RSVD_MODEM_IF_BASE, 0x0000000100000000ULL - EXYNOS_RSVD_MODEM_IF_BASE },  // modem_if .. 4 GiB
  { EXYNOS_RS_EL2_EARLY_BASE, EXYNOS_RS_EL2_EARLY_SIZE },  // el2_earlymem
};

STATIC CONST MMIO_REGION  mMmioRegions[] = {
  { EXYNOS_UART_BASE,          EXYNOS_UART_SIZE,          "UART"        },

  { EXYNOS_GICD_BASE,          EXYNOS_GICD_SIZE,          "GICD"        },
  { EXYNOS_GICC_BASE,          EXYNOS_GICC_SIZE,          "GICC"        },  // GICv2 CPU interface
  { EXYNOS_GICH_BASE,          EXYNOS_GICH_SIZE,          "GICH"        },  // GICv2 hypervisor
  { EXYNOS_GICV_BASE,          EXYNOS_GICV_SIZE,          "GICV"        },  // GICv2 virtual CPU

  { EXYNOS_CHIPID_BASE,        EXYNOS_CHIPID_SIZE,        "CHIPID"      },
  { EXYNOS_PMU_BASE,           EXYNOS_PMU_SIZE,           "PMU"         },  // moved from 0x14060000
  { EXYNOS_SPEEDY_BASE,        EXYNOS_SPEEDY_SIZE,        "SPEEDY"      },  // extent now a live DTB read (0x2000) -> published
  { EXYNOS_SPEEDY1_BASE,       EXYNOS_SPEEDY1_SIZE,       "SPEEDY1"     },  // extent now a live DTB read (0x2000) -> published

  { EXYNOS_CMU_BANK0_BASE,     EXYNOS_CMU_BANK0_SIZE,     "CMU_BANK0"   },
  { EXYNOS_CMU_BANK1_BASE,     EXYNOS_CMU_BANK1_SIZE,     "CMU_BANK1"   },
  { EXYNOS_CMU_BANK2_BASE,     EXYNOS_CMU_BANK2_SIZE,     "CMU_BANK2"   },
  { EXYNOS_CMU_BANK3_BASE,     EXYNOS_CMU_BANK3_SIZE,     "CMU_BANK3"   },
  { EXYNOS_CMU_BANK4_BASE,     EXYNOS_CMU_BANK4_SIZE,     "CMU_BANK4"   },
  //
  // FSYS0A (which clocks USB31DRD) and PERIC0 (which clocks USI03, the touch
  // bus) are two DIFFERENT banks of the five above, but which is which is
  // unresolved: the 9820 clock driver names them through Samsung's cal-if
  // headers, where QCH ids are enum positions rather than addresses. These
  // stay zero so the loop below skips them instead of mapping a wrong bank.
  //
  { EXYNOS_CMU_FSYS0A_BASE,    EXYNOS_CMU_FSYS0A_SIZE,    "CMU_FSYS0A"  },
  { EXYNOS_CMU_PERIC0_BASE,    EXYNOS_CMU_PERIC0_SIZE,    "CMU_PERIC0"  },

  { EXYNOS_UFS_HCI_BASE,       EXYNOS_UFS_HCI_SIZE,       "UFS_HCI"     },
  { EXYNOS_UFS_UNIPRO_BASE,    EXYNOS_UFS_UNIPRO_SIZE,    "UFS_UNIPRO"  },  // map the full 0x8000
  { EXYNOS_UFS_PMA_BASE,       EXYNOS_UFS_PMA_SIZE,       "UFS_PMA"     },
  { EXYNOS_UFS_UFSP_BASE,      EXYNOS_UFS_UFSP_SIZE,      "UFS_UFSP"    },
  { EXYNOS_UFS_CPORT_BASE,     0x0000000000001000ULL,     "UFS_CPORT"   },  // extent unread
  { EXYNOS_UFS_IOCOH_BASE,     0x0000000000001000ULL,     "UFS_IOCOH"   },  // extent unread

  { EXYNOS_USBDRD_BASE,        EXYNOS_USBDRD_SIZE,        "USBDRD"      },  // xHCI window, same as 9810
  { EXYNOS_USB_COMBO_PHY_BASE,
      (EXYNOS_USBDRD_PHY_BASE + EXYNOS_USBDRD_PHY_SIZE - EXYNOS_USB_COMBO_PHY_BASE),
                             "USB_PHY"       },                            // envelope of all three

  { EXYNOS_TOUCH_HSI2C_BASE,   EXYNOS_TOUCH_HSI2C_SIZE,   "HSI2C10"     },  // touch bus, same base
  { EXYNOS_PERIC0_GPIO_BASE,   EXYNOS_PERIC0_GPIO_SIZE,   "GPIO_PERIC0" },
  { EXYNOS_ALIVE_GPIO_BASE,    EXYNOS_ALIVE_GPIO_SIZE,    "GPIO_ALIVE"  },  // moved from 0x14050000

  { EXYNOS_DECON_F_BASE,       EXYNOS_DECON_F_SIZE,       "DECON_F"     },
  { EXYNOS_DECON_T_BASE,       EXYNOS_DECON_T_SIZE,       "DECON_T"     },
  { EXYNOS_DSIM_BASE,          EXYNOS_DSIM_SIZE,          "DSIM"        },

  { EXYNOS_WDT_BASE,           EXYNOS_WDT_SIZE,           "WDT"         },  // same base as 9810
};

#define MMIO_REGION_COUNT  (sizeof (mMmioRegions) / sizeof (mMmioRegions[0]))

/**
  Build the platform memory map. Caller frees nothing; the table is static.

  @param[out] MemoryTable  Pointer to the descriptor array (NULL-terminated by
                           a zero-length entry).
**/
VOID
ArmPlatformGetVirtualMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **MemoryTable
  )
{
  UINTN  Index;
  UINTN  Region;

  // bank0/3/4 gaps (up to 6) + framebuffer slot + the MMIO windows + terminator
  STATIC ARM_MEMORY_REGION_DESCRIPTOR  Table[MMIO_REGION_COUNT + 12];

  BEYOND0LTE_CHECKPOINT (1540);   // MemoryPeim, MMU still off (B39)
  ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[1] = 0x61;   // W31 progress: about to build tables + enable MMU
  ASSERT (MemoryTable != NULL);
  Index = 0;

  //
  // --- DRAM banks (cached, identity-mapped), carved at the monitor's own RAM ---
  //
  // Bank0 is published only to min(EXYNOS_DRAM_SIZE, EXYNOS_DRAM_BANK0_SIZE):
  // the live DTB declares memory@80000000 as 0x3AB00000 long (ends
  // 0xBAB00000), and /proc/iomem agrees ("System RAM 80000000-baafffff"), so
  // publishing EXYNOS_DRAM_SIZE (to 0xD0000000) claimed 0x15500000 of RAM the
  // DTB never declared.
  //
  // Each bank is then split around mMonitorReservations, so sec_debug_magic,
  // kaslr, el2_code and el2_earlymem are never handed to Windows as RAM. The
  // hazard that remains is the OPPOSITE one: the Android-side heaps (camera,
  // secure_camera, modem_spare, modem_if, cp_ram_logging) are still published
  // as RAM because no read says what would break if Windows used them.
  //
  {
    UINT64 BankBase[5];
    UINT64 BankSize[5];
    UINTN  B;

    BankBase[0] = EXYNOS_DRAM_BASE;
    BankSize[0] = (EXYNOS_DRAM_SIZE < EXYNOS_DRAM_BANK0_SIZE)
                  ? EXYNOS_DRAM_SIZE : EXYNOS_DRAM_BANK0_SIZE;
    BankBase[1] = EXYNOS_DRAM_BANK3_BASE;
    BankSize[1] = EXYNOS_DRAM_BANK3_SIZE;
    BankBase[2] = EXYNOS_DRAM_BANK4_BASE;
    BankSize[2] = EXYNOS_DRAM_BANK4_SIZE;
    //
    // W2: the two 32-bit banks the 9810 port also gave Windows (its RamManager published bank1
    // around seclog + framebuffer). Carved at the reservations above. Unproven for EL1 on the
    // 9820: DXE allocates top-down, so the first boot with these tests them immediately (a bad
    // range resets the phone the way banks 3/4 did, B62).
    //
    BankBase[3] = EXYNOS_DRAM_BANK1_BASE;
    BankSize[3] = EXYNOS_DRAM_BANK1_SIZE;
    BankBase[4] = EXYNOS_DRAM_BANK2_BASE;
    BankSize[4] = EXYNOS_DRAM_BANK2_SIZE;
#if !BEYOND0LTE_PUBLISH_BANK12
    BankSize[3] = 0;
    BankSize[4] = 0;
#endif
#if !BEYOND0LTE_PUBLISH_HIGH_BANKS
    //
    // UEFI uses bank0 only (notes/m8-uefi-image.md B62/B63). Boot #16 reset (no EL1 exception)
    // as Beyond0LteRamManagerDxe started on the high banks; the 9810 port never gave UEFI its high
    // banks either (Star2LteRamManagerDxe adds 32-bit bank-1 ranges only) and its UFS work found RAM
    // outside bank0 not usable by UEFI at boot (Beyond0LteUfsHcDxe.c A105). Re-enable only with an
    // on-device read that EL1 may touch 0x880000000+ before the OS.
    // A zero size makes the loop below skip the bank (End == Start).
    //
    BankSize[1] = 0;
    BankSize[2] = 0;
#endif

    for (B = 0; B < 5; B++) {
      UINT64 Start;
      UINT64 End;
      UINT64 Cursor;
      UINTN  R;

      Start = BankBase[B];
      End   = Start + BankSize[B];
      if (End == Start) {
        continue;
      }
      Cursor = Start;
      for (R = 0; R < sizeof (mMonitorReservations) / sizeof (mMonitorReservations[0]); R++) {
        UINT64 ResBase;
        UINT64 ResEnd;

        ResBase = mMonitorReservations[R].Base;
        ResEnd  = ResBase + mMonitorReservations[R].Size;
        if ((ResEnd <= Start) || (ResBase >= End)) {
          continue;
        }
        if (ResBase > Cursor) {
          Table[Index].PhysicalBase = Cursor;
          Table[Index].VirtualBase  = Cursor;
          Table[Index].Length       = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (ResBase - Cursor));
          Table[Index].Attributes   = MMAP_DRAM;
          Index++;
        }
        if (ResEnd > Cursor) {
          Cursor = ResEnd;
        }
      }
      if (Cursor < End) {
        Table[Index].PhysicalBase = Cursor;
        Table[Index].VirtualBase  = Cursor;
        Table[Index].Length       = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (End - Cursor));
        Table[Index].Attributes   = MMAP_DRAM;
        Index++;
      }
    }
  }

  //
  // --- Framebuffer carveout (device/non-cached so writes hit the panel) ---
  // Base VERIFIED on the device from its own kernel command line
  // (/proc/device-tree/chosen/bootargs: s3cfb.bootloaderfb=0xca000000), not the
  // 9810's 0xCC000000. No DT node reserves it, so it is published as device
  // memory rather than RAM — otherwise Windows would claim the page the panel
  // scans out. notes/s10e-root-reads.md §3.
  //
  //
  // W32: the 2 MiB handoff window (Windows PRAM ring + progress word at 0x8FE00000, RAM boot log
  // at 0x8FF00000) NON-CACHEABLE. Mapped write-back, UEFI's own writes (zeroing after a save, the
  // progress word) sat dirty in the cache and were lost at our SWRESET, and the next boot read
  // stale lines instead of what the Windows driver wrote non-cached: WINPRAM.BIN came back
  // byte-identical to an older run. The shim and SEC (MMU off) already write it uncached. This
  // entry overrides the bank0 write-back mapping above (later entries win).
  //
  Table[Index].PhysicalBase = BEYOND0LTE_WINPRAM_BASE;
  Table[Index].VirtualBase  = BEYOND0LTE_WINPRAM_BASE;
  Table[Index].Length       = BEYOND0LTE_WINPRAM_SIZE + BEYOND0LTE_BOOTLOG_SIZE;
  Table[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;
  Index++;
  STATIC_ASSERT (BEYOND0LTE_WINPRAM_BASE + BEYOND0LTE_WINPRAM_SIZE == BEYOND0LTE_BOOTLOG_BASE,
                 "handoff window must be contiguous");

  if (EXYNOS_FB_BASE != 0) {
    Table[Index].PhysicalBase = EXYNOS_FB_BASE;
    Table[Index].VirtualBase  = EXYNOS_FB_BASE;
    Table[Index].Length       = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (EXYNOS_FB_SIZE));
    Table[Index].Attributes   = MMAP_DEV;
    Index++;
  }

  //
  // --- MMIO device windows (skip unverified placeholders) ---
  //
  // The 9810 skipped only Base == 0. Here Size == 0 is skipped too, because
  // Exynos9820.h records several windows whose ADDRESS is a read but whose
  // EXTENT is not — and a zero-length region would collide with the
  // zero-length terminator below.
  //
  for (Region = 0; Region < MMIO_REGION_COUNT; Region++) {
    if ((mMmioRegions[Region].Base == 0) || (mMmioRegions[Region].Size == 0)) {
      DEBUG ((DEBUG_WARN, "MemoryMap: skipping unverified MMIO '%a'.\n",
        mMmioRegions[Region].Name));
      continue;
    }
    //
    // Page tables map whole 4 KB pages, and ArmConfigureMmu REJECTS the entire
    // table if any entry is unaligned ("UpdateRegionMapping RegionStart ... are
    // not page aligned" -> "Failed to enable MMU", boot #14, notes/m8-uefi-image.md
    // B56). UFS_IOCOH (0x13C10700) is a register inside a page, not a page, so
    // every window is rounded OUT to the pages that contain [Base, Base + Size).
    //
    {
      UINT64  WinStart;
      UINT64  WinEnd;

      WinStart = mMmioRegions[Region].Base & ~(UINT64)EFI_PAGE_MASK;
      WinEnd   = (mMmioRegions[Region].Base + mMmioRegions[Region].Size + EFI_PAGE_MASK) &
                 ~(UINT64)EFI_PAGE_MASK;
      Table[Index].PhysicalBase = WinStart;
      Table[Index].VirtualBase  = WinStart;
      Table[Index].Length       = WinEnd - WinStart;
      Table[Index].Attributes   = MMAP_DEV;
      Index++;
    }
  }

  //
  // --- High DRAM banks: published by the bank loop above ---
  //
  // The loop already emits EXYNOS_DRAM_BANK3_BASE/SIZE and EXYNOS_DRAM_BANK4_BASE/SIZE
  // (carved at the monitor reservations, which puts el2_earlymem 0x97E000000+0x200000
  // in the fourth bank). These were separate entries before; keeping them here
  // as well would publish the same range twice.
  //

  //
  // --- Terminator (zero-length entry) ---
  //
  ASSERT (Index + 1 < sizeof (Table) / sizeof (Table[0]));
  Table[Index].PhysicalBase = 0;
  Table[Index].VirtualBase  = 0;
  Table[Index].Length       = 0;
  Table[Index].Attributes   = 0;

  //
  // m10 W18: the carve above only keeps these out of the PAGE TABLES. The UEFI memory map comes
  // from MemoryInitPeiLib's one resource HOB over PcdSystemMemoryBase/Size, so sec_debug_magic,
  // kaslr and el2_code (uH) were listed as EfiConventionalMemory and handed to Windows. Reserve
  // every monitor reservation that lies inside that range. Called once, from PeilessSec's
  // MemoryPeim (the only user of this library), with the HOB list live.
  //
  {
    STATIC BOOLEAN  mReservedHobsBuilt = FALSE;
    UINT64          MemBase;
    UINT64          MemEnd;

    MemBase = PcdGet64 (PcdSystemMemoryBase);
    MemEnd  = MemBase + PcdGet64 (PcdSystemMemorySize);
    if (!mReservedHobsBuilt) {
      mReservedHobsBuilt = TRUE;
      for (Region = 0; Region < sizeof (mMonitorReservations) / sizeof (mMonitorReservations[0]); Region++) {
        if ((mMonitorReservations[Region].Base >= MemBase) &&
            (mMonitorReservations[Region].Base + mMonitorReservations[Region].Size <= MemEnd))
        {
          BuildMemoryAllocationHob (
            mMonitorReservations[Region].Base,
            mMonitorReservations[Region].Size,
            EfiReservedMemoryType
            );
        }
      }

      // W19: the RAM boot log (Beyond0LteBootLog.h) - mapped as bank0 RAM, kept out of every allocator.
      BuildMemoryAllocationHob (BEYOND0LTE_BOOTLOG_BASE, BEYOND0LTE_BOOTLOG_SIZE, EfiReservedMemoryType);
      // W23: the Windows UFS driver's PRAM ring - reserved so Windows never allocates it as RAM.
      //
      // W33: give UEFI and Windows the bank1/bank2 RAM. W2 only MAPPED these ranges; MemoryInitPeiLib
      // publishes a single resource HOB for PcdSystemMemory (bank0), so they never reached the
      // memory map (W18) and Windows Setup ran on ~0.9 GB. Every write-back DRAM entry of the table
      // above that lies in 0xC0000000..4 GiB (already carved around seclog, the framebuffer, the
      // modem block and modem_if..4 GiB) becomes system memory. UFS DMA stays in bank0 (W2).
      //
#if BEYOND0LTE_PUBLISH_BANK12
      {
        UINTN  T;

        for (T = 0; T < Index; T++) {
          if ((Table[T].Attributes == MMAP_DRAM) && (Table[T].PhysicalBase >= 0xC0000000ULL) &&
              (Table[T].PhysicalBase + Table[T].Length <= 0x100000000ULL))
          {
            BuildResourceDescriptorHob (
              EFI_RESOURCE_SYSTEM_MEMORY,
              EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
              EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE | EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
              EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE | EFI_RESOURCE_ATTRIBUTE_TESTED,
              Table[T].PhysicalBase,
              Table[T].Length
              );
          }
        }
      }
#endif

      // W35: the Windows UFS driver's DMA workspace (inside its bank0 window) - never RAM to anyone.
      BuildMemoryAllocationHob (BEYOND0LTE_UFS_DMA_POOL_BASE, BEYOND0LTE_UFS_DMA_POOL_SIZE, EfiReservedMemoryType);

      // W31: runtime data (still never handed to the OS as RAM) so the runtime ResetSystem can
      // reach the boot progress word at 0x8FE0F000 after SetVirtualAddressMap.
      BuildMemoryAllocationHob (BEYOND0LTE_WINPRAM_BASE, BEYOND0LTE_WINPRAM_SIZE, EfiRuntimeServicesData);
    }
  }

  *MemoryTable = Table;
}
