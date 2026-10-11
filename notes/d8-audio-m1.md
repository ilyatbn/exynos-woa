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
