# M17 - Windows kernel debugging over USB (KDNET over USB device mode)

Goal: debug the Windows kernel live over the S10e's USB-C port (WinDbg on the PC) - for the M4 big cores
hang (notes/m14-smp.md: "Needs kernel-level debugging (KDNET/USB) - parked"), early bugchecks like 0x14C
(notes/m16-usb.md), and driver work generally.

## Known going in (2026-10-11)
- DWC_usb31 1.70a on this SoC has NO xHCI Debug Capability (UEFILOG run 4: DebugCapSupOffset 0xFFFFFFFF,
  notes/m16-usb.md) -> KDUSB over xHCI DbC is impossible. The route is the controller in DEVICE mode as a
  Windows KD transport (KDNET over USB EEM, if Windows supports it on DWC3).
- DSDT/ACPI: no DBG2/SPCR (notes/m10-windows-deploy-plan.md:63).
- Prior art in tree: Star2LtePkg PlatformBootManagerLib UsbDebugHid.c / UsbDebug.c = DWC3 peripheral-mode
  register vocabulary (PRTCAP_DEVICE, EP0 + HID IN), compiled off (MILESTONES.md:284, notes/dock-desktop.md §9).
- Our UEFI already brings DWC3 up in HOST mode (Beyond0LteUsbHostDxe: PHY, clocks, DWC3 core) - reusable.

## Progress log
- 2026-10-11: task opened; research split: (A) Windows side - DBG2 encoding, kd extension module, bcdedit,
  host side; (B) repo inventory of reusable device-mode code.
- Host (x64, 26100) System32 kd modules: kdnet.dll, kdnet_uart16550.dll, kdusb.dll, kdcom.dll, kd_02_<vid>
  (class 02 = network NICs), kd_07_1415 (class 07 serial, Oxford), **kd_0C_8086.dll** (class 0C = serial bus /
  USB, vendor Intel). INFERRED: kd_0C_8086 = KDNET over USB for Intel xDCI, which is a Synopsys DWC3 device
  controller -> Windows likely already has a DWC3 device-mode KD transport. TODO: list System32\kd*.dll in the
  ARM64 image (TWRP read after Setup) - which kd_0C_* it ships decides the DBG2 subtype/vendor.
