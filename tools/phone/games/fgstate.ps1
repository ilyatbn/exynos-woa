# (user session, via run-as-user.ps1) fgstate.ps1 - which window has the foreground (title, class, process) and the
# game's window list with visibility/rect - is a task-started game actually in front? -> C:\S10eTmp\fgstate.out.
# notes/games-nfsu2.md.
Add-Type -Name F -Namespace S10eFg -MemberDefinition @'
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(System.IntPtr h, out int pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
'@
$h = [S10eFg.F]::GetForegroundWindow(); $fpid = 0; [void][S10eFg.F]::GetWindowThreadProcessId($h, [ref]$fpid)
$t = New-Object System.Text.StringBuilder 256; $c = New-Object System.Text.StringBuilder 256
[void][S10eFg.F]::GetWindowText($h, $t, 256); [void][S10eFg.F]::GetClassName($h, $c, 256)
$o = @("foreground $h '$t' class '$c' pid $fpid ($((Get-Process -Id $fpid -ErrorAction SilentlyContinue).ProcessName))")
foreach ($p in Get-Process SPEED2 -ErrorAction SilentlyContinue) { $o += "SPEED2 pid $($p.Id) main window $($p.MainWindowHandle) '$($p.MainWindowTitle)' responding $($p.Responding)" }
$o | Set-Content C:\S10eTmp\fgstate.out
