# SSH into the phone's Windows (2026-10-14)

User asked why the phone cannot be controlled remotely, then: "ssh server is perfect".

## Why there was no remote control
- Probe from the PC to 192.168.137.90 (phone on the laptop hotspot), 2026-10-14 11:23: TCP 22 closed, 3389 closed,
  5985 closed, 445 open (SMB - needs an account password, file access only).
- adb exists only in TWRP/Android; S10eAgent takes requests only from C:\S10eTmp\req files and the ESP at boot (no
  network listener, by design).

## Design
- Server: PowerShell/Win32-OpenSSH **10.0.0.0p2** OpenSSH-ARM64.zip (GitHub release 2025-10-27; every release there
  is tagged "Preview"/"Beta" but not marked prerelease). Local copy
  s10e-artifacts\openssh-10.0.0.0p2\OpenSSH-ARM64.zip sha256 698c6aec31c1dd0f...; Authenticode on the PC: scp.exe,
  sftp-server.exe, sftp.exe Valid, CN=Microsoft Corporation.
- Offline install (no Windows Update / FOD needed): drivers/S10eDiag/esp/extra-ssh-install.cmd as the agent's
  extra.cmd - expand to C:\Program Files\OpenSSH, install-sshd.ps1, own sshd_config (PasswordAuthentication no,
  KbdInteractiveAuthentication no, the stock "Match Group administrators" key file), the PC key into
  C:\ProgramData\ssh\administrators_authorized_keys (ACL Administrators + SYSTEM only), firewall TCP 22 inbound from
  192.168.137.0/24 only, sshd start=auto.
- PC key: ~/.ssh/s10e_ed25519 (dedicated, no passphrase, comment "exynos-woa PC -> S10e"); public half on the ESP as
  \S10EDIAG\ssh\authorized_keys.
- Login user: a local administrator account (admins.txt in the run output lists them).

## Progress log
- 11:25 zip downloaded + verified, key generated, install script written. Waiting for TWRP to stage it with the
  boot116-gpuon-nowdt flash.
- 11:30 sshd up (port 22 open ~70 s after Windows answered ping); `ssh s10e` -> desktop-24g4oqf\s10e (admin).
- Quirks found while using it:
  - scp stops at 204800 B on this link (Transferred says 2.5 MB compressed, file 200 KiB); sftp `get /C:/path` works.
  - Admin may rename but not create files in the DriverStore folder -> SYSTEM one-shot task (C:\S10eTmp\kmdswap.cmd).
  - System32 UMD: rename the in-use DLLs, copy the new ones (new processes load them; DWM at the next boot).
  - Desktop-session programs: run-as-user.ps1 (agent version) gets killed after ~20 s when called from SSH - the
    task State reads Ready while the instance runs and Unregister-ScheduledTask kills it (0xC000013A). Use
    tools/phone/runuser2.ps1 (copied to C:\S10eTmp): cmd.exe action + a done-marker file. 20 s cube: 29.1 fps.
  - Reboot: `ssh s10e shutdown /r /t 5` -> back in Windows ~90 s later (stay.flg).

## Windows Update disabled (2026-10-14)
- Trigger: a restart during brightness work (15:45) was Windows installing the 2025-11 cumulative update KB5068865
  (staged earlier, "Installation pending" 09:10 UTC) + Store app updates (Paint, Terminal, Photos, ...). Build is now
  10.0.22631.6199 (23H2); dxgkrnl.sys and d3d10warp.dll replaced (08:40/08:42 local). After the reboot MaliG76Wddm
  (oem29.inf) still bound and Started, S10eDiag/audio/UFS/WLAN running. LogonUI showed "working on updates" while
  TiWorker finished the online phase (SSH worked throughout).
- tools/phone/wuoff.cmd, run once as SYSTEM (schtasks /ru SYSTEM, task deleted afterwards): WU policies (NoAutoUpdate,
  AUOptions 1, no auto reboot, exclude drivers, DisableWindowsUpdateAccess, no WU internet locations), driver search
  off, Store AutoDownload 2, DO download mode 0; wuauserv/UsoSvc/WaaSMedicSvc/DoSvc Start=4 (verified DISABLED with
  sc qc; no stop - servicing was running); 9 UpdateOrchestrator/WaaSMedic tasks disabled (Reboot_AC/Battery do not
  exist on this build). RebootPending / RebootRequired keys absent afterwards.
- To re-enable: delete the policy keys, set the four services back (wuauserv 3, UsoSvc 2, WaaSMedicSvc 3, DoSvc 2).
