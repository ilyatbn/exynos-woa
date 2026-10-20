# touchstate.ps1 - is the touch path up? PnP state of the touch / HID devices, whether the S6SY761 driver's
# Global\S10eTouchActivity event exists (the governor's touch boost waits on it), and the governor's touch stats.
Get-PnpDevice | Where-Object { $_.FriendlyName -match 'touch|S6SY|HID' -or $_.InstanceId -match 'S6SY|TOUCH' } |
  ForEach-Object { '{0,-8} {1,-4} {2} | {3}' -f $_.Status, $_.Problem, $_.FriendlyName, $_.InstanceId }
Add-Type -Name E -Namespace S10eTs -MemberDefinition '[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern System.IntPtr OpenEventW(uint a, bool i, string n);'
$h = [S10eTs.E]::OpenEventW(0x00100000, $false, 'Global\S10eTouchActivity')
'Global\S10eTouchActivity: ' + $(if ($h -ne [IntPtr]::Zero) { 'exists' } else { 'MISSING (error ' + [Runtime.InteropServices.Marshal]::GetLastWin32Error() + ')' })
$g = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\s10egovernor\Parameters -ErrorAction SilentlyContinue
'governor: TouchEvent {0} Touches {1} CurMid {2} CurG3d {3}' -f $g.TouchEvent, $g.Touches, $g.CurMid, $g.CurG3d
