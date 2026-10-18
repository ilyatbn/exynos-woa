# (user session, via runapp.sh) idle.ps1 - seconds since the user's last keyboard/mouse/touch input
# (GetLastInputInfo) + the foreground window title, into C:\S10eTmp\idle.out. Checked before any test that takes the
# screen, so a run does not cut into the user's own use of the phone (notes/perf-threaded-context.md).
Add-Type -Namespace W -Name I -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct LII { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LII p);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(System.IntPtr h, System.Text.StringBuilder s, int n);
'@
$l = New-Object W.I+LII; $l.cbSize = 8; [void][W.I]::GetLastInputInfo([ref]$l)
$idle = [int](([Environment]::TickCount - [int]$l.dwTime) / 1000)
$sb = New-Object System.Text.StringBuilder 256; [void][W.I]::GetWindowText([W.I]::GetForegroundWindow(), $sb, 256)
"idle $idle s; foreground '$($sb.ToString())'" | Set-Content C:\S10eTmp\idle.out
