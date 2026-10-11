# D8 audio milestone 1 — talk to the audio chips (CS47L93 codec, CS35L40 amps)

Goal: read the device-ID registers of the Cirrus CS47L93 codec (spi_2) and the
two CS35L40 amps (hsi2c_38 @0x40/0x41) from our UEFI (new DXE, logged to
UEFILOG), built and ready for a device test. Then (time permitting) milestone 2
groundwork: smallest path to audible sound (codec + 3.5 mm jack).

Rules: AGENTS.md (no adb/phone in this session — another session owns the
device; no flashing; UNVERIFIED naming for any constant not pinned from a
source).

## Progress log

- 2026-10-12: worktree branch `worktree-agent-af7371b6f9c487478` was at
  a8d8a62 (264 commits behind main, 0 ahead) -> fast-forwarded to main
  302c285. Inputs read: notes/d8-audio.md, d8-audio-design.md,
  d8-audio-linux.md (Q3/Q4/Q5), drivers-branch-port-plan.md.
- Facts already in notes (with their sources): CS47L93 under `&spi_2`
  (cs0, 25 MHz, reset gpg1-0, IRQ gpa2-2 = GSIV 55, supplies ldo17/ldo18);
  CS35L40 R @0x40 reset gph1-1, L @0x41 reset gph1-2, both on `&hsi2c_38`
  400 kHz, pinctrl hsi2c38_bus + spk_amp_rst_n + rcv_amp_rst_n + amp_int;
  hsi2c_38 = hsi2c@109A0000, GSIV 460 (d8-audio-design.md §3). Live device
  node `109a0000` exists (notes/s10e-live-reads.md §2).
- Progress codes: 0x67 = UsbHostDxe (main), 0x68 = reserved for PCIe WLAN
  (drivers-branch-port-plan.md). Audio probe will use 0x69 (grep pending).
- origin/drivers f22718a constants (ALIVE banks/EINT/keys/ABOX) are NOT on
  main's Exynos9820.h yet.

## 1. Bus / chip facts pinned this session (sources = LineageOS lineage-22.2 raw files, fetched 2026-10-12)

Abbrev: [SOC] = arch/arm64/boot/dts/exynos/exynos9820.dts, [BRD] =
arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi, [PIN] =
arch/arm64/boot/dts/exynos/exynos9820-pinctrl.dtsi, [PCTL] =
drivers/pinctrl/samsung/pinctrl-exynos-arm64.c, [CAL] = cal-if 9820
cmucal-sfr.c/cmucal-node.c (local copy /tmp/opencode/calif, the same source
Exynos9820.h already cites for CMU blocks).

### 1.1 Amps: CS35L40 x2 on hsi2c_38
- hsi2c_38 = `hsi2c@109A0000` +0x1000, "USI17_USI_I2C", irq INTREQ__USI17_I2C,
  clocks DOUT_CLK_PERIC1_USI_I2C + GATE_USI17_I2C, `samsung,usi-i2c-v2`,
  default-clk 200 MHz, gpio_scl/sda = gph0-6/gph0-7 [SOC:3277-3295].
  NOTE: this is the *dedicated I2C* half of USI17 (hsi2c_37 @0x10990000 is
  the USI17_USI half). Board: `&hsi2c_38` okay, 400 kHz [BRD:2110-2117].
- USI mode registers: `usi_17` @0x1082104C (the USI17_USI block) and
  `usi_17_i2c` @0x10821050 [SOC:2381-2395]. The board sets only `&usi_17`
  usi_v2_mode="i2c" [BRD:1257-1260]; `usi_17_i2c` stays disabled, i.e. the
  vendor kernel NEVER writes a mode for the block hsi2c_38 lives in. Mode
  encoding I2C=4 SPI=2 UART=1 (drivers/soc/samsung/usi_v2.c:18-20).
  -> Probe READS 0x10821050 for the log, does not write it.
- The HSI2C block itself: i2c-exynos5.c exynos_usi_init writes USI_CON
  (0xC4) = 0 (reset release) - our Hsi2cReset already does this.
- Pins: hsi2c38_bus = gph0-6, gph0-7, function 3, pud 0, drv 0 [PIN:1869-1875,
  BRD:2068-2074].
- Amp resets: spk_amp_rst_n = gph1-1, rcv_amp_rst_n = gph1-2, function 1
  (output), pin-val 0 [BRD:2075-2090]. cs35l40_r @0x40 reset gph1-1 ("bot"),
  cs35l40_l @0x41 reset gph1-2 ("rcv") [BRD:2119-2173]. No VA/VP supply
  properties in the amp nodes -> dummy regulators, i.e. amp supplies are
  board-fixed rails (no PMIC LDO to enable).
- Reset semantics: cs35l41-core.c:72-92 gets "reset" GPIOD_OUT_LOW (held in
  reset), sleeps 1 ms, sets 1 (releases), sleeps 2 ms. Active-high flag 0
  in DT -> logical 1 = pin high = out of reset.
- ID (sound/soc/codecs/cs35l41.c:2131-2154, include/linux/mfd/cs35l41/
  registers.h): DEVID @0x00000000, REVID @0x00000004; expected DEVID
  0x35A40 (CS35L41_CHIP_ID) when REVID[3:0] (MTLREVID) is even, 0x35B40 when
  odd. REVID 0xA0 = rev A0. I2C regmap (drivers/mfd/cs35l41-i2c.c:25-30):
  32-bit register address, 32-bit value, both big-endian -> read = write
  4 address bytes (no stop), restart, read 4 bytes.
- Pin banks [PCTL:658-667, exynos9820_pin_banks6 = pinctrl_6 =
  pinctrl@10830000 (PERIC1 GPIO) [SOC:1171-1175]]: gph0 @0x0E0 (8 pins),
  gph1 @0x100 (5 pins). Bank type exynos9810_bank_type_off: CON +0, DAT +4,
  PUD +8 (4 bits/pin CON) - same layout as Exynos9820.h's ALIVE note.

### 1.2 Codec: CS47L93 on spi_2
- spi_2 = `spi@10470000` +0x100, "USI_PERIC0_USI01_SPI", compatible
  "samsung,exynos-spi", fifosize 64, `swap-mode`, clocks GATE_USI01_USI +
  DOUT_CLK_PERIC0_USI01_USI, pinctrl spi2_bus [SOC:3503-3522].
- USI mode: `usi_1` @0x1042100C [SOC:2134-2139] (same PERIC0 USI page as the
  touch bus' usi_3 @0x1042101C, Exynos9820.h EXYNOS_PERIC0_USI_SYSREG_BASE),
  board usi_v2_mode="spi" (=2) [BRD:1183-1186].
- Pins: spi2_bus = gpp0-6, gpp0-5, gpp0-4 function 2 [PIN:1298-1303];
  spi2_cs_func = gpp0-7 function 2, pud 3 [BRD:1886-1891]. gpp0 = bank @0x000
  of pinctrl_5 = pinctrl@10430000 (PERIC0 GPIO, already mapped in UEFI)
  [PCTL:646-654].
- controller-data: spi-feedback-delay 1, spi-chip-select-mode 0 =
  MANUAL_CS_MODE (platform_data/spi-s3c64xx.h:40) -> CS driven via
  SLAVE_SEL bit0 (0 = asserted, 1 = SIG_INACT) [BRD:2060-2063].
- CS47L93 node: reg 0 (cs0), spi-max-frequency 25 MHz, no cpol/cpha (mode 0),
  reset-gpios <&gpg1 0 0>, irq gpa2-2; supplies AVDD/DBVDD1/CPVDD1 = ldo17
  (LDO17M "vdd_pmic_codec_1p8", always-on + boot-on), CPVDD2/DCVDD = ldo18
  (LDO18M "vdd_codec_1p2", NOT always-on, NOT boot-on) [BRD:1614-1630,
  1894-1921]. codec_reset pinctrl = gpg1-0 function 1 (output) [BRD:1878-1884].
  gpg1 = bank @0x0A0 of pinctrl_5 (8 pins) [PCTL:652].
- => DCVDD (1.2 V core) is a PMIC LDO (S2MPS19, ACPM/SPEEDY owned) that the
  bootloader is NOT required to leave on. We do NOT program PMIC rails. If
  sboot left LDO18 off the codec cannot answer; the probe logs that outcome.
- ID: madera-core.c:780-797 reads MADERA_SOFTWARE_RESET (reg 0x0000) right
  after the hard-reset release (madera_disable_hard_reset: gpio 1 + 1-2 ms,
  :263-266); CS47L92 and CS47L93 both report CS47L92_SILICON_ID **0x6371**
  (:39, :791). HW revision = reg 0x0001 & 0xFF. BOOT_DONE = reg 0x1880
  (IRQ1_RAW_STATUS_1) bit 7 (registers.h:17-18,1505,7932).
- SPI framing: cs47l92_16bit_spi_regmap reg_bits 32, pad_bits 16,
  val_bits 16, big-endian (cs47l92-tables.c:2356-2362); regmap-spi
  read_flag_mask 0x80 (regmap-spi.c:111) -> read of reg R =
  TX [0x80|R>>24, R>>16, R>>8, R, 0, 0, 0, 0], value = RX bytes 6..7 (BE).
- SPI controller (drivers/spi/spi-s3c64xx.c): regs CH_CFG 0x00, MODE_CFG 0x08,
  SLAVE_SEL 0x0C, INT_EN 0x10, STATUS 0x14, TX_DATA 0x18, RX_DATA 0x1C,
  PACKET_CNT 0x20 (EN bit16), PENDING_CLR 0x24, SWAP_CFG 0x28, FB_CLK 0x2C,
  USI_CON 0xC4 (write 0 = reset release) [:50-61,109,152-155,1412-1431].
  exynos_spi_port_config: rx_lvl_offset 15, tx_st_done 25, clk_from_cmu
  (no CLK_CFG prescaler - SCLK = CMU USI01 clock / 4) [:2193-2199,:815-822];
  fifo_lvl_mask = (64<<1)-1 = 0x7F [:1780]; TX level = STATUS>>6 & mask,
  RX level = STATUS>>15 & mask [:132-134]. hwinit [:1448-1477], polled
  transfer = enable_datapath (RXCH_ON always, PACKET_CNT = len|EN, TX bytes,
  TXCH_ON) then CS assert, then poll RX level [:529-606, :1044-1049, :665-667].
- SPI clock: rate is whatever CMU_PERIC0 DIV_CLK_PERIC0_USI01_USI gives;
  vendor sets it to 4x 25 MHz at runtime. We do NOT write CMU. [CAL] offsets
  (read-only diagnostics): CMU_PERIC0 0x10400000 (+0x1A0 mux USI01 user,
  +0x1808 div USI01 [3:0], +0x207C gate USI01 IPCLK, +0x2080 gate PCLK,
  +0x302C QCH USI01); CMU_PERIC1 0x10800000 +0x8000 (SFR_BLOCK,
  cmucal-sfr.c:37) (+0x2E0 mux USI_I2C user, +0x1838 div USI_I2C, +0x2110/
  +0x2114 gate USI17_I2C IPCLK/PCLK, +0x3074 QCH USI17_I2C). Gate CON bits:
  CG_VAL 21, MANUAL 20, AUTO 28. Unknown parent rate -> SCLK is UNVERIFIED;
  a too-fast SCLK garbles the read (harmless), it cannot damage anything.

### 1.3 Decision (probe shape)
- Both probes: save -> configure bus (USI mode where the kernel sets one,
  pin functions, controller reset/init) -> read ID -> restore everything
  to the handoff values (Pmic.c pattern, 0x14C lesson).
- Reset lines: read CON/DAT first. If the chip does not answer AND its reset
  line is driven low, release it (CON=output, DAT=1, wait like the kernel),
  read again, then put CON/DAT back (chip returns to reset as sboot left it).
  Gated by BEYOND0LTE_AUDIO_PROBE_RELEASE_RESETS (1). These are verified
  constants (pins/function from [BRD]/[PCTL]); no PMIC rail is touched.
- MMIO to add to the UEFI map: SPI2 0x10470000 (page), PERIC0 USI page
  0x10421000 (already in Exynos9820.h, not mapped), PERIC1 GPIO
  0x10830000, PERIC1 USI page 0x10821000, HSI2C38 0x109A0000, CMU_PERIC1
  0x10800000 +0x8000 (read-only diagnostics).
- Progress code 0x69 (free: grep of firmware/Platform + bootshim shows
  0x60,61,63,65,66,67,70,71,72,80 in use; 0x68 reserved for PCIe WLAN).

## 2. Commit reviews (AGENTS.md §4)

- bc71273 (facts + task list) review: checked mechanically that the
  pinctrl_N -> exynos9820_pin_banksN mapping is by alias order
  (exynos9820.dts:49-56 pinctrl0..7 = pinctrl_0..7; exynos9820_pin_ctrl[]
  index 5 = banks5 "PERIC0", index 6 = banks6 "PERIC1"), and cross-checked
  against the device-validated Windows touch driver: gpp1 CON at 0x020 of
  0x10430000 (drivers/Exynos9820Hsi2c/exynos9820hsi2c.h:84) == banks5 gpp1
  @0x020. Bank table use is therefore proven on hardware for PERIC0.
  CS47L93 ID 0x6371 confirmed: madera-core accepts only the *_SILICON_ID
  list and CS47L93 has no own entry, so a CS47L93 must read 0x6371 or the
  shipped kernel would reject it. No follow-ups.
- Task "port f22718a": `git cherry-pick f22718a` applied clean onto main
  (header-only, +79). Duplicate-#define check: `grep -o '#define NAME' |
  sort | uniq -d` on Exynos9820.h = empty. Amended to carry the milestone
  move + this note + attribution (same commit, per §3).
- Task "audio constants": added the `Audio control buses (D8a)` block after
  the ABOX block in Exynos9820.h. Re-read INTREQ__USI17_I2C = 428 (dt-bindings
  exynos9820.h:442) -> GSIV 460; INTREQ__USI01_USI = 384 (:398) -> GSIV 416
  (not added, SPI is polled). Duplicate-define check empty. Syntax is
  checked by the platform build (task "AudioProbeDxe").
- Build environment (this worktree): `firmware/edk2` is not checked out in
  the worktree. Running build-uefi.ps1 through a junction would write into
  main's `firmware/edk2/Build/Beyond0LtePkg` - the FD the other session
  flashes - so builds use a private copy of the script (scratchpad
  `build-private.ps1`): EDK_TOOLS/PACKAGES from main's edk2 (read-only),
  PACKAGES_PATH second entry = this worktree's firmware, WORKSPACE =
  `C:\Users\ilya\code\w8a` (Conf copied from main). First try with WORKSPACE
  under %TEMP% failed in make (`No rule to make target ...object_files.lst`,
  `GNUmakefile: No such file`): main's longest Build path is 234 chars with a
  43-char root, the Temp root is ~110 chars -> > 260 (GnuWin32 make has no
  long-path support). Short root fixes it. Running PowerShell's build with
  `*>` redirection also fails (NativeCommandError on python's stderr under
  ErrorActionPreference Stop) - use Start-Process with redirected streams.
- Task "memory map": six device windows added to mMmioRegions (SPI2_AUDIO
  0x10470000, PERIC0_USI 0x10421000, HSI2C38 0x109A0000, PERIC1_USI
  0x10821000, GPIO_PERIC1 0x10830000, CMU_PERIC1 0x10800000+0x8000).
  Overlap check by hand: none of them falls inside another mapped window
  (CMU_PERIC0 ends 0x10408000, CMU_PERIC1 ends 0x10808000). Build (private
  workspace) exit 0, BEYOND0LTE_UEFI.fd 7 340 032 B, FVMAIN_COMPACT 99%
  (1 192 B free, auto-sized), FVMAIN 32%.
