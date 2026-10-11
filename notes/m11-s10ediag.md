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
