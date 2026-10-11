# Off-mode charging (user, 2026-10-12)

Symptom: charger plugged into the powered-off phone -> sboot shows its charging screen, then boots our image on top
and lands in TWRP (boot pair, fixed separately for stay.flg) / Windows.
Design: sboot hands the FDT (x0) with /chosen/bootargs "lpcharge=1" / "androidboot.mode=charger" on a charger
power-on (Android LPM). bootshim.S stores x0 at progress page 0x8FE0F020 (+ 'FDT0' 0x30544446 at +0x28).
Beyond0LteUsbHostDxe entry -> ChargerModeCheckAndRun: validate pointer (bank0, aligned) + FdtCheckHeader, log the
bootargs, look for the flag; if set: stop the dead man's WDT (INFORM3 normal, WTCON 0), PMIC bus up, battery bar on
the framebuffer from the fuel gauge SOC (refresh 30 s), blank after 10 s, power tap shows it, power held 1 s ->
EfiResetCold (sboot enters LPM only on a power-on, so a warm reset boots normally), BC_STATUS VBUSDet 0 for 2 s ->
EfiResetShutdown (PS_HOLD).
UNVERIFIED: that sboot's FDT at x0 survives until DXE (log line "no valid FDT at ..." if overwritten); the exact
flag text; the framebuffer pixel order (only white/red used); that a warm reset from LPM boots normally.
Test: shutdown Windows (power menu), plug charger -> expect the battery bar; hold power -> Windows; unplug -> off.
Every boot logs "ChargerMode: sboot FDT at ..., bootargs ...", so even a normal boot verifies the FDT path.
- boot #88 (sha 7e7e1219…) = main ff30e87: charger mode + stay.flg boot pair + audio probe DXE (0x69 guard) + WiFi
  PCIe DXE (0x68 guard). Build exit 0. FVMAIN_COMPACT 99 % full (784 B free of 0xBB000) -> FOLLOW-UP before the
  next DXE: grow the compressed FV / FD region (Beyond0LtePkg.fdf).
