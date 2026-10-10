// UsbPhy.c — USBCON/PHY bring-up for the Exynos 9820 USB3 DRD, USB2-first.
//
// Sequence transcribed from two agreeing sources (notes/d1-usb-host.md Q1/Q3):
//   [K] drivers/phy/samsung/phy-exynos-usbdrd.c + phy-exynos-usb3p1.c
//       (UTMI init: q-channel WA, POR high, UTMI/HSP setup, tune, power,
//        POR low, LINK_PORT OCD select, pipe3 override)
//   [M] Mu-Silicium S5E9830Pkg PlatformUsbLib BoardInit.c — the same sequence
//       already proven in UEFI on the 9830 (same PMU offsets/bits). Only the
//       BLOCK BASES differ between 9830 and 9820; we use the 9820 bases from
//       <Platform/Exynos9820.h> exclusively.
//
// Delays are the kernel's (udelay(500)/100/10/75 -> gBS->Stall).
// phy_version 0x301 (DT) selects the MINOR >= 1 branch: PHY20/PHY30 split
// reset controls. use_io_for_ovc = 0 (DT) -> OCD bits SET.
// common_block_disable = 1 (DT) -> EN_UTMISUSPEND|COMMONONN set.
//
// NOT ported (recorded open items, notes/d1-usb-host.md):
//   - OTP calibration writes (samsung_exynos_cal_usb3phy_write_register) —
//     Mu's working 9830 UEFI path skips them too.
//   - SS (PIPE3) PHY enable — deliberately held down: pipe3 forced to
//     RX_ELEC_IDLE and PMA LOW_PWRN, so the first bring-up is HS-only.

#include "UsbHostInternal.h"

#define PHY0  EXYNOS_USBDRD_PHY_BASE   // USBCON control block, 0x200

STATIC
VOID
UsbPhyIsolation (IN BOOLEAN DeIsolate)
{
  UINT32  Register;

  Register = MmioRead32 (EXYNOS_PMU_BASE + PMU_USBDRD_ISO);
  if (DeIsolate) {
    Register |= PMU_USB_ISO_EN;
  } else {
    Register &= ~PMU_USB_ISO_EN;
  }

  MmioWrite32 (EXYNOS_PMU_BASE + PMU_USBDRD_ISO, Register);
}

STATIC
VOID
UsbPhyForceQChannel (VOID)
{
  UINT32  Register;

  // [K exynos_cal_usbphy_q_ch] / [M UsbPhyForceQChannel] — identical.
  Register  = MmioRead32 (PHY0 + USBCON_LINK_CTRL);
  Register |= LINKCTRL_DIS_QACT_ID0;
  Register |= LINKCTRL_DIS_QACT_VBUS_VALID;
  Register |= LINKCTRL_DIS_QACT_BVALID;
  Register |= LINKCTRL_DIS_QACT_LINKGATE;
  Register &= ~LINKCTRL_FORCE_QACT;
  gBS->Stall (500);
  MmioWrite32 (PHY0 + USBCON_LINK_CTRL, Register);
  gBS->Stall (500);

  Register  = MmioRead32 (PHY0 + USBCON_LINK_CTRL);
  Register |= LINKCTRL_FORCE_QACT;
  gBS->Stall (500);
  MmioWrite32 (PHY0 + USBCON_LINK_CTRL, Register);
}

STATIC
VOID
UsbPhySoftResetAssert (VOID)
{
  // [K phy_sw_rst_high, MINOR >= 1 branch] / [M] — PHY20 + PHY30 held in
  // reset, RST_SEL selecting the SW reset source.
  UINT32  Register;

  Register  = MmioRead32 (PHY0 + USBCON_CLKRST);
  Register |= CLKRST_PHY20_SW_RST | CLKRST_PHY20_RST_SEL;
  Register |= CLKRST_PHY30_SW_RST | CLKRST_PHY30_RST_SEL;
  MmioWrite32 (PHY0 + USBCON_CLKRST, Register);
}

STATIC
VOID
UsbPhySoftResetDeassert (VOID)
{
  // [K phy_sw_rst_low, MINOR >= 1 branch] / [M].
  UINT32  Register;

  Register  = MmioRead32 (PHY0 + USBCON_CLKRST);
  Register |= CLKRST_PHY20_RST_SEL;
  Register &= ~(CLKRST_PHY20_SW_RST | CLKRST_PHY30_SW_RST | CLKRST_PORT_RST);
  MmioWrite32 (PHY0 + USBCON_CLKRST, Register);
}

STATIC
VOID
UsbPhyPowerEnable (IN BOOLEAN Enable)
{
  // [M UsbPhyPowerEnable] — SIDDQ powers the HS PHY transceiver.
  UINT32  Register;

  Register = MmioRead32 (PHY0 + USBCON_HSP_TEST);
  if (Enable) {
    Register &= ~HSP_TEST_SIDDQ;
  } else {
    Register |= HSP_TEST_SIDDQ;
  }

  MmioWrite32 (PHY0 + USBCON_HSP_TEST, Register);
}

STATIC
VOID
UsbPhyTune (VOID)
{
  // [K phy_exynos_usb_v3p1_tune] — RMW of exactly the five fields the kernel
  // parser applies, HOST column of the board DT (see UsbHw.h).
  UINT32  Register;

  Register  = MmioRead32 (PHY0 + USBCON_HSP_TUNE);
  Register &= ~USBCON_HSP_TUNE_HOST_MASK;
  Register |= USBCON_HSP_TUNE_HOST_SET;
  MmioWrite32 (PHY0 + USBCON_HSP_TUNE, Register);
}

STATIC
VOID
UsbPhyPipe3Override (VOID)
{
  // [K phy_exynos_usb_v3p1_pipe_ovrd] — SS PHY stays electrically idle and
  // its PMA powered down: first bring-up is USB2/HS only.
  UINT32  Register;

  Register  = MmioRead32 (PHY0 + USBCON_LINK_CTRL);
  Register |= LINKCTRL_PIPE3_FORCE_EN;
  Register &= ~LINKCTRL_PIPE3_FORCE_PHY_STATUS;
  Register |= LINKCTRL_PIPE3_FORCE_RX_ELEC_IDLE;
  MmioWrite32 (PHY0 + USBCON_LINK_CTRL, Register);

  Register  = MmioRead32 (PHY0 + USBCON_COMBO_PMA_CTRL);
  Register |= PMA_LOW_PWRN;
  MmioWrite32 (PHY0 + USBCON_COMBO_PMA_CTRL, Register);
}

EFI_STATUS
UsbPhyInit (VOID)
{
  UINT32  Register;

  // 1. De-isolate the USB2 (UTMI) PHY half. The DP/PIPE3 half at +0x704 is
  //    left isolated — SS is not part of this bring-up.
  UsbPhyIsolation (TRUE);
  gBS->Stall (10);

  // 2. Clock audit ([M] trick): 0 or ~0 means the block has no clocks or is
  //    still isolated. The USB gates are ACPM-owned (notes/d1-usb-host.md
  //    Q1); we rely on the bootloader leaving them on and FAIL LOUDLY here
  //    if it did not — no CMU writes, no guessing.
  Register = MmioRead32 (PHY0 + USBCON_CTRL_VER);
  if ((Register == 0) || (Register == 0xFFFFFFFF)) {
    DEBUG ((DEBUG_ERROR, "%a: USBCON CTRL_VER = %x — clocks off/isolated, "
            "ACPM clock state must be solved first\n", __func__, Register));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "%a: USBCON CTRL_VER = %x\n", __func__, Register));

  // 3. The kernel's UTMI init order, verbatim.
  UsbPhyForceQChannel ();
  UsbPhySoftResetAssert ();

  Register  = MmioRead32 (PHY0 + USBCON_UTMI);
  Register &= ~(UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP);
  Register &= ~(UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN);
  MmioWrite32 (PHY0 + USBCON_UTMI, Register);

  Register  = MmioRead32 (PHY0 + USBCON_HSP);
  Register |= HSP_EN_UTMISUSPEND | HSP_COMMONONN;   // DT common_block_disable = 1
  MmioWrite32 (PHY0 + USBCON_HSP, Register);

  gBS->Stall (100);

  // VBUS-valid signalling: the SoC has NO vbus pad (is_not_vbus_pad = 1), so
  // the PHY must be told VBUS is valid. The kernel does this unconditionally
  // in UTMI init — including on boards that act as host (DeX) — so we keep
  // it; the earlier "skip in host mode" note was inference, this is the
  // kernel's fact (phy-exynos-usb3p1.c:440-455).
  Register  = MmioRead32 (PHY0 + USBCON_LINK_CTRL);
  Register |= LINKCTRL_BUS_FILTER_BYPASS (0xF);     // [K link_vbus_filter_en(false)]
  MmioWrite32 (PHY0 + USBCON_LINK_CTRL, Register);

  Register  = MmioRead32 (PHY0 + USBCON_UTMI);
  Register |= UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID;
  MmioWrite32 (PHY0 + USBCON_UTMI, Register);

  Register  = MmioRead32 (PHY0 + USBCON_HSP);
  Register |= HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT;
  MmioWrite32 (PHY0 + USBCON_HSP, Register);

  UsbPhyTune ();
  UsbPhyPowerEnable (TRUE);
  gBS->Stall (10);
  UsbPhySoftResetDeassert ();
  gBS->Stall (75);

  UsbPhyPipe3Override ();

  // 4. OVC not on IO (DT use_io_for_ovc = 0) -> OCD port select SET
  //    ([K phy_exynos_usb_v3p1_enable tail] / [M]).
  Register  = MmioRead32 (PHY0 + USBCON_LINK_PORT);
  Register |= LINKPORT_HUB_PORT_SEL_OCD_U3 | LINKPORT_HUB_PORT_SEL_OCD_U2;
  MmioWrite32 (PHY0 + USBCON_LINK_PORT, Register);

  return EFI_SUCCESS;
}
