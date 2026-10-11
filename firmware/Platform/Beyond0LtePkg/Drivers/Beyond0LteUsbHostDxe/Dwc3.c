// Dwc3.c — DWC3 core init, HOST path, for the Exynos 9820 USB3 DRD.
//
// Mirrors the downstream kernel's dwc3_core_init() HOST branch
// (drivers/usb/dwc3/core.c, LineageOS exynos9820 tree; step-by-step in
// notes/d1-usb-host.md Q2). The board's DT quirk set is fixed and recorded
// there: snps,quirk-frame-length-adjustment = 0x20, snps,u1u2_exitfail_quirk,
// snps,ux_exit_in_px_quirk, snps,elastic_buf_mode_quirk, snps,has-lpm-erratum,
// snps,usb3_lpm_capable, adj-sof-accuracy = 1, snps,dis-u2-freeclk-exists-quirk,
// enable_sprs_transfer = 1.
//
// The core REVISION (GSNPSID) is UNVERIFIED on this SoC until read on
// device, so every revision-gated branch below is decided at RUNTIME from
// the GSNPSID read — exactly like the kernel — never from a guessed constant.
//
// Deliberately NOT done (kernel-faithful):
//   - DCTL CSFTRST: in HOST mode the kernel returns before touching DCTL;
//     xHCI's own HCRESET (issued by XhciDxe) resets the host block.
//   - scratch buffers: DT tx-fifo-resize = 0.
//   - GUCTL1/GUCTL3: their DT quirk inputs are absent / DWC_usb31-only.

#include "UsbHostInternal.h"

#define DWC3  EXYNOS_USBDRD_BASE   // 0x10C00000: xHCI regs 0..0x7FFF, globals 0xC100+

// Board DT: snps,quirk-frame-length-adjustment = <0x20>.
#define FLADJ_30MHZ_BOARD  0x20

STATIC
VOID
Dwc3EventBufferSetup (VOID)
{
  EFI_STATUS            Status;
  VOID                  *Buffer;
  UINTN                 BufferSize;
  EFI_PHYSICAL_ADDRESS  Address;

  // [K dwc3_alloc_event_buffers + dwc3_event_buffers_setup] — the kernel
  // skips this only when GHWPARAMS0 says HOST-only; our core is DRD, so the
  // buffer is set up. 4 KiB (DWC3_EVENT_BUFFERS_SIZE), page-aligned like the
  // kernel's dma_alloc_coherent, GEVNTCOUNT reset. The buffer is never
  // freed: the controller keeps writing events into it while xHCI runs.
  if ((MmioRead32 (DWC3 + DWC3_GHWPARAMS0) & 0x3) == 1) {
    return;   // GHWPARAMS0_MODE_HOST — kernel skips
  }

  // EfiReservedMemoryType, not BootServicesData: the controller keeps this address after
  // ExitBootServices, and Windows would otherwise reuse the page under its DMA. Below 4 GiB because the
  // high banks are published RAM now (AllocateAnyPages could hand out an address GEVNTADRHI = 0 would
  // truncate); HI is still written from the real address.
  BufferSize = DWC3_EVENT_BUFFERS_SIZE;
  Address    = 0xFFFFFFFFULL;
  Status     = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType,
                                   EFI_SIZE_TO_PAGES (BufferSize), &Address);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: event buffer alloc %r — continuing without\n",
            __func__, Status));
    return;
  }

  Buffer = (VOID *)(UINTN)Address;
  ZeroMem (Buffer, BufferSize);
  MmioWrite32 (DWC3 + DWC3_GEVNTADRLO (0), (UINT32)Address);
  MmioWrite32 (DWC3 + DWC3_GEVNTADRHI (0), (UINT32)(Address >> 32));
  MmioWrite32 (DWC3 + DWC3_GEVNTSIZ (0), GEVNTSIZ_SIZE (BufferSize));
  MmioWrite32 (DWC3 + DWC3_GEVNTCOUNT (0), 0);
}

STATIC
VOID
Dwc3RootHubPowerOff (VOID)
{
  // [Xilinx linux-xlnx drivers/usb/dwc3/host.c] — after switching PRTCAP to
  // host, clear PORTSC.POWER on every root port BEFORE xHCI's own reset, so
  // no VBUS glitch happens ahead of our explicit MAX77705 boost decision.
  UINT32  HcParams;
  UINT32  PortCount;
  UINT32  PortScBase;
  UINT32  Index;
  UINT32  Register;

  HcParams  = MmioRead32 (DWC3 + XHCI_HCSPARAMS1);
  PortCount = HCSPARAMS1_MAXPORTS (HcParams);
  if ((PortCount == 0) || (PortCount > 64)) {
    DEBUG ((DEBUG_WARN, "%a: HCSPARAMS1.MAXPORTS = %u — skipping\n",
            __func__, PortCount));
    return;
  }

  // PORTSC(n) = CAPLENGTH (byte read at 0x0) + 0x400 + 0x10*n  [Xilinx host.c]
  // Run 2 crash (notes/m16-usb.md): this was a 32-bit read, which adds HCIVERSION (bits 31:16) into the
  // offset and sends the PORTSC accesses ~17 MiB past the xHCI window -> synchronous abort. CAPLENGTH is
  // the low byte only, and every PORTSC must stay inside the 0x8000 xHCI window.
  PortScBase = (MmioRead32 (DWC3) & 0xFF) + XHCI_PORTSC_BASE;   // 32-bit read + mask, like Linux HC_LENGTH()
  if ((PortScBase + PortCount * 0x10) > 0x8000) {
    DEBUG ((DEBUG_ERROR, "%a: CAPLENGTH %x MAXPORTS %u outside the xHCI window - skipping\n",
            __func__, PortScBase - XHCI_PORTSC_BASE, PortCount));
    return;
  }

  DEBUG ((DEBUG_INFO, "%a: CAPLENGTH %x, %u root ports powered off\n", __func__,
          PortScBase - XHCI_PORTSC_BASE, PortCount));
  for (Index = 0; Index < PortCount; Index++) {
    Register  = MmioRead32 (DWC3 + PortScBase + (Index * 0x10));
    Register &= ~PORTSC_POWER;
    MmioWrite32 (DWC3 + PortScBase + (Index * 0x10), Register);
  }
}

EFI_STATUS
Dwc3CoreInit (IN BOOLEAN Device, OUT UINT32 *Revision)
{
  UINT32  Rev;
  UINT32  Reg;
  BOOLEAN AdjSofAccuracy;

  // 0. Identity. [K dwc3_core_is_valid] — 0x5533 = DWC_usb3 (full GSNPSID is
  //    the revision encoding); 0x3331 = DWC_usb31: the revision is VER_NUMBER
  //    with DWC3_REVISION_IS_DWC31 OR-ed in, so every ">= usb3 rev" branch below
  //    is taken and every "<= old usb3 rev" workaround skipped, exactly as the
  //    4.14 kernel compares. Run 1 (notes/m16-usb.md): GSNPSID 0x33313130 = DWC_usb31.
  Reg = MmioRead32 (DWC3 + DWC3_GSNPSID);
  if ((Reg & GSNPSID_ID_MASK) == GSNPSID_ID_DWC3) {
    Rev = Reg;
    DEBUG ((DEBUG_INFO, "%a: DWC_usb3 revision %x\n", __func__, Rev));
  } else if ((Reg & GSNPSID_ID_MASK) == GSNPSID_ID_DWC31) {
    Rev = MmioRead32 (DWC3 + DWC3_VER_NUMBER) | DWC3_REVISION_IS_DWC31;
    DEBUG ((DEBUG_INFO, "%a: DWC_usb31 GSNPSID %x VER_NUMBER %x\n", __func__, Reg, Rev & ~DWC3_REVISION_IS_DWC31));
    if (Rev >= DWC31_REV_170A) {
      // [K] GUCTL3 USB20_RETRY_DISABLE applies here; its bit is not in our
      // notes, so it is NOT written (UNVERIFIED) - record it if this fires.
      DEBUG ((DEBUG_WARN, "%a: DWC_usb31 >= 1.70a: GUCTL3 USB20_RETRY_DISABLE quirk NOT applied (UNVERIFIED bit)\n", __func__));
    }
  } else {
    DEBUG ((DEBUG_ERROR, "%a: GSNPSID = %x — not DWC_usb3/usb31\n", __func__, Reg));
    return EFI_DEVICE_ERROR;
  }

  // adj-sof-accuracy is DT = 1 but the kernel zeroes it below 2.50a
  // [K core.c:1082-1084].
  AdjSofAccuracy = (BOOLEAN)(Rev >= DWC3_REV_250A);

  MmioWrite32 (DWC3 + DWC3_GUID, 0x60000000u);  // [K] LINUX_VERSION_CODE write — cosmetic

  // 1. AHB burst config. [K dwc3_core_config]
  Reg  = MmioRead32 (DWC3 + DWC3_GSBUSCFG0);
  Reg |= GSBUSCFG0_INCRBRSTEN | GSBUSCFG0_INCR16BRSTEN;
  if (Rev >= DWC3_REV_220A) {
    Reg |= (2 << 16) | (2 << 20) | (2 << 24) | (2 << 28);  // cacheable/modifiable req info
  }

  MmioWrite32 (DWC3 + DWC3_GSBUSCFG0, Reg);

  // [K] plain assignment, not RMW — the kernel overwrites the whole register.
  MmioWrite32 (DWC3 + DWC3_GSBUSCFG1, GSBUSCFG1_BREQLIMIT (8));

  // 2. GUCTL: retry + refclk period + sparse-transfer (all DT-selected).
  Reg  = MmioRead32 (DWC3 + DWC3_GUCTL);
  Reg |= GUCTL_USBHSTINAUTORETRYEN;
  // DT snps,dis-u2-freeclk-exists-quirk (and adj-sof-accuracy >= 2.50a) both
  // select the 125 us ITP fix. [K core.c:278-287]
  Reg &= ~GUCTL_REFCLKPER_MASK;
  Reg |= GUCTL_REFCLKPER (0xF);
  Reg |= GUCTL_SPRSCTRLTRANSEN;     // DT enable_sprs_transfer = 1
  MmioWrite32 (DWC3 + DWC3_GUCTL, Reg);

  // 3. RX threshold WA, 1.90a..2.10a. [K core.c:291-299]
  if ((Rev >= DWC3_REV_190A) && (Rev <= DWC3_REV_210A)) {
    Reg  = MmioRead32 (DWC3 + DWC3_GRXTHRCFG);
    Reg &= ~(GRXTHRCFG_USBRXPKTCNT (0xF) | GRXTHRCFG_USBMAXRXBURSTSIZE (0x1F));
    Reg |= GRXTHRCFG_RXPKTCNTSEL | GRXTHRCFG_USBRXPKTCNT (3) |
           GRXTHRCFG_USBMAXRXBURSTSIZE (3);
    MmioWrite32 (DWC3 + DWC3_GRXTHRCFG, Reg);
  }

  // 4. GUSB3PIPECTL: SS PHY is held down, but the kernel still programs this
  //    register on the HOST path; mirror it.
  Reg = MmioRead32 (DWC3 + DWC3_GUSB3PIPECTL (0));
  if (Rev <= DWC3_REV_210A) {
    Reg &= ~(7 << 19);
    Reg |= GUSB3PIPECTL_DEP1P2P3_EN;            // [K] LFPS entry WA
  }

  if (Rev >= DWC3_REV_250A) {
    Reg |= GUSB3PIPECTL_DISRXDETINP3;           // [K]
  }

  Reg |= GUSB3PIPECTL_UX_EXIT_PX;               // DT snps,ux_exit_in_px_quirk
  Reg |= GUSB3PIPECTL_ELASTIC_BUFFER_MODE;      // DT snps,elastic_buf_mode_quirk
  Reg |= GUSB3PIPECTL_U1U2EXITFAIL_TO_RECOV;    // DT snps,u1u2_exitfail_quirk
  if (Rev > DWC3_REV_194A) {
    Reg |= GUSB3PIPECTL_SUSPHY;                 // [K dwc3_phy_setup]
  }

  MmioWrite32 (DWC3 + DWC3_GUSB3PIPECTL (0), Reg);

  // 5. GUSB2PHYCFG.
  Reg = MmioRead32 (DWC3 + DWC3_GUSB2PHYCFG (0));
  if (Rev > DWC3_REV_194A) {
    Reg |= GUSB2PHYCFG_SUSPHY;                  // [K]
  }

  Reg |= GUSB2PHYCFG_ENBLSLPM;                  // [K] no dis_enblslpm quirk in DT -> SET
  Reg &= ~GUSB2PHYCFG_U2_FREECLK_EXISTS;        // [K] both freeclk and adj-sof clear it
  if (Rev <= DWC3_REV_210A) {
    // [K core.c:377-381] PORTSC race WA forces 16-bit UTMI; Mu's 9830 lib
    // also declares UtmiWidth16Bit. Above 2.10a the DT sets no phy-mode, so
    // the kernel leaves the width at the core default — we do the same.
    Reg &= ~(GUSB2PHYCFG_PHYIF (1) | (0xF << 10));
    Reg |= GUSB2PHYCFG_PHYIF (1) | (5 << 10);   // 16-bit, USBTRDTIM_UTMI_16_BIT = 5
  }

  MmioWrite32 (DWC3 + DWC3_GUSB2PHYCFG (0), Reg);

  // 6. GCTL: clock-gating/SOFITPSYNC decision + role switch to HOST.
  //    [K dwc3_core_setup_global_control + dwc3_core_init_mode]
  Reg = MmioRead32 (DWC3 + DWC3_GCTL);
  switch (GHWPARAMS1_EN_PWROPT (MmioRead32 (DWC3 + DWC3_GHWPARAMS1))) {
    case GHWPARAMS1_EN_PWROPT_CLK:
      // STAR#9000588375: host/otg, 2.10a..2.50a -> disable clock gating +
      // SOFITPSYNC; otherwise leave gating on.
      if ((Rev >= DWC3_REV_210A) && (Rev <= DWC3_REV_250A)) {
        Reg |= GCTL_DSBLCLKGTNG | GCTL_SOFITPSYNC;
      } else {
        Reg &= ~GCTL_DSBLCLKGTNG;
      }

      break;
    case GHWPARAMS1_EN_PWROPT_HIB:
      Reg |= GCTL_GBLHIBERNATIONEN;
      break;
    default:
      break;
  }

  Reg &= ~GCTL_DISSCRAMBLE;                     // [K] FPGA-only quirk, off
  // DT has dis-u2-freeclk-exists-quirk -> SET SOFITPSYNC ... then
  // adj-sof-accuracy (>= 2.50a) CLEARS it again. [K core.c:1038-1043]
  Reg |= GCTL_SOFITPSYNC;
  if (AdjSofAccuracy) {
    Reg &= ~GCTL_SOFITPSYNC;
  }

  Reg = (Reg & ~(3 << 12)) | GCTL_PRTCAPDIR (Device ? GCTL_PRTCAP_DEVICE : GCTL_PRTCAP_HOST);
  MmioWrite32 (DWC3 + DWC3_GCTL, Reg);

  if (Device) {
    // M17 (notes/m17-usb-kd.md): Windows' kd_8003_5143 (Synopsys USB-function KDNET) owns the device
    // side from here - it programs DCFG/DCTL, its own event buffer and endpoints. Leave the core
    // attached-but-stopped (download mode is device mode and may have left RUN_STOP set), with no UEFI
    // event buffer or xHCI root-port writes behind it.
    Reg = MmioRead32 (DWC3 + DWC3_DCTL);
    MmioWrite32 (DWC3 + DWC3_DCTL, Reg & ~DCTL_RUN_STOP);
    DEBUG ((DEBUG_INFO, "%a: DEVICE role, DCTL %x -> %x, DSTS %x\n", __func__, Reg,
            MmioRead32 (DWC3 + DWC3_DCTL), MmioRead32 (DWC3 + DWC3_DSTS)));
    *Revision = Rev;
    return EFI_SUCCESS;
  }

  // 7. Frame length adjustment. [K dwc3_frame_length_adjustment]
  Reg  = MmioRead32 (DWC3 + DWC3_GFLADJ);
  Reg &= ~GFLADJ_30MHZ_MASK;
  Reg |= GFLADJ_30MHZ_SDBND_SEL | FLADJ_30MHZ_BOARD;
  MmioWrite32 (DWC3 + DWC3_GFLADJ, Reg);

  // 8. Event buffer (step order: kernel does it after num_eps, before
  //    revision quirks — position among the register writes is not
  //    order-sensitive; kept after GFLADJ for readability).
  Dwc3EventBufferSetup ();

  // 9. Final GFLADJ block. [K core.c:1185-1197]
  Reg = MmioRead32 (DWC3 + DWC3_GFLADJ);
  if (AdjSofAccuracy) {
    Reg &= ~BIT31;                              // REFCLK_240MHZDECR_PLS1
    Reg &= ~(0x7F << 24);
    Reg |= GFLADJ_REFCLK_240MHZ_DECR (0xA);
    Reg |= GFLADJ_REFCLK_LPM_SEL;
    Reg &= ~(0x3FFF << 8);
    Reg |= GFLADJ_REFCLK_FLADJ (0x7F0);
  } else {
    // dis_u2_freeclk_exists_quirk branch: clear LPM_SEL. [K]
    Reg &= ~GFLADJ_REFCLK_LPM_SEL;
  }

  MmioWrite32 (DWC3 + DWC3_GFLADJ, Reg);

  // 10. Root-hub PORTSC power-off before xHCI takes over (Xilinx WA).
  Dwc3RootHubPowerOff ();

  *Revision = Rev;
  return EFI_SUCCESS;
}
