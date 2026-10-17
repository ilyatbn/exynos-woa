# wakeevents.ps1 [-Minutes 90] - G21 (notes/g21-submissions.md): why did the screen take long to come back? DWM /
# LogonUI process state plus System events from display, power and graphics providers, and Application errors.
param([int]$Minutes = 90)
$t = (Get-Date).AddMinutes(-$Minutes)
Get-Process dwm, LogonUI, KairoGames -ErrorAction SilentlyContinue |
  Select-Object Name, Id, StartTime, Responding, @{n='cpu'; e={[int]$_.CPU}} | Format-Table -AutoSize | Out-String -Width 160
$sys = Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$t} -MaxEvents 400 -ErrorAction SilentlyContinue |
  Where-Object { $_.ProviderName -match 'Display|dxg|Kernel-Power|Power-Troubleshooter|DWM|Win32k|UserModePowerService|MaliG76' -or $_.Level -le 2 }
$sys | Select-Object TimeCreated, ProviderName, Id, LevelDisplayName,
  @{n='m'; e={($_.Message -replace '\s+', ' ').Substring(0, [Math]::Min(160, ($_.Message -replace '\s+', ' ').Length))}} |
  Format-Table -AutoSize -Wrap | Out-String -Width 230
Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$t; Level=1,2} -MaxEvents 20 -ErrorAction SilentlyContinue |
  Select-Object TimeCreated, ProviderName, Id,
  @{n='m'; e={($_.Message -replace '\s+', ' ').Substring(0, [Math]::Min(160, ($_.Message -replace '\s+', ' ').Length))}} |
  Format-Table -AutoSize -Wrap | Out-String -Width 230
