# appevents.ps1 [-Minutes 3] - Application log errors/warnings (crashes: Application Error 1000, WER 1001, SideBySide)
param([int]$Minutes = 3)
Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=(Get-Date).AddMinutes(-$Minutes); Level=1,2,3} -MaxEvents 15 -ErrorAction SilentlyContinue |
  Select-Object TimeCreated, ProviderName, Id, @{n='m'; e={($_.Message -replace '\s+', ' ').Substring(0, [Math]::Min(400, ($_.Message -replace '\s+', ' ').Length))}} |
  Format-List | Out-String -Width 200
