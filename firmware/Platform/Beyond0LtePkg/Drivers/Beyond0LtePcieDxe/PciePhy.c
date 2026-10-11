/** @file
  PciePhy.c — the Exynos 9820 PCIe0 PHY bring-up, transliterated from
  drivers/pci/dwc/pci-exynos9820_cal.c (ch0 = the WiFi port) in the shipping
  LineageOS exynos9820 kernel. The CMN/TRSV tables are that driver's
  "26MHz Gen2 / Ch0 HS1 Gen2" tuning constants, copied verbatim; the OTP
  re-tune (exynos_pcie_phy_otp_config, magic 0x5030) is NOT applied — the
  kernel only writes OTP values when an OTP parse succeeds, and we have no
  OTP reader; deviation recorded here and in the design doc.

  Kernel source: notes/m12-wifi-chip.md WP1. Nothing here is validated on an
  S10e yet.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include "PcieInternal.h"

//
// phy_base + i*4, i = 0..47 (cal.c:181-188)
//
STATIC CONST UINT32  mPhyCmnCh0[48] = {
  0x01, 0xE1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x0C,
  0x61, 0x45, 0x65, 0x24, 0x33, 0x18, 0xE3, 0xFC,
  0xD8, 0x05, 0xE6, 0x80, 0x00, 0x00, 0x00, 0x00,
  0x60, 0x11, 0x00, 0xA0, 0x05, 0x04, 0x18, 0x88,
  0xC0, 0xFF, 0x9B, 0x52, 0x22, 0x30, 0x4F, 0xDC,
  0x40, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x80,
};

//
// phy_base + (0x30 + i)*4, i = 0..79 (cal.c:189-200)
//
STATIC CONST UINT32  mPhyTrsvCh0[80] = {
  0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xC0, 0xFF,
  0xFF, 0x3F, 0x8C, 0xC8, 0x02, 0x01, 0x88, 0x80,
  0x06, 0x90, 0x6C, 0x66, 0x09, 0x61, 0x42, 0x44,
  0xC6, 0x50, 0x0A, 0x33, 0x58, 0xE7, 0x20, 0x22,
  0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7E,
  0x00, 0x00, 0x55, 0x15, 0xAC, 0xAA, 0x3E, 0x00,
  0x00, 0x00, 0x20, 0x3F, 0x00, 0x03, 0x01, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
  0x05, 0x85, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
};

VOID
PhyCheckRxElecidle (
  IN UINT32  Val
  )
// cal.c:31-46 (ch0 branch): PCS+0xEC bit3 <- val
{
  UINT32  R;

  R = mPcie.Pcs[PCIE_PCS_RX_ELECIDLE_CTL / 4] & ~(0x1u << 3);
  mPcie.Pcs[PCIE_PCS_RX_ELECIDLE_CTL / 4] = R | ((Val & 0x1u) << 3);
}

VOID
PhyAllPwrdnClear (
  VOID
  )
// cal.c:97-127 (ch0 path). The SUPPORT_CHANNEL_1 sysreg block is compiled out
// in the shipping build (ch1 is the disabled modem doorbell), so not here.
// exynos_100Mhz_from_socpll_off is #ifdef SOCPLL_GATING (off): "already set in
// bootloader" per the comment.
{
  UINT32  R;

  R = mPcie.Pcs[PCIE_PCS_REFCLK_CTRL0 / 4];
  mPcie.Pcs[PCIE_PCS_REFCLK_CTRL0 / 4] = R & ~(0x1u << 6);          // cal.c:114-116
  R = mPcie.Pcs[PCIE_PCS_REFCLK_CTRL1 / 4];
  mPcie.Pcs[PCIE_PCS_REFCLK_CTRL1 / 4] = R & ~(0x1u << 7);          // cal.c:118-120
  mPcie.Phy[PCIE_PHY_CMN_PWRCTL / 4]   = 0xC0;                      // cal.c:123
  mPcie.Phy[PCIE_PHY_TRSV_PWRCTL / 4]  = 0x7E;                      // cal.c:126
}

VOID
PhyConfig (
  VOID
  )
// cal.c:177-394, ch0 branch, verbatim order.
{
  UINT32  R;
  UINTN   I;

  // PCS_HIGH_SPEED (cal.c:254)
  mPcie.SysReg[PCIE_SYSREG_PCIE_CTRL0 / 4] =
    mPcie.SysReg[PCIE_SYSREG_PCIE_CTRL0 / 4] | (0x1u << 1);

  // lane mux (cal.c:255-258)
  R = mPcie.SysReg[PCIE_SYSREG_PCIE_CTRL_C / 4];
  R = ((R & ~(0xFu << 4)) & ~(0xFu << 2)) | (0x3u << 2);
  mPcie.SysReg[PCIE_SYSREG_PCIE_CTRL_C / 4] = R & ~(0x1u << 1);

  // Lane0 enable (cal.c:260)
  mPcie.SysReg[PCIE_SYSREG_PCIE_CTRL_C / 4] =
    mPcie.SysReg[PCIE_SYSREG_PCIE_CTRL_C / 4] | (0x1u << 12);

  // pcs_g_rst: ELBI+0x288 pulse (cal.c:271-276)
  mPcie.Elbi[PCIE_ELBI_PHY_G_RST / 4] = 1;
  gBS->Stall (100);
  mPcie.Elbi[PCIE_ELBI_PHY_G_RST / 4] = 0;
  gBS->Stall (100);
  mPcie.Elbi[PCIE_ELBI_PHY_G_RST / 4] = 1;
  gBS->Stall (100);

  // PHY common block (cal.c:280-282)
  for (I = 0; I < ARRAY_SIZE (mPhyCmnCh0); I++) {
    mPcie.Phy[I] = mPhyCmnCh0[I];
  }

  // PHY transceiver/receiver block (cal.c:284-287)
  for (I = 0; I < ARRAY_SIZE (mPhyTrsvCh0); I++) {
    mPcie.Phy[(0x30 + I)] = mPhyTrsvCh0[I];
  }

  // OTP tuning (cal.c:299-302) intentionally NOT applied — see file header.

  // ch0 PCS extras (cal.c:346-357)
  mPcie.Pcs[PCIE_PCS_TX_LATENCY / 4] = 0x70;
  mPcie.Pcs[PCIE_PCS_REFCLK_CTRL0 / 4] = 0x87;
  mPcie.Pcs[PCIE_PCS_REFCLK_CTRL1 / 4] = 0x50;
  R = mPcie.Pcs[PCIE_PCS_L1SS_TIMEOUT / 4];
  R &= ~(0x1u << 1);
  R |= (0x1u << 4);
  mPcie.Pcs[PCIE_PCS_L1SS_TIMEOUT / 4] = R;

  // PCIE_MAC RST: ELBI+0x290 pulse (cal.c:366-372)
  mPcie.Elbi[PCIE_ELBI_PHY_MAC_RST / 4] = 1;
  gBS->Stall (100);
  mPcie.Elbi[PCIE_ELBI_PHY_MAC_RST / 4] = 0;
  gBS->Stall (100);
  mPcie.Elbi[PCIE_ELBI_PHY_MAC_RST / 4] = 1;
  gBS->Stall (100);

  // PCIE_PHY PCS&PMA(CMN)_RST: ELBI+0x28C pulse, 100us tail (cal.c:374-380)
  mPcie.Elbi[PCIE_ELBI_PHY_PCS_CMN_RST / 4] = 1;
  gBS->Stall (100);
  mPcie.Elbi[PCIE_ELBI_PHY_PCS_CMN_RST / 4] = 0;
  gBS->Stall (100);
  mPcie.Elbi[PCIE_ELBI_PHY_PCS_CMN_RST / 4] = 1;
  gBS->Stall (1000);

  // CDR Reset: PCS+0xD0 bits[7:6] 3 -> 2 -> 0 (cal.c:382-393)
  R = mPcie.Pcs[PCIE_PCS_CDR_RST / 4];
  R |= (0x3u << 6);
  mPcie.Pcs[PCIE_PCS_CDR_RST / 4] = R;
  gBS->Stall (200);
  R = mPcie.Pcs[PCIE_PCS_CDR_RST / 4];
  R &= ~(0x3u << 6);
  R |= (0x2u << 6);
  mPcie.Pcs[PCIE_PCS_CDR_RST / 4] = R;
  R &= ~(0x3u << 6);
  mPcie.Pcs[PCIE_PCS_CDR_RST / 4] = R;
}

VOID
PhyRunIaSequencer (
  VOID
  )
// exynos_pcie_assert_phy_reset() I/A block, 9820 branch
// (pcie-exynos-host-v0.c:1420-1490). The microprogram is written as
// (instruction, operand) register pairs at fixed IA offsets, verbatim.
{
  // ELBI TOE enable (v0.c:1423)
  mPcie.Elbi[PCIE_ELBI_TOE_ENABLE / 4] = 0x10;

  // base-address map (v0.c:1431-1436, 9820 branch)
  mPcie.Ia[PCIE_IA_BASE0_ELBI / 4] = (UINT32)EXYNOS_PCIE0_ELBI_BASE;
  mPcie.Ia[PCIE_IA_BASE1_PMA / 4]  = (UINT32)EXYNOS_PCIE0_PHY_BASE;
  mPcie.Ia[PCIE_IA_BASE2_PCS / 4]  = (UINT32)EXYNOS_PCIE0_PCS_BASE;
  mPcie.Ia[PCIE_IA_BASE3_IA / 4]   = (UINT32)EXYNOS_PCIE0_IA_BASE;

  // repeat 10, delay (v0.c:1439-1440)
  mPcie.Ia[PCIE_IA_CNT_TRG / 4]   = 0xB;
  mPcie.Ia[PCIE_IA_CNT_DELAY / 4] = 0x2FFFF;

  // SQ0: UIR_1 data mask (v0.c:1443-1444)
  mPcie.Ia[0x100 / 4] = 0x50000004;   // DATA MASK_REG
  mPcie.Ia[0x104 / 4] = 0xFFFF;       // enable bit 5

  // SQ1: BRT count check and exit (v0.c:1447-1448)
  mPcie.Ia[0x108 / 4] = 0x20930014;   // READ AFC_DONE
  mPcie.Ia[0x10C / 4] = 0xA;

  // SQ2: count increment (v0.c:1451-1452)
  mPcie.Ia[0x110 / 4] = 0x40030044;
  mPcie.Ia[0x114 / 4] = 0x1;

  // SQ3: data mask, mask AFC_DONE bit (v0.c:1455-1456)
  mPcie.Ia[0x118 / 4] = 0x50000004;
  mPcie.Ia[0x11C / 4] = 0x20;

  // SQ4: BRT read and check AFC_DONE (v0.c:1459-1460)
  mPcie.Ia[0x120 / 4] = 0x209101EC;
  mPcie.Ia[0x124 / 4] = 0x20;

  // SQ5..SQ7: CDR_EN 0xC0 -> 0x80 -> 0x0 (v0.c:1463-1474)
  mPcie.Ia[0x128 / 4] = 0x400200D0;
  mPcie.Ia[0x12C / 4] = 0xC0;
  mPcie.Ia[0x130 / 4] = 0x400200D0;
  mPcie.Ia[0x134 / 4] = 0x80;
  mPcie.Ia[0x138 / 4] = 0x400200D0;
  mPcie.Ia[0x13C / 4] = 0x0;

  // SQ8: loop (v0.c:1477-1478)
  mPcie.Ia[0x140 / 4] = 0x100101EC;
  mPcie.Ia[0x144 / 4] = 0x20;

  // SQ9: clear L1.2 exit interrupt (v0.c:1481-1482)
  mPcie.Ia[0x148 / 4] = 0x70000004;
  mPcie.Ia[0x14C / 4] = 0x10;

  // SQ10: IA_CNT clear (v0.c:1485-1486)
  mPcie.Ia[0x150 / 4] = 0x40030010;
  mPcie.Ia[0x154 / 4] = 0x1000B;

  // SQ11: EOP, return to idle (v0.c:1489-1490)
  mPcie.Ia[0x158 / 4] = 0x80000000;
  mPcie.Ia[0x15C / 4] = 0x0;

  // start (v0.c:1493)
  mPcie.Ia[PCIE_IA_ENABLE / 4] = 0x1;
}
