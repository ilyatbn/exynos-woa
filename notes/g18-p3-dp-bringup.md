# G18 prep P3 - Exynos 9820 DisplayPort register-level bring-up (vendor source study)

Input for a future Windows DP driver (rungs H3/H4 of notes/g18-hdmi-dp-alt.md). Source: LineageOS
android_kernel_samsung_exynos9820 @ lineage-23.2 (raw.githubusercontent.com), fetched 2026-10-20 to the session
scratchpad (not the repo). Prior facts NOT re-derived here: notes/d6-external-display.md P2/P3/P3b,
notes/g18-hdmi-dp-alt.md, notes/g18-hdmi-prep.md.

Tags: every constant is **UNVERIFIED (vendor source)** unless it is #defined in
firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h (checked 2026-10-20: only EXYNOS_PMU_BASE 0x15860000,
EXYNOS_USB_COMBO_PHY_BASE 0x10AE0000, EXYNOS_DECON_T_BASE/SIZE 0x19050000/0x10000, EXYNOS_DECON_F_BASE,
EXYNOS_DPU_DMA_G0_BASE 0x19071000 are there; **no DP controller 0x130B0000, no PMU 0x704 offset**). INFERRED = my
reasoning beyond the source. File refs are `file:line` in the fetched copies (lineage-23.2).

No device access, no source changes in this task.

## Progress log
- 2026-10-20 file created; prior notes read (d6 P1-P3b, g18-hdmi-dp-alt, g18-hdmi-prep).
- 2026-10-20 fetched (scratchpad `k/`): displayport_drv.c, cal_9820/displayport_reg.c, regs-displayport.h,
  displayport.h, decon_displayport.c, cal_9820/decon_reg.c, regs-decon.h, secdp_aux_control.c, displayport_edid.c,
  phy-exynos-usbdp-gen2.c/.h/-reg.h, phy-exynos-usb3p1.c, phy-exynos-usbdrd.c/.h, dt-bindings/clock/exynos9820.h,
  clk/samsung/clk-exynos9820.c, exynos9820-pm-domains.dtsi. Already in scratchpad from earlier sessions: cmucal-sfr.c,
  cmucal-node.c, cmucal-vclk.c, exynos9820.dts, exynos9820-beyond0lte_common.dtsi.
- 2026-10-20 section 1 (power/clock) written from drv.c + clk/cmucal/pm-domains. Cross-read notes/m16-ss-phy.md
  (our firmware's own combo-PHY bring-up: LC PLL never locked, PHY parked) - it matters for DP, see 1.4.

## 1. Power / clock prerequisites before touching 0x130B0000

### 1.1 Where the DP link block actually lives (NEW, corrects an assumption)
- 0x130B0000 is **not** in the DPU block (DPU = 0x19xxxxxx, CMU_DPU 0x19000000, cmucal-sfr.c:24). The DP link
  is an IP of **BLK_FSYS0** (CMU_FSYS0 0x13000000 +0x8000, cmucal-sfr.c:26; also in Exynos9820.h:383 comment):
  - `CLK_CON_GAT_GOUT_BLK_FSYS0_UID_DP_LINK_IPCLKPORT_I_PCLK` = CMU_FSYS0+0x2014 (cmucal-sfr.c:1084)
  - `..._DP_LINK_IPCLKPORT_I_DP_GTC_CLK` = CMU_FSYS0+0x2010 (cmucal-sfr.c:1086)
  - gate bit fields: CG_VAL b21, MANUAL b20, ENABLE_AUTOMATIC_CLKGATING b28 (cmucal-sfr.c:5575-5583)
  - `QCH_CON_DP_LINK_QCH` = CMU_FSYS0+0x3018, `QCH_CON_DP_LINK_QCH_GTC` = +0x301C (cmucal-sfr.c:2211-2212);
    fields ENABLE b0, CLOCK_REQ b1, IGNORE_FORCE_PM_EN b2, EXPIRE_VAL [25:16] (cmucal-sfr.c:9226-9233)
  - parents: PCLK <- MUX_CLKCMU_FSYS0_BUS_USER (cmucal-node.c:1978), GTC <- MUX_CLKCMU_FSYS0_DPGTC_USER
    (cmucal-node.c:1980). USER muxes: PLL_CON0_MUX_CLKCMU_FSYS0_BUS_USER CMU_FSYS0+0x120,
    ..._DPGTC_USER +0x140 (cmucal-sfr.c:219-222). TOP side: CLK_CON_MUX_MUX_CLKCMU_FSYS0_DPGTC CMU_CMU+0x1060,
    DIV +0x1854, GATE +0x2070 (cmucal-sfr.c:134/414/754; CMU_CMU = 0x1A240000 = EXYNOS_CMU_TOP_BASE, VERIFIED).
  - Linux clock names GATE_DP_LINK / GATE_DP_LINK_GTC are HWACG Q-channel clocks (clk-exynos9820.c:166-167) -
    **not referenced by the displayport DT node**; the hardware auto-gates them (Q-channel), so the vendor never
    explicitly enables the DP link's own clock.
- No `pd_fsys0` power domain exists (pm-domains.dtsi has pd_fsys0a@15862200 for USB, :135; no fsys0 node) ->
  INFERRED: BLK_FSYS0 is always-on (PCIe gen3 / pcie1 13120000 also sit in it). "130b0000.displayport" is an
  idle-IP entry in exynos9820.dts:739 (blocks SICD/low-power idle while DP is busy), not a power domain.

### 1.2 What the DT clock actually is
- `displayport@0x130B0000 { clocks = <&clock GATE_DPU_DPU>; clock-names = "aclk"; }` (exynos9820.dts:1924-1934).
- GATE_DPU_DPU = CLK_DPU_BASE+1 (dt-bindings/clock/exynos9820.h:142) -> `HWACG_VCLK(GATE_DPU_DPU, DPU_QCH_DPU,
  ..., "UMUX_CLKCMU_DPU_BUS")` (clk-exynos9820.c:132) -> QCH_CON_DPU_QCH_DPU = CMU_DPU 0x19000000+0x3020
  (cmucal-sfr.c:2158), ENABLE b0 / CLOCK_REQ b1 (cmucal-sfr.c:9015-9016).
  => "aclk" is the **DPU bus Q-channel** (the DMA side: DECON_T reads via DPU and pushes pixels to the DP link),
  not the DP link APB clock. INFERRED: it is there so the DPU bus stays up while DP is active.
- Enable point: `displayport_runtime_resume()` = `clk_prepare_enable(aclk)` (displayport_drv.c:4297-4304),
  triggered by `pm_runtime_get_sync()` at the top of `displayport_hpd_changed(1)` (displayport_drv.c:995-996),
  i.e. **before** the first link-register access (`displayport_reg_sw_reset()`, :1005). Disabled in
  runtime_suspend (:4286-4295) after `pm_runtime_put_sync` on HPD_FAIL (:1129) / disable.
- pd-dpu = PMU 0x15861E00 (+0x20), cal_id 0xB1380003, need_smc 0x191b0204 (pm-domains.dtsi:44-50). The DP node is
  not in pd-dpu (no samsung,power-domain on it); DECON_T is (it is in the DPU block). INFERRED: pd-dpu is ON in our
  firmware/Windows because DECON_F + DPU_DMA G0 scan out the panel (Exynos9820.h:963-971); DECON_T shares it.

### 1.3 PMU 0x15860704 (combo PHY isolation)
- `#define USBDP_PHY_CONTROL 0x15860704` (displayport.h:1145), ioremapped 4 bytes (displayport_drv.c:2593).
  EXYNOS_PMU_BASE 0x15860000 is VERIFIED (Exynos9820.h:296); offset 0x704 = DT `pmu_offset_dp` (exynos9820.dts
  usbdrd_phy0 node, 4302-4304) = UNVERIFIED-in-header (already used by our UsbHostDxe as `PMU_USBDP_ISO`, UsbHw.h:38).
- The DP driver **never writes** it. Bit0 is set by the USB PHY driver: `exynos_usbdrd_pipe3_phy_isol(inst, on,
  mask)` = `regmap_update_bits(pmu, 0x704, mask, on ? 0 : mask)` (phy-exynos-usbdrd.c:394-405); DT pmu_mask=0 is
  replaced by BIT(0) in code (notes/m16-ss-phy.md s5, usbdrd.c:1932). NB the source reads `val = on ? 0 : mask`
  on power_on(`phy_isol(inst,0,mask)`, usbdrd.c:1606) -> bit0 = 1 = de-isolated / "PHY enabled".
- **Every combo-PHY access in the DP driver is guarded** by `displayport_phy_enabled()` = `readl(0x15860704) & 1`;
  if 0 the read returns 0 and the write is dropped (displayport.h:1205-1271). Link-register accessors
  (`displayport_read/write`, displayport.h:1163-1203) have **no** guard - they rely on runtime PM.
- HAZARD (INFERRED from that guard): a Windows driver must read PMU+0x704 bit0 before every PHY access batch; a PHY
  access while isolated is the likely bus hang (same rule as m16-ss-phy.md s12).

### 1.4 Vendor order (summary) and what it means for us
1. Boot / USB: DWC3 core init -> phy_power_on(usb3) = PMU 0x704 b0=1 -> pipe3_init (USB combo PHY up; LC PLL,
   lane mux by gpg0-4) (m16-ss-phy.md s9; phy-exynos-usbdrd.c:1155-1240).
2. CCIC DP attach -> AUX switch GPIOs (section 2.1).
3. HPD high + link conf -> `pm_runtime_get_sync` -> aclk (DPU QCH) on -> `displayport_reg_sw_reset` ->
   `displayport_reg_init` (DP-mode PHY + AUX, section 2/4) (displayport_drv.c:995-1007).
- **BLOCKER for us (from m16-ss-phy.md, "Run 15")**: our firmware runs the combo PHY sequence once with
  USB_SS_LOCK_ATTEMPT 0 (UsbPhy.c:283) and tears it down to the g2_disable state; the LC PLL has never locked
  on our boots (0x350 stuck at 0x3E/0x38). The DP lanes are in the same PMA; if the PMA's PLL / refclk / LDOs are
  the problem for USB SS they are likely the problem for DP too (INFERRED). Settle section 4 before link training.
- Bus-hang list (INFERRED unless cited): (a) DP link 0x130B0000 with BLK_FSYS0 clocks gated - Q-channel HWACG
  should wake it on access, but verify CMU_FSYS0+0x2014/0x2010/0x3018 read-only first; (b) combo PHY 0x10AE0000
  with PMU 0x704 b0=0 (vendor guard, displayport.h:1205); (c) DECON_T 0x19050000 with pd-dpu off (not our case
  while the panel runs).
- 2026-10-20 sections 2+3 (AUX, DPCD, EDID) written from displayport_reg.c, regs-displayport.h, displayport.h,
  displayport_edid.c, pinctrl-exynos-arm64.c, exynos9820-beyond0lte_eur_open_25.dts (dtbo-hw_rev 25..255 -> covers
  our hw rev 26).

## 2. AUX channel

### 2.1 Switch GPIOs (board rev >= 25)
- Board DT (beyond0lte_eur_open_25.dts:206-214, `dtbo-hw_rev = <25>`..`<255>` :27-28): `dp,aux_sw_oe = <&gpg1 6 0>`,
  `dp,sbu_sw_sel = <&gpg4 0 0>`, `dp,usb_con_sel = <&gpg0 4 0>`, pinctrl-0 = aux_oe + sbu_sel = gpg1-6 and gpg4-0
  function 1 (output), pud 0, con-pdn 3 (keep in sleep) (:189-203). gpg0-4 = "USB3.1_CON_SEL", input pull-up
  (gpio_06b.dtsi:112 sleep config). (The common dtsi's `expander_reset_active` on gpg4-0, :2301, is unreferenced
  - older-rev leftover; on rev >= 25 gpg4-0 is the SBU switch select.)
- Addresses (PERIC0 pinctrl, bank table pinctrl-exynos-arm64.c:646-655; CON +0 4 bits/pin, DAT +4, PUD +8
  (Exynos9820.h:630 comment)):
  - gpg0 bank +0x080 -> GPG0DAT 0x10430084 (UNVERIFIED-in-header; :651). gpg0-5 is WLAN_EN - **RMW only**.
  - gpg1 bank +0x0A0 = `EXYNOS_GPIO_GPG1_OFF` (VERIFIED, Exynos9820.h:903) -> GPG1CON 0x104300A0, GPG1DAT
    0x104300A4. gpg1-0 is the CS47L93 codec reset (Exynos9820.h:907) - **RMW only**; pin 6 = CON bits [27:24].
  - gpg4 bank +0x0E0 -> GPG4CON 0x104300E0, GPG4DAT 0x104300E4 (UNVERIFIED-in-header; :654). Pin 0 = CON[3:0].
  - base EXYNOS_PERIC0_GPIO_BASE 0x10430000 VERIFIED (Exynos9820.h:768).
- Vendor sequence (displayport_drv.c):
  1. probe: gpg1-6 output **1** = AUX switch OFF (`gpio_direction_output(gpio_sw_oe, 1)`, :2531-2537).
  2. CCIC DP_CONNECT/ATTACH (:2733-2752): `displayport_aux_sel()` then `displayport_aux_onoff(1)`.
     - aux_sel (:2651-2669): `dp_sw_sel = !gpio_get_value(gpg0-4)`; `gpg4-0 := !dp_sw_sel` (= the gpg0-4 level).
       **Orientation comes from the gpg0-4 GPIO** (the same PHY_CON strap the USB PHY reads), not from a CCIC
       CC-pin register. (The brief said "from CC pin" - the source does not; INFERRED the gpg0-4 level is
       driven by the board's Type-C mux from CC; cross-check with max77705 CC_STATUS0 CCPinStat.)
     - aux_onoff(1) (:2632-2649): gpg1-6 := **0** (OE active low), then `msleep(100)`.
  3. CCIC DP_LINK_CONF (:2756-2797): aux_sel again; pin C/E/A/unknown -> 4-lane; pin B -> `dp_sw_sel` inverted
     (affects lane map + tune only; gpg4-0 is not rewritten after the flip); D/F -> 2-lane.
  4. Detach (:2711-2731): hpd_changed(0) then aux_onoff(0) = gpg1-6 := 1.
- HAZARD: none of the 3 pins is a power rail; wrong gpg4-0 = AUX P/N swapped/crossed -> AUX timeouts (safe
  failure, INFERRED). Keep gpg1-6 = 1 (off) whenever no DP partner is present.

### 2.2 DP controller reset + init (exact order, `displayport_hpd_changed(1)`, displayport_drv.c:995-1007)
All link offsets relative to 0x130B0000 (UNVERIFIED - not in Exynos9820.h).
1. `pm_runtime_get_sync` -> aclk (DPU QCH) on (section 1.2).
2. `displayport_reg_sw_reset()` (displayport_reg.c:234-251): SYSTEM_SW_RESET_CONTROL **0x0004** |= SW_RESET b0;
   poll until b0 self-clears, 10 x udelay(1) (regs-displayport.h:19-20).
3. `displayport_reg_init()` (displayport_reg.c:1470-1479):
   a. `displayport_reg_phy_init()` (:1447-1458) - combo-PHY DP mode, section 4 (dwc3_exynos_phy_enable(1,1) ->
      CMN_REG00E3 reset -> 32-entry default table -> lane mux -> SSC off -> release reset -> PLL lock poll).
   b. `displayport_reg_function_enable()` (:578-583): SYSTEM_COMMON_FUNCTION_ENABLE **0x0018** |= PCS_FUNC_EN b1,
      AUX_FUNC_EN b0; SYSTEM_SST1_FUNCTION_ENABLE **0x001C** |= SST1_VIDEO_FUNC_EN b0 (regs :37-49).
   c. `displayport_reg_sw_function_en(1)` (:1439-1445): SYSTEM_SW_FUNCTION_ENABLE **0x0014** |= SW_FUNC_EN b0.
   d. `displayport_reg_set_interrupt(1)` (:655-674): write ~0 to clear SYSTEM_IRQ_COMMON_STATUS 0x0108,
      SST1_INTERRUPT_STATUS_SET0 0x5024, _SET1 0x5028, SST2 status regs; set VSYNC_DET (0x502C b7), MAPI FIFO
      underflow (0x502C b8), audio-buffer-empty (SST1_AUDIO_BUFFER_CONTROL) "mask" bits. HPD masks are NOT touched
      with CONFIG_USB_TYPEC_MANAGER_NOTIFIER (:663-668) - HPD comes from the CCIC.
      Mask semantics: `set_interrupt_mask(param, 1)` writes ones into the "MASK" register (:585-653) - INFERRED
      1 = enabled/unmasked (the vendor calls it with en=1 to turn interrupts on and with 0 to "prevent intr
      storm", displayport_drv.c:1079).
   e. `displayport_reg_set_lane_map_config()` (:1347-1384): PCS_LANE_CONTROL **0x3004** LANE0..3_MAP (2 bits each at
      [1:0],[5:4],[9:8],[13:12]): pin C/D/E/F: dp_sw_sel=0 -> (0,1,3,2), dp_sw_sel=1 -> (3,2,0,1); pin A: (2,0,3,1) /
      (3,1,2,0); pin B: (1,0,2,3) / (3,2,1,0).
4. `usleep_range(10000, 11000)` (displayport_drv.c:1007) before the first AUX transaction.
- AUX_CONTROL 0x1000 fields (regs :133-138): AUX_POWER_DOWN b16, AUX_REPLY_TIMER_MODE [13:12], AUX_RETRY_TIMER
  [10:8], AUX_PN_INV b1, REG_MODE_SEL b0. Vendor only writes REPLY_TIMER_MODE = 3 ("AUX_TIMEOUT_1800us",
  displayport.h:752) per transaction; the AUX_PN_INV setter exists (displayport_reg.c:2172) but has **no caller**
  in this tree -> P/N orientation is handled by the gpg4-0 switch, not by the controller. AUX_POWER_DOWN is never
  written -> INFERRED reset value 0 (powered) - read it first.

### 2.3 AUX transaction interface (regs-displayport.h:140-213, displayport.h:110-115)
- AUX_TRANSACTION_START 0x1004 b0 AUX_TRAN_START (self-clearing); AUX_BUFFER_CLEAR 0x1008 b0; AUX_ADDR_ONLY_COMMAND
  0x100C b0; AUX_REQUEST_CONTROL 0x1010: REQ_COMM [31:28], REQ_ADDR [27:8] (20-bit DPCD addr or 7-bit I2C addr),
  REQ_LENGTH [5:0] = len-1; AUX_COMMAND_CONTROL 0x1014: DEFER_CTRL_EN b8, DEFER_COUNT [6:0]; AUX_MONITOR_1 0x1018:
  BUF_DATA_COUNT [30:24], CMD_STATUS [11:8], RX_COMM [7:4], BUSY b2; AUX_MONITOR_2 0x101C: ERROR_NUMBER [7:0];
  TX data 0x1030-0x103C, RX data 0x1040-0x104C (16 bytes; byte i at reg base+(i/4)*4, bits (i%4)*8).
- REQ_COMM values: I2C_WRITE 0x4, I2C_READ 0x5, DPCD_WRITE 0x8, DPCD_READ 0x9 (displayport.h:110-115). NB 0x4/0x5
  are the DP-spec I2C codes **with MOT=1** (bit2); the vendor never issues MOT=0 (INFERRED from the values; the
  "stop" is the address-only transaction instead).
- Start + completion (`displayport_reg_set_aux_ch_operation_enable`, displayport_reg.c:973-1007): set 0x1004 b0;
  poll b0 clear, up to 5000 x udelay(10) = **50 ms**; then success iff `(AUX_MONITOR_1 & 0xF00) == 0` and
  `AUX_MONITOR_2 == 0`; on error udelay(400) and return -EIO.
- Native DPCD read (`displayport_reg_dpcd_read`, :1047-1080), per attempt, AUX_RETRY_COUNT = 3 (displayport.h:751):
  REQ_COMM=9 -> REQ_ADDR=addr -> REQ_LENGTH=len-1 -> BUF_CLR -> DEFER_CTRL_EN=1 -> REPLY_TIMER_MODE=3 -> start/poll;
  on success read len bytes from 0x1040+. Max 16 bytes per transaction (AUX_DATA_BUF_COUNT 16, :749); bursts are
  split in 16-byte chunks (`dpcd_read_burst`, :1107-1132).
- Native DPCD write (:1014-1045): BUF_CLR -> DEFER on -> timer -> REQ_COMM=8 -> ADDR -> LEN -> TX bytes -> start.
- I2C-over-AUX EDID read (`displayport_reg_edid_read(offset, 128, buf)`, :1262-1337), per attempt (x3):
  1. REQ_COMM=4, REQ_ADDR=0x50, ADDR_ONLY=1, start, ADDR_ONLY=0 (address-only "I2C start").
  2. BUF_CLR, DEFER on, timer=3, ADDR_ONLY=0, REQ_COMM=4, ADDR=0x50, LEN=0 (1 byte), TX byte0 = offset, start
     (write the EDID word offset).
  3. REQ_COMM=5; for each 16-byte chunk: LEN=15, BUF_CLR, start, read 16 bytes from 0x1040+ (8 chunks per block).
  4. REQ_COMM=4, ADDR=0x50, ADDR_ONLY=1, start, ADDR_ONLY=0 (address-only "stop").
  - Before every block: `edid_check_set_i2c_capabilities()` reads DPCD 0x0000C and, if nonzero, writes the fastest
    supported speed bit to DPCD 0x00109 (displayport_edid.c:50-74).
  - Block offset = `((block & 1) * 128)` (displayport_edid.c:24) - **no E-DDC segment pointer (0x30)**: blocks 2/3
    re-read blocks 0/1 (vendor limitation; MAX_EDID_BLOCK 4, displayport.h:583). For a 1080p HDMI sink blocks 0+1
    suffice.
  - Checksum: sum of 128 bytes == 0 and not all-zero (displayport_edid.c:76-92); ext blocks must start with 0x02.

## 3. DPCD reads (order the vendor uses)
`displayport_link_status_read()` (displayport_drv.c:800-856), first thing after init:
1. DPCD 0x00000..0x0000B (12 bytes, DPCD_BUF_SIZE, displayport.h:162) - receiver caps: rev 0x000, MAX_LINK_RATE
   0x001 (0x06/0x0A/0x14), MAX_LANE_COUNT 0x002 [4:0] + TPS3 b6 + ENHANCED_FRAME_CAP b7, MAX_DOWNSPREAD 0x003
   (NO_AUX_HANDSHAKE b6), DOWN_STREAM_PORT_PRESENT 0x005 (DFP type bits [2:1]: 0 DP, 1 VGA, 2 HDMI, 3 other),
   TRAINING_AUX_RD_INTERVAL 0x00E is outside the 12 bytes and the vendor does not use it (displayport.h:203-240).
   Retried up to 5x with 50 ms sleeps (:806-813). This is the **first AUX proof of life**.
2. SINK_COUNT 0x00200 polled until `(val & (0x3F | 0x80)) != 0`, up to 200 x 20 ms = 4 s (:814-819); if it took
   more than 190 polls an extra 10 ms sleep (:833-834).
3. 0x00201..0x00207 (7 bytes): DEVICE_SERVICE_IRQ_VECTOR, LANE0_1_STATUS 0x202 (CR_DONE b0/b4, CHANNEL_EQ_DONE
   b1/b5, SYMBOL_LOCKED b2/b6), LANE2_3_STATUS 0x203, LANE_ALIGN_STATUS_UPDATED 0x204 (INTERLANE_ALIGN_DONE b0,
   LINK_STATUS_UPDATED b7), SINK_STATUS 0x205, ADJUST_REQUEST_LANE0_1 0x206 (swing [1:0]/[5:4], pre-emph
   [3:2]/[7:6]), ADJUST_REQUEST_LANE2_3 0x207 (displayport.h:268-316). All six bytes 0xFF = fail (:824-825).
4. Automated test request (0x201 b1) -> CTS EDID checksum path (:836-852) - not needed for bring-up.
Then `displayport_read_branch_revision()`: DPCD_BRANCH_HW_REVISION, 3 bytes (Samsung DeX version) (:780-798),
then in link training: `edid_update()` (EDID blocks, section 2.3), DPCD 0x003 MAX_DOWNSPREAD (:944-945), then full
link training (section 5). After training: DPCD 0x005 DFP type (:750-764).
- 2026-10-20 section 4 (combo PHY DP lanes) written from displayport_reg.c:253-436/1447-1468, phy-exynos-usbdp-gen2.c
  :1083-1190 + -reg.h, dwc3-exynos.c:371-420, dwc3 otg.c:279-300/312-370, xhci-plat.c:230-297; cross-checked every
  DP PHY write against the USB enable trace in notes/m16-ss-phy.md s7 and Beyond0LteUsbHostDxe/UsbSsPhyTable.h.

## 4. Combo PHY DP-lane setup (pin C/E = 4-lane, D = 2-lane + USB3)

PHY base for DP accesses = `phy_exynos_usbdp_get_address()` = the USB PHY driver's own mapping of 0x10AE0000
(phy-exynos-usbdrd.c:986, :1798-1801), not the DP node's 2nd reg (0x10AE0000+0x20CC, unused by the code).
EXYNOS_USB_COMBO_PHY_BASE 0x10AE0000 is VERIFIED (Exynos9820.h:473). All offsets below UNVERIFIED (vendor source).
Every access is behind the PMU 0x15860704 b0 guard (section 1.3).

### 4.1 `displayport_reg_phy_init()` (displayport_reg.c:1447-1458), exact order
0. `dwc3_exynos_phy_enable(1 /*dp*/, 1)` (:1450): sets owner bit1 in `combo_phy_control`; **if no owner yet**
   (USB idle) it runs `phy_conn` + `dwc3_core_init()` = the full USB PHY power-up incl. PMU 0x704 b0=1 and
   pipe3_init (dwc3 otg.c:312-349). If USB already owns the PHY it only sets the bit (:330-333). => **the DP path
   relies on the USB driver having powered the PMA** (USBCON 0x10B00000+0x48 LOW_PWRN=0, resets released, PCS/PMA
   table loaded, notes/m16-ss-phy.md s6-s7). There is no DP-private PMA power-up.
1. `displayport_reg_phy_reset(1)` (:253-265): CMN_REG00E3 **0x038C** RMW clear DP_CMN_RSTN b2.
2. `displayport_reg_phy_init_setting()` (:267-275): 32 writes `phy_default_value[]` (EVT1 table, :73-81;
   DEFAULT_SFR_CNT 32, regs-displayport.h:1566; beyond0 defconfig:436 `# CONFIG_SOC_EXYNOS9820_EVT0 is not set`):
   ```
   0x0830=07 0x085c=80 0x1030=07 0x105c=80 0x1830=07 0x185c=80 0x2030=07 0x205c=80 0x0228=38 0x0104=44
   0x0248=44 0x038C=02 0x0878=04 0x1878=04 0x09A4=07 0x09A8=22 0x19A4=07 0x19A8=22 0x0994=12 0x1994=12
   0x0998=12 0x1998=12 0x099C=48 0x199C=48 0x0898=77 0x1898=77 0x0054=01 0x00e0=38 0x0060=24 0x0064=77
   0x0070=76 0x0234=E8
   ```
   then CMN_REG0009 **0x0024** RMW ANA_AUX_TX_LVL_CTRL [6:3] = 0xD (regs :1569-1570).
   CROSS-CHECK (mechanical, vs m16-ss-phy.md s7 trace): **31 of 32 entries are identical writes already done by
   the USB `phy_exynos_usbdp_g2_enable`** (same address, same value). Only **0x0234=0xE8** and **0x0024** are
   DP-only (neither in the USB trace; no field names in phy-exynos-usbdp-gen2-reg.h). 0x038C=0x02 clears
   DP_INIT_RSTN/DP_CMN_RSTN - the USB init does the same (RMW set 0x02 keep 0xFFFFFFF1).
3. `displayport_reg_phy_mode_setting()` (:277-372): lane mux + TX clock inversion, then CMN_REG00A2 **0x0288** =
   lane_config | lane_en (absolute write, :371):
   - pin A/C/E (4-lane): `exynos_usbdrd_inform_dp_use(1, 4)` first -> `xhci_portsc_set(0)` = clear PORT_POWER in the
     xHCI USB3 PORTSC (xHCI base + 0x430) and set DIS_RX_DETECT b9 in USB PHYCON+0x70 (xhci-plat.c:43-44,
     230-297; dwc3 otg.c:279-298). Then 0x0288 = 0xFF (LANE_MUX_SEL_DP_LN3..0 b7..4 | DP_LANE_EN_LN3..0 b3..0,
     regs :1572-1580). TXCLK_INV (b1 of TRSV_REG0215/0415/0615/0815 = 0x0854/0x1054/0x1854/0x2054) = 1 on all 4
     lanes below HBR2, 0 at HBR2.
   - pin B/D/F (2-lane): `inform_dp_use(1, 2)` (no xHCI change). dp_sw_sel=1: 0x0288 = **0xCC** (LN3, LN2); TXCLK_INV
     LN2/LN3 = 1 below HBR2 (0 at HBR2), LN0/LN1 = 0. dp_sw_sel=0: 0x0288 = **0x33** (LN1, LN0); TXCLK_INV LN0/LN1 = 1
     below HBR2, LN2/LN3 = 0.
   - unknown pin: 0x0288 = 0x00 (DP lanes off).
   USB side of the same register (phy-exynos-usbdp-gen2.c:1083-1150, non-EVT0): used_phy_port 0 -> 0x0288 = 0xC0
   ("usb rx0, tx1, dp tx2, tx3", mux to DP but lanes disabled), used_phy_port 1 -> 0x30 ("dp rx0, tx1, usb tx2,
   tx3"). used_phy_port = gpg0-4 level and dp_sw_sel = !gpg0-4 -> port 0 <-> dp_sw_sel 1 <-> DP on LN2/LN3: the
   2-lane DP write (0xCC / 0x33) **only adds DP_LANE_EN bits to the mux the USB init already set** (INFERRED from
   the two sources agreeing).
4. `displayport_reg_phy_ssc_enable(0)` (:374-377): CMN_REG00B4 **0x02D0** RMW ROPLL_SSC_EN b1 := 0
   (regs-displayport.h:1586-1587). CONFLICT: phy-exynos-usbdp-gen2-reg.h:307-314 names 0x02D0 b0 = SSC_EN and leaves b1
   unnamed (fields CDR_LOCK_DELAY [7:5], RX_OC_DONE_DELAY [4:2], SSC_EN b0) - the two
   vendor headers disagree on this register; UNVERIFIED which is right. Read 0x02D0 before and after.
5. `displayport_reg_phy_reset(0)` (:259-264): 0x038C = DP_INIT_RSTN|CDR_WATCHDOG_EN (0x0A); udelay(1);
   0x038C = DP_INIT_RSTN|DP_CMN_RSTN|CDR_WATCHDOG_EN (0x0E). Absolute writes: also clears b0
   CDR_WATCHDON_MASK_CDR_EN (gen2-reg.h:404-408) - the USB init leaves b0 at its reset value (keep mask F1 keeps b0).
6. `displayport_reg_wait_phy_pll_lock()` (:379-392): poll **DP link** SYSTEM_PLL_LOCK_CONTROL 0x002C b4
   PLL_LOCK_STATUS, 300 x udelay(1) (comment "150us + 10% margin"); timeout only logs. (PLL_LOCK_FORCE b3 /
   PLL_LOCK_FORCE_EN b2 exist, regs :73-76.) INFERRED: this is the DP ROPLL (CMN_A0258 PLL_LC12_RO34_GEN_SEL,
   PMA_CTRL ROPLL_REF_REQ in m16 s6), separate from the USB LC PLL that has never locked on our boots - unknown
   whether it shares the failing refclk.

### 4.2 Link rate (also PHY): `displayport_reg_phy_set_link_bw()` (:394-413)
CMN_REG00A3 **0x028C** RMW DP_TX_LINK_BW [6:5]: 0 = RBR 1.62, 1 = HBR 2.7, 2 = HBR2 5.4 (regs :1582-1583).
The same register holds USB bits OVRD_RX_CDR_DATA_MODE_EXIT b4 / RX_CDR_DATA_MODE_EXIT b3 (gen2-reg.h:297-305,
USB writes 0x18 absolute, gen2.c:1112-1115) - the DP write is RMW, so USB bits survive. The DP **link** register
SYSTEM_MAIN_LINK_BANDWIDTH 0x000C is never written by the vendor (rate lives in the PHY).

### 4.3 Coexistence with our USB host - BOOT HAZARD list
Our state (notes/m16-ss-phy.md, UsbPhy.c USB_SS_LOCK_ATTEMPT 0): USB runs on the UTMI (USB2) PHY only; the combo
PMA was sequenced once and parked in the g2_disable state (USBCON 0x48 LOW_PWRN=1 + resets cycled). So DP has
**no powered PMA** to borrow today.
- HAZARD H-A (USBCON 0x10B00000+0x48 PMA power/resets, +0x20 CLKRST b7 LINK_PCLK_SEL): powering the PMA for DP means
  re-running the USB side's g2_pma_ready / usbdp_g2_enable / rst_release (m16 s6-s7) underneath a live xHCI.
  LINK_PCLK_SEL must stay 0 (USB2-only link) unless both USB PLL + CDR lock - setting it with a dead PMA killed the
  link on boot #75 (UsbPhy.c comment). **Never touch CLKRST b7 from a DP driver.**
- HAZARD H-B (PMU 0x15860704 b0): DP must never clear it (vendor never does). Clearing = combo PHY isolated under
  the running USB core.
- HAZARD H-C (0x0288 lane mux): writing the 4-lane value 0xFF takes the USB SS lanes; writing the wrong 2-lane
  value (0x33 when USB is on port 1 / LN2-3, i.e. the mux bits that USB set) re-routes the USB SS pair. With USB
  in USB2-only mode this cannot break enumeration (SS is not used) - INFERRED - but it would break a future M16 SS.
- HAZARD H-D (xHCI PORTSC 0x430 PORT_POWER / USBCON+0x70 DIS_RX_DETECT b9) on 4-lane: this is the USB3 root port;
  Windows' xHCI owns PORTSC -> a DP driver must not write it. For pin C/E under Windows, leave USB3 port power to
  the xHCI driver (it sees no SS device anyway). Base of `phycon_base_addr` not resolved (INFERRED USBCON
  0x10B00000; UNVERIFIED).
- HAZARD H-E (0x038C DP_CMN_RSTN / DP_INIT_RSTN, 0x0234, 0x0024, 0x02D0, TRSV xx15 b1 of the USB lanes): DP-only
  bits; USB init leaves the DP resets asserted. INFERRED safe for USB2, but the TXCLK_INV writes also hit the two
  USB lanes (value 0) in the 2-lane case and USB never writes those regs -> read the four 0x0854/0x1054/0x1854/
  0x2054 first; if the USB lanes' b1 reads 0 already, the write is a no-op.
- Teardown (`displayport_reg_phy_disable`, :1460-1468): 0x038C clear DP_CMN_RSTN, `inform_dp_use(0, lanes)`,
  `dwc3_exynos_phy_enable(1, 0)` (drops the DP owner bit; dwc3_core_exit only if USB is also not an owner).
- 2026-10-20 section 5 (link training) written from displayport_drv.c:179-748 + displayport_reg.c:394-576.

## 5. Link training (`displayport_full_link_training`, displayport_drv.c:293-748)

### 5.1 Rate / lane choice
- DPCD 0x000..0x00B: link_rate = byte 1, lane_cnt = byte 2 & 0x1F, TPS3 = byte 2 b6, enhanced frame = byte 2 b7
  (:315-326). Then `displayport_get_min_link_rate()` (:242-272) picks the **lowest** of RBR/HBR/HBR2 that carries
  the EDID's max pixel clock: capacity per lane RBR 54 MHz, HBR 90 MHz, HBR2 180 MHz pixel clock at 8 bpc
  (displayport.h:159-161), times lanes, times 9/10 margin (:200-201); 10 bpc x1.25. Never above the sink's
  MAX_LINK_RATE. No HBR3.
- For a forced 1920x1080p60 (148.5 MHz, INFERRED from CEA-861 VIC 16; table entry displayport_reg.c:184):
  2 lanes -> RBR 97.2 < 148.5, **HBR 162 >= 148.5 -> HBR x2**; 4 lanes -> **RBR x4** (194.4). Both are inside
  what the vendor would choose; HBR x2 = pin D (keeps USB3).
- DPCD 0x00E TRAINING_AUX_RD_INTERVAL & 0x7F (:337-338): wait = interval x 4 ms, or 100 us (CR) / 400 us (EQ)
  if 0 (:428-431, :617-620).

### 5.2 PHY / link programming before training (only if DECON_T is off or rate/lanes changed, :356-386)
If DECON_T (decon id 2) is ON and rate/lanes differ the vendor refuses ("phy_reset not permitted on decon on
state", :360-363) -> **train before starting DECON_T**.
1. `phy_reset(1)` (0x038C clear b2) -> `phy_init_setting()` (32-entry table + 0x0024, section 4.1 step 2)
2. `phy_set_link_bw(rate)` = 0x028C [6:5] (section 4.2)
3. `phy_mode_setting()` (lane mux/enable + TXCLK_INV, section 4.1 step 3; TXCLK_INV depends on the rate just set)
4. SYSTEM_MAIN_LINK_LANE_COUNT **0x0010** = lane_cnt (LANE_COUNT_SET [2:0], regs :31-32; displayport_reg.c:438-441)
5. if enhanced frame cap: SST1_MAIN_CONTROL **0x5000** |= ENHANCED_MODE b5 (regs :586-592)
6. udelay(60); `phy_reset(0)` (0x038C = 0x0A, 1 us, 0x0E)
7. DPCD write 0x100 LINK_BW_SET = rate code (0x06/0x0A/0x14), 0x101 LANE_COUNT_SET = lanes | 0x80 if enhanced (:388-394)
8. `wait_phy_pll_lock()` (0x002C b4, 300 us) (:396)

### 5.3 Clock recovery (TPS1)
- `set_training_pattern(TRAINING_PATTERN_1)` (displayport_reg.c:448-457): PCS_TEST_PATTERN_CONTROL **0x3008**
  LINK_QUALITY_PATTERN_SET [2:0] = 0; PCS_CONTROL **0x3000** LINK_TRAINING_PATTERN_SET [5:4] = 1; SCRAMBLE_BYPASS b0
  = 1 (0 only for NORMAL_DATA) (regs :374-395).
- DPCD 0x102 TRAINING_PATTERN_SET = 0x21 (scrambling disabled | TPS1) (:400-401).
- Loop (`Voltage_Swing_Retry`, :410-551): program PHY swing/pre-emph per lane (5.5), write DPCD 0x103..0x106
  TRAINING_LANEx_SET = pre<<3 | swing | MAX_SWING_REACHED b2 (if swing >= 3) | MAX_PRE_EMPHASIS_REACHED b5 (if pre >=
  3) (:274-291, :419-426); wait; read 0x202/0x203; CR_DONE per lane (b0 / b4).
  - all lanes CR_DONE -> EQ. All lanes at swing 3 without CR -> reduce rate.
  - else read ADJUST_REQUEST 0x206/0x207 and adopt the sink's swing/pre-emph; same request 5 times in a row
    (training_retry_no == 4) or >16 changed requests -> reduce rate (:526-537).
- Reduce rate (`Check_Link_rate`, :553-575): HBR2 -> HBR -> RBR -> fail (writes DPCD 0x102 = 0x00 on fail).
  Lane count is never reduced.

### 5.4 Channel equalisation (TPS2 / TPS3)
- TPS3 if sink supports it: PCS 0x3000 [5:4] = 3, DPCD 0x102 = 0x23; else [5:4] = 2, DPCD 0x102 = 0x22 (:582-592).
- Loop (`EQ_Training_Retry`, :594-747): program swing/pre-emph, write 0x103..0x106, wait, read 0x202..0x204:
  CR lost on any lane -> reduce rate; success when CHANNEL_EQ_DONE + SYMBOL_LOCKED on all lanes **and**
  INTERLANE_ALIGN_DONE (0x204 b0) -> `set_training_pattern(NORMAL_DATA)` (0x3000 [5:4] = 0, SCRAMBLE_BYPASS = 0)
  + DPCD 0x102 = 0x00 (:668-725). Else adopt ADJUST_REQUEST and retry; > 5 EQ retries -> fail (:731-740).

### 5.5 Voltage swing / pre-emphasis -> PHY
- `displayport_reg_set_phy_tune(lane, swing, pre)` (displayport_reg.c:479-507): 5 consecutive writes to the
  lane's TX block: lane0 TRSV_REG0204 **0x0810**, lane1 TRSV_REG0404 **0x1010**, lane2 TRSV_REG0604 **0x1810**, lane3
  TRSV_REG0804 **0x2010** (regs :1594-1618), at +0 AMP, +4 POST_EMP, +8 PRE_EMP, +0xC IDRV, +0x10 ACCDRV
  (displayport.h:442-448), values `phy_tune_parameters[swing][pre][0..4]`.
- Table = `phy_tune_param_default` (no `dp,phy_tuning` in the board DT -> index 0, displayport_drv.c:2556-2559;
  grep of beyond0lte_eur_open_25.dts: no dp,phy_tuning), EVT1 version (displayport_reg.c:83-106):
  ```
  {amp, post, pre, idrv, accdrv}   pre-emph 0           1                    2                    3
  swing 0                {21 10 42 E5 60}     {25 14 42 E5 60}     {26 17 43 E5 60}     {2B 1C 43 E7 60}
  swing 1                {26 10 42 E7 60}     {2B 15 42 E7 60}     {2B 18 43 E7 60}     {2B 18 43 E7 60}
  swing 2                {2A 10 42 E7 60}     {2B 15 43 E7 60}     {2B 15 43 E7 60}     {2B 15 43 E7 60}
  swing 3                {2B 10 43 E7 60}     {2B 10 43 E7 60}     {2B 10 43 E7 60}     {2B 10 43 E7 60}
  ```
- Logical DP lane -> physical PHY lane mapping (`displayport_reg_set_phy_voltage_and_pre_emphasis`, :509-571):
  pin C/E: dp_sw_sel=0 -> phy0<-L0, phy1<-L1, phy2<-L3, phy3<-L2; dp_sw_sel=1 -> phy0<-L2, phy1<-L3, phy2<-L1,
  phy3<-L0. pin D/F: dp_sw_sel=0 -> phy0<-L0, phy1<-L1; dp_sw_sel=1 -> phy2<-L1, phy3<-L0. (Consistent with the
  PCS_LANE_CONTROL maps in 2.2e.)
- HAZARD: TRSV 0x1010 and 0x2010 are also the USB TX lanes' amp registers (USB writes RMW set 0x2B keep
  0xFFFFFFC0, m16-ss-phy.md s7). In pin D the DP tune only writes the DP lanes (phy2/3 or phy0/1), never the USB
  TX lane - but in pin C/E all four are written (USB has no SS lanes then anyway).
- 2026-10-20 section 6 (video: DP SST1 timing/MSA, BIST, DECON_T) written from displayport_reg.c:695-876/1386-1658,
  displayport_drv.c:1699-1839/2112-2172, decon_displayport.c:175-221, cal_9820/decon_reg.c, regs-decon.h,
  decon_cal.h, decon.h:1323-1399, decon_core.c:425-689/2195-2250, helper.c:568-590, exynos9820.dts (decon_t, dpp_*,
  disp_ss), v4l2-dv-timings.h:190-196.

## 6. Video

### 6.1 1920x1080p60 (CEA VIC 16) numbers
`V4L2_DV_BT_CEA_1920X1080P60` (v4l2-dv-timings.h:190-196; field order of V4L2_INIT_BT_TIMINGS = width, height,
interlaced, polarities, pixelclock, hfp, hsync, hbp, vfp, vsync, vbp): 1920x1080, +H +V, 148.5 MHz, HFP 88, HSYNC 44,
HBP 148, VFP 4, VSYNC 5, VBP 36 -> HTOTAL 2200, VTOTAL 1125. Vendor table row: supported_videos[V1920X1080P60],
SYNC_POSITIVE (= 0, displayport.h:434) both, vic 16 (displayport_reg.c:184).

### 6.2 DP SST1 video registers (`displayport_reg_set_video_configuration`, displayport_reg.c:1487-1498)
Order and 1080p60 values (offsets UNVERIFIED, regs-displayport.h:586-778):
1. SST1_VIDEO_CONTROL **0x5400**: DYNAMIC_RANGE_MODE b7 = 0 (VESA 0-255; CEA = 1) (:695-698, enum displayport.h:97-98);
   BPC [6:4] = 1 (BPC_8, enum displayport.h:550-556); COLOR_FORMAT [3:2] = 0 (RGB).
2. `video_format_register_setting` (:716-756): SST1_VIDEO_VERTICAL_TOTAL_PIXELS **0x5418** = 1125;
   HORIZONTAL_TOTAL **0x5414** = 2200; VERTICAL_ACTIVE **0x5430** = 1080; V_FRONT_PORCH **0x5428** = 4;
   V_BACK_PORCH **0x542C** = 36; HORIZONTAL_ACTIVE **0x5424** = 1920; H_FRONT_PORCH **0x541C** = 88; H_BACK_PORCH
   **0x5420** = 148; VIDEO_CONTROL VSYNC_POLARITY b1 = 0, HSYNC_POLARITY b0 = 0. (Sync widths are not programmed -
   INFERRED derived as total - active - porches.)
3. `set_video_clock` (:782-802), stream clock in kHz: SST1_MVID_MASTER_MODE **0x5044** = 148500 >> 1 = **74250**;
   SST1_NVID_MASTER_MODE **0x5048** = link-symbol clock kHz (HBR **270000**, RBR 162000, HBR2 540000; :765-780);
   SST1_MAIN_CONTROL **0x5000** |= MVID_MODE b11; SST1_MVID_SFR_CONFIGURE **0x504C** = 148500; SST1_NVID_SFR_CONFIGURE
   **0x5050** = 270000 (HBR).
4. `set_active_symbol` (:804-875): SST1_ACTIVE_SYMBOL_MODE_CONTROL **0x5104** b0 = 1 (regs-displayport.h:700-701);
   THRESHOLD_SEL_FEC_OFF **0x508C** = 1; THRESHOLD_SEL_FEC_ON **0x509C** = 1;
   TU = clk*bpp*32/(lanes*ls_clk*8) in 1e-10 fixed point. Computed with the vendor's exact integer math (python,
   2026-10-20):
   | link | INTEGER_FEC_OFF 0x5080 | FRACTION_FEC_OFF 0x5084 | THRESHOLD_FEC_OFF 0x5088 | INTEGER_FEC_ON 0x5090 | FRACTION_FEC_ON 0x5094 | THRESHOLD_FEC_ON 0x5098 |
   |---|---|---|---|---|---|---|
   | HBR x2 | 26 | 400000000 | 9 | 27 | 49180327 | 9 |
   | RBR x4 | 22 | 0 | 9 | 22 | 540983606 | 9 |
   Vendor quirk: THRESHOLD_FEC_OFF is written with mask ACTIVE_SYMBOL_FRACTION_FEC_OFF (0x3FFFFFFF) instead of its
   own 0x0F mask (:870) - same value lands in bits [3:0], harmless; copy the intent (value 9), not the mask bug.
5. SST1_VIDEO_MASTER_TIMING_GEN **0x5408** |= VIDEO_MASTER_TIME_GEN b0 (:1496).
6. SST1_MAIN_CONTROL 0x5000 VIDEO_MODE b6 = 0 (:1497).
Then (`displayport_enable`, displayport_drv.c:2140-2160): AVI infoframe -> SST1_INFOFRAME_AVI_PACKET_DATA_SET0..3
**0x5C40..0x5C4C** (DB1 = 0x10 "AFI present", DB2 = 0x08 "same as picture aspect", DB4 = VIC 16; rest 0;
displayport_drv.c:1706-1722, displayport.h:1013-1016), SST1_INFOFRAME_UPDATE_CONTROL **0x5C04** |= AVI b2,
SEND_CONTROL **0x5C08** |= AVI b2 (displayport_reg.c:1536-1558); `displayport_reg_start()` = SST1_VIDEO_ENABLE
**0x5404** |= VIDEO_EN b0 (:1640-1643); enable_irq. Stop = VIDEO_EN = 0 (:1652-1656).
Note the vendor ordering: **DP video config + VIDEO_EN happen before DECON_T is initialised** (s_stream(1) in
`decon_set_out_sd_state`, decon_core.c:579, runs before `decon_reg_init`, :652-653).

### 6.3 First light WITHOUT DECON_T: the DP controller's own video BIST (recommended rung H4a)
`displayport_reg_set_bist_video_configuration_for_blue_screen()` (displayport_reg.c:1523-1534): video config as 6.2
(8 bpc, CEA range) -> SST1_VIDEO_BIST_USER_DATA_R **0x5454** = 0x00, _G **0x5458** = 0x00, _B **0x545C** = 0xFF
(10-bit fields) -> SST1_VIDEO_BIST_CONTROL **0x5450** |= BIST_USER_DATA_EN b4, CTS_BIST_EN b17 = 0 ->
`set_video_bist_mode(1)` = SST1_VIDEO_CONTROL |= STRM_VALID_FORCE b9 | STRM_VALID_CTRL b8 and BIST_CONTROL |= BIST_EN b3
(:700-706). Pattern types without user data: BIST_TYPE [1:0] = COLOR_BAR 0 / WGB_BAR 1 / MW_BAR 2 (displayport.h:
559-567, :1500-1521). The vendor uses this path when `bist_used` (displayport_drv.c:2140-2147, hpd_changed :1064
calls displayport_enable directly, no DECON). => **a solid colour or colour bars on the monitor needs only: AUX
switch + PHY + link training + SST1 registers + BIST**; no DECON_T, no DPP, no SYSMMU, no DPU write. This isolates
the DP/PHY risk from the DPU risk.

### 6.4 DECON_T (decon2, EXYNOS_DECON_T_BASE 0x19050000 VERIFIED) for real pixels
DT (exynos9820.dts:2000-2034): out_type 2 (DP), psr_mode 0 (video), trig_mode 0 (HW), max_win 6, **default_win 4,
default_ch 2**, ppc 2, irqs DECON2 FRAME_START 195 / FRAME_DONE 196 / EXTRA 197 (d6 P3).
**Shared-register warning (mechanical, from the code):** the 9820 DECON keeps several global/window registers only
in **DECON0 = DECON_F 0x19030000** and the vendor writes them with id 0 even when configuring DECON_T:
- `DP_CONNECTION_CONTROL` **0x0270** (DECON0) SEL_DP0 [2:0] = 2 ("decon2 - DP0") (decon_reg.c:496-509, regs-decon.h:352-358)
- `CLOCK_CONTROL_0` **0x00F0** (DECON0) TE_QACTIVE_PLL_ON b28 = 1 for DP (`decon_reg_set_te_qactive_pll_mode`
  writes id 0, decon_reg.c:160-168, called :1783-1784)
- per-window **WIN_START_POSITION 0x1008+w*0x30, WIN_END_POSITION 0x100C+w*0x30, WIN_START_TIME_CONTROL
  0x1018+w*0x30, WIN_COLORMAP_0/1 0x1010/0x1014+w*0x30, blend function (WIN_CONTROL_0/1)** - written with id 0
  (`decon_reg_set_window_control`, :1905-1924; `set_winmap` -> `set_win_mapcolor(0, ...)`, :1606-1619). Windows
  are a shared pool; only WIN_EN / MAPCOLOR_EN (DATA_PATH_CONTROL_0 0x0214) and the channel map
  (DATA_PATH_CONTROL_1 0x0218) are per-DECON.
- SYSREG_DPU (`disp_ss`, reg 0x19011000, exynos9820.dts:1887-1890) DISP_DPU_TE_QACTIVE_PLL_EN **+0x10** b0 = 1
  (`dpu_reg_set_qactive_pll`, decon_reg.c:15-19, regs-decon.h:23-24) -> 0x19011010.
  => HAZARD (panel): every one of these is a RMW in the live panel DECON's register file; a wrong window index or a
  plain write kills the panel scan-out. Pick a window that DECON_F does not use: read DECON_F DATA_PATH_CONTROL_0/1
  (0x19030214/0x19030218) first (vendor decon_f default_win 5 ch 0, exynos9820.dts:1952-1954).
`decon_reg_init(2, ...)` (decon_reg.c:1761-1826), order:
1. if GLOBAL_CONTROL **0x0000** RUN_STATUS b4 already set -> "already called by BOOTLOADER", only probe-init (:1771-1777).
2. SYSREG_DPU+0x10 b0 = 1 (qactive pll).
3. CLOCK_CONTROL_0 0x00F0 (DECON_T): clear CG_MASK 0x11111 and QACTIVE bits 24/28 (clock gating off, :150-158).
4. DECON0 CLOCK_CONTROL_0 b28 = 1 (DP only).
5. SRAM_SHARE_ENABLE **0x0030** = SRAM1|SRAM2|SRAM3 (b16|b20|b24, DECON_FIFO_12K for id 2, :181-221) - absolute
   write in DECON_T. INFERRED conflict check needed with DECON_F (it uses DECON_FIFO_04K = SRAM0 b12).
6. GLOBAL_CONTROL OPERATION_MODE b8 = 0 (video) (:56-66).
7. BLENDER_BG_IMAGE_SIZE_0 **0x0200** = (1080 << 16) | 1920 (:283-297).
8. SCALED_SIZE_CONTROL_0 **0x00A0** = (1080 << 16) | 1920 (:223-232).
9. HW_SW_TRIG_CONTROL **0x0070** HW_TRIG_EN b0 = 1 (trig_mode HW) (:533-540).
10. `configure_lcd` (:1371-1421): OUTFIFO_DATA_ORDER_CONTROL **0x0130** PIXEL_ORDER_SWAP [6:4] = DECON_RGB 0
    (DP uses RGB, DSI-without-DSC uses BGR 4; decon_cal.h:123-126); DATA_PATH_CONTROL_2 **0x0230** COMP_OUTIF_PATH
    [7:0] = DECON2_NOCOMP_OUTFIFO0_DPIF **0x008** (decon_cal.h:91), SCALER_PATH [25:24] = 0;
    OUTFIFO_SIZE_CONTROL_0 **0x0120** = (1080 << 16) | 1920 (plain write); OUTFIFO_TH_CONTROL_0 **0x012C** [..] =
    OUTFIFO_TH_1H_F 5 (:234-248); SCALED_SIZE again; GLOBAL_CONTROL TEN_BPC_MODE b20 = 0 (8 bpc, :512-522);
    `per_frame_off` GLOBAL_CONTROL DECON_EN_F b0 = 0.
11. clear INTERRUPT_PENDING **0x004C** b12/b13, EXTRA_INTERRUPT_PENDING **0x0050** b4/b8 (write-1, :1358-1369).
12. `set_interface`: DECON0 DP_CONNECTION_CONTROL SEL_DP0 = 2.
Window (vendor black window, decon_core.c:425-451, used for DSI; for DP the first frame comes from winconfig
:2215-2242): window w: WIN_START_POSITION = 0, WIN_END_POSITION = ((1080-1) << 16) | (1920-1) (decon.h:1498-1501
`win_end_pos` = y+yres-1 / x+xres-1), WIN_COLORMAP = colour + DATA_PATH_CONTROL_0
WIN_MAPCOLOR_EN_F(w) b(4w+1) for a **DMA-less solid colour**, or channel map DATA_PATH_CONTROL_1 WIN_CHMAP_F(w, 2)
[4w+2:4w] for DPP channel 2; WIN_EN_F(w) b(4w); then SHADOW_REG_UPDATE_REQ **0x0060** |= 0x3F (all windows).
`decon_reg_start(2)` (:1828-1847): **`displayport_reg_lh_p_ch_power(1)` first** = DP SYSTEM_SST1_FUNCTION_ENABLE
0x001C SST1_LH_PWR_ON b4 = 1, poll SST1_LH_PWR_ON_STATUS b5, 20000 x udelay(1) (displayport_reg.c:1386-1437; the
"wait 1ms" comment is wrong, it is ~20 ms) - the DECON_T -> DP link-layer P-channel; then GLOBAL_CONTROL |=
DECON_EN b1 | DECON_EN_F b0; SHADOW_REG_UPDATE_REQ |= GLOBAL b31; poll RUN_STATUS b4, 20 ms (:104-126). The vendor
**BUG()s (panics) if DECON_T does not reach run status** (decon_core.c:2242-2249). Interrupts:
INTERRUPT_ENABLE **0x0040** = FRAME_DONE b13 | FRAME_START b12 | EXTRA b4 | INT_EN b0 (mask 0x3011),
EXTRA_INTERRUPT_ENABLE **0x0044** = RESOURCE_CONFLICT b8 | TIME_OUT b4 (:2101-2124).
Stop: `decon_reg_stop` -> te_qactive off, per-frame off (wait idle), lh_p_ch_power(0) (:1501-1556, :1856-1888).

### 6.5 DPP / IDMA channel for DECON_T
default_ch 2 = `dpp_2` "GF1": DPU_DMA **0x19072000** (+0x1000), DPP **0x19022000** (+0x1000), IRQs DPU_DMA_GF1 /
DPU_DPP_GF1, attr 0x50007 (IDMA/FLIP/BLOCK/AFBC), **AXI port 1** (exynos9820.dts:1767-1779) -> INFERRED IOMMU
sysmmu_dpu1 @0x190B0000 (d6 P3; port -> sysmmu mapping not read). Same compatible as the panel's G0
(0x19071000) whose IDMA layout our driver already uses (MaliG76Wddm.h:63-76: IDMA_ENABLE 0x00, SRC_SIZE 0x10,
IMG_SIZE 0x18, IN_BASE_ADDR_Y 0x40, latched copy +0x800) -> INFERRED identical register layout at 0x19072000.
The DPP block (0x19022000, format/CSC) was not studied here (dpp_reg.c not fetched) - use the mapcolor / BIST paths
first. OPEN: whether sysmmu_dpu1 is in bypass under our firmware (G0 scans out a physical address today).
- 2026-10-20 sections 7 (hot plug) + 8 (bring-up order) written from displayport_drv.c:979-1130/1200-1258/1469-1697/
  2692-2833, displayport_reg.c:585-693, regs-displayport.h:62-127, max77705_alternate.c:594-657.

## 7. Hot plug (HPD from the max77705, not a SoC pin)

- Source: max77705_alternate.c reads `HPD_State` / `HPD_Interrupt` from the DP Status Update reply
  (:594-603) and from DP Attention VDMs (:648-657) and raises `CCIC_NOTIFY_ID_DP_HPD` (sub1 HIGH/LOW, sub2 IRQ).
  Our equivalent: tools/phone/usbc/usbcaltmode (G18 prep P2) already decodes and logs HPD from those replies.
- The DP controller has an HPD block (SYSTEM_HPD_CONTROL **0x0028**: HPD_FORCE_EN b0, HPD_FORCE b1, HPD_EVENT_CTRL_EN
  b4, EVENT_IRQ/PLUG/UNPLUG b5-7, HPD_STATUS b8, DEGLITCH_COUNT [29:16]; regs :62-71; SYSTEM_DEBUG 0x0200
  AUX_HPD_CONTROL b2, :123-125) but the vendor **never writes it** and never unmasks the HPD interrupts with the CCIC
  build (displayport_reg.c:663-668; IRQ handler HPD branches compiled out, displayport_drv.c:1617-1637). INFERRED:
  AUX works without the controller seeing HPD; read HPD_STATUS b8 once on a live link to learn whether the pin is
  wired at all. If AUX turns out to be gated on HPD, HPD_FORCE_EN|HPD_FORCE is the knob (UNVERIFIED).
- HPD HIGH (and link conf already known, or link conf arriving after HPD) -> `displayport_hpd_changed(1)`
  (displayport_drv.c:2800-2822, :2788-2792): full section 2.2 init -> DPCD/EDID -> link training -> announce
  (extcon) -> userspace enables DECON_T + `displayport_enable` within 3 s (:1062-1067).
- HPD LOW -> `hpd_changed(0)` (:1072-1127): cancel works, SST1_INTERRUPT_MASK_SET0 MAPI_FIFO_UNDER_FLOW_MASK := 0
  ("prevent intr storm", :1079), announce removal, wait up to 7 s for DECON_T/DP disable (`displayport_disable`:
  all DP interrupt masks 0, disable_irq, VIDEO_EN 0 + SW_FUNC_EN 0, BIST off, PHY disable = 0x038C DP_CMN_RSTN 0 +
  `dwc3_exynos_phy_enable(1,0)`, pm_runtime_put -> DPU QCH aclk off; :2174-2207).
- CCIC detach additionally: aux_onoff(0) = gpg1-6 := 1 (:2711-2731).
- HPD IRQ (HIGH with sub2 = IRQ while already plugged) -> `displayport_hpd_irq_work` (:1469-1581): read DPCD
  0x200..0x207; automated-test request -> CTS handler; `displayport_check_dpcd_lane_status()` (:1200-1258): rate/lanes
  in DPCD 0x100/0x101 must match the PHY's (0x028C) and SYSTEM_MAIN_LINK_LANE_COUNT; if 0x204 LINK_STATUS_UPDATED b7 is
  set, require INTERLANE_ALIGN_DONE and CR/EQ/SYMBOL_LOCKED on every active lane -> else **re-run link training**
  (only allowed while DECON_T is off or with the same rate/lanes, section 5.2). CP_IRQ (0x201 b2) = HDCP only.
- Unplug glitch rule from the vendor: `hpd_changed()` ignores a repeated state (:986-990).
- DP interrupt line: GIC SPI 248 INTREQ__DP_LINK (d6 P3). Handler (:1597-1697): read+clear (write ~0)
  SYSTEM_IRQ_COMMON_STATUS 0x0108, SST1_INTERRUPT_STATUS_SET0 0x5024 (MAPI_FIFO_UNDER_FLOW b8 = DECON_T underrun,
  VSYNC_DET b7 = **the DECON_T vsync source for DP** - it drives decon->vsync), audio buffer status. A WDDM driver's
  VSync for the external target would come from here.

## 8. Minimal bring-up order (numbered; read-only first)

Legend: [R] read-only, [W] write; V = verification read; HZ = hazard. All addresses UNVERIFIED unless noted.
Phase A - no cable needed (all read-only; every one can be run from TWRP/Windows probe without side effects):
1. [R] PMU 0x15860704 (EXYNOS_PMU_BASE VERIFIED + 0x704). V: bit0. HZ: none (PMU always on). If 0 -> do not touch
   0x10AE0000 in this boot.
2. [R] CMU_FSYS0 0x13000140 (DPGTC USER mux), 0x13002010/0x13002014 (DP_LINK gates), 0x13003018/0x1300301C (DP_LINK
   QCH), CMU_TOP 0x1A241060/0x1A241854/0x1A242070 (CLKCMU_FSYS0_DPGTC mux/div/gate). V: gate CG_VAL b21 / QCH ENABLE b0.
   HZ: none (CMU regs are always clocked).
3. [R] CMU_DPU 0x19003020 (QCH_CON_DPU_QCH_DPU). V: ENABLE b0. HZ: none while pd-dpu is on (panel running).
4. [R] DP link 0x130B0000+0x0000 SYSTEM_DEVICE_VERSION, then 0x0004, 0x0008 SYSTEM_CLK_CONTROL, 0x0014/0x0018/0x001C,
   0x0028 HPD_CONTROL, 0x002C PLL_LOCK_CONTROL, 0x1000 AUX_CONTROL. V: a non-zero, non-0xFFFFFFFF version.
   HZ: **first touch of the DP block** - if BLK_FSYS0's DP_LINK clock is gated and HWACG does not wake it, this is
   the read that could hang (INFERRED; FSYS0 has no power domain, so a hang is unlikely but unproven). Do it only
   after step 2 shows the gates.
5. [R] GPIO: GPG0DAT 0x10430084 b4 (orientation), GPG1CON/DAT 0x104300A0/A4 (pin 6 function + level), GPG4CON/DAT
   0x104300E0/E4 (pin 0). V: gpg1-6 function 1 and level 1 (AUX switch off) as left by sboot/Android, or reset.
6. [R] combo PHY (only if step 1 bit0 = 1 **and** USBCON 0x10B00000+0x48 shows APB_SW_RST b0 = 0): 0x0288, 0x028C,
   0x02D0, 0x038C, 0x0024, 0x0234, 0x0854/0x1054/0x1854/0x2054, 0x0350 (LC PLL). V: compare against the USB trace
   (section 4.1). HZ: PHY read while isolated/APB in reset (m16 s12).
7. [R] DECON_F 0x19030000 + 0x0214/0x0218 (window enable / channel map in use), +0x0270 DP_CONNECTION_CONTROL,
   +0x00F0 CLOCK_CONTROL_0, +0x0030 SRAM_SHARE; DECON_T 0x19050000 + 0x0000 GLOBAL_CONTROL (RUN_STATUS b4 expected 0),
   +0x0030; SYSREG_DPU 0x19011010. V: which window DECON_F owns; that DECON_T is idle. HZ: none (reads).
Phase B - PD alt mode (H2, user-gated writes to the max77705; already prepared in S10eBattery 1.0.0.7 + usbcaltmode):
8. Enter DP alt mode, DP Configure with pin D (2-lane + USB3) preferred; wait for HPD_State = 1. V: max77705 Altmode
   bit / DP Status reply. HZ: VBUS/charging side effects (g18-hdmi-prep.md gates).
Phase C - DP AUX (first SoC writes; panel and USB untouched):
9. [W] gpg4-0 := gpg0-4 level (RMW GPG4DAT b0; set GPG4CON[3:0] = 1 if not output); gpg1-6 := 0 (RMW GPG1DAT b6, CON
   [27:24] = 1); sleep 100 ms. V: read back both DAT regs; gpg1-0 (codec reset) and gpg0-5 (WLAN_EN) unchanged.
   HZ: RMW mistakes on shared banks (codec reset / WLAN_EN).
10. [W] DP link SW reset: 0x0004 |= b0, poll b0 = 0 (10 us). V: 0x0004 = 0.
11. [W] DP-mode PHY init (section 4.1 steps 1-6) **only if** the PMA is already powered by the USB side (step 6
    reads sane values). V: DP 0x002C b4 PLL_LOCK_STATUS = 1 within 300 us. HZ: H-A..H-E (section 4.3); on our
    current firmware the PMA is parked -> expect this to fail until the USB-side PMA power-up is solved (OPEN).
12. [W] 0x0018 |= AUX_FUNC_EN b0 | PCS_FUNC_EN b1; 0x001C |= b0; 0x0014 |= b0; clear 0x0108/0x5024/0x5028 (write ~0);
    PCS_LANE_CONTROL 0x3004 lane map (section 2.2e). V: read back.
13. [W] AUX DPCD read 0x00000 len 12 (section 2.3). V: AUX_MONITOR_1[11:8] = 0, MONITOR_2 = 0, DPCD rev 0x11/0x12/
    0x14, MAX_LINK_RATE in {06,0A,14}. **This is the H3 proof of life.** HZ: none beyond a 50 ms poll.
14. [W] DPCD 0x200 sink count, 0x201..0x207; EDID block 0 (+1) over I2C-over-AUX at 0x50. V: EDID header
    00 FF FF FF FF FF FF 00 and checksum 0.
Phase D - link + BIST (no DPU involvement):
15. [W] Link training at HBR x2 (pin D) per section 5 (PHY rate/mode, lane count 0x0010, DPCD 0x100/0x101, TPS1
    CR loop, TPS2/3 EQ loop, normal data). V: DPCD 0x202/0x203 all lanes 0x77 pattern for active lanes, 0x204 b0 = 1.
16. [W] SST1 1080p60 registers (section 6.2) + BIST blue screen (6.3) + AVI infoframe + VIDEO_EN. V: monitor shows
    blue; SST1_INTERRUPT_STATUS_SET0 VSYNC_DET b7 toggles; no MAPI underflow (b8) is expected with BIST.
Phase E - DECON_T (DPU writes; panel at risk only through the shared DECON0 registers):
17. [W] DECON_T init (section 6.4, steps 1-12) with RMW-only on DECON0 shared regs, a window DECON_F does not use,
    MAPCOLOR solid colour (no DMA). V: DECON_F scan-out unchanged (screenshot + DSIM line counter, memory
    self-check-frozen-screen), DECON0 0x0270 SEL_DP0 = 2.
18. [W] BIST off (STRM_VALID_FORCE/CTRL 0, BIST_EN 0), `lh_p_ch_power(1)` (0x001C b4, poll b5), DECON_T DECON_EN |
    DECON_EN_F, SHADOW GLOBAL, poll RUN_STATUS (20 ms). V: RUN_STATUS b4 = 1, DP VSYNC_DET, monitor shows the
    mapcolor. HZ: vendor panics on run-status timeout (decon_core.c:2242-2249) -> on timeout run the stop sequence,
    do not retry blindly.
19. [W] Switch the window to DPP channel 2 (G1 IDMA 0x19072000 + DPP 0x19022000) with a physical framebuffer.
    V: image on the monitor; IDMA latched copy (+0x800) matches. HZ: SYSMMU dpu1 state (OPEN), DPU bandwidth with two
    scan-outs (OPEN, vendor uses BTS).
Teardown at any step: VIDEO_EN 0, SW_FUNC_EN 0, 0x038C DP_CMN_RSTN 0, gpg1-6 := 1. Never PMU 0x704, never
USBCON 0x20 b7 / 0x48, never xHCI PORTSC.

## Open questions (each needs a read or a decision before code)
1. PMA power for DP under our firmware: the USB side parks the combo PMA (LOW_PWRN=1). Can the PMA be powered and
   the DP ROPLL lock while the USB LC PLL does not? Same refclk/LDO suspects as M16 (notes/m16-ss-phy.md s12).
   Read: section 8 step 6 on the current build; then a DP-only PMA power-up experiment (user-gated, USB host at risk).
2. 0x02D0 SSC bit: displayport header says b1 ROPLL_SSC_EN, usbdp-gen2 header says b0 SSC_EN. Read before/after.
3. Is the DP controller's HPD input wired (SYSTEM_HPD_CONTROL b8 on a live link)? Does AUX need HPD_FORCE?
4. sysmmu_dpu1 (AXI port 1 of dpp_2) bypass state under our firmware.
5. DPU bus / BTS headroom for panel + 1080p60 external at once (vendor drives ACPM DVFS DISP/INT/MIF).
6. Whether DECON_T SRAM_SHARE (SRAM1-3) conflicts with DECON_F's allocation as sboot left it (read 0x19030030).
7. `phycon_base_addr` (USBCON+0x70 DIS_RX_DETECT) base not resolved - only matters for 4-lane pin C/E.
8. EDID blocks >= 2 (vendor has no segment pointer) - irrelevant for a 1080p HDMI adapter.

## Self-review (2026-10-20)
- Checked mechanically: pmu_mask BIT(0) fallback (phy-exynos-usbdrd.c:1932), USB_SS_LOCK_ATTEMPT 0 (UsbPhy.c:283),
  no `dp,phy_tuning` in exynos9820.dts / beyond0lte_common.dtsi / eur_open_25.dts (grep count 0), EVT0 unset
  (beyond0lte_defconfig:436), CONFIG_PHY_SAMSUNG_USB_CAL=y (:5369, so dwc3_exynos_phy_enable / inform_dp_use are
  live), 31/32 DP PHY default writes equal to the m16 USB trace (per-address grep), active-symbol values recomputed
  with the vendor integer formula, gpg bank offsets from pinctrl-exynos-arm64.c:646-655 (gpg1 matches the VERIFIED
  EXYNOS_GPIO_GPG1_OFF 0x0A0).
- Corrections to the brief: (a) the DP link block is in BLK_FSYS0, its DT "aclk" is the DPU Q-channel; (b) AUX
  orientation comes from the gpg0-4 GPIO, not a CC-pin register; (c) DECON_T configuration writes DECON_F (DECON0)
  registers (DP_CONNECTION_CONTROL, CLOCK_CONTROL_0 b28, all window position/colour registers).
- Not studied: dpp_reg.c (DPP format/CSC for channel 2), SYSMMU, BTS. No device access, no source edits.
