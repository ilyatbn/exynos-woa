/** @file
  Exynos9820.h — central SoC register-block map for the Galaxy S10e
  (`beyond0lte`, SM-G970F), Exynos 9820.

  The single place the repo keeps MMIO base/size pairs and the framebuffer
  geometry, mirroring firmware/Silicon/Exynos9810Pkg/Include/Platform/
  Exynos9810.h for the Galaxy S9+. Both the UEFI memory map (PlatformMemoryMapLib)
  and the platform drivers include this, so there are no scattered magic numbers.

  NOTHING HERE IS VALIDATED ON HARDWARE. This header is the port's floor: every
  value is a documented read, not an observed one. Under the repo's golden rule a
  stage stays Guided until it has been run end to end on an S10e.

  VALUES SOURCED FROM (see notes/dock-desktop.md §5 and §13 in this repo):
    the shipping 9820 device tree — arch/arm64/boot/dts/exynos/exynos9820.dts
      from CruelKernel/samsung-exynos9820, branch cruel-HVJ5-v5.3, which lists
      G970F among its supported models. Numeric phandles show it is the
      decompiled DT of a shipping build, i.e. what the phone actually runs.
    the live S10e itself — /proc/device-tree/ and /sys/devices/platform/ node
      NAMES. Property *contents* are SELinux-blocked to an unprivileged shell,
      but node names carry the unit address, which is what makes this readable.
    the device's own /vendor/etc/fstab.exynos9820, which names
      /dev/block/platform/13d60000.ufs — an independent confirmation of the
      UFS HCI base.

  Convention: a base of 0 means "unknown / not configured". The memory map lib
  skips zero-based device windows so a placeholder never maps PA 0 by accident.

  Lines marked UNVERIFIED are NOT reads. They are either the 9810 value carried
  over so the tree still compiles, or a value whose address is known but whose
  extent or meaning is not. Do not treat them as facts.
**/

#ifndef EXYNOS9820_H_
#define EXYNOS9820_H_

//
// ---------------------------------------------------------------------------
// DRAM  [bank0 VERIFIED from the vendor DT; the rest are node names only]
//
// The live S10e publishes five memory nodes:
//   memory@80000000  memory@C0000000  memory@E1900000
//   memory@880000000 memory@900000000
// The vendor DT gives bank0 exactly:
//   memory@80000000  reg = <0x0 0x80000000 0x80000000>   (2 GiB)
//
// That is a DIFFERENT shape from the S9+: bank0 there was 0x3C800000 long and
// ended at 0xBC800000, and the UFS driver's DMA window was derived from that
// end. Here bank0 runs to 0x100000000 with no hole, so anything that assumed
// "DRAM ends at 0xBC800000" is wrong on this SoC — see EXYNOS_UFS_* below and
// the DMA window in drivers/Exynos9820Ufs/.
//
// For UEFI bring-up we still map ONLY bank0: it is the one bank whose extent
// is a read, and it is large enough to host the firmware. The other four must
// be published once their sizes are read from the DT (contents are blocked on
// the phone; the vendor DT has them).
// ---------------------------------------------------------------------------
//
#define EXYNOS_DRAM_BASE          0x0000000080000000ULL   // VERIFIED
#define EXYNOS_DRAM_SIZE          0x0000000080000000ULL   // VERIFIED (vendor DT reg)

#define EXYNOS_DRAM_BANK1_BASE    0x00000000C0000000ULL   // VERIFIED (node name)
#define EXYNOS_DRAM_BANK1_SIZE    0x0000000000000000ULL   // UNVERIFIED — size not yet read
#define EXYNOS_DRAM_BANK2_BASE    0x00000000E1900000ULL   // VERIFIED (node name)
#define EXYNOS_DRAM_BANK2_SIZE    0x0000000000000000ULL   // UNVERIFIED — size not yet read
#define EXYNOS_DRAM_BANK3_BASE    0x0000000880000000ULL   // VERIFIED (node name)
#define EXYNOS_DRAM_BANK3_SIZE    0x0000000000000000ULL   // UNVERIFIED — size not yet read
#define EXYNOS_DRAM_BANK4_BASE    0x0000000900000000ULL   // VERIFIED (node name)
#define EXYNOS_DRAM_BANK4_SIZE    0x0000000000000000ULL   // UNVERIFIED — size not yet read

//
// ---------------------------------------------------------------------------
// Debug UART (Samsung/Exynos UART — NOT 16550/PL011 compatible)
//
// VERIFIED on-device: uart@10440000 is the first PERIC0 UART, same base as on
// the S9+. The vendor DT declares the window as 0x100 (not the 9810's 0x1000)
// and fifo-size = 0x100, so the smaller size is used here rather than the
// 9810's. Many more UARTs exist (0x10450000 … 0x15D60000); 0x10440000 is the
// debug-console candidate, as on the 9810.
// ---------------------------------------------------------------------------
//
#define EXYNOS_UART_BASE          0x0000000010440000ULL   // VERIFIED (uart@10440000)
#define EXYNOS_UART_SIZE          0x0000000000000100ULL   // VERIFIED (vendor DT reg)

//
// ---------------------------------------------------------------------------
// GICv2 interrupt controller  [VERIFIED — IDENTICAL to the 9810]
//
// The 9820's node is interrupt-controller@10100000, compatible
// "arm,cortex-a15-gic", "arm,cortex-a9-gic" (GICv2, same architecture as the
// 9810's arm,gic-400). Its reg list, in DT order GICD, GICC, GICH, GICV:
//   <0x0 0x10101000 0x1000  0x0 0x10102000 0x1000
//    0x0 0x10104000 0x2000  0x0 0x10106000 0x2000>
// So the GIC does NOT move between these two SoCs — the 9810's MADT work
// transfers unchanged, which is the single largest simplification in this port.
//
// Do not confuse this with abox-gic@18cf0000 (reg-names "gicd","gicc"), which
// is Samsung's ABOX GIC interface, a different thing entirely.
// ---------------------------------------------------------------------------
//
#define EXYNOS_GICD_BASE          0x0000000010101000ULL   // VERIFIED (distributor)
#define EXYNOS_GICD_SIZE          0x0000000000001000ULL   // VERIFIED
#define EXYNOS_GICC_BASE          0x0000000010102000ULL   // VERIFIED (CPU interface)
#define EXYNOS_GICC_SIZE          0x0000000000001000ULL   // VERIFIED (vendor DT)
#define EXYNOS_GICH_BASE          0x0000000010104000ULL   // VERIFIED (hypervisor)
#define EXYNOS_GICH_SIZE          0x0000000000002000ULL   // VERIFIED
#define EXYNOS_GICV_BASE          0x0000000010106000ULL   // VERIFIED (virtual CPU)
#define EXYNOS_GICV_SIZE          0x0000000000002000ULL   // VERIFIED

//
// The exynos-pm node also publishes a small latch page inside the GICD window:
//   reg-names = "gpio_alive_base", "gicd_ispendrn_base"
//   reg       =  <0x0 0x15850000 0x1000  0x0 0x10101200 0x100>
// It is inside EXYNOS_GICD_BASE's page, so it needs no separate mapping, but
// anything that carves the GICD window must not treat +0x200..+0x300 as free.
//
#define EXYNOS_GICD_ISPENDRN_BASE 0x0000000010101200ULL   // VERIFIED (exynos-pm reg)
#define EXYNOS_GICD_ISPENDRN_SIZE 0x0000000000000100ULL   // VERIFIED

//
// ---------------------------------------------------------------------------
// Architected generic timer  [UNVERIFIED on this SoC]
//
// On the S9+ the stock bootloader does NOT program CNTFRQ_EL0 and the real
// counter frequency is 26 MHz, so the 9810 header hardcodes it. Whether the
// S10e's sboot behaves the same way has not been established: the value below
// is the 9810's, kept so the tree compiles, and the first on-device read of
// CNTFRQ_EL0 (or of the kernel's own ticks) decides it.
// ---------------------------------------------------------------------------
//
#define EXYNOS_ARCH_TIMER_FREQ_HZ 26000000u                // UNVERIFIED (9810 value)

//
// ---------------------------------------------------------------------------
// Power management unit (PMU) + chip ID  [PMU MOVES, chip ID does not]
//
// VERIFIED: the 9820's PMU is system-controller@15860000, compatible
// "samsung,exynos9820-pmu","syscon" — the 9810's 0x14060000 is NOT present here.
// The size below is the 9810's; the vendor DT's window for this node was not
// captured, so treat the extent as unverified even though the base is a read.
//
// The PMU is what the reset-to-recovery path and the watchdog mask live in, so
// a wrong base is not cosmetic: it is the difference between landing in TWRP
// and resetting into nothing.
// ---------------------------------------------------------------------------
//
#define EXYNOS_PMU_BASE           0x0000000015860000ULL   // VERIFIED (system-controller@15860000)
#define EXYNOS_PMU_SIZE           0x0000000000010000ULL   // UNVERIFIED (9810 extent)
#define EXYNOS_CHIPID_BASE        0x0000000010000000ULL   // VERIFIED (chipid@10000000)
#define EXYNOS_CHIPID_SIZE        0x0000000000000100ULL   // VERIFIED

//
// ---------------------------------------------------------------------------
// SPEEDY  [MOVES, and there are two]
//
// VERIFIED node names: speedy@15970000 and speedy1@15980000. The 9810 had one
// at 0x141C0000. Sizes not read yet.
// ---------------------------------------------------------------------------
//
#define EXYNOS_SPEEDY_BASE        0x0000000015970000ULL   // VERIFIED (speedy@15970000)
#define EXYNOS_SPEEDY_SIZE        0x0000000000000000ULL   // UNVERIFIED — extent not read
#define EXYNOS_SPEEDY1_BASE       0x0000000015980000ULL   // VERIFIED (speedy1@15980000)
#define EXYNOS_SPEEDY1_SIZE       0x0000000000000000ULL   // UNVERIFIED — extent not read

//
// ---------------------------------------------------------------------------
// Clock management (CMU)  [TOP same base, the banks are a different set]
//
// VERIFIED from the vendor DT:
//   cmu      reg = <0x0 0x1a200000 0x8000  0x0 0x1bc00000 0x8000
//                    0x0 0x1bd00000 0x8000  0x0 0x1be00000 0x8000
//                    0x0 0x1bf00000 0x8000>
//   cmu_ewf  reg = <0x0 0x1a240000 0x1000>
//
// So 0x1A240000 is still inside the 0x1A200000 bank, and the 9810's CMU_FSYS0
// at 0x11000000 does not exist on this SoC at all. Which of the four 0x1BC…
// 0x1BF… banks is FSYS0A (the one that clocks USB31DRD) and which is PERIC0
// (the one that clocks USI03, i.e. the touch bus) is NOT resolved: the 9820
// clock driver names them through Samsung's cal-if headers
// (soc/samsung/cal-if/exynos9820/cmucal-{vclk,node,qch}.h), where the QCH ids
// are enum positions, not addresses. Until that mapping is derived, the two
// windows the USB and touch code poke stay zero so the memory map lib skips
// them rather than mapping a wrong one.
// ---------------------------------------------------------------------------
//
#define EXYNOS_CMU_TOP_BASE       0x000000001A240000ULL   // VERIFIED (cmu_ewf)
#define EXYNOS_CMU_TOP_SIZE       0x0000000000001000ULL   // VERIFIED (vendor DT)
#define EXYNOS_CMU_BANK0_BASE     0x000000001A200000ULL   // VERIFIED (cmu reg #1)
#define EXYNOS_CMU_BANK0_SIZE     0x0000000000008000ULL   // VERIFIED
#define EXYNOS_CMU_BANK1_BASE     0x000000001BC00000ULL   // VERIFIED (cmu reg #2)
#define EXYNOS_CMU_BANK1_SIZE     0x0000000000008000ULL   // VERIFIED
#define EXYNOS_CMU_BANK2_BASE     0x000000001BD00000ULL   // VERIFIED (cmu reg #3)
#define EXYNOS_CMU_BANK2_SIZE     0x0000000000008000ULL   // VERIFIED
#define EXYNOS_CMU_BANK3_BASE     0x000000001BE00000ULL   // VERIFIED (cmu reg #4)
#define EXYNOS_CMU_BANK3_SIZE     0x0000000000008000ULL   // VERIFIED
#define EXYNOS_CMU_BANK4_BASE     0x000000001BF00000ULL   // VERIFIED (cmu reg #5)
#define EXYNOS_CMU_BANK4_SIZE     0x0000000000008000ULL   // VERIFIED

#define EXYNOS_CMU_FSYS0A_BASE    0x0000000000000000ULL   // UNVERIFIED — which bank is FSYS0A
#define EXYNOS_CMU_FSYS0A_SIZE    0x0000000000000000ULL   // UNVERIFIED
#define EXYNOS_CMU_PERIC0_BASE    0x0000000000000000ULL   // UNVERIFIED — which bank is PERIC0
#define EXYNOS_CMU_PERIC0_SIZE    0x0000000000000000ULL   // UNVERIFIED

//
// ---------------------------------------------------------------------------
// UFS storage (JEDEC UFSHCI host + Samsung UFS PHY)  [ALL VERIFIED — MOVES]
//
// VERIFIED twice: the vendor DT's ufs@13D60000 reg list, and the device's own
// fstab, which names /dev/block/platform/13d60000.ufs/by-name/keydata. The
// live phone shows the platform node 13d60000.ufs with a host0 child.
//
// The 9810's whole 0x111… region is gone. The intra-window vendor-specific
// offset is the SAME (HCI + 0x1100 = 0x13D61100), which is why
// drivers/Exynos9820Ufs/ keeps UFS_VENDOR_OFFSET unchanged.
//
// The UNIPRO window must keep its FULL 0x8000: the same debug-APB flow-control
// writes that cost a device cycle on the 9810 land at +0x7888, i.e. page 7.
// ---------------------------------------------------------------------------
//
#define EXYNOS_UFS_HCI_BASE       0x0000000013D60000ULL   // VERIFIED (ufs@13D60000)
#define EXYNOS_UFS_HCI_SIZE       0x0000000000002000ULL   // VERIFIED
#define EXYNOS_UFS_VS_BASE        0x0000000013D61100ULL   // VERIFIED (vendor-specific HCI)
#define EXYNOS_UFS_UNIPRO_BASE    0x0000000013D50000ULL   // VERIFIED (0x8000 — map it all)
#define EXYNOS_UFS_UNIPRO_SIZE    0x0000000000008000ULL   // VERIFIED
#define EXYNOS_UFS_PMA_BASE       0x0000000013D64000ULL   // VERIFIED (ufs-phy, PMA/analog)
#define EXYNOS_UFS_PMA_SIZE       0x0000000000000800ULL   // VERIFIED (vendor DT reg)
#define EXYNOS_UFS_UFSP_BASE      0x0000000013D70000ULL   // VERIFIED (FMP/UFSP protector)
#define EXYNOS_UFS_UFSP_SIZE      0x0000000000001000ULL   // VERIFIED
#define EXYNOS_UFS_CPORT_BASE     0x0000000013D68000ULL   // VERIFIED (cport)
#define EXYNOS_UFS_IOCOH_BASE     0x0000000013C10700ULL   // VERIFIED (io-coherency)

//
// ---------------------------------------------------------------------------
// USB 3.0 DRD (DWC3 — exposes a standard xHCI register interface) + USB PHY
//
// VERIFIED on-device and in the vendor DT:
//   usb@10C00000   compatible "samsung,exynos-dwusb", reg <0x0 0x10c00000 0x10000>
//                  child dwc3, compatible "synopsys,dwc3", same reg
//   phy@10B00000   compatible "samsung,exynos-usbdrd-phy"
//                  reg = <0x0 0x10b00000 0x200   0x0 0x10ae0000 0x2800
//                         0x0 0x10af0000 0x800>
//
// Three facts that change the 9810's model:
//   1. The DWC3 core window is at the SAME address and size as on the 9810, so
//      the xHCI MMIO Windows' inbox USBXHCI.sys binds to does not move.
//   2. The PHY is ONE node with THREE register windows, not three separate
//      nodes: main 0x10B00000+0x200, combo 0x10AE0000+0x2800, PCS 0x10AF0000+0x800.
//      The node declares has_combo_phy = 1 and has_other_phy = 0.
//   3. Both usb@10C00000 and phy@10B00000 carry status = "disabled" in the
//      shipping build, and the board-level patch sets dr_mode = "otg" on the
//      dwc3 child. The device tree itself therefore proves the port is on the
//      device side under Android — a dock needs "host".
//
// phy_version = 0x301 and sub_phy_version = 0x400: the PHY generation differs
// from the 9810's, so the 9810's PHY init sequence cannot be pasted here.
// ---------------------------------------------------------------------------
//
#define EXYNOS_USBDRD_BASE        0x0000000010C00000ULL   // VERIFIED (usb@10C00000)
#define EXYNOS_USBDRD_SIZE        0x0000000000010000ULL   // VERIFIED
#define EXYNOS_USBDRD_PHY_BASE    0x0000000010B00000ULL   // VERIFIED (phy@10B00000 reg #1)
#define EXYNOS_USBDRD_PHY_SIZE    0x0000000000000200ULL   // VERIFIED
#define EXYNOS_USB_COMBO_PHY_BASE 0x0000000010AE0000ULL   // VERIFIED (phy reg #2)
#define EXYNOS_USB_COMBO_PHY_SIZE 0x0000000000002800ULL   // VERIFIED
#define EXYNOS_USB_PCS_BASE       0x0000000010AF0000ULL   // VERIFIED (phy reg #3)
#define EXYNOS_USB_PCS_SIZE       0x0000000000000800ULL   // VERIFIED

//
// ---------------------------------------------------------------------------
// Touch: the HSI2C bus the panel IC hangs off, and the panel itself.
//
// VERIFIED on the live phone: touchscreen@48 is a CHILD of hsi2c@104B0000 —
// the same bus window and the same slave address as on the S9+. Enumerating
// every hsi2c@… node on the phone shows the touch IC under this one and no
// other, so the touch path's hardware identity does not move at all.
//
// What DOES move is the interrupt number: the 9820's hsi2c@104B0000 declares
// interrupts = <0x0 0x184 0x0>, i.e. SPI 388, where the 9810's was SPI 404.
// With the repo's convention (GSIV = SPI + 32) that is GSIV 420, not 436.
// The bus clock is declared as default-clk = <0xbebc200> = 200 MHz, which
// matches the 9810 driver's EXYNOS_HSI2C_SOURCE_CLOCK_HZ.
// ---------------------------------------------------------------------------
//
#define EXYNOS_TOUCH_HSI2C_BASE   0x00000000104B0000ULL   // VERIFIED (hsi2c@104B0000)
#define EXYNOS_TOUCH_HSI2C_SIZE   0x0000000000001000ULL   // VERIFIED
#define EXYNOS_TOUCH_I2C_ADDR     0x48                    // VERIFIED (touchscreen@48)
#define EXYNOS_TOUCH_HSI2C_GSIV   420                     // VERIFIED (SPI 388 + 32)
#define EXYNOS_HSI2C_SOURCE_CLOCK_HZ 200000000u           // VERIFIED (default-clk)

//
// The panel blob on THIS model is tsp_sec/y761_beyond0_phole.bin — a y761,
// the same family the Windows driver is already named after. The 9820 kernel's
// source directory is sec_ts/y771 and its default blob is y771_beyond2.bin;
// the S10 (beyond1lte) used y771_beyond1_phole.bin. So "y771" names the kernel
// driver family, not this phone's panel IC.
//
// The node also declares, per model: sec,project_name = BEYOND0G970,
// sec,afe_base = 0x0205 (the S10's was 0x0206), sec,tclm_level = 2,
// sec,max_coords = 4096 x 4096, sync-reportrate-120, support_dex_mode and
// support_ear_detect_mode. There is NO support_fod on this node, so the
// fingerprint-on-glass complication the 9820 .dtsi family carries does not
// apply to the donation device.
//
// The GPIO pair the bus uses is unchanged: gpio_scl/gpio_sda point at
// phandle 0x74 (gpp1) pins 4 and 5, exactly as on the 9810.
//
#define EXYNOS_TOUCH_GPIO_SCL_PIN 4                       // VERIFIED (gpp1 pin 4)
#define EXYNOS_TOUCH_GPIO_SDA_PIN 5                       // VERIFIED (gpp1 pin 5)

//
// ---------------------------------------------------------------------------
// GPIO  [PERIC0 unchanged, ALIVE moves]
//
// VERIFIED: pinctrl@10430000, compatible "samsung,exynos9820-pinctrl", reg
// <0x0 0x10430000 0x1000>, interrupts SPI 0x17c = 380 (GSIV 412).
// The ALIVE GPIO is not a node of its own: exynos-pm names it via reg-names,
// giving 0x15850000 where the 9810 had 0x14050000.
// ---------------------------------------------------------------------------
//
#define EXYNOS_PERIC0_GPIO_BASE   0x0000000010430000ULL   // VERIFIED (pinctrl@10430000)
#define EXYNOS_PERIC0_GPIO_SIZE   0x0000000000001000ULL   // VERIFIED
#define EXYNOS_ALIVE_GPIO_BASE    0x0000000015850000ULL   // VERIFIED (exynos-pm gpio_alive_base)
#define EXYNOS_ALIVE_GPIO_SIZE    0x0000000000001000ULL   // VERIFIED

//
// ---------------------------------------------------------------------------
// Display: DECON (display controller) + DSIM (MIPI DSI master)  [MOVES, and splits]
//
// VERIFIED node names: decon_f@0x19030000 and decon_t@0x19050000 (the 9810 had
// one DECON at 0x16030000), dsim at 0x19080000. decon_f declares reg size 0x10000
// and interrupts SPI 0xb7, 0xb8, 0xbd, 0x197 (GSIVs 215, 216, 221, 439).
//
// As on the 9810, these are NOT initialised for bring-up: the firmware reuses
// whatever sboot already lit. They exist for a future native display driver,
// and for the external-monitor path (DP Alt Mode out of the phone's own
// connector — see notes/dock-desktop.md §11).
// ---------------------------------------------------------------------------
//
#define EXYNOS_DECON_F_BASE       0x0000000019030000ULL   // VERIFIED (decon_f@0x19030000)
#define EXYNOS_DECON_F_SIZE       0x0000000000010000ULL   // VERIFIED
#define EXYNOS_DECON_T_BASE       0x0000000019050000ULL   // VERIFIED (decon_t@0x19050000)
#define EXYNOS_DECON_T_SIZE       0x0000000000010000ULL   // UNVERIFIED (extent not read)
#define EXYNOS_DSIM_BASE          0x0000000019080000ULL   // VERIFIED (node name)
#define EXYNOS_DSIM_SIZE          0x0000000000010000ULL   // UNVERIFIED (extent not read)

//
// ---------------------------------------------------------------------------
// Watchdog (Samsung s3c2410-style)  [SAME base as the 9810]
//
// VERIFIED: watchdog_cl0@10050000 and watchdog_cl2@10060000, at the same
// addresses the 9810 uses. This is what lets the storage driver's hardware-
// watchdog servicer and the firmware's EBS arm transfer as-is.
// ---------------------------------------------------------------------------
//
#define EXYNOS_WDT_BASE           0x0000000010050000ULL   // VERIFIED (watchdog_cl0@10050000)
#define EXYNOS_WDT_SIZE           0x0000000000001000ULL   // UNVERIFIED (extent not read)

//
// ---------------------------------------------------------------------------
// Framebuffer  [GEOMETRY MOVES, BASE NOT READ]
//
// The S10e's panel is 1440 x 3040, not the S9+'s 1440 x 2960. The 9810's fb
// base 0xCC000000 has NOT been confirmed on this SoC: the vendor DT's chosen
// node carried no fb address and the live phone cannot read the property.
// The base is therefore left at zero so the memory map lib skips it rather
// than mapping a wrong region — a wrong fb base is a blank or faulting screen,
// not a recoverable mistake.
//
// The first on-device read that settles it is the sboot-lit carveout, the same
// measurement the 9810 flow already has a tool for.
// ---------------------------------------------------------------------------
//
#define EXYNOS_FB_BASE            0x0000000000000000ULL   // UNVERIFIED — must be read on device
#define EXYNOS_FB_WIDTH           1440                    // panel geometry (S10e)
#define EXYNOS_FB_HEIGHT          3040                    // S10e is taller than the S9+
#define EXYNOS_FB_BYTES_PER_PIXEL 4
#define EXYNOS_FB_STRIDE_PIXELS   EXYNOS_FB_WIDTH         // width*4, as on the 9810
#define EXYNOS_FB_SIZE            ((UINT64)EXYNOS_FB_STRIDE_PIXELS * \
                                   EXYNOS_FB_HEIGHT * EXYNOS_FB_BYTES_PER_PIXEL)

//
// ---------------------------------------------------------------------------
// IRQ numbers the ACPI tables will need (SPI + 32 = GSIV, the repo's rule)
//
//   DWC3 core        (usb@10C00000/dwc3)   SPI 0xf1 = 241   GSIV 273
//   USB PHY (two)    (phy@10B00000)        SPI 0xf5, 0xf6   GSIV 277, 278
//   HSI2C #10        (hsi2c@104B0000)      SPI 0x184 = 388  GSIV 420
//   pinctrl PERIC0   (pinctrl@10430000)    SPI 0x17c = 380  GSIV 412
//   decon_f          (decon_f@0x19030000)  SPI 0xb7,0xb8,0xbd,0x197
//                                          GSIV 215, 216, 221, 439
// ---------------------------------------------------------------------------
//

#endif // EXYNOS9820_H_
