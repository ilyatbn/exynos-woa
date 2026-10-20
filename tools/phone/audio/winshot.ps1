# winshot.ps1 -Process NAME [-Out C:\S10eTmp\winshot.png] - PNG of one process's main window via PrintWindow
# (PW_RENDERFULLCONTENT), so console windows of the launching task cannot cover it. Must run in the user's session
# (tools/phone/runuser2.ps1). Used to check the S10eSpeakerTrim layout (notes/d8-mic.md).
param([Parameter(Mandatory)][string]$Process, [string]$Out = 'C:\S10eTmp\winshot.png')
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
'@
$p = Get-Process $Process -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { "no window for $Process"; exit 1 }
$h = $p.MainWindowHandle
[W]::ShowWindow($h, 9) | Out-Null   # SW_RESTORE
Start-Sleep -Milliseconds 500
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$ok = [W]::PrintWindow($h, $dc, 2)
$g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"PrintWindow $ok, $($bmp.Width)x$($bmp.Height) -> $Out"
