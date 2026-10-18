# stopauto.ps1 - stop every piece of the game automation (keyplay, gamedrive.ps1 hosts, screenrec, the game) and list
# what is left. Used when the user takes the phone back mid-run (notes/games-nfsu2.md).
Get-Process keyplay, keyrec, SPEED2, screenrec -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
  Where-Object { $_.CommandLine -match 'gamedrive|nfsrace|keyrec-run' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "killed $($_.ProcessId): $($_.CommandLine)" }
"left:"; Get-Process keyplay, keyrec, SPEED2, screenrec -ErrorAction SilentlyContinue | Select-Object Name, Id | Format-Table | Out-String
