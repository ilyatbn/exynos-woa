# D6 — External display (HDMI via the dock): research notes

Plan-of-record task list: [`docs/exynos9820-port/DRIVERS-MILESTONES.md`](../docs/exynos9820-port/DRIVERS-MILESTONES.md) §D6.
Findings appended as discovered; facts carry source URLs; inference marked; dead
ends recorded.

## Known already (do not re-derive)

- The user's hub is **non-powered and HAS an HDMI port** (2026-10-10) — model
  not yet identified. Two completely different mechanisms hide behind "USB-C
  hub with HDMI": **DP alt mode** (the HDMI socket is wired to the phone's
  SuperSpeed lanes through the combo PHY — no USB data involved for video) vs
  **DisplayLink** (HDMI rendered by a chip in the dock, fed over USB data).
  Identification matters more than any driver work.
- `notes/dock-desktop.md` §11: the 9820 DT has a **USB/DP combo PHY**
  (`10AE0000`+0x2800, `has_combo_phy=1`) and a `displayport@130B0000` node;
  DeX on the S10e is listed as DP-alt-mode capable.
- D1 research found the PHY_CON GPIO (`gpg0 4`) muxes the combo PHY between
  USB and DP paths, read by the kernel PHY driver at init.
- `Exynos9820.h` already carries **DECON_F** (phone panel) and **DECON_T**
  windows — decon_t is the second display pipeline (the "T" is for TV/external
  in Exynos naming — verify).
- M11 established: DisplayLink ships a **Windows ARM64 WDDM driver** (Synaptics
  11.3+), and DisplayLink's own docs say the phone must become the USB host —
  which is exactly what D1 built.
- D5/D5b established: Windows enumerates displays ONLY through WDDM drivers;
  our shipped display path is GOP + in-box BasicDisplay (one panel, no
  second-output concept).

## Questions the research must answer

1. **Identify the two hub types from the outside**: what in a product's spec
   sheet / chip marks / behaviour says DP-alt-mode vs DisplayLink vs MHL?
   Cheap "USB-C hub with HDMI + USB2 ports" — which is it usually?
2. **DP alt mode on the 9820 in the Samsung kernel**: which driver binds
   `displayport@130B0000` (exynos DP? "smv" stack?), what is the full
   enable sequence (combo PHY lane flip/swap, HPD, the gpg0-4 PHY_CON GPIO,
   DECON_T -> DP link training), and what does DeX actually drive through it?
3. **DECON_T facts**: is decon_t really the external pipeline on this SoC
   (DT status, clocks, interrupts in Exynos9820.h vs the vendor DTS)?
4. **UEFI feasibility**: could our firmware light an external HDMI at all
   (DECON_T + DP init in DXE), and does any prior art exist (edk2-exynos,
   mainline DRM as documentation)?
5. **Windows feasibility per path**: (a) DP alt mode needs a WDDM driver for
   DECON_T+DP (ties to D5b rung 1 — display-only WDDM); (b) DisplayLink needs
   NOTHING from us beyond working USB host (D1) + the vendor ARM64 driver —
   confirm current driver + whether it is WHQL-signed (installable without
   test-signing) for a donation build.
6. **Verdict**: given the user's hub has HDMI, which mechanism is it likely to
   be, what is the cheapest validation on our device, and what does each path
   cost.

## Progress log

(append below)

### P1 — DP driver located in the Samsung kernel tree (2026-10-10, subagent)

- (fact) The driver stack for `displayport` on 9820 is the Samsung **DPU20**
  fbdev stack, not mainline DRM and not "smv":
  `drivers/video/fbdev/exynos/dpu20/displayport_drv.c` (core DP),
  `decon_displayport.c` (DECON↔DP glue), `displayport_edid.c`,
  `secdp_aux_control.c` (AUX channel), `displayport_hdcp13.c` /
  `displayport_hdcp22_if.c`, and SoC-cal layer
  `drivers/video/fbdev/exynos/dpu20/cal_9820/{displayport_reg.c,regs-displayport.h,decon_reg.c,regs-decon.h,dsim_reg.c}`.
  Source: GitHub tree JSON of LineageOS/android_kernel_samsung_exynos9820
  @ lineage-23.2 (`/tmp/opencode/d1/tree.json`, tree sha a412cfaf86b9).
- (fact) There is also a `drivers/phy/samsung/phy-exynos-displayport.c` — a
  DP-PHY driver distinct from the usbdp combo PHY. To be checked which one the
  9820 DTS actually references.
- (fact) DTS aliases in `arch/arm64/boot/dts/exynos/exynos9820.dts`
  (local copy `/tmp/opencode/d1/exynos9820.dts`, lines 215–218):
  `dsim0 = &dsim_0; displayport = &displayport; decon0 = &decon_f; decon2 = &decon_t;`
  → decon_f is display 0 (panel), **decon_t is display 2** and the DP node is a
  separate device; the `domain-clients = <&dsim_0>, <&displayport>` line (1697)
  is under some power/PMU domain and needs reading.

### P2 — Q2/Q3 answered: the DP alt-mode enable chain in the vendor kernel

Driver binding (fact):
- `displayport@0x130B0000` compatible `"samsung,exynos-displayport"` binds to
  `drivers/video/fbdev/exynos/dpu20/displayport_drv.c` (of_match table line
  4308). Fetched copy: `/tmp/opencode/d1/dpu20_displayport_drv.c` (126 KB).
- DECON side: compatible `"samsung,exynos9-decon"` → `dpu20/decon_core.c`;
  the DP output glue is `dpu20/decon_displayport.c` — it explicitly checks
  `decon->id == 2` for the DP path (vsync thread, line 147) and sets
  `decon->dt.out_type = DECON_OUT_DP` (line 204). So **DECON_T (id 2) is the
  external pipeline** — Q3's "T = TV/external" guess is confirmed by code, not
  just naming.

DTS facts (exynos9820.dts lines 1924–2039 + beyond0lte.dts lines 206–214):
- displayport: `reg = <0 0x130B0000 0xFFFF>, <0 0x10AE0000 0x20CC>` — the DP
  controller **maps the combo PHY registers directly** as its 2nd resource;
  `interrupts = <0 INTREQ__DP_LINK 0>`; `iommus = <&sysmmu_dpu0/1/2>`;
  `clocks = <&clock GATE_DPU_DPU>` (aclk).
- decon_t: `reg = <0 0x19050000 0x10000>`; interrupts
  `INTREQ__DPU_DECON2_FRAME_START / _FRAME_DONE / _EXTRA`; `out_type = <2>`
  with the DTS comment `/* 0: DSI, 1: eDP, 2:DP */`; `psr_mode = <0>` (video
  mode); `default_ch = <2>`; no separate clocks property (DPU aclk is owned by
  the dsim/displayport nodes).
- decon_f: `reg = <0 0x19030000 0x10000>`, `out_type = <0>` (DSI → panel).
- No `dsim_t` node exists in the 9820 DTS; only `dsim_0@0x19080000`
  (panel DSIM). decon_f declares `dsim_cnt = <2>` (HW capability) but the DT
  instantiates one DSIM. (fact)
- beyond0lte.dts enables DP: `&displayport { status = "okay";
  dp,aux_sw_oe = <&gpg1 6 0>; dp,sbu_sw_sel = <&gpg4 0 0>;
  dp,usb_con_sel = <&gpg0 4 0>; }` — three switch-control GPIOs:
  - **gpg1-6** = AUX-channel switch output-enable (active low: driver does
    `gpio_direction_output(gpio_sw_oe, 0)` + 100 ms to turn AUX on; 1 = off).
  - **gpg4-0** = SBU mux select, driven from cable orientation.
  - **gpg0-4** = `PHY_CON`/`usb_con_sel` — same pin D1 found on
    `usbdrd_phy0` (`phy,gpio_phy_port`, requested as **input**, name
    "PHY_CON", `phy-exynos-usbdrd.c` lines 1161–1188/1935–1952). Its level
    becomes `used_phy_port` and selects which SS lane pair the combo PHY uses:
    `phy-exynos-usbdp-gen2.c` line ~1083: `used_phy_port==0` →
    `CMN_A0288_DP_LANE_EN/LANE_MUX_SEL_DP = 0xc` with the source comment
    **"usb rx0, tx1, dp tx2, tx3"** — i.e. DP takes lanes 2+3 by default;
    the GPIO strap flips the pairing (lane swap). CDR lock polls TRSV A0 vs A1
    registers accordingly (line 47).
- PMU glue: `displayport.h` line 1145 `#define USBDP_PHY_CONTROL 0x15860704`
  — DP driver ioremaps it and reads bit 0 (`displayport_phy_enabled()`);
  matches `usbdrd_phy0` DTS `pmu_offset_dp = <0x704>` (PMU syscon base
  0x15860000). One PMU bit gates the whole combo PHY. (fact)

Alt-mode entry sequence (fact, from `displayport_drv.c` + `max77705_*`):
1. max77705 CCIC runs the USB-PD DP-alt-mode state machine in silicon:
   `max77705_usbc.c` has VDM interrupts `VDM_DP_STATUS_UPDATE` /
   `VDM_DP_CONFIGURE` and `max77705_vdm_process_set_DP_configure_mode_req()`
   (pin assignment written by CCIC FW, lines ~1981–3070).
2. CCIC → DP driver via **manager notifier**
   (`manager_notifier_register(..., MANAGER_NOTIFY_CCIC_DP)`, line ~2860):
   - `CCIC_NOTIFY_ID_DP_CONNECT / ATTACH` (carries sink VID/PID; Samsung
     VID 0x04e8 + PID 0xa029 "PAD" / 0xa020 "Station" get DeX WQHD tweaks —
     `displayport_check_adapter_type`) → `displayport_aux_sel()` (read gpg0-4
     strap → orientation `dp_sw_sel`, drive gpg4-0) +
     `displayport_aux_onoff(1)` (drive gpg1-6 low).
   - `CCIC_NOTIFY_ID_DP_LINK_CONF` with pin assignment A–F: A/C/E = 4-lane
     DP; B/D/F = 2-lane DP + USB3 (B flips `dp_sw_sel`). Optional NXP
     **PTN36502 combo redriver** hooks under
     `CONFIG_COMBO_REDRIVER_PTN36502` (must check if beyond0lte uses it).
   - `CCIC_NOTIFY_ID_DP_HPD` HIGH/LOW/IRQ → `displayport_hpd_changed()`.
   **HPD is signalled by the CCIC chip, not a SoC GPIO** — the max77705
   monitors the connector HPD line and interrupts. (fact)
3. `displayport_hpd_changed(1)`: sw-reset DP regs → AUX init → read sink
   DPCD (`displayport_link_status_read`) → `displayport_link_training()`
   (full CT cp: clock recovery + EQ with retries, lines 293–928) →
   `displayport_set_switch_state(1)` = **extcon `EXTCON_DISP_DP` +
   `/switch/class` node** → userspace (DeX/SurfaceFlinger) then drives DECON_T
   via the fbdev/ioctl path and calls `displayport_enable()` (video config
   from EDID `supported_videos[cur_video]`, AVI infoframe,
   `displayport_reg_start()`).
4. Combo PHY DP-lane power-up actually happens in the **USB** PHY driver:
   `phy-exynos-usbdrd.c` `exynos_usbdrd_pipe3_init()` calls
   `phy_exynos_usbdp_g2_enable/check_pll_lock/run_lane/rx_cdr_pll_lock`
   (lines 1141–1225) — the DP driver only borrows the PHY register base via
   `phy_exynos_usbdp_get_address()`. So USB-side and DP-side init are
   entangled in one driver; D1's PHY work already covers this file. (fact)
- DeX drives external through **DECON_T → DP controller → combo PHY lanes**
  (fact by construction: decon_t out_type=2, decon_displayport.c id==2).
- `drivers/phy/samsung/phy-exynos-displayport.c` exists in the tree but the
  9820 DTS has no node with its compatible (9820 uses the combo PHY path
  above) — dead end for this SoC, likely older SoCs. (fact from DTS grep)

### P3 — Q3 header numbers + link budget (all verified from tree)

Interrupts (fact, `include/dt-bindings/interrupt-controller/exynos9820.h`,
fetched to `/tmp/opencode/d1/irq-exynos9820.h`; DTS first cell is 0 = GIC SPI):
- `INTREQ__DP_LINK = 248`
- `INTREQ__DPU_DECON2_FRAME_START = 195`, `_FRAME_DONE = 196`, `_EXTRA = 197`
- (for reference: decon_f 183/184, `INTREQ__DPU_DSIM0 = 190`)

Reg windows (fact, exynos9820.dts): DP link `0x130B0000+0xFFFF`, combo PHY
`0x10AE0000+0x20CC` (DP view) / `+0x2800` (usbdrd_phy0 view), DECON_T
`0x19050000+0x10000`, PMU combo-phy gate `0x15860704` bit 0.

Link budget (fact, `dpu20_displayport_drv.c` line 246): link rates tried are
only **1.62 / 2.7 / 5.4 Gbps** (RBR/HBR/HBR2), max 4 lanes → 21.6 Gbps
aggregate; no HBR3. HDMI-side conversion happens in the dock (DP→HDMI), the
SoC speaks DP only.

Mode table (fact, `dpu20/cal_9820/displayport_reg.c` lines 146–203, fetched):
`supported_videos[]` covers 640x480p60 … 1920x1080p60 (VIC 16) … 2560x1440p60
… 3840x2160p60 — a UEFI bring-up target of **1080p60 (CEA VIC 16)** is the
middle of the table and what DeX FHD adapters use.

defconfig (fact, `arch/arm64/configs/exynos9820-beyond0lte_defconfig`):
`CONFIG_EXYNOS_DPU20=y`, `CONFIG_EXYNOS_DISPLAYPORT=y`, `CONFIG_EXTCON=y`,
`CONFIG_SWITCH=y`, `CONFIG_ANDROID_SWITCH=y`,
`CONFIG_SND_SOC_SAMSUNG_DISPLAYPORT=y` (DP audio), and
**`# CONFIG_COMBO_REDRIVER_PTN36502 is not set`** → the S10e board has no
external DP redriver chip; **`# CONFIG_PHY_EXYNOS_DISPLAYPORT is not set`**
→ the standalone DP-PHY driver is unused on this SoC.

IOMMU for the whole DPU/DP path (fact, `arch/arm64/boot/dts/exynos/exynos9820-sysmmu.dtsi`):
`sysmmu_dpu0@190A0000` (irqs 212/211), `sysmmu_dpu1@190B0000` (214/213),
`sysmmu_dpu2@190C0000` (216/215), each `reg = <0 0x190X0000 0x9000>`,
compatible `"samsung,exynos-sysmmu"` — shared by dsim_0 and displayport
(`iommu-domain_dpu` binds `<&dsim_0>, <&displayport>`). A UEFI bring-up must
either keep sboot's SYSMMU identity mapping or set one up itself.

### P3b — the CCIC alt-mode driver: `drivers/ccic/max77705_alternate.c`

(fact, fetched to /tmp/opencode/d1/ccic_alternate.c) The DP-alt-mode PD
state machine on the CCIC side lives in `drivers/ccic/max77705_alternate.c`
(not max77705_usbc.c, which is the older MFD-integrated variant):
- `max77705_vdm_dp_select_pin()` picks the pin assignment (prefers **D** =
  2-lane DP + USB3, falls back B); the DP_STATUS/DP Attention VDM fields
  `HPD_State` / `HPD_Interrupt` are read from the max77705's message RAM
  and forwarded as `CCIC_NOTIFY_ID_DP_HPD` to the DP driver — confirming
  the VESA rule that HPD travels over CC/PD, not a dedicated SoC pin.
- Source comment (verbatim): "If DP use 4lane(Pin Assignment C,E,A),
  dp_hs_connect is 1. USB can support HS. If DP use 2lane(Pin Assignment
  B,D,F), dp_hs_connect is 0. USB can support SS" — i.e. 4-lane DP leaves
  the phone's USB at high-speed (USB2) only; 2-lane DP keeps one SS pair
  for USB3. Directly relevant to D1: with a 4-lane dock our xHCI SS root
  port loses its lanes.
- On DP attach the driver raises `CCIC_NOTIFY_ID_DP_CONNECT/ATTACH` with
  the sink's Vendor_ID/Product_ID, then **waits on `host_turn_on_wait_q`**
  for the DWC3 host to come up — alt-mode entry and D1 host-mode turn-on
  are sequenced through this wait.
- Also in `drivers/ccic/`: `usbpd-manager.c` (the MANAGER_NOTIFY_* layer),
  `ccic_notifier.c`, `sec_pd.c`, `usbpd-core.c` — the full PD stack.

### P4 — Q1: telling the three hub mechanisms apart

Electrical facts (fact, VESA DP Alt Mode overview + USB-IF/VESA DevDays
slides, see Sources):
- USB-C has **four SuperSpeed pairs (TX1/RX1/TX2/RX2), two USB2 pairs
  (D+/D- on A6/A7/B6/B7), SBU1/2, and CC1/CC2** — separate pins.
- DP alt mode consumes the four SS pairs as DP lanes 0–3, routes **DP AUX
  over SBU1/SBU2**, and carries **HPD/IRQ over the CC pin via the USB-PD
  protocol** (VESA slides: "The DisplayPort HPD / IRQ is transmitted over
  the CC pin using the USB-PD protocol"). This is exactly why our kernel's
  HPD arrives from the max77705 CCIC, not a GPIO (P2) — the two independent
  observations corroborate each other.
- "USB 2.0 and USB Power Delivery [are] available in all configurations"
  (VESA). **Precise answer to the USB2-only-hub puzzle:** a hub whose
  downstream ports are USB2-only can still do HDMI via DP alt mode, because
  alt mode never touches the USB2 pins — it eats only the SS pairs (+SBU).
  What is *not* possible is alt mode over a cable/plug with no SS
  conductors at all. Marketing "USB 2.0 hub with HDMI" therefore almost
  always means: USB2 hub chip on D+/D- + DP-alt passthrough of the SS pairs
  into a DP→HDMI bridge chip.
- Pin assignments A–F (fact, kernel typec docs + DP driver LINK_CONF
  handling): A/C/E = 4 DP lanes; B/D/F = 2 DP lanes + 1 SS pair kept for
  USB3. A/B/F deprecated since DP alt mode 1.0b; D is the common phone-dock
  compromise. The 9820 supports both (P2).
- USB Type-C does **not** support DisplayPort Dual-Mode (DP++); a
  USB-C-to-HDMI dongle must contain an active **DP→HDMI converter** (fact,
  VESA slides: "USB Type-C will NOT support DisplayPort Dual Mode (DP++)").

Mechanism tells (fact/inference as marked):
- **DP alt mode**: no video-related USB device ever enumerates; the dock
  shows up only as a USB2/USB3 hub (+ maybe billboard). Works only if the
  *phone* has DP alt mode. If negotiation fails, the dock appears as a USB
  **"Billboard" device** class (fact, TI USB-C primer: "Provide a USB
  billboard message over the D+/D- lines"). Spec sheets say "DisplayPort
  Alt Mode", "DP 1.2/1.4", "4K@30/60". (inference) Cheap "USB-C to HDMI +
  3×USB2 + PD" dongles are almost always this: SS pairs passthrough +
  DP→HDMI bridge (e.g. Parade/ITE/MediTek bridge chips) + GL850-style USB2
  hub. They are dead HDMI on phones without alt mode.
- **DisplayLink**: a USB device with **VID 0x17E9** (DisplayLink Corp,
  fact, usb.ids via devicehunt/rxtx) enumerates on the USB2 bus; HDMI is
  generated by a DL-1xx/3xxx/5xxx/6xxx/7xxx chip; works over USB2 data
  alone, on *any* host with the driver — Synaptics: "DisplayLink only
  requires USB signals to operate, DisplayLink enabled products will work
  with any USB Standard A or USB-C connector" (fact). Needs a host-side
  driver (Windows: vendor WDDM package; Android: DisplayLink Presenter).
  DisplayLink's own KB (fact): "USB C docks may not all use DisplayLink
  technology and instead rely on USB 'Alternate mode' … Alternate mode is
  not supported by all phone e.g. Google Pixel phones up to the pixel 3."
- **MHL**: MHL Alt Mode for USB-C exists (fact, MHL consortium, Nov 2014
  press release) — MHL3 4K@30 over SS pairs, needs host MHL alt-mode
  support. (fact) No Samsung phone since ~Note4/S5 used MHL for display
  out; DeX-era Galaxies are DP alt mode. superMHL chips (Lattice SiI8630)
  shipped in nothing mainstream; the consortium page is a museum. Treat MHL
  as **irrelevant for the S10e** — dead end recorded: MHL was checked only
  to rule it out.
- HDMI Alt Mode (HDMI-launched, host-to-host) is **dead** — HDMI LA
  confirmed it was never implemented (fact, Ars Technica Jan 2023).
- DeX requirement (fact, Plugable Samsung-DeX page + Samsung developer
  docs): DeX needs a "USB 3.1 Type-C port" / HDMI adapter — i.e. DP alt
  mode on the phone side; S10e is on every supported-device list.

### P5 — Q4: UEFI feasibility, prior art

- (fact) **edk2-exynos has no display-controller code at all.** Tree dump
  `/tmp/opencode/d1/edk2exy-tree.json` (github.com/sonic011gamer/edk2-exynos,
  tree c0003541, 375 paths) contains exactly one display driver:
  `Silicon/Samsung/ExynosPkg/Drivers/SimpleFbDxe/SimpleFbDxe.c`, which just
  wraps `PcdMipiFrameBufferAddress/Width/Height` — a framebuffer the
  *previous* bootloader (sboot) already lit. No DP, no DECON, no DSIM init.
  So the S8/S9 Windows port never drove a display from UEFI; our M8 GOP
  approach (sboot framebuffer + SimpleFb) is the same trick. Dead end for
  prior art; the only real reference is the vendor kernel itself.
- (fact) Mainline DRM has an Exynos DP driver family
  (`drivers/gpu/drm/exynos/exynos_dp.c`, `exynos_dp_core.c`) but only for
  Exynos5-era SoCs; 9810/9820 are not supported upstream. Useful as
  readable DP-link-training documentation; the register-level truth for
  9820 is `dpu20/displayport_drv.c` + `cal_9820/displayport_reg.c` +
  `regs-displayport.h` (fetched locally).
- Minimum DXE bring-up for external HDMI (inference from P2/P3):
  1. clocks: enable `GATE_DPU_DPU` (CMU DPU domain) — D1's cmucal work
     already enumerates these gates;
  2. combo PHY: write PMU `0x15860704` bit0 (enable), then port the
     DP-relevant subset of `phy-exynos-usbdp-gen2.c`
     (`usbdp_g2_enable/check_pll_lock/run_lane`, lane mux `CMN_A0288 =
     0xc` for DP on lanes 2/3, honoring the gpg0-4 PHY_CON strap);
  3. switch GPIOs: gpg1-6 AUX-OE low, gpg4-0 SBU mux per orientation;
  4. DP controller @0x130B0000: AUX transaction to read EDID (or a fixed
     1080p60 preset), then classic DP training: `displayport_reg_init`,
     clock-recovery + EQ pattern loop at HBR2 4-lane (the vendor retry
     ladder in `displayport_full_link_training` is the reference);
  5. DECON_T @0x19050000: program CEA-1920x1080P60 timing (VIC 16, in
     `supported_videos[]`), video mode (`psr_mode=0`), window 0 scanout
     from a linear RGB framebuffer;
  6. SYSMMU DPU0/1/2: keep identity mapping (sboot already leaves them
     bypassed for the panel path — verify at runtime, not assumed);
  7. publish a second GraphicsOutput (or replace GOP mode list).
  The CCIC/PD side (entering alt mode) is the hard firmware part: in DXE
  we'd have to bit-bang the max77705 I2C PD controller to Discover/Enter
  Mode/Configure — no prior art anywhere; sboot/Android own that chip today.
  Cost estimate: this is a multi-week driver, not a config change.

### P6 — Q5: Windows feasibility per path

(a) DP alt mode under Windows (inference, anchored in D5/D5b findings):
Windows enumerates displays only through WDDM miniports (established in
D5b); there is no inbox driver for Exynos DECON_T/DP, and alt-mode entry
needs the max77705 CCIC driven by *something*. So the DP path = our own
**display-only WDDM miniport** driving DECON_T+DP+PHY (exactly D5b rung 1,
plus the DP/PHY half and a CCIC/PD client), or UEFI GOP pre-training + a
thin WDDM wrapper. No shortcut exists.

(b) DisplayLink under Windows (fact, Synaptics release notes 12.3,
4 Sep 2026 — current latest):
- Package 12.3.5143.0, graphics driver 12.3.5011.0 (dated 08/26/2026).
- Supported OS list: "Windows 10 (x64 v1709 & **ARM64 v2004 onwards**) and
  Windows 11 (x64 & **ARM64** edition)"; "DisplayLink for Windows on Arm64
  is available on all packages except MSI" (EXE end-user installer carries
  ARM64 since 11.5 per Plugable KB; ARM64 listed since 11.3).
- **WHQL: yes** — 12.3 new features: "**26H1 WHQL certification** for the
  Video and Audio Extension driver" → installs on a normal (donation-build)
  Windows 11 ARM64 without test-signing. (fact)
- Chips supported: DL-7400/7200, DL-6910/6350/6700/6950, DL-5500/5700/
  5900/5910, DL-4100, DL-3100/3500/3700/3900/3950, DL115/125/165/195 —
  the DL-3xxx/5xxx class is USB2-protocol (fact, compliance list in the
  same notes) → fine behind our DWC3 host at high-speed.
- GPU independence: Synaptics markets DisplayLink as "GPU Agnostic"
  (fact, product page); the driver is a WDDM display adapter, so it needs
  no Exynos GPU work from us — only a working USB host (D1) and any
  primary display for dxgkrnl (our GOP/BasicDisplay panel satisfies that;
  DisplayLink's old KB minimum-PC blurb demanding a DirectX-10 GPU is
  Win7-era text — on Win10/11 the in-box BasicDisplay host adapter is
  enough. (inference, flagged)).
- Nothing to port: install the vendor EXE once D1 host mode boots Windows
  and the hub enumerates.

### P7 — Q6 verdict

The user's hub (non-powered, HDMI + USB2 ports) is **almost certainly a
DP-alt-mode dongle** (inference from P4: that topology at that price point
is alt-mode + DP→HDMI bridge + USB2 hub; DisplayLink parts cost the
manufacturer more and such products advertise "driver required"/DisplayLink
certification).

What works TODAY under our Windows build, with that hub:
- keyboard/mouse/ethernet on its USB2 ports: **yes** once D1 host mode runs
  under Windows (USB2 needs no alt-mode negotiation and no SS pairs);
- its HDMI port: **nothing** — alt mode is never entered (nothing drives
  the max77705 PD state machine under Windows) and no WDDM driver exists
  for DECON_T/DP. The monitor stays dark; the hub may even show as a
  Billboard device.

Cost of each path:
- **DP alt path**: port `dpu20/displayport_drv.c` + `decon_displayport.c`
  + combo-PHY DP functions + CCIC PD alt-mode entry into a display-only
  WDDM miniport (D5b rung 1 + DP half), or pre-train in DXE and wrap in
  GOP→WDDM. Full vendor reference exists (this note); realistic cost is a
  multi-week driver project with real boot-hazard risk (combo PHY shared
  with the USB3 bus D1 depends on — misprogramming `0x15860704`/lane mux
  can kill host mode).
- **DisplayLink path**: buy a DL-3xxx/DL-5xxx-based dock/adapter (~$50–150
  used/new; DL-165-era adapters even less). Zero driver work: vendor
  WHQL-signed ARM64 driver (12.3, P6) installs on the donation build.
  Only prerequisite: D1 xHCI host mode proven under Windows. This is the
  cheapest *working external display under Windows* by a wide margin.

Cheapest device-side validation experiments (both zero-risk, no flashing):
1. **Alt-mode hardware path (proves the hub + phone silicon end to end)**:
   plug the hub into the S10e under Android, HDMI to a monitor. If the
   mirror/DeX picture appears, the whole chain (CCIC PD, gpg0-4/gpg1-6/
   gpg4-0 switches, combo PHY, DECON_T→DP, DP→HDMI bridge) works and the
   hub is confirmed alt-mode. Corroborate from adb: `dmesg` should show
   `displayport hpd changed 1` and link-training lines from
   `displayport_drv.c`; if the kernel is LineageOS (our tree), those exact
   strings from P2 are greppable.
2. **Hub identity check**: after plugging under Android, look for a USB
   device with VID `17e9` in `dmesg`/lsusb output — present ⇒ DisplayLink,
   absent (only a hub class) ⇒ alt-mode dongle.
3. **DisplayLink path (proves D1 host + DL enumeration)**: plug a DL dock
   under Android with the DisplayLink Presenter app, or under our Windows
   build directly with the vendor driver — first successful WDDM adapter
   in Device Manager is the milestone.

Recommended order: run experiment 1+2 with the existing hub tonight
(5 minutes, no risk); if a second external-display-under-Windows result is
actually wanted, buy a DL-3xxx adapter rather than funding the DP-alt WDDM
port before D5b rung 1 exists.

## Sources

Kernel tree (fetched via raw.githubusercontent.com, branch lineage-23.2):
- https://github.com/LineageOS/android_kernel_samsung_exynos9820/tree/lineage-23.2
  (tree JSON: /tmp/opencode/d1/tree.json, sha a412cfaf86b9)
- drivers/video/fbdev/exynos/dpu20/displayport_drv.c, decon_displayport.c,
  displayport.h, cal_9820/displayport_reg.c
- drivers/phy/samsung/phy-exynos-usbdrd.c, phy-exynos-usbdp-gen2.c
- drivers/ccic/max77705_alternate.c, max77705_usbc.c, max77705_cc.c,
  ccic_notifier.c (alternate.c fetched; others via /tmp/opencode/d1 copies)
- arch/arm64/boot/dts/exynos/exynos9820.dts, exynos9820-sysmmu.dtsi
- arch/arm64/configs/exynos9820-beyond0lte_defconfig
- include/dt-bindings/interrupt-controller/exynos9820.h
- https://github.com/sonic011gamer/edk2-exynos (tree JSON: /tmp/opencode/d1/edk2exy-tree.json;
  SimpleFbDxe: .../Silicon/Samsung/ExynosPkg/Drivers/SimpleFbDxe/SimpleFbDxe.c)

USB-C / DP alt mode / MHL:
- https://vesa.org/wp-content/uploads/2019/10/USB-DevDays-2019-VESA-DP-Alt-Mode-over-USB-Type-C.pdf
- http://www.vesa.org/wp-content/uploads/2014/09/DP-Alt-Mode-Overview-for-VESA-v1.pdf
- https://www.st.com/resource/en/technical_article/ta0356-usb-typec-and-power-delivery-displayport-alternate-mode-stmicroelectronics.pdf
- https://www.ti.com/lit/wp/slyy109b/slyy109b.pdf (billboard, muxing)
- https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/10cc5d483ebc00e82d9a38d3419b2edc8b79b64d/Documentation/ABI/testing/sysfs-driver-typec-displayport (pin assignments A–F)
- https://www.mhlconsortium.com/usbtype-c.aspx , https://www.mhltech.org/PressReleaseDetail.aspx?id=2147501807 (MHL alt mode)
- https://arstechnica.com/gadgets/2023/01/hdmi-to-usb-c-spec-axed-as-displayport-alt-mode-reigns-supreme (HDMI alt mode dead)
- https://www.synaptics.com/products/displaylink-graphics/usb-c (DisplayLink needs USB only)
- https://support.displaylink.com/knowledgebase/articles/1915774 (alt mode vs DisplayLink on phones)
- https://kb.plugable.com/docking-stations-and-video/chipset-modal (DisplayLink vs alt mode)
- https://devicehunt.com/view/type/usb/vendor/17E9/device/any , https://rxtx.org/device/17e9-0198-displaylink (VID 0x17E9)
- https://developer.samsung.com/samsung-dex/how-it-works.html , https://www.samsung.com/sg/support/mobile-devices/how-to-connect-samsung-dex-with-samsung-galaxy-s10-series , https://plugable.com/en-ca/pages/samsung (DeX on S10e via HDMI adapter / USB 3.1 port)

DisplayLink Windows driver:
- https://www.synaptics.com/sites/default/files/release_notes/2026-09/DisplayLink%20USB%20Graphics%20Software%20for%20Windows12.3-Release%20Notes.txt (12.3, ARM64, WHQL 26H1, chip list)
- https://www.displaylink.com/downloads/windows (12.3 latest, Sep 4 2026)
- https://www.synaptics.com/sites/default/files/release_notes/2025-06/DisplayLink%20USB%20Graphics%20Software%20for%20Windows11.7%20M3-Release%20Notes.txt (ARM64 packages)
- https://kb.plugable.com/are-displaylink-drivers-available-for-windows-11-on-arm (ARM64 timeline)
- https://www.synaptics.com/products/displaylink-graphics (GPU-agnostic)
- https://support.displaylink.com/knowledgebase/articles/524951 (min PC reqs; legacy GPU wording)

## The user's hub, identified from live USB descriptors (2026-10-10)

Read directly while the hub is attached to this PC (`lsusb` + sysfs):

- *(fact)* **UGREEN hub** enumerating as: `05e3:0625` Genesys Logic **USB3.2
  hub** (GL352 family) + `05e3:0610` "USB2.1 hub" + two `05e3:0608` USB2.0
  hub ports, and `0b95:1790` **ASIX AX88179B** gigabit ethernet. Downstream
  today: Logitech `046d:c53f` receiver + Microdia `0c45:8009` keyboard.
- *(fact)* **No `17e9:*` DisplayLink device anywhere** -> per Q1's tell, the
  HDMI socket is **DP alt mode** (SS lanes -> DP->HDMI bridge), not USB
  graphics. Matches the "cheap USB-C hub with HDMI" prediction.
- *(fact, requested-current from descriptors)* power budget as configured:
  hub 100 mA + Logitech rx 98 mA + keyboard 100 mA + AX88179B **184 mA** =
  **482 mA worst case**, inside our `OTG_ILIM` 500 mA setting with ~18 mA
  margin. The hub is exactly the non-powered class D1 was designed for.
- *(consequence, inference)* LAN on Windows = AX88179B, and D4's catalog scan
  already found **ASIX shipping ARM64 packages on Windows Update** — this
  specific hub's ethernet is therefore expected inbox-class once D1/D3 are
  proven. M13 just got a concrete, verified chip.
- *(consequence for HDMI, inference)* DP alt mode needs the **SuperSpeed
  lanes** — our D1 v1 deliberately holds the SS PHY down (USB2/HS only), and
  alt mode additionally needs the CCIC PD state machine + DECON_T + a WDDM
  story Windows does not have without D5b rung 1. So: this hub's HDMI stays
  dark under our Windows build; its keyboard/mouse/LAN half works.
- *(next cheap validation, zero risk, no flashing)* plug the hub into the
  S10e **under Android** and check DeX/screen-mirror lights the monitor, and
  `dmesg | grep -iE 'displayport|hpd'` shows `hpd changed 1` — proves the
  whole DP-alt hardware path (cable, hub bridge, combo PHY, DECON_T) before
  any of it is ever attempted under Windows.
