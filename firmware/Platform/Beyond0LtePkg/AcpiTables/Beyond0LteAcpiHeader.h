/** @file
  Shared ACPI definitions for Beyond0LtePkg (Exynos 9820 / Galaxy S10e).

  Mirrors Star2LtePkg's header for the Exynos 9810: the OEM identity plus the
  header-initialization macro reused by every static ACPI table (.aslc) in this
  directory.

  Nothing here is validated on an S10e yet.
**/

#ifndef BEYOND0LTE_ACPI_HEADER_H_
#define BEYOND0LTE_ACPI_HEADER_H_

#include <IndustryStandard/Acpi.h>

//
// OEM identity. 6-byte OEM ID, 8-byte OEM Table ID.
//
#define EFI_ACPI_OEM_ID            {'E', 'X', 'Y', 'N', 'O', 'S'}
#define EFI_ACPI_OEM_TABLE_ID      SIGNATURE_64 ('B','E','Y','O','N','D','0','L')
#define EFI_ACPI_OEM_REVISION      0x00000001
#define EFI_ACPI_CREATOR_ID        SIGNATURE_32 ('W','O','A',' ')
#define EFI_ACPI_CREATOR_REVISION  0x00000001

//
// A NULL Generic Address Structure (used to zero out unused FADT registers).
//
#define NULL_GAS  { EFI_ACPI_6_3_SYSTEM_MEMORY, 0, 0, EFI_ACPI_6_3_UNDEFINED, 0 }

//
// m14 (notes/m14-smp.md): Windows SMP through the ACPI Multiprocessor Parking
// Protocol. Shared by Fadt.aslc and Beyond0LteAcpiPlatformDxe (which starts the
// cores and patches the MADT), so both sides see the same switches.
//
// BEYOND0LTE_SMP_PARKING 1: at MADT install time the DXE PSCI-0.1 CPU_ONs every
//   secondary in BEYOND0LTE_SMP_PARK_MASK into a firmware parking stub and fills
//   that GICC's ParkingProtocolVersion/ParkedAddress. Every other ENABLED
//   secondary GICC is marked disabled (with PSCI off Windows could not start it).
//   0: MADT untouched (all eight GICCs enabled, PSCI CPU_ON - today's image).
//
// BEYOND0LTE_SMP_PARK_MASK: bit n = the n-th GICC in Madt.aslc (0-3 A55, 4-5
//   prometheus, 6-7 cheetah). Default 0x0E = the three secondary A55s. The boot
//   core (MPIDR == MPIDR_EL1) is always skipped. The mid/big cores are NOT
//   parked by default: on the 9810 a PSCI CPU_ON of a big core never returned
//   and took the SoC down (Star2LtePkg PBML STAR2LTE_PSCI_PARK block); on the
//   9820 that is untested.
//
// BEYOND0LTE_FADT_PSCI: FADT ArmBootArch PSCI_COMPLIANT. Default = !PARKING:
//   with parking on, Windows must stop using PSCI (it calls SYSTEM_RESET
//   0x84000009, which Samsung's PSCI 0.1 EL3 answers NOT_SUPPORTED -> bugcheck
//   0xA0, notes/m11-s10ediag.md run 5) and fall back to the FADT reset register
//   (PMU SWRESET, VERIFIED W32). Whether this Windows build boots with a
//   non-PSCI FADT is UNVERIFIED; set 1 to get the S9's proven combination
//   (PSCI_COMPLIANT + parked GICCs) back without touching anything else.
//
#ifndef BEYOND0LTE_SMP_PARKING
#define BEYOND0LTE_SMP_PARKING  1
#endif

// m14 run 15: GICC 1-3 (A55) proven on device. Cores outside BEYOND0LTE_SMP_SAFE_MASK are started behind the
// progress-0x65 guard (Beyond0LteParking.c): a boot after a hang there parks only the safe cores.
#ifndef BEYOND0LTE_SMP_SAFE_MASK
#define BEYOND0LTE_SMP_SAFE_MASK  0x3Eu   // m14 run 16: GICC 1-5 (A55 + A75) proven
#endif
#ifndef BEYOND0LTE_SMP_PARK_MASK
#define BEYOND0LTE_SMP_PARK_MASK  0xFEu   // m14: + GICC 6-7 (M4 big cores; CPU_ON of a 9810 big core hung its SoC)
#endif

#ifndef BEYOND0LTE_FADT_PSCI
#if BEYOND0LTE_SMP_PARKING
#define BEYOND0LTE_FADT_PSCI  0
#else
#define BEYOND0LTE_FADT_PSCI  1
#endif
#endif

//
// Standard ACPI table header initializer. 'Signature' and 'Type' (the C struct
// type) and 'Revision' are table-specific; everything else is platform-wide.
//
#define ACPI_HEADER(Signature, Type, Revision) {                    \
    Signature,                       /* UINT32  Signature */         \
    sizeof (Type),                   /* UINT32  Length */            \
    Revision,                        /* UINT8   Revision */          \
    0,                               /* UINT8   Checksum (fixed up by installer) */ \
    EFI_ACPI_OEM_ID,                 /* UINT8   OemId[6] */          \
    EFI_ACPI_OEM_TABLE_ID,           /* UINT64  OemTableId */        \
    EFI_ACPI_OEM_REVISION,           /* UINT32  OemRevision */       \
    EFI_ACPI_CREATOR_ID,             /* UINT32  CreatorId */         \
    EFI_ACPI_CREATOR_REVISION        /* UINT32  CreatorRevision */   \
  }

#endif // BEYOND0LTE_ACPI_HEADER_H_
