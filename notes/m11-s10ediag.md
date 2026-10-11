# M11 - S10eDiag: in-Windows diagnostics driver + agent (2026-10-11)

User (2026-10-11, after boot #45): "screen went black again during login. after 4 minutes it actually got to the
desktop (i think the screen lit up when i pressed the volume key) ... start with a new diagnostics and debug driver
that will help you automate the process of restarting windows to twrp once it launches and collecting data from
within it ... after that start with getting touch working. then ... at least 1 more core and as much ram as possible."

## Facts this design rests on
- Boot #45 run (%TEMP%\c45b): System.evtx stops at 06:35:37, 2 min after the kernel (06:33:33) - evtx is flushed
  lazily and a forced reset loses the rest. => collection must happen INSIDE Windows (wevtutil epl) and the run must
  end with a clean restart (also leaves NTFS clean, so TWRP can mount it rw - W37 needed ntfsfix after a forced reset).
- Blank screen + volume key woke it: the display still powers off during the slow logon; a key press is a wake
  source after all (input via the generic button path is UNVERIFIED - only the user's observation).
- Windows restarts through the FADT reset register = PMU SWRESET (W32), not the EFI ResetSystem -> INFORM3 set by
  a driver before a clean `shutdown /r` survives to sboot (UNVERIFIED until the first agent run).
- Boot pair (BootLogDxe OnReadyToBoot): \WINRUN.FLG on the ESP + progress[0] != 0x80 => next UEFI boot saves WINPRAM
  and goes to TWRP. A run that already ended in TWRP must delete WINRUN.FLG or the next `adb reboot` bounces to TWRP.
- TWRP can always mount the ESP (vfat, by-name/cache); NTFS only when clean.

## Design
- S10eDiag.sys (legacy WDM, no INF, Services\S10eDiag Type 1 Start 1, test-signed like Exynos9820Ufs):
  \\.\S10eDiag (SYSTEM + Administrators). IOCTLs: read physical (MMIO mapped non-cached, 32-bit accesses, or RAM via
  MmCopyMemory), write32, recovery-on-next-reset (INFORM3 = 0x12345674 now and again from a shutdown notification),
  hard reset to recovery (INFORM3 + SWRESET), arm a hard-reset fallback timer (seconds).
- S10eAgent.exe (Win32 service, LocalSystem, auto start): mounts the ESP (mountvol S: /S), waits for explorer.exe
  (= desktop) or a timeout, runs S:\S10EDIAG\collect.cmd with OUT=S:\S10EDIAG\out, logs to out\agent.log. All policy
  (what to collect, whether to return to TWRP) lives in the ESP script - editable from TWRP without touching NTFS.
- s10ediag.exe (CLI): peek / poke / recovery / hardreset / fallback - used by the scripts and by me.

## Progress log
- 2026-10-11: sources written - drivers/S10eDiag/{S10eDiag.c (WDM driver), S10eDiagCtl.c (s10ediag.exe),
  S10eAgent.c (service), S10eDiagIoctl.h, build.bat, esp/collect.cmd}; firmware/tools/{s10e-hive-diag.ps1,
  s10e-install-diag.sh}. NOTE: S10eDiag.c and s10ediag.c collide on NTFS (case-insensitive) - the CLI source
  overwrote the driver once; CLI is S10eDiagCtl.c.
- build.bat -> %TEMP%\s10ediag: S10EDIAG_BUILD_OK; S10eDiag.sys 29592 B, s10ediag.exe 161688 B, S10eAgent.exe
  144792 B, signed with the WDK test cert 5DD5DEE1... (signtool verify on the PC: untrusted root = expected).
- collect.cmd: wevtutil epl names written out one by one (a %%~nG loop would name three logs "Operational").
  Agent logs are CREATE_ALWAYS (user: logs overwritten, not appended); %OUT%\last is wiped each run.
- First agent run (boot #45 image + S10eDiag installed, hashes matched): Windows reached the desktop (user: "desktop
  is running and looks fine" - screen stayed on: the W37 power policy works), user forced TWRP at ~10 min. ESP had
  NO S10EDIAG\out. Prefetch has MOUNTVOL.EXE-6DDF7EDE.pf at ~6 min (agent started and ran mountvol); evtx useless
  (lazy flush; NOTE the Windows clock restarts at 06:33:33 every boot - no RTC - so evtx times repeat across boots).
  ROOT CAUSE: SYSTEM\MountedDevices has \DosDevices\C: .. Z: - Windows auto-assigned letters to the Android
  partitions, S: is taken, `mountvol S: /S` fails, the agent retried for ~14 min. (Setup\SystemPartition =
  \Device\HarddiskVolume28 = CACHE, so /S itself would work with a free letter.)
  Also: System.evtx has Ntfs 55/130 (index corruption found + self-healed) after the forced resets + ntfsfix.
  Fix (next commits): hive - delete MountedDevices \DosDevices\D:..Z:, mountmgr NoAutoMount=1; agent - find the
  volume holding \S10EDIAG itself, mount it at a free letter, pass it as %ESP%; mirror agent.log to
  C:\Windows\Temp\S10eAgent.log from the first line.
- Run 2 (fixed agent, 2026-10-11 ~10:12): WORKED end to end until the restart. agent.log: ESP S: at attempt 0
  (89 s uptime), LogonUI until ~136 s, explorer at 151 s (desktop 2.5 min after boot), collect.cmd 151 -> 263 s,
  recovery armed (INFORM3 0x12345674), service control 5 (shutdown) at 293 s. Then the user saw a bugcheck
  "internal power error" (0xA0) during the restart; the UFS driver's bugcheck reset (INFORM3 normal) + the script
  having deleted WINRUN.FLG => Windows booted again; user forced TWRP. A bounce through UEFI (WINRUN.FLG of the
  second boot) dumped PRAM, but it held the second boot's marks only (no =BUGCHECK record).
  powercfg /a: NO sleep state at all (S0 Low Power Idle "firmware does not support" once CsEnabled=0) - the W37
  keys CsEnabled=0 / PlatformAoAcOverride=0 are the prime suspect for 0xA0 (Setup's restarts worked before them).
  pnp-problem: ACPI\SAMS9820\a code 28 (no driver - expected), CPUs 1-7 code 10 (numproc 1).
  Changes (commit after this): collect.cmd keeps WINRUN.FLG (crash -> boot pair -> PRAM dump -> TWRP), quick
  profile by default (full.flg for the slow inventory); agent: explorer up 30 s, 5 s poll (user request); install
  removes CsEnabled/PlatformAoAcOverride, deletes WINRUN.FLG, can stage extra.cmd + drivers.
- Run 3 (boot #46 = DSDT I2C0 sysreg window 0x10421000; agent 30 s explorer; power keys reverted; touch staged):
  back in TWRP after 345 s total. agent.log: ESP S: 92 s, explorer appeared 146 s, "up 30 s" 179 s, collect.cmd
  started; collect.log stops at "--- quick" (touch extra never ran). WINRUN.FLG consumed + WINPRAM.BIN rewritten =>
  a crash went through the boot pair. WINPRAM +0x28D: =UFSBUGCHECK BC=0x124 (WHEA_UNCORRECTABLE_ERROR)
  B1=0x11 (WHEA error source type SEA = synchronous external abort) B2=ffffb4839d9a3038 (record ptr). Early-mark
  ring full of =FAIL 1<<38 (UFS_DIAG_FAILURE_TRANSFER_LENGTH) from early boot (ring is 63 slots, no late marks).
  The same quick commands passed in run 2 => the step that aborted is unknown. Added: S10eDiag PRAM mark ring at
  WINPRAM +0xA000 (16 KiB, 64-byte lines, uptime-stamped), `s10ediag mark`, agent mirrors every log line into it,
  collect.cmd / extra-touch.cmd mark each step, driver marks load / shutdown notification / bugcheck callback.
- Run 5 (PERIC0 probe): user: "the phone is rebooting into windows after a crash. not to twrp" - the crash
  return depended on Exynos9820Ufs's bugcheck reset (INFORM3 NORMAL) + the firmware boot pair, which did not
  route to TWRP this time (cause not read yet). Fix: S10eDiag's own bugcheck callback (runs before the UFS one:
  newest registration first) records BC + P1..P4 via AuxKlibGetBugCheckData into the mark ring, then INFORM3 =
  recovery + SWRESET => sboot boots TWRP directly. The PRAM stays in RAM; read it with a firmware bounce (adb
  reboot with WINRUN.FLG present). Agent also arms the S10eDiag fallback (20 min hard reset to TWRP) unless
  stay.flg exists. My wait loop assumed crashes always return - it now must not be trusted alone.
