reg delete HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters\Probe /f 2>$null | Out-Null
pnputil /restart-device "ACPI\SAMS9828\0" | Out-Null
Start-Sleep 15
"unprobe:"; pnputil /enum-devices /instanceid "ACPI\SAMS9828\0" | Select-String "Status|Problem"
