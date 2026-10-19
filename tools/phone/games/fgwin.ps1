# (user session, via userrun.ps1) fgwin.ps1 - who owns the foreground: window title / class / process / elevation of the
# foreground window, plus every visible top-level window of DaveTheDiver. Written to C:\S10eTmp\fgwin.txt.
# keyplay "game NOT foreground" / InjectTouchInput error 5 diagnosis (notes/games-unity.md).
Add-Type -Namespace W -Name G -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)] public static extern int GetWindowText(System.IntPtr h, System.Text.StringBuilder s, int n);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)] public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern System.IntPtr OpenInputDesktop(uint f, bool i, uint a);
'@
$out = 'C:\S10eTmp\fgwin.txt'
$h = [W.G]::GetForegroundWindow()
$pid0 = 0; [void][W.G]::GetWindowThreadProcessId($h, [ref]$pid0)
$t = New-Object System.Text.StringBuilder 256; [void][W.G]::GetWindowText($h, $t, 256)
$c = New-Object System.Text.StringBuilder 256; [void][W.G]::GetClassName($h, $c, 256)
$p = Get-Process -Id $pid0 -ErrorAction SilentlyContinue
$lines = @("$(Get-Date -Format HH:mm:ss) foreground hwnd $h pid $pid0 $($p.Name) title '$t' class '$c'")
$lines += "input desktop open: $([W.G]::OpenInputDesktop(0, $false, 0x0100) -ne [System.IntPtr]::Zero)"
$lines += "self session $((Get-Process -Id $PID).SessionId); game: " + ((Get-Process DaveTheDiver -ErrorAction SilentlyContinue |
  ForEach-Object { "pid $($_.Id) session $($_.SessionId) main '$($_.MainWindowTitle)' $($_.MainWindowHandle)" }) -join '; ')
$lines | Set-Content $out
