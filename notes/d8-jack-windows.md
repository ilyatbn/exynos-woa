# 3.5 mm jack in Windows (Exynos9820Audio) - 2026-10-16

User: "headphone jack either doesnt work at all or does not understand i plugged something in" / "it already worked
before" (= the UEFI milestone-2 boot beep, Beyond0LteAudioProbeDxe AudioTone.c; Windows never drove the codec).
User: "do both" (app gain control + jack).

## Facts carried over (all VERIFIED in firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h)
- Jack = CS47L93 HPOUT2L/R, codec on spi_2 = 0x10470000 (size 0x100), USI01 mode reg 0x10421000+0x0C = 2 (SPI),
  pins gpp0-4..7 function 2 on PERIC0 GPIO 0x10430000 (gpp0 @0x000), reset gpg1-0 (gpg1 @0x0A0, fn 1 out),
  codec IRQ gpa2-2 (ALIVE 0x15850000 + 0x040). Clock gates CMU_PERIC0 0x10400000 USI01 (UEFI reads only; sboot
  leaves them on). DCVDD LDO18M switched on by Beyond0LteTouchPowerDxe at ReadyToBoot.
- At Windows handoff UEFI has put the bus and the reset line back as found (codec held in reset).
- Proven UEFI sequence: SpiInit/SpiXfer/CodecRead/Write (Beyond0LteAudioProbeDxe.c:465-745), release reset + 2 ms,
  ID 0x6371, BOOT_DONE poll, cs47l92 patch, FLL1 from MCLK2 32 kHz -> SYSCLK 98.304 MHz, tone1 -> OUT2 (AudioTone.c).

## Decision: no DSDT change, no flash
DSDT I2C0 (touch) already owns CMU_PERIC0 / the USI sysreg page / the PERIC0 GPIO page; a second _CRS claim would
risk a resource conflict. Exynos9820Ufs maps PMU/WDT with MmMapIoSpace without resources (Exynos9820Ufs.c:1434
"performs no resource-list validation") -> the audio driver maps spi_2, the USI sysreg page and the PERIC0 GPIO page
the same way. Touch uses USI03 (sysreg +0x1C) - a different register from USI01 (+0x0C).
Hazard to check before writing gpp0 CON: which pins the touch / other drivers use in gpp0 (RMW race).

## Tasks
- J1 codec bus in Windows: map pages, SPI init (port), release reset, read ID 0x6371 + BOOT_DONE, log. Parameters\
  CodecEnable (default 0). Verify: driver log shows ID 0x6371.
- J2 tone in Windows: port AudioTone.c as Parameters\JackTone diagnostic (beep in headphones at driver start).
  Verify: user hears it in wired headphones.
- J3 data path: ABOX RDMA -> UAIF0 -> codec AIF1 -> OUT2 (research: vendor UAIF0 format/master, madera AIF1 regs,
  AIF1RX -> OUT2 mixer, HP power sequence). Verify: music in headphones.
- J4 jack detection: codec JD/HPDET status (poll first, IRQ gpa2-2 later) -> auto-route speakers <-> headphones and
  KSJACK_DESCRIPTION2. Verify: plug/unplug switches output.

## Progress log
- 16:4x plan written; J1 started.
- 16:27 **J1 verified** (1.0.1.19, Parameters\CodecEnable 1): log "codec: at entry usi01 mode 0 gpp0 CON 00000000 gpg1
  CON 10000001 DAT D6 spi CH 0 MODE 0 USI_CON 1" (= UEFI left the bus released, codec reset gpg1-0 output, DAT bit0
  low -> held in reset) then "codec: ID 6371 HW_REV 01 IRQ1_RAW_STATUS1 0080 (BOOT_DONE after 5 ms) -> CS47L92/93 OK".
  The reinstall restarted the audio device during the user's playback ("sound driver crashed"): device Started, no
  error events, audiodg alive - the player lost the endpoint. Do not reinstall during user playback without warning.
- 16:4x **J2 verified** (1.0.1.21, JackTone 2000, wired headphones plugged): log "patch 16-bit 36/38 exact, 32-bit
  28/28" (the two 16-bit misses = the 0x120 double write / self-clearing bits, as in UEFI), "FLL1 <- MCLK2 (32 kHz)
  locked after 3 ms", outputs on: SYSCLK1 0444 OUT_EN1 000C RAW_OUT1 000C TONE1 0001, no HP2 short, HPD0 0000; mid-tone
  same; teardown clean; "codec tone: PLAYED". **User: "i heard that".** JackTone set back to 0.
  Note: the second CodecInit saw the bus already in SPI mode (usi01 2, gpp0 CON 2222xxxx) - left by the previous
  driver instance; the reset pulse makes it start clean regardless.
  User expects a Headphones device in Sound settings -> J3 (data path) + J4 (detection / endpoint).
- Review of 8fcbde6 (J2): checked every constant in CodecTone.c against AudioTone.c (copied block-for-block: patch
  tables, FLL configs, register/field defines, tone levels -32 dBFS); the 2 s tone sleeps inside PrepareHardware only
  when JackTone != 0 (default 0, set back to 0 on the phone); CodecClockOn re-applies the patch on every call -
  harmless now, but J3 should apply it once per CodecInit (follow-up inside J3). CodecTone logs but ignores the
  per-write status (same as UEFI) - the read-back in CodecLogStatus is the evidence.
- 16:5x J3 research delegated (subagent -> notes/d8-jack-research.md): link format/clock domains (FLL ref 32 kHz vs
  ABOX BCLK), ABOX UAIF0 route/CTRL/CMU/pins, codec AIF1 registers, jack detection.
- 17:2x J4 jack detection (research Q4 registers). 1.0.1.22 install: the phone hung/reset seconds after (raw ~1100 s,
  Kernel-Power 41, no bugcheck/minidump, KDNET off) and PnP rolled back to oem84 (1.0.1.21); its log was overwritten.
  Cause not identified (1.0.1.23 with the same codec writes did not hang). Added: RunLog keeps the previous driver
  load's log as Exynos9820Audio-prev.txt (first flush renames), step flushes in CodecInit/Setup/JackStart.
  1.0.1.23: "jack: PLUGGED (start, IRQ1_RAW_STATUS_7 0225)" with headphones in; the periodic passive timer failed
  C00000BB (STATUS_NOT_SUPPORTED) - MS docs WDF_TIMER_CONFIG: "The time period must be zero if WdfTimerCreate sets
  the execution level to WdfExecutionLevelPassive". 1.0.1.24: one-shot timer re-armed by its callback + JackStopping
  flag; log "jack: polling every 500 ms". Waiting for the user's unplug/replug to verify transitions.
- J3 decision input (research Q6): option (b) DSP-A 4x32 on UAIF0 keeps the proven RDMA0 -> SIFS1 4x32 chain;
  normal polarity both sides (UAIF0 CTRL1 0x1BCDF7DF, codec 0x500 0x000F) or inverted both (0x1B4DF7DF / 0x008F);
  codec 0x504 0, 0x506 0x0080, 0x507/0x508 0x2020, 0x511/0x512 0/1; FLL2 {8,16,0,1,1,0,0x21F0,0,8,0} at 0x190 for a
  6.144 MHz AIF1BCLK reference. All codec values VERIFIED-by-code only.
- **J4 detection verified** (1.0.1.24): user "unplugged and plugged twice" -> log
  [1951.8] unplugged (RAW7 021A) / [1960.8] PLUGGED (0225) / [1981.4] unplugged / [1992.9] PLUGGED. Bit 0x10 of
  IRQ1_RAW_STATUS_7 clear = present, as madera_jack_present. Routing / KSJACK reporting still to do.
- J3 (1.0.1.25-1.0.1.33). ABOX master (stock option): UAIF0 configured (DIV 95, MUX 0, gates open 0x00200000 like
  UAIF1, gpb0 CON 0x00022220, QCH bit2, CTRL1 0x1B4DF7DF, CTRL0 0x01000014, ROUTE_CTRL0 0x2), gpb0-1/2 DAT toggle on
  the AP side, codec AIF1 regs read back as written, FLL2 regs read back as programmed (191 = 0x8001 ...), yet FLL2 never
  locked to AIF1BCLK; control test: FLL2 from MCLK2 locked in 5 ms -> FLL2 fine, BCLK not seen by the codec. Codec GPn_LVL
  read 0 on GPIO5-8 throughout. Moving AIF1 RX enable before the lock wait changed nothing. Stock dtbo: uaif0 cpu =
  abox_uaif_0, codec = cs47l93 dai 0 (AIF1); cs47l92 aif1 pins = GPIO5..8 (pinctrl-cs47l92.c, zero-based 4..7); single
  DBVDD1 supply; aif1 bias-bus-hold (matches CTRL_2 0xF000). Cause of the AP->codec failure still UNKNOWN.
- **Codec master works** (HpMaster 1, 1.0.1.32): madera_set_fmt CBM_CFM = 0x500 0x00AF (BCLK_MSTR 0x20 | INV | 6.144 MHz),
  0x501/0x502 LRCLK_MSTR 0x0001 (registers.h:5007/5032/5046), AIF1 RATE 0 (SYSCLK rate 1); ABOX UAIF0 slave (CTRL0
  0x01000010) clocked from its BCLK pin (MUX_UAIF0 SELECT 1, BUSY bit16; read back 0x00000001). gpb0 DAT quiet before
  the codec starts, bits 1/2 toggling after AIF1 RX enable -> codec clocks reach the AP. "hp: ON: OUTRATE 0040 MIX
  8020/8021 RXEN 0003 OUT_EN1 000C DAC 006B/006B". **User: "yeah i can hear it now."** Synchronous by construction: the
  RDMA is paced by the codec's BCLK, the codec runs on SYSCLK (FLL1 <- MCLK2). 1.0.1.33 makes HpMaster 1 the default.
- Open: route switches only at the next stream start (user: "still gotta play pause when disconnecting/reconnecting ...
  usually windows handles that") -> J4b: separate Headphones endpoint with jack state so Windows switches.
