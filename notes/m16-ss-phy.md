# M16 — USB 3.x SuperSpeed PHY bring-up research (code reading)

Task: find the kernel's SS bring-up sequence for Exynos9820 (beyond0, S10e) so our
Beyond0LteUsbHostDxe can bring up the USBDP combo PHY. Code-reading only.

Sources: LineageOS android_kernel_samsung_exynos9820 @ lineage-18.1
(drivers/phy/samsung/*, drivers/usb/dwc3/dwc3-exynos.c, arch/arm64/boot/dts/exynos/exynos9820.dts,
arch/arm64/configs/exynos9820-beyond0lte_defconfig). Local copies in session scratchpad only.

## Progress log

### 1. Build config (beyond0lte_defconfig)
- CONFIG_PHY_EXYNOS_USBDRD=y, CONFIG_PHY_SAMSUNG_USB_CAL=y (phy-exynos-usb3p1.c),
  CONFIG_PHY_SAMSUNG_USB_GEN2=y (phy-exynos-usbdp-gen2.c), CONFIG_PHY_EXYNOS_DEBUGFS=y.
- phy-exynos-usbdp.c (gen1) is NOT in the Makefile -> not built. Gen2 is the combo PHY driver.
- CONFIG_USB_DWC3_DUAL_ROLE=y, CONFIG_EXYNOS_DISPLAYPORT=y; CONFIG_PHY_EXYNOS_DISPLAYPORT not set.

### 2. DT (exynos9820.dts:9085-9150, base SoC dts; board overlay not in this tree)
phy@10B00000: reg = <0x10b00000 0x200>, <0x10ae0000 0x2800>, <0x10af0000 0x800>;
irqs 0xf5, 0xf6; clocks ext_xtal + aclk(0x11d, same as dwc3 aclk); pmu_offset=0x72c,
pmu_offset_dp=0x704, pmu_mask=0; phy_version=0x301; sub_phy_version=0x400; has_combo_phy=1;
ip_type=0; phy_refsel_clockcore=1 (others 0); common_block_disable=1; is_not_vbus_pad=1;
used_phy_port=0; phyclk_mux="none"; phy_refclk="ext_xtal".
No ss_tune_param / hs_tune_param nodes in the base dts (they would be child nodes; absent here).
dwc3: phys = <phy 0>, <phy 1> ("usb2-phy","usb3-phy"); ldos = 3, ldo_number = <0xa 0xb 0xc>.

### 3. Board overlay (arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_eur_open_25.dts)
- fragment@70 (usbdrd_phy0): status okay, pinctrl-0 = phy-port (gpg0-4, func 0 = input,
  pud 3 = pull-up), `phy,gpio_phy_port = <gpg0 4 0>`, hs_tune_param, ss_tune_param (dts:5118-5128).
- fragment@69 (dwc3): dr_mode otg, maximum-speed super-speed-plus, quirk-frame-length-adjustment
  0x20, u1u2_exitfail, ux_exit_in_px, elastic_buf_mode, usb3_lpm_capable, has-lpm-erratum,
  lpm-nyet-threshold 0, dis-u2-freeclk-exists (dts:5094-5115).
- **ss_tune table** (node `ss_tune`, phandle 0x35, dts:1489-1590), `<device host>`:
  ssrx_sqhs_th 4/4, ssrx_lfps_th 2/2, ssrx_mf_eq_en 1/1, ssrx_mf_eq_ctrl_ss 6/6,
  ssrx_hf_eq_ctrl_ss 0xc/**0xe**, ssrx_mf_eq_ctrl_ssp 6/6, ssrx_hf_eq_ctrl_ssp 0xe/0xe,
  ssrx_dfe1_tap_ctrl 0/**2**, dfe2..dfe5 0/0, ssrx_term_cal 0xb/0xb, sstx_amp 0xb/0xb,
  sstx_deemp 5/5, sstx_pre_shoot 3/3, sstx_idrv_up 7/7, sstx_idrv_dn 0/0, sstx_up_term 3/3,
  sstx_dn_term 3/3. (ss_tune_cnt = 0x14 = 20.) Note the node itself is status="disabled" but the
  driver parses it via phandle regardless (of_parse_phandle, no availability check:
  phy-exynos-usbdrd.c:988-996).
- hs_tune (host): tx_pre_emp 3, tx_vref 0xc, rx_sqrx 5, utmi_clk 1, compdis 7, tx_res 1.

### 4. Orientation source in Linux = GPIO GPG0[4] ("PHY_CON"), not the CCIC
- phy-exynos-usbdrd.c:1157-1192 (pipe3_init): `used_phy_port = gpio_get_value(gpg0-4)`
  (inverted if DT `reverse_con_dir`, absent on beyond0 -> 0). Read at every pipe3_init.
- gpg0 = 5th bank of pinctrl@10430000 (gpp0,gpp1,gpp2,gpp3,gpg0: exynos9820.dts:1190-1226)
  -> INFERRED GPG0CON = 0x10430080, GPG0DAT = 0x10430084 (bank stride 0x20, Samsung standard;
  verify against Exynos9820.h / a live read before use).
- Who drives gpg0-4 is not in the kernel (input with pull-up) - INFERRED external Type-C
  mux / redriver orientation output. Cross-check with MAX77705 CC_STATUS0 CCPinStat on device.

### 5. PMU isolation (phy-exynos-usbdrd.c:394-405, 1589-1608, 1911-1932)
- pmu_mask DT = 0 -> code does `pmu_mask = BIT(0)` (usbdrd.c:1932).
- PIPE3 instance power_on: `regmap_update_bits(pmu, 0x704, BIT0, BIT0)` -> PMU 0x15860704 bit0 = 1
  (de-isolate). UTMI instance does the same on 0x72C. No delay after either.
- PIPE3 init order (usbdrd.c:1194-1239): g2_pma_ready -> usbdp_g2_enable -> g2_pma_sw_rst_release
  -> poll check_pll_lock (100 iters, no delay in loop) -> run_lane -> poll rx_cdr_pll_lock
  (100 iters) -> g2_link_pclk_sel. On failure: utmi_exit (disables both PHYs + clocks).

### 6. USBCON (0x10B00000) writes in the PIPE3 path (phy-exynos-usb3p1.c; bits phy-exynos-usb3p1-reg.h:24-77,154-169)
LINK_CTRL 0x04: PIPE3_FORCE_EN b16, FORCE_PHY_STATUS b17, FORCE_RX_ELEC_IDLE b18.
CLKRST 0x20: LINK_PCLK_SEL b7, LINK_SW_RST b0. COMBO_PMA_CTRL 0x48: ROPLL_REF_REQ[13:12],
PLL_REF_REQ[11:10], REF_FREQ[9:8], LOW_PWRN b4 (set = PMA powered DOWN; pipe_ovrd comment "PMA Disable"),
TRSV_SW_RST b3, CMN_SW_RST b2, INIT_SW_RST b1, APB_SW_RST b0.
- UTMI init (runs first, usb3p1.c:349-364 pipe_ovrd): LINK_CTRL |= b16|b18, &= ~b17; PMA_CTRL |= b4.
  (= what our UsbPhyPipe3Override does today.)
- g2_pma_ready (usb3p1.c:221-256):
  1. udelay(100)
  2. PMA_CTRL RMW: clear [13:12],[11:10]; |= REF_FREQ_SET(1) (=0x100, NOT masked first) | b4 | b3 | b2 | b1 | b0
  3. udelay(1); PMA_CTRL &= ~b4 (LOW_PWRN off = PMA powered up); udelay(1)
  4. LINK_CTRL &= ~b16 (PIPE3_FORCE_EN off; b18 FORCE_RX_ELEC_IDLE is left set but is a don't-care
     once FORCE_EN=0 - INFERRED); udelay(1)
  5. PMA_CTRL &= ~b0 (APB_SW_RST release -> PMA/PCS APB registers now accessible - INFERRED from name)
- [usbdp_g2_enable: PMA + PCS programming, see 7]
- g2_pma_sw_rst_release (usb3p1.c:304-324): PMA_CTRL &= ~b1 (INIT); udelay(1) ("spec 200 ns");
  PMA_CTRL &= ~(b3|b2) (TRSV, CMN = "run pll"); udelay(1000).
- [poll LCPLL lock, run_lane, poll CDR lock]
- g2_link_pclk_sel (usb3p1.c:326-337): CLKRST |= b7; mdelay(3). LAST step - only after PLL+CDR lock.
  Explains our dead link clock: PCLK_SEL switches the link's pipe clock to the PMA-generated PCLK.
- expected final PMA_CTRL = 0x00000100 (REF_FREQ=1, everything released) INFERRED from sequence
  assuming reset value bits [9:8]=0.
- g2_disable (usb3p1.c:258-283): CLKRST &= ~b7 FIRST, then PMA_CTRL |= b4|b3|b2|b1|b0, udelay(1),
  release b3..b0 (leaves LOW_PWRN set). This is the safe teardown order.

### 7. USBDP combo PMA (0x10AE0000) + PCS (0x10AF0000) - exact trace
Method: compiled phy-exynos-usbdp-gen2.c (non-EVT0, CONFIG_SOC_EXYNOS9820_EVT0 unset in beyond0
defconfig:437) on the host with readl/writel stubbed, tune table = DT host column, run twice with
fake reads 0x0 and 0xFFFFFFFF to separate set bits from preserved bits ("keep"). Trace is exact for
the kernel code; reset values of RMW registers are unknown (need the live regdump).
Source lines: enable = gen2.c:519-703 (fixed writes), 1046 (tune), 1048-1058 (PCS_*), 1060
(set_pcs 479-517), 1068-1078 (CDR FBB), 1081-1191 (lane mux by used_phy_port).
```
== enable
WR PMA+0x0830 = 0x07
WR PMA+0x1830 = 0x07
WR PMA+0x085c = 0x80
WR PMA+0x185c = 0x80
WR PMA+0x1030 = 0x07
WR PMA+0x2030 = 0x07
WR PMA+0x105c = 0x80
WR PMA+0x205c = 0x80
WR PMA+0x0228 = 0x38
WR PMA+0x0104 = 0x44
WR PMA+0x0248 = 0x44
WR PMA+0x038c  RMW set=0x02 keep=0xfffffff1
WR PMA+0x0878 = 0x04
WR PMA+0x1878 = 0x04
WR PMA+0x09a4 = 0x07
WR PMA+0x19a4 = 0x07
WR PMA+0x09a8 = 0x22
WR PMA+0x19a8 = 0x22
WR PMA+0x0994 = 0x12
WR PMA+0x1994 = 0x12
WR PMA+0x0998 = 0x12
WR PMA+0x1998 = 0x12
WR PMA+0x099c = 0x48
WR PMA+0x199c = 0x48
WR PMA+0x0898 = 0x77
WR PMA+0x1898 = 0x77
WR PMA+0x0054 = 0x01
WR PMA+0x00e0 = 0x38
WR PMA+0x0060 = 0x24
WR PMA+0x0064 = 0x77
WR PMA+0x0070 = 0x76
WR PMA+0x09b8  RMW set=0x40 keep=0xffffff0f
WR PMA+0x19b8  RMW set=0x40 keep=0xffffff0f
WR PMA+0x09c0  RMW set=0x04 keep=0xfffffff1
WR PMA+0x19c0  RMW set=0x04 keep=0xfffffff1
WR PMA+0x0904  RMW set=0x70 keep=0xffffff87
WR PMA+0x1904  RMW set=0x70 keep=0xffffff87
WR PMA+0x0c44  RMW set=0x06 keep=0xffffffe0
WR PMA+0x1c44  RMW set=0x06 keep=0xffffffe0
WR PMA+0x0a34 = 0x08
WR PMA+0x1a34 = 0x08
WR PMA+0x0a34 = 0x18
WR PMA+0x1a34 = 0x18
WR PMA+0x0a34 = 0x1e
WR PMA+0x1a34 = 0x1e
WR PMA+0x0c2c  RMW set=0x0e keep=0xffffffe0
WR PMA+0x1c2c  RMW set=0x0e keep=0xffffffe0
WR PMA+0x0a34 = 0x08
WR PMA+0x1a34 = 0x08
WR PMA+0x0a34 = 0x18
WR PMA+0x1a34 = 0x18
WR PMA+0x0a34 = 0x1e
WR PMA+0x1a34 = 0x1e
WR PMA+0x0c48  RMW set=0x06 keep=0xffffffe0
WR PMA+0x1c48  RMW set=0x06 keep=0xffffffe0
WR PMA+0x0a34 = 0x08
WR PMA+0x1a34 = 0x08
WR PMA+0x0a34 = 0x18
WR PMA+0x1a34 = 0x18
WR PMA+0x0a34 = 0x1e
WR PMA+0x1a34 = 0x1e
WR PMA+0x0c30  RMW set=0x0e keep=0xffffffe0
WR PMA+0x1c30  RMW set=0x0e keep=0xffffffe0
WR PMA+0x0a34 = 0x08
WR PMA+0x1a34 = 0x08
WR PMA+0x0a34 = 0x18
WR PMA+0x1a34 = 0x18
WR PMA+0x0a34 = 0x1e
WR PMA+0x1a34 = 0x1e
WR PMA+0x09e4  RMW set=0x02 keep=0xffffff00
WR PMA+0x19e4  RMW set=0x02 keep=0xffffff00
WR PMA+0x0a34 = 0x08
WR PMA+0x1a34 = 0x08
WR PMA+0x0a34 = 0x18
WR PMA+0x1a34 = 0x18
WR PMA+0x0a34 = 0x1e
WR PMA+0x1a34 = 0x1e
WR PMA+0x09e8  RMW set=0x00 keep=0xffffff00
WR PMA+0x19e8  RMW set=0x00 keep=0xffffff00
WR PMA+0x09ec  RMW set=0x00 keep=0xffffff00
WR PMA+0x19ec  RMW set=0x00 keep=0xffffff00
WR PMA+0x09f0  RMW set=0x00 keep=0xffffff00
WR PMA+0x19f0  RMW set=0x00 keep=0xffffff00
WR PMA+0x09f4  RMW set=0x00 keep=0xffffff00
WR PMA+0x19f4  RMW set=0x00 keep=0xffffff00
WR PMA+0x0af4  RMW set=0x1b keep=0xffffffe0
WR PMA+0x1af4  RMW set=0x1b keep=0xffffffe0
WR PMA+0x1010  RMW set=0x2b keep=0xffffffc0
WR PMA+0x2010  RMW set=0x2b keep=0xffffffc0
WR PMA+0x1014  RMW set=0x15 keep=0xffffffe0
WR PMA+0x2014  RMW set=0x15 keep=0xffffffe0
WR PMA+0x1018  RMW set=0x4c keep=0xffffff83
WR PMA+0x2018  RMW set=0x4c keep=0xffffff83
WR PMA+0x101c  RMW set=0xe0 keep=0xffffff1f
WR PMA+0x201c  RMW set=0xe0 keep=0xffffff1f
WR PMA+0x041c  RMW set=0x00 keep=0xfffffff8
WR PMA+0x0420  RMW set=0x00 keep=0xfffffff8
WR PMA+0x1080  RMW set=0x30 keep=0xffffff0f
WR PMA+0x2080  RMW set=0x30 keep=0xffffff0f
WR PMA+0x1080  RMW set=0x03 keep=0xfffffff0
WR PMA+0x2080  RMW set=0x03 keep=0xfffffff0
WR PMA+0x0310 = 0x00
WR PMA+0x0308 = 0x00
WR PCS+0x0304  RMW set=0x01 keep=0xffc0ffc0
WR PCS+0x0318  RMW set=0x20000 keep=0xfffdffff
WR PCS+0x030c  RMW set=0x00 keep=0xfffffffe
WR PCS+0x014c  RMW set=0x100 keep=0xfffffcff
WR PCS+0x0150  RMW set=0x4000 keep=0xffff3fff
WR PMA+0x0a98  RMW set=0x20 keep=0xffffff0f
WR PMA+0x1a98  RMW set=0x20 keep=0xffffff0f
WR PMA+0x0258 = 0x00
WR PMA+0x0288 = 0xc0
WR PMA+0x028c = 0x18
WR PMA+0x1000 = 0x08
WR PMA+0x2000 = 0x28
WR PMA+0x0af0  RMW set=0x00 keep=0xffffffe3
== run_lane
WR PMA+0x031c = 0x00
WR PMA+0x0328 = 0x00
WR PMA+0x032c = 0x00
```
used_phy_port=1 differs only in:
```
113c113
< WR PMA+0x0288 = 0xc0
---
> WR PMA+0x0288 = 0x30
115,117c115,117
< WR PMA+0x1000 = 0x08
< WR PMA+0x2000 = 0x28
< WR PMA+0x0af0  RMW set=0x00 keep=0xffffffe3
---
> WR PMA+0x1000 = 0x28
> WR PMA+0x2000 = 0x08
> WR PMA+0x1af0  RMW set=0x00 keep=0xffffffe3
```
Decoding (gen2-reg.h): A0258 (0x258)=0: OVRD_CMN_RATE b5=0, LCPLL_ALONE_MODE b2=0.
A0288 (0x288) = LANE_MUX_SEL_DP[7:4] | DP_LANE_EN[3:0]: port0 0xC0 ("usb rx0, tx1, dp tx2, tx3"),
port1 0x30 ("dp rx0, tx1, usb tx2, tx3"); DP_LANE_EN = 0 in both (DP lanes off).
A028C (0x28C) = 0x18: OVRD_RX_CDR_DATA_MODE_EXIT b4 | RX_CDR_DATA_MODE_EXIT b3.
0x1000 (TRSV_0400, LN1 TX) / 0x2000 (TRSV_0800, LN3 TX): 0x08 = BEACON_LFPS_SYNC_EN on the USB TX lane;
0x28 = SYNC_EN | OVRD_..._BEACON_LFPS_OUT_EN(b5)=0... (the lane NOT used for USB gets OVRD b5 with value 0
= beacon/LFPS output forced off) - INFERRED reading of the bit names.
0x0af0 / 0x1af0: SIGVAL LPF delay = 0 on the USB RX lane (LN0 or LN2).
A0310 (0x310)=0 / A0308 (0x308)=0: drop all PCS overrides (PLL_EN, LANE_RSTN, INIT_RSTN, BIAS, BGR, PM_STATE)
-> PCS hardware controls the PMA. A038C RMW: clears DP_INIT_RSTN b3, DP_CMN_RSTN b2 (DP held in reset),
sets CDR_WATCHDOG_EN b1.
run_lane (gen2.c:414-477): 0x31C=0, 0x328=0, 0x32C=0 (drop TX_DRV_EN / TX_ELECIDLE / TX_SER, RX_LFPS,
RCV_DET overrides -> PCS drives them).
PCS (0x10AF0000, gen2.c:479-517): +0x304 EBUF_PARAM: clear NUM_INIT_BUFFERING + SKP_REMOVE_TH, SKP_REMOVE_TH=1;
+0x318 EBUF_DRAINER |= b17 (EN_MASK_TSEQ); +0x30C BACK_END_MODE_VEC &= ~b0 (DISABLE_DATA_MASK);
+0x14C OUT_VEC_2: clear b9 (DYN_CON_DESERIAL), set b8 (SEL_OUT_DESERIAL);
+0x150 OUT_VEC_3: clear b15 (DYN_CON_PMA_TX_KEEPER), set b14 (SEL_OUT_PMA_TX_KEEPER).
Tune "post setting" (gen2.c:356-365): after each of mf/hf eq ss/ssp + dfe1, writes 0x08,0x18,0x1E
to 0x0A34 and 0x1A34 (both RX lanes, regardless of port).

### 8. Lock polling (gen2.c:47-75, 392-412; usbdrd.c:1201-1233)
- LCPLL: PMA+0x0350 (CMN_A0350) bit7 ANA_LCPLL_LOCK_DONE (bit6 AFC_DONE). Inner loop 1000 x udelay(1),
  outer loop in usbdrd 100x -> worst case ~100 ms. Polled after g2_pma_sw_rst_release (which already
  waited 1 ms).
- CDR: port0 PMA+0x0B84 bit0 LN0_MON_RX_CDR_LOCK_DONE; port1 PMA+0x1B84 bit0 LN2_... Same 1000x1us x100.
  Polled after run_lane, BEFORE LINK_PCLK_SEL. No link partner needed at that point in Linux (pipe3_init
  runs at dwc3 core init, cable may be absent) -> INFERRED: CDR locks to the reference in FLD/PLL
  mode without data (bits 1/3 FLD_PLL_MODE_DONE / AFC_DONE are the intermediate flags).
- On timeout Linux tears everything down (utmi_exit -> usb3p1_disable + usbdp_g2_disable + clk off) and
  just logs "pll lock is fail!!" - it does NOT set LINK_PCLK_SEL. Copy that: never set PCLK_SEL without
  both locks.

### 9. Call order from DWC3 (drivers/usb/dwc3/core.c, LineageOS)
dwc3_core_soft_reset (core.c:403-450): GUSB3PIPECTL(0x10C0C2C0) &= ~b17 SUSPHY; GUSB2PHYCFG &= ~SUSPHY;
phy_power_on(usb3) = PMU 0x704 b0=1; phy_power_on(usb2) = PMU 0x72C b0=1; phy_init(usb2) = utmi_init
(v3p1_enable + pipe_ovrd); phy_init(usb3) = pipe3_init (sections 6-8). Then dwc3_phy_setup (core.c:779-835),
then phy_tune(usb2), phy_tune(usb3, otg state) (core.c:1087-1093): **tune is applied AFTER init and after
LINK_PCLK_SEL**. On the very first init the tune_param .value fields are still 0 (kzalloc,
usbdrd.c:840-882 only fills ss_tune_param_value[][]), so the first enable writes zeros through the tune
path and the host column is applied afterwards by pipe3_tune (usbdrd.c:1397-1421). Steady state =
host column = the trace in section 7. INFERRED: writing host values directly inside enable gives the
same end state.
GUSB3PIPECTL bits (core.h:288-303): dwc3_phy_setup sets b27 UX_EXIT_PX (DT quirk), b17 SUSPHY (rev>1.94a),
b25 U1U2EXITFAIL_TO_RECOV (DT snps,u1u2_exitfail_quirk); dwc3_core_config (core.c:236-370, called
core.c:1160) sets b28 DISRXDETINP3 (rev>=2.50a - DWC31 revisions compare greater), b27, b0
ELASTIC_BUFFER_MODE; LLUCTL 0xD024 |= PENDING_HP_TIMER(0xB)<<16 | b15 EN_US_HP_TIMER | b10 **FORCE_GEN1**
(rev > USB31 1.20a -> our 1.70a => Linux runs Gen1 5 Gbps only); LSKIPFREQ 0xD020 |= 9<<20 | 5<<24 | b27.
Live value (TWRP, both device and host-with-dock): GUSB3PIPECTL(0) = 0x1B021103
(b28,b27,b25,b24,b17,b12,b8,b1,b0; b24 REQP1P2P3 / b12 / b8 RX_DETOPOLL / b1 TX_DEEPH=1 are not
written by this code -> reset defaults, INFERRED).

### 10. Live USBCON cross-check (s10e-artifacts/twrp-usb-regs-20261011/hostlog-dock.txt:9-31, phy-regmap.txt)
Host with SS dock: LINKCTRL 0x00043FF0 (b18 FORCE_RX_ELEC_IDLE still 1, b16 FORCE_EN 0 - confirms
g2_pma_ready leaves b18), CLKRST 0x00001084 (b7 LINK_PCLK_SEL=1, b2 PHY_RESET_SEL, b12), SECPMACTL(0x48)
= 0x00000100 (= predicted final value: REF_FREQ=1, LOW_PWRN 0, all resets released), SSPPLLCTL 0x30 = 6,
LTSTATE_HIS 0x000F4545. Device mode shows the same LINKCTRL/CLKRST/0x48 -> SS PHY is up in both roles.

### 11. usbdp debugfs dump: already captured, but tiny
usbdp-device-regdump.txt exists (device mode). phy-exynos-dp-debugfs.c:38-49 only dumps 10 tune regs:
TRSV_0241/0641 (PMA 0x0904/0x1904), 0311/0711 (0x0C44/0x1C44), 030B/070B (0x0C2C/0x1C2C),
0279/0679 (0x09E4/0x19E4), 02BD/06BD (0x0AF4/0x1AF4). Device values: 0x71,0x71,6,6,0xC,0xC,0,0,0x1B,0x1B.
Consistent with trace (0x0904: OR 0x70, clear b3 -> reset low bits 0x1; 0x0AF4 = 0x1B).
Host-mode expected: 0x0C2C/0x1C2C = 0x0E, 0x09E4/0x19E4 = 0x02, rest same. Useful only as a tune
spot check; no PLL/lane/CMN coverage. A host-mode capture would add nothing beyond these 2 deltas.

### 12. Risks / safe detection (INFERRED unless cited)
- PMU 0x15860704 b0 is the single isolation bit for the whole combo PHY (DP driver reads the same bit,
  notes/d6-external-display.md). Read it first (PMU always on). Touching 0x10AE0000/0x10AF0000 while
  isolated or while USBCON 0x48 APB_SW_RST (b0) is asserted is the most likely bus-hang -> only read
  PMA after step g2_pma_ready.5; first PMA access should be a single read of +0x0350.
- Clocks: PHY DT clocks are only OSCCLK + aclk(0x11D, same gate as the DWC3 aclk), phyclk_mux="none"
  (usbdrd.c:129-170) -> no extra CMU user-mux, no ACPM clock in the DT path. Our link already runs, so
  the APB clock is present. PMA refclk = "clockcore" (phy_refsel_clockcore=1) - provenance not in
  this driver; if LCPLL never locks, that and LDO10/11/12 (runtime-switched by phy_conn,
  usbdrd.c:1459-1517) are the suspects.
- Never set CLKRST b7 before both LCPLL (0x0350 b7) and CDR (0x0B84/0x1B84 b0) locks; on any
  timeout run g2_disable order (clear b7 first, then PMA resets + LOW_PWRN).
- Orientation: read GPG0DAT b4 (INFERRED 0x10430084; gpp1 CON = 0x10430020 is device-verified in
  notes/m12-touch.md, so stride 0x20 / gpg0 = bank 4 is strong). CCIC CCPinStat should agree; if it
  doesn't, trust the GPIO (that's what Linux uses). Wrong port -> CDR poll on the wrong lane times out
  (safe failure).
