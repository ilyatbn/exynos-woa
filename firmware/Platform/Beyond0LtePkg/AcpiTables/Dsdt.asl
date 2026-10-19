/** @file
  DSDT for Exynos 9820 / Galaxy S10e (`beyond0lte`).

  Copied from Star2LtePkg's DSDT for the Exynos 9810 and changed only where a
  read says to change. NOTHING HERE IS VALIDATED on an S10e.

  CHANGED, because it is a verified read (notes/dock-desktop.md §5, §13, §14):
    * the device classes EXYN9810 -> EXYN9820 and SAMS9810 -> SAMS9820. The
      touchscreen keeps SCY0761 — the S10e's panel IC is a y761, the same
      family the Windows driver is already named after.
    * the four UFS windows: 0x111… -> 0x13D60000 / 0x13D50000 / 0x13D64000
      (size 0x800, as the vendor DT declares) / 0x13D70000.
    * the ALIVE GPIO window on the touchscreen: 0x14050000 -> 0x15850000.
    * the HSI2C #10 interrupt: GSIV 436 -> 420 (SPI 388 + 32, verified).
    * the eight CPU sub-devices. Their _MAT buffers are re-derived as the 9820
      MADT GICC entries: the buffer is an 80-byte EFI_ACPI_6_3_GIC_STRUCTURE
      (firmware/edk2/MdePkg/Include/IndustryStandard/Acpi63.h:520-539 - the
      widths sum to 0x50, so this is the 6.3 shape without the 6.5 TRBE pair),
      with MPIDR at offset 68 and ProcessorPowerEfficiencyClass at 76. The
      equality _MAT == the MADT entry with the matching _UID is not assumed: it
      is what the 9810 already did (its DSDT bytes reproduce its own Madt.aslc
      GICC entries field for field), and each sub-device here is an ACPI0007
      processor device whose _UID matches. Values now mirror Madt.aslc:149-163
      - MPIDRs 0x00-0x05 and 0x100-0x101, classes 2 (A55 x4) / 1 (prometheus
      x2) / 0 (cheetah x2), 0 being the most efficient. GICC/GICH/GICV and the
      VGIC GSIV 25 are unchanged because Exynos9820.h:139-146 VERIFIEDs the
      GIC windows identical to the 9810.

  DELIBERATELY UNCHANGED, and it is NOT correct on this SoC:
    (M12: the SYSREG window under I2C0 moved from the 9810's 0x10411000 to the
      9820's PERIC0 USI sysreg page 0x10421000 - live DT usi@1042101C, label usi_3,
      is USI03's mode register, the driver's +0x1C; Exynos9820.h
      EXYNOS_PERIC0_USI_SYSREG_BASE. notes/m12-touch.md.)
    The CMU window IS now correct: `SFR_BLOCK(CMU_PERIC0, 0x10400000, 0x8000)`
    in Samsung's cal-if table (drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c)
    confirms the 9810 base and gives the full 0x8000 extent, which is what the
    line below advertises.
    * the parked Storport interrupt (GSIV 259) on UFS0. It is chosen to be a
      line with no consumer, so it never asserts; the 9820's own unconsumed
      number has not been identified.
    * the touchscreen's EINT: 76 -> 45. The pin is the same (sec,irq_gpio =
      <&gpa1 0 0>) but the shipping DT lists gpa1's SPIs as 13..20, so gpa1-0 is
      SPI 13 and GSIV 45. Derived from the DT, not observed — notes/dock-
      desktop.md §18 says why, and the first confirmation is the panel raising
      an interrupt at a tap.
    * the _MAT Flags byte (offset 12) stays 1 = ENABLED on all eight sub-devices
      even in a BEYOND0LTE_UNIPROCESSOR_BOOT build, where the MADT marks the
      seven secondaries disabled. The DSDT has no access to that knob, so the
      two tables disagree in that configuration only; the shipped default (0)
      leaves them in agreement.
**/

#ifndef BEYOND0LTE_UFS_GSIV
#define BEYOND0LTE_UFS_GSIV  259   // parked line (default); 281 = the real UFS line, needs driver >= w43
#endif

DefinitionBlock ("Dsdt.aml", "DSDT", 2, "EXYNOS", "BEYOND0L", 0x00000002)
{
    Scope (\_SB)
    {
        Device (CPU0)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0)
            Name (_MAT, Buffer (0x50)
            {
                0x0B, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x20, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x60, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x40, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU1)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 1)
            Name (_MAT, Buffer (0x50)
            {
                0x0B, 0x50, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x20, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x60, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x40, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU2)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 2)
            Name (_MAT, Buffer (0x50)
            {
                0x0B, 0x50, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x20, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x60, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x40, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU3)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 3)
            Name (_MAT, Buffer (0x50)
            {
                0x0B, 0x50, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
                0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x20, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x60, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x40, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU4)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 4)
            Name (_MAT, Buffer (0x50)
            {
                0x0B, 0x50, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
                0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x20, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x60, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x40, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU5)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 5)
            Name (_MAT, Buffer (0x50)
            {
                0x0B, 0x50, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
                0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x20, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x60, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x40, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU6)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 6)
            Name (_MAT, Buffer (0x50)
            {
                0x0B, 0x50, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
                0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x20, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x60, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x40, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU7)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 7)
            Name (_MAT, Buffer (0x50)
            {
                0x0B, 0x50, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
                0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x20, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x60, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x40, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (UFS0)
        {
            Name (_HID, "EXYN9820")
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x13D60000, 0x00002000)
                Memory32Fixed (ReadWrite, 0x13D50000, 0x00008000)
                Memory32Fixed (ReadWrite, 0x13D64000, 0x00000800)
                Memory32Fixed (ReadWrite, 0x13D70000, 0x00001000)
                //
                // Parked interrupt for Storport admission. Storport's line-based
                // connect path hardcodes InterruptMode = LevelSensitive (verified in
                // storport.sys: RaidAdapterConnectInterrupt reads the resource's
                // Latched flag but never forwards it, and the IoConnectInterruptEx
                // wrapper stores 0 into IO_CONNECT_INTERRUPT_PARAMETERS.
                // FullySpecifiedGroup.InterruptMode). Declaring this line Edge made
                // IoConnectInterruptEx return STATUS_NOT_SUPPORTED (0xC00000BB),
                // which storport propagates verbatim, failing adapter start with
                // PnP Code 10. It must therefore be Level.
                //
                // GSIV 259 = GIC SPI 227, which has no consumer in the device tree,
                // so the line never asserts. The miniport completes by polling with
                // UTRD IOC cleared and a no-op ISR, so a level-triggered parked line
                // is inert. The real UFS line is GSIV 281 / SPI 249 (VERIFIED 2026-10-19: TWRP
                // /proc/interrupts "GIC-0 281 Edge ufshcd"; stock DT interrupts <0 0xf9 0>; the
                // earlier "274 / SPI 242" here was wrong). BEYOND0LTE_UFS_GSIV 281 connects it - ONLY
                // with Exynos9820Ufs.sys >= w43, whose ISR acknowledges IS armed or not (an older
                // driver's ISR returns FALSE -> level-interrupt storm). notes/ufs-async-plan.md stage 3.
                //
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { BEYOND0LTE_UFS_GSIV }
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (I2C0)
        {
            Name (_HID, "SAMS9820")
            Name (_UID, 10)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x104B0000, 0x00001000)
                Memory32Fixed (ReadWrite, 0x10400000, 0x00008000)  // cal-if SFR_BLOCK(CMU_PERIC0) size 0x8000
                Memory32Fixed (ReadWrite, 0x10421000, 0x00001000)  // USI sysreg page (usi@1042101C = USI03 mode)
                Memory32Fixed (ReadWrite, 0x10430000, 0x00001000)
                Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive)
                {
                    420   // VERIFIED 9820: hsi2c@104B0000 declares SPI 0x184 = 388, +32 = 420
                }
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (TSP0)
        {
            Name (_HID, "SCSY0761")
            Name (_UID, 0)
            Name (_DEP, Package (1)
            {
                \_SB.I2C0
            })
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (
                    0x0048,
                    ControllerInitiated,
                    400000,
                    AddressingMode7Bit,
                    "\\_SB.I2C0",
                    0x00,
                    ResourceConsumer,
                    ,
                    Exclusive
                )
                Interrupt (ResourceConsumer, Level, ActiveLow, Exclusive)
                {
                    45   // DERIVED 9820: sec,irq_gpio = <&gpa1 0 0>, and the shipping DT lists gpa1's SPIs as 13..20, so gpa1-0 is SPI 13 and GSIV = 13 + 32 = 45. The 9810's was GSIV 76 (SPI 44) — same pin name, renumbered because the ALIVE pinctrl moved from 0x14050000 to 0x15850000.
                }
                Memory32Fixed (ReadWrite, 0x15850000, 0x00001000)
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        // USB host controller (D3): the DWC3 core's xHCI-compliant register
        // window. Windows' INBOX usbxhci.inf matches ACPI\PNP0D10 (fact,
        // notes/d1b-usb-vbus-hcd.md Q6: "usbxhci.inf: PCI\CC_0C0330 ACPI\PNP0D15
        // ACPI\PNP0D10"), so no Windows driver is needed IF the firmware has
        // left the core in host role — Beyond0LteUsbHostDxe does that before
        // ExitBootServices and the DWC3 globals block (+0xC100) is deliberately
        // NOT in _CRS: USBXHCI must never re-touch it (contract recorded in
        // notes/d1-usb-host.md Q6). Shape follows the Marvell Armada70x0 EDK2
        // DSDT (Memory32Fixed 0x8000 = DWC3_XHCI_REGS_END 0x7fff + 1).
        // _CCA 0 (M16 review): the vendor DT does NOT mark usb@10C00000
        // dma-coherent and the UEFI side uses DmaTypeNonCoherent. _CCA 1 would
        // tell Windows the device snoops the CPU caches, so it would skip the
        // cache maintenance and transfers would see stale data. Same
        // declaration as UFS0 above, which Windows already runs with.
        Device (XHC0)
        {
            Name (_HID, "PNP0D10")   // xHCI-compliant USB controller with standard debug (Microsoft SoC _CID table)
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x10C00000, 0x00008000)  // VERIFIED usb@10C00000, xHCI regs 0..0x7FFF
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                {
                    273  // VERIFIED 9820: dwc3 child SPI 0xf1 = 241, +32 = GSIV 273 (EXYNOS_USBDRD_GSIV, Exynos9820.h)
                }
            })
            // M16: present only when Beyond0LteUsbHostDxe brought the PHY/DWC3 up this boot (it writes
            // 'USBH' to the progress page word [2], 0x8FE0F008; SEC clears it). Otherwise USBXHCI would
            // touch a block whose ACPM-owned clocks may be off (notes/m16-usb.md).
            OperationRegion (USBS, SystemMemory, 0x8FE0F008, 4)
            Field (USBS, DWordAcc, NoLock, Preserve) { USBH, 32 }
            Method (_STA, 0, NotSerialized)
            {
                If (LEqual (USBH, 0x48425355)) { Return (0x0F) }
                Return (Zero)
            }
        }

        // M17 (notes/m17-usb-kd.md): the same DWC3 as a USB FUNCTION for Windows kernel debugging. Dbg2.aslc's
        // KDNET entry (0x8003/0x5143, inbox kd_8003_5143.dll) names "\_SB.UFN0"; Windows then reserves this
        // device for the kernel debugger and loads no driver on it. Present only when Beyond0LteUsbHostDxe left
        // the core in DEVICE role this boot ('USBD' in word [2]; host mode writes 'USBH' instead, so XHC0 and
        // UFN0 are never present together). No interrupt: the KD transport is polled. The window covers the
        // whole block (globals + device registers at 0xC100..0xCFFF), unlike XHC0's xHCI-only 0x8000.
        Device (UFN0)
        {
            Name (_HID, "SAMS9821")  // project-local ID: no Windows driver binds; the debugger owns the block
            Name (_UID, Zero)
            Name (_CCA, Zero)        // not dma-coherent, same as XHC0
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x10C00000, 0x00010000)  // EXYNOS_USBDRD_BASE / _SIZE (Exynos9820.h)
            })
            OperationRegion (USBS, SystemMemory, 0x8FE0F008, 4)
            Field (USBS, DWordAcc, NoLock, Preserve) { USBD, 32 }
            Method (_STA, 0, NotSerialized)
            {
                If (LEqual (USBD, 0x44425355)) { Return (0x0F) }
                Return (Zero)
            }
        }

        // M12 WiFi milestone 1 (notes/m12-wifi-m1.md): the BCM4375 WLAN endpoint behind PCIe root
        // port 0. NOT a PCI bind (docs/exynos9820-port/M12-WIFI-DESIGN.md section 2): Beyond0LtePcieDxe
        // trains the link, programs the iATU windows, sizes + assigns the EP BARs and latches them;
        // this device hands the Windows driver exactly the two translated windows.
        // VERIFIED windows (vendor DT pcie0@13ED0000, Exynos9820.h):
        //   CFG0 iATU window 0x14FFE000 +0x2000 (bus1 dev0 func0 config)   EXYNOS_PCIE0_CFG_BASE/SIZE
        //   MEM iATU window  0x14000000 +0xFF0000 (1:1; EP BARs land here)  EXYNOS_PCIE0_MEM_BASE/SIZE
        // VERIFIED IRQ: INTREQ__PCIE_WIFI0 = 253 -> GSIV 285 (EXYNOS_PCIE0_GSIV), level-high. The UEFI
        // half does not enable INTx at the root port yet; the milestone-1 driver does not connect it.
        // The ELBI/DBI/PHY/IA windows are deliberately NOT here: the role-latch contract (same as XHC0)
        // keeps the controller itself out of the OS path.
        // _CRS ORDER IS A BINDING CONTRACT: Exynos9820Wlan takes memory descriptor 0 = CFG, 1 = MEM.
        // _CCA 0: whether the EP's DMA is coherent is open item P5 (the board DT asks for coherency
        // and SysMMU; we program the sharability bits but run without the SysMMU) - milestone 1 does no
        // DMA, so the conservative value (Windows does cache maintenance) is used, like XHC0/UFS0.
        // Present only when Beyond0LtePcieDxe brought the link up this boot and read a Broadcom vendor
        // ID: it writes 'WLAN' (0x4E414C57) to the progress page word [3], 0x8FE0F00C; SEC clears it.
        Device (WLAN)
        {
            Name (_HID, "SAMS9825")   // project-local ID for the BCM4375 WLAN device (SAMS9821 = UFN0)
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x14FFE000, 0x00002000)  // CFG0 window
                Memory32Fixed (ReadWrite, 0x14000000, 0x00FF0000)  // MEM window (EP BARs)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                {
                    285  // VERIFIED: INTREQ__PCIE_WIFI0 253 + 32 (Exynos9820.h EXYNOS_PCIE0_GSIV)
                }
            })
            OperationRegion (WLST, SystemMemory, 0x8FE0F00C, 4)
            Field (WLST, DWordAcc, NoLock, Preserve) { WLNS, 32 }
            Method (_STA, 0, NotSerialized)
            {
                If (LEqual (WLNS, 0x4E414C57)) { Return (0x0F) }
                Return (Zero)
            }
        }

        // D4c Bluetooth (notes/bt-driver.md, D4c-BT-DESIGN.md): the BT function of the same BCM4375, on the dedicated
        // BT UART uart@10840000. Windows half = drivers/Exynos9820Bt (serialhcibus-based BthxDDI transport, direct MMIO).
        // _CRS ORDER: the driver takes the first Memory (UART window, >= 0x100) and the first Interrupt.
        // GSIV 440 = DT SPI 408 + 32 (Exynos9820.h EXYNOS_UART_BT_GSIV). No GpioInt hostwake: no GPIO controller
        // device exists in this DSDT yet. NOT in _CRS (role latch by Beyond0LteBtUartDxe): CMU_PERIC1 UART_BT clock,
        // gpq0 pin mux, USI mode register, bt_en gpm8-0. _HID SAMS9829 (SAMS9826 is BAT0).
        // Present only when this boot's Beyond0LteBtUartDxe latched the role: progress word [11] = 'BTH0' (0x30485442)
        // at 0x8FE0F02C; the DXE clears it at entry every boot.
        Device (BTH0)
        {
            Name (_HID, "SAMS9829")
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x10840000, 0x00000100)  // VERIFIED uart@10840000 reg (base.dtb)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                {
                    440  // VERIFIED: SPI 408 + 32
                }
            })
            OperationRegion (BTST, SystemMemory, 0x8FE0F02C, 4)
            Field (BTST, DWordAcc, NoLock, Preserve) { BTNS, 32 }
            Method (_STA, 0, NotSerialized)
            {
                If (LEqual (BTNS, 0x30485442)) { Return (0x0F) }
                Return (Zero)
            }
        }

        // D10 video codec (notes/d10-mfc-main.md): the MFC v13 (ip_ver 0x1300). Windows half = drivers/Exynos9820Mfc.
        // _CRS ORDER: driver takes the first Memory (MFC core window) and the first Interrupt (GSIV 370 = SPI 338 + 32).
        // NOT in _CRS: pd-mfc (sboot leaves it on, UEFI R1 read), CMU_MFC clocks (on at hand-off), the two SYSMMUs
        // (left in reset bypass: the driver uses physically contiguous buffers below 4 GiB, as UEFI R3/R4 proved).
        // _HID SAMS982A (the branch's SAMS9828 is GPU0 on main).
        // Present only when this boot's Beyond0LteMfcDxe saw pd-mfc on: progress word [13] = 'VIDC' (0x43444956) at
        // 0x8FE0F034; the DXE clears it at entry every boot.
        Device (VIDC)
        {
            Name (_HID, "SAMS982A")
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x188D0000, 0x00010000)  // VERIFIED base.dtb /mfc@188D0000 reg
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                {
                    370  // VERIFIED: DT cell 0x152 = SPI 338 + 32
                }
            })
            OperationRegion (VDST, SystemMemory, 0x8FE0F034, 4)
            Field (VDST, DWordAcc, NoLock, Preserve) { VDNS, 32 }
            Method (_STA, 0, NotSerialized)
            {
                If (LEqual (VDNS, 0x43444956)) { Return (0x0F) }
                Return (Zero)
            }
        }

        // Battery (notes/battery-driver-m1.md): the MAX77705 fuel gauge (7-bit 0x36) and charger (0x69) sit on
        // HSI2C45, the CMGP USI03 block. drivers/S10eBattery (KMDF battc miniclass) binds this ID and owns the
        // bus itself, polled, exactly as Beyond0LteUsbHostDxe/Pmic.c does in UEFI: so the three windows below
        // are the I2C block, the CMGP sysreg page holding the USI_CMGP3 mode register (+0x30) and the CMGP pin
        // controller page holding gpm12/gpm13 CON (+0x180/+0x1A0). No interrupt (polled), no CMU (ACPM-owned
        // clocks, left running). The driver refuses to start unless these equal the Exynos9820.h values.
        // Windows takes the AC state from the battery's BATTERY_POWER_ON_LINE flag: no ACPI0003 device.
        Device (BAT0)
        {
            Name (_HID, "SAMS9826")  // project-local ID (SAMS9820 I2C, SAMS9821 UFN0, SAMS9825 WLAN are taken)
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x15D60000, 0x00001000)  // EXYNOS_PMIC_HSI2C_BASE / _SIZE (hsi2c_45)
                Memory32Fixed (ReadWrite, 0x15C22000, 0x00001000)  // EXYNOS_CMGP_SYSREG_USI_PAGE / _SIZE
                Memory32Fixed (ReadWrite, 0x15C30000, 0x00001000)  // EXYNOS_CMGP_GPIO_BASE / _SIZE
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        // Audio (notes/d8-audio-win-m1.md): loudspeaker path ABOX RDMA0 -> UAIF1 -> CS35L40 @0x40/@0x41 on hsi2c_38,
        // device-validated from UEFI at boot #99 (AudioSpeaker.c). drivers/Exynos9820Audio (ACX 1.0) binds this ID,
        // drives the ABOX SFRs directly (DSP held off) and owns hsi2c_38 polled, so no SpbCx/I2C device claims that
        // block. _CRS ORDER IS A BINDING CONTRACT (Exynos9820Audio.h AUD_RES_*; the driver refuses to map anything
        // on a mismatch): ABOX IFR, SYSREG_AUD, CMU_AUD, GPIO_AUD, HSI2C38, ABOX SRAM, the PMU page holding
        // AUD_CONFIGURATION/STATUS/PAD (0x15861900/04/20: every AUD access is gated on AUD_STATUS bit0), and the
        // PERIC1 pin page (gph0-6/7 = hsi2c_38 SCL/SDA, gph1-1/2 = amp resets). All from Exynos9820.h; CMU_AUD and
        // GPIO_AUD are SOURCE-ONLY there but were read live at boot #96. No interrupt (polled, timer-clocked).
        // _CCA 0: the RDMA does not snoop; the driver uses a non-cached ring.
        Device (AUD0)
        {
            Name (_HID, "SAMS9827")  // project-local ID (SAMS9820 I2C, 9821 UFN0, 9825 WLAN, 9826 BAT0 are taken)
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x18C50000, 0x00010000)  // EXYNOS_ABOX_IFR_BASE / _SIZE
                Memory32Fixed (ReadWrite, 0x18C10000, 0x00010000)  // EXYNOS_ABOX_SYSREG_BASE / _SIZE
                Memory32Fixed (ReadWrite, 0x18C00000, 0x00008000)  // EXYNOS_CMU_AUD_BASE / _SIZE
                Memory32Fixed (ReadWrite, 0x18C60000, 0x00001000)  // EXYNOS_PINCTRL_AUD_BASE / _SIZE
                Memory32Fixed (ReadWrite, 0x109A0000, 0x00001000)  // EXYNOS_AUDIO_HSI2C_BASE / _SIZE (hsi2c_38)
                Memory32Fixed (ReadWrite, 0x18D00000, 0x00059000)  // EXYNOS_ABOX_SRAM_BASE / _SIZE
                Memory32Fixed (ReadWrite, 0x15861000, 0x00001000)  // EXYNOS_PMU_BASE + 0x1000 (AUD_* offsets 0x19xx)
                Memory32Fixed (ReadWrite, 0x10830000, 0x00001000)  // EXYNOS_PERIC1_GPIO_BASE / _SIZE
            })
            // Rescue switch (2026-10-12): \S10EDIAG\noaudio.flg on the ESP -> BootLogDxe writes 'NOAU' (0x55414F4E) to
            // progress word [5], 0x8FE0F014, at ReadyToBoot (and 0 when the file is absent) -> AUD0 hidden, so the
            // audio driver never loads (run audA5b/c: Windows reset ~35 s into boot with Exynos9820Audio 1.0.1.4).
            OperationRegion (AUST, SystemMemory, 0x8FE0F014, 4)
            Field (AUST, DWordAcc, NoLock, Preserve) { AUDS, 32 }
            Method (_STA, 0, NotSerialized)
            {
                If (LEqual (AUDS, 0x55414F4E)) { Return (Zero) }
                Return (0x0F)
            }
        }

        // M16 G9 (notes/gpu-m16-g9b.md): the Mali-G76 ("G3D") for drivers/MaliG76Probe (G9b) and later the
        // reused wddm-mali-g52 KMD (G10/G11). _CRS ORDER IS A BINDING CONTRACT: the GPU register block must be
        // the FIRST memory descriptor (m1probe.c:231-238, malikm/device.c:43-48 and maliwddm/adapter.c:106-126
        // all keep the first CmResourceTypeMemory). Second: the PMU page holding EMBEDDED_G3D_STATUS (+0x2104)
        // and G3D_STATUS (+0x2384), so MaliG76Probe re-checks the power state before its first GPU access (an
        // access to the unpowered domain is a bus fault). Interrupts in DT order JOB, MMU, GPU (stock dt.img,
        // notes/gpu-odin-rom.md:55-56); the reused ISR reads all three INT_STAT blocks, so the order is free
        // (malikm/device.c:49-59, maligpu.c:373-424). Trigger: the DT flags cell is 0; Level/ActiveHigh follows
        // this DSDT's convention and is UNVERIFIED for these lines. _CCA 0: no `system-coherency` in the stock
        // DT, the reused KMD is non-coherent (notes/gpu-wddm-mali-g52.md R12).
        // Present only when Beyond0LteGpuProbeDxe saw the G3D domain ON at the end of its run (both PMU status
        // bits, GPU clock not gated off, GPU_ID = G76): it writes 'GPU0' (0x30555047) to the progress page
        // word [4], 0x8FE0F010; SEC clears it. With BEYOND0LTE_GPU_POWER_ON 0 and sboot leaving the domain off,
        // the device stays hidden and no Windows driver can touch an unpowered GPU.
        Device (GPU0)
        {
            Name (_HID, "SAMS9828")  // project-local ID (9820 I2C, 9821 UFN0, 9822 GPIO plan, 9823/9824 audio plan, 9825-9827 taken)
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x18500000, 0x00005000)  // EXYNOS_G3D_BASE / _SIZE (Exynos9820.h, mali@18500000)
                Memory32Fixed (ReadWrite, 0x15862000, 0x00001000)  // EXYNOS_PMU_BASE + 0x2000 (EMBEDDED_G3D_STATUS 0x2104, G3D_STATUS 0x2384)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 320 }  // JOB: SPI 288 + 32
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 319 }  // MMU: SPI 287 + 32
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 321 }  // GPU: SPI 289 + 32
            })
            OperationRegion (GPST, SystemMemory, 0x8FE0F010, 4)
            Field (GPST, DWordAcc, NoLock, Preserve) { GPUS, 32 }
            Method (_STA, 0, NotSerialized)
            {
                If (LEqual (GPUS, 0x30555047)) { Return (0x0F) }
                Return (Zero)
            }
        }
    }
}
