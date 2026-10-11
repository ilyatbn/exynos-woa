# D8 — Linux audio stack research (ABOX / madera / cs35l40 / beyond-madera)

Started: 2026-10-11. Subagent task: map the Linux driver architecture for the
S10e (Exynos 9820) audio subsystem, with file paths + line refs for a later
from-scratch UEFI/Windows port pass.

Sections Q1..Q7 + Sources. Facts (sourced) vs inference (mine) separated.

## Q1 — Where the Linux driver source lives

### FACT (verified 2026-10-11 via git ls-remote + raw.githubusercontent.com)

**LineageOS/android_kernel_samsung_exynos9820** — branches (git ls-remote):
lineage-18.1, 19.0, 19.1, 20, 21, 22.1, 22.2, 23.0, 23.2.
Used for all refs below: **lineage-22.2** (last branch generation carrying the
4.19 Samsung vendor kernel for beyond0lte; 23.x may differ — not checked).
Base URL: https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-22.2/

Layout found (via GitHub tree HTML, path-filtered):
- `sound/soc/samsung/abox/` — full ABOX driver, 39 files:
  abox.c, abox.h, abox_adaptation.c, abox_cmpnt.h, abox_cmpnt_v10.c,
  abox_cmpnt_v20.c, abox_core.c/.h, abox_dbg.c, abox_dump.c, abox_effect.c,
  abox_failsafe.c, abox_gic.c, abox_if.c, abox_ipc.c, abox_log.c,
  abox_mmapfd.c, abox_msg.c/.h, abox_qos.c, abox_rdma.c, abox_soc.c,
  abox_util.c, abox_vdma.c, abox_vss.c, abox_wdma.c, Kconfig, Makefile
- `sound/soc/samsung/beyond_madera.c` (2107 lines) — the `samsung,beyond-madera`
  machine driver (+ `jack_madera_sysfs_cb.c`, `sec_audio_debug_v2.c`, `vts/` dir)
- `sound/soc/codecs/madera.c` + `cs47l90.c` + `cs47l92.c` — Madera family.
  **No cs47l93.c** in this tree (417 files listed, dir not truncated).
- **No cs35l40 file anywhere in sound/.** The vendor CS35L40 driver is:
  - MFD front-end: `drivers/mfd/cs35l41-i2c.c` — of_match contains
    `{.compatible = "cirrus,cs35l40"}` (line 40) and cs35l41 (line 41);
    i2c_device_id table has {"cs35l40",0} (line 47).
  - Codec/DSP: `sound/soc/codecs/cs35l41.c` (2247 lines) — of_match line 2018
    `{.compatible = "cirrus,cs35l40"}`; one driver serves both chips.
  - Sysfs class glue: `drivers/mfd/cirrus-amp.c` + cirrus-bd.c (big data),
    cirrus-cal.c (calibration), cirrus-pwr.c (power model).
- MFD core: `drivers/mfd/madera-core.c`, `madera-spi.c` (defconfig:
  `CONFIG_MFD_MADERA_SPI=y`, `# CONFIG_MFD_MADERA_I2C is not set` →
  **CS47L93 is on SPI**, not I2C).
- defconfig `arch/arm64/configs/exynos9820-beyond0lte_defconfig`:
  CONFIG_SND_SOC_SAMSUNG_ABOX=y, ABOX_VERSION=0x20001, ABOX_V2=y,
  ABOX_DRAM_SIZE=0x1C00000 (28 MiB), CONFIG_SND_SOC_MADERA=y,
  CONFIG_SND_SOC_SAMSUNG_BEYOND_MADERA=y, EXTCON_MADERA=y, MADERA_IRQ=y.

**CruelKernel/samsung-exynos9820** branch `cruel-HVJ5-v5.3` (branch exists per
git ls-remote): same layout — `sound/soc/samsung/abox/`, `beyond_madera.c`,
`sound/soc/codecs/madera.c`/`cs47l92.c`, no cs35l40 file. (Checked tree HTML.)

**DT source split (fact):** the board DT in the kernel repo is a DTBO overlay
(`/plugin/;` in exynos9820-beyond0lte_eur_open_25.dts). The sound card node is
in `arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi` line ~200
(`compatible = "samsung,beyond-madera"`), cs35l40 amp nodes at lines 2119/2163.
The `abox@18c50000` node itself is NOT in the kernel repo (no dts file named
abox; labels &abox_uaif_0 etc. are only referenced) — it lives in the shipped
**base.dts**, confirmed locally: /home/ilya/exynos-woa/dumps/hhxj6/base.dts
abox node 5321, abox-core@18c55000 5345, abox-core@18c55080 5371.

### Mainline status (facts, checked against torvalds/linux master via raw.githubusercontent.com — see Q7 for method)
- `samsung,abox`: NOT mainlined (no sound/soc/samsung/abox in mainline; confirmed Q7).
- `madera` codec: mainline (sound/soc/codecs/madera.c + cs47l90/92/93 variants — details Q7).
- `cs35l40`: mainline since ~6.3 as MFD+codec split (drivers/mfd/cs35l40-i2c.c,
  sound/soc/codecs/cs35l40-lib.c) — different architecture from Samsung's vendor
  cs35l41.c driver (details Q5/Q7).
- No mainline exynos9820 audio CPU driver ("exynos-dmaudio" does not exist — confirmed Q7).

### Firmware filenames — FACT, extracted from dumps/hhxj6/vendor/vendor.raw (strings):
- cs35l40-bot-dsp1-spk-prot.wmfw / cs35l40-bot-dsp1-spk-prot.bin / cs35l40-bot-dsp1-spk-prot-calib.bin
- cs35l40-rcv-dsp1-spk-prot.wmfw / .bin / -calib.bin
- cs47l93-dsp1-trace.wmfw
- calliope_sram.bin, calliope_dram.bin, calliope_sram_2.bin, calliope_dram_2.bin, calliope2.dt

## Q2 — ABOX driver architecture (vendor kernel, all refs = lineage-22.2)

All paths under https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-22.2/sound/soc/samsung/abox/

### Probe / resource mapping — abox.c
- samsung_abox_probe (abox.c ~line 2700+): ioremaps reg-names "sfr" (ifr
  0x18C50000), "sysreg", "sram" (0x18D00000, 0x59000), "timer" (0x18CE0000)
  (lines ~2780-2800).
- Attaches the ABOX device to its own IOMMU domain (get_domain_from_dev +
  iommu_attach_device, lines ~2805-2815) — matches DT iommus=<&sysmmu@18E00000>.
- Allocates DRAM firmware buffer: dmam_alloc_coherent(DRAM_FIRMWARE_SIZE) and
  iommu-maps it at fixed IOVA `IOVA_DRAM_FIRMWARE` (line ~2820).
  DRAM_FIRMWARE_SIZE from CONFIG_SND_SOC_SAMSUNG_ABOX_DRAM_SIZE=0x1C00000.
- Also maps IOVA_PRIVATE, IOVA_VSS_FIRMWARE (modem VSS shm), and identity map
  0x10000000 (lines ~2825-2845).
- Clocks fetched by con-id: pll, pll1, audif, cpu, dmic, bus, cnt (lines ~2850-2880).
- IPC setup (lines 2885-2905 + 2956): reads DT
  samsung,ipc-tx-offset=0x2f000, ipc-rx-offset=0x2f300, ipc-tx-ack=0x2f2fc,
  ipc-rx-ack=0x2f5fc (SRAM-relative, legacy IPC v1), and
  samsung,ipc-tx-area=<1 0x850000 0x4000>, ipc-rx-area=<1 0x854000 0x4000>
  (area 1 = DRAM, IPC v2 queues), handshake-area=<0 0x2f2e0 0x20> (SRAM).
  Values confirmed in local dumps/hhxj6/base.dts lines 5329-5335.
  abox_ipc_init returns <0 → ipc_version=1 else 2 (line ~2903).
- ABOX GIC: registers handlers for IRQ_WDT + SGIs 0..SGI_ABOX_MSG (lines ~2946-2950).

### Core bring-up — abox_core.c + abox.c abox_enable()
- PMU offsets are **SoC-PMU-relative**, NOT ABOX-relative: abox_core.c parses
  DT `samsung,pmu_power/enable/standby` (lines 557-568) and applies them via
  `exynos_pmu_update(offset, mask, val)` (lines 96-128) — the exynos-pmu
  driver's regmap. On 9820 the PMU syscon is `system-controller@15860000`
  (compatible "samsung,exynos9820-pmu") per dumps/hhxj6/base.dts:5296-5298.
  → core0 power 0x15862d80/mask 0x1, enable(reset) 0x15862da0/0x1,
    standby-status 0x15862da4/mask 0x11; core1 0x15862e00/0x15862e20/0x15862e24
    (base.dts:5351-5353, 5377-5379).
- abox_core_download_firmware (abox_core.c:415): for each core's fw list
  (from child nodes abox-firmware-sram0/dram0 with samsung,area/samsung,offset,
  parsed in abox.c:2212-2216): request_firmware_direct, else async
  request_firmware_nowait (line 407) and retry later; SRAM area →
  `memcpy_toio(data->sram_base + fw->offset, ...)` + zero rest of SRAM
  (lines 444-449); DRAM area → memcpy into the coherent dram_base buffer
  (lines 451-455).
- Boot order (abox.c abox_enable, lines ~2395-2450): enable audif clk →
  cfg_gpio("default") → pad retention off (exynos_pmu_update ABOX_PAD_NORMAL,
  line ~2105) → if reset happened: abox_gic_init_gic + abox_download_firmware
  → abox_request_dram_on(DEFAULT_SYS_POWER_ID) → `abox_cpu_power(true)`
  (PMU 0x2d80 set) → `abox_cpu_enable(true)` — NOTE: enable path is a no-op;
  PMU 0x2da0 is only written to RESET the core on disable (lines 2125-2131).
  → wait for boot_done: DSP sends IPC_SYSTEM_MSG (param3=version) via SGI on
  the ABOX GIC → abox_boot_done() (abox.c:1681, 1789).
- Standby check: abox_core_standby polls pmu_standby bit (0x2da4 & 0x11) with
  100 ms timeout (abox_core.c:130-160).

### IPC transport — abox_ipc.c + abox_msg.c/.h
- abox_ipc_send (abox_ipc.c:70): wraps ABOX_IPC_MSG into 2-segment
  abox_msg_send_data {cmd, payload} → abox_msg_send (abox_msg.c:49) writes into
  a shared-memory ring: struct abox_msg_queue { q_cmd (16 cmds,
  ABOX_MSG_LEN_CMD=16, abox_msg.h:37), q_data } at the DRAM IPC areas;
  then `abox_gic_generate_interrupt(dev_gic, ABOX_IPC_IRQ)` = SGI doorbell to
  Calliope (abox_ipc.c:171-172).
- RX: abox_ipc_irq_handler (abox_ipc.c:238) registered on ABOX_IPC_IRQ via
  abox_gic_register_irq_handler (abox_ipc.c:288); abox_msg_recv (abox_msg.c:146).
- Message format: include/sound/samsung/abox_ipc.h — ABOX_IPC_MSG with ipcid
  (IPC_PCMPLAYBACK/IPC_PCMTASK/IPC_OFFLOAD/IPC_SYSTEM/IPC_ERAP/IPC_CONFIG...)
  and union of IPC_PCMTASK_MSG {msgtype: PCM_OPEN/PCM_HW_PARAMS/PCM_SET_BUFFER/
  PCM_START/PCM_STOP..., param.setbuff{phyaddr,size,count}, hw_params{rate,
  bit_depth,channels}}, IPC_OFFLOADTASK_MSG, IPC_SYSTEM_MSG, IPC_ABOX_CONFIG_MSG.

### SFR map (ifr @0x18C50000) — abox_soc.h
- ABOX_SFR(name,o,x) = ABOX_<name>_BASE + x*ITV + o (line 22).
- UAIF: base 0x0500, ITV 0x10 → UAIF0..3 at ifr+0x500/510/520/530
  (CTRL0 +0x0, CTRL1 +0x4, STATUS +0xC) (lines 305-309).
- DSIF: CTRL 0x0550, STATUS 0x0554 (lines 366-367).
- RDMA: base 0x1000, ITV 0x100; CTRL0 +0x0 (ENABLE bit0, BURST_LEN bits22:19),
  CTRL1 +0x4, BUF_STR +0x8, BUF_END +0xC, BUF_OFFSET +0x10, STR_POINT +0x14,
  VOL_FACTOR +0x18, STATUS +0x30 (RBUF_OFFSET field) (lines 408-433).
- WDMA: base 0x3000 (v2; 0x2000 for v1 ifdef, lines 467-469), ITV 0x100,
  same layout (CTRL +0x0, BUF_STR +0x8, ... STATUS +0x30).
- Routing SFRs exist: ABOX_SPUS_SBANK_RDMA / SPUS_SBANK_ASRC / SPUM_SBANK_*
  (lines 116-237) — the internal audio router banks.

### DAI components registered
- abox_if.c: platform driver "samsung-abox-if" matching samsung,abox-uaif /
  samsung,abox-dsif (lines 692-715); one component + one DAI per UAIF/DSIF.
  UAIF DAI: playback+capture, 1-8ch, 8k-384k, AP writes UAIF_CTRL0/1 directly
  (set_fmt line 248, hw_params line 346 — clock div, format, width).
  DSIF DAI: playback-only struct but used for PDM capture, S32 (line 499).
- abox_rdma.c:2206 abox_rdma_dai_drv + abox_wdma.c:675 abox_wdma_dai_drv —
  one DAI per RDMA/WDMA channel, names generated at probe
  (data->dai_drv->name = get_dai_name(id), abox_rdma.c:2403).
- abox_cmpnt_v10/v20.c: "component control" kcontrols — every node of the
  Calliope audio graph (effect, mux, ...) is exposed as ALSA controls whose
  get/put are IPC round-trips (abox.c:1145-1410, abox_cmpnt_v20.c).

### CRITICAL — can playback work WITHOUT Calliope firmware? (FACT from code)
- In the vendor driver: NO. PCM hw_params (abox_rdma.c:1455-1466) sends
  IPC PCM_SET_BUFFER with `phyaddr = IOVA_RDMA_BUFFER(id)` + PCM_PLTDAI_HW_PARAMS
  to the DSP; trigger/start/stop are IPC; the AP never writes BUF_STR/BUF_END
  of any DMA channel (grep: no writes to ABOX_RDMA_BUF_STR/ABOX_WDMA_BUF_STR
  anywhere in the AP driver — only STATUS reads for the pointer callback,
  abox_rdma.c:1629, abox_wdma.c:344, and a burst-len tweak, abox_rdma.c:1445).
  The DSP programs the DMA channel and connects the graph (WDMA→effect→UAIF).
- There is no "plain I2S+DMA" mode in the vendor driver.
- INFERENCE: the hardware itself looks programmable without the DSP — UAIF
  CTRL0/1 (incl. bclk divider) and RDMA/WDMA BUF_STR/END/ENABLE are ordinary
  SFRs in the AP-mapped ifr region; the unknown is the internal router
  (SPUS/SPUM banks) that the Calliope graph normally programs, plus ABOX clock
  tree (CMU at 0x1a240000). A from-scratch Windows/UEFI path would need to
  reverse-engineer SPUS/SPUM bank programming (or find that reset-default
  direct paths exist). calliope2.dt in /vendor/firmware is the graph config
  blob and could be mined for the actual SFR writes.

## Q3 — beyond-madera machine driver (sound/soc/samsung/beyond_madera.c, 2107 lines)

FACT (line refs = lineage-22.2 raw file):
- Driver: platform_driver "beyond-madera", of_match "samsung,beyond-madera"
  (lines ~2070-2090). Card struct `beyond_madera` static.
- probe (lines ~1890-2070): gets clk "xclkout" = <&clock OSC_AUD> (DT
  clock-names), parses cirrus,sysclk/asyncclk/dspclk/fll1-refclk/fll2-refclk/
  outclk/ampclk triples (id, source, rate) from the sound node, then walks DT
  child dailink nodes (rdma0..wdma7, uaif0..3, dsif0, sifs0..4, ...) matching
  them positionally to the static `beyond_dai[]` table: FE links RDMA0-11
  (dpcm_playback, id=1<<n, ops rdma_ops) lines 823-1000, WDMA0-7 (dpcm_capture,
  id=1<<(12+n)) lines 1003-1122, then BE links UAIF0..3 (id 1<<24..1<<27,
  no_pcm, be_hw_params_fixup=abox_hw_params_fixup_helper, init fns
  beyond_uaif0/1/2_init), DSIF, SIFS0-4, RSRC0/1, NSRC0...
  DAI fmt comes from DT `format = "i2s"/"dsp_a"` via snd_soc_of_parse_daifmt.
- samsung,codec = 29 phandles + samsung,prefix ("ABOX"×26, "Left", "Right",
  "VTS") → card->num_configs (codec_conf of_node+prefix, lines ~2035-2045);
  samsung,aux = <&abox_effect> → aux_dev (lines ~2047-2052).
- beyond_uaif0_init (line 605): sets codec sysclk/asyncclk/dspclk/outclk via
  snd_soc_codec_set_sysclk with the parsed clk_conf; registers madera jack
  detection notifier (hpdet/micdet, lines 389-446) and adds custom controls
  (impedance, auxpdm hiz, fm mute, lines 461-601).
- uaif_ops startup/shutdown (lines 202-296): on UAIF0/UAIF1 streams, toggles
  codec asyncclk (MCLK for the amps path) and wake_lock "madera-sound".
- DPCM: FE=RDMA (playback) / WDMA (capture) with snd-soc-dummy codec;
  BE=UAIF/DSIF/SIFS. Playback path: userspace PCM → RDMA FE → (Calliope graph
  inside ABOX) → UAIF1 BE (dsp_a) → CS35L40 amps. Capture: DMIC/IN → CS47L93
  ADC → UAIF0 BE (i2s) → (graph) → WDMA FE → userspace.
  (link wiring = FACT from beyond_dai[] + DT dailink nodes; the "graph in the
  middle" part = established in Q2.)

### Codec/amp bus + interface assignment — FACT from DT
(exynos9820-beyond0lte_common.dtsi, same repo)
- CS47L93: node `cs47l93: cs47l93@0` under `&spi_2` (line 1901),
  compatible "cirrus,cs47l93", spi-max-frequency 25 MHz, reg 0,
  reset-gpios <&gpg1 0 0>, irq <&gpa2 2 0> (interrupts=<2 0 0>),
  also an interrupt-controller + gpio-controller (madera-pinctrl, groups
  aif1/aif3 used). Supplies AVDD/DBVDD1/CPVDD1=ldo17, CPVDD2/DCVDD=ldo18.
- uaif0 (line 493): format "i2s", cpu &abox_uaif_0, codec <&cs47l93 0>
  → **CS47L93 AIF1 on ABOX UAIF0**.
- uaif1 (line 501): format "dsp_a" + bitclock-inversion, codec
  <&cs35l40_l 0 &cs35l40_r 0> → **both CS35L40 amps share ABOX UAIF1**
  (DSP-A TDM, two codecs on one BE).
- uaif2 (line 512): format "i2s", codec <&cs47l93 2> → CS47L93 AIF3
  (pinctrl group aif3 active) — second codec interface (VTS/modem path).
- uaif3 (line 520): i2s, dummy codec — graph-internal (BT mic / modem).
- dsif0 (line 527): format "pdm", cpu &abox_dsif, dummy codec — present but
  no real codec attached (see Q6).
- CS35L40 amps: `cs35l40_r: cs35l40@40` (line 2119) and `cs35l40_l: cs35l40@41`
  (line 2163) under `&hsi2c_38` (I2C bus 38, 400 kHz, pinctrl hsi2c38_bus +
  spk_amp_rst_n + rcv_amp_rst_n + amp_int).
  - cs35l40_r: reg 0x40, reset-gpios <&gph1 1 0>, cirrus,right-channel-amp,
    cirrus,dsp-part-name = "cs35l40-bot" → **right amp = bottom speaker**.
  - cs35l40_l: reg 0x41, reset-gpios <&gph1 2 0>,
    cirrus,dsp-part-name = "cs35l40-rcv" → **left amp = receiver**.
  - IRQ: both interrupt-parent <&gpm24> pin 0 (amp_int line).
  - Amps are AP-controlled (I2C MFD + codec driver), NOT inside Calliope;
    their audio stream is the shared UAIF1 DSP-A bus. Their protection
    firmware runs on each amp's own Halo DSP (wm_adsp), not on Calliope.
    (FACT: cs35l41.c registers wm_adsp dsp1, part name from DT.)

## Q6 — Mic wiring: DMIC into CS47L93, not ABOX DSIF

FACT (exynos9820-beyond0lte_common.dtsi lines 216-231, 1926-1929):
- sound node samsung,routing contains:
  "DMIC1","MICBIAS1A" + "IN3L","DMIC1" and "DMIC2","MICBIAS1B" +
  "IN4L","DMIC2" → two digital mics fed to CS47L93 **IN3L/IN4L** digital-mic
  inputs, powered from MICBIAS1A/1B.
- codec node: `cirrus,dmic-ref = <0 0 1 1>` → IN3/IN4 select DMIC data lines
  (DMIC1/DMIC2 pads); `cirrus,inmode` only set for IN1/IN2 (analog).
- "HEADSETMIC","MICBIAS2A" + "IN1BL","HEADSETMIC" → headset analog mic IN1BL.
- "IN2BL"/"IN2BR","FM IN" → FM tuner analog input.
- "ABOX UAIF3 Capture","BLUETOOTH MIC" → BT mic enters the ABOX graph via
  UAIF3 (modem/SCO path, not a local mic).
- dsif0 link exists (pdm, dummy codec) and DT has dmic-pdm pinctrl, but no
  sound-dai consumer → **DSIF is not the S10e mic path** (inference from
  absence of any codec on dsif0 + explicit DMIC→IN3L/IN4L routing).
- The HAL routing blob "MIC1..MIC4 -> ABOX UAIF3 Capture" (notes/d8-audio.md
  §1.2) is the Calliope-graph-side naming; the kernel DAPM shows the physical
  mics land in CS47L93 IN3/IN4. Reconciliation (INFERENCE): mic capture =
  CS47L93 ADC → UAIF0 → ABOX graph (VTS effect) → WDMA FE; "UAIF3" entries in
  the HAL blob are graph virtual endpoints.

## Q4 — Madera CS47L93 codec driver essentials (vendor tree)

FACT (refs = lineage-22.2):
- Vendor tree has full Madera support incl. CS47L93:
  - MFD: `drivers/mfd/madera-core.c` — of_match `{ .compatible = "cirrus,cs47l93",
    .data = CS47L93 }` (line 423); chip name fn case CS47L93 (194);
    `drivers/mfd/madera-spi.c` picks cs47l93 16/32-bit regmap configs;
    `drivers/mfd/madera-i2c.c` exists but disabled in defconfig (SPI used).
  - Codec: `sound/soc/codecs/cs47l92.c` header line 2: "ALSA SoC Audio driver
    for **CS47L92/CS47L93** codecs" — one driver for both; core in madera.c
    (case CS47L93 at lines 653-3328).
- Reset/init sequence (madera-core.c):
  1. enable DCVDD regulator (line ~291),
  2. hard reset via reset-gpios (madera_get_reset_gpio line 441; DT
     reset-gpios <&gpg1 0 0>),
  3. `madera_soft_reset`: regmap_write(MADERA_SOFTWARE_RESET, 0) +
     usleep_range(1000,2000) (lines 239-254),
  4. `madera_wait_for_boot`: regmap_read_poll_timeout on
     MADERA_IRQ1_RAW_STATUS1 & MADERA_BOOT_DONE_STS1, then ack
     MADERA_BOOT_DONE_EINT1 (lines 207-233),
  5. load reg defaults/patches, register irqchip (MADERA_IRQ), gpiochip,
     pinctrl (aif1/aif3/io groups), regulators (LDO1, MICVDD, MICBIAS1/2...).
- Clocking (sound/soc/codecs/madera.h lines 45-72):
  clk ids: 1=SYSCLK_1, 2=ASYNCCLK_1, 5=SYSCLK_2, 6=SYSCLK_3, 7=ASYNCCLK_2,
  8=DSPCLK, 9=OUTCLK; sources: 0=MCLK1, 1=MCLK2, 4=FLL1, 5=FLL2, 6=FLL3,
  7=FLLAO_HI/FLL1_DIV6, 8=AIF1BCLK, 9=AIF2BCLK, 0xA=AIF3BCLK, 0xF=FLLAO.
  Decoding the S10e DT (exynos9820-beyond0lte_common.dtsi lines 207-214):
  - SYSCLK1 ← FLL1 @ 98.304 MHz; ASYNCCLK1 ← FLL2 @ 98.304 MHz;
    DSPCLK ← FLL1 @ 147.456 MHz (ratio check dspclk/3==sysclk/2 enforced in
    beyond_madera.c probe ~line 1940); OUTCLK ← MCLK2 (auto);
  - **FLL1 ref = MCLK1 @ 26 MHz** (cirrus,fll1-refclk <1 0 26000000>) —
    CORRECTION to the task brief: reference is 26 MHz, not 25 MHz.
  - FLL2 ref = AIF1BCLK @ 26 MHz (cirrus,fll2-refclk <2 8 26000000>) —
    async domain locked to the ABOX UAIF0 BCLK.
  - FLL constants madera.c lines 79-119 (FOUT 90-100 MHz, MAX_FREF 13.5 MHz
    → 26 MHz ref needs REFDIV; FLLAO/FLLHJ variants for CS47L9x).
- AIF register block (madera.c lines 30-44): per-AIF BCLK_CTRL/TX_PIN_CTRL/
  RX_PIN_CTRL/RATE/FORMAT/FRAME_CTRL1..9 — AIF1 = DAI index 0 (UAIF0 link
  <&cs47l93 0>), AIF3 = DAI index 2 (<&cs47l93 2>). (INFERENCE: index→AIF
  mapping from standard madera dai table ordering; DT pinctrl enables aif1 +
  aif3 groups only, consistent.)
- DSP1 on CS47L93: adsp@0FFE00 node, wmfw "cs47l93-dsp1-trace.wmfw",
  bin "None" (trace-only; DT line ~2052; wm_adsp2_codec_probe cs47l92.c:2010).
- Mic path: micbias regulators MICBIAS1 (1.8 V, ext cap) / MICBIAS2 (2.8 V)
  in DT; DMIC data selected by `cirrus,dmic-ref = <0 0 1 1>` (IN3/IN4 =
  DMIC pads); analog IN1 (headset), IN2 (FM). CS47L93 also does
  accdet/hpdet (cirrus,accdet block, lines ~2010-2050) — jack detection.
- Power supplies: AVDD, DBVDD1, CPVDD1 = ldo17; CPVDD2, DCVDD = ldo18.

## Q5 — CS35L40 amp driver essentials

### Vendor (lineage-22.2) — FACT
- Architecture: MFD glue `drivers/mfd/cs35l41-i2c.c` (of_match "cirrus,cs35l40"
  line 40; reg = 0x40/0x41 from DT) → `cs35l41_dev_init` in
  `drivers/mfd/cs35l41-core.c` (line 39): supplies, reset-gpio
  (devm_gpiod_get_optional "reset", toggles 1→0 with delay ~lines 72-95),
  then creates platform device → codec `sound/soc/codecs/cs35l41.c`
  `cs35l41_probe` (line ~2093):
  1. read DEVID, poll IRQ1_STATUS4 for chip-id match (timeout loop,
     CS35L41_CHIP_ID / CS35L41R_CHIP_ID chosen by MTLREVID parity —
     lines 2131-2146 region),
  2. REVA0 errata patch regmap_register_patch (cs35l41_reva0_errata_patch,
     line 2040),
  3. read OTP_TRIM_30/31 (speaker trim, lines ~2201-2202),
  4. `cs35l41_dsp_init` (line 2054): wm_adsp HALO dsp1,
     dsp->part = DT `cirrus,dsp-part-name` ("cs35l40-bot"/"cs35l40-rcv"),
     base CS35L41_DSP1_CTRL_BASE, wm_halo_init; RX5..8 sources = VPMON/
     CLASSH/TEMPMON (feedback channels for the protection algo).
- Firmware files (wm_adsp naming "<part>-dsp1-<system>"): confirmed present on
  device (strings on dumps/hhxj6/vendor/vendor.raw):
  cs35l40-bot-dsp1-spk-prot.wmfw + cs35l40-bot-dsp1-spk-prot.bin +
  cs35l40-bot-dsp1-spk-prot-calib.bin (same trio for -rcv).
  .wmfw = library/pmap, .bin = hybrid firmware, -calib.bin = per-device
  calibration (handled by drivers/mfd/cirrus-cal.c, sysfs class "cirrus"
  from cirrus-amp.c; big-data logging via cirrus-bd.c).
- bot/rcv mapping — FACT from DT: cs35l40_r@0x40 = "cs35l40-bot" (bottom
  speaker, right channel, cirrus,right-channel-amp), cs35l40_l@0x41 =
  "cs35l40-rcv" (receiver). I2C addr pins: CS35L40 has a CS/ADDR pin;
  0x40/0x41 differ by that pin (datasheet: 0x40/0x41/0x48/0x49 combos —
  **UNVERIFIED** exact pin table; two amps on one bus need distinct ADDR).
- Amp audio stream: shared DSP-A TDM on ABOX UAIF1 (Q3); amp protection DSP
  runs locally on each chip's Halo core (not Calliope).

### Mainline — FACT (torvalds/linux master, checked via raw 200/404)
- Standalone cs35l40 files are GONE from master: sound/soc/codecs/cs35l40-i2c.c,
  cs35l40-lib.c, drivers/mfd/cs35l40-i2c.c all 404.
- Current mainline: unified **cs35l41** driver family, no MFD split:
  sound/soc/codecs/cs35l41-i2c.c (i2c_device_id "cs35l40" line 23,
  of_match "cirrus,cs35l40" line 65), cs35l41-spi.c, cs35l41-lib.c,
  cs35l41-tables.c, cs-amp-lib.c (calibration).
  History (INFERENCE from file archaeology, not bisected): cs35l40-i2c.c
  existed ~v6.3..v6.12 as MFD+codec split, later folded into cs35l41.
- Mainline driver ≠ vendor driver: vendor uses Samsung-era Cirrus tree
  (cs35l41.c with wm_adsp HALO + cirrus-cal sysfs); mainline uses
  cs-amp-lib + regmap patches. A from-scratch port can use either as a
  register-sequence reference; the chip init is the same silicon.

## Q7 — Mainline status & other open efforts

### Mainline (FACT, verified against torvalds/linux master via raw.githubusercontent.com today)
- **ABOX is NOT mainlined**: sound/soc/samsung/Makefile has no abox/dmaudio/
  exynos9820 entry (grep rc=1); sound/soc/samsung/abox.c 404. No
  "exynos-dmaudio" exists at all.
- **Madera IS mainline**: drivers/mfd/madera-core.c of_match
  `{ .compatible = "cirrus,cs47l93", .data = CS47L93 }` (line 382),
  mfd_cell cs47l92_devs → codec "cs47l92-codec" (sound/soc/codecs/cs47l92.c,
  platform_driver line 2088) with core sound/soc/codecs/madera.c
  (case CS47L93 lines 2303/2422/2947). So CS47L93 is covered in mainline via
  the cs47l92 driver. (Note: the task brief's "madera-core.c" is the MFD file
  drivers/mfd/madera-core.c, not a codecs/ one.)
- **CS35L40 in mainline**: no standalone cs35l40 files on master (404);
  unified into the cs35l41 driver family: sound/soc/codecs/cs35l41-i2c.c
  (of_match "cirrus,cs35l40" line 65, i2c id line 23), cs35l41-spi.c,
  cs35l41-lib.c, cs35l41-tables.c, cs-amp-lib.c (calibration). MFD split
  (drivers/mfd/cs35l41-*.c) is gone on master (404).
  INFERENCE: cs35l40-i2c.c existed ~v6.3-v6.12 then folded into cs35l41;
  not bisected, marked uncertain.

### Samsung's upstream ABOX attempt (FACT)
- LKML July 2025: "[PATCH 0/9] Add ExynosAuto ABOX generic platform and PCM
  support" by Eunwoo Kim (ew.kim@samsung.com), for **ExynosAutoV920
  (automotive)**, not 9820. Adds sound/soc/samsung/auto_abox/generic/
  (abox_generic.c, "samsung,abox_generic" compatible, global-pointer +
  EXPORT_SYMBOL design). Reviewed by Mark Brown + Krzysztof Kozlowski
  (critical). NOT merged (absent from master today).
  URLs: https://www.spinics.net/lists/devicetree/msg830241.html ,
  https://lkml.org/lkml/2025/7/21/87 ,
  https://www.spinics.net/linux/fedora/linux-sound/msg25810.html (patch 7/9).
- Relevance: confirms ABOX family = "DMA controllers, I2S interfaces, GIC,
  ADSP" (patch cover note) and that Samsung's own upstream design still
  routes PCM through the ABOX stack; no firmware-free mode proposed.

### Other projects (FACT)
- **postmarketOS**: package `linux-postmarketos-exynos9820` v7.2.0-r0,
  "Close-to-mainline kernel for Samsung Exynos 9820 devices", project
  https://github.com/chiffathefox/exynos-9820-mainline-linux (branches
  exynos9820-7.1-rc2, exynos9820-7.2-rc2, master). Checked branch
  exynos9820-7.2-rc2 tree: sound/soc/samsung = stock mainline file list,
  **no abox directory** → the 9820 mainline effort has NOT ported ABOX audio.
  (https://pkgs.postmarketos.org/package/main/postmarketos/aarch64/linux-postmarketos-exynos9820)
  postmarketOS wiki page exists (https://wiki.postmarketos.org/wiki/Samsung_Exynos_9820_(samsung-exynos9820))
  but is behind Anubis bot-check from this network — could not read audio
  status (dead end).
- **sonic011gamer/edk2-exynos** (EDK2 UEFI for Exynos; Renegade Project
  style, README: https://raw.githubusercontent.com/sonic011gamer/edk2-exynos/master/README.md):
  has Silicon/Samsung/Exynos9820Pkg but it contains only Include/, Library/,
  exynos9820.dec — **no audio/abox drivers** (tree listing). Same package
  family our repo builds on.
- **FDValente/Exynos-mainline**: git ls-remote → no refs; github.com page →
  404. Repo does not exist under that name (dead end — recorded so it isn't
  re-searched). Closest real thing found instead: chiffathefox repo above.
- LineageOS still ships the vendor 4.19 kernel for these devices (branches
  up to lineage-23.2 exist in the kernel repo); all S10 Android forks
  (CruelKernel, Evolution-X, GoRhanHee, kafoula/newbit GitLab mirrors) carry
  the same vendor abox tree — nobody has rewritten it.

### Q2 addendum — implementation-grade details (line refs verified)
- PCM IPC message enum (include/sound/samsung/abox_ipc.h lines 16-101):
  SIGNAL_ID {SIGID_SYSTEM=1, SIGID_PCMOUT_TASK0..3, SIGID_PCMIN_TASK0/1,
  SIGID_OFFLOAD, SIGID_ERAP, SIGID_ASB};
  enum PCMMSG {PCM_OPEN=1, PCM_CLOSE=2, PCM_CPUDAI_TRIGGER=3,
  PCM_CPUDAI_HW_PARAMS=4, PCM_CPUDAI_SET_FMT=5, PCM_CPUDAI_SET_CLKDIV=6,
  PCM_CPUDAI_SET_SYSCLK=7, PCM_CPUDAI_STARTUP=8, PCM_CPUDAI_SHUTDOWN=9,
  PCM_PLTDAI_OPEN=11, PCM_PLTDAI_HW_PARAMS=14, PCM_PLTDAI_HW_FREE=15,
  PCM_PLTDAI_TRIGGER=17, PCM_PLTDAI_POINTER=18, PCM_SET_BUFFER=20,
  PCM_SYNCHRONIZE=21, PCM_PLTDAI_ACK=22, PCM_PLTDAI_CLOSED=23,
  PCM_PLTDAI_REGISTER=50};
  struct IPC_PCMTASK_MSG {msgtype, channel_id, union{hw_params{rate,depth,ch},
  setbuff{phyaddr,size,count}, hardware{...}, pointer, trigger, synchronize}}.
- UAIF hw_params (abox_if.c:346-417): AP computes bclk = rate*channels*width
  and calls clk_set_rate(data->clk_bclk) (ABOX audio CMU), then writes
  UAIF_CTRL1 fields: ABOX_SBIT_MAX=width-1, ABOX_VALID_STR/END (0 for I2S,
  width-1 for DSP mode), ABOX_SLOT_MAX=channels-1, ABOX_FORMAT.
- UAIF trigger (abox_if.c:419-460): AP sets/clears ABOX_UAIF_CTRL0
  SPK_ENABLE (playback) / MIC_ENABLE (capture) bits directly.
  → AP owns: UAIF format/clock/enable. DSP owns: DMA buffer prog + routing.
- ABOX GIC (doorbell + IRQ ack): DT abox-gic@18cf0000 actually regs
  0x18CF1000 (gicd, 0x1000) + 0x18CF2000 (gicc, 0x1004)
  (dumps/hhxj6/base.dts:5312-5314). Doorbell = GICv2 SGI:
  writel((0x1<<16)|(irq&0xF), gicd + GIC_DIST_SOFTINT(0xF00)) →
  0x18CF1F00 (abox_gic.c:64-73; alternative SMC path via exynos_smc
  SMC_CMD_REG writing 0x13EF1000+0xF00 when GIC_IS_SECURE_FREE is off —
  i.e. the ABOX GIC may be secured behind EL3 smcinvoke on shipped builds;
  boot-hazard note for the Windows port).
  RX from DSP: abox_gic.c:143-153 reads GICC_INTACK/EOI; AP-side delivery is
  an ordinary Linux IRQ wired from the abox node (SGIs from ABOX GIC to AP).
- RDMA pcm path (abox_rdma.c): hw_params allocates dma buffer, sets
  RDMA_CTRL0 burst len (1445), sends PCM_SET_BUFFER phyaddr=IOVA_RDMA_BUFFER(id)
  (1455-1459) + PCM_PLTDAI_HW_PARAMS (1463-1466); pointer cb reads
  RDMA_STATUS RBUF_OFFSET<<4 (1629-1630); wdma symmetric (abox_wdma.c:344).

## Correction to Q1 wording
- Verified: sound/soc/samsung/abox/abox.c exists on lineage-21 AND lineage-23.2
  (raw 200) — the vendor ABOX tree is carried on all LineageOS branches of that
  kernel repo, not just ≤22.2.

## Dead ends (recorded so they aren't re-searched)
- GitHub code search HTML (needs auth) — unusable; used raw + tree HTML instead.
- GitHub API unauthenticated — avoided per task note.
- FDValente/Exynos-mainline — repo 404 / no refs (see Q7).
- postmarketOS wiki S10 page — Anubis bot-check blocks fetch from this network.
- No cs35l40* file in LineageOS/Cruel sound/soc/codecs (417-entry listing,
  complete) — the driver hides in cs35l41.c/cs35l41-i2c.c (found via DT
  compatible grep of the codec file).
- kallsyms.log in dumps/hhxj6 is empty (2 lines) — can't use for symbol
  location; used strings on kernel/vendor.raw instead.

## Sources
- https://github.com/LineageOS/android_kernel_samsung_exynos9820 (branches via
  git ls-remote; trees + raw files @ lineage-22.2):
  sound/soc/samsung/beyond_madera.c, sound/soc/samsung/abox/* (abox.c,
  abox_core.c, abox_ipc.c, abox_msg.c/.h, abox_if.c, abox_rdma.c, abox_wdma.c,
  abox_soc.h, abox_gic.c), include/sound/samsung/abox_ipc.h,
  sound/soc/codecs/madera.c, madera.h, cs47l92.c, cs35l41.c,
  drivers/mfd/{cs35l41-i2c.c,cs35l41-core.c,cirrus-amp.c,madera-core.c,madera-spi.c},
  arch/arm64/configs/exynos9820-beyond0lte_defconfig,
  arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi,
  exynos9820-beyond0lte_eur_open_25.dts
- https://github.com/CruelKernel/samsung-exynos9820 (branch cruel-HVJ5-v5.3 trees)
- https://raw.githubusercontent.com/torvalds/linux/master/... (existence checks:
  sound/soc/samsung/Makefile, sound/soc/codecs/{madera.c,cs47l92.c,cs35l41-i2c.c,
  cs35l41-lib.c,cs-amp-lib.c}, drivers/mfd/madera-core.c; 404s for abox/cs35l40)
- https://www.spinics.net/lists/devicetree/msg830241.html (ABOX auto patch 1/9, full diff)
- https://lkml.org/lkml/2025/7/21/87 (thread + reviewer list)
- https://www.spinics.net/linux/fedora/linux-sound/msg25810.html (patch 7/9 PCM driver)
- https://pkgs.postmarketos.org/package/main/postmarketos/aarch64/linux-postmarketos-exynos9820
- https://github.com/chiffathefox/exynos-9820-mainline-linux (tree sound/soc/samsung @ exynos9820-7.2-rc2)
- https://github.com/sonic011gamer/edk2-exynos (+ README raw, Silicon/Samsung/Exynos9820Pkg tree)
- https://wiki.postmarketos.org/wiki/Samsung_Exynos_9820_(samsung-exynos9820) (exists; content unreadable — Anubis)
- Local (no URL): dumps/hhxj6/base.dts, dumps/hhxj6/vendor/vendor.raw strings,
  notes/d8-audio.md §1.
