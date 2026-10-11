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

STATIC
VOID
UsbPhyDump (IN CONST CHAR8 *Label)
{
  DEBUG ((DEBUG_INFO, "UsbPhy %a: LINKCTRL %x LINKPORT %x CLKRST %x UTMI %x HSP %x TUNE %x TEST %x REWA_CTRL %x\n", Label,
          MmioRead32 (PHY0 + USBCON_LINK_CTRL), MmioRead32 (PHY0 + USBCON_LINK_PORT), MmioRead32 (PHY0 + USBCON_CLKRST),
          MmioRead32 (PHY0 + USBCON_UTMI), MmioRead32 (PHY0 + USBCON_HSP), MmioRead32 (PHY0 + USBCON_HSP_TUNE),
          MmioRead32 (PHY0 + USBCON_HSP_TEST), MmioRead32 (PHY0 + USBCON_HSREWA_CTRL)));
}

//
// M17 (notes/m17-usb-kd.md): UEFI device mode attached internally but the host never saw the pull-up. Linux, in
// device mode on the same cable, has these PHY-wrapper bits that UsbPhyInit never writes. Applied in the KD
// device path only (host mode unchanged until this is proven); the before/after dump goes to UEFILOG.
//
VOID
UsbPhyDeviceModeDeltas (VOID)
{
  UsbPhyDump ("before");
  MmioWrite32 (PHY0 + USBCON_HSREWA_CTRL, MmioRead32 (PHY0 + USBCON_HSREWA_CTRL) | HSREWA_DIG_BYPASS_CON_EN);
  MmioWrite32 (PHY0 + USBCON_HSP, MmioRead32 (PHY0 + USBCON_HSP) | HSP_RETENABLE_EN);
  MmioWrite32 (PHY0 + USBCON_HSP_TUNE, USBCON_HSP_TUNE_DEVICE);
  // NOT CLKRST LINK_PCLK_SEL: Linux has it set because its SS PHY runs; with ours held down it moved the link to a
  // dead clock - boot #75 link never left Disconnected, the core would not halt and Windows' kd module hung.
  UsbPhyDump ("after ");
}

//
// M16 (notes/m16-usb.md, TWRP host-mode capture hostlog-dock.txt): Linux in HOST role runs the PHY wrapper with the
// UTMI D+/D- 15k pull-downs forced on (UTMICTRL 0x0C) and WITHOUT the device-side VBUSVALID/BVALID forcing
// (UTMICTRL bits 5:4 = 0, HSPCTRL VBUSVLDEXTSEL/EXT = 0). UsbPhyInit sets the device-side values for both roles; a
// host without its pull-downs leaves the lines floating - UEFI saw one connect change then a disconnect.
//
VOID
UsbPhyHostModeDeltas (VOID)
{
  UsbPhyDump ("host before");
  MmioWrite32 (PHY0 + USBCON_UTMI,
               (MmioRead32 (PHY0 + USBCON_UTMI) & ~(UTMI_FORCE_VBUSVALID | UTMI_FORCE_BVALID)) |
               UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN);
  MmioWrite32 (PHY0 + USBCON_HSP, MmioRead32 (PHY0 + USBCON_HSP) & ~(HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT));
  UsbPhyDump ("host after ");
}

//
// M16 SuperSpeed (notes/m16-ss-phy.md): the kernel's pipe3_init order (phy-exynos-usbdrd.c:1194-1239) -
// PMU 0x704 de-isolate -> g2_pma_ready (USBCON 0x48 / LINK_CTRL) -> usbdp_g2_enable (combo PMA + PCS table,
// lane mux by orientation) -> g2_pma_sw_rst_release -> LC PLL lock -> run_lane -> CDR lock -> LINK_PCLK_SEL.
// LINK_PCLK_SEL hands the link's pipe clock to the PMA, so it is set ONLY after both locks (boot #75 set it with
// the PMA down and the link died). Any timeout tears the SS side back down (g2_disable order) and leaves USB2.
//
#include "UsbSsPhyTable.h"

#define USB_SS_PMU_ISO           0x704     // [K] pmu_offset_dp, bit0 = de-isolated
#define USB_SS_PMA_LCPLL         0x0350    // [K gen2.c:392] CMN_A0350 bit7 ANA_LCPLL_LOCK_DONE
#define USB_SS_PMA_CDR_PORT0     0x0B84    // [K gen2.c:47] LN0 RX CDR lock, bit0
#define USB_SS_PMA_CDR_PORT1     0x1B84    // [K] LN2 RX CDR lock, bit0
#define USB_SS_LOCK_POLL_US      100000    // [K] 100 outer x 1000 x 1 us
// Orientation: Linux reads GPIO gpg0-4 ("PHY_CON") as used_phy_port (usbdrd.c:1157-1192). gpg0 = 5th bank of
// pinctrl@10430000 -> DAT at +0x84 by the Samsung 0x20 bank stride. UNVERIFIED address (notes/m16-ss-phy.md §4):
// only read and logged; a CDR timeout on that port retries the other one.
#define USB_SS_GPG0_DAT_UNVERIFIED  0x10430084ULL
#define USB_SS_GPG0_PHY_CON_BIT     BIT4

// Run 15: the LC PLL never locks yet (0x350 stops at 0x3E/0x38 - calibration done, no lock), but running the
// sequence and then the g2_disable teardown is what made USB3-capable devices fall back to USB2 and enumerate
// (the isolated / half-reset combo PHY blocked them). 0 = PARK: run the sequence once (detected orientation) and
// tear down without the 2 x 100 ms lock polls - the state USB2 needs. 1 = try for a real SuperSpeed lock.
#define USB_SS_LOCK_ATTEMPT  0

STATIC
VOID
UsbSsApply (IN CONST USB_SS_PHY_WRITE *Table, IN UINTN Count)
{
  UINTN   Index;
  UINTN   Address;

  for (Index = 0; Index < Count; Index++) {
    Address = (UINTN)((Table[Index].Block == 0) ? EXYNOS_USB_COMBO_PHY_BASE : EXYNOS_USB_PCS_BASE) + Table[Index].Offset;
    if (Table[Index].Keep == 0) {
      MmioWrite32 (Address, Table[Index].Value);
    } else {
      MmioWrite32 (Address, (MmioRead32 (Address) & Table[Index].Keep) | Table[Index].Value);
    }
  }
}

STATIC
BOOLEAN
UsbSsPollBit (IN UINTN Address, IN UINT32 Bit, OUT UINTN *WaitedUs)
{
  UINTN  Us;

  for (Us = 0; Us < USB_SS_LOCK_POLL_US; Us += 10) {
    if ((MmioRead32 (Address) & Bit) != 0) {
      *WaitedUs = Us;
      return TRUE;
    }

    gBS->Stall (10);
  }

  *WaitedUs = Us;
  return FALSE;
}

STATIC
VOID
UsbSsTeardown (VOID)
{
  // g2_disable (usb3p1.c:258-283): PCLK_SEL off FIRST, then hold the PMA in reset/low power; then the USB2-only
  // PIPE3 override we used before (link sees an idle, absent SS PHY).
  MmioWrite32 (PHY0 + USBCON_CLKRST, MmioRead32 (PHY0 + USBCON_CLKRST) & ~CLKRST_LINK_PCLK_SEL);
  MmioWrite32 (PHY0 + USBCON_COMBO_PMA_CTRL, MmioRead32 (PHY0 + USBCON_COMBO_PMA_CTRL) |
               PMA_LOW_PWRN | PMA_TRSV_SW_RST | PMA_CMN_SW_RST | PMA_INIT_SW_RST | PMA_APB_SW_RST);
  gBS->Stall (1);
  MmioWrite32 (PHY0 + USBCON_COMBO_PMA_CTRL, MmioRead32 (PHY0 + USBCON_COMBO_PMA_CTRL) &
               ~(PMA_TRSV_SW_RST | PMA_CMN_SW_RST | PMA_INIT_SW_RST | PMA_APB_SW_RST));
  MmioWrite32 (PHY0 + USBCON_LINK_CTRL, MmioRead32 (PHY0 + USBCON_LINK_CTRL) | LINKCTRL_PIPE3_FORCE_EN);
}

STATIC
BOOLEAN
UsbSsTryPort (IN UINTN Port)
{
  UINT32  Reg;
  UINTN   Waited;
  BOOLEAN Locked;

  // g2_pma_ready (usb3p1.c:221-256).
  gBS->Stall (100);
  Reg  = MmioRead32 (PHY0 + USBCON_COMBO_PMA_CTRL);
  Reg &= ~((3u << 12) | (3u << 10));
  Reg |= PMA_REF_FREQ_SET (1) | PMA_LOW_PWRN | PMA_TRSV_SW_RST | PMA_CMN_SW_RST | PMA_INIT_SW_RST | PMA_APB_SW_RST;
  MmioWrite32 (PHY0 + USBCON_COMBO_PMA_CTRL, Reg);
  gBS->Stall (1);
  MmioWrite32 (PHY0 + USBCON_COMBO_PMA_CTRL, MmioRead32 (PHY0 + USBCON_COMBO_PMA_CTRL) & ~PMA_LOW_PWRN);
  gBS->Stall (1);
  MmioWrite32 (PHY0 + USBCON_LINK_CTRL, MmioRead32 (PHY0 + USBCON_LINK_CTRL) & ~LINKCTRL_PIPE3_FORCE_EN);
  gBS->Stall (1);
  MmioWrite32 (PHY0 + USBCON_COMBO_PMA_CTRL, MmioRead32 (PHY0 + USBCON_COMBO_PMA_CTRL) & ~PMA_APB_SW_RST);

  // First PMA access = one read (bus-hang canary; the 0x67 progress guard covers a hang).
  DEBUG ((DEBUG_INFO, "UsbSs: port %u PMA_CTRL %x, PMA+0x350 %x\n", Port, MmioRead32 (PHY0 + USBCON_COMBO_PMA_CTRL),
          MmioRead32 ((UINTN)EXYNOS_USB_COMBO_PHY_BASE + USB_SS_PMA_LCPLL)));

  // usbdp_g2_enable.
  UsbSsApply (mUsbSsPhyEnable, ARRAY_SIZE (mUsbSsPhyEnable));
  if (Port == 0) {
    UsbSsApply (mUsbSsPhyLaneMuxPort0, ARRAY_SIZE (mUsbSsPhyLaneMuxPort0));
  } else {
    UsbSsApply (mUsbSsPhyLaneMuxPort1, ARRAY_SIZE (mUsbSsPhyLaneMuxPort1));
  }

  // g2_pma_sw_rst_release (usb3p1.c:304-324).
  MmioWrite32 (PHY0 + USBCON_COMBO_PMA_CTRL, MmioRead32 (PHY0 + USBCON_COMBO_PMA_CTRL) & ~PMA_INIT_SW_RST);
  gBS->Stall (1);
  MmioWrite32 (PHY0 + USBCON_COMBO_PMA_CTRL, MmioRead32 (PHY0 + USBCON_COMBO_PMA_CTRL) &
               ~(PMA_TRSV_SW_RST | PMA_CMN_SW_RST));
  gBS->Stall (1000);

  if (!USB_SS_LOCK_ATTEMPT) {
    DEBUG ((DEBUG_INFO, "UsbSs: port %u sequence run, parking (no lock attempt), 0x350 = %x\n", Port,
            MmioRead32 ((UINTN)EXYNOS_USB_COMBO_PHY_BASE + USB_SS_PMA_LCPLL)));
    return FALSE;
  }

  Locked = UsbSsPollBit ((UINTN)EXYNOS_USB_COMBO_PHY_BASE + USB_SS_PMA_LCPLL, BIT7, &Waited);
  DEBUG ((DEBUG_INFO, "UsbSs: port %u LC PLL %a after %u us (0x350 = %x)\n", Port, Locked ? "LOCKED" : "TIMEOUT",
          Waited, MmioRead32 ((UINTN)EXYNOS_USB_COMBO_PHY_BASE + USB_SS_PMA_LCPLL)));
  if (!Locked) {
    return FALSE;
  }

  UsbSsApply (mUsbSsPhyRunLane, ARRAY_SIZE (mUsbSsPhyRunLane));
  Locked = UsbSsPollBit ((UINTN)EXYNOS_USB_COMBO_PHY_BASE + ((Port == 0) ? USB_SS_PMA_CDR_PORT0 : USB_SS_PMA_CDR_PORT1),
                         BIT0, &Waited);
  DEBUG ((DEBUG_INFO, "UsbSs: port %u CDR %a after %u us\n", Port, Locked ? "LOCKED" : "TIMEOUT", Waited));
  return Locked;
}

EFI_STATUS
UsbPhySsInit (VOID)
{
  UINT32  Gpio;
  UINTN   Port;
  UINTN   Try;

  MmioWrite32 (EXYNOS_PMU_BASE + USB_SS_PMU_ISO, MmioRead32 (EXYNOS_PMU_BASE + USB_SS_PMU_ISO) | BIT0);
  Gpio = MmioRead32 (USB_SS_GPG0_DAT_UNVERIFIED);
  Port = ((Gpio & USB_SS_GPG0_PHY_CON_BIT) != 0) ? 1 : 0;
  DEBUG ((DEBUG_INFO, "UsbSs: PMU 0x704 = %x, gpg0 DAT(UNVERIFIED) = %x -> used_phy_port %u\n",
          MmioRead32 (EXYNOS_PMU_BASE + USB_SS_PMU_ISO), Gpio, Port));

  for (Try = 0; Try < (USB_SS_LOCK_ATTEMPT ? 2u : 1u); Try++, Port ^= 1) {
    if (UsbSsTryPort (Port)) {
      MmioWrite32 (PHY0 + USBCON_CLKRST, MmioRead32 (PHY0 + USBCON_CLKRST) | CLKRST_LINK_PCLK_SEL);
      gBS->Stall (3000);   // [K g2_link_pclk_sel: mdelay(3)]
      UsbPhyDump ("SS up");
      return EFI_SUCCESS;
    }

    UsbSsTeardown ();
  }

  DEBUG ((USB_SS_LOCK_ATTEMPT ? DEBUG_WARN : DEBUG_INFO, "UsbSs: %a - USB2-only, combo PHY parked\n",
          USB_SS_LOCK_ATTEMPT ? "no lock on either port" : "SS lock attempt disabled"));
  UsbPhyDump ("SS down");
  return EFI_NOT_READY;
}
