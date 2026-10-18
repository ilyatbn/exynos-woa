# recentcrash.ps1 [-Minutes 60] - every Application Error (1000) and .NET Runtime (1026) event of the last N minutes:
# app, module, code, offset (Steam / indie game triage, notes/games-steam.md). Phone clock resets at boot.
param([int]$Minutes = 60)
"now $(Get-Date -Format 'HH:mm:ss')"
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = (Get-Date).AddMinutes(-$Minutes) } -ErrorAction SilentlyContinue |
  Where-Object { $_.Id -in 1000, 1026, 1023 } | Select-Object -First 30 | ForEach-Object {
    if ($_.Id -eq 1000) { '{0} 1000 {1} module {2} code {3} offset {4}' -f $_.TimeCreated.ToString('HH:mm:ss'), $_.Properties[0].Value, $_.Properties[3].Value, $_.Properties[6].Value, $_.Properties[7].Value }
    else { '{0} {1} {2}' -f $_.TimeCreated.ToString('HH:mm:ss'), $_.Id, ($_.Message -replace '\s+', ' ').Substring(0, [Math]::Min(400, $_.Message.Length)) } }
"processes:"; Get-Process | Where-Object { $_.ProcessName -match 'steam|stardew|dave|unity|game' } | ForEach-Object { '{0} {1} {2} MB' -f $_.ProcessName, $_.Id, [int]($_.WorkingSet64/1MB) }
