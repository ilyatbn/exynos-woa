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
                // is inert. The real UFS line is GSIV 274 / SPI 242 and stays absent.
                //
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 259 }
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
        // _CCA 1: standard WoA declaration (Windows' WDF DMA helpers do the
        // cache maintenance; the vendor DT does NOT mark this node
        // dma-coherent, and the UEFI side uses DmaTypeNonCoherent for the same
        // reason). UNVERIFIED on device until Windows enumerates it.
        Device (XHC0)
        {
            Name (_HID, "PNP0D10")   // xHCI-compliant USB controller with standard debug (Microsoft SoC _CID table)
            Name (_UID, Zero)
            Name (_CCA, 1)
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
    }
}
