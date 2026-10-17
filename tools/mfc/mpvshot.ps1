# mpvshot.ps1 [-File C:\S10eTmp\mfc\test.mp4] [-After 8] [-Out C:\S10eTmp\mpvshot.png] - G17 (notes/g17-dxva-mfc.md):
# start mpv (portable config, no audio, looping), take a GDI screenshot of the desktop -After seconds in, then close
# mpv. Run in the user session (tools/phone/userrun.ps1 -Exe powershell.exe ...) - one launcher run, so the screenshot
# does not collide with a second userrun/runuser2 task while mpv plays. -NoGdi: MALIKM_NO_GDI_PRESENT=1 for this mpv only
# (DIAGNOSIS, AGENTS rule 12: does DWM compose the runtime present of mpv's flip-model chain without the GDI draw?).
# -After 0 skips the screenshot and just plays for -Play seconds. -Opts: extra mpv options, spaces written as + (cmd).
param([string]$File = 'C:\S10eTmp\mfc\test.mp4', [int]$After = 8, [string]$Out = 'C:\S10eTmp\mpvshot.png', [switch]$NoGdi, [int]$Play = 30, [string]$Opts = '', [string]$SetEnv = '', [switch]$Audio)
if ($NoGdi) { $env:MALIKM_NO_GDI_PRESENT = '1' }
if ($SetEnv) { $kv = $SetEnv.Split('=', 2); [Environment]::SetEnvironmentVariable($kv[0], $kv[1]) }   # DIAGNOSIS: NAME=VALUE for this mpv only
$p = Start-Process C:\S10eTmp\mpv\mpv.exe -ArgumentList "$(if ($Audio) { "--volume=5" } else { "--no-audio" }) --loop-file=inf $($Opts.Replace('+', ' ')) $File" -PassThru
if ($After -le 0) { Start-Sleep -Seconds $Play; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; "played $Play s nogdi=$NoGdi"; return }
Start-Sleep -Seconds $After
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"$Out $($b.Width)x$($b.Height) mpv alive=$(-not $p.HasExited)"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
