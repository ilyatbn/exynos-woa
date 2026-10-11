/** @file
  PcieHw.h — register vocabulary for the S10e PCIe root port 0 (the WLAN bus).

  Every offset is VERIFIED against a named source; nothing is guessed. Sources
  (all LineageOS/android_kernel_samsung_exynos9820 @ lineage-23.2 unless noted):
    - window bases:              arch/arm64/boot/dts/exynos/exynos9820.dts:4380-4418
                                 (mirrored in Silicon/Exynos9820Pkg Include/Platform/
                                 Exynos9820.h — this file uses those macros)
    - ELBI / DBI port registers: drivers/pci/dwc/pcie-exynos-host-v0.h (cited per line)
    - DW generic port regs:      drivers/pci/dwc/pcie-designware.h / -host.c
    - PHY/PCS/SYSREG sequence:   drivers/pci/dwc/pci-exynos9820_cal.c
    - clock gate offsets:        drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c
  Research trail: notes/m12-wifi-chip.md (W2, W8, WP1).
**/

#ifndef _BEYOND0LTE_PCIE_HW_H
#define _BEYOND0LTE_PCIE_HW_H

#include <Uefi.h>
#include <Platform/Exynos9820.h>

//
// ELBI (port-logic glue) register offsets, from pcie-exynos-host-v0.h.
// Access width 32 unless noted.
//
#define PCIE_ELBI_IRQ_PULSE            0x000  // :123 W1C; bit0 INTA assert, bit2 INTB...
#define PCIE_ELBI_IRQ_INTA_ASSERT      (0x1u << 0)
#define PCIE_ELBI_IRQ_LEVEL            0x004  // :132
#define PCIE_ELBI_IRQ_EN_PULSE         0x00C  // :135
#define PCIE_ELBI_IRQ_LINKDOWN         (0x1u << 30)  // :140/141
#define PCIE_ELBI_APP_LTSSM_ENABLE     0x02C  // :147 value reg; 1 = enable (:156), 0 = disable
#define PCIE_ELBI_TOE_ENABLE           0x034  // establish/assert_phy_reset: write 0x10
#define PCIE_ELBI_SW_WAKE              0x018  // :143; clear bit1 = bus-number enable
#define PCIE_ELBI_RDLH_LINKUP          0x074  // :154; bits[4:0] LTSSM link state, up = 0x0d..0x14
#define PCIE_ELBI_PM_DSTATE            0x088  // :157
#define PCIE_ELBI_APP_REQ_EXIT_L1_MODE 0x0F4  // :159; bit0 HW mode, bit4 L1_REQ_NAK master
#define PCIE_ELBI_L1_REQ_NAK_MASTER    (0x1u << 4)  // :162
#define PCIE_ELBI_PHY_G_RST            0x288  // pci-exynos9820_cal.c:271 pulse 1->0->1
#define PCIE_ELBI_PHY_PCS_CMN_RST      0x28C  // :375 pulse 1->0->1 (100us tail)
#define PCIE_ELBI_PHY_MAC_RST          0x290  // :367 pulse 1->0->1
#define PCIE_ELBI_LINKDOWN_RST_SEL     0x1B8  // :167; 1<<1 = manual (:168)
#define PCIE_ELBI_LINKDOWN_RST_MANUAL  (0x1u << 1)
#define PCIE_ELBI_STATE_HISTORY_CHK    0x274  // :173
#define PCIE_ELBI_HISTORY_BUFFER_EN    0x3    // :176
#define PCIE_ELBI_STATE_POWER_S        0x2BC  // :174; poweron writes 0x200000
#define PCIE_ELBI_STATE_POWER_M        0x2C0  // :175; poweron writes 0xFFFFFFFF
#define PCIE_ELBI_QCH_SEL              0x2C8  // :178
#define PCIE_ELBI_QCH_MASK             (0xF00u | 0xF0u | 0xFu)  // PMU|APB|AXI gating masks :187/193/199
#define PCIE_ELBI_SOFT_CORE_RESET      0x1D0  // :172; 0 -> 20us -> 1

//
// I/A microprogram sequencer (reg "ia" window). Register list verbatim from
// exynos_pcie_assert_phy_reset() (pcie-exynos-host-v0.c:1420-1490, 9820 branch).
//
#define PCIE_IA_ENABLE                 0x000
#define PCIE_IA_BASE0_ELBI             0x030  // <- EXYNOS_PCIE0_ELBI_BASE
#define PCIE_IA_BASE1_PMA              0x034  // <- EXYNOS_PCIE0_PHY_BASE
#define PCIE_IA_BASE2_PCS              0x038  // <- EXYNOS_PCIE0_PCS_BASE
#define PCIE_IA_BASE3_IA               0x03C  // <- EXYNOS_PCIE0_IA_BASE
#define PCIE_IA_CNT_TRG                0x010  // <- 0xb (repeat 10)
#define PCIE_IA_CNT_DELAY              0x040  // <- 0x2ffff

//
// DBI (root port's own config space, DesignWare). Offsets from
// pcie-exynos-host-v0.h unless marked (dw).
//
#define PCIE_DBI_VENDOR_DEVICE         0x000  // RO unless DBI_RO_WR_EN
#define PCIE_DBI_COMMAND               0x004
#define PCIE_DBI_CLASS_CODE            0x008  // dw: PCI_CLASS_DEVICE at 0x00A (2B)
#define PCIE_DBI_HEADER_TYPE           0x00E  // dw: PCI_HEADER_TYPE (1B)
#define PCIE_DBI_PRIMARY_BUS           0x018  // dw: PCI_PRIMARY_BUS (bus/secondary/subordinate)
#define PCIE_DBI_BASE_ADDRESS_0        0x010
#define PCIE_DBI_CAP_OFFSET_PCIE       0x070  // :228 PCIE_CAP_OFFSET
#define PCIE_DBI_CAP_OFFSET_PM         0x040  // :227 PM_CAP_OFFSET
#define PCIE_DBI_LINK_CTRL_STAT        0x080  // :229; bits[19:16] current link speed
#define PCIE_DBI_EXP_LNKCAP            (0x070 + 0x04)
#define PCIE_DBI_EXP_LNKCTL            (0x070 + 0x10)
#define PCIE_DBI_EXP_LNKCTL2           (0x070 + 0x30)
#define PCIE_DBI_LNKCAP_MLW_X1         (0x1u << 4)   // :230
#define PCIE_DBI_LNKCAP_L1EL_64USEC    (0x7u << 15)  // :231
#define PCIE_DBI_LNKCAP_SLS_5_0GB      (0x2u << 0)   // PCI_EXP_LNKCAP_SLS_5_0GB (pci_regs.h)
#define PCIE_DBI_LNKCTL_RL             (0x1u << 5)   // pci_regs.h RETRAIN_LINK
#define PCIE_DBI_LNKCTL2_TLS_MASK      0xFu          // pci_regs.h
#define PCIE_DBI_LNKCTL2_TLS_GEN1      0x1u          // PCI_EXP_LNKCTL2_TLS_2_5GB (:232)
#define PCIE_DBI_PM_CTRL               (0x040 + 0x08)  // pci_regs.h PCI_PM_CTRL
#define PCIE_DBI_MISC_CONTROL          0x8BC  // :248; bit0 DBI_RO_WR_EN (:249)
#define PCIE_DBI_AUX_CLK_FREQ          0xB40  // :251; 0x1A = 26 MHz (:253)
#define PCIE_DBI_L1_SUBSTATES          0xB44  // :254; value 0xEA (:255)
#define PCIE_DBI_PORT_LINK_CONTROL     0x710  // :246
#define PCIE_DBI_PORT_LINK_MODE_MASK   (0x3Fu << 16)  // pcie-designware.h:35
#define PCIE_DBI_PORT_LINK_MODE_1LANE  (0x1u << 16)   // pcie-designware.h:36
#define PCIE_DBI_LINK_WIDTH_SPEED      0x80C  // :257
#define PCIE_DBI_PORT_LOGIC_LW_MASK    (0x1Fu << 8)   // pcie-designware.h:43
#define PCIE_DBI_PORT_LOGIC_LW_1LANE   (0x1u << 8)    // pcie-designware.h:44
#define PCIE_DBI_PORT_LOGIC_SPEED_CHG  (0x1u << 17)   // :258

//
// iATU (outbound address translation), via DBI. pcie-exynos-host-v0.h:260-283.
//
#define PCIE_ATU_VIEWPORT              0x900
#define PCIE_ATU_REGION_OUTBOUND       (0x0u << 31)
#define PCIE_ATU_REGION_IDX0           0x0
#define PCIE_ATU_REGION_IDX1           0x1
#define PCIE_ATU_CR1                   0x904
#define PCIE_ATU_TYPE_MEM              (0x0u << 0)
#define PCIE_ATU_TYPE_CFG0             (0x4u << 0)
#define PCIE_ATU_CR2                   0x908
#define PCIE_ATU_ENABLE                (0x1u << 31)
#define PCIE_ATU_LOWER_BASE            0x90C
#define PCIE_ATU_UPPER_BASE            0x910
#define PCIE_ATU_LIMIT                 0x914
#define PCIE_ATU_LOWER_TARGET          0x918
#define PCIE_ATU_UPPER_TARGET          0x91C
#define PCIE_ATU_BUS(x)                (((x) & 0xFFu) << 24)  // :275
#define PCIE_ATU_DEV(x)                (((x) & 0x1Fu) << 19)  // :276
#define PCIE_ATU_FUNC(x)               (((x) & 0x7u)  << 16)  // :277

//
// PHY / PCS / SYSREG offsets — pci-exynos9820_cal.c ch0 (WiFi port) path.
//
#define PCIE_PCS_RX_ELECIDLE_CTL       0x0EC  // cal.c:36 bit3 (ch0)
#define ELECIDLE_IGNORE                1u     // v0.h:326
#define ELECIDLE_ENABLE                0u     // v0.h:327
#define PCIE_PCS_L1SS_TIMEOUT          0x00C  // cal.c:354 clear bit1, set bit4
#define PCIE_PCS_CDR_RST               0x0D0  // cal.c:383-392 bits[7:6] 3->2->0
#define PCIE_PCS_REFCLK_CTRL0          0x100  // cal.c:114/351 (pwrdn bit6; config <- 0x87)
#define PCIE_PCS_REFCLK_CTRL1          0x104  // cal.c:118/352 (pwrdn bit7; config <- 0x50)
#define PCIE_PCS_TX_LATENCY            0x0F8  // cal.c:349 <- 0x70
#define PCIE_PHY_CMN_PWRCTL            0x080  // phy + 0x20*4: pwrdn 0xC1 / clear 0xC0 (cal.c:75/123)
#define PCIE_PHY_TRSV_PWRCTL           0x15C  // phy + 0x57*4: pwrdn 0xFE / clear 0x7E (cal.c:72/126)
#define PCIE_PHY_TRSV_BASE_WORD        0x0C0  // phy + 0x30*4: TRSV table start (cal.c:287)
#define PCIE_SYSREG_PCIE_CTRL0         0x000  // pcie_0 "sysreg" window +0: set bit1 PCS_HIGH_SPEED (cal.c:254)
#define PCIE_SYSREG_PCIE_CTRL_C        0x00C  // +0xC: lane mux + lane0 enable bit12 (cal.c:255-261)

//
// SYSREG_FSYS1 block (separate from the pcie_0 "sysreg" window; DT
// sysreg-controller@13C10000 +0x1200). Cache-coherency sharability control:
// pcie-exynos-host-v0.c enable_cache_cohernecy() + v0.h:297-300.
//
#define SYSREG_FSYS1_SHARABILITY_CTRL  0x700  // bits [8+4*ch +: 2] = 0x3 enable (ch0 -> bits 11:8)

//
// Clocks: CMU_FSYS1 gate registers (cal-if SFR table; macro names in
// Exynos9820.h). Manual-override convention (notes/s10e-live-reads.md §7.5):
// bit 20 MANUAL, bit 21 CG_VAL. QCH value follows the repo's HSI2C driver
// convention: ENABLE|CLOCK_REQUEST = 0x3 (drivers/Exynos9820Hsi2c/exynos9820hsi2c.h:71-74).
//
#define CMU_GATE_MANUAL_ON             ((0x1u << 21) | (0x1u << 20))
#define CMU_QCH_ON                     0x3u

//
// GPIO (bank register layout VERIFIED from drivers/pinctrl/samsung/
// pinctrl-exynos-arm64.c:638-655: FSYS1 gpf2 bank @+0x020, PERIC0 gpg0 bank
// @+0x080; exynos9810_bank_type_off field widths {CON 4, DAT 1, PUD 4, DRV 4}
// at reg offsets {0x00, 0x04, 0x08, 0x0C}).
//
#define GPIO_BANK_STRIDE               0x20
#define GPIO_REG_CON                   0x00
#define GPIO_REG_DAT                   0x04
#define GPIO_REG_PUD                   0x08
#define GPIO_REG_DRV                   0x0C
#define PERIC0_GPG0_BANK_OFF           0x080  // arm64.c:649
#define FSYS1_GPF2_BANK_OFF            0x020  // arm64.c:641
#define PIN_F(p)                       ((p) * 4)

// DT pinctrl states (exynos9820-pinctrl.dtsi:937-963, 1060-1066):
#define WLAN_EN_FUNC_OUT               1u     // cfg_wlanen gpg0-5 function 1
#define PERST_FUNC_OUT                 1u     // pcie0_perst  gpf2-2 function 1
#define CLKREQ_FUNC                    2u     // pcie0_clkreq gpf2-0 function 2

//
// Endpoint config-space offsets used at bring-up (pci_regs.h names).
//
#define EP_CFG_COMMAND                 0x004
#define EP_CFG_BASE_ADDRESS_0          0x010
#define EP_CFG_INTERRUPT_LINE          0x03C
#define EP_CFG_COMMAND_MEM_EN          (0x1u << 1)
#define EP_CFG_COMMAND_BUS_MASTER_EN (0x1u << 2)

#define PCIE_LINK_MAX_TRIES            10     // establish_link retry_cnt (v0.c:1296)
#define PCIE_LINK_POLL_ITER            2000   // MAX_TIMEOUT (v0.h:16)

#endif // _BEYOND0LTE_PCIE_HW_H
