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
