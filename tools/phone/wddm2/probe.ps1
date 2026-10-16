param([int]$Wddm2 = 1, [int]$Ver = 0, [int]$WddmVersion = 0x2000, [int]$Quirk = 0)
$dev = 'ACPI\SAMS9828\0'
$tr = "HKLM:\SYSTEM\CurrentControlSet\Enum\$dev\Device Parameters\MaliG76Wddm"
Remove-Item $tr -Recurse -Force -ErrorAction SilentlyContinue
$p = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters', $true)
$k = $p.CreateSubKey('Probe', [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [Microsoft.Win32.RegistryOptions]::Volatile)
$k.SetValue('Wddm2', $Wddm2, [Microsoft.Win32.RegistryValueKind]::DWord)
$k.SetValue('Ver', $Ver, [Microsoft.Win32.RegistryValueKind]::DWord)
$k.SetValue('WddmVersion', $WddmVersion, [Microsoft.Win32.RegistryValueKind]::DWord)
$k.SetValue("Quirk", $Quirk, [Microsoft.Win32.RegistryValueKind]::DWord)
$k.Close(); $p.Close()
pnputil /restart-device $dev | Out-Null
Start-Sleep 20
"probe Wddm2=$Wddm2 Ver=0x{0:x4} WddmVersion=0x{1:x4} Quirk={2}:" -f $Ver, $WddmVersion, $Quirk
pnputil /enum-devices /instanceid $dev | Select-String "Status|Problem"
$v = Get-ItemProperty $tr -ErrorAction SilentlyContinue
if ($v) { $v.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { '{0}=0x{1:x}' -f $_.Name, $_.Value } }
