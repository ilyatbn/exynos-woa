# D1 — DWC3 host mode on Exynos 9820: research notes

Plan-of-record task list: [`docs/exynos9820-port/DRIVERS-MILESTONES.md`](../docs/exynos9820-port/DRIVERS-MILESTONES.md) §D1.
Findings are appended **as they are discovered**, facts carry source URLs, inference is
marked, dead ends are recorded. Do not rewrite earlier sections.

## Questions the research must answer

1. **Linux/Android bring-up sequence** — in what order does Samsung's 9820 kernel
   (and mainline) initialise the USB host path: CMU gates (PERIC0/FSYS0A), the PHY
   node (`10B00000` USB2 / `10AE0000` combo / `10AF0000` PCS), the `usbfn` glue at
   `10C00000`, and the `dwc3` child (`dr_mode`, VBUS, role)? What registers does each
   step touch?
2. **DWC3 → host mode** — exact register sequence on DWC3 (GCTL/DCTL `PRTCAP_HOST`,
   `dwc3_core_init`, xHCI enable), and anything Exynos-specific around it (suspension,
   refclk, `xhci_l2_support`, `enable_sprs_transfer`).
3. **Mu-Silicium `UsbfnDwc3Dxe`** — what does it actually implement, for which SoCs,
   and why is USB Host still ❌ on Exynos targets? Is it usable as a base for 9820?
4. **edk2-exynos (exynos9820 UEFI port)** — does it have any DWC3/USB host driver at
   all, working or not?
5. **VBUS and the connector** — who drives VBUS 5 V out of the S10e's USB-C port in
   host mode (PMIC? a `vbus` regulator in the DT? the DWC3 core?), and what does a
   **non-powered hub** draw (power budget)?
6. **Windows half (preview for D3)** — how do WoA targets expose a memory-mapped xHCI
   to Windows (ACPI `_HID`/`_CID`, `USBXHCI.sys` binding)? Just a pointer-level answer;
   D3 gets its own pass.

## Progress log

(append below)

### Q1 — Linux/Android bring-up sequence

- Started. Context already in this repo (`notes/dock-desktop.md` §5, §8, §9): vendor DT has
  `usb@10C00000` (wrapper, `status="disabled"`) + `phy@10B00000` (3 windows: 10b00000/0x200,
  10ae0000/0x2800, 10af0000/0x800), `dr_mode="otg"`, `xhci_l2_support=1`,
  `enable_sprs_transfer=1`, `is_not_vbus_pad=1`, PHY `phy_version=0x301`. Not repeated below.
- *(fact, dead end)* `github.com/Samsung/android_kernel_samsung_exynos9820` → **404** via API
  (Samsung never published it under that org name). Usable primary source instead:
  **`LineageOS/android_kernel_samsung_exynos9820`, branch `lineage-23.2`**
  (https://github.com/LineageOS/android_kernel_samsung_exynos9820) — the downstream Samsung
  kernel for beyond0lte. Local copies under `/tmp/opencode/d1/`.
- *(fact)* The 9820 glue is **`drivers/usb/dwc3/dwc3-exynos.c`** (742 lines), NOT the 9810-era
  `drivers/usb/misc/usbfn.c` — there is no usbfn.c in this tree. SoC DT
  (`arch/arm64/boot/dts/exynos/exynos9820.dts:4258`):
  `udc: usb@10C00000` compatible `"samsung,exynos-dwusb"`, `clocks = <&clock GATE_USB31DRD_SLV_LINK>, <&clock USB31DRD>`
  (`clock-names = "aclk", "sclk"`), `samsung,power-domain = <&pd_fsys0a>`, `status="disabled"`.
- *(fact)* dwc3 child (`exynos9820.dts:4269`): compatible `"synopsys,dwc3"` (downstream spelling),
  same reg `0x10C00000 0x10000`, `tx-fifo-resize=<0>`, `is_not_vbus_pad=<1>`,
  `enable_sprs_transfer=<1>`, `xhci_l2_support=<1>`, `ldos=<3>`, `ldo_number=<10 11 12>`,
  `usb-pm-qos-int=<400000>`, `phys = <&usbdrd_phy0 0>, <&usbdrd_phy0 1>`.
- *(fact)* PHY node `usbdrd_phy0: phy@10B00000` compatible **`"samsung,exynos-usbdrd-phy"`** →
  driver `drivers/phy/samsung/phy-exynos-usbdrd.c` (2121 lines). Properties:
  `samsung,pmu-syscon = <&pmu_system_controller>`, `pmu_mask=<0x0>`, **`pmu_offset=<0x72c>`**,
  **`pmu_offset_dp=<0x704>`**, `phy_version=<0x301>`, `phyclk_mux="none"`, `phy_refclk="ext_xtal"`,
  `has_combo_phy=<1>`, `sub_phy_version=<0x400>`, `ip_type=<0x0>` (USB3DRD),
  `phy_refsel_clockcore=<1>` (refclk source = clockcore, not xtal), `use_io_for_ovc=<0>`,
  `common_block_disable=<1>`, `is_not_vbus_pad=<1>`. Clocks: `OSCCLK` ("ext_xtal", 26 MHz per
  `fixed-rate-clocks` in the board dtsi) + `GATE_USB31DRD_SLV_LINK` ("aclk").
- *(fact)* Board overlay `arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi:1290`:
  `&udc status="okay"`, `&usbdrd_phy0 status="okay"` (LineageOS enables both; the *shipping*
  Samsung DT keeps them disabled — dock-desktop §5). dwc3 gets `dr_mode="otg"`,
  `maximum-speed="super-speed-plus"`, `snps,quirk-frame-length-adjustment=<0x20>`,
  `snps,u1u2_exitfail_quirk`, `snps,ux_exit_in_px_quirk`, `snps,elastic_buf_mode_quirk`,
  `snps,usb3_lpm_capable`, `snps,has-lpm-erratum`, `snps,lpm-nyet-threshold=<0>`,
  `adj-sof-accuracy=<1>`, `snps,dis-u2-freeclk-exists-quirk` (chosen instead of
  `snps,dis_u2_susphy_quirk` — "one of them should be selected" per the DT comment).
- *(fact)* PHY gets `phy,gpio_phy_port = <&gpg0 4 0x0>` + pinctrl `phy_port` — a GPIO (GPG0_4)
  muxes the combo PHY port (USB vs DP path); plus `hs_tune_param`/`ss_tune_param` with
  per-signal tune values given as `<device host>` pairs (e.g. `tx_pre_emp <0x3 0x3>`,
  `tx_res <0x3 0x1>` — host value differs for tx_res/rx_sqrx).
- *(fact)* `usb_notifier` node (`samsung,usb-notifier`, `usb-ldocontrol=<3>` =
  "USB_LDOCONTROL_EXYNOS9820", `udc=<&udc>`) sits on the PMU side and manages the USB LDOs
  named by `ldo_number=<10 11 12>` on the dwc3 child. Driver file not yet located in the tree
  (search `usb_notifier` in tree listing found nothing under drivers/ — likely compiled from
  `drivers/samsung/exynos-pm` or similar; **open**).
- *(fact)* **Glue probe order** — `dwc3_exynos_probe` (`drivers/usb/dwc3/dwc3-exynos.c:488`):
  1. `dma_set_mask(dev, DMA_BIT_MASK(36))`
  2. `dwc3_exynos_clk_get/prepare/enable` — every clock in DT `clock-names` (9820: `aclk` =
     GATE_USB31DRD_SLV_LINK, `sclk` = USB31DRD; CMU gates)
  3. `pm_runtime_set_active/enable` (genpd `pd_fsys0a` via `samsung,power-domain`)
  4. optional regulators `vdd33`/`vdd10` (absent in 9820 DT → skipped)
  5. `dwc3_exynos_register_phys` (DT-PHY case: no-op)
  6. `of_platform_populate` → instantiates the `synopsys,dwc3` child → `dwc3_probe`.
- *(fact)* **dwc3 core probe order** (`drivers/usb/dwc3/core.c:1608`, downstream):
  `platform_get_resource(MEM,0)` → `xhci_resources[0] = res->start .. +DWC3_XHCI_REGS_END(0x7fff)`
  → `res->start += DWC3_GLOBALS_REGS_START(0xc100)` → ioremap globals →
  `dwc3_get_properties` → `dma_set_mask(36)` → `dwc3_cache_hwparams` → alloc event/scratch
  buffers → pm_runtime → `dwc3_core_init` → `dwc3_core_init_mode`.
  Register-offset convention: core.h offsets are absolute from the block base; `io.h`
  `dwc3_readl(base,off) = readl(base + off - DWC3_GLOBALS_REGS_START)` — so `GCTL` lives at
  **0x10C00000 + 0xc110 = 0x10C00C110**.
- *(fact)* **PHY driver** = `drivers/phy/samsung/phy-exynos-usbdrd.c` (+ `phy-exynos-usb3p1.c`
  1687 l, `phy-exynos-usbdp-gen2.c` 1254 l, reg headers). Window map from probe:
  `reg_phy` = window0 **0x10B00000** (0x200) = "USBCON" control block (`usbphy_info.regs_base`);
  `reg_phy2` = window1 **0x10AE0000** (0x2800) = USBDP combo PHY main (`usbphy_sub_info.regs_base`);
  `reg_phy3` = window2 **0x10AF0000** (0x800) = PCS (`regs_base_2nd`). Two phy instances:
  index 0 = UTMI (usb2-phy), index 1 = PIPE3 (usb3-phy) — matches `phys = <&usbdrd_phy0 0>, <&usbdrd_phy0 1>`.
- *(fact)* PHY probe also: requests 2 IRQs (USB2_REMOTE_WAKEUP, USB2_REMOTE_CONNECT,
  `IRQ_NOAUTOEN` + `irq_set_irq_wake`), gets clocks `ext_xtal`(OSCCLK 26 MHz)/`aclk`,
  looks up `samsung,pmu-syscon` regmap, reads `pmu_offset=0x72c`, `pmu_offset_dp=0x704`,
  `pmu_mask=0` → **`pmu_mask = BIT(0)`** (code: `pmu_mask = (u32)BIT(pmu_mask)`), requests
  GPIO `phy,gpio_phy_port` (GPG0_4) as **input** ("PHY_CON" — reads the USB/DP mux state), and
  **requires regulators `m_vdd_ldo10/11/12`** (`regulator_get`, returns -ENODEV if missing).
  Those are S2MP200 main-PMIC LDOs (`&speedy` bus `speedy@15970000`,
  `compatible="samsung,exynos-speedy"`; PMIC MFD `samsung,s2mps19mfd`): LDO10 0.7–1.1 V,
  LDO11 1.6–1.95 V, LDO12 1.7–3.35 V, all **`regulator-boot-on`**
  (`exynos9820-beyond0lte_common.dtsi:1570-1595`).
- *(fact)* PHY `power_on` = optional vbus regulator (NULL on 9820) then **de-isolate via PMU
  syscon**: `regmap_update_bits(pmu, pmu_offset, mask, val)` with `val = on ? 0 : mask`;
  called as `phy_isol(inst, 0, pmu_mask)` → sets **PMU 0x15860000+0x72c bit0 = 1** for UTMI
  (`EXYNOS_USBDRD_ENABLE BIT(0)`, `EXYNOS_USBHOST_ENABLE BIT(1)` in
  `phy-exynos-usbdrd.h:20-25`) and **PMU +0x704 bit0 = 1** for PIPE3.
- *(fact)* PHY `init` (per instance):
  - UTMI (`exynos_usbdrd_utmi_init`): clk_enable → `phy_exynos_usb_v3p1_enable` →
    `phy_exynos_usb_v3p1_pipe_ovrd` → OTP cal writes (`samsung_exynos_cal_usb3phy_write_register`).
  - PIPE3 (`exynos_usbdrd_pipe3_init`): read PHY_CON GPIO → `phy_exynos_usb_v3p1_g2_pma_ready`
    → `phy_exynos_usbdp_g2_enable` → `phy_exynos_usb_v3p1_g2_pma_sw_rst_release` →
    poll `phy_exynos_usbdp_g2_check_pll_lock` (≤100 iters) → `phy_exynos_usbdp_g2_run_lane` →
    poll `phy_exynos_usbdp_g2_rx_cdr_pll_lock` → `phy_exynos_usb_v3p1_g2_link_pclk_sel`.
- *(fact)* USBCON block register map (`phy-exynos-usb3p1-reg.h`, offsets from 0x10B00000):
  `LINK_CTRL 0x04` (`PIPE3_FORCE_EN bit16`, `..._FORCE_RX_ELEC_IDLE`), `CLKRST 0x20`
  (`LINK_PCLK_SEL bit7`, `LINK_SW_RST bit0`), `COMBO_PMA_CTRL 0x48`
  (`PMA_ROPLL_REF_REQ 13:12`, `PMA_PLL_REF_REQ 11:10`, `PMA_REF_FREQ_SET(1) bits 9:8`,
  `LOW_PWRN bit4`, `TRSV_SW_RST bit3`, `CMN_SW_RST bit2`, `INIT_SW_RST bit1`, `APB_SW_RST bit0`),
  `UTMI 0x50` (FORCE_SUSPEND/FORCE_SLEEP/DP_PULLDOWN/DM_PULLDOWN), `HSP 0x54`
  (`EN_UTMISUSPEND bit9`, `COMMONONN bit8` — set because DT `common_block_disable=1`).
  `phy_exynos_usb_v3p1_g2_pma_ready` sequence: hold all PMA resets + ref_freq_set(1) + LOW_PWRN,
  drop LOW_PWRN, clear LINKCTRL_PIPE3_FORCE_EN, release APB_SW_RST.
  `g2_pma_sw_rst_release`: release INIT_SW_RST (udelay 1 = spec 200 ns), release TRSV+CMN
  (udelay 1000). `g2_link_pclk_sel`: CLKRST bit7=1 then `mdelay(3)`.
  `pipe_ovrd` (UTMI-only path): force pipe3 + RX_ELEC_IDLE in LINK_CTRL and set PMA LOW_PWRN
  (i.e. SS PMA stays powered down until PIPE3 init runs).
- *(fact)* `phy_conn` API (`exynos_usbdrd_phy_conn`, called by dwc3_probe end with 0 =
  "Disable LDO") toggles LDO10/11/12 on/off via `exynos_usbdrd_ldo_control` + `mdelay(1)`.
  So the LDOs are runtime-switchable, not just boot-on.

### Q2 — DWC3 core → host mode exact sequence

- *(fact)* `dwc3_core_init` (`drivers/usb/dwc3/core.c:1057`, downstream) order:
  1. `dwc3_core_is_valid` (GSNPSID 0xc120: `[31:16]==0x5533` "U3" → DWC_usb3, revision=GSNPSID;
     `==0x3331` → DWC_usb31, revision from `DWC3_VER_NUMBER` 0xc1a0; else -ENODEV)
  2. write `LINUX_VERSION_CODE` to `DWC3_GUID` (0xc128)
  3. GHWPARAMS3 SSPHY_IFC==DIS → cap speed to HS
  4. `dwc3_core_ulpi_init`, `dwc3_core_get_phy`
  5. **`dwc3_core_soft_reset`**: `phy_reset(usb2_generic_phy)`; clear SUSPHY in
     `GUSB3PIPECTL(0)` (0xc2c0) and `GUSB2PHYCFG(0)` (0xc200); `phy_power_on(usb3)` then
     `phy_power_on(usb2)`; `usb_phy_init`×2; `phy_init(usb2)`; `phy_init(usb3)`;
     **if `dr_mode == HOST` it returns before touching `DCTL`** — no `CSFTRST`; the xHCI
     driver's HCRESET resets the host block instead.
  6. `dwc3_phy_setup` (0xc2c0/0xc200 RMW: SUSPHY set for rev>1.94a, quirk bits, UTMI width
     PHYIF/USBTRDTIM)
  7. `phy_tune` (both phys, state 0)
  8. `dwc3_core_setup_global_control` — GCTL (0xc110): clock-gating/`SOFITPSYNC` decision by
     GHWPARAMS1 EN_PWROPT + revision, hibernation `GBLHIBERNATIONEN`, scramble,
     `dis_u2_freeclk_exists_quirk → GCTL_SOFITPSYNC`, `adj_sof_accuracy → clear SOFITPSYNC`
  9. `dwc3_core_num_eps`; then a **dead-code block**: computes
     `GCTL_PWRDNSCALE(50 MHz/16k)` into a local `reg` but **never writes it** (downstream bug,
     harmless)
  10. `dwc3_setup_scratch_buffers`, `dwc3_frame_length_adjustment`
     (`GFLADJ 0xc630`: `GFLADJ_30MHZ_SDBND_SEL | fladj` when `snps,quirk-frame-length-adjustment`)
  11. `usb_phy_set_suspend(usb2/usb3, 0)`
  12. `dwc3_event_buffers_setup`: `GEVNTADRLO/HI(0)` (0xc400/0xc404), `GEVNTSIZ(0)` (0xc408),
     `GEVNTCOUNT(0)` (0xc40c)
  13. revision quirks: `GUCTL2 (0xc19c) RST_ACTBITLATER` (≥3.10a), `GUCTL1 (0xc11c)` bits
     (≥2.50a), `GUCTL3 (0xc60c) USB20_RETRY_DISABLE` (≥DWC31 1.70a)
  14. **`dwc3_core_config`** (Samsung downstream, core.c:240): `GSBUSCFG0 (0xc100)`
     INCRBRSTEN|INCR16BRSTEN (+cache req info ≥2.20a); `GSBUSCFG1 (0xc104)` =
     `BREQLIMIT(8)` (plain assign); `GUCTL (0xc12c)`: `USBHSTINAUTORETRYEN (bit14)` always,
     `REFCLKPER(0xF) (bits 31:22)` when `adj_sof_accuracy` or `dis_u2_freeclk_exists`
     ("fix ITP interval to 125 µs"), **`SPRSCTRLTRANSEN (bit17)` when DT
     `enable_sprs_transfer`** (parsed at core.c:1428); GRXTHRCFG quirk 1.90a–2.10a;
     GUSB3PIPECTL DEP1P2P3 ≤2.10a; `DISRXDETINP3 ≥2.50a` written **four times in a row**
     (copy-paste artifact, harmless); `UX_EXIT_PX` + `ELASTIC_BUFFER_MODE` from DT quirks;
     `LLUCTL`/`LSKIPFREQ` only for DWC31 revisions; ≤2.10a 16-bit UTMI PHYIF (PORTSC race WA).
  15. final `GFLADJ` block: `adj_sof_accuracy` → `GFLADJ_REFCLK_LPM_SEL` +
     `REFCLK_240MHZ_DECR(0xA)` + `REFCLK_FLADJ(0x7F0)`; else
     `dis_u2_freeclk_exists → clear REFCLK_LPM_SEL`.
- *(fact)* `dwc3_core_init_mode` (core.c:1294) HOST path:
  `dwc3_set_prtcap(dwc, DWC3_GCTL_PRTCAP_HOST)` — GCTL bits 13:12 (`PRTCAPDIR(n)=n<<12`,
  HOST=1, DEVICE=2, OTG=3, core.h:238-242) — then `otg_set_vbus(usb2_phy->otg, true)`
  (no-op here: no OTG/VBUS pad) and `phy_set_mode(usb2_generic_phy, PHY_MODE_USB_HOST)`,
  then `dwc3_host_init`. OTG path (what Android runs) instead inits otg fsm, host, gadget
  and lets the state machine flip PRTCAP via `dwc3_set_mode`/`__dwc3_set_mode` (core.c:146-233).
- *(fact)* `dwc3_host_init` (`drivers/usb/dwc3/host.c:73`): creates platform device
  **"xhci-hcd"** with resources = `dwc->xhci_resources` — MMIO
  `0x10C00000+DWC3_XHCI_REGS_START(0x0) .. +0x7fff` and the dwc3 IRQ — i.e. **the xHCI MMIO is
  the same 0x10C00000 window, offsets 0x0000-0x7fff**; DWC3 globals live at 0xc100-0xc6ff,
  device regs 0xc700-0xcbff, OTG 0xcc00-0xccff (core.h:78-85). Device properties:
  `usb3-lpm-capable` (from `snps,usb3_lpm_capable`), `quirk-broken-port-ped` for rev ≤3.00a.
  Downstream also hardcodes `phycon_base_addr = ioremap(0x10b00000, SZ_1K)` and a 2 MB
  pre-alloc DMA pool for USB audio offload.
- *(fact)* `xhci_l2_support` is consumed by the **xHCI platform driver**, not dwc3:
  `drivers/usb/host/xhci-plat.c:489` reads it from the *parent* (dwc3) node →
  `xhci->quirks |= XHCI_L2_SUPPORT` (Samsung L2 deep-sleep suspend; sysfs `l2_state`).
  Not needed for a UEFI bring-up (no L2 suspend).
- *(fact)* After `dwc3_probe` returns, xhci-plat binds to "xhci-hcd" and does the standard
  xHCI init (`xhci_gen_setup` → HCRESET in USBSTS, HCSPARAMS/HCCCPARAMS read, then
  xhci_run → HC_CFG flag, EHCI-style port reset on root hubs). *(inference)* For our DXE
  driver the equivalent is: run steps above through `set_prtcap(HOST)`, then hand the
  0x10C00000 window to an xHCI init (or to Windows' USBXHCI.sys) — the xHCI capability
  registers are live as soon as the core is out of reset with clocks on; the DWC3-side
  writes (GCTL/GUCTL/GUSB*CFG/GFLADJ/GEVNT*) must happen first or the host controller
  will come up with a mis-tuned refclk/SOF and no event buffer.
- *(open)* The DWC3 core **revision** (GSNPSID at 0x10C00C120) on 9820 is not stated in any
  source read so far — several quirk branches depend on it (2.10a/2.50a/3.00a/3.10a).
  Must be read on device (adb is blocked for /dev/mem, but our own DXE/shim can read it, or
  check Mu-Silicium logs).
- *(fact)* Mainline binding `samsung,exynos-dwc3.yaml`
  (https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/usb/samsung,exynos-dwc3.yaml):
  wrapper compatibles are SoC-specific (`samsung,exynos5433-dwusb3`, `exynos7`, `exynos8890`
  (fallback exynos7), `exynos990` (fallback exynos850), `exynos850`, `exynos2200`, gs101 …) —
  **there is no `exynos9820` entry**: 9820 was never upstreamed; the downstream compatible
  `samsung,exynos-dwusb` is not in mainline. Wrapper has **no `reg` of its own**
  (`#address-cells/#size-cells/ranges` + `clocks`/`clock-names` + `power-domains` +
  `vdd33-supply` (required) / `vdd10-supply`), child node = `snps,dwc3`.
- *(fact)* Mainline `snps,dwc3.yaml`
  (https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/usb/snps,dwc3.yaml):
  `compatible = snps,dwc3` or deprecated `synopsys,dwc3` (the spelling Samsung downstream
  uses); one `reg`; interrupts `dwc_usb3` or named `host/peripheral/otg/wakeup`; clock names
  `bus_early` (bus), `ref` (ITP when PHY suspended), `suspend` (clocks small core part when
  SS PHY in P3); `dr_mode`/`maximum-speed` come from `usb-drd.yaml` (already cited in
  dock-desktop §8). *(inference)* on 9820 downstream the wrapper's `aclk`≈bus_early and
  `sclk`≈link clock; the PHY's `ext_xtal` is the 26 MHz ref — the "ref clock" the GFLADJ/
  GUCTL REFCLKPER writes are tuned against.
- *(dead end)* `git.kernel.org/.../plain/Documentation/...` is behind Anubis anti-bot
  challenge for this fetcher — use `raw.githubusercontent.com/torvalds/linux/master/...` instead.
- *(fact)* **Clock gates are ACPM-owned, not plain MMIO.** `drivers/clk/samsung/
  clk-exynos9820.c:191-195` declares the USB gates as `HWACG_VCLK(GATE_USB31DRD_SLV_LINK,
  USB31DRD_QCH_SLV_LINK, …, parent "UMUX_CLKCMU_FSYS0A_BUS", …, VCLK_GATE, …)` — vclk/QCH
  entries in the CAL interface (`drivers/soc/samsung/cal-if/cmucal.c` binds them to
  `acpm_vclk_list`, and the ACPM coprocessor stack lives at `drivers/soc/samsung/acpm/`).
  Gate nodes exist per port (`cmucal-node.c:2007-2010`:
  `GOUT_BLK_FSYS0A_UID_USB31DRD_IPCLKPORT_ACLK_BUS/_ACLK_BUS_PHYCTRL/_ACLK_PHYCTRL/
  _I_USBDPPHY_REF_SOC_PLL`, plus `_I_USB31DRD_REF_CLK_40`, `_I_USBDPPHY_SCL_APB_PCLK`).
  This confirms the repo's earlier "CMU bank → power domain" dead end
  (MILESTONES §"Try to resolve CMU bank -> power domain"): on 9820 the AP does not gate
  these clocks by writing CMU registers directly — the ACPM does it on the AP's behalf.
- *(fact)* `drivers/samsung/misc/vbus_notifier.c` is only a generic blocking-notifier chain
  for VBUS events (`vbus_notifier_register`, sec_class) — **not** the `samsung,usb-notifier`
  DT driver. A file whose *contents* match compatible `"samsung,usb-notifier"` was not found
  by path search in the LineageOS tree (searched `drivers/**` path names + candidate files
  `exynos-pm.c`, `dwc3-exynos.c`, `phy-exynos-usbdrd.c`). **Open, low priority**: the LDO
  on/off machinery that matters (`exynos_usbdrd_ldo_control` via `phy_conn`) is already
  pinned down in the PHY driver.
- *(fact)* **SS (PIPE3) PHY lock/status registers** (combo-PHY window 0x10AE0000, direct
  MMIO — `usbdp_cal_reg_rd` is just `readl`, `phy-exynos-usbdp-gen2.c:26`):
  LC-PLL lock = **0x10AE0350 bit7** (`EXYNOS_USBDP_COM_CMN_A0350`,
  `USBDP_CMN_A0350_ANA_LCPLL_LOCK_DONE`, `phy-exynos-usbdp-gen2-reg.h:387-388`);
  RX CDR lock lane0 = **0x10AE0B84 bit0** (`COM_TRSV_A0B84`), lane2 selected when
  `used_phy_port != 0` (`COM_TRSV_A1B84`) — `phy-exynos-usbdp-gen2.c:47-74, 392-411`.
  `phy_exynos_usbdp_g2_enable` (line 519) is a long list of analog trims guarded by
  `!CONFIG_SOC_EXYNOS9820_EVT0` (power-optimization code, EVT1+ only) — **EVT0/EVT1 silicon
  revision matters** for the SS PHY path; beyond0lte has both an evt0 and a non-evt0 PMIC
  dtsi variant (`exynos9820-evt0-beyond0lte_if-pmic-max77705.dtsi`).
  *(inference)* a first host bring-up should stay USB2/HS-only (like Mu's 9830 lib:
  `SuperSpeedPhyDisabled=TRUE`, `MaximumSpeed=UsbBusSpeedHigh`, pipe3 forced to
  elec-idle + PMA LOW_PWR) and add SS later, once the EVT revision and PHY_CON GPIO
  polarity are known.

### Q3 — Mu-Silicium `UsbfnDwc3Dxe`

- *(fact)* Canonical repo is **`Project-Silicium/Mu-Silicium`** (branch `main`,
  https://github.com/Project-Silicium/Mu-Silicium). Driver lives at
  `Silicon/Samsung/SamsungPkg/Drivers/UsbfnDwc3Dxe/` (`UsbfnDwc3Dxe.c` 998 l,
  `Dwc3Core.c` 348 l, `Dwc3Endpoint.c` 845 l).
- *(fact)* **It is a device/function-mode driver, not a host driver.** It produces
  `gEfiUsbFunctionIoProtocolGuid` (EFI_USB_FUNCTION_IO — the protocol a Fastboot-style
  download stack consumes), reports VID 0x04E8 PID 0x2143, and its `Source.txt` says it was
  ported from `LineageOS/android_kernel_samsung_universal9830` files
  `drivers/usb/dwc3/{core.h,core.c,gadget.c,gadget.h,ep0.c}` — i.e. the Linux **gadget** side.
  `Dwc3Core.c:243/279` sets `GCTL PRTCAPDIR = DWC3_GCTL_PRTCAP_DEVICE`.
- *(fact)* `Status.md` (https://raw.githubusercontent.com/Project-Silicium/Mu-Silicium/main/Status.md):
  **USB Host Mode ❌ on every Samsung target** — S24 (e1s), Note 20 5G (c1s), S20 5G (x1s),
  S20 FE (r8s), S10 (beyond1lte), S9 (starlte), A10, A7, Tab A. USB Device Mode ✅ only on
  **x1s = Galaxy S20 5G** (note: in Mu, `x1s` is the Exynos 990 S20 5G, *not* a Snapdragon —
  the repo's earlier note in MILESTONES.md line 274 grouping x1s with Snapdragon is worth
  re-checking). Even x1s' Windows OS-status table lists USB Host Mode ❌ (and its Windows
  boot requires erasing the `uh` partition — the destructive pattern HANDOFF §2 rule 5 bans).
- *(fact)* Build wiring: `SamsungPkg.dsc.inc:48` defaults
  `PlatformUsbfnLib → SamsungNullPkg/Library/PlatformUsbfnLibNull` (no-op) and still lists
  `UsbfnDwc3Dxe.inf` at line 82; **only `S5E9830Pkg.dsc.inc:107` overrides it with a real
  board lib**. `S5E9820Pkg.dsc.inc` and `S5E9810Pkg.dsc.inc` contain **no USB lines at all**
  → on 9820 the driver builds against the NULL lib and does nothing. That is why Host is ❌
  on beyond1lte: nobody ever wrote the 9820 board library, and the driver itself can't do
  host mode anyway.
- *(fact)* The board-library abstraction is the reusable part:
  `SamsungPkg/Include/Library/PlatformUsbfnLib.h` defines `USBFN_PLATFORM_DATA`
  {ControllerBase (globals at +0xC100), MaximumSpeed, UtmiWidth, Usb2FreeClockExists,
  FrameLengthAdjustMask/Value (GFLADJ), SuperSpeedPhyDisabled, NumEndpoints, VID/PID} plus
  `UsbfnPlatformPhyInit/Exit/IsVbusPresent`. `S5E9830Pkg/Library/PlatformUsbfnLib/BoardInit.c`
  (295 l) is a complete, UEFI-native PHY bring-up for the same combo-PHY family:
  PMU de-isolation (`0x15860000+0x72C` BIT0 — **same PMU offsets and bit as 9820's DT**),
  force-Q-channel via `LINK_CTRL` (0x10C00000+0x04 on 9830), PHY soft reset via `CLKRST`
  (PHY20/30 SW_RST+RST_SEL), UTMI force-suspend/sleep + DP/DM pulldown clear,
  `HSP` EN_UTMISUSPEND|COMMONONN, bus-filter bypass, `UTMI FORCE_BVALID|FORCE_VBUSVALID` +
  `HSP VBUSVLDEXTSEL|VBUSVLDEXT` (device-mode VBUS faking), HSP_TUNE values, SIDDQ power,
  pipe3 override (PMA LOW_PWR), LINK_PORT OCD U2/U3 port select.
  `PlatformUsbRegisters.h` (126 l) is a clean USBCON register map: CTRL_VER 0x00, LINK_CTRL
  0x04, LINK_PORT 0x08, CLKRST 0x20, PWR 0x24, DUALPHYSEL 0x28, COMBO_PMA_CTRL 0x48,
  UTMI 0x50, HSP 0x54, HSP_TUNE 0x58, HSP_TEST 0x5C + all bit definitions — matches the
  kernel's `phy-exynos-usb3p1-reg.h` layout.
- *(fact)* **Address trap**: on 9830 (x1s) the DWC3 core sits at **0x10E00000** and the
  USBCON PHY block at **0x10C00000**; on 9820 the DWC3 core is at **0x10C00000** and USBCON
  at **0x10B00000** (kernel DT). Same PMU offsets, different block bases — Mu's constants
  must not be pasted into a 9820 driver.
- *(fact)* `UsbfnPlatformIsVbusPresent` reads `HSP_TEST` line state bits 21:20
  ("No vbus pad, next best thing") — confirms the PHY has no VBUS sensing even in Mu's world.
- *(verdict, inference)* Usable as a base for **9820 host mode? No** — the driver has no
  xHCI/host path at all. Usable as a base for our **register vocabulary + board-init
  skeleton? Yes** — `PlatformUsbRegisters.h`, `BoardInit.c`'s PMU/USBCON sequence, and
  `Dwc3Core.c`'s soft-reset/FLADJ/event-buffer helpers are exactly the DXE-side primitives
  we need, under a BSD-style license (Mu-Silicium is BSD-2-Clause-Patent per our README credits).
- *(dead end)* Did not verify `github.com/WD7789/Mu` (task's alternate name) — the
  Project-Silicium org repo is canonical and current; a fork would not change the verdict.

### Q4 — edk2-exynos (sonic011gamer) USB prior art

- *(fact)* https://github.com/sonic011gamer/edk2-exynos: a recursive tree search for
  `usb|dwc|xhci` in **file paths** returns **zero hits**. `Silicon/Samsung/Exynos9820Pkg`
  (37 files) contains only: `ExynosUfsLib` (ufs.c/scsi.c/ufs_dbg.c), `KeypadDeviceImplLib`,
  `MsPlatformDevicesLib`, `PlatformMemoryMapLib`, `PlatformPeiLib`, `PlatformPrePiLib`,
  `SOCSmbiosInfoLib`, `exynos9820.dec`, includes.
- *(fact)* `Platform/Samsung/exynos9820/exynos9820.fdf:139-143` has a `# USB Host Support`
  section that pulls in **only the generic class drivers** — `MdeModulePkg/Bus/Usb/UsbBusDxe`,
  `UsbMouseDxe`, `UsbMassStorageDxe` — and **no host controller driver** (no XhciDxe, no
  DWC3 DXE anywhere). *(inference)* So UsbBusDxe has no `USB_HCD_IO` producer to bind to:
  USB host is dead-on-arrival in edk2-exynos; they never got DWC3 up in UEFI either.
- *(verdict)* No usable 9820 USB prior art in edk2-exynos. Its value stays what this repo
  already uses it for: boot shim, UFS lib, DECON bring-up.

### Q5 — VBUS and the connector

- *(fact)* The S10e's Type-C controller + charger is the **MAX77705 interface PMIC** on
  hsi2c_45 addr 0x66 (`exynos9820-beyond0lte_if-pmic-max77705.dtsi`,
  `compatible = "maxim,max77705"`, `support_pd_role_swap`). Samsung's CCIC stack
  `drivers/ccic/` (`max77705_cc.c`, `max77705_usbc.c`, `max77705_pd.c`) drives it.
- *(fact)* Host-role VBUS chain: `max77705_vbus_turn_on_ctrl(usbc_data, ON, ...)`
  (`drivers/ccic/max77705_pd.c:479`) → `power_supply_get_by_name("otg")` →
  `POWER_SUPPLY_PROP_ONLINE = 1` → `max77705_set_otg()` (`drivers/battery_v2/max77705_charger.c:800`)
  → `max77705_chg_set_mode_state(SEC_BAT_CHG_MODE_OTG_ON)` → I2C writes to
  **`MAX77705_CHG_REG_CNFG_00`**: mode value `0xA`/`0xE`/`0xF` (`BOOST_OTG_ON` /
  `BUCK_BOOST_OTG_ON` / `BUCK_CHG_BOOST_OTG_ON`); bits: `CHG_CNFG_00_OTG` = bit1,
  `CHG_CNFG_00_BOOST` = bit3 (`drivers/battery_v2/include/charger/max77705_charger.h:121-162`).
  Transition rule: go through `MODE_4_BUCK_ON`, `usleep_range(1000,2000)`, then set OTG mode.
- *(fact)* OTG output current limit is a PMIC register field: `CHG_CNFG_02` bits 7:6
  `OTG_ILIM` = 500 mA / 900 mA / 1.2 A / 1.5 A (`max77705_charger.h:183-188`). So the phone's
  5 V rail is a battery boost in the MAX77705, hard-limited ≤1.5 A.
- *(fact)* The CCIC also reads the advertised source current from CC: `CCI_500mA`, `CCI_1_5A`,
  `CCI_3_0A` cases in `drivers/ccic/max77705_cc.c:449-460` (that's the *sink* side, i.e. charging).
- *(fact)* DWC3 itself has **no VBUS pad** on this SoC: `is_not_vbus_pad=<1>` on both the dwc3
  child and the PHY node, and `use_io_for_ovc=<0>` — no over-current GPIO either. So in host
  role nothing in the SoC senses or limits VBUS; the MAX77705 boost is the only 5 V source.
- *(fact)* `usb_notifier` DT node with `usb-ldocontrol = <3>` ("USB_LDOCONTROL_EXYNOS9820")
  shows the LDOs feeding the USB blocks are PMIC LDOs #10/11/12 (`ldo_number` on the dwc3 node).
- *(fact)* **USB 2.0 spec power rules** (primary source: USB 2.0 spec rev 2.0, §7.2 "Power
  Distribution", §7.2.1 "Classes of Devices" — official archive
  https://www.usb.org/sites/default/files/usb_20_20250603.zip, file `usb_20.pdf`):
  - "A unit load is defined to be 100 mA… absolute maximum, not an average over time."
  - Root port hubs: externally powered systems "must supply at least five unit loads to each
    port" (high-power ports); **battery-powered systems "may supply either one or five unit
    loads"** (100 mA = low-power port).
  - **Bus-powered (non-powered) hubs**: "may only draw up to one unit load upon power-up and
    five unit loads after configuration" — i.e. **≤500 mA total from the upstream port** — and
    their downstream ports "can supply only one unit load per port" (100 mA each), in Active
    *or* Suspend state. Figure 7-42 budget: 5 unit loads − (1 for hub controller +
    non-removable function) → 1 unit load/port; downstream VBUS must be switched with soft turn-on.
  - *(inference)* Our host is battery-powered with a MAX77705 OTG_ILIM of 500 mA at minimum
    setting — that is exactly a spec "low-power" 1-unit-load port unless we raise the limit;
    a bus-powered 4-port hub (≤500 mA draw, 100 mA/port) is the worst-case legal load, and a
    **self-powered** dock is the only way to exceed 500 mA total.
- *(fact)* The main PMIC itself is the S2MP200 family (`samsung,s2mps19mfd`) under
  **`speedy@15970000`** (`compatible="samsung,exynos-speedy"`, enabled by the board dtsi);
  the regulator driver (`drivers/regulator/s2mps19_regulator.c`) talks to it through an
  i2c-client abstraction whose adapter is the SPEEDY controller — i.e. MMIO at 0x15970000
  speaking the PMIC's serial protocol. So LDO10/11/12 are reachable in DXE if needed —
  separate interface from the MAX77705 (which is on hsi2c_45 I2C). *(inference)* whether
  SPEEDY is writable from non-secure EL1/EL2 on the shipping fuse configuration is untested.

### Q6 — Windows half (pointer for D3)

- *(fact)* Microsoft documents ACPI enumeration of SoC USB host controllers: vendor-assigned
  `_HID`, `_UID` if multiple, **`_CID` = `PNP0D10` for XHCI-compliant** (`PNP0D20` for EHCI),
  and `_CRS` with the controller's resources per the xHCI spec
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/other-acpi-namespace-objects,
  "USB host controllers and devices"). The SoC `_CID` table in
  https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/device-management-namespace-objects
  lists `PNP0D10` = "XHCI-compliant USB controller with standard debug", `PNP0D15` = same
  without standard debug (`PNP0D20/D25` = EHCI).
- *(fact)* The **inbox** `usbxhci.inf` matches `PCI\CC_0C0330`, `ACPI\PNP0D10` and
  `ACPI\PNP0D15` ("USB xHCI Compliant Host Controller") — seen in Windows compatibility INF
  dumps (e.g. https://idoc.pub/documents/hwcompatpe-vnd577ppwrlx: "usbxhci.inf: PCI\CC_0C0330
  ACPI\PNP0D15 ACPI\PNP0D10"; also a Win7 backport INF edit showing
  `%ACPI\PNP0D10.DeviceDesc%=Generic.Install,ACPI\PNP0D10`). So yes: **USBXHCI.sys binds
  ACPI-enumerated memory-mapped xHCI via `_CID PNP0D10/PNP0D15`** — no custom driver needed
  if the controller is spec-compliant. USBXHCI.sys is itself a client of the inbox class
  extension `Ucx01000.sys` (UCX)
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/developing-windows-drivers-for-usb-host-controllers).
- *(fact)* Real Snapdragon WoA machines use a **vendor `_HID` + the standard `_CID`**: the
  Qualcomm "USB3 eXtensible Host Controller" PnP IDs include `ACPI\QCOM06A1`, `ACPI\QCOM0A24`,
  `ACPI\QCOM0AA1`, `ACPI\QCOM0D08`, `ACPI\QCOM0D09` (secondary source: driver-fusion dump
  https://treexy.com/products/driver-fusion/database/universal-serial-bus-controllers/qualcomm/usb3-extensible-host-controller/).
  A minimal working DSDT pattern for a memory-mapped xHCI on ARM64 (from the edk2 sbsa-ref
  upstreaming patch): `Device (XHCI) { Name(_HID,"PNP0D10") Name(_CCA,1) Name(_CRS,
  ResourceTemplate(){ Memory32Fixed(ReadWrite, base, size) Interrupt(Level,ActiveHigh,Exclusive){gsiv} }) }`
  (https://patchew.org/EDK2/20230605104913.117643-1-wangyuquan1236@phytium.com.cn/20230605104913.117643-4-wangyuquan1236@phytium.com.cn/).
  *(inference)* For D3 we can copy that shape with `_HID` = our own (Samsung-style vendor ID)
  plus `_CID PNP0D10`, MMIO `0x10C00000+0x0..0x7fff` (xHCI regs) — and note the DWC3 globals
  at +0xc100 must be reserved from Windows or handled by our own minifilter; Windows'
  USBXHCI.sys only touches the xHCI window. `_CCA=1` (cache-coherent) matches Exynos DMA.

## What this means for our UEFI port

### Minimal init sequence a Beyond0LtePkg `UsbHostDxe` must reproduce (USB2/HS-first path)

All addresses already VERIFIED in `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h`
(0x10C00000 DWC3, 0x10B00000/0x10AE0000/0x10AF0000 PHY, PMU 0x15860000).

1. **State audit** (all plain MMIO reads): `USBCON CTRL_VER` @0x10B00000+0x00 — Mu's trick:
   0 or 0xFFFFFFFF ⇒ clocks off / block isolated. `GSNPSID` @0x10C00C120 — must read
   **0x5533xxxx** ("U3" + revision, `dwc3_core_is_valid` core.c:945-959; 0x3331xxxx would mean
   DWC_usb31 and revision comes from `DWC3_VER_NUMBER` 0xc1a0 instead). PMU
   0x1586072C/0x15860704 current isolation bits.
2. **Power/clocks**: do NOT try to gate clocks via CMU MMIO — the USB31DRD gates are
   ACPM-owned vclk/QCH entries. Strategy: rely on sboot leaving the FSYS0A USB clocks on
   (download mode is USB device mode, so the block is live at handoff — *verify on device*),
   and treat LDO10/11/12 (S2MP200, `regulator-boot-on`) as already on. Fallback if audit
   fails: implement enough ACPM IPC, or ask sboot/boot-shim to leave USB enabled.
3. **PHY de-isolation**: set bit0 at PMU **0x15860000+0x72C** (USBDRD/UTMI) — kernel
   `phy_isol(inst,0,mask)` and Mu `UsbPhyIsolation(TRUE)` agree (same offset/bit on 9830).
4. **USBCON PHY init (USB2 only)** — port Mu's `S5E9830 BoardInit.c` sequence with 9820
   bases (0x10B00000 instead of 0x10C00000): force-QChannel via LINK_CTRL(0x04); assert
   PHY20 soft reset via CLKRST(0x20); UTMI(0x50) clear FORCE_SUSPEND/FORCE_SLEEP/DP_DM_PULLDOWN;
   HSP(0x54) set EN_UTMISUSPEND|COMMONONN (DT `common_block_disable=1`); LINK_CTRL bus-filter
   bypass 0xF; HSP_TUNE(0x58) with the **host column** of the DT `hs_tune` pairs
   (tx_pre_emp 3, tx_vref 0xC, rx_sqrx **5**, utmi_clk 1, compdis 7, tx_res **1**);
   clear SIDDQ in HSP_TEST(0x5C); deassert PHY20 reset; then **pipe3 override**:
   LINK_CTRL PIPE3_FORCE_EN|FORCE_RX_ELEC_IDLE + COMBO_PMA_CTRL(0x48) LOW_PWRN (keeps SS PMA
   down — skip the whole 0x10AE0000 gen2 enable until SS is attempted). Skip Mu's
   UTMI FORCE_VBUSVALID/BVALID + HSP VBUSVLDEXT writes: those fake *device*-mode VBUS;
   in host mode VBUS is real (step 6).
5. **DWC3 core** (mirror `dwc3_core_init` HOST path, offsets absolute from 0x10C00000):
   GSBUSCFG0(0xc100) INCR16 burst; GSBUSCFG1(0xc104)=BREQLIMIT(8); GUCTL(0xc12c) |=
   USBHSTINAUTORETRYEN(bit14) | SPRSCTRLTRANSEN(bit17) | REFCLKPER(0xF)<<22;
   GCTL(0xc110) PRTCAPDIR=**HOST** (1<<12) and SOFITPSYNC per freeclk quirk (DT chose
   `dis-u2-freeclk-exists-quirk` → set SOFITPSYNC; `adj-sof-accuracy=1` on this board →
   kernel *clears* SOFITPSYNC and instead writes GFLADJ REFCLK_LPM_SEL + DECR(0xA) +
   FLADJ_REFCLK(0x7F0) + GUCTL REFCLKPER(0xF) — follow the accuracy path, it is what the
   board DT selects); GFLADJ(0xc630) |= 30MHZ_SDBND_SEL|FLADJ_30MHZ(0x20);
   GUSB2PHYCFG(0xc200): PHYIF=16-bit + USBTRDTIM=5, clear SUSPHY/U2_FREECLK_EXISTS/ENBLSLPM
   (note: the 9820 DT sets **no** `phy-mode` property — `of_usb_get_phy_mode` at core.c:1462
   yields UNKNOWN and the kernel leaves the width at the core default; Mu explicitly forces
   16-bit on the same PHY family, so force 16-bit and confirm against GHWPARAMS3 on device);
   GUSB3PIPECTL(0xc2c0): leave SUSPHY set + DEP1P2P3 when SS disabled (Mu rule);
   GEVNTADRLO/HI/SIZ/COUNT(0xc400+) event buffer (cheap, do it); **no DCTL CSFTRST** in host
   mode (kernel returns early; the xHCI HCRESET does the host-block reset).
6. **VBUS/CC**: I2C to MAX77705 (hsi2c_45, addr 0x66): CHG_CNFG_00 mode → 0x4 (BUCK),
   1 ms delay, → 0xE/0xF (BUCK+BOOST+OTG_ON); CHG_CNFG_02 OTG_ILIM → 500 mA (safe) or
   900 mA. *(open)* whether the MAX77705 CCIC sources Rp / detects the dock automatically
   once OTG is on, or needs explicit CC init registers from `max77705_cc.c` — must be read
   off that driver next.
7. **Handoff**: xHCI MMIO = 0x10C00000+0x0000..0x7fff, IRQ SPI 241 → GSIV 273. For UEFI
   console (keyboard/storage) instantiate edk2 `MdeModulePkg/Bus/Pci/XhciDxe` on that window;
   for Windows, emit the DSDT node (`_CID PNP0D10`, `Memory32Fixed(0x10C00000,0x8000)`,
   GSI 273, `_CCA=1`) and leave the core in host mode across ExitBootServices.

### Open unknowns (each needs a device read or a follow-up source dive)

- **GSNPSID revision** of the 9820's DWC3 — decides GUCTL1/GUCTL2/≤2.10a quirk branches.
- **EVT0 vs EVT1** silicon of RF8M21HYDSZ — matters only for the SS PHY gen2 trim list
  (`CONFIG_SOC_EXYNOS9820_EVT0` guards); USB2 path unaffected.
- **Clock/LDO state at DXE entry** — audit reads above; if clocks are off at handoff, the
  ACPM IPC route (or a sboot-side change) is the only way, and that is a much bigger task.
- **MAX77705 CCIC host-role (Rp sourcing) register setup** — `drivers/ccic/max77705_cc.c`
  internals not yet extracted; also need the hsi2c_45 base + a DXE I2C driver for it.
- **PHY_CON (GPG0_4) polarity/meaning** (`reverse_con_dir` absent in DT → default 0; the
  kernel reads it as input and picks lane0/lane2 by its value) — read the pin state at boot.
- **usb_notifier driver** for `samsung,usb-notifier` not located in the kernel tree
  (path-search dead end); only needed if we must replicate its LDO bookkeeping.
- **xHCI + DWC3 globals cohabitation**: Windows USBXHCI.sys maps only 0x0-0x7fff; our DXE
  writes to 0xc100+ must be done before ExitBootServices and not re-touched — reserve the
  whole 0x10000 window in the UEFI memory map and mark 0xc100..0xc6ff as in-use.

### Sources (consulted this pass)

- https://github.com/LineageOS/android_kernel_samsung_exynos9820 (branch lineage-23.2) —
  all kernel files quoted above: `drivers/usb/dwc3/{dwc3-exynos.c,core.c,core.h,host.c,io.h}`,
  `drivers/phy/samsung/phy-exynos-usbdrd.{c,h}`, `phy-exynos-usb3p1.c`,
  `phy-exynos-usb3p1-reg.h`, `phy-exynos-usbdp-gen2.c`, `phy-exynos-usbdp-gen2-reg.h`,
  `drivers/usb/host/xhci-plat.c`, `drivers/ccic/{max77705_cc.c,max77705_pd.c}`,
  `drivers/battery_v2/max77705_charger.c` + `include/charger/max77705_charger.h`,
  `drivers/regulator/s2mps19_regulator.c`, `drivers/clk/samsung/clk-exynos9820.c`,
  `drivers/soc/samsung/cal-if/{cmucal.c,exynos9820/cmucal-node.c}`,
  `arch/arm64/boot/dts/exynos/exynos9820.dts`,
  `arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_{common.dtsi,eur_open_25.dts,if-pmic-max77705.dtsi}`
- https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/usb/samsung,exynos-dwc3.yaml
- https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/usb/snps,dwc3.yaml
- https://github.com/Project-Silicium/Mu-Silicium (main): `Status.md`,
  `Silicon/Samsung/SamsungPkg/Drivers/UsbfnDwc3Dxe/*`, `SamsungPkg/Include/Library/PlatformUsbfnLib.h`,
  `S5E9830Pkg/Library/PlatformUsbfnLib/{BoardInit.c,PlatformUsbRegisters.h}`,
  `S5E9820Pkg/S5E9820Pkg.dsc.inc`, `SamsungPkg/SamsungPkg.dsc.inc`
- https://github.com/sonic011gamer/edk2-exynos: tree listing,
  `Platform/Samsung/exynos9820/exynos9820.fdf`
- https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/other-acpi-namespace-objects
- https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/device-management-namespace-objects
- https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/developing-windows-drivers-for-usb-host-controllers
- https://usb.org/document-library/usb-20-specification → `usb_20_20250603.zip` / `usb_20.pdf`
  (§7.2 Power Distribution)
- https://patchew.org/EDK2/20230605104913.117643-1-wangyuquan1236@phytium.com.cn/20230605104913.117643-4-wangyuquan1236@phytium.com.cn/
  (sbsa-ref PNP0D10 DSDT example)
- https://idoc.pub/documents/hwcompatpe-vnd577ppwrlx (usbxhci.inf device-ID dump)
- https://treexy.com/products/driver-fusion/database/universal-serial-bus-controllers/qualcomm/usb3-extensible-host-controller/
  (Qualcomm WoA xHCI PnP IDs — secondary)
- https://forums.mydigitallife.net/threads/usb-3-xhci-driver-stack-for-windows-7.81934/page-8
  (usbxhci.inf ACPI\PNP0D10 section — secondary)
- Dead ends: `Samsung/android_kernel_samsung_exynos9820` (404); git.kernel.org raw (Anubis
  bot-block); **elixir.bootlin.com also Anubis-blocked for this fetcher**;
  `usb.org/sites/default/files/usb_20.pdf` (404, superseded by the dated zip);
  `samsung,usb-notifier` driver file not found by path search;
  `of_usb_get_phy_mode` definition not located in the downstream tree (call site only).

## Safety review of the host-mode bring-up (2026-10-10, pre-device-test)

Asked directly: can what we built damage the phone? Enumerating every physical
hazard path and what the code does about each. The dangerous quantity is **5 V
on the VBUS pin against another source** (the community rule behind
MILESTONES ground rule 7).

1. **Host-vs-host collision (feeding 5 V into a PC's port).** GATED. The boost
   is only enabled after a LIVE read of `CC_STATUS0.CCStat == SOURCE`
   (Pmic.c/`Max77705WaitSourceAttach`). The CCIC firmware (DRP config
   CCCTRL1=0x19, set by the bootloader, which we never touch) only declares
   SOURCE when the partner presents Rd — i.e. a device/hub. A PC presents Rp,
   the CCIC settles SINK, our gate never fires. Plugging into a PC = boost off.
2. **Powered-hub backfeed** (the "irreversibly damage" case). The donation
   build's hub is **non-powered** (hard requirement in DRIVERS-MILESTONES);
   nothing backfeeds. `OTG_ILIM` is set to its **minimum 500 mA** — the most
   conservative setting the PMIC has (500 mA is also exactly the USB-2
   bus-powered-hub budget, notes Q5).
3. **Shorted cable/port.** The SoC has NO over-current sensing (fact:
   `use_io_for_ovc=0`, no VBUS pad) — protection is entirely inside the
   MAX77705 boost (current limit + its internal fault handling). *(open,
   non-blocking)* datasheet-level confirmation of the MAX77705 boost OCP/
   thermal-shutdown behaviour; the 500 mA ILIM bounds the worst case either way.
4. **Blind power/clock writes: none exist in the driver.** No CMU/ACPM writes
   (audit-only: `USBCON_CTRL_VER` 0/~0 -> abort), no SPEEDY writes (LDO10/11/12
   are `regulator-boot-on`, we rely on that and never touch rails), the only
   PMU write is the documented de-isolation bit the shipping kernel and Mu's
   working 9830 firmware both set. Every MMIO window is DT+device VERIFIED.
5. **Register-sequence hazards.** Every DWC3 write mirrors the kernel HOST
   path with runtime GSNPSID gating; we never set `GCTL_CORESOFTRESET`, never
   touch DCTL (kernel-faithful); the Xilinx PORTSC power-off write prevents a
   VBUS glitch before xHCI's own reset.
6. **Hot-swap while powered (the one real behavioural hazard we DO carry).**
   UEFI enables the boost once at boot and never turns it off on detach
   (Samsung's Android ccic stack does that; we don't replicate it). So:
   **usage rule for the donation build — plug the hub in BEFORE power-on,
   do not swap the cable between a device and a host while the phone is on.**
   Recorded as a known limitation, not a hidden one.
7. **Charge interaction.** Mode 0xE (not 0xF): the charger engine stays OFF
   while OTG boosts — the phone runs on battery, the charge path is untouched.
8. **Every failure mode is fail-safe, not destructive.** Clocks off -> abort
   at the audit; no CC source -> no VBUS; dead I2C -> no VBUS; wrong GSNPSID
   -> abort. The realistic worst case of a wrong guess in this driver is a
   non-working keyboard, not smoke.
9. **Nothing runs on the device until the boot stream succeeds**, and with
   nothing attached the port is exactly as inert as stock Android.

Verdict: with a non-powered hub and the plug-before-power rule, the built path
is bounded by the PMIC's own 500 mA limit and gated by live CC state. The one
open item (MAX77705 OCP datasheet confirmation) is recorded, not guessed.
