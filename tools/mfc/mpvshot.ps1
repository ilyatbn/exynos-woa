# mpvshot.ps1 [-File C:\S10eTmp\mfc\test.mp4] [-After 8] [-Out C:\S10eTmp\mpvshot.png] - G17 (notes/g17-dxva-mfc.md):
# start mpv (portable config, no audio, looping), take a GDI screenshot of the desktop -After seconds in, then close
# mpv. Run in the user session (tools/phone/userrun.ps1 -Exe powershell.exe ...) - one launcher run, so the screenshot
# does not collide with a second userrun/runuser2 task while mpv plays.
param([string]$File = 'C:\S10eTmp\mfc\test.mp4', [int]$After = 8, [string]$Out = 'C:\S10eTmp\mpvshot.png')
$p = Start-Process C:\S10eTmp\mpv\mpv.exe -ArgumentList "--no-audio --loop-file=inf $File" -PassThru
Start-Sleep -Seconds $After
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"$Out $($b.Width)x$($b.Height) mpv alive=$(-not $p.HasExited)"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
