# dwmevents.ps1 [-Minutes 30] - Desktop Window Manager / Winlogon / Application Error entries in the Application and
# System logs (DWM exits, restarts, crashes). G17 (notes/g17-dxva-mfc.md): DWM did not come back with UMD 0099.
param([int]$Minutes = 30)
foreach ($log in 'Application', 'System') {
  Get-WinEvent -FilterHashtable @{ LogName = $log; StartTime = (Get-Date).AddMinutes(-$Minutes) } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'Desktop Window Manager|Dwm|Winlogon|Application Error|Windows Error Reporting' -or $_.Message -match 'dwm\.exe' } |
    Select-Object -First 15 | ForEach-Object {
      '{0} {1:HH:mm:ss} {2,5} {3}: {4}' -f $log.Substring(0, 3), $_.TimeCreated, $_.Id, $_.ProviderName,
        ((($_.Message -split "`n") | Select-Object -First 3) -join ' ').Trim()
    }
}
