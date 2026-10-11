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
VOID UsbPhyDeviceModeDeltas (VOID);   // M17: Linux device-mode PHY values (TWRP debugfs read)
VOID UsbPhyHostModeDeltas (VOID);     // M16: Linux host-mode PHY values (TWRP capture): pull-downs on, no VBUS force

//
// Dwc3.c — DWC3 core init (dwc3_core_init). Revision-gated quirk branches
// follow the kernel exactly; Revision carries the full GSNPSID. Device = TRUE
// stops after the role switch (M17: Windows' KD transport owns device mode).
//
EFI_STATUS Dwc3CoreInit (IN BOOLEAN Device, OUT UINT32 *Revision);

//
// Pmic.c — polled HSI2C master on the MAX77705 bus + host-role VBUS.
//
EFI_STATUS PmicBusInit (VOID);
BOOLEAN PmicBusProbe (VOID);
VOID PmicBusRelease (VOID);
VOID Hsi2cDump (IN CONST CHAR8 *Label);
EFI_STATUS Hsi2cXfer (IN UINT8 SlaveAddr, IN BOOLEAN Read, IN OUT UINT8 *Data, IN UINTN Length, IN BOOLEAN Stop);
EFI_STATUS Max77705RegWrite (IN UINT8 ClientAddr, IN UINT8 Reg, IN UINT8 Value);
EFI_STATUS Max77705RegRead (IN UINT8 ClientAddr, IN UINT8 Reg, OUT UINT8 *Value);
EFI_STATUS Max77705VbusOn (VOID);
EFI_STATUS Max77705WaitSourceAttach (IN UINTN TimeoutMs, OUT UINT8 *CcStatus);
EFI_STATUS Max77705UsbSwitchToAp (VOID);   // M17: COM switch -> USB-AP via the opcode mailbox, logged
VOID Max77705CcicInit (VOID);   // M16: DRP config the kernel sends at probe (opcodes 0x54, 0x0C, CTRLREG3)

//
// FuelGauge.c — read-only MAX77705 fuel-gauge report (battery % PoC,
// notes/battery-driver.md). Reads only; never writes a gauge register.
//
EFI_STATUS FuelGaugeReport (VOID);

#endif // USB_HOST_INTERNAL_H_
