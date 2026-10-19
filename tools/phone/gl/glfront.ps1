# (user session, via userrun.ps1) glfront.ps1 [-Name "Stardew Valley"] - for screenshots of a GL game: minimise the
# console windows (ConsoleWindowClass; a game started from our scheduled task's cmd gets one) and bring the process's
# main window to the front. Nothing is closed. notes/gl-icd.md.
param([string]$Name = 'Stardew Valley')
Add-Type -Namespace W -Name F -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, System.IntPtr l);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int c);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint p);
'@
$p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1
$game = @()
[W.F]::EnumWindows({ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [void][W.F]::GetClassName($h, $sb, 256)
    $pid2 = 0; [void][W.F]::GetWindowThreadProcessId($h, [ref]$pid2)
    if ([W.F]::IsWindowVisible($h)) {
      if ($sb.ToString() -eq 'ConsoleWindowClass') { [void][W.F]::ShowWindow($h, 6); "minimised console window (pid $pid2)" }
      elseif ($p -and $pid2 -eq $p.Id) { $script:game += $h; "game window class $($sb.ToString())" }
    }
    $true }, [IntPtr]::Zero) | Out-Null
foreach ($h in $game) { [void][W.F]::ShowWindow($h, 9); [void][W.F]::SetForegroundWindow($h) }
"game windows: $($game.Count)"
