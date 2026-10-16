# scroll.ps1 [-X 0.5] [-Y 0.6] [-Notches -5] [-NoClick] - (user session, via runuser2) move the mouse to a fraction of
# the virtual screen, left-click there (focus the page) and turn the wheel (negative = down). For reading long
# browser-internal pages (edge://gpu) by screenshots.
param([double]$X = 0.5, [double]$Y = 0.6, [int]$Notches = -5, [switch]$NoClick)
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class S10eMouse {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, int d, UIntPtr e);
}
'@
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
[S10eMouse]::SetCursorPos([int]($b.Left + $b.Width * $X), [int]($b.Top + $b.Height * $Y)) | Out-Null
Start-Sleep -Milliseconds 200
if (-not $NoClick) { [S10eMouse]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [S10eMouse]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero) }
Start-Sleep -Milliseconds 200
[S10eMouse]::mouse_event(0x800, 0, 0, 120 * $Notches, [UIntPtr]::Zero)
"scrolled $Notches at $X,$Y of $($b.Width)x$($b.Height)" | Out-File C:\S10eTmp\scroll.txt
