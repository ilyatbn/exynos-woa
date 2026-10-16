# debloat.ps1 - one-shot cleanup for a fresh S10e Windows image (user request 2026-10-16):
#   "completely get rid of onedrive ... no onedrive. no copilot app. cleanup of svchost services."
# Run elevated (ssh session as the admin user, or "Run as administrator"). Idempotent; re-run after every clean reflash.
# Per-user parts apply to the running user (HKCU) AND to the Default profile, so new users start clean too.
# Log: C:\S10eTmp\debloat.log (or $env:TEMP if C:\S10eTmp is missing).
#
# What it does
#   1. OneDrive: stop, uninstall (per-user + system setup), remove leftovers, Run keys, scheduled tasks, Explorer nav
#      entry, and block it by policy (DisableFileSyncNGSC) + remove the Default-user "OneDriveSetup" Run entry.
#   2. Copilot: remove Microsoft.Copilot and Microsoft 365 Copilot (MicrosoftOfficeHub) for all users + provisioned,
#      policy TurnOffWindowsCopilot (machine + user), hide the taskbar button.
#   3. svchost: SvcHostSplitThresholdInKB above the installed RAM so services share svchost processes again
#      (Windows splits every service into its own process when RAM > threshold; 5.5 GB phone had 74 svchosts),
#      and disable services this device never uses (list below). Takes effect after a reboot.
# Kept on purpose: audio, Bluetooth (bthserv/BTAGService/BthAvctpSvc), WLAN (WlanSvc, Wcmsvc, Dhcp, Dnscache, nsi),
# sshd, Schedule, S10eAgent, Defender, Windows Update, XtaCache (x64 emulation cache), WER (we use LocalDumps),
# TextInputManagementService (touch keyboard), Themes, Power, Audiosrv/AudioEndpointBuilder.
param([switch]$WhatIf)
$ErrorActionPreference = 'Continue'
$log = if (Test-Path C:\S10eTmp) { 'C:\S10eTmp\debloat.log' } else { Join-Path $env:TEMP 'debloat.log' }
function L($m) { $l = "{0:HH:mm:ss} {1}" -f (Get-Date), $m; $l; Add-Content -Path $log -Value $l }
function Do-It($desc, [scriptblock]$sb) { if ($WhatIf) { L "WHATIF $desc" } else { try { & $sb; L "ok     $desc" } catch { L "FAIL   $desc : $($_.Exception.Message)" } } }
function RegSet($path, $name, $value, $type = 'DWord') {
  Do-It "$path\$name = $value" { if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }; New-ItemProperty -Path $path -Name $name -Value $value -PropertyType $type -Force | Out-Null }
}
L "=== debloat start (WhatIf=$WhatIf)"

# ---------------------------------------------------------------- 1. OneDrive
Do-It 'stop OneDrive' { Get-Process OneDrive, OneDrive.App, OneDriveStandaloneUpdater -ErrorAction SilentlyContinue | Stop-Process -Force }
$setups = @("$env:LOCALAPPDATA\Microsoft\OneDrive\*\OneDriveSetup.exe", "$env:LOCALAPPDATA\Microsoft\OneDrive\OneDriveSetup.exe",
            'C:\Windows\System32\OneDriveSetup.exe', 'C:\Windows\SysWOW64\OneDriveSetup.exe') |
          ForEach-Object { Get-ChildItem $_ -ErrorAction SilentlyContinue } | Select-Object -ExpandProperty FullName -Unique
foreach ($s in $setups) {
  Do-It "uninstall via $s" { $p = Start-Process -FilePath $s -ArgumentList '/uninstall', '/allusers' -PassThru -WindowStyle Hidden; if (-not $p.WaitForExit(120000)) { $p.Kill() } }
  Do-It "uninstall (per-user) via $s" { $p = Start-Process -FilePath $s -ArgumentList '/uninstall' -PassThru -WindowStyle Hidden; if (-not $p.WaitForExit(120000)) { $p.Kill() } }
}
Do-It 'stop OneDrive (again)' { Get-Process OneDrive* -ErrorAction SilentlyContinue | Stop-Process -Force }
foreach ($d in "$env:LOCALAPPDATA\Microsoft\OneDrive", "$env:PROGRAMDATA\Microsoft OneDrive", "$env:USERPROFILE\OneDrive",
               'C:\OneDriveTemp', 'C:\Program Files\Microsoft OneDrive') {
  # never delete a OneDrive folder that still holds user files
  if ((Test-Path $d) -and ($d -notlike "$env:USERPROFILE\OneDrive" -or -not (Get-ChildItem $d -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1))) {
    Do-It "remove $d" { Remove-Item $d -Recurse -Force -ErrorAction Stop }
  }
}
# Explorer keeps FileSyncShell64.dll loaded until the next sign-in -> delete the leftover folder at the next logon
if (Test-Path "$env:LOCALAPPDATA\Microsoft\OneDrive") {
  RegSet 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce' 'S10eOneDriveLeftovers' "cmd.exe /c rd /s /q `"$env:LOCALAPPDATA\Microsoft\OneDrive`"" 'String'
}
foreach ($k in 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run', 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run',
               'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Run') {
  foreach ($n in 'OneDrive', 'OneDriveSetup') { if (Get-ItemProperty $k -Name $n -ErrorAction SilentlyContinue) { Do-It "remove $k\$n" { Remove-ItemProperty $k -Name $n -Force } } }
}
Get-ScheduledTask -TaskName 'OneDrive*' -ErrorAction SilentlyContinue | ForEach-Object { $t = $_; Do-It "remove task $($t.TaskName)" { Unregister-ScheduledTask -TaskName $t.TaskName -TaskPath $t.TaskPath -Confirm:$false } }
RegSet 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\OneDrive' 'DisableFileSyncNGSC' 1
RegSet 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\OneDrive' 'DisableLibrariesDefaultSaveToOneDrive' 1
# Explorer navigation pane entry ({018D5C66-...} = OneDrive)
RegSet 'HKCU:\Software\Classes\CLSID\{018D5C66-4533-4307-9B53-224DE2ED1FE6}' 'System.IsPinnedToNameSpaceTree' 0
RegSet 'HKCU:\Software\Classes\WOW6432Node\CLSID\{018D5C66-4533-4307-9B53-224DE2ED1FE6}' 'System.IsPinnedToNameSpaceTree' 0
# Default profile: new users must not get the OneDriveSetup Run entry
$hive = 'C:\Users\Default\NTUSER.DAT'
if (Test-Path $hive) {
  Do-It 'Default profile: drop OneDriveSetup Run entry' {
    & reg.exe load HKU\S10eDefault $hive | Out-Null
    try { & reg.exe delete 'HKU\S10eDefault\Software\Microsoft\Windows\CurrentVersion\Run' /v OneDriveSetup /f 2>$null | Out-Null }
    finally { [gc]::Collect(); Start-Sleep 1; & reg.exe unload HKU\S10eDefault | Out-Null }
  }
}

# ---------------------------------------------------------------- 2. Copilot
foreach ($name in 'Microsoft.Copilot', 'Microsoft.MicrosoftOfficeHub', 'Microsoft.Windows.Ai.Copilot.Provider') {
  Get-AppxPackage -AllUsers -Name $name -ErrorAction SilentlyContinue | ForEach-Object { $p = $_; Do-It "remove appx $($p.PackageFullName)" { Remove-AppxPackage -Package $p.PackageFullName -AllUsers -ErrorAction Stop } }
  Get-AppxProvisionedPackage -Online | Where-Object DisplayName -eq $name | ForEach-Object { $p = $_; Do-It "deprovision $($p.PackageName)" { Remove-AppxProvisionedPackage -Online -PackageName $p.PackageName -ErrorAction Stop | Out-Null } }
}
RegSet 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsCopilot' 'TurnOffWindowsCopilot' 1
RegSet 'HKCU:\Software\Policies\Microsoft\Windows\WindowsCopilot' 'TurnOffWindowsCopilot' 1
RegSet 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' 'ShowCopilotButton' 0

# ---------------------------------------------------------------- 3. svchost
$ramKB = [int64]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1KB)
$thr = [int64]([math]::Max($ramKB + 1048576, 8388608))   # >= RAM + 1 GB, at least 8 GB
RegSet 'HKLM:\SYSTEM\CurrentControlSet\Control' 'SvcHostSplitThresholdInKB' $thr
$disable = @(
  'DiagTrack', 'dmwappushservice',                     # telemetry
  'MapsBroker', 'lfsvc', 'RetailDemo', 'wisvc',        # offline maps, geolocation, demo mode, Insider
  'WSearch', 'SysMain',                                # indexer, superfetch (CPU/disk churn on a phone SoC)
  'PhoneSvc', 'OneSyncSvc', 'MessagingService', 'PimIndexMaintenanceSvc',   # phone/mail sync (UnistoreSvc/UserDataSvc kept: shell flyouts use them)
  'Spooler', 'PrintNotify', 'Fax',                     # printing
  'lmhosts', 'SSDPSRV', 'upnphost', 'iphlpsvc', 'RemoteRegistry', 'LanmanServer',   # LAN discovery / sharing / IPv6 transition
  'XblAuthManager', 'XblGameSave', 'XboxNetApiSvc', 'XboxGipSvc', 'WMPNetworkSvc'   # Xbox, media sharing
)
foreach ($s in $disable) {
  $svc = Get-Service -Name $s -ErrorAction SilentlyContinue
  if ($svc) {
    Do-It "disable service $s" {
      Stop-Service -Name $s -Force -ErrorAction SilentlyContinue
      # registry Start = 4 (disabled) also covers the per-user template services (OneSyncSvc etc.)
      Set-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$s" -Name Start -Value 4 -ErrorAction Stop
    }
  }
}
L "=== debloat done - reboot to apply the svchost grouping and service changes"
