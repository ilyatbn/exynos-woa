# usbdevs.ps1 - boot time + present USB devices (status, class, name): did the USB-C hub and what hangs off it
# enumerate? (G18 / dock tests). Run over ssh.
"boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime)"
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB*' -or $_.InstanceId -like 'HID*' } |
  Sort-Object Class | Format-Table -AutoSize Status, Class, FriendlyName | Out-String -Width 160
