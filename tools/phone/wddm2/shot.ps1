# shot.ps1 [-Out C:\S10eTmp\shot.png] - screenshot of the user session through GDI (run in the user session via
# runuser2): what Windows thinks is on screen, to compare with the scan-out dump (wddm2dbg.sh fb).
param([string]$Out = 'C:\S10eTmp\shot.png')
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
# DPI-aware first: otherwise at 150 % VirtualScreen is the virtualised 1013x480 and only the top-left two thirds of the
# 1520x720 screen were copied - the taskbar was cut off (G27, 2026-10-18)
Add-Type -Name Dpi -Namespace S10e -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[S10e.Dpi]::SetProcessDPIAware() | Out-Null
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"$Out $($b.Width)x$($b.Height)" | Out-File C:\S10eTmp\shot.txt
