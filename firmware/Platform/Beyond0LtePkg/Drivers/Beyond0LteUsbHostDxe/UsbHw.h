// UsbHw.h — register vocabulary for the Exynos 9820 USB host path.
//
// Everything in this header is a transcription from a named primary source;
// nothing is guessed. Sources (see notes/d1-usb-host.md and
// notes/d1b-usb-vbus-hcd.md for the research trail):
//
//   [K] LineageOS/android_kernel_samsung_exynos9820 (branch lineage-23.2),
//       the downstream Samsung kernel for this SoC:
//       drivers/usb/dwc3/core.h, drivers/phy/samsung/phy-exynos-usb3p1-reg.h,
//       drivers/phy/samsung/phy-exynos-usbdrd.{c,h},
//       drivers/i2c/busses/i2c-exynos5.c, include/linux/mfd/max77705-private.h,
//       drivers/battery_v2/include/charger/max77705_charger.h,
//       include/linux/ccic/max77705.h
//   [M] Mu-Silicium (Project-Silicium/Mu-Silicium, BSD-2-Clause-Patent),
//       Silicon/Samsung/S5E9830Pkg/Library/PlatformUsbfnLib/PlatformUsbRegisters.h
//       — same PHY family (9830), used only to CROSS-CHECK bit positions; the
//       9830 block BASES differ and are never used here (trap recorded in
//       notes/d1-usb-host.md Q3).
//
// Block bases come exclusively from <Platform/Exynos9820.h> (repo rule 6).

#ifndef USB_HW_H_
#define USB_HW_H_

#include <Platform/Exynos9820.h>

//
// ---------------------------------------------------------------------------
// 1. PMU isolation registers (system-controller, EXYNOS_PMU_BASE = 15860000).
//    The USB DRD blocks come out of reset ISOLATED; the PHY driver's
//    phy_isol(inst, 0, mask) clears the isolation by writing bit0 = 1.
//    Offsets are the DT properties pmu_offset = <0x72c> (UTMI/USB2) and
//    pmu_offset_dp = <0x704> (PIPE3/USB3); pmu_mask = <0> becomes BIT(0) in
//    the driver. [K phy-exynos-usbdrd.c, phy-exynos-usbdrd.h:20-25; Mu's
//    9830 BoardInit.c uses the same offset and bit]
// ---------------------------------------------------------------------------
#define PMU_USBDRD_ISO            0x72C   // [K] pmu_offset  — UTMI (USB2)
#define PMU_USBDP_ISO             0x704   // [K] pmu_offset_dp — PIPE3 (USB3/DP combo)
#define   PMU_USB_ISO_EN          BIT0    // [K] EXYNOS_USBDRD_ENABLE = BIT(0): 1 = de-isolated

//
// ---------------------------------------------------------------------------
// 2. USBCON — the PHY control block at EXYNOS_USBDRD_PHY_BASE (10B00000,
//    0x200). Register map [K phy-exynos-usb3p1-reg.h], cross-checked [M].
// ---------------------------------------------------------------------------
#define USBCON_CTRL_VER           0x00    // read 0 or ~0 => clocks off / isolated [M trick]
#define USBCON_LINK_CTRL          0x04
#define   LINKCTRL_PIPE3_FORCE_RX_ELEC_IDLE  BIT18   // [K]
#define   LINKCTRL_PIPE3_FORCE_PHY_STATUS   BIT17   // [K]
#define   LINKCTRL_PIPE3_FORCE_EN           BIT16   // [K]
#define   LINKCTRL_DIS_QACT_BUSPEND         BIT13   // [K]
#define   LINKCTRL_DIS_QACT_LINKGATE        BIT12   // [K]
#define   LINKCTRL_DIS_QACT_ID0             BIT11   // [K]
#define   LINKCTRL_DIS_QACT_VBUS_VALID      BIT10   // [K]
#define   LINKCTRL_DIS_QACT_BVALID          BIT9    // [K]
#define   LINKCTRL_FORCE_QACT               BIT8    // [K]
#define   LINKCTRL_BUS_FILTER_BYPASS(x)     (((x) & 0xF) << 4)  // [K]
#define   LINKCTRL_BUS_FILTER_BYPASS_MASK   (0xF << 4)          // [K]
#define USBCON_LINK_PORT          0x08
#define   LINKPORT_HUB_PORT_SEL_OCD_U3      BIT3    // [M]
#define   LINKPORT_HUB_PORT_SEL_OCD_U2      BIT2    // [M]
#define USBCON_CLKRST             0x20
#define   CLKRST_LINK_PCLK_SEL              BIT7    // [K]
#define   CLKRST_PHYCLOCKSEL                BIT6    // [K]
#define   CLKRST_REFCLK_SEL                 BIT4    // [K]
#define   CLKRST_PHY30_SW_RST               BIT3    // [K] active-low style: 1 = in reset
#define   CLKRST_PHY30_RST_SEL              BIT2    // [K]
#define   CLKRST_PHY20_SW_RST               BIT13   // [K]
#define   CLKRST_PHY20_RST_SEL              BIT12   // [K]
#define   CLKRST_PORT_RST                   BIT1    // [K]
#define   CLKRST_LINK_SW_RST                BIT0    // [K]
#define USBCON_PWR                0x24
#define USBCON_DUALPHYSEL         0x28
#define USBCON_COMBO_PMA_CTRL     0x48    // SS PHY PMA (combo block reaches it via USBCON too)
#define   PMA_ROPLL_REF_REQ(x)              (((x) & 0x3) << 12) // [K]
#define   PMA_PLL_REF_REQ(x)                (((x) & 0x3) << 10) // [K]
#define   PMA_REF_FREQ_SET(x)               (((x) & 0x3) << 8)  // [K]
#define   PMA_LOW_PWRN                      BIT4    // [K] 1 = powered down
#define   PMA_TRSV_SW_RST                   BIT3    // [K]
#define   PMA_CMN_SW_RST                    BIT2    // [K]
#define   PMA_INIT_SW_RST                   BIT1    // [K]
#define   PMA_APB_SW_RST                    BIT0    // [K]
#define USBCON_UTMI               0x50
#define   UTMI_OPMODE_CTRL_EN               BIT8    // [K]
#define   UTMI_FORCE_VBUSVALID              BIT5    // [K] device-mode only — do NOT set in host
#define   UTMI_FORCE_BVALID                 BIT4    // [K] device-mode only — do NOT set in host
#define   UTMI_DP_PULLDOWN                  BIT3    // [K]
#define   UTMI_DM_PULLDOWN                  BIT2    // [K]
#define   UTMI_FORCE_SUSPEND                BIT1    // [K]
#define   UTMI_FORCE_SLEEP                  BIT0    // [K]
#define USBCON_HSP                0x54
#define   HSP_VBUSVLDEXTSEL                 BIT13   // [K] device-mode VBUS faking — not used in host
#define   HSP_VBUSVLDEXT                    BIT12   // [K]
#define   HSP_EN_UTMISUSPEND                BIT9    // [K]
#define   HSP_COMMONONN                     BIT8    // [K] set: DT common_block_disable = 1
#define USBCON_HSP_TUNE           0x58
#define   HSP_TUNE_TXVREF_MASK              (0xF << 28)         // [K]
#define   HSP_TUNE_TXVREF(x)                (((x) & 0xF) << 28) // [K]
#define   HSP_TUNE_TXRISE(x)                (((x) & 0x3) << 24) // [K]
#define   HSP_TUNE_TXRES_MASK               (0x3 << 21)         // [K]
#define   HSP_TUNE_TXRES(x)                 (((x) & 0x3) << 21) // [K]
#define   HSP_TUNE_TXPREEMPA_PLUS           BIT20   // [K]
#define   HSP_TUNE_TXPREEMPA_MASK           (0x3 << 18)         // [K]
#define   HSP_TUNE_TXPREEMPA(x)             (((x) & 0x3) << 18) // [K]
#define   HSP_TUNE_HSXV(x)                  (((x) & 0x3) << 16) // [K]
#define   HSP_TUNE_TXFSLS(x)                (((x) & 0xF) << 12) // [K]
#define   HSP_TUNE_SQRX_MASK                (0x7 << 8)          // [K]
#define   HSP_TUNE_SQRX(x)                  (((x) & 0x7) << 8)  // [K]
#define   HSP_TUNE_OTG_MASK                 (0x7 << 4)          // [K]
#define   HSP_TUNE_OTG(x)                   (((x) & 0x7) << 4)  // [K]
#define   HSP_TUNE_COMPDIS_MASK             0x7                 // [K]
#define   HSP_TUNE_COMPDIS(x)               ((x) & 0x7)         // [K]
#define USBCON_HSP_TEST           0x5C
#define   HSP_TEST_SIDDQ                    BIT24   // [M] 1 = power down HS PHY
#define   HSP_TEST_LINESTATE_MASK           (0x3 << 20)         // [M] J/N state, our only VBUS proxy

// Host-side HS PHY tuning: the HOST column of the board DT's hs_tune_param
// (exynos9820-beyond0lte_common.dtsi:33-65, notes/d1-usb-host.md Q1). The
// kernel applies exactly these five fields (phy-exynos-usb3p1.c:980-1012);
// the DT's sixth entry "utmi_clk" matches no branch in that parser and is
// IGNORED — do not invent a field for it. Fields not listed here (tx_rise,
// tx_fsls, tx_hsxv, otg) are left at their current register value: the
// driver must RMW, not write this constant raw.
#define USBCON_HSP_TUNE_HOST_SET \
  (HSP_TUNE_TXPREEMPA (0x3) | HSP_TUNE_TXVREF (0xC) | HSP_TUNE_SQRX (0x5) | \
   HSP_TUNE_COMPDIS (0x7)   | HSP_TUNE_TXRES (0x1))
#define USBCON_HSP_TUNE_HOST_MASK \
  (HSP_TUNE_TXPREEMPA_MASK | HSP_TUNE_TXVREF_MASK | HSP_TUNE_SQRX_MASK | \
   HSP_TUNE_COMPDIS_MASK | HSP_TUNE_TXRES_MASK)

//
// ---------------------------------------------------------------------------
// 3. DWC3 globals — EXYNOS_USBDRD_BASE + 0xC100..0xC6FF. Offsets and bits
//    [K drivers/usb/dwc3/core.h]. The xHCI register window is the SAME block
//    at offset 0x0000..0x7FFF (dwc3/host.c: xhci_resources = base+0 .. +0x7fff).
// ---------------------------------------------------------------------------
#define DWC3_GSBUSCFG0            0xC100
#define   GSBUSCFG0_INCRBRSTEN              BIT0    // [K]
#define   GSBUSCFG0_INCR16BRSTEN            BIT3    // [K]
#define DWC3_GSBUSCFG1            0xC104
#define   GSBUSCFG1_BREQLIMIT(n)            ((n) << 8)          // [K]
#define DWC3_GCTL                 0xC110
#define   GCTL_DSBLCLKGTNG                  BIT0    // [K core.h:251]
#define   GCTL_PRTCAPDIR(n)                 ((n) << 12)         // [K]
#define   GCTL_PRTCAP_HOST                  1       // [K]
#define   GCTL_PRTCAP_DEVICE                2       // [K]
#define   GCTL_PRTCAP_OTG                   3       // [K]
#define   GCTL_CORESOFTRESET                BIT11   // [K]
#define   GCTL_SOFITPSYNC                   BIT10   // [K]
#define   GCTL_U2RSTECN                     BIT16   // [K] rev < 1.90a only
#define   GCTL_DISSCRAMBLE                  BIT3    // [K core.h:249] FPGA quirk — always off for us
#define   GCTL_GBLHIBERNATIONEN             BIT28   // [K core.h] hibernation mode
#define DWC3_GSNPSID              0xC120  // [K] 0x5533xxxx = DWC_usb3, low 16 = rev
#define   GSNPSID_ID_MASK                   0xFFFF0000          // [K]
#define   GSNPSID_ID_DWC3                   0x55330000          // [K] "U3"
#define   GSNPSID_ID_DWC31                  0x33310000          // [K] "13" (DWC_usb31)
// Revision encodings [K core.h:1049-1056] — the kernel compares the FULL
// GSNPSID value against these; the 9820's revision is UNVERIFIED until read
// on device, so every quirk branch in the driver is gated on the read value.
#define   DWC3_REV_190A                     0x5533190a
#define   DWC3_REV_194A                     0x5533194a
#define   DWC3_REV_210A                     0x5533210a
#define   DWC3_REV_220A                     0x5533220a
#define   DWC3_REV_250A                     0x5533250a
#define   DWC3_REV_310A                     0x5533310a
#define DWC3_GUID                 0xC128  // [K] write LINUX_VERSION_CODE (cosmetic)
#define DWC3_GUCTL                0xC12C
#define   GUCTL_REFCLKPER(n)                ((n) << 22)         // [K]
#define   GUCTL_REFCLKPER_MASK              (0x3FF << 22)       // [K core.h:254]
#define   GUCTL_USBHSTINAUTORETRYEN         BIT14   // [K] Samsung downstream bit
#define   GUCTL_SPRSCTRLTRANSEN             BIT17   // [K] Samsung downstream (DT enable_sprs_transfer)
#define DWC3_GHWPARAMS0           0xC140  // [K]
#define   GHWPARAMS0_MODE(n)                ((n) & 0x3)         // [K core.h:316]
#define   GHWPARAMS0_MODE_DRD               2       // [K] ours: dual-role (dr_mode otg in DT)
#define DWC3_GHWPARAMS1           0xC144  // [K]
#define   GHWPARAMS1_EN_PWROPT(n)           (((n) & (3 << 24)) >> 24)  // [K core.h:327]
#define   GHWPARAMS1_EN_PWROPT_CLK          1       // [K]
#define   GHWPARAMS1_EN_PWROPT_HIB          2       // [K]
#define DWC3_GHWPARAMS3           0xC14C  // [K] bits 27:26 SSPHY_IFC: 0 = disabled
#define   GHWPARAMS3_SSPHY_IFC_DIS          0       // [K]
#define DWC3_GRXTHRCFG            0xC10C  // [K core.h:91]
#define   GRXTHRCFG_RXPKTCNTSEL             BIT29   // [K core.h:205]
#define   GRXTHRCFG_USBRXPKTCNT(n)          ((n) << 24)         // [K core.h:208]
#define   GRXTHRCFG_USBMAXRXBURSTSIZE(n)    ((n) << 19)         // [K core.h:211]
#define DWC3_GUSB2PHYCFG(n)       (0xC200 + ((n) * 4))  // [K]
#define   GUSB2PHYCFG_PHYSOFTRST            BIT31   // [K]
#define   GUSB2PHYCFG_U2_FREECLK_EXISTS     BIT30   // [K]
#define   GUSB2PHYCFG_SUSPHY                BIT6    // [K]
#define   GUSB2PHYCFG_ULPI_UTMI             BIT4    // [K]
#define   GUSB2PHYCFG_ENBLSLPM              BIT8    // [K]
#define   GUSB2PHYCFG_PHYIF(n)              ((n) << 3)          // [K] 0=8-bit, 1=16-bit
#define   GUSB2PHYCFG_USBTRDTIM(n)          ((n) << 10)         // [K]
#define DWC3_GUSB3PIPECTL(n)      (0xC2C0 + ((n) * 4))  // [K]
#define   GUSB3PIPECTL_PHYSOFTRST           BIT31   // [K]
#define   GUSB3PIPECTL_DISRXDETINP3         BIT28   // [K] rev >= 2.50a
#define   GUSB3PIPECTL_UX_EXIT_PX           BIT27   // [K] DT snps,ux_exit_in_px_quirk
#define   GUSB3PIPECTL_U1U2EXITFAIL_TO_RECOV BIT25  // [K] DT snps,u1u2_exitfail_quirk
#define   GUSB3PIPECTL_DEP1P2P3(n)          ((n) << 19)         // [K]
#define   GUSB3PIPECTL_DEP1P2P3_EN          (1 << 19)           // [K] rev <= 2.10a WA
#define   GUSB3PIPECTL_SUSPHY               BIT17   // [K]
#define   GUSB3PIPECTL_ELASTIC_BUFFER_MODE  BIT0    // [K core.h:303] DT snps,elastic_buf_mode_quirk (Samsung downstream bit)
#define DWC3_GEVNTADRLO(n)        (0xC400 + ((n) * 0x10))  // [K]
#define DWC3_GEVNTADRHI(n)        (0xC404 + ((n) * 0x10))  // [K]
#define DWC3_GEVNTSIZ(n)          (0xC408 + ((n) * 0x10))  // [K]
#define DWC3_GEVNTCOUNT(n)        (0xC40C + ((n) * 0x10))  // [K]
#define   GEVNTSIZ_SIZE(n)                  ((n) & 0xFFFF)      // [K core.h:313]
#define   GEVNTSIZ_INTMASK                  BIT31   // [K core.h:312]
#define   GEVNTCOUNT_MASK                   0xFFFC  // [K core.h:72]
#define DWC3_EVENT_BUFFERS_SIZE   4096    // [K core.h:53]
#define DWC3_GFLADJ               0xC630  // [K]
#define   GFLADJ_REFCLK_240MHZ_DECR(n)      ((n) << 24)         // [K]
#define   GFLADJ_REFCLK_LPM_SEL             BIT23   // [K]
#define   GFLADJ_REFCLK_FLADJ(n)            ((n) << 8)          // [K]
#define   GFLADJ_30MHZ_SDBND_SEL            BIT7    // [K]
#define   GFLADJ_30MHZ_MASK                 0x3F    // [K]

// Frame-length adjustment value from the board DT:
// snps,quirk-frame-length-adjustment = <0x20> (exynos9820-beyond0lte_common.dtsi).
#define DWC3_FLADJ_VALUE          0x20

//
// ---------------------------------------------------------------------------
// 4. xHCI registers inside the SAME window (EXYNOS_USBDRD_BASE + 0x0000).
//    Offsets [K drivers/usb/dwc3/host.c + Xilinx linux-xlnx dwc3/host.c].
//    We touch these only for the root-hub power-off trick; XhciDxe owns the
//    rest.
// ---------------------------------------------------------------------------
#define XHCI_HCSPARAMS1           0x0004  // MAXPORTS bits 31:24 — expect 2 (1xHS + 1xSS DRD)
#define   HCSPARAMS1_MAXPORTS(x)            (((x) >> 24) & 0xFF)
// PORTSC(n) = CAPLENGTH + 0x400 + 0x10*n; CAPLENGTH = byte read at 0x0000.
#define XHCI_PORTSC_BASE          0x0400  // + capability-register CAPLENGTH
#define   PORTSC_POWER                      BIT8    // xHCI 1.2 §4.3.14: 1 = port power on
#define   PORTSC_CONNECT_STATUS             BIT0    // [xHCI spec]

//
// ---------------------------------------------------------------------------
// 5. HSI2C #45 (EXYNOS_PMIC_HSI2C_BASE = 15D60000) — polled master, write
//    only. Map and bits [K drivers/i2c/busses/i2c-exynos5.c, usi-i2c-v2
//    path]; identical block to our validated Windows driver
//    (drivers/Exynos9810Hsi2c/exynos9810hsi2c.h).
// ---------------------------------------------------------------------------
#define HSI2C_CTL                 0x00
#define   HSI2C_CTL_MASTER                  BIT3    // [K]
#define   HSI2C_CTL_RXCHON                  BIT6    // [K]
#define   HSI2C_CTL_TXCHON                  BIT7    // [K]
#define   HSI2C_CTL_SW_RST                  BIT31   // [K]
#define HSI2C_FIFO_CTL            0x04    // [K] enables + trigger levels
#define   HSI2C_FIFO_CTL_RX_EN              BIT0    // [K] HSI2C_RXFIFO_EN
#define   HSI2C_FIFO_CTL_TX_EN              BIT1    // [K] HSI2C_TXFIFO_EN
#define   HSI2C_FIFO_CTL_RX_TRIG(x)         ((x) << 4)   // [K]
#define   HSI2C_FIFO_CTL_TX_TRIG(x)         ((x) << 16)  // [K]
#define HSI2C_TRAIL_CTL           0x08    // [K] written 0xFFFFFF by the kernel
#define HSI2C_INT_EN              0x20    // [K]
#define HSI2C_INT_STAT            0x24    // [K]
#define   HSI2C_INT_TRANSFER_DONE           BIT7    // [K]
#define   HSI2C_INT_NODEV                   BIT10   // [K]
#define HSI2C_FIFO_STAT           0x30    // [K]
#define   HSI2C_FIFO_STAT_TX_EMPTY          BIT8    // [K]
#define   HSI2C_FIFO_STAT_TX_LEVEL(x)       ((x) & 0x7F)        // [K]
#define   HSI2C_FIFO_STAT_RX_LEVEL(x)       (((x) >> 16) & 0x7F) // [K]
#define HSI2C_TX_DATA             0x34    // [K]
#define HSI2C_RX_DATA             0x38    // [K]
#define HSI2C_CONF                0x40
#define   HSI2C_CONF_AUTO_MODE              BIT31   // [K]
#define HSI2C_AUTO_CONF           0x44
#define   HSI2C_AUTO_CONF_READ_WRITE        BIT16   // [K]
#define   HSI2C_AUTO_CONF_STOP_AFTER_TRANS  BIT17   // [K]
#define   HSI2C_AUTO_CONF_MASTER_RUN        BIT31   // [K]
#define HSI2C_TIMEOUT             0x48    // [K] bit31 = watchdog enable; kernel clears it in polling
#define HSI2C_TRANS_STATUS        0x50    // [K]
#define   HSI2C_MASTER_BUSY                 BIT17   // [K] TRANS_STATUS bit17
#define HSI2C_TIMING_FS1          0x60    // [K]
#define HSI2C_TIMING_FS2          0x64    // [K]
#define HSI2C_TIMING_FS3          0x68    // [K]
#define HSI2C_ADDR                0x70    // [K] master mode: target addr bits 19:10 ((addr&0x7f)<<10), master-id bits 31:24 = 0x7 in FS mode (i2c-exynos5.c:755-763)
#define HSI2C_USI_CON             0xC4    // [K] the kernel writes 0 here (USI_RESET = (0<<0): bit0 is the USI block reset, active high, must be released)
#define   HSI2C_USI_RESET                   BIT0    // [K]
#define HSI2C_USI_OPTION          0xC8    // [K] mode select — kernel never writes it; bus is already I2C at handoff

// FS (400 kHz) divider math at the DT default-clk of 200 MHz, from
// exynos5_i2c_set_timing (FAST branch, i2c-exynos5.c:484-503):
//   fs_div = 200000000/(400000*15) = 33 -> TIMING_FS3[23:16] = 0x21
//   TSCL_H = (9*200)/((33+1)*10) = 5, then the low 5 bits are ZEROED in an
//   8-bit field -> FS2[7:0] = 0xE0
//   TSTART_HD = 5-1 = 4, low 4 bits zeroed -> FS1[23:16] = 0xF0
// UNVERIFIED on our own silicon: these are the kernel's computed values for
// this ipclk; the block is live at handoff, so the driver reads them back and
// only writes if they differ.
#define HSI2C_TIMING_FS1_BOOT     0x00F00000
#define HSI2C_TIMING_FS2_BOOT     0x000000E0
#define HSI2C_TIMING_FS3_BOOT     0x00210000

//
// ---------------------------------------------------------------------------
// 6. MAX77705 interface PMIC on that bus. Sub-addresses (7-bit)
//    [K drivers/mfd/max77705.c:57-61].
// ---------------------------------------------------------------------------
#define MAX77705_CHG_ADDR         0x69    // [K] 0xD2>>1
#define MAX77705_CCIC_ADDR        0x25    // [K] 0x4A>>1
// Charger block (client 0x69) [K max77705-private.h:75-99, max77705_charger.h:117-188]:
#define MAX77705_CHG_INT_MASK     0xB1
#define MAX77705_CHG_CNFG_00      0xB7    // MODE[3:0]: bit0 CHG, bit1 OTG, bit2 BUCK, bit3 BOOST
#define MAX77705_CHG_CNFG_02      0xB9    // bits 7:6 OTG_ILIM: 0=500mA 1=900mA 2=1.2A 3=1.5A
#define   MAX77705_MODE_BUCK                0x4     // [K] safe intermediate
#define   MAX77705_MODE_OTG_ON              0xE     // [K] BUCK+BOOST+OTG, charger OFF
#define   MAX77705_MODE_CHG_OTG_ON          0xF     // [K] same + charger engine
#define   MAX77705_MODE_MASK                0xF
#define   MAX77705_OTG_ILIM_500MA           0x0     // [K]
// CCIC block (client 0x25) [K max77705-private.h:160-176, ccic/max77705.h]:
#define MAX77705_CC_STATUS0       0x0A    // CCStat[2:0]: 0 none, 1 SINK, 2 SOURCE
#define   MAX77705_CCSTAT_SOURCE            2       // [K]
#define MAX77705_CC_INT_M         0x0F    // init mask 0x20 (VCONNSCI masked) [K]

#endif // USB_HW_H_
