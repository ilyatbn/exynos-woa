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
- Task "AudioProbeDxe": `Drivers/Beyond0LteAudioProbeDxe/` (.c, .inf,
  AudioProbeHw.h), FILE_GUID ...555554 (...553 left free for PCIe WLAN),
  added to .dsc and .fdf after TouchPowerDxe. Flags
  BEYOND0LTE_AUDIO_PROBE 1, BEYOND0LTE_AUDIO_PROBE_RELEASE_RESETS 1.
  Build (private workspace) exit 0 with -Werror; Guid.xref lists
  `11111111-2222-3333-4444-555555555554 Beyond0LteAudioProbeDxe`;
  .efi 18 944 B; BEYOND0LTE_UEFI.fd 7 340 032 B sha256
  67f009ab27c5c1d7045653ec28673c52a2a168e8db2dbf05648b3590396f43c3;
  FVMAIN_COMPACT 99% (1 944 B free, auto-sized), FVMAIN 32%.
- Review b23cd6d (memory map) + a833302 (AudioProbeDxe), adversarial:
  * HSI2C register/bit values in AudioProbeHw.h diffed mechanically against
    UsbHw.h (28 names, script loop): all equal (ERROR_STATUS 0x2C is local
    to Pmic.c, same value).
  * Madera SPI frame re-derived from regmap semantics: pad_bits sit between
    reg and value; read_flag_mask ORs into the first reg byte; the kernel's
    write-then-read inside one CS window == our 8-byte full-duplex frame
    with the value in RX bytes 6..7. OK.
  * UEFILOG capacity: ring is 1 MiB (Beyond0LteBootLog.h:16), largest
    recorded UEFILOG is 75 KB (uefilog-boot33.txt) -> the probe's lines
    cannot be pushed out before ReadyToBoot.
  * FOUND (fixed in the follow-up commit): comment claimed a "64-deep"
    HSI2C FIFO - not verified anywhere (Pmic.c says 16); reworded to the
    real bound (Length <= 16). FOUND: RX-FIFO-empty timeout in I2cXfer
    returned without I2cReset/dump, unlike every other error path - a
    half-finished read could leave the block mid-transfer for the second
    amp; now dumps + resets. Build exit 0, FD sha256 6c303f10...
  * Accepted risks (recorded, not bugs): the PERIC1 GPIO block's own clock
    gate is not checked before its SFRs are read (only the bus PCLK gates
    are); a hang there is caught by the 0x69 guard on the next boot.
- Task "codec rails log": S2MPS19 L17CTRL 0x56 / L18CTRL 0x57
  (s2mps19-regulator.h:112-113; LDO17M/LDO18M are children of
  s2mps19mfd@00, board dtsi:1334-1663 - not the s2mpb02 ldo17/18 at
  dtsi:1818-1826, which are a different PMIC) read through the existing
  ACPM PmicRead in TouchPowerDxe's ReadyToBoot pass, after the touch rails,
  before DVFS, skipped if the IPC broke. Read only. Build exit 0, FD
  sha256 29de7b4d...
  Review: Ch at that point is still the PMIC channel (FindChannel(PMIC)
  earlier in the same function; the DVFS FindChannel comes after). The
  probe runs at DXE dispatch, this log at ReadyToBoot - so the rail value is
  read ~seconds after the codec probe; LDO18M is only ever changed by the
  kernel, so the value is representative of the probe's moment.
- Review f3fa577 (rails log): offsets 0x56/0x57 re-read from the header
  (s2mps19-regulator.h:112-113); enable field [7:6] mask is the generic
  S2MPS19 LDO layout (header :220-221) used for L19/L27 already. No
  follow-ups.

## 3. Device test procedure (milestone 1)

Image: any boot image packed from this branch's FD (private-workspace build
`w8a\Build\Beyond0LtePkg\DEBUG_CLANGPDB\FV\BEYOND0LTE_UEFI.fd` under
`C:\Users\ilya\code`, or - better - rebuild on main after merge with the
normal `build-uefi.ps1`). Same pack + flash flow as every boot (HANDOFF
§25/§26; only by-name/boot is written). This session did NOT pack or flash.

1. Boot once to Windows (or just past BDS). The probe runs during DXE
   dispatch, well under 1 s, and never fails the boot.
2. From TWRP pull `UEFILOG.TXT` from the ESP (as every boot) and
   `grep -E 'AudioProbe|TouchPower: L1[78]CTRL'`.
3. Expected lines, in order (<> = what we learn):
   ```
   AudioProbe: start (release resets 1)
   AudioProbe: CMU_PERIC1 USI_I2C mux <> div <> | USI17_I2C gate IPCLK <> PCLK <> QCH <>
   AudioProbe: amp bus at handoff: usi_17_i2c mode <> (never written), gph0 CON <>, gph1 CON <> DAT <> (reset R gph1-1 fn <> lvl <>, L gph1-2 fn <> lvl <>)
   AudioProbe: hsi2c38 at handoff: CTL ... USI_CON <>
   AudioProbe: hsi2c38 after init: CTL 8 ... CONF 80000000 ... FS1 F00000 FS3 210000 ... USI_CON 0
   AudioProbe: amp R/bot @40 DEVID 35A40 REVID <> OTPID <> IRQ1_STATUS4 <> (OTP_BOOT_DONE 1) -> CS35L40 ID OK
   AudioProbe: amp L/rcv @41 DEVID 35A40 ... -> CS35L40 ID OK
   AudioProbe: amps R OK, L OK; bus released: gph0 CON <handoff> gph1 CON <handoff> DAT <handoff> USI_CON <handoff>
   AudioProbe: CMU_PERIC0 USI01 mux <> div <> gate IPCLK <> PCLK <> QCH <> (SCLK = ipclk/4, rate UNVERIFIED)
   AudioProbe: codec bus at handoff: usi_1 mode <>, gpp0 CON <>, gpg1 CON <> DAT <> (reset gpg1-0 fn <> lvl <>), codec IRQ gpa2-2 lvl <>
   AudioProbe: spi2 at handoff: ... / spi2 after init: ...
   AudioProbe: codec ID 6371 HW_REV <> IRQ1_RAW_STATUS1 <> (BOOT_DONE <>) -> CS47L92/93 ID OK
   AudioProbe: codec OK; bus released: usi_1 mode <handoff> ...
   AudioProbe: done
   ...
   TouchPower: L17CTRL codec 1.8V (0x56) = 0x?? enable[7:6] = 3 ...
   TouchPower: L18CTRL codec DCVDD 1.2V (0x57) = 0x?? enable[7:6] = <0 or 3> ...
   ```
   (DEBUG %x prints upper-case hex without 0x.) If a chip first fails and
   its reset line was low, a "held in reset ... releasing it" line and a
   second ID line follow.
4. Milestone 1 is **done** when at least one CS35L40 DEVID and the CS47L93
   ID read their expected values on the device. Paste the raw lines here.
5. Reading failures:
   - amp `DEVID read Not Found` before and after the reset release -> no
     ACK: check the `after init` dump (USI_CON 0, CTL 8) and the CMU_PERIC1
     gate/QCH words; next lever = the never-written usi_17_i2c mode (value
     is logged) - a mode write (I2C = 4) is the follow-up experiment, not
     done blind here.
   - `xfer timeout` -> the block is not clocked (IPCLK gate / QCH): CMU
     territory, stop and read cal-if before touching anything.
   - codec `no answer (...)` with L18CTRL enable 0 -> DCVDD off: the fix is
     the kernel's own enable through ACPM (TouchPowerDxe EnsureLdoOn on
     0x57, check vsel first), a separate flag-gated task.
   - codec ID neither 0x0000/0xFFFF nor 0x6371 -> bits move but garbled:
     SCLK too fast (CMU USI01 mux/div in the log) or frame off by a byte
     (compare with the value shifted by 8).
   - boot hangs in the probe (no UEFILOG, progress word ends at 0x69) -> the
     next boot skips it by itself; read `/proc/debug_history` from TWRP
     (AGENTS.md §9) and the WINPRAM progress word.
6. Safety: no PMIC write, no CMU write, no ABOX access. The only outputs
   driven are the three reset lines, low -> high for the ID read, then
   back. The amps cannot play anything (no I2S clock, no firmware, never
   enabled).

## 4. Milestone 2 - smallest path to an audible sound (design, not built)

### 4.1 Cheapest audible test: the codec's own tone generator (NO ABOX, NO Windows driver)
The CS47L93 can make sound from SPI register writes alone: tone generator
1 -> OUT2 mixers -> HPOUT2 -> 3.5 mm jack (the jack is codec-native,
d8-audio.md §7). Already pinned (lineage-22.2):
- MADERA_TONE_GENERATOR_1 = 0x20, TONE1_ENA = bit0 (registers.h:25,2341)
- OUT2LMIX_INPUT_1_SOURCE = 0x690, OUT2RMIX_INPUT_1_SOURCE = 0x698
  (registers.h:467,475); mixer source "Tone Generator 1" = 0x04
  (madera.c:1422-1424 texts / 1574-1576 values)
- OUTPUT_ENABLES_1 = 0x400, OUT2L_ENA = 0x0008 (registers.h:262,4293);
  DAC_DIGITAL_VOLUME_2L = 0x419, OUT2L_MUTE = 0x0100 (:275,4452)
- CLOCK_32K_1 0x100, SYSTEM_CLOCK_1 0x101, SAMPLE_RATE_1 0x102,
  FLL1_CONTROL_1 0x171 (:50-52,72)
Still to pin before any code (each its own research task):
 a. A SYSCLK source with no AP clock work. The DT uses FLL1 <- MCLK1 = AP
    "xclkout" OSC_AUD 26 MHz (needs the AP CLKOUT mux = AP work). Option:
    FLL1 <- MCLK2 (the board's 32 kHz, DT outclk source MCLK2) - which
    clock is on MCLK2 is UNVERIFIED; read the cs47l92 FLL ref limits.
 b. The HPOUT2 power-up sequence the DAPM runs (charge pump, HP_CTRL,
    EDRE, output enable order: madera_hp_ev / madera_out_ev + the cs47l92
    patch table) - transliterate like the SPI master was.
 c. DCVDD: LDO18M must be on (§3 step 3 tells). Enabling = one ACPM
    FUNC_UPDATE (the touch-rail path), flag-gated.
 d. Hand-off: after the beep the codec goes back into reset before
    ExitBootServices (same restore rule as the probe).
Deliverable: flag-gated `BEYOND0LTE_AUDIO_BEEP` (default 0) in
Beyond0LteAudioProbeDxe: ~1 s tone in wired headphones at boot, then codec
back in reset. Proves codec power, SPI writes, clocking and the HP output
stage end to end - the same pieces the Windows driver needs.

### 4.2 Windows: minimal driver architecture (from notes/d8-audio-windows.md Q1)
- Framework: **portcls WaveRT miniport** (SimpleAudioSample skeleton, KMDF
  service, Class=MEDIA, Include=ks.inf,wdmaudio.inf, AddInterface
  KSCATEGORY_*). ACX stays the rung-2 target. A custom non-KS driver is not
  an option: only KS filters become Windows audio endpoints.
- Enumeration: DSDT `Device (AUD0)`, _HID from the reserved range
  (SAMS9823, drivers-branch-port-plan.md), `_CRS` = ABOX ifr
  0x18C50000+0x10000 (UAIF/RDMA), ABOX sysreg 0x18C10000+0x10000, spi_2
  0x10470000+0x1000 + PERIC0 USI page, PERIC0 GPIO (codec reset) - or GpioIo
  through the Exynos9820Gpio provider once it ships - and the codec IRQ
  (gpa2-2, GSIV 55) for jack detect. One driver owns the codec over a
  private SPI master (the UEFI probe's code); an SPB SPI controller driver
  is NOT needed for M2.
- Endpoints for M2: one render endpoint (headphones, KSJACK
  eConnTypeMiniPhoneJack, IsConnected from HPDET); capture and speakers
  later.
- Hard blocker (unchanged): moving PCM from a Windows buffer to UAIF0
  without the Calliope DSP (RDMA -> SPUS/SPUM -> UAIF0 direct route,
  d8-audio-design.md §2.4). Until it closes a Windows endpoint can exist
  but only the tone generator makes sound.

### 4.3 Exact milestone-2 task list (each = one commit)
1. Research: FLL1 reference choice (MCLK2 wiring, 32 kHz ref limits) +
   HPOUT2 enable sequence from cs47l92.c/madera.c DAPM events -> notes.
2. Firmware: ACPM enable of LDO18M behind `BEYOND0LTE_AUDIO_DCVDD` (0) -
   only if M1's L18CTRL read shows it off.
3. Firmware: `BEYOND0LTE_AUDIO_BEEP` (0): reset release, BOOT_DONE poll,
   FLL1, SYSCLK, tone1 -> OUT2 -> HPOUT2 for 1 s, then reset.
4. Device test: headphones in, listen at boot (needs the user).
5. Research: SPUS/SPUM direct route (mine calliope2.dt / SFR diff on
   Android with root reads) - the rung-1 gate.
6. Windows: AUD0 DSDT + portcls skeleton with a loopback-only WaveRT pin
   (no hardware) to prove endpoint creation on ARM64.
7. Windows: real RDMA -> UAIF0 -> AIF1 streaming (after 5).

- Review fefa7c2 (notes): checked the "%x prints upper-case" claim
  against a real UEFILOG (notes/uefilog-boot33.txt shows `5B1B31A1`,
  `0xBA27EF78`) - correct. FOUND: DRIVERS-MILESTONES pointed the device
  test at §4 (it is §3), and the M2-groundwork row was not moved in the
  same commit - both fixed in the follow-up commit.
- RESULT (boot #88): amps @40/@41 NACK (Not Found) on hsi2c38; reset pins gph1-1/2 fn 0 lvl 0 at handoff (held?).
  Codec: ID 0 HW_REV 0 before and after releasing gpg1-0; LDO18M (DCVDD 1.2 V) enable[7:6] = 0 -> codec unpowered,
  as predicted; LDO17M (1.8 V) on. Next (milestone 1b): LDO18M on via ACPM, amp resets high, retry IDs.

## 5. Milestone 1b - codec DCVDD on, amps out of reset, IDs answer

Goal: the CS47L93 and both CS35L40 answer their ID reads (boot #88 result above: codec ID 0,
amps NACK). Rules as §0 (no adb, no flash; the main session runs the device test).

### 5.0 Progress log
- 2026-10-12: worktree `agent-a052cd89fc73ae14a` fast-forwarded to main 7f03e5b.
- Read Beyond0LteAudioProbeDxe.c (runs at DXE dispatch) and Beyond0LteTouchPowerDxe.c (ACPM path,
  runs at ReadyToBoot, TPL_NOTIFY; EnsureLdoOn = PmicRead + PmicUpdate(0xC0, mask 0xC0) + read-back).
  => the codec probe currently runs *before* any ACPM access exists; LDO18M on must be ordered
  before it (design in 5.2).
- Amp-probe bug found by reading the code: the reset release only fires when the pin is an output
  driven low (`PinHeldLow`). Boot #88 logged gph1-1/2 **fn 0 (input)**, lvl 0 -> the release branch
  never ran; the amps were never taken out of reset. Same rule for the codec reset: it was released
  (it reports "before and after"), so gpg1-0 must have been output-low there.
- Primary DT source for this section: the **stock shipped dtbo** (the phone's own, extracted earlier
  to `%TEMP%\opencode\stock\dtbo.dtb`), decompiled with tools/dump-tools/fdt.py (scratchpad
  dtdump.py). [DTBO] line numbers below refer to that decompile (out_0_0.dts).
- Kernel files fetched 2026-10-12 (raw, LineageOS android_kernel_samsung_exynos9820 @ lineage-22.2) to
  the scratchpad: drivers/regulator/s2mps19_regulator.c [REG], include/linux/mfd/samsung/
  s2mps19-regulator.h [REGH], drivers/mfd/madera-core.c [MAD], drivers/mfd/cs35l41-core.c [L41C],
  sound/soc/codecs/cs35l41.c [L41].

### 5.1 Facts (citations)

**LDO18M = codec DCVDD, vsel 0x28 = 1.200 V (VERIFIED)**
- [DTBO:4774-4782] `LDO18M { regulator-name = "vdd_codec_1p2"; min = max = 0x124f80 (1 200 000 uV);
  regulator-ramp-delay = 12000; regulator-enable-ramp-delay = 0x1388 (5000 us);
  regulator-initial-mode = 3; }` - no always-on, no boot-on (matches §1.2 from the LineageOS dtsi).
- [DTBO:4764-4773] `LDO17M "vdd_pmic_codec_1p8"` 1 800 000 uV, always-on, boot-on.
- [REG:476-477] `LDO_DESC("LDO18M", _LDO(18), &_ldo_ops(), 3, _REG(_L18CTRL), _REG(_L18CTRL), ...)` ->
  voltage group 3; [REG:406-420] LDO_DESC: min_uV = _LDO_MIN(g), uV_step = _LDO_STEP(g), vsel_mask 0x3F,
  enable_mask S2MPS19_ENABLE_MASK; [REGH:208-209] `S2MPS19_LDO_MIN3 700000`, `S2MPS19_LDO_STEP3 12500`.
  => vsel 0x28 (40) -> 700 000 + 40 x 12 500 = **1 200 000 uV**. The live value read on boot #88
  (L18CTRL = 0x28) is exactly the DT voltage. A different vsel would be refused by the firmware.
- Cross-check with the rail that is on: LDO17M group 1 [REG:474-475], MIN1 700 000 / STEP1 25 000
  [REGH:204-205]; boot #88 L17CTRL 0xEC -> vsel 0x2C (44) -> 700 000 + 44 x 25 000 = 1 800 000 uV = the DT
  value. The formula reproduces a live, known-good rail.
- Enable: [REG:120-126] s2m_enable = update_reg(enable_reg, opmode[id], enable_mask); opmode from
  initial-mode 3 -> s2mps19_of_map_mode(3) = 0x3 [REG:93-104], shifted by S2MPS19_ENABLE_SHIFT 6
  [REGH:220-221] -> value 0xC0 under mask 0xC0 = exactly TouchPowerDxe's EnsureLdoOn update. Register
  0x57 [REGH:113].
- Regulator core waits enable-ramp-delay (5000 us, DT) after enable; S2MPS19_ENABLE_TIME_LDO 128 us
  [REGH:225] is the descriptor minimum. Use 5000 us.

**CS47L93 power-up order (madera-core)**
- [MAD:43-46] core supplies = AVDD, DBVDD1 (both LDO17M here, already on).
- [MAD:755-771] probe: `msleep(20)` "Ensure period of reset asserted before we apply the supplies" ->
  regulator_bulk_enable(core) -> regulator_enable(DCVDD) -> madera_disable_hard_reset.
- [MAD:261-266] disable_hard_reset: gpio 1, usleep_range(1000, 2000).
- [MAD:778-797] read MADERA_SOFTWARE_RESET (0x0000) = silicon ID; CS47L92/93 = 0x6371.
- [MAD:204-235] madera_wait_for_boot: poll IRQ1_RAW_STATUS_1 for BOOT_DONE_STS1, interval <= 5000 us,
  timeout 25 000 us; then acks BOOT_DONE_EINT1 by a write to IRQ1_STATUS_1 (we will NOT write it: the
  codec goes back into reset afterwards, which clears it).
- => firmware order: gpg1-0 output low (reset asserted) >= 20 ms -> LDO18M on -> 5 ms ramp ->
  gpg1-0 high -> 2 ms -> ID read -> BOOT_DONE poll (5 ms steps, 25 ms) -> gpg1-0 back to handoff.

**CS35L40 amps - why they NACKed**
- [DTBO:5292-5360] cs35l40@40 reset-gpios <&gph1 1 0>, cs35l40@41 reset-gpios <&gph1 2 0> (flags 0 =
  GPIO_ACTIVE_HIGH). **No VA-supply / VP-supply properties** in either node.
- [L41C:34-36] supplies "VA", "VP" via devm_regulator_bulk_get -> without DT properties these are dummy
  regulators: the rails are board-fixed, there is no PMIC LDO or enable GPIO to switch. No
  amp-power-enable GPIO exists in the overlay (only hsi2c38-bus, spk-amp-rst-n, rcv-amp-rst-n, amp-int
  in the pinctrl-0 of the bus node [DTBO:5291]).
- [L41C:72-92] reset: devm_gpiod_get_optional("reset", GPIOD_OUT_LOW) (output, logical 0 = pin low =
  in reset) -> usleep_range(1000, 1100) -> set 1 (pin high = out of reset) -> usleep_range(2000, 2100).
  Then DEVID/REVID [L41:2131-2154] and OTP_BOOT_DONE poll, 1 ms steps, 100 tries [L41:2100, 2185-2197].
- [DTBO:5239-5258] spk-amp-rst-n gph1-1 / rcv-amp-rst-n gph1-2: function 1 (output), pin-val 0,
  pud 0, con-pdn 3 - the kernel's pinctrl state drives them **output low**; only the driver lifts them.
- Boot #88: gph1-1/2 were fn 0 (input) lvl 0 at handoff = amps in reset (an input with no
  pull cannot hold RESET high; the line reads 0). Our probe's release condition was
  `PinHeldLow` = output AND low, which an input pin never matches -> **the release never ran**. Root
  cause of the NACK = our probe's condition, not the bus: the NACK itself proves the controller drove
  SCL/SDA and got no ACK (a dead/unclocked block times out instead).
- hsi2c38 USI mode: logged 4 (= I2C, usi_v2.c:18-20) at handoff; the kernel never writes it
  (§1.1) and it already reads I2C -> nothing to write.
- Bus speed: DT clock-frequency 0x61a80 = 400 kHz [DTBO:5289] = what we program.

### 5.2 Design (milestone 1b)
- Ordering problem: AudioProbeDxe runs at DXE dispatch; the only ACPM/PMIC writer (TouchPowerDxe) runs
  at ReadyToBoot. Moving ACPM traffic earlier would be a new, untested ACPM timing; instead:
  * AudioProbeDxe entry (flag `BEYOND0LTE_AUDIO_DCVDD` = 1): amp probe as before (fixed release rule),
    codec: save gpg1 handoff CON/DAT, assert the codec reset (gpg1-0 output low - madera's "reset
    asserted >= 20 ms before supplies"; DXE->ReadyToBoot is seconds), register a TPL_NOTIFY event in
    the new event group `gBeyond0LteCodecPowerEventGuid` and defer the codec probe to it.
  * TouchPowerDxe ReadyToBoot (after the touch rails, same ACPM channel): L17CTRL read (log), L18CTRL
    read; if enable[7:6] != 3 and vsel == 0x28 (1.2 V, 5.1) -> FUNC_UPDATE(0xC0, mask 0xC0), read back,
    wait 5000 us (DT enable-ramp-delay). vsel != 0x28 -> refuse, log. Then - in every path, including
    "no ACPM channel" - signal the event group once.
  * The group event (TPL_NOTIFY) is queued (DxeCore Event.c:235 InsertTailList on gEventQueue[NotifyTpl])
    during TouchPower's own TPL_NOTIFY ReadyToBoot handler and
    dispatched by the same TPL_NOTIFY dispatch loop (Event.c:153-200 CoreDispatchEventNotifies walks
    the queue until empty), i.e. before BootLogDxe's TPL_CALLBACK UEFILOG flush: the codec lines land
    in the same boot's UEFILOG.
- Codec probe (deferred): progress guard 0x69 again; bus up; ID read with reset held (expect 0 - the
  "before" point); release reset (pin high), 2 ms, ID read, BOOT_DONE poll 5 ms x 5; restore bus, then
  the reset pin to the *handoff* CON/DAT saved at entry (= codec back in reset).
- Amps: release rule becomes "pin not an output driven high" (was: output driven low). Vendor sequence:
  DAT 0 + output (reset asserted), 1 ms, DAT 1, 3 ms (kernel 2-2.1 ms), DEVID read. Afterwards the
  reset pins go back to handoff (input) -> amps back in reset.
- State left behind for Windows: **LDO18M stays ON** (codec DCVDD 1.2 V; the codec itself is held in
  reset, which is the madera probe's own pre-release state - no current path beyond reset leakage).
  Every pin/bus register is restored. Nothing else stays on.
- Build env this session: private copy of build-uefi.ps1 (scratchpad `build-private.ps1`): tools +
  edk2 packages from main's firmware/edk2 (read only), PACKAGES_PATH 2nd entry = this worktree's
  firmware, WORKSPACE = `C:\Users\ilya\code\w8b` (Conf copied from w8a; w8b so a concurrent w8a user
  is not clobbered). Run via Start-Process with redirected streams.
- Task "amp reset rule" (AudioProbeDxe): `PinHeldLow` -> `PinNotDrivenHigh` (release unless the pin is
  already output-high), amps get the vendor sequence (PinDriveLow, 1 ms, PinDriveHigh, 3 ms), new log
  lines "gph1 PUD", "amp ... in reset (gph1-N fn F lvl L) - output low, 1 ms, high", "amp resets
  released: gph1 CON DAT". Codec call site uses the same rule (behaviour unchanged for boot #88, where
  gpg1-0 was output-low). Build exit 0 (FVMAIN_COMPACT 99%, 544 B free).
- Task "LDO18M on" (TouchPowerDxe + new shared header `Beyond0LteAudioProbeDxe/Beyond0LteAudioPower.h`:
  flag `BEYOND0LTE_AUDIO_DCVDD` (default 1), event group GUID fd6b0be6-c838-4907-ace9-e459868c6826).
  The read-only L17/L18 loop became `CodecRails`: L17 read; L18 read -> already on: no write; flag or
  TOUCH_POWER_WRITE 0: no write; vsel != 0x28: REFUSE; else PmicUpdate(0x57, 0xC0, 0xC0), 5000 us,
  read back (logs "VSEL CHANGED!" if the APM moved the voltage bits). Then `SignalCodecPower` - also on
  the "no ACPM channel" early return and when the IPC broke. Nothing listens yet (next task). **This is
  the PMIC write of milestone 1b.** Build exit 0, FVMAIN_COMPACT 176 B free (auto-sized FV).

### 5.3 Commit reviews (AGENTS.md §4)
- Review 1cc53ce (facts): cited [DTBO] line numbers re-checked with grep on the decompile (spk-amp-rst-n
  5239, clock-frequency 5289, pinctrl-0 5291 - three cites were off by a few lines and were fixed before
  the commit). vsel formula cross-checked on the live, known-good LDO17M (0xEC -> 1.8 V = DT). OK.
- Review (amp reset rule): `grep PinHeldLow` in the driver = none left; the codec call site changed
  rule too (its boot #88 state, output-low, still matches -> same behaviour). Restore order DAT then
  CON puts the line low while still an output, then input = back in reset. An amp that answered on the
  first read is never toggled. No follow-ups.
- Review (LDO18M on): DxeCore checked - CoreSignalEvent on a group member signals every event of the
  group (Event.c CoreNotifySignalList), the TPL_CALLBACK empty event queued by the signal is unlinked by
  CloseEvent. The CodecRails path is gated by both flags and the vsel check; `PmicUpdate` mask 0xC0 cannot
  touch vsel. FOUND: the SignalCodecPower comment said the codec probe "runs now"; it is only queued and
  runs after the handler (incl. the DVFS block) returns - comment fixed in the follow-up commit.
- Task "deferred codec probe" (AudioProbeDxe): entry saves gpg1 CON/DAT (handoff), asserts gpg1-0
  (PinDriveLow) and registers `OnCodecPower` (TPL_NOTIFY, group fd6b0be6-...). Event-create failure ->
  pin restored and the codec probed at once (old flow). `CodecProbe(HandoffCon, HandoffDat)` restores the
  reset to the *handoff* values; new `CodecWaitBoot` polls BOOT_DONE 5 ms x 5 after the release (no ack
  write). OnCodecPower closes its event, runs once, sets/restores progress 0x69. If TouchPowerDxe never
  signals, the codec stays held in reset (= its boot #88 handoff state) and no codec lines appear.
  Build exit 0: AudioProbeDxe.efi 20 480 B, FD 7 340 032 B sha256 645c8328a0c7889f...,
  FVMAIN_COMPACT 3 728 B free (auto-size grew one block), FVMAIN 33%.
- Review (deferred codec probe): checked the guard path (a hang in OnCodecPower leaves [1] = 0x69 ->
  next boot's [0] = 0x69 -> the whole probe incl. the reset assert is skipped), Stall at TPL_NOTIFY
  (allowed), event closed in its own notify (allowed for a signal event), RELEASE_RESETS 0 keeps the
  old non-deferred flow. FOUND: the end-of-probe restore wrote the *whole* gpg1 CON/DAT back from values
  saved seconds earlier at DXE entry - would clobber any other gpg1 pin changed in between (none today:
  grep finds no other gpg1 user, but the hazard is real). Follow-up: restore only the reset pin's CON
  nibble + DAT bit; the event-create-failure branch no longer restores twice. Build exit 0, FD sha256
  5aece1ce38616270..., sizes unchanged.

### 5.4 Device test procedure (milestone 1b) - for the main session

Build on main after merging this branch (normal `build-uefi.ps1 -Platform Beyond0LtePkg`), pack + flash
as every boot (only by-name/boot). Boot once to Windows (or past BDS), pull `UEFILOG.TXT` from TWRP,
`grep -E 'AudioProbe|TouchPower: (L1[78]CTRL|signalling)'`.

**PMIC write in this image:** S2MPS19 0x57 (LDO18M) enable field 0b11 through ACPM FUNC_UPDATE
(0xC0 / mask 0xC0 - the kernel's own s2m_enable), only if the register reads vsel 0x28 (1.2 V).
Disable: build with `-D`/edit `BEYOND0LTE_AUDIO_DCVDD 0` in Beyond0LteAudioPower.h (back to milestone-1
behaviour, read only).

Expected lines on success, in order (`<>` = learned on the device; DEBUG %x = upper-case hex, no 0x):
```
AudioProbe: start (release resets 1, codec DCVDD flow 1)
AudioProbe: CMU_PERIC1 USI_I2C mux <> div <> | USI17_I2C gate IPCLK <> PCLK <> QCH <>
AudioProbe: gph1 PUD <> at handoff (4 bits/pin; reset R <> L <>, 0 = no pull) - read only
AudioProbe: amp bus at handoff: usi_17_i2c mode 4 (never written), gph0 CON <>, gph1 CON <> DAT <> (reset R gph1-1 fn 0 lvl 0, L gph1-2 fn 0 lvl 0)
AudioProbe: hsi2c38 at handoff: ... / after init: ...
AudioProbe: hsi2c38 addr 40 write: INT_STAT <> ERR <>          (first try, amps still in reset)
AudioProbe: amp R/bot @40 DEVID read Not Found
AudioProbe: hsi2c38 addr 41 write: ... / amp L/rcv @41 DEVID read Not Found
AudioProbe: amp R/bot in reset (gph1-1 fn 0 lvl 0) - output low, 1 ms, high for the ID read
AudioProbe: amp L/rcv in reset (gph1-2 fn 0 lvl 0) - output low, 1 ms, high for the ID read
AudioProbe: amp resets released: gph1 CON <fn 1 in nibbles 1,2> DAT <bits 1,2 set>
AudioProbe: amp R/bot @40 DEVID 35A40 REVID <> OTPID <> IRQ1_STATUS4 <> (OTP_BOOT_DONE <>) -> CS35L40 ID OK   (35B40 if REVID[3:0] odd)
AudioProbe: amp L/rcv @41 DEVID 35A40 ... -> CS35L40 ID OK
AudioProbe: amps R OK, L OK; bus released: gph0 CON <handoff> gph1 CON <handoff> DAT <handoff> USI_CON <handoff> (resets back to their handoff level)
AudioProbe: codec reset asserted (gpg1 CON <h> -> <h, nibble0=1>, DAT <h> -> <bit0=0>); codec probe deferred to TouchPowerDxe's LDO18M step at ReadyToBoot
AudioProbe: done (codec part pending)
... (other drivers, BDS) ...
TouchPower: L17CTRL codec 1.8V (0x56) = 0xEC  enable[7:6] = 3  vsel[5:0] = 0x2C (read only)
TouchPower: L18CTRL codec DCVDD 1.2V (0x57) = 0x28  enable[7:6] = 0  vsel[5:0] = 0x28
TouchPower: ACPM seq <> sent <0x0157|seq> <C0C002> -> reply ...
TouchPower: L18CTRL codec DCVDD after enable = 0xE8 (Success)  enable[7:6] = 3, waited 5000 us
TouchPower: signalling codec-power group (codec rails step done)
TouchPower: DVFS ... (unchanged)
AudioProbe: codec-power signalled - deferred codec probe (reset held since entry)
AudioProbe: CMU_PERIC0 USI01 ... / codec bus at handoff: ... | now gpg1 ... (reset gpg1-0 fn 1 lvl 0) ...
AudioProbe: spi2 at handoff: ... / spi2 after init: ...
AudioProbe: codec ID 0 HW_REV 0 IRQ1_RAW_STATUS1 0 (BOOT_DONE 0) -> no answer (...)      (reset still held: expected)
AudioProbe: codec in reset (gpg1-0 not driven high) - releasing it for the ID read
AudioProbe: codec ID 6371 HW_REV <> IRQ1_RAW_STATUS1 <> (BOOT_DONE <>) -> CS47L92/93 ID OK
AudioProbe: codec BOOT_DONE after <0..25000> us (IRQ1_RAW_STATUS1 <bit7 set>)
AudioProbe: codec OK; bus released: usi_1 mode <h> gpp0 CON <h> gpg1 CON <h> DAT <h> USI_CON <h> (reset back to its handoff level)
AudioProbe: deferred codec probe done
```
Milestone 1b is **done** when both `CS35L40 ID OK` and `CS47L92/93 ID OK` appear. Paste the raw lines here.

Reading failures:
- amps still `Not Found` after "amp resets released" -> the reset was not the (only) cause: compare
  `gph1 CON/DAT` in that line (must show output + high), then suspect the amp VA rail (board-fixed,
  no DT supply - nothing we can switch) or SDA/SCL pull-ups; next read = Android `i2cdetect`-free
  check of /sys/bus/i2c/devices on the stock ROM (needs root) - do not guess further.
- `REFUSING to enable` -> L18CTRL vsel was not 0x28: no write happened; record the value.
- `L18CTRL ... after enable` shows enable 0 or `VSEL CHANGED!` -> the APM rejected/altered the update;
  stop, do not retry blindly.
- No `AudioProbe: codec-power signalled` line although `TouchPower: signalling` is there -> the group
  event did not dispatch before the UEFILOG flush (ordering assumption wrong): look in the next boot's
  UEFILOG tail / the FB console.
- codec still ID 0 after the release with DCVDD on -> SPI side (SCLK rate UNVERIFIED, CS mode); compare
  the spi2 dumps with boot #88.
- Hang (progress word ends 0x69, no UEFILOG) -> the next boot skips the whole probe by itself; read
  `/proc/debug_history` from TWRP (AGENTS.md §9).

State handed to Windows: LDO18M **on** (left on deliberately - the codec is held in reset, the madera
pre-release state); amp and codec reset pins, gph0/gpp0 pin functions, USI modes, hsi2c38/spi2
controller registers all back at their handoff values.

## RESULT milestone 1b (boot #92, unattended, 2026-10-12 ~01:40) - ALL THREE CHIPS ANSWER
- Amps: at handoff gph1-1/2 fn 0 (input) lvl 0 -> NACK; after release (output low, 1 ms, high): gph1 CON 110 DAT 6,
  @40 DEVID 35A40 REVID A0 OTPID 3 OTP_BOOT_DONE 1 -> CS35L40 ID OK; @41 same -> OK. Bus + resets restored after.
- LDO18M: 0x28 (off) -> 0xE8 (enable 3, vsel 0x28 = 1.2 V) Success, 5000 us ramp; group signalled at ReadyToBoot.
- Codec: first read ID 0 (reset held), release gpg1-0 -> ID 6371 HW_REV 1 -> CS47L92/93 ID OK; BOOT_DONE after
  5000 us (IRQ1_RAW_STATUS1 0x80). Reset back to handoff level. Milestone 1b DEVICE-VALIDATED.
- Next (milestone 2): codec tone generator -> OUT2 -> 3.5 mm jack (SPI only, notes section 4) - needs a codec SYSCLK
  source decision (MCLK from the AP or the codec FLL on a 32 kHz ref).

## 6. Milestone 2 - codec tone generator -> 3.5 mm jack (SPI only)

Goal: ~1 kHz tone on the headphone jack for ~2 s at boot, flag `BEYOND0LTE_AUDIO_TONE` (1 for the test
build, 0 once validated), every step proven by a register read-back in UEFILOG (the test runs unattended;
the user listens later). Rules as §0: no adb, no flash, no pack; the main session runs the test.

### 6.0 Progress log
- 2026-10-12: worktree `agent-a5da5ac72292bcd28` fast-forwarded to main e3f72a2. Read §1-§5 + RESULT 1b.
  Local kernel copies from earlier sessions: scratchpad b82d8ce8.../k (madera-core.c, madera-spi.c,
  madera registers.h, sound/soc/codecs/madera.c/.h). Missing: cs47l92.c, cs47l92-tables.c - fetch next.
- Fetched 2026-10-12 (LineageOS android_kernel_samsung_exynos9820 @ lineage-22.2, raw) into scratchpad
  `m2src/`: sound/soc/codecs/cs47l92.c [L92], drivers/mfd/cs47l92-tables.c [L92T], sound/soc/codecs/madera.c
  [MC], madera.h [MH], include/linux/mfd/madera/registers.h [MR], sound/soc/samsung/beyond_madera.c [BM],
  drivers/extcon/extcon-madera.c [EXT]. Board DT = the stock dtbo decompile [DTBO] (scratchpad dt/out_0_0.dts).
  madera-core.c = scratchpad k/drivers_mfd_madera-core.c [CORE].

### 6.1 Facts (citations)

**Jack = HPOUT2 (OUT2), not OUT1.** [DTBO:1481] `samsung,routing` decoded (NUL-split string pairs):
`HEADPHONE <- HPOUT2L`, `HEADPHONE <- HPOUT2R`, `RECEIVER <- Left AMP SPK`, `SPEAKER <- Right AMP SPK`,
`HEADSETMIC -> IN1BL`. Accessory-detect node `acc@1 reg = <2>` (output 2). [L92:939-944] OUT2L/OUT2R widgets =
`madera_hp_ev` on OUTPUT_ENABLES_1 bits 3/2; [L92:1694-1695] HPOUT2L/R <- OUT2L/R.

**Vendor clocking (what Android does)** [DTBO:1469-1480] + [BM]:
- `cirrus,sysclk = <1 4 98304000>` = SYSCLK_1, src FLL1 (MADERA_CLK_SRC_FLL1 = 4 [MH:57]), 98.304 MHz.
- `cirrus,fll1-refclk = <1 0 26000000>` = FLL1, ref src MCLK1 (MADERA_FLL_SRC_MCLK1 = 0 [MH:31]), 26 MHz.
- clock-names "xclkout": [BM:350] `clk_enable(xclkout)` right before set_sysclk + set_pll(FLL1, MCLK1, 26 MHz)
  [BM:352-364] on BIAS_OFF->STANDBY; `clk_disable` at BIAS_OFF [BM:379]. => MCLK1 = an AP clock-out of the
  26 MHz OSC (fixed-rate `oscclk` 26 MHz [DTBO:1328]) that only runs while audio is active. Using it = AP
  CMU/PMU clock-out work (avoid).
- `cirrus,outclk = <9 1 0>` = OUTCLK src ASYNCCLK (MADERA_OUTCLK_ASYNCCLK = 1 [MH:69]). Not needed: OUT_CLK_SRC
  reset default 0 = SYSCLK ([L92T:263] OUTPUT_RATE_1 = 0x0040, [MH:68]).

**MCLK2 = the codec's 32 kHz input.** [CORE:917-921] every madera probe does "Init 32k clock sourced from
MCLK2": CLOCK_32K_1 (0x100) |= CLK_32K_ENA (0x40) | MADERA_32KZ_MCLK2 (1) [CORE:41, MR:2477-2481]. What drives
MCLK2 on this board and whether it runs at handoff is **UNVERIFIED** - no DT property names it. The FLL lock bit
is the proof. The real rate (32768 vs 32000 Hz) is also UNVERIFIED; with the integer config below a 32000 Hz
ref would lock at 48.0 MHz -> tone 2.3 % low - harmless.

**Decision: FLL1 <- MCLK2 (32 kHz) first; fallback FLL1 <- MCLK1 (26 MHz) only if MCLK2 does not lock** (the
fallback writes nothing on the AP side - it only finds out whether sboot left the clock-out running). No AP
CMU/PMU/ABOX write in either case.

**CS47L92/93 FLL = "FLLHJ"** ([L92:1741-1751] madera_fllhj_set_refclk; base = FLL1_CONTROL_1 - 1 = 0x170
[L92:2148]). Offsets [MC:105-127]: CONTROL_1 +1 (0x171) .. _6 +6 (0x176), GAIN +8 (0x178), CONTROL_10 +0xA
(0x17A), CONTROL_11 +0xB (0x17B), DIGITAL_TEST_1 +0xD (0x17D) [MR:72-87]. set_refclk halves fout ("sysclk rate,
which is 2x the calculated fll out") [MC:4971-4976] -> fout 49 152 000. madera_fllhj_apply [MC:4737-4881],
constants [MC:85-101]:
- MCLK2 32768 Hz: refdiv 0, fref < LOW_THRESH 192000 -> lockdet_thr 2, gains LOW 0x23F0; 49152000 % 32768 = 0
  -> integer: fbdiv 4, hp 0, N 1..1023; gcd(49152000, 131072) = 131072 -> num 375, lambda 1 ->
  **N 375 (0x177), THETA 0, LAMBDA 1, FB_DIV 4**, fast_clk 0.
- MCLK1 26 MHz (fallback): refdiv 1 (13 MHz <= MAX_THRESH 13 MHz), fref >= MID_THRESH -> lockdet 8, gains
  HIGH 0x21F0, fbdiv 1; fractional -> fast_clk 3, fout x6 = 294 912 000, hp 3, N 4..255 (numbers in 6.2).
Enable madera_fllhj_enable [MC:4883-4942]: CONTROL_1 HOLD(bit2)=1; apply (CONTROL_2 = N - plain write, clears
CTRL_UPD; _3 THETA; _4 LAMBDA; _5 FB_DIV[9:0]; _6 REFCLK_DIV[7:6]; GAIN = gains; _10 HP[15:14] + PHASEDET_ENA
bit12; _11 LOCKDET_THR[4:1]; DIGITAL_TEST_1 SYNC_EFS_ENA bit8 + CLK_VCO_FAST_SRC[1:0] = fast_clk); CONTROL_1
REFCLK_SRC[15:12] = src [MR:2640-2641]; CONTROL_1 ENA(bit0)=1; CONTROL_11 LOCKDET(bit0)=1; CONTROL_2
CTRL_UPD(bit15)=1; CONTROL_1 HOLD=0; wait lock. Bits [MR:2643-2733].
Lock = IRQ1_RAW_STATUS_2 (0x1881) FLL1_LOCK_STS1 bit8 [MR:1506, 7946]; madera_wait_for_fll polls ~300 ms max
[MC:4187-4218]. Disable madera_fllhj_disable [MC:4695-4735]: CONTROL_11 LOCKDET=0, CONTROL_1 HOLD=1, ENA=0, wait
lock clear, CONTROL_2 CTRL_UPD=0. Reset defaults [L92T:161-172]: 0x171 0x7004, 0x172 0x0004, 0x175 0x0001,
0x176 0x8000, 0x178 0x21F0, 0x17A 0, 0x17B 0x0011, 0x17D 0x33E8.

**SYSCLK**: SYSTEM_CLOCK_1 0x101: FRAC bit15, FREQ[10:8], ENA bit6, SRC[3:0] [MR:51, 2486-2497]; set_sysclk
[MC:2995-3073]: 98.304 MHz = MADERA_SYSCLK_98MHZ 4 [MC:71, 2889-2891], no FRAC. Reset default 0x0404 [L92T:145] =
FREQ 4, SRC 4 (FLL1) -> only ENA has to be set. SYSCLK_ENA = DAPM "SYSCLK" supply [L92:744-746]; POST_PMU
madera_sysclk_ev = madera_spin_sysclk: 4 dummy reads of reg 0 + 300 us [MC:174-209].
**Sample rate**: SAMPLE_RATE_1 0x102 [4:0] [MR:52, 2502], reset 0x0011 [L92T:146] = 8 kHz (madera_sr_vals[17]
[MC:3216-3241]) -> set 0x03 (48 kHz). OUTPUT_RATE_1 OUT_RATE[15:11] default 0 = SAMPLE_RATE_1 [MC:3249-3252].
**Hardware patch** applied by madera-core right after the ID check [CORE:903-910] = cs47l92_patch
[L92T:22-118]: 38 x 16-bit writes + 28 x 32-bit writes (0x3030..0x31D8, output write-sequencer entries) through
the 32-bit regmap (reg 32, pad 16, val 32, BE, stride 2 [L92T:2391-2405]). Applied the same way first (values
copied verbatim).

**Tone -> OUT2**: TONE_GENERATOR_1 0x20: TONE_RATE[15:11] (0 = SAMPLE_RATE_1), TONE1_OVD bit4, TONE1_ENA bit0
[MR:25, 2323-2343]; default 0 [L92T:122]. "Tone Generator 1" PGA needs SYSCLK [L92:980, 1492]. Mixer source
"Tone Generator 1" = 0x04 (§4.1). OUT2LMIX_INPUT_1_SOURCE/VOLUME 0x690/0x691, OUT2RMIX 0x698/0x699 [MR:467-476];
volume [7:1] [MH:74-75], madera_mixer_tlv -32 dB + 1 dB/step [MC:1741] -> 0x40 = 0 dB (reset 0x0080).
DAC_DIGITAL_VOLUME_2L/2R 0x419/0x41D: VU bit9, MUTE bit8, VOL[7:0] [MR:4447-4457]; madera_digital_tlv -64 dB +
0.5 dB/step [MC:1732] -> 0x80 = 0 dB; reset 0x0180 = muted [L92T:273,277].
OUTPUT_ENABLES_1 0x400: OUT2L_ENA bit3, OUT2R_ENA bit2, EP_SEL bit15 [MR:4258, 4293, 4297]. madera_hp_ev
[MC:2737-2778] POST_PMU sets the bit, then madera_out_ev sleeps 6 ms per CS47L92/93 output (12 ms L+R)
[MC:2646-2687]; PRE_PMD clears it, POST_PMD sleeps 1 ms per output [MC:2695-2727].
HP status: IRQ1_RAW_STATUS_12 0x188B HP2L/R_SC_STS1 bits 2/3; _13 0x188C HP2L/R_ENABLE_DONE_STS1 bits 2/3;
_14 0x188D HP2L/R_DISABLE_DONE_STS1 bits 2/3 [MR:1516-1518, 8069-8170]; readable [L92T:2198-2208].
HP clamp (CS47L92/93): HEADPHONE_DETECT_0 0x299 HPD_OVD_ENA_SEL bit15 [EXT:859-918, MR:182, 3524]; reset 0 = no
clamp [L92T:207] -> read only. Charge pump / HP offset: the driver writes nothing beyond the patch's
write-sequencer entries - the codec's sequencer runs them on OUTnx_ENA. DRE/EDRE default on (0x3F), noise gate
off (0x458 = 0) [L92T:292-296] - left as is.
**Tone frequency**: nothing in the tree states it; "1 kHz at 48 kHz" is recalled Cirrus datasheet behaviour ->
**UNVERIFIED** (harmless: any audible tone proves the path).
**Level**: mixer -20 dB (0x2C -> reg 0x58) + DAC -12 dB (0x68) = **-32 dBFS** square wave (tone is a full-scale
square wave per the same recollection - UNVERIFIED, which is why the level is ~12 dB below the asked -20 dBFS).
Mixer volume control = SOC_SINGLE_RANGE_TLV(min 0x20, max 0x50) [MH:194-197] -> field 0x20 = -32 dB, 0x40 = 0 dB.
Mixer source value "Tone Generator 1" = 0x04 [MC:1422-1424 texts, 1574-1576 values].

### 6.2 Design (milestone 2)
- Where: inside `CodecProbe` (the deferred ReadyToBoot path, guard 0x69 already set by OnCodecPower), after the
  ID read + BOOT_DONE succeeded **on a codec we took out of reset ourselves** (= fresh hard reset, all registers at
  their defaults). Then the bus is released and the reset line restored as in 1b (codec back in hard reset ->
  every register we wrote is wiped). Flag `BEYOND0LTE_AUDIO_TONE` (1 for the test build).
- Sequence (kernel order: madera-core probe -> machine-driver init/bias -> DAPM power-up -> DAPM power-down):
  1. cs47l92_patch: 38 x 16-bit + 28 x 32-bit writes, verbatim (incl. the odd 0x30D1 - regmap_register_patch
     -> _regmap_multi_reg_write -> _regmap_write does not check the stride); log the read-back mismatch count.
  2. CLOCK_32K_1 0x100: mask 0x43 <- 0x41 (ENA + MCLK2).
  3. SAMPLE_RATE_1 0x102 <- 0x0003 (48 kHz); SYSTEM_CLOCK_1 0x101 mask 0x870F <- 0x0404 (98 MHz, FLL1; = default).
  4. FLL1 <- MCLK2 (src 1): HOLD; CONTROL_2 = 0x0177; _3 THETA 0; _4 LAMBDA 1; _5 FB_DIV 4; _6 REFCLK_DIV 0;
     GAIN 0x23F0; _10 HP 0 | PHASEDET_ENA; _11 LOCKDET_THR 2; DIGITAL_TEST_1 &= ~0x103; REFCLK_SRC 1; ENA; LOCKDET;
     CTRL_UPD; HOLD off; poll FLL1_LOCK_STS1 every 1 ms up to 300 ms.
     No lock -> fllhj_disable, then FLL1 <- MCLK1 (src 0, 26 MHz): N 22 (0x16), THETA 1114 (0x45A), LAMBDA 1625
     (0x659), FB_DIV 1, REFCLK_DIV 1, GAIN 0x21F0, HP 3, LOCKDET_THR 8, DIGITAL_TEST_1 fast_clk 3 (check:
     13 MHz x (22 + 1114/1625) = 294.912 MHz = 6 x 49.152 MHz). Still no lock -> disable FLL, abort (no tone).
  5. SYSTEM_CLOCK_1 |= SYSCLK_ENA; madera_spin_sysclk (4 reads of reg 0 + 300 us).
  6. OUT2L/R mixer input 1 = 0x04 (tone 1), volume reg 0x58 (-20 dB); DAC_DIGITAL_VOLUME_2L/2R = VU | 0x68
     (-12 dB, unmuted); TONE_GENERATOR_1 = TONE1_ENA (rate 0 = SAMPLE_RATE_1, OVD 0).
  7. OUTPUT_ENABLES_1 |= OUT2L_ENA | OUT2R_ENA (0x000C); 12 ms (madera_out_ev 6 ms x 2); read back
     OUTPUT_ENABLES_1, RAW_OUTPUT_STATUS_1 0x406, IRQ1_RAW_STATUS_12/13 (SC / ENABLE_DONE), HEADPHONE_DETECT_0,
     FLL lock. HP2 short-circuit bit set -> outputs off at once (skip the hold).
  8. Hold ~2 s (Stall in 100 ms steps), FLL lock + OUTPUT_ENABLES_1 re-read at the middle.
  9. Teardown (DAPM PRE_PMD/POST_PMD order): OUTPUT_ENABLES_1 &= ~0x000C, 2 ms, read IRQ1_RAW_STATUS_14
     (DISABLE_DONE); DAC volumes back to VU | MUTE | 0x80 (default + VU); tone off; mixer sources 0; SYSCLK_ENA
     off; fllhj_disable; read back everything. Then the 1b release path puts the codec back into hard reset.
- Safety: no AP CMU/PMU/ABOX write; only SPI writes to a codec that ends in hard reset; level -32 dBFS; the amps
  (speaker/receiver) are never enabled (OUT2 feeds only the jack); if no headphones are plugged the output just
  drives the open jack.

### 6.3 Progress log (cont.) + commit reviews (AGENTS.md §4)
- Review 0b4e1b9 (research notes): re-grepped every cite. [DTBO:1481] routing, [MC:1422-1424/1574-1576] mixer
  texts/values, [MH:194-197] range TLV, [CORE:41, 905, 917] 32k MCLK2 + patch_fn, [L92:2148] FLL1 base - OK.
  FOUND: two registers.h ranges were off (HP SC/ENABLE/DISABLE status cited 8083-8106, real headers at
  8069/8103/8137; DAC volume 4446-4456 -> 4447-4457) - fixed in the follow-up commit. FLLHJ numbers re-derived
  by a python transliteration of madera_fllhj_apply (scratchpad one-liner): MCLK2 -> N 375 THETA 0 LAMBDA 1
  FB 4 gains 0x23F0 thr 2; MCLK1 -> refdiv 1 N 22 THETA 0x45A LAMBDA 0x659 FB 1 gains 0x21F0 thr 8 hp 3 fast 3.
- Build env (this session): scratchpad `build-m2.ps1` = build-private.ps1 with PACKAGES_PATH 2nd entry = this
  worktree's firmware and WORKSPACE `C:\Users\ilya\code\w8c` (Conf copied from w8b), run via Start-Process with
  redirected streams. Baseline build of a9e61b4: exit 0, FVMAIN_COMPACT 3 728 B free, FVMAIN 33%.
- Task "codec write helpers": CodecFrameAddr (addr + read flag + 2 pad bytes), CodecWrite (8-byte frame),
  CodecUpdate (read-modify-write, no write on a failed read), CodecWrite32 / CodecRead32 (10-byte frames, value
  in bytes 6..9); CodecRead made non-STATIC; prototypes in new AudioCodec.h (added to the .inf). Build exit 0,
  FVMAIN_COMPACT 3 720 B free (helpers unused yet).
- Review 7204296 (helpers): SPI_FIFO_DEPTH 64 >= the 10-byte frames; write frame has bit 7 of byte 0 clear
  (regmap-spi write_flag_mask 0); one CS window per register = regmap-spi's own framing; CodecUpdate never
  writes after a failed read. No follow-ups.
- Task "tone sequence": new `AudioTone.c` (CodecTonePlay, §6.2 steps 1-9 transliterated; every Set() logs
  "[reg] mask <- value : read X ok|MISMATCH"; LogStatus() dumps FLL lock, SYSCLK, OUT_EN1, RAW_OUT_STS1, TONE1,
  HP2 SC/EN_DONE/DIS_DONE, HPD0), flag `BEYOND0LTE_AUDIO_TONE` (1) + prototype in AudioCodec.h, AudioTone.c in
  the .inf. Call site: CodecProbe, inside the "codec in reset - releasing it" branch, only if
  `CodecWaitBoot()` (now returns BOOLEAN) and the ID matched. Build exit 0: AudioProbeDxe.efi 26 112 B (was
  ~20 KB), FD 7 340 032 B sha256 4f3f0c9b7789d2a7abae33ff9604f8cc715f8b72898090f32c23d981748192c1,
  FVMAIN_COMPACT 1 232 B free (99%, auto-sized), FVMAIN 33%. `grep -a` on the .efi finds the AudioTone strings.
- Review f6ce147 (tone sequence), adversarial:
  * Patch tables diffed by script (scratchpad m2check.py: regex-extract both tables from cs47l92-tables.c and
    AudioTone.c): 16-bit 38 == 38 entries identical, 32-bit 28 == 28 identical.
  * 48 register/field constants in AudioTone.c compared by script with registers.h: 0 mismatches; composites
    OUT2_ENA_BOTH 0xC, DT1 mask 0x103, SYSCLK set mask 0x870F, HP2 SC 0xC, HPD_OVD 0x8000 recomputed - equal.
  * Every register the sequence reads is in the cs47l92 16-bit readable list (FLL1 0x171-0x17D incl.
    CS47L92_FLL1_CONTROL_7..10 [L92T:1188-1199]), so CodecUpdate's read-modify-write on 0x17A/0x17D does not
    merge garbage into the bits it keeps.
  * FOUND: the notes said 37 x 16-bit patch writes - it is 38 (0x120 is written twice). Fixed in the follow-up.
  * FOUND: DAC volume writes verified the VU bit; cs47l92 probe "latches" VU once [L92:2024-2033, 2156-2159],
    whether VU reads back is unknown -> possible false MISMATCH in an unattended log. Follow-up: SetEx() with a
    no-verify mask (VU) for the four DAC writes.
  * FOUND: the final log line asserted "~1 kHz" - the pitch is UNVERIFIED (6.1). Reworded in the follow-up.
  * Checked, no change: tone only runs inside the "released from reset" branch (RELEASE_RESETS 0 -> never);
    hang anywhere inside is covered by the 0x69 guard set in OnCodecPower; total stall <= 2 s hold + 12 + 2 ms
    + 2 x 300 ms FLL waits + 300 ms disable wait, at TPL_NOTIFY (Stall is legal there).

- Follow-up to f6ce147 (SetEx no-verify VU, pitch wording, 38 patch writes): build exit 0, AudioProbeDxe.efi
  26 624 B, FD sha256 68fd92c3a42a95912649e57b51d62716d6e4384c6ba60c081f176fb0176ad0f1, FVMAIN_COMPACT 1 112 B free.
- Review 04b69b9 (follow-ups): SetEx's Check mask = Mask & ~NoVerify, only the four DAC writes pass DAC_VU;
  `grep -n "~1 kHz"` in AudioTone.c now only hits the UNVERIFIED wording; build exit 0. No follow-ups.

### 6.4 Device test procedure (milestone 2) - for the main session (unattended) and the user (ear test)

Image: build on main after merging this branch (normal `build-uefi.ps1 -Platform Beyond0LtePkg`), pack + flash
as every boot (only by-name/boot). This session did NOT pack or flash. Private-workspace FD of 04b69b9:
`C:\Users\ilya\code\w8c\Build\Beyond0LtePkg\DEBUG_CLANGPDB\FV\BEYOND0LTE_UEFI.fd`, sha256 68fd92c3...

**Writes in this image beyond 1b:** SPI writes to the CS47L93 only (patch, clocks, FLL1, mixers, volumes,
output enables), all undone + codec put back into hard reset before the handoff. No PMIC, CMU, PMU or ABOX write.
The amps are untouched by m2 (their 1b ID probe is unchanged). Disable: `BEYOND0LTE_AUDIO_TONE 0` in
`Drivers/Beyond0LteAudioProbeDxe/AudioCodec.h` (back to 1b behaviour).

Unattended run: boot once to Windows (or past BDS), pull `UEFILOG.TXT` from TWRP, `grep -E 'AudioTone|AudioProbe'`.
Expected lines (DEBUG %x = upper-case hex, no 0x; `<>` = learned on the device), after the 1b lines up to
`AudioProbe: codec BOOT_DONE after 5000 us (IRQ1_RAW_STATUS1 80)`:
```
AudioTone: start - tone1 -> OUT2 mixers -> HPOUT2 (3.5 mm jack) for 2000 ms at -32 dBFS (mixer -20 dB, DAC -12 dB)
AudioTone: status at reset: FLL1 lock 0 (RAW2 <>) SYSCLK1 404 OUT_EN1 0 RAW_OUT_STS1 <> TONE1 0 HP2 SC 0 EN_DONE 0 DIS_DONE <> (bits 3:2 = R:L) HPD0 0
[AudioTone: patch16 [reg] <- val read <> (Success)   - only for writes that did not read back exactly; a few are tolerated]
AudioTone: cs47l92 patch applied: 16-bit <~38>/38 read back exact, 32-bit <~28>/28
AudioTone: CLOCK_32K_1 ena + MCLK2 [100] mask 43 <- 41 : read 41 ok
AudioTone: SAMPLE_RATE_1 48k [102] mask 1F <- 3 : read 3 ok
AudioTone: SYSTEM_CLOCK_1 98M FLL1 [101] mask 870F <- 404 : read 404 ok
AudioTone: FLL1 <- MCLK2 (32 kHz) programmed: CTRL1 1001 CTRL2 <8177 or 177> THETA 0 LAMBDA 1 CTRL5 4 CTRL6 8000 GAIN 23F0 CTRL10 1000 CTRL11 5 DT1 32E8 (want N 177 TH 0 LA 1 FB 4 REFDIV 0 GAIN 23F0 HP 0 THR 2 FAST 0)
AudioTone: FLL1 <- MCLK2 (32 kHz) LOCKED after <ms> ms (IRQ1_RAW_STATUS_2 <bit8 set>)          <- KEY LINE 1
AudioTone: SYSCLK_ENA [101] mask 40 <- 40 : read 444 ok
AudioTone: OUT2LMIX in1 = tone1 [690] mask FF <- 4 : read 4 ok           (and OUT2RMIX [698])
AudioTone: OUT2LMIX in1 vol -20dB [691] mask FE <- 58 : read 58 ok      (and [699])
AudioTone: DAC2L -12dB unmute [419] mask 3FF <- 268 : read <68 or 268> ok (and DAC2R [41D])
AudioTone: TONE1_ENA (rate = SAMPLE_RATE_1) [20] mask FFFF <- 1 : read 1 ok
AudioTone: OUT2L+OUT2R ENA [400] mask C <- C : read C ok
AudioTone: status outputs on (+12 ms): FLL1 lock 1 (RAW2 <>) SYSCLK1 444 OUT_EN1 C RAW_OUT_STS1 <> TONE1 1 HP2 SC 0 EN_DONE C DIS_DONE <> ... HPD0 0   <- KEY LINE 2
AudioTone: status mid-tone: FLL1 lock 1 ... OUT_EN1 C ... TONE1 1 HP2 SC 0 EN_DONE C ...                                                        <- KEY LINE 3
AudioTone: OUT2L+OUT2R off [400] mask C <- 0 : read 0 ok
AudioTone: status outputs off (+2 ms): ... OUT_EN1 0 ... DIS_DONE C ...
AudioTone: TONE1 off / DAC2L/2R mute (default) / OUT2xMIX in1 none / vol 0dB (default) / SYSCLK off : ... ok
AudioTone: FLL1 disabled after <ms> ms: CTRL1 1004 CTRL2 177 CTRL11 4 RAW2 <bit8 clear>
AudioTone: status after teardown: FLL1 lock 0 ... SYSCLK1 404 OUT_EN1 0 ... TONE1 0 ...
AudioTone: done - tone PLAYED (listen: ~2 s, both ears; pitch UNVERIFIED, expected ~1 kHz); codec goes back into hard reset next
AudioProbe: codec OK; bus released: ... (reset back to its handoff level)
```
**Milestone 2 is proven in the log** when KEY LINES 1-3 appear (FLL lock, OUT_EN1 C with EN_DONE C and SC 0,
still so at mid-tone) and the final line says PLAYED. The ear test confirms the analogue end.

Reading other outcomes:
- `FLL1 <- MCLK2 (32 kHz) NO LOCK after 301 ms` -> MCLK2 has no clock (or not ~32 kHz); then the MCLK1 attempt:
  `FLL1 <- MCLK1 (26 MHz) LOCKED` = sboot left the AP 26 MHz clock-out running (tone still plays); `NO LOCK` on
  both + `locked on neither ... abort` -> nothing was enabled; next step = find what drives MCLK2 (PMIC 32k buffer
  enable? read-only research first) or the AP CLKOUT (xclkout clock 0x385) - a flagged AP write, separate task.
- `cs47l92 patch applied: 16-bit 0/38 ... 32-bit 0/28` + abort -> SPI writes do not stick (reads worked in 1b):
  check the write framing (bit 7 of byte 0 clear) against a read of a known RW register.
- `HP2 SC` non-zero or `HP2 short-circuit status set` -> outputs were switched off at once; record, do not retry
  louder.
- `HP clamp set` -> HPD_OVD_ENA_SEL was set at reset (not expected, default 0): record the value.
- EN_DONE 0 with OUT_EN1 C -> the HP enable sequence did not finish (charge pump / CPVDD?): record RAW_OUT_STS1.
- Hang (progress word ends 0x69, no UEFILOG) -> next boot skips the whole audio probe by itself; read
  `/proc/debug_history` from TWRP (AGENTS.md §9).

**Ear test (the user, later):** plug wired headphones / earbuds into the S10e's 3.5 mm jack **before** power-on,
keep them in your ears at a normal distance, boot this image. The tone comes at ReadyToBoot - after the
UEFI boot UI, just before the Windows Boot Manager takes over (TouchPower's LDO18M step runs there). Expect a
steady beep for ~2 s in both ears at a moderate level (-32 dBFS, a square wave if the recalled datasheet is
right, so it may sound buzzy rather than pure). Report: heard / not heard, both ears or one, rough pitch,
anything else (click at start/end). If nothing is heard but the log shows KEY LINES 1-3, the digital path is
proven and the problem is analogue (jack detect / HP stage) - say so in the result.

- Review dfe7169 (procedure): every expected line compared against its DEBUG format string in AudioTone.c
  (status / programmed / LOCKED / Set "[reg] mask <- : read" / patch summary / done); predicted register values
  re-derived from the reset defaults [L92T:145,161-172] through the exact update order in FllEnable/FllDisable
  (CTRL1 7004 -> 1001 running -> 1004 disabled; CTRL11 11 -> 5 -> 4; DT1 33E8 -> 32E8; SYSCLK1 404 -> 444).
  "NO LOCK after 301 ms" matches the 0..300 inclusive loop. No follow-ups. Branch state: 7 commits on e3f72a2,
  not pushed, nothing packed or flashed.

- 2026-10-12 02:12: audio M2 merged to main (c20e6c0), built into boot #93 (FVMAIN_COMPACT 1152 B free),
  flashed; unattended run - result from UEFILOG `AudioTone|AudioProbe` after the cycle.
