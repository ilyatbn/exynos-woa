# (user session, via run-as-user.ps1) wake the display: a zero-distance relative mouse move (SendInput path), the
# same input a touch would give; G20 scan-out dumps were black because the screen had timed out.
Add-Type -Namespace W -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, System.UIntPtr e);'
[W.U]::mouse_event(1, 1, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 100
[W.U]::mouse_event(1, -1, 0, 0, [UIntPtr]::Zero)
