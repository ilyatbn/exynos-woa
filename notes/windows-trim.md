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
- 01:15 powercfg /energy (20 s): CPU avg 21.54 %; timer resolution 1.000 ms (default 15.6) - "Outstanding Kernel Timer
  Request" period 10000, count 2; display + sleep timeouts disabled (battery and AC); 2 CPU devices code 0xA; Unknown
  Device ACPI\SAMS9821 code 0x1C; 802.11 power policy Maximum Performance (AC). Our only ExSetTimerResolution caller:
  Exynos9820WlanCx CxSetLinked (1 ms for the whole association).
- Exynos9820WlanCx 1.0.10.6: adaptive poll - 5 ms waits + 1 ms timer only while traffic is recent; after
  CX_QUIET_POLLS (40 passes ~200 ms) without D2H items or tx -> 50 ms waits and the timer request released; any tx
  (CxWake) or D2H item -> back to fast at once. Cost: first rx after quiet up to ~50 ms.
- 01:25 1.0.10.6 on device: timer stayed 1.000 ms in an on-phone detached check (4 samples, CPU 19.7-23.3 %), and from ~01:20 the phone had ~1-minute stretches with no ping/SSH while the association stayed up (no disconnect in the WlanCx log; WLAN-AutoConfig last 'disconnected by the driver' = the install). Adaptive poll suspected (rx/tx not served in slow mode); mechanism not analysed. Reverted -> 1.0.10.7 (= 1.0.10.5 code; a reinstall of 1.0.10.5 was outranked by 1.0.10.6). Open: which second component holds the 1 ms timer request (energy report count 2).
- 01:35 after 1.0.10.7: ssh 12/12. 30 s CPU: MsMpEng 4.73 (Defender's post-boot scans done), svchost[EventLog] 2.17,
  svchost[AppXSvc] 1.83, System 1.36, ShellExperienceHost 1.00, Widgets 0.68. Event rate (RecordCount diff over 60 s):
  Storage-ClassPnP/Operational 27/min (id 507 "failed non-ReadWrite SCSI SRB": CDB 42 00..00 18 00 = UNMAP, sense 5 /
  ASC 0x20 invalid opcode from the UFS LU KLUDG4U1EA), TaskScheduler 16/min, OpenSSH 12/min (= our watchall port-22
  probe), Kernel-Cache 10/min.
- trim step 2 (C:\S10eTmp\trim2.ps1, undo trim2-undo.ps1): fsutil DisableDeleteNotify NTFS 0 -> 1 (the UFS rejects
  UNMAP anyway); disabled tasks CEIP\Consolidator, OneDrive Startup Task-<user>. Access denied (protected, left alone
  - not forcing ownership): DeviceDirectoryClient\Register* (3), UpdateOrchestrator\Start Oobe Expedite Work.
