# autostate.ps1 - list what could still be injecting input: powershell / cmd / our tool processes with command lines,
# and running scheduled tasks (notes/games-nfsu2.md).
Get-CimInstance Win32_Process | Where-Object { $_.Name -match 'powershell|cmd\.exe|keyplay|keyrec|screenrec|mousewiggle|S10e' } |
  ForEach-Object { "$($_.ProcessId) $($_.Name) $($_.CommandLine)" }
"running tasks:"; Get-ScheduledTask | Where-Object State -eq 'Running' | ForEach-Object { "$($_.TaskPath)$($_.TaskName)" }
