# procline.ps1 [-Name msedge.exe] [-Match ''] [-NotMatch ''] - command lines of running processes (quoting through ssh
# is fragile, so this lives in a file). Diagnosis helper.
param([string]$Name = 'msedge.exe', [string]$Match = '', [string]$NotMatch = '')
Get-CimInstance Win32_Process -Filter "Name='$Name'" | Where-Object {
  ($Match -eq '' -or $_.CommandLine -match $Match) -and ($NotMatch -eq '' -or $_.CommandLine -notmatch $NotMatch)
} | ForEach-Object { "$($_.ProcessId): $($_.CommandLine)" }
