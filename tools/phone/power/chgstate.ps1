# chgstate.ps1 - S10eBattery diagnostics (charger/gauge/USB-C) from Device Parameters + Win32_Battery, for a
# "does not charge" report (notes/battery-driver.md).
$k = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9826\0\Device Parameters'
$p = Get-ItemProperty $k
$p.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { '{0,-18} 0x{1:X8} {1}' -f $_.Name, $_.Value }
Get-CimInstance Win32_Battery | Select-Object EstimatedChargeRemaining, BatteryStatus | Format-List | Out-String
(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\S10eBattery' -ErrorAction SilentlyContinue).ImagePath
Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceName like '%Battery%'" | Select-Object DeviceName, DriverVersion | Format-Table | Out-String
