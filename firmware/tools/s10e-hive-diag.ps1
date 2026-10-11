param([string]$Dir)
# Registers S10eDiag.sys (kernel, system start) and S10eAgent.exe (own-process service, auto start, LocalSystem)
# in an offline SYSTEM hive pulled from the S10e. notes/m11-s10ediag.md. Run elevated (reg load).
$ErrorActionPreference = 'Stop'
& reg.exe load HKLM\S10ESYS (Join-Path $Dir 'SYSTEM') | Out-Null
try {
  $sel = (Get-ItemProperty 'HKLM:\S10ESYS\Select').Current
  $cs = 'HKLM:\S10ESYS\ControlSet{0:D3}' -f $sel
  "ControlSet = $cs"

  $k = "$cs\Services\S10eDiag"
  New-Item -Path $k -Force | Out-Null
  New-ItemProperty -Path $k -Name Type -PropertyType DWord -Value 1 -Force | Out-Null          # kernel driver
  New-ItemProperty -Path $k -Name Start -PropertyType DWord -Value 1 -Force | Out-Null         # system
  New-ItemProperty -Path $k -Name ErrorControl -PropertyType DWord -Value 0 -Force | Out-Null  # ignore failures
  New-ItemProperty -Path $k -Name ImagePath -PropertyType ExpandString -Value 'System32\drivers\S10eDiag.sys' -Force | Out-Null
  New-ItemProperty -Path $k -Name DisplayName -PropertyType String -Value 'S10e diagnostics' -Force | Out-Null

  $k = "$cs\Services\S10eAgent"
  New-Item -Path $k -Force | Out-Null
  New-ItemProperty -Path $k -Name Type -PropertyType DWord -Value 0x10 -Force | Out-Null       # own process
  New-ItemProperty -Path $k -Name Start -PropertyType DWord -Value 2 -Force | Out-Null         # auto
  New-ItemProperty -Path $k -Name ErrorControl -PropertyType DWord -Value 0 -Force | Out-Null
  New-ItemProperty -Path $k -Name ImagePath -PropertyType ExpandString -Value '%SystemRoot%\System32\S10eAgent.exe' -Force | Out-Null
  New-ItemProperty -Path $k -Name ObjectName -PropertyType String -Value 'LocalSystem' -Force | Out-Null
  New-ItemProperty -Path $k -Name DisplayName -PropertyType String -Value 'S10e diagnostics agent' -Force | Out-Null
  Get-ItemProperty "$cs\Services\S10eDiag", "$cs\Services\S10eAgent" | Format-List PSChildName, Type, Start, ImagePath

  # Agent run 1 (notes/m11-s10ediag.md): Windows had given D:..Z: to the Android partitions. Drop those
  # letters (C: stays) and stop mountmgr from lettering new volumes; the agent letters the ESP itself.
  $md = 'HKLM:\S10ESYS\MountedDevices'
  $props = (Get-ItemProperty $md).PSObject.Properties | Where-Object { $_.Name -match '^\\DosDevices\\[D-Z]:$' }
  foreach ($p in $props) { Remove-ItemProperty -Path $md -Name $p.Name }
  "removed $(@($props).Count) drive letters"
  $mm = "$cs\Services\mountmgr"
  New-ItemProperty -Path $mm -Name NoAutoMount -PropertyType DWord -Value 1 -Force | Out-Null
  "mountmgr NoAutoMount = $((Get-ItemProperty $mm).NoAutoMount)"

  # m11 run 2: shutdown /r bugchecked 0xA0 INTERNAL_POWER_ERROR after W37 turned Modern Standby off
  # (powercfg /a: no sleep state left at all). Undo those two keys; the display/sleep timeout policies stay.
  $pw = "$cs\Control\Power"
  foreach ($n in 'CsEnabled', 'PlatformAoAcOverride') {
    if ($null -ne (Get-ItemProperty $pw).$n) { Remove-ItemProperty -Path $pw -Name $n; "removed Power\$n" }
  }
}
finally {
  [gc]::Collect(); Start-Sleep -Milliseconds 500
  & reg.exe unload HKLM\S10ESYS
}
