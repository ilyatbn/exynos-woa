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
#include <Platform/Exynos9820.h>

#define MMAP_DRAM   ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define MMAP_DEV    ARM_MEMORY_REGION_ATTRIBUTE_DEVICE

//
// The 9810 flow wrote single-byte boot breadcrumbs to 0xFED14000 and mapped a
// 64 KB Device window over it. That worked because on the S9+ DRAM ENDED at
// 0xBC800000, so 0xFED14000 was above every bank.
//
// On the 9820 that assumption is false: bank0 is 0x80000000 + 0x80000000, i.e.
// it runs to 0x100000000, so 0xFED14000 is INSIDE conventional DRAM. Writing
// there would scribble into memory the loader owns, and mapping it DEVICE would
// overlap a cached DRAM region with a different attribute.
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

STATIC CONST MMIO_REGION  mMmioRegions[] = {
  { EXYNOS_UART_BASE,          EXYNOS_UART_SIZE,          "UART"        },

  { EXYNOS_GICD_BASE,          EXYNOS_GICD_SIZE,          "GICD"        },
  { EXYNOS_GICC_BASE,          EXYNOS_GICC_SIZE,          "GICC"        },  // GICv2 CPU interface
  { EXYNOS_GICH_BASE,          EXYNOS_GICH_SIZE,          "GICH"        },  // GICv2 hypervisor
  { EXYNOS_GICV_BASE,          EXYNOS_GICV_SIZE,          "GICV"        },  // GICv2 virtual CPU

  { EXYNOS_CHIPID_BASE,        EXYNOS_CHIPID_SIZE,        "CHIPID"      },
  { EXYNOS_PMU_BASE,           EXYNOS_PMU_SIZE,           "PMU"         },  // moved from 0x14060000
  { EXYNOS_SPEEDY_BASE,        EXYNOS_SPEEDY_SIZE,        "SPEEDY"      },  // size unread -> skipped
  { EXYNOS_SPEEDY1_BASE,       EXYNOS_SPEEDY1_SIZE,       "SPEEDY1"     },  // size unread -> skipped

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

  STATIC ARM_MEMORY_REGION_DESCRIPTOR  Table[MMIO_REGION_COUNT + 3];

  ASSERT (MemoryTable != NULL);
  Index = 0;

  //
  // --- DRAM (cached, identity-mapped) ---
  //
  // Only the front of bank0 is published: EXYNOS_DRAM_SIZE stops at 0xD0000000,
  // the first of the modem/CP reservations the board file declares (0xD0000000
  // .. 0xFD900000, a contiguous 729 MB). The vendor DT declares bank0 as a full
  // 2 GiB, so publishing all of it would hand the modem's memory to Windows as
  // conventional RAM. The tail above 0xFD900000 and the two high 2 GiB banks are
  // what has to be added once the split sizes at 0xC0000000 and 0xE1900000 are
  // confirmed — see notes/dock-desktop.md §17.
  //
  Table[Index].PhysicalBase = EXYNOS_DRAM_BASE;
  Table[Index].VirtualBase  = EXYNOS_DRAM_BASE;
  Table[Index].Length       = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (EXYNOS_DRAM_SIZE));
  Table[Index].Attributes   = MMAP_DRAM;
  Index++;

  //
  // --- Framebuffer carveout (device/non-cached so writes hit the panel) ---
  // Only mapped once the base is verified. It is NOT verified on this SoC: the
  // 9810's 0xCC000000 was never read on an S10e, and the vendor DT's chosen
  // node carried no fb address. Leaving it zero means the screen stays dark
  // until the sboot-lit carveout is measured — which is the honest state, not
  // a bug to paper over.
  //
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
    Table[Index].PhysicalBase = mMmioRegions[Region].Base;
    Table[Index].VirtualBase  = mMmioRegions[Region].Base;
    Table[Index].Length       = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (mMmioRegions[Region].Size));
    Table[Index].Attributes   = MMAP_DEV;
    Index++;
  }

  //
  // --- Terminator (zero-length entry) ---
  //
  Table[Index].PhysicalBase = 0;
  Table[Index].VirtualBase  = 0;
  Table[Index].Length       = 0;
  Table[Index].Attributes   = 0;

  *MemoryTable = Table;
}
