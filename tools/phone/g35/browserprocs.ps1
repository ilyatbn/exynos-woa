# browserprocs.ps1 [-Name msedge] - G35: a Chromium browser's processes: pid, --type, start time, and whether a UMD
# log exists for it in C:\S10eTmp\tlog (notes/g35-browser-video.md).
param([string]$Name = 'msedge', [switch]$Full)   # -Full: + the user-data-dir and the first 120 chars of the command line
Get-CimInstance Win32_Process -Filter "Name='$Name.exe'" | ForEach-Object {
  $t = if ($_.CommandLine -match '--type=(\S+)') { $Matches[1] } else { 'browser' }
  $u = if ($_.CommandLine -match '--utility-sub-type=(\S+)') { ' ' + $Matches[1] } else { '' }
  $log = Test-Path "C:\S10eTmp\tlog\malikm-$Name.exe-$($_.ProcessId).txt"
  '{0,6} {1,-24} {2} umdlog={3}' -f $_.ProcessId, ($t + $u), $_.CreationDate.ToString('HH:mm:ss'), $log
  if ($Full) { $d = if ($_.CommandLine -match '--user-data-dir=("[^"]+"|\S+)') { $Matches[1] } else { '(default)' }
               "         profile $d | $($_.CommandLine.Substring(0, [Math]::Min(120, $_.CommandLine.Length)))" }
}
