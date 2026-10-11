param([string]$Dir)
$ErrorActionPreference = 'Stop'
& reg.exe load HKLM\S10ESW  (Join-Path $Dir 'SOFTWARE') | Out-Null
& reg.exe load HKLM\S10ESYS (Join-Path $Dir 'SYSTEM')   | Out-Null
try {
  $sel = (Get-ItemProperty 'HKLM:\S10ESYS\Select').Current
  $cs = 'HKLM:\S10ESYS\ControlSet{0:D3}' -f $sel
  "ControlSet = $cs"

  # Power policy overrides (take precedence over the active scheme): 0 = never.
  $settings = @{
    '3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e' = 'VIDEOIDLE (turn off display)'
    '8ec4b3a5-6868-48c2-be75-4f3044be88a7' = 'VIDEOCONLOCK (display off on lock/logon screen)'
    '29f6c1db-86da-48c5-9fdb-f2b67b1f44da' = 'STANDBYIDLE (sleep)'
    '9d7815a6-7ee4-497e-8888-515a05f02364' = 'HIBERNATEIDLE'
  }
  foreach ($g in $settings.Keys) {
    $k = "HKLM:\S10ESW\Policies\Microsoft\Power\PowerSettings\$g"
    New-Item -Path $k -Force | Out-Null
    New-ItemProperty -Path $k -Name ACSettingIndex -PropertyType DWord -Value 0 -Force | Out-Null
    New-ItemProperty -Path $k -Name DCSettingIndex -PropertyType DWord -Value 0 -Force | Out-Null
    "policy $g = 0  ($($settings[$g]))"
  }

  # No lock screen (straight to the logon/desktop).
  $k = 'HKLM:\S10ESW\Policies\Microsoft\Windows\Personalization'
  New-Item -Path $k -Force | Out-Null
  New-ItemProperty -Path $k -Name NoLockScreen -PropertyType DWord -Value 1 -Force | Out-Null

  # Modern Standby off, hibernation + fast startup off.
  $p = "$cs\Control\Power"
  "before: CsEnabled=$((Get-ItemProperty $p -ErrorAction SilentlyContinue).CsEnabled) PlatformAoAcOverride=$((Get-ItemProperty $p -ErrorAction SilentlyContinue).PlatformAoAcOverride) HibernateEnabled=$((Get-ItemProperty $p -ErrorAction SilentlyContinue).HibernateEnabled)"
  # m11: NOT set any more - with Modern Standby off this platform had no sleep state at all and the next
  # restart bugchecked 0xA0 INTERNAL_POWER_ERROR. (removed: PlatformAoAcOverride)
  # m11: NOT set any more - with Modern Standby off this platform had no sleep state at all and the next
  # restart bugchecked 0xA0 INTERNAL_POWER_ERROR. (removed: CsEnabled)
  New-ItemProperty -Path $p -Name HibernateEnabled -PropertyType DWord -Value 0 -Force | Out-Null
  $sm = "$cs\Control\Session Manager\Power"
  New-Item -Path $sm -Force | Out-Null
  New-ItemProperty -Path $sm -Name HiberbootEnabled -PropertyType DWord -Value 0 -Force | Out-Null

  # A crash leaves a minidump we can read from TWRP (needs a pagefile on C:; Windows default).
  $cc = "$cs\Control\CrashControl"
  "before: CrashDumpEnabled=$((Get-ItemProperty $cc).CrashDumpEnabled) AutoReboot=$((Get-ItemProperty $cc).AutoReboot)"
  New-ItemProperty -Path $cc -Name CrashDumpEnabled -PropertyType DWord -Value 3 -Force | Out-Null
  "after: done"
}
finally {
  [gc]::Collect(); Start-Sleep -Milliseconds 500
  & reg.exe unload HKLM\S10ESW
  & reg.exe unload HKLM\S10ESYS
}
