param([int]$Wddm2 = 3, [switch]$Reboot)
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters'
Remove-Item "$k\Probe" -Recurse -Force -ErrorAction SilentlyContinue
Set-ItemProperty $k -Name Wddm2 -Value $Wddm2 -Type DWord
"Parameters\Wddm2 = $((Get-ItemProperty $k).Wddm2)"
Remove-Item C:\S10eTmp\dumps\*.dmp -ErrorAction SilentlyContinue
if ($Reboot) { shutdown /r /t 3 /f }
