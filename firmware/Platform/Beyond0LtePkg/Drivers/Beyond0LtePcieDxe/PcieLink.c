/** @file
  PcieLink.c — clocks, GPIO, root-port setup and link training for the S10e
  PCIe0 port, transliterated from drivers/pci/dwc/pcie-exynos-host-v0.c
  (poweron at :2447, establish_link at :1217, setup_rc at :1072, viewport at
  :1133/:1191) plus the dw_pcie_setup/dw_pcie_setup_rc generic parts
  (pcie-designware.c / -host.c). Kernel source line cites are against the
  LineageOS lineage-23.2 tree; research trail notes/m12-wifi-chip.md W8.

  Deviations from the kernel, each deliberate and recorded:
   - Clocks: the kernel goes through the ACPM/HWACG clock framework; we write
     the CMU_FSYS1 gate registers directly (offsets VERIFIED from cal-if).
     Whether HWACG objects is open item P4 (design doc §6).
   - SysMMU: the kernel enables the PCIe SysMMU (board use-sysmmu=true); we
     run identity (P5). The DT MEM iATU range is 1:1 (CPU == PCI address), so
     the EP's DMA targets are plain CPU addresses.
   - PMU wakeup mask: skipped by the kernel itself on 9820
     (#if !defined(CONFIG_SOC_EXYNOS9820), v0.c:2461).
   - IRQ: we do not wire a DXE interrupt handler; R1 only needs the link and
     the config read. ELBI INTx pulse routing is left for the firmware-up rung.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include "PcieInternal.h"

//
// ELBI / DBI accessors (the kernel's exynos_elbi_read/write and
// exynos_pcie_rd/wr_own_conf over the DBI window).
//
VOID
ElbiWrite (IN UINT32  Val, IN UINT32  Off)
{
  mPcie.Elbi[Off / 4] = Val;
}

UINT32
ElbiRead (IN UINT32  Off)
{
  return mPcie.Elbi[Off / 4];
}

VOID
DbiWrite (IN UINT32  Val, IN UINT32  Off)
{
  mPcie.Dbi[Off / 4] = Val;
}

UINT32
DbiRead (IN UINT32  Off)
{
  return mPcie.Dbi[Off / 4];
}

VOID
DbiRmw (IN UINT32  Set, IN UINT32  Clear, IN UINT32  Off)
{
  UINT32  V = DbiRead (Off);

  V = (V & ~Clear) | Set;
  DbiWrite (V, Off);
}

VOID
ClocksOn (
  VOID
  )
// CMU_FSYS1 PCIe GEN2 gates: MANUAL (bit20) + CG_VAL (bit21) = clock on by
// software (convention VERIFIED in notes/s10e-live-reads.md §7.5). QCH set to
// ENABLE|CLOCK_REQUEST (repo HSI2C driver convention). Offsets VERIFIED from
// cal-if cmucal-sfr.c; names in Exynos9820.h.
{
  STATIC CONST UINT32  Gates[] = {
    EXYNOS_FSYS1_GATE_PCIE_PHY_REFCLK,
    EXYNOS_FSYS1_GATE_PCIE_DBI_ACLK,
    EXYNOS_FSYS1_GATE_PCIE_IEEE1500,
    EXYNOS_FSYS1_GATE_PCIE_MSTR_ACLK,
    EXYNOS_FSYS1_GATE_PCIE_DRIVER_APB,
    EXYNOS_FSYS1_GATE_PCIE_PIPE2_APB,
    EXYNOS_FSYS1_GATE_PCIE_SLV_ACLK,
  };
  STATIC CONST UINT32  Qchs[] = {
    EXYNOS_FSYS1_QCH_PCIE_APB,
    EXYNOS_FSYS1_QCH_PCIE_DBI,
    EXYNOS_FSYS1_QCH_PCIE_MSTR,
    EXYNOS_FSYS1_QCH_PCIE_PCS,
    EXYNOS_FSYS1_QCH_PCIE_PHY,
  };
  UINTN  I;

  // RMW (set bits), matching the repo HSI2C driver's SetRegisterBits
  // convention (drivers/Exynos9820Hsi2c/controller.cpp:231) so the
  // automatic-gating bits are not clobbered.
  for (I = 0; I < ARRAY_SIZE (Gates); I++) {
    MmioOr32 (EXYNOS_CMU_FSYS1_BASE + Gates[I], CMU_GATE_MANUAL_ON);
  }

  for (I = 0; I < ARRAY_SIZE (Qchs); I++) {
    MmioOr32 (EXYNOS_CMU_FSYS1_BASE + Qchs[I], CMU_QCH_ON);
  }
}

VOID
PmuPhyControlOn (
  VOID
  )
// v0.c poweron: regmap_update_bits(pmureg, PCIE_PHY_CONTROL + ch*4, 0x1, 1)
// with PCIE_PHY_CONTROL = 0x71C (v0.h:223), ch0.
{
  UINTN  Addr = EXYNOS_PMU_BASE + EXYNOS_PMU_PCIE_PHY_CONTROL;  // ch0: +0
  UINT32 V      = MmioRead32 (Addr);

  MmioWrite32 (Addr, (V & ~0x1u) | 0x1u);
}

VOID
CacheCoherencyEnable (
  VOID
  )
// v0.c:1885 enable_cache_cohernecy(): ip_ver 0x982000 > 0x889500, so the
// board's use-cache-coherency=true is applied at SYSREG_FSYS1+0x700,
// bits [8 + 4*ch +: 2] = 0x3 (v0.h:297-300).
{
  UINTN   Addr = EXYNOS_SYSREG_FSYS1_BASE + SYSREG_FSYS1_SHARABILITY_CTRL;
  UINT32  Mask = 0x3u << (8 + 0 * 4);
  UINT32  V    = MmioRead32 (Addr);

  MmioWrite32 (Addr, (V & ~Mask) | (0x3u << (8 + 0 * 4)));
}

VOID
WlanEnOn (
  VOID
  )
// Board dts: WLAN_EN = gpg0-5, cfg_wlanen function 1 (out), pud 0 (none),
// drv 3. PERIC0 pinctrl base + gpg0 bank 0x080 (arm64.c:649).
// DT default state drives the pin; poweron path sets it high (v0.c:776-778).
{
  UINTN  Bank = EXYNOS_PERIC0_GPIO_BASE + PERIC0_GPG0_BANK_OFF;

  MmioWrite32 (Bank + GPIO_REG_CON,
    (MmioRead32 (Bank + GPIO_REG_CON) & ~(0xFu << PIN_F (EXYNOS_WLAN_EN_PIN)))
      | (WLAN_EN_FUNC_OUT << PIN_F (EXYNOS_WLAN_EN_PIN)));
  MmioWrite32 (Bank + GPIO_REG_PUD,
    MmioRead32 (Bank + GPIO_REG_PUD) & ~(0xFu << PIN_F (EXYNOS_WLAN_EN_PIN)));
  MmioWrite32 (Bank + GPIO_REG_DRV,
    (MmioRead32 (Bank + GPIO_REG_DRV) & ~(0xFu << PIN_F (EXYNOS_WLAN_EN_PIN)))
      | (0x3u << PIN_F (EXYNOS_WLAN_EN_PIN)));      // cfg_wlanen pin-drv 3
  MmioWrite32 (Bank + GPIO_REG_DAT,
    MmioRead32 (Bank + GPIO_REG_DAT) | (1u << EXYNOS_WLAN_EN_PIN));
}

VOID
PowerOnPrep (
  VOID
  )
// The part of exynos_pcie_poweron (v0.c:2447-2500) that runs BEFORE
// establish_link: PHY comes out of its standby power-down state first, then
// the ELBI history/monitor registers are armed. Kernel order kept exactly.
{
  PhyAllPwrdnClear ();                              // v0.c:2492-2497

  ElbiWrite (0, PCIE_ELBI_STATE_HISTORY_CHK);       // v0.c:2500-2502
  ElbiWrite (PCIE_ELBI_HISTORY_BUFFER_EN, PCIE_ELBI_STATE_HISTORY_CHK);
  ElbiWrite (0x200000, PCIE_ELBI_STATE_POWER_S);    // v0.c:2504-2505
  ElbiWrite (0xFFFFFFFF, PCIE_ELBI_STATE_POWER_M);
}

VOID
PerstSet (
  IN BOOLEAN  High
  )
// PERST# = gpf2-2, pcie0_perst function 1 (out), FSYS1 pinctrl + gpf2 bank
// 0x020 (arm64.c:641). Also mux gpf2-0 to CLKREQ function 2 (pcie0_clkreq,
// part of the DT default state).
{
  UINTN  Bank = EXYNOS_FSYS1_GPIO_BASE + FSYS1_GPF2_BANK_OFF;
  UINT32 Con  = MmioRead32 (Bank + GPIO_REG_CON);

  Con &= ~(0xFu << PIN_F (EXYNOS_WLAN_PERST_PIN));
  Con |= PERST_FUNC_OUT << PIN_F (EXYNOS_WLAN_PERST_PIN);
  Con &= ~(0xFu << PIN_F (0));                    // gpf2-0
  Con |= CLKREQ_FUNC << PIN_F (0);
  MmioWrite32 (Bank + GPIO_REG_CON, Con);

  if (High) {
    MmioWrite32 (Bank + GPIO_REG_DAT,
      MmioRead32 (Bank + GPIO_REG_DAT) | (1u << EXYNOS_WLAN_PERST_PIN));
  } else {
    MmioWrite32 (Bank + GPIO_REG_DAT,
      MmioRead32 (Bank + GPIO_REG_DAT) & ~(1u << EXYNOS_WLAN_PERST_PIN));
  }
}

VOID
SetupRc (
  VOID
  )
// dw_pcie_setup (lanes) + dw_pcie_setup_rc (bus numbers, command, class) +
// exynos_pcie_setup_rc (v0.c:1072-1131). The kernel rewrites the RC's own
// VID/DID to Samsung IDs; we keep whatever reset left and just read it —
// rewriting it serves the kernel's own enumeration bookkeeping, not ours.
// Recorded deviation.
{
  UINT32  V;

  // 1 lane (DT num-lanes = 1)
  V = DbiRead (PCIE_DBI_PORT_LINK_CONTROL);
  V = (V & ~PCIE_DBI_PORT_LINK_MODE_MASK) | PCIE_DBI_PORT_LINK_MODE_1LANE;
  DbiWrite (V, PCIE_DBI_PORT_LINK_CONTROL);

  V = DbiRead (PCIE_DBI_LINK_WIDTH_SPEED);
  V = (V & ~PCIE_DBI_PORT_LOGIC_LW_MASK) | PCIE_DBI_PORT_LOGIC_LW_1LANE;
  DbiWrite (V, PCIE_DBI_LINK_WIDTH_SPEED);

  // RC BAR0 = config-space type marker (dw-host.c: PCI_BASE_ADDRESS_0 <- 4)
  DbiWrite (0x00000004, PCIE_DBI_BASE_ADDRESS_0);
  DbiWrite (0x00000000, PCIE_DBI_BASE_ADDRESS_0 + 4);

  // bus numbers: primary 0, secondary 1, subordinate 0xFF (dw-host.c:
  // val |= 0x00ff0100)
  V = DbiRead (PCIE_DBI_PRIMARY_BUS) & 0xFF000000u;
  DbiWrite (V | 0x00FF0100u, PCIE_DBI_PRIMARY_BUS);

  // command: IO|MEM|MASTER|SERR (dw-host.c)
  DbiRmw (0x1u << 0 | 0x1u << 1 | 0x1u << 2 | 0x1u << 8, 0, PCIE_DBI_COMMAND);

  // class code = PCI bridge (dw-host.c, under DBI_RO_WR_EN)
  DbiRmw (0x1, 0, PCIE_DBI_MISC_CONTROL);              // DBI_RO_WR_EN (v0.c:1079)
  V = DbiRead (PCIE_DBI_CLASS_CODE);
  V = (V & 0x0000FFFFu) | (0x060400u << 8);            // class 0x06/0x04/0x00
  DbiWrite (V, PCIE_DBI_CLASS_CODE);
  DbiRmw (0, 0x1, PCIE_DBI_MISC_CONTROL);              // RO_WR_DIS

  // exynos_pcie_setup_rc (v0.c:1082-1131):
  // LNKCAP: MLW x1, L1 exit latency 64us, SLS 5.0GB (ch0 on >=0x981000)
  V = DbiRead (PCIE_DBI_EXP_LNKCAP);
  V &= ~(0xFu | 0x3Fu << 4 | 0x7u << 15);            // SLS|MLW|L1EL (pci_regs.h)
  V |= PCIE_DBI_LNKCAP_L1EL_64USEC | PCIE_DBI_LNKCAP_MLW_X1 |
       PCIE_DBI_LNKCAP_SLS_5_0GB;
  DbiWrite (V, PCIE_DBI_EXP_LNKCAP);

  // aux clock 26 MHz (v0.c:1099)
  DbiWrite (0x1A, PCIE_DBI_AUX_CLK_FREQ);

  // L1 substate durations (v0.c:1102, PCIE_L1_SUB_VAL 0xEA)
  DbiWrite (0xEA, PCIE_DBI_L1_SUBSTATES);

  // PM control clear (v0.c:1105)
  DbiWrite (DbiRead (PCIE_DBI_PM_CTRL) & 0xFFFF0000u, PCIE_DBI_PM_CTRL);

  // link retrain request (v0.c:1108-1110)
  DbiRmw (PCIE_DBI_LNKCTL_RL, 0, PCIE_DBI_EXP_LNKCTL);

  // target link speed = Gen1 (DT max-link-speed; v0.c:1113-1116)
  V = DbiRead (PCIE_DBI_EXP_LNKCTL2);
  V = (V & ~PCIE_DBI_LNKCTL2_TLS_MASK) | PCIE_DBI_LNKCTL2_TLS_GEN1;
  DbiWrite (V, PCIE_DBI_EXP_LNKCTL2);

  // PORT_LOGIC_SPEED_CHANGE (dw-host.c tail)
  DbiRmw (PCIE_DBI_PORT_LOGIC_SPEED_CHG, 0, PCIE_DBI_LINK_WIDTH_SPEED);
}

EFI_STATUS
ProgramAtu (
  VOID
  )
// exynos_pcie_prog_viewport_cfg0 (v0.c:1133, region1, bus 1 dev 0 func 0) +
// prog_viewport_mem_outbound (v0.c:1191, region0, 1:1 MEM).
{
  // CFG0: CPU 0x14FFE000..0x14FFFFFF -> config bus1/dev0/func0
  DbiWrite (PCIE_ATU_REGION_OUTBOUND | PCIE_ATU_REGION_IDX1, PCIE_ATU_VIEWPORT);
  DbiWrite ((UINT32)EXYNOS_PCIE0_CFG_BASE, PCIE_ATU_LOWER_BASE);
  DbiWrite (0, PCIE_ATU_UPPER_BASE);
  DbiWrite ((UINT32)EXYNOS_PCIE0_CFG_BASE + (UINT32)EXYNOS_PCIE0_CFG_SIZE - 1,
    PCIE_ATU_LIMIT);
  DbiWrite (PCIE_ATU_BUS (1) | PCIE_ATU_DEV (0) | PCIE_ATU_FUNC (0),
    PCIE_ATU_LOWER_TARGET);
  DbiWrite (0, PCIE_ATU_UPPER_TARGET);
  DbiWrite (PCIE_ATU_TYPE_CFG0, PCIE_ATU_CR1);
  DbiWrite (PCIE_ATU_ENABLE, PCIE_ATU_CR2);

  // MEM: CPU 0x14000000 -> PCI 0x14000000, 0xFF0000 (DT ranges, 1:1)
  DbiWrite (PCIE_ATU_REGION_OUTBOUND | PCIE_ATU_REGION_IDX0, PCIE_ATU_VIEWPORT);
  DbiWrite (PCIE_ATU_TYPE_MEM, PCIE_ATU_CR1);
  DbiWrite ((UINT32)EXYNOS_PCIE0_MEM_BASE, PCIE_ATU_LOWER_BASE);
  DbiWrite (0, PCIE_ATU_UPPER_BASE);
  DbiWrite ((UINT32)EXYNOS_PCIE0_MEM_BASE + (UINT32)EXYNOS_PCIE0_MEM_SIZE - 1,
    PCIE_ATU_LIMIT);
  DbiWrite ((UINT32)EXYNOS_PCIE0_MEM_BASE, PCIE_ATU_LOWER_TARGET);
  DbiWrite (0, PCIE_ATU_UPPER_TARGET);
  DbiWrite (PCIE_ATU_ENABLE, PCIE_ATU_CR2);

  return EFI_SUCCESS;
}

UINT32
EpCfgRead (
  IN UINT32  Where
  )
// CFG0 window is programmed once for bus1/dev0/func0 (single endpoint), so a
// plain read at Cfg + offset is the kernel's rd_other_conf for this device.
{
  return mPcie.Cfg[Where / 4];
}

VOID
EpCfgWrite (
  IN UINT32  Val,
  IN UINT32  Where
  )
{
  mPcie.Cfg[Where / 4] = Val;
}

EFI_STATUS
EstablishLink (
  VOID
  )
// exynos_pcie_establish_link (v0.c:1217-1341), kernel-faithful order.
{
  UINT32   Try;
  UINTN    Poll;
  UINT32   V;

  for (Try = 0; Try < PCIE_LINK_MAX_TRIES; Try++) {
    PhyCheckRxElecidle (ELECIDLE_IGNORE);           // v0.c:1227

    ElbiWrite (0, PCIE_ELBI_SOFT_CORE_RESET);       // v0.c:1231
    gBS->Stall (200);
    ElbiWrite (1, PCIE_ELBI_SOFT_CORE_RESET);

    PerstSet (TRUE);                                // v0.c:1235, 18-20ms
    gBS->Stall (200000);

    V = ElbiRead (PCIE_ELBI_APP_REQ_EXIT_L1_MODE);  // v0.c:1240-1243
    ElbiWrite (V | 0x1u | PCIE_ELBI_L1_REQ_NAK_MASTER,
      PCIE_ELBI_APP_REQ_EXIT_L1_MODE);
    ElbiWrite (PCIE_ELBI_LINKDOWN_RST_MANUAL, PCIE_ELBI_LINKDOWN_RST_SEL); // 1244

    V = ElbiRead (PCIE_ELBI_QCH_SEL);               // v0.c:1247-1251 (>=0x889500)
    ElbiWrite (V & ~PCIE_ELBI_QCH_MASK, PCIE_ELBI_QCH_SEL);

    PhyConfig ();                                   // assert_phy_reset v0.c:1407
    PhyRunIaSequencer ();                           // v0.c:1420-1493

    // bus-number enable (v0.c:1496-1498)
    V = ElbiRead (PCIE_ELBI_SW_WAKE);
    ElbiWrite (V & ~(0x1u << 1), PCIE_ELBI_SW_WAKE);

    SetupRc ();                                     // v0.c:1266-1267

    PhyCheckRxElecidle (ELECIDLE_ENABLE);           // v0.c:1271

    ElbiWrite (1, PCIE_ELBI_APP_LTSSM_ENABLE);      // v0.c:1279 (LTSSM enable)

    Poll = 0;                                       // v0.c:1281-1288
    while (Poll < PCIE_LINK_POLL_ITER) {
      V = ElbiRead (PCIE_ELBI_RDLH_LINKUP) & 0x1F;
      if (V >= 0x0D && V <= 0x14) {
        break;
      }

      Poll++;
      gBS->Stall (100);
    }

    if (Poll < PCIE_LINK_POLL_ITER) {
      V = DbiRead (PCIE_DBI_LINK_CTRL_STAT);        // v0.c:1331-1333
      DEBUG ((DEBUG_INFO, "[PCIe] link up, speed GEN%x\n", (V >> 16) & 0xF));

      ElbiWrite (ElbiRead (PCIE_ELBI_IRQ_PULSE),    // W1C stale pulses, v0.c:1336-1337
        PCIE_ELBI_IRQ_PULSE);
      // linkdown watch on (v0.c:1338-1340); INTx enable is a later rung.
      ElbiWrite (ElbiRead (PCIE_ELBI_IRQ_EN_PULSE) | PCIE_ELBI_IRQ_LINKDOWN,
        PCIE_ELBI_IRQ_EN_PULSE);

      ProgramAtu ();                                // v0.c:1342-1345
      return EFI_SUCCESS;
    }

    DEBUG ((DEBUG_WARN, "[PCIe] link try %x failed, RDLH=%x\n", Try,
      ElbiRead (PCIE_ELBI_RDLH_LINKUP)));

    // teardown for retry (v0.c:1300-1307)
    PerstSet (FALSE);
    ElbiWrite (0, PCIE_ELBI_APP_LTSSM_ENABLE);
  }

  return EFI_DEVICE_ERROR;                          // link never came up
}
