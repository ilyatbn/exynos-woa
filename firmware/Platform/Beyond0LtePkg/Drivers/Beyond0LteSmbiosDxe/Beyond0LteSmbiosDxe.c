/** @file
  R2 (notes/ram-6gb.md): SMBIOS records for Windows - BIOS (type 0), system (1), enclosure (3), physical memory array
  (16), memory device (17) and memory array mapped addresses (19), through the edk2 SmbiosDxe EFI_SMBIOS_PROTOCOL.

  Why: without SMBIOS Windows has no "installed RAM" figure. MS Learn GetPhysicallyInstalledSystemMemory: the value
  comes "from the computer's SMBIOS firmware tables" and "must be equal to or greater than the amount reported by
  GlobalMemoryStatusEx" (else ERROR_INVALID_DATA) - 6144 MB installed vs ~5912 MB usable (R1) satisfies that.
  Identity fields follow MS Learn bringup/smbios (Type 1 Manufacturer / Family / Product Name / SKU, Type 3 Tablet 1Eh).
  No serial number and no device-unique value is published (the type 1 UUID is a fixed build constant).

  Memory facts: the S10e has 6 GB of LPDDR4X (3 x 2 GiB = 6144 MiB, notes/cpu-ram-plan-20261012.md). The mapped ranges
  are the VERIFIED bank constants of Exynos9820.h (bank0-2 below 4 GiB, banks 3+4 at 0x880000000); the 110 MiB between
  the low banks is not DRAM address space and is not mapped here.

  Copyright (c) 2026, exynos-woa contributors. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/Smbios.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Platform/Exynos9820.h>

#define S10E_DRAM_INSTALLED_MB  6144u   // 6 GB LPDDR4X package (3 x 2 GiB), notes/cpu-ram-plan-20261012.md

STATIC EFI_SMBIOS_PROTOCOL  *mSmbios;

//
// Copy the formatted area, append the strings (each NUL-terminated) and the final NUL; a record without strings ends
// in two NULs (SMBIOS 3.x 6.1.3). Returns the handle the protocol assigned.
//
STATIC
EFI_SMBIOS_HANDLE
AddRecord (
  IN VOID          *Formatted,
  IN UINTN         Length,
  IN CONST CHAR8   **Strings,
  IN UINTN         StringCount
  )
{
  UINTN              Total;
  UINTN              Index;
  UINTN              Len;
  CHAR8              *Buffer;
  CHAR8              *Out;
  EFI_SMBIOS_HANDLE  Handle;
  EFI_STATUS         Status;

  Total = Length + 1;
  for (Index = 0; Index < StringCount; Index++) {
    Total += AsciiStrLen (Strings[Index]) + 1;
  }
  if (StringCount == 0) {
    Total++;
  }
  Buffer = AllocateZeroPool (Total);
  if (Buffer == NULL) {
    return SMBIOS_HANDLE_PI_RESERVED;
  }
  CopyMem (Buffer, Formatted, Length);
  ((SMBIOS_STRUCTURE *)Buffer)->Length = (UINT8)Length;
  Out = Buffer + Length;
  for (Index = 0; Index < StringCount; Index++) {
    Len = AsciiStrLen (Strings[Index]);
    CopyMem (Out, Strings[Index], Len);
    Out += Len + 1;
  }
  Handle = SMBIOS_HANDLE_PI_RESERVED;
  Status = mSmbios->Add (mSmbios, NULL, &Handle, (EFI_SMBIOS_TABLE_HEADER *)Buffer);
  DEBUG ((DEBUG_INFO, "Beyond0LteSmbios: type %u -> handle 0x%x (%r)\n", ((SMBIOS_STRUCTURE *)Buffer)->Type, Handle, Status));
  FreePool (Buffer);
  return EFI_ERROR (Status) ? SMBIOS_HANDLE_PI_RESERVED : Handle;
}

STATIC
VOID
AddMappedRange (
  IN EFI_SMBIOS_HANDLE  ArrayHandle,
  IN UINT64             Base,
  IN UINT64             Size
  )
{
  SMBIOS_TABLE_TYPE19  T19;

  ZeroMem (&T19, sizeof (T19));
  T19.Hdr.Type = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
  //
  // 32-bit KB fields when both ends fit, else 0xFFFFFFFF and the byte-granular extended fields (SMBIOS 7.20)
  //
  if (((Base + Size - 1) >> 10) < 0xFFFFFFFFull) {
    T19.StartingAddress = (UINT32)(Base >> 10);
    T19.EndingAddress   = (UINT32)((Base + Size - 1) >> 10);
  } else {
    T19.StartingAddress         = 0xFFFFFFFF;
    T19.EndingAddress           = 0xFFFFFFFF;
    T19.ExtendedStartingAddress = Base;
    T19.ExtendedEndingAddress   = Base + Size - 1;
  }
  T19.MemoryArrayHandle = ArrayHandle;
  T19.PartitionWidth    = 1;
  AddRecord (&T19, sizeof (T19), NULL, 0);
}

EFI_STATUS
EFIAPI
Beyond0LteSmbiosEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  SMBIOS_TABLE_TYPE0   T0;
  SMBIOS_TABLE_TYPE1   T1;
  SMBIOS_TABLE_TYPE3   T3;
  SMBIOS_TABLE_TYPE16  T16;
  SMBIOS_TABLE_TYPE17  T17;
  EFI_SMBIOS_HANDLE    ArrayHandle;
  //
  // fixed build constant, not a device read (no device-unique value is published)
  //
  STATIC CONST EFI_GUID  SystemUuid = {
    0x43936af7, 0x78d6, 0x4099, { 0xbc, 0x76, 0x16, 0xc5, 0xb7, 0xc2, 0xd8, 0x82 }
  };
  CONST CHAR8  *S0[] = { "exynos-woa", "Beyond0LtePkg R2", "10/18/2026" };
  CONST CHAR8  *S1[] = { "Samsung", "Galaxy S10e", "SM-G970F", "SM-G970F", "Galaxy S" };
  CONST CHAR8  *S3[] = { "Samsung" };
  CONST CHAR8  *S17[] = { "LPDDR4X", "Package 0" };

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&mSmbios);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Beyond0LteSmbios: no SMBIOS protocol (%r)\n", Status));
    return Status;
  }

  ZeroMem (&T0, sizeof (T0));
  T0.Hdr.Type                    = EFI_SMBIOS_TYPE_BIOS_INFORMATION;
  T0.Vendor                      = 1;
  T0.BiosVersion                 = 2;
  T0.BiosSegment                 = 0;
  T0.BiosReleaseDate             = 3;
  T0.BiosSize                    = 0x3F;            // (n + 1) * 64 KiB = 4 MiB: the FV (Beyond0LtePkg.fdf FVMAIN)
  T0.BiosCharacteristics.BiosCharacteristicsNotSupported = 1;
  T0.BIOSCharacteristicsExtensionBytes[1] = BIT3;   // UEFI is supported (SMBIOS 7.1.2.2)
  T0.SystemBiosMajorRelease      = 0;
  T0.SystemBiosMinorRelease      = 2;
  T0.EmbeddedControllerFirmwareMajorRelease = 0xFF;
  T0.EmbeddedControllerFirmwareMinorRelease = 0xFF;
  AddRecord (&T0, sizeof (T0), S0, ARRAY_SIZE (S0));

  ZeroMem (&T1, sizeof (T1));
  T1.Hdr.Type     = EFI_SMBIOS_TYPE_SYSTEM_INFORMATION;
  T1.Manufacturer = 1;
  T1.ProductName  = 2;
  T1.Version      = 3;
  T1.SerialNumber = 0;                              // none published
  CopyGuid ((EFI_GUID *)&T1.Uuid, &SystemUuid);
  T1.WakeUpType   = SystemWakeupTypePowerSwitch;
  T1.SKUNumber    = 4;
  T1.Family       = 5;
  AddRecord (&T1, sizeof (T1), S1, ARRAY_SIZE (S1));

  ZeroMem (&T3, sizeof (T3));
  T3.Hdr.Type         = EFI_SMBIOS_TYPE_SYSTEM_ENCLOSURE;
  T3.Manufacturer     = 1;
  T3.Type             = 0x1E;                       // Tablet (MS Learn bringup/smbios Enclosure Type)
  T3.BootupState      = ChassisStateSafe;
  T3.PowerSupplyState = ChassisStateSafe;
  T3.ThermalState     = ChassisStateSafe;
  T3.SecurityStatus   = ChassisSecurityStatusNone;
  //
  // the formatted area ends before the variable ContainedElements array (count 0) and the SKU byte
  //
  AddRecord (&T3, OFFSET_OF (SMBIOS_TABLE_TYPE3, ContainedElements), S3, ARRAY_SIZE (S3));

  ZeroMem (&T16, sizeof (T16));
  T16.Hdr.Type                     = EFI_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
  T16.Location                     = MemoryArrayLocationSystemBoard;
  T16.Use                          = MemoryArrayUseSystemMemory;
  T16.MemoryErrorCorrection        = MemoryErrorCorrectionNone;
  T16.MaximumCapacity              = 0x80000000;    // use the extended field (SMBIOS 7.17)
  T16.ExtendedMaximumCapacity      = (UINT64)S10E_DRAM_INSTALLED_MB << 20;
  T16.MemoryErrorInformationHandle = 0xFFFE;
  T16.NumberOfMemoryDevices        = 1;
  ArrayHandle = AddRecord (&T16, sizeof (T16), NULL, 0);

  ZeroMem (&T17, sizeof (T17));
  T17.Hdr.Type                     = EFI_SMBIOS_TYPE_MEMORY_DEVICE;
  T17.MemoryArrayHandle            = ArrayHandle;
  T17.MemoryErrorInformationHandle = 0xFFFE;
  T17.TotalWidth                   = 64;
  T17.DataWidth                    = 64;
  T17.Size                         = (UINT16)S10E_DRAM_INSTALLED_MB;   // MB, bit 15 clear
  T17.FormFactor                   = MemoryFormFactorRowOfChips;      // soldered package
  T17.DeviceLocator                = 1;
  T17.BankLocator                  = 2;
  T17.MemoryType                   = MemoryTypeLpddr4;
  T17.TypeDetail.Synchronous       = 1;
  T17.MemoryTechnology             = MemoryTechnologyDram;
  AddRecord (&T17, sizeof (T17), S17, ARRAY_SIZE (S17));

  AddMappedRange (ArrayHandle, EXYNOS_DRAM_BASE, EXYNOS_DRAM_BANK0_SIZE);
  AddMappedRange (ArrayHandle, EXYNOS_DRAM_BANK1_BASE, EXYNOS_DRAM_BANK1_SIZE);
  AddMappedRange (ArrayHandle, EXYNOS_DRAM_BANK2_BASE, EXYNOS_DRAM_BANK2_SIZE);
  AddMappedRange (ArrayHandle, EXYNOS_DRAM_BANK3_BASE, EXYNOS_DRAM_BANK3_SIZE + EXYNOS_DRAM_BANK4_SIZE);
  return EFI_SUCCESS;
}
