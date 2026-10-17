# devcheck.ps1 - after a firmware change: status of the devices the firmware gates (GPU, Bluetooth transport, MFC video
# decoder, USB host, Wi-Fi, audio, touch) and any device with a problem.
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'SAMS9828|SAMS9829|VIDC|MFC|PNP0D10|BTH|SAMS|Mfc' -or $_.Class -in 'Bluetooth','Net','Display','MEDIA','HIDClass' } |
  Select-Object Status, Class, FriendlyName | Sort-Object Class | Format-Table -AutoSize | Out-String -Width 160
"--- problem devices"
Get-PnpDevice -PresentOnly | Where-Object Status -ne 'OK' | Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String -Width 200
