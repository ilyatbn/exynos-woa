# Phone debloat - OneDrive / Copilot / svchost (2026-10-16)

User: "completely get rid of onedrive for me. make it a script for later reflashes of clean images. no onedrive.
no copilot app. cleanup of svchost services." -> tools/phone/debloat.ps1 (elevated, idempotent, -WhatIf dry run,
log C:\S10eTmp\debloat.log).

## Before (inventory, scratchpad bloatq.ps1)
- OneDrive running (%LOCALAPPDATA%\Microsoft\OneDrive 22.012 + 26.178), HKCU Run "/background", 3 scheduled tasks,
  C:\Windows\SysWOW64\OneDriveSetup.exe.
- Appx: Microsoft.Copilot 0.4.2.0, Microsoft.MicrosoftOfficeHub (M365 Copilot) installed + provisioned.
- 74 svchost processes: SvcHostSplitThresholdInKB 3670016 (3.5 GB) < RAM 5498544 KB, so Windows ran every service
  in its own svchost.

## Run 2026-10-16 14:14 (phone clock)
- OneDrive: uninstalled (22.012 per-user setup + SysWOW64 setup ok; the 26.178 setup binary failed to start),
  process gone, Run key + tasks gone, policy DisableFileSyncNGSC 1, Explorer nav entry hidden, Default-profile
  OneDriveSetup Run entry removed. %LOCALAPPDATA%\Microsoft\OneDrive could not be deleted (FileSyncShell64.dll held by
  explorer) -> HKCU RunOnce "S10eOneDriveLeftovers" deletes it at the next sign-in. SysWOW64\OneDriveSetup.exe stays
  (system file; blocked by the policy).
- Copilot + OfficeHub removed (all users; OfficeHub deprovisioned), TurnOffWindowsCopilot machine + user, taskbar
  button off. BingSearch left (not asked).
- SvcHostSplitThresholdInKB = 8388608 (8 GB > RAM -> services share svchost hosts after reboot); 27 services
  disabled (list in the script). svchost count 74 -> 68 before the reboot; grouping takes effect at the next boot.
- Kept on purpose: audio, BT, WLAN stack, sshd, Schedule, S10eAgent, Defender, WU, XtaCache, WER, touch keyboard,
  UnistoreSvc/UserDataSvc (shell flyouts).
- To verify after the next boot: svchost count, Start menu / taskbar / Wi-Fi / BT / audio still fine.
- 14:18 found the Win32 Copilot desktop app still running (C:\Program Files (x86)\Microsoft\Copilot, 9 copilotapp.exe,
  MicrosoftCopilotElevationService) - the appx removal does not cover it. Script now runs its uninstall entry
  (copilot_setup.exe --uninstall --mscopilot --system-level + --force-uninstall), then deletes the service, folder and
  Copilot tasks. Verified: no uninstall entry, folder, service or copilotapp process left.
- Scheduled-task question (user: tasks 30-45 min after boot?): delayed boot/logon tasks are Autochk\Proxy (+30 min),
  CertificateServicesClient\UserTask (+25 min, repeat 60), Work Folders Maintenance (+30 min logon) - none at the
  ~47 min freeze. Event-log windows per boot are unusable: the phone clock restarts at 10-15 11:47:32 every boot (no
  RTC) and jumps later, so all boots overlap. The freeze recorder now prints newly started processes per sample
  ("NEW ...") to catch a task launching right before the hang.
