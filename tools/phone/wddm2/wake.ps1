# wake.ps1 - wake the panel for screenshots (run in the user session via runuser2): a small mouse move + display-
# required execution state. G15 test runs: presents return DXGI_STATUS_OCCLUDED while the display is off.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class S10eWake {
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
  [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
}
'@
[S10eWake]::SetThreadExecutionState([Convert]::ToUInt32("80000002", 16)) | Out-Null   # ES_CONTINUOUS | ES_DISPLAY_REQUIRED (PS 5.1 reads 0x80000002 as a negative Int32)
[S10eWake]::mouse_event(1, 5, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 300
[S10eWake]::mouse_event(1, -5, 0, 0, [UIntPtr]::Zero)
