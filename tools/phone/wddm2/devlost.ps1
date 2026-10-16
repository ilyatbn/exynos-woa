# devlost.ps1 [-Minutes 10] - signs of lost D3D devices: "binding ... dropped" (malikm: a runtime device destroyed) and
# CreateDevice counts per DWM log, DWM restarts, Display/TDR events (4101, 4097) and dxgkrnl warnings.
param([int]$Minutes = 10)
$d = [Environment]::GetEnvironmentVariable('MALIKM_TGSI_DIR', 'Machine')
Get-ChildItem $d -Filter 'malikm-dwm*.txt' | Where-Object { $_.Name -notlike '*-stderr.txt' -and $_.LastWriteTime -gt (Get-Date).AddMinutes(-$Minutes) } |
  Sort-Object LastWriteTime | ForEach-Object {
    $drop = (Select-String -Path $_.FullName -Pattern 'dropped').Count
    $cd = (Select-String -Path $_.FullName -Pattern 'CreateDevice interface').Count
    "{0}  {1:T}  CreateDevice {2}  dropped {3}" -f $_.Name, $_.LastWriteTime, $cd, $drop
  }
"dwm now: " + ((Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { "pid $($_.Id) since $($_.StartTime.ToString('T'))" }) -join ', ')
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddMinutes(-$Minutes)} -ErrorAction SilentlyContinue |
  Where-Object { $_.ProviderName -match 'Display|DxgKrnl|nvlddmkm|Dwm' -or $_.Id -in 4101, 4097 } | Select-Object -First 10 |
  ForEach-Object { "{0:T} {1} {2}: {3}" -f $_.TimeCreated, $_.ProviderName, $_.Id, (($_.Message -split "`n")[0]) }
