# usbstate.ps1 - is the USB-C port a host in this Windows boot? USB host controller (ACPI\PNP0D10 = XHC0), the KD
# device-mode function (UFN0), USB devices present, and whether kernel debugging is on in the boot entry.
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'PNP0D10|UFN|XHC|^USB' -or $_.Class -eq 'USB' } |
  Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String -Width 200
Get-PnpDevice | Where-Object { $_.InstanceId -match 'PNP0D10' } | Select-Object Status, Present, InstanceId | Format-Table -AutoSize | Out-String -Width 200
"--- bcdedit {current} debug settings"
bcdedit /enum '{current}' | Select-String -Pattern 'debug'
bcdedit /dbgsettings
