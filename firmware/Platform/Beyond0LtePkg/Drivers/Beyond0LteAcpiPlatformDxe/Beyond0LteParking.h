/** @file
  m14: ACPI parking protocol bring-up of the Windows secondaries (Beyond0LteParking.c).
**/

#ifndef BEYOND0LTE_PARKING_H_
#define BEYOND0LTE_PARKING_H_

#include <IndustryStandard/Acpi.h>

/**
  Start the BEYOND0LTE_SMP_PARK_MASK secondaries into the parking stub and patch the
  (not yet installed) MADT: parked GICCs get ParkingProtocolVersion 1 + ParkedAddress,
  every other enabled secondary GICC is disabled.

  @param[in,out]  Madt  The MADT buffer read from the FV, before InstallAcpiTable.
**/
VOID
Beyond0LteParkSecondaries (
  IN OUT EFI_ACPI_DESCRIPTION_HEADER  *Madt
  );

#endif // BEYOND0LTE_PARKING_H_
