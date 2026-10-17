# stuck.ps1 - why a shutdown/restart does not finish: services in a *Pending state, top CPU processes, pending
# reboot/servicing markers, recent shutdown-related events (User32 1074, Kernel-General 13, Winlogon).
Get-Service | Where-Object { $_.Status -like '*Pending' } | Format-Table Name, Status -AutoSize | Out-String -Width 120
Get-Process | Sort-Object CPU -Descending | Select-Object -First 12 Name, Id, CPU, StartTime | Format-Table -AutoSize | Out-String -Width 140
'RebootPending (CBS): ' + (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending')
'PendingFileRename: ' + [bool](Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue)
Get-WinEvent -FilterHashtable @{LogName = 'System'; StartTime = (Get-Date).AddHours(-2)} -MaxEvents 25 -ErrorAction SilentlyContinue |
  Select-Object TimeCreated, Id, ProviderName, @{n = 'Msg'; e = { ($_.Message -split "`n")[0] }} | Format-Table -AutoSize | Out-String -Width 220
