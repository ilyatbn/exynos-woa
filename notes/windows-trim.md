# Windows trim (user, 2026-10-16): "Trim windows. Disable useless services, lower hardware usage when idle (attempt
# to). Dont go overboard though."

Context: notes/battery-driver.md - Windows idle ~2.7 W vs TWRP ~1.1 W on the same 500 mA port, CPU 18-22 % busy at
idle. Rule for this task: measure first, change only clearly useless things, one reversible step at a time, record
every change with its undo; keep everything this project needs (SSH, S10eAgent/S10eDiag, s10egov, audio, BT, Wi-Fi,
WLAN AutoConfig, networking, Windows Update decision left to the user).

## Progress log
- 2026-10-16 01:00 baseline (alpha4 tag at baa0caf first), 30 s per-process CPU on the phone (6 logical CPUs, % of
  total): MsMpEng (Defender) 15.84, lsass 2.14, svchost[CryptSvc] 1.23, sihost 1.00, System 0.90, svchost[DcomLaunch,
  Power, BrokerInfrastructure, SystemEventsBroker] 0.49, svchost[RpcSs] 0.48, S10eAgent 0.29, SearchIndexer 0.16,
  explorer 0.15, sshd 0.09. => Defender is ~3/4 of the idle CPU load.
- 01:05 Defender: AMRunningMode Normal, RealTime on, **Tamper Protection on** (not changeable from a script, by design -
  not worked around); "Windows Defender Cache Maintenance" task running at the time. Decision on real-time scanning
  left to the user (Windows Security UI).
- 01:08 trim step 1 (phone C:\S10eTmp\trim1.ps1, undo C:\S10eTmp\trim1-undo.ps1): Disabled + stopped SysMain
  (Running/Automatic), DiagTrack (Running/Automatic), WSearch (Running/Automatic), Spooler (Running/Automatic),
  PcaSvc (Running/Automatic), lfsvc (Running/Manual), MapsBroker (Stopped/Automatic). Kept: CDPSvc (BT proximal
  transport), PhoneSvc, DPS/WdiSystemHost, networking, WLAN, audio, BT, sshd, S10eAgent/S10eDiag, s10egov.
  Windows Update services were already off (UsoSvc/DoSvc Disabled, wuauserv/BITS Manual+Stopped) - untouched.
