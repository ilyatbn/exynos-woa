# davebench-stop.ps1 - stop every running davebench.ps1 instance, keyplay and the game, and drop the frame-time flag
# (a run started twice / an aborted run). notes/games-unity.md.
Get-CimInstance Win32_Process -Filter "name='powershell.exe'" | Where-Object { $_.CommandLine -match 'davebench\.ps1' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "stopped bench pid $($_.ProcessId)" }
Stop-Process -Name keyplay, DaveTheDiver -Force -ErrorAction SilentlyContinue
Remove-Item C:\S10eTmp\frametimes.on -ErrorAction SilentlyContinue
Get-Process keyplay, DaveTheDiver -ErrorAction SilentlyContinue | ForEach-Object { "still running: $($_.Name) $($_.Id)" }
