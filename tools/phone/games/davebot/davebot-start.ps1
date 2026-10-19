# davebot-start.ps1 (run via userrun.ps1 in the user session) - start davebot.ps1 hidden and return at once, so the
# userrun cmd window closes before anything else happens. Any running davebot is stopped first. notes/games-unity.md.
Get-CimInstance Win32_Process -Filter "name='powershell.exe'" | Where-Object { $_.CommandLine -match 'davebot\.ps1' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Process -WindowStyle Hidden -FilePath powershell.exe -ArgumentList '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\S10eTmp\davebot.ps1'
