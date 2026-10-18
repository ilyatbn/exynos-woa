# appcrash.ps1 [MINUTES=15] - recent application crash events (Application log, event 1000) + whether SPEED2 runs.
# Used after a game crash report (notes/games-nfsu2.md).
param([int]$Minutes = 15)
Get-Process SPEED2 -ErrorAction SilentlyContinue | Select-Object Id, StartTime | Format-Table -AutoSize | Out-String
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = (Get-Date).AddMinutes(-$Minutes) } -ErrorAction SilentlyContinue |
  Select-Object -First 3 | ForEach-Object { "$($_.TimeCreated)"; $_.Message.Substring(0, [Math]::Min(600, $_.Message.Length)); '' }
