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
