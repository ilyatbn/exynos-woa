# shot.ps1 [-Out C:\S10eTmp\shot.png] - screenshot of the user session through GDI (run in the user session via
# runuser2): what Windows thinks is on screen, to compare with the scan-out dump (wddm2dbg.sh fb).
param([string]$Out = 'C:\S10eTmp\shot.png')
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"$Out $($b.Width)x$($b.Height)" | Out-File C:\S10eTmp\shot.txt
