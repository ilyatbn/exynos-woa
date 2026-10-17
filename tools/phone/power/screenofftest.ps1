# screenofftest.ps1 [-Seconds 40] - governor screen-off check: darken the panel with panelon (held), sample the KMD's
# PanelDisplayPower and S10ePower's granted clocks every 5 s while dark and 10 s after relight.
param([int]$Seconds = 40)
$k = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9828\0\Device Parameters\MaliG76Wddm'
Start-Process -WindowStyle Hidden C:\S10eTmp\panelon.exe -ArgumentList '0', "$Seconds"
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds + 10) {
  Start-Sleep 5
  $p = (Get-ItemProperty $k).PanelDisplayPower
  $c = (& C:\S10eTmp\s10epowerctl.exe state | Select-String 'LITTLE cur').Line
  '{0,3:n0}s panel 0x{1:X3} | {2}' -f ((Get-Date) - $t0).TotalSeconds, $p, $c
}
