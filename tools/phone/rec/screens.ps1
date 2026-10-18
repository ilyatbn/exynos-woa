# (user session, via run-as-user.ps1) screens.ps1 - the displays this session sees (G18 remote-monitor PoC: does the
# IddCx virtual monitor show up?): Screen.AllScreens bounds/primary + EnumDisplayDevices adapters/monitors.
# -> C:\S10eTmp\screens.out.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Name D -Namespace S10eScr -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
'@
[void][S10eScr.D]::SetProcessDPIAware()
$o = @("virtual screen $([System.Windows.Forms.SystemInformation]::VirtualScreen)")
foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $o += "screen $($s.DeviceName) $($s.Bounds) primary $($s.Primary)" }
Get-CimInstance Win32_VideoController | ForEach-Object { $o += "adapter '$($_.Name)' status $($_.Status) $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)" }
Get-PnpDevice -Class Monitor -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object { $o += "monitor '$($_.FriendlyName)' $($_.Status) $($_.InstanceId)" }
$o | Set-Content C:\S10eTmp\screens.out
