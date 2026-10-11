// Beyond0LteUsbHostDxe.c — bring the S10e's USB-C port up as a HOST and
// publish its xHCI interface to the EDK2 USB stack.
//
// Shape follows NXP Layerscape's UsbHcdInitDxe (the upstream precedent for a
// memory-mapped DWC3/xHCI in EDK2, notes/d1b-usb-vbus-hcd.md Q3): we do the
// platform-specific bring-up (PHY + DWC3 globals + VBUS), then call
// RegisterNonDiscoverableMmioDevice so NonDiscoverablePciDeviceDxe
// synthesizes a PciIo and the STOCK MdeModulePkg XhciDxe drives the
// controller. No xHCI ring/transfer code lives here.
//
// Everything device-specific is a read recorded in notes/d1-usb-host.md,
// notes/d1b-usb-vbus-hcd.md and <Platform/Exynos9820.h>; the core revision is
// read at runtime (GSNPSID), never assumed. NOTHING in this driver has run
// on an S10e yet: the first device-visible test is a keyboard behind a
// non-powered hub reaching ConIn.

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include "UsbHostInternal.h"

// The xHCI register window inside the DWC3 block: 0x10C00000 + 0x0000..0x7FFF
// (dwc3/host.c xhci_resources; DWC3_XHCI_REGS_END = 0x7fff).
#define XHCI_MMIO_SIZE  0x8000

// How long to wait at entry for a hub/dock to already be attached (the CCIC
// is DRP from the bootloader, so attach detection needs no work from us —
// only the read). A late plug-in is NOT re-polled in this first cut: the
// keyboard must be plugged in before boot, which matches the donation-build
// usage (dock first, then power on).
#define CC_ATTACH_TIMEOUT_MS  3000

// M16 (notes/m16-usb.md): 0 = skip the whole bring-up (XHC0 then stays hidden from Windows).
#define BEYOND0LTE_USB_HOST  1

// M16 run 7: 0 = bring USB up in UEFI but keep XHC0 hidden from Windows (no 'USBH' word). Every boot
// after the first one in which Windows installed usbxhci on XHC0 (run 4) bugchecks 0x14C, even on the
// run-4 image itself - this isolates Windows' early-boot USBXHCI start on our controller.
#define BEYOND0LTE_USB_EXPOSE_TO_WINDOWS  0

// M17 (notes/m17-usb-kd.md): 1 = bring the port up as a USB DEVICE for Windows kernel debugging (KDNET over
// USB EEM, inbox kd_8003_5143.dll via ACPI DBG2) instead of as a host. PHY + DWC3 in device role, no PMIC
// traffic (a PC host's Rp settles the CCIC to SINK by itself, no VBUS boost), no UEFI xHCI, XHC0 hidden;
// status word 'USBD' makes the DSDT's UFN0 (the DBG2 namespace device) present.
#define BEYOND0LTE_USB_KD_DEVICE  1

// Boot progress page (0x8FE0F000, WINPRAM, EfiRuntimeServicesData): [0] = the previous boot's last code,
// [1] = this boot's, [2] = USB status (cleared in SEC, ArmPlatformLib.c). The USB clock gates are
// ACPM-owned (notes/d1-usb-host.md); if sboot left them off, the first PHY/DWC3 read can hang the bus
// instead of returning 0/~0. Same self-healing shape as the parking 0x65 / DVFS 0x66 guards: 0x67 while
// the bring-up is in flight, and a boot whose previous boot ended at 0x67 skips it.
#define USB_PROGRESS_TRYING  0x67u
#define USB_STATUS_READY     0x48425355u   // 'USBH' - DSDT XHC0._STA returns 0x0F only on this value
#define USB_STATUS_KD_DEVICE 0x44425355u   // 'USBD' - DSDT UFN0._STA returns 0x0F only on this value
STATIC volatile UINT32  *CONST  mUsbProgress = (volatile UINT32 *)(UINTN)0x8FE0F000ULL;

STATIC
EFI_STATUS
UsbHostBringUp (
  VOID
  );

STATIC
EFI_STATUS
UsbKdDeviceBringUp (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Revision;

  // M17: route the USB-C D+/D- to the AP first - Linux opens the MAX77705 COM switch on every reboot
  // (notes/m17-muic-switch.md); without this the controller attaches internally but the host never sees it.
  // A failure here is logged, not fatal; the bus goes back to the bootloader's state either way.
  if (!EFI_ERROR (PmicBusInit ())) {
    (VOID)Max77705UsbSwitchToAp ();
  }

  PmicBusRelease ();

  Status = UsbPhyInit ();   // includes the UTMI/HSP VBUS-valid override the device side needs
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: PHY init %r\n", __func__, Status));
    return Status;
  }

  UsbPhyDeviceModeDeltas ();   // M17: Linux device-mode PHY bits (REWA bypass, RETENABLE, device tune, LINK_PCLK_SEL)

  Revision = 0;
  Status   = Dwc3CoreInit (TRUE, &Revision);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: DWC3 init %r\n", __func__, Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "%a: DWC3 rev %x in DEVICE role, left for the Windows KD transport\n", __func__, Revision));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Beyond0LteUsbHostEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Saved;

  mUsbProgress[2] = 0;
  if (!BEYOND0LTE_USB_HOST) {
    DEBUG ((DEBUG_WARN, "UsbHost: disabled (BEYOND0LTE_USB_HOST 0)\n"));
    return EFI_UNSUPPORTED;
  }
  if (mUsbProgress[0] == USB_PROGRESS_TRYING) {
    DEBUG ((DEBUG_ERROR, "UsbHost: previous boot hung in USB bring-up (progress 0x67) - skipping it\n"));
    return EFI_UNSUPPORTED;
  }

  Saved           = mUsbProgress[1];
  mUsbProgress[1] = USB_PROGRESS_TRYING;
  if (BEYOND0LTE_USB_KD_DEVICE) {
    Status          = UsbKdDeviceBringUp ();
    mUsbProgress[1] = Saved;
    if (!EFI_ERROR (Status)) {
      mUsbProgress[2] = USB_STATUS_KD_DEVICE;
    }

    DEBUG ((DEBUG_INFO, "UsbHost: KD device bring-up %r, UFN0 %a to Windows, XHC0 hidden\n", Status,
            EFI_ERROR (Status) ? "hidden" : "visible"));
    return Status;
  }

  Status          = UsbHostBringUp ();
  PmicBusRelease ();   // every exit path: the bus goes back to the bootloader's state (run 5: 0x14C otherwise)
  mUsbProgress[1] = Saved;
  if (!EFI_ERROR (Status) && BEYOND0LTE_USB_EXPOSE_TO_WINDOWS) {
    mUsbProgress[2] = USB_STATUS_READY;
  }
  DEBUG ((DEBUG_INFO, "UsbHost: bring-up %r, XHC0 %a to Windows\n", Status,
          (mUsbProgress[2] == USB_STATUS_READY) ? "visible" : "hidden"));
  return Status;
}

STATIC
EFI_STATUS
UsbHostBringUp (
  VOID
  )
{
  EFI_STATUS                      Status;
  UINT32                          Revision;
  UINT8                           CcStatus;
  BOOLEAN                         VbusOn;
  BOOLEAN                         CcicOk;
  EFI_HANDLE                      DeviceHandle;

  Revision  = 0;
  CcStatus  = 0;
  VbusOn    = FALSE;
  CcicOk    = FALSE;

  // 1. PMIC bus first: if the I2C block is not alive we can still bring up
  //    the controller, but VBUS will stay off — log it and continue.
  Status = PmicBusInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: HSI2C45 init %r — VBUS will stay OFF\n",
            __func__, Status));
  } else {
    CcicOk = PmicBusProbe ();   // M16 run 2: which MAX77705 clients answer
    // Battery PoC read (notes/battery-driver.md): proves the gauge path and
    // gives a boot-time SOC we can cross-check against Android. Report-only;
    // a failure here must never affect host bring-up.
    (VOID)FuelGaugeReport ();
  }

  // 2. PHY (USB2-first; SS PMA held down).
  Status = UsbPhyInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: PHY init %r — USB host disabled\n",
            __func__, Status));
    return Status;
  }

  // 3. DWC3 core -> HOST.
  Status = Dwc3CoreInit (FALSE, &Revision);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: DWC3 init %r — USB host disabled\n",
            __func__, Status));
    return Status;
  }

  // 4. VBUS: only with a source-role attach on the CC lines. Turning the
  //    boost on while the port sits idle is safe for the phone but pointless;
  //    the dangerous case (feeding 5 V into another host's port) cannot
  //    happen because the CCIC's DRP would have detected a host and settled
  //    to SINK, not SOURCE.
  //    The poll is skipped when the CCIC did not answer the probe (a dead bus
  //    would only add 30 timeouts to the boot).
  if (!CcicOk) {
    DEBUG ((DEBUG_ERROR, "%a: CCIC 0x25 not answering - no CC poll, VBUS stays OFF\n", __func__));
  } else if (!EFI_ERROR (Max77705WaitSourceAttach (CC_ATTACH_TIMEOUT_MS, &CcStatus))) {
    Status = Max77705VbusOn ();
    if (!EFI_ERROR (Status)) {
      VbusOn = TRUE;
      DEBUG ((DEBUG_INFO, "%a: CC source attach (CC_STATUS0 = %x), VBUS boost on\n",
              __func__, CcStatus));
    } else {
      DEBUG ((DEBUG_ERROR, "%a: VBUS boost failed %r — hub will not enumerate\n",
              __func__, Status));
    }
  } else {
    DEBUG ((DEBUG_WARN, "%a: no CC source attach within %d ms (CC_STATUS0 = %x) — "
            "plug the hub before power-on for ConIn\n",
            __func__, CC_ATTACH_TIMEOUT_MS, CcStatus));
  }

  // 5. Publish the controller. DmaTypeNonCoherent: the vendor DT marks UFS
  //    and G2D dma-coherent but NOT usb@10C00000 (notes/d1b Q3), so the
  //    wrapper must do cache maintenance on Map/Unmap.
  DeviceHandle = NULL;
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,                    // init already done here, at entry
             &DeviceHandle,
             1,
             EXYNOS_USBDRD_BASE, XHCI_MMIO_SIZE
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: NDD registration %r\n", __func__, Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "%a: xHCI @ %lx published (DWC3 rev %x, VBUS %a)\n",
          __func__, EXYNOS_USBDRD_BASE, Revision, VbusOn ? "on" : "off"));

  return EFI_SUCCESS;
}
