# browserrefresh.ps1 [-Name msedge] - (user session, via runuser2.ps1) bring the browser's main window to the front and
# press F5 (the user's "refresh the video page" that broke pacing, notes/g18-hdmi-prep.md). -> C:\S10eTmp\refresh.txt
param([string]$Name = 'msedge')
Add-Type -Name W -Namespace S10eRf -MemberDefinition @'
[DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
'@
$p = Get-Process $Name -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { "no window for $Name" | Set-Content C:\S10eTmp\refresh.txt; exit 1 }
[S10eRf.W]::SwitchToThisWindow($p.MainWindowHandle, $true)
Start-Sleep -Milliseconds 500
[S10eRf.W]::keybd_event(0x74, 0x3F, 0, [UIntPtr]::Zero); [S10eRf.W]::keybd_event(0x74, 0x3F, 2, [UIntPtr]::Zero)
"F5 to $Name pid $($p.Id) '$($p.MainWindowTitle)' $(Get-Date -Format HH:mm:ss)" | Set-Content C:\S10eTmp\refresh.txt
