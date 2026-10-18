# (user session, via run-as-user.ps1) taskbarstate.ps1 - why is no taskbar in the screenshot? Shell_TrayWnd: exists,
# visible, its rect vs the screen; StuckRects3 byte 8 (bit 0 = auto-hide); TaskbarAl / TaskbarSi. -> C:\S10eTmp\taskbar.out
$out = 'C:\S10eTmp\taskbar.out'
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class TB {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[TB]::SetProcessDPIAware() | Out-Null
$h = [TB]::FindWindow('Shell_TrayWnd', $null)
$r = New-Object TB+RECT; [TB]::GetWindowRect($h, [ref]$r) | Out-Null
$l = @("screen $([TB]::GetSystemMetrics(0))x$([TB]::GetSystemMetrics(1))",
       "Shell_TrayWnd $h visible $([TB]::IsWindowVisible($h)) rect $($r.L),$($r.T),$($r.R),$($r.B)")
$sr = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StuckRects3' -ErrorAction SilentlyContinue).Settings
if ($sr) { $l += "StuckRects3 byte8 0x{0:x2} (bit0 = auto-hide)" -f $sr[8] } else { $l += 'StuckRects3 absent' }
$adv = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -ErrorAction SilentlyContinue
$l += "TaskbarAl $($adv.TaskbarAl) TaskbarSi $($adv.TaskbarSi)"
$l | Set-Content $out
