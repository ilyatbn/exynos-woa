# M12 - touch on Windows (S10e) (2026-10-11)

User: "start with getting touch working" (after S10eDiag). Stack: drivers/Exynos9820Hsi2c (SAMS9820, bus) +
drivers/S6SY761TouchBeyond0 (SCSY0761, HID), DSDT I2C0 / TSP0 (Dsdt.asl:250-300). Neither is in the Windows image yet
(W37: DriverStore has only exynos9820ufs).

## Live DT read from TWRP (root), 2026-10-11 (%TEMP%\c46\dt, tar of /proc/device-tree nodes)
hsi2c@104B0000: compatible samsung,exynos5-hsi2c; reg 0x104B0000+0x1000; interrupts <0 0x184 0> (SPI 388, GSIV 420);
  clocks <&clock 0x1fd> <&clock 0x1ed> names ipclk_hsi2c|gate_hsi2c_clk; default-clk 0x0bebc200 (200 MHz);
  gpio_scl <0x74 4 1>, gpio_sda <0x74 5 1>; flags samsung,usi-i2c-v2, check-transdone-int, reset-before-trans,
  scl-clk-stretching, stop-after-trans.
touchscreen@48: compatible sec,sec_ts; reg 0x48; sec,irq_gpio <0x135 0 0>; sec,irq_type 0x2008; max_coords 4096x4096;
  firmware tsp_sec/y761_beyond0_phole.bin; project BEYOND0|G970; regulator_avdd "tsp_avdd", regulator_dvdd "tsp_io",
  sec,regulator_boot_on present (=> rails expected ON from boot - UNVERIFIED on the Windows side);
  pinctrl on_state 0x31c / off_state 0x31d.
USI mode slots (samsung,exynos-usi, each reg = 4 bytes in sysreg 0x10421000): 'i2c' at 0x1042101C and 0x1042102C,
  'spi' at +0x04/+0x0C/+0x14/+0x24, all others disabled. => the 9810's SYSREG +0x1C slot exists on the 9820 as an
  i2c-mode USI at base 0x10421000 (not 0x10411000). Which of 0x1C / 0x2C belongs to hsi2c@104B0000 is NOT proven:
  next read = /proc/device-tree/__symbols__ (labels) and aliases in TWRP.
- 2026-10-11 TWRP read of /proc/device-tree/__symbols__ + aliases: hsi2c_10 = aliases/hsi2c10 = /hsi2c@104B0000;
  aliases/usi10 = /usi@1042102C (label usi_5, usi_v2_mode 'i2c', okay); usi6 = /usi@1042101C (usi_3, 'i2c');
  usi5 = /usi@10421018 (usi_2_i2c, disabled); usi9 = /usi@10421028 (usi_4_i2c, disabled).
  => DERIVED (alias index pairing hsi2c10 <-> usi10): touch-bus USI mode register = sysreg 0x10421000 + 0x2C, mode
  I2C (driver's EXYNOS_SYSREG_USI_MODE_I2C 0x4). The 9810 pair 0x10411000 + 0x1C does NOT apply. First on-device
  check: s10ediag peek 0x1042102C from Windows (extra-touch.cmd reads 0x10421000..0x54 before/after install).
- CORRECTION (same day): clock IDs decide it. LineageOS android_kernel_samsung_exynos9820 (lineage-22.2)
  include/dt-bindings/clock/exynos9820.h: CLK_PERIC0_BASE 470; hsi2c@104B0000 clocks 0x1fd = 509 =
  DOUT_CLK_PERIC0_USI03_USI (ipclk_hsi2c), 0x1ed = 493 = GATE_USI03_USI (gate_hsi2c_clk). The DT label of
  usi@1042101C is usi_3 => touch bus = USI03 (its USI, not the USI03_I2C companion), mode register = sysreg
  0x10421000 + 0x1C (the 9810 offset, new base). The aliases usiN/hsi2cN numbering is NOT the pairing (usi10 =
  usi_5). VERIFIED-by-DT: base 0x10421000 (usi@... reg), offset 0x1C, mode 'i2c'.
  => driver CMU gates are wrong: header uses USI03_I2C_IPCLKPORT_{IPCLK,PCLK} (0x2094/0x2098); the bus needs the
  USI03_USI gates. Next: cal-if offsets for USI03_USI gates/QCH.
- Run 4 (2026-10-11 ~10:57, S10eDiag mark ring): the agent reached the desktop at 140 s, quick collection ran
  clean (Get-PnpDevice alone took 105 s), then extra-touch.cmd's FIRST register read - `s10ediag peek 0x10421000
  0x54` (USI sysreg page) - aborted: mark "t: peek before" at 264317 ms, "BUGCHECK callback" at 264364 ms,
  =UFSBUGCHECK 0x124 B1=0x11 (SEA). Run 3's identical 0x124 was the same peek (its FAT log never flushed).
  => non-secure EL1 reads of the PERIC0 USI sysreg abort at Windows time. The HSI2C driver programs exactly
  that register, so it would abort the same way. Candidates: SYSREG_PERIC0 clock gated (cal-if gate 0x2058,
  QCH 0x3018), TZPC (D_TZPC_PERIC0), or the hypervisor's stage-2. Run 5 = extra-probe-peric0.cmd: one word per
  peek, cal-if CMU_PERIC0 gates/QCHs first, GPIO, then the USI03 mode word, then HSI2C; every value is marked.
- Run 5 probe (2026-10-11 ~11:05, all single-word reads SURVIVED): CMU_PERIC0 gates 0x2004/0x2014/0x2018/
  0x201c/0x2038/0x2058/0x209c/0x20a0/0x2104 = 0x00200000 (CG_VAL=1, MANUAL=0); QCH 0x3004..0x3010, 0x3018 (SYSREG),
  0x303c (USI03_USI) = 0x003f0002 (ENABLE=0, CLOCK_REQ=1 -> no HW gating) => USI03 + sysreg clocks are ON at
  Windows time. GPIO gpp1 CON 0x10430020 = 0 (pins input), USI03 mode 0x1042101C = 0 (not yet i2c), HSI2C
  0x104B0000 CTL = 0. /proc/iomem (TWRP): only usi@...04/0C/14/1C/24/2C exist in the 0x10421000 page => run 4's
  abort was reading an UNIMPLEMENTED word (0x10421000), not a clock/TZPC problem. Driver touches only +0x1C: safe.
- Run 6 (11:23, first FULLY unattended cycle: desktop 202 s, crash-reset to TWRP, back in 411 s): certutil added
  the WDK test cert to Root + TrustedPublisher; pnputil /add-driver FAILED for both packages ("file or directory
  is corrupted"). setupapi.dev.log (read from NTFS in TWRP): "Failed to create temporary directory
  'C:\Windows\TEMP\{guid}'. Error = 0x570" - C:\Windows\TEMP's index is corrupt (Ntfs 55 after the forced
  resets/ntfsfix). Package files on the ESP hash-match the PC. mshidkmdf and SpbCx services exist (stopped).
  Run 7: TMP/TEMP = C:\S10eTmp for xcopy + pnputil, hashes logged, setupapi.dev.log saved, chkdsk C: /spotfix.
