// UsbHostInternal.h — shared prototypes for Beyond0LteUsbHostDxe.
//
// The driver mirrors the Linux downstream bring-up for this SoC
// (notes/d1-usb-host.md §"What this means for our UEFI port", steps 1-7).
// The xHCI half itself is NOT written here: we register the controller as an
// EDK2 "non-discoverable" MMIO device so the stock MdeModulePkg XhciDxe
// drives it (NXP Layerscape UsbHcdInitDxe is the precedent,
// notes/d1b-usb-vbus-hcd.md Q3).

#ifndef USB_HOST_INTERNAL_H_
#define USB_HOST_INTERNAL_H_

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "UsbHw.h"

//
// UsbPhy.c — USBCON PHY block + PMU isolation (USB2/HS-first; the SS PMA is
// deliberately held powered-down, see notes/d1-usb-host.md Q2 end).
//
EFI_STATUS UsbPhyInit (VOID);

//
// Dwc3.c — DWC3 core, HOST path of dwc3_core_init. Revision-gated quirk
// branches follow the kernel exactly; Revision carries the full GSNPSID.
//
EFI_STATUS Dwc3HostInit (OUT UINT32 *Revision);

//
// Pmic.c — polled HSI2C master on the MAX77705 bus + host-role VBUS.
//
EFI_STATUS PmicBusInit (VOID);
BOOLEAN PmicBusProbe (VOID);
VOID Hsi2cDump (IN CONST CHAR8 *Label);
EFI_STATUS Hsi2cXfer (IN UINT8 SlaveAddr, IN BOOLEAN Read, IN OUT UINT8 *Data, IN UINTN Length, IN BOOLEAN Stop);
EFI_STATUS Max77705RegWrite (IN UINT8 ClientAddr, IN UINT8 Reg, IN UINT8 Value);
EFI_STATUS Max77705RegRead (IN UINT8 ClientAddr, IN UINT8 Reg, OUT UINT8 *Value);
EFI_STATUS Max77705VbusOn (VOID);
EFI_STATUS Max77705WaitSourceAttach (IN UINTN TimeoutMs, OUT UINT8 *CcStatus);

//
// FuelGauge.c — read-only MAX77705 fuel-gauge report (battery % PoC,
// notes/battery-driver.md). Reads only; never writes a gauge register.
//
EFI_STATUS FuelGaugeReport (VOID);

#endif // USB_HOST_INTERNAL_H_
