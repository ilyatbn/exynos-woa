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
    * the SYSREG_PERIC0 (0x10411000) window under I2C0. The 9820 DT has no node
      there; its USI sysreg block is `usi@10421000` with `usi3` at 0x10421010,
      which is the candidate, but the 9810's +0x1C slot does not translate, so
      it stays at the 9810 value and the driver header keeps the pair in
      agreement.
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
                Memory32Fixed (ReadWrite, 0x10411000, 0x00001000)
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
                    273  // VERIFIED 9820: dwc3 child SPI 0xf1 = 241, +32 = GSIV 273 (Exynos9820.h:522)
                }
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        // D8: ALIVE GPIO controller (Exynos9820Gpio driver, GpioClx provider).
        // Window VERIFIED: pinctrl@15850000 +0x1000 (live DTB; Exynos9820.h).
        // The 34 Interrupt entries are the wakeup-EINT lines EINT0..EINT33 in
        // pin order (GpioClx maps the Nth entry to pin N; pattern from the
        // official gpio/samples ASL). VERIFIED: INTREQ__ALIVE_EINTn = 5+n
        // (vendor dt-bindings irq header), GSIV = SPI + 32 — so GSIVs 37..70
        // (notes/d8-audio.md §3). Banks: gpa0..gpa3 8 pins, gpa4 2 pins.
        // Level/ActiveHigh/Shared follows the sample convention; the driver
        // re-programs EINT_CON per pin anyway (ActiveBoth is emulated by
        // GpioClx: learn.microsoft.com "Other ACPI Namespace Objects").
        // NOTE: TSP0 above also claims this window for the touch EINT —
        // Windows maps both; the two drivers touch disjoint registers.
        Device (GPIO)
        {
            Name (_HID, "SAMS9821")   // vendor _HID for the Exynos9820 ALIVE pinctrl
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x15850000, 0x00001000)  // VERIFIED pinctrl@15850000
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , )
                {
                    37, 38, 39, 40, 41, 42, 43, 44,   // gpa0-0..7 = EINT0..7  = SPI 5..12
                    45, 46, 47, 48, 49, 50, 51, 52,   // gpa1-0..7 = EINT8..15 = SPI 13..20 (45 = touch EINT, cf. TSP0)
                    53, 54, 55, 56, 57, 58, 59, 60,   // gpa2-0..7 = EINT16..23 = SPI 21..28 (55 = CS47L93 codec irq, 57 = power key)
                    61, 62, 63, 64, 65, 66, 67, 68,   // gpa3-0..7 = EINT24..31 = SPI 29..36
                    69, 70                            // gpa4-0..1 = EINT32..33 = SPI 37..38
                }
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        // D8: the four hardware buttons as a generic ACPI button device.
        // Format VERIFIED from learn.microsoft.com "ACPI Button Device"
        // (desktop-edition sample, fetched 2026-10-11); inbox Hidinterrupt.sys
        // binds ACPI0011 and synthesizes a HID device from the _DSD usages —
        // no custom HID driver needed. Pins VERIFIED from the shipped overlay
        // (ov00.dts gpio_keys): VOLUP gpa0-3, VOLDN gpa0-4, BIXBY gpa0-6,
        // POWER gpa2-4; pin numbers are GpioClx-linear (bank*8+pin).
        // Wake: POWER + BIXBY are SharedAndWake (the DT's gpio-key,wakeup
        // flags); the volume keys are not wake sources on Android, mirrored.
        // DebounceTimeout 0x2710 = 10 000 x 100us = 10 ms, matching the
        // DT's debounce-interval = <10>; the driver sets EmulateDebouncing
        // so GpioClx applies it in software (the HW FLTCON filter is much
        // shorter). Bixby maps to AC Search (0x0C/0x221) — Windows has no
        // Bixby usage.
        Device (BTNS)
        {
            Name (_HID, "ACPI0011")   // generic button device (inbox Hidinterrupt.sys)
            Name (_UID, Zero)
            Name (_DEP, Package (1)
            {
                \_SB.GPIO
            })
            Name (_CRS, ResourceTemplate ()
            {
                GpioInt (Edge, ActiveBoth, SharedAndWake, PullUp, 0x2710,
                    "\\_SB.GPIO", 0x00, ResourceConsumer, , )
                { 20 }   // index 0: POWER  (gpa2-4)
                GpioInt (Edge, ActiveBoth, Shared, PullUp, 0x2710,
                    "\\_SB.GPIO", 0x00, ResourceConsumer, , )
                { 3 }    // index 1: VOLUME UP (gpa0-3)
                GpioInt (Edge, ActiveBoth, Shared, PullUp, 0x2710,
                    "\\_SB.GPIO", 0x00, ResourceConsumer, , )
                { 4 }    // index 2: VOLUME DOWN (gpa0-4)
                GpioInt (Edge, ActiveBoth, SharedAndWake, PullUp, 0x2710,
                    "\\_SB.GPIO", 0x00, ResourceConsumer, , )
                { 6 }    // index 3: BIXBY (gpa0-6)
            })
            Name (_DSD, Package ()
            {
                ToUUID ("FA6BD625-9CE8-470D-A2C7-B3CA36C4282E"),
                Package (5)
                {
                    Package (5)
                    {
                        0,      // Collection
                        1,      // collection unique ID
                        0,      // top-level
                        0x01,   // Generic Desktop page
                        0x0D    // Portable Device Control
                    },
                    Package (5)
                    {
                        1,      // Control
                        0,      // _CRS interrupt index: POWER
                        1,      // parent collection
                        0x01,   // Generic Desktop page
                        0x81    // System Power Down
                    },
                    Package (5)
                    {
                        1,      // Control
                        1,      // _CRS interrupt index: VOLUME UP
                        1,
                        0x0C,   // Consumer page
                        0xE9    // Volume Increment
                    },
                    Package (5)
                    {
                        1,      // Control
                        2,      // _CRS interrupt index: VOLUME DOWN
                        1,
                        0x0C,   // Consumer page
                        0xEA    // Volume Decrement
                    },
                    Package (5)
                    {
                        1,      // Control
                        3,      // _CRS interrupt index: BIXBY
                        1,
                        0x0C,   // Consumer page
                        0x221   // AC Search
                    }
                }
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
    }
}
