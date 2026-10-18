# (user session, via run-as-user.ps1) mousewiggle.ps1 - move the mouse in circles over the IddCx virtual monitor
# (right of the panel) for 8 s so the desktop keeps changing (G18 stream frame-rate test). SetCursorPos only.
Add-Type -Name M -Namespace S10eWig -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y); [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[void][S10eWig.M]::SetProcessDPIAware()
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 8) {
  $a = ((Get-Date) - $t0).TotalSeconds * 6
  [void][S10eWig.M]::SetCursorPos([int](1520 + 1280 + 400 * [math]::Cos($a)), [int](720 + 400 * [math]::Sin($a)))
  Start-Sleep -Milliseconds 30
}
