# touchevt.ps1 - touch driver version on the touch device + does \BaseNamedObjects\S10eTouchActivity exist (the clock
# governor's touch-boost input, touch driver >= 6.0.5058.7).
Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.InfName -and ($_.DeviceName -like '*S6SY761*' -or $_.OriginalName -like 's6sy761*') } |
  Select-Object DeviceName, DriverVersion, InfName | Format-Table -AutoSize | Out-String -Width 160
try { $e = [System.Threading.EventWaitHandle]::OpenExisting('Global\S10eTouchActivity'); 'S10eTouchActivity: present'; $e.Close() }
catch { "S10eTouchActivity: absent ($($_.Exception.Message))" }
