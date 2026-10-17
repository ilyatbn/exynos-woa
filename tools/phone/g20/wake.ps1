# (user session, via run-as-user.ps1) wake the display: a zero-distance relative mouse move (SendInput path), the
# same input a touch would give, then a Shift tap and ES_DISPLAY_REQUIRED (G21: the mouse move alone did not wake a
# display that had been off for a while). G20 scan-out dumps were black because the screen had timed out.
Add-Type -Namespace W -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, System.UIntPtr e);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, System.UIntPtr e);
[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
'@
[W.U]::mouse_event(1, 1, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 100
[W.U]::mouse_event(1, -1, 0, 0, [UIntPtr]::Zero)
[W.U]::keybd_event(0x10, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 50; [W.U]::keybd_event(0x10, 0, 2, [UIntPtr]::Zero)
[void][W.U]::SetThreadExecutionState(0x00000002)   # ES_DISPLAY_REQUIRED, one-shot (no ES_CONTINUOUS)
