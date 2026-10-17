# photostest.ps1 [-Clip C:\S10eTmp\mfc\test.mp4] [-Seconds 8] - G17 V6d (notes/g17-dxva-mfc.md): play a clip in the
# Photos app (packaged, AppContainer, Media Foundation on D3D11) and show whether it decodes on the MFC. Evidence
# without tracing: the MFC runs one session device-wide, so while Photos holds the decoder a second session
# (mfcdec2) is refused (ERROR_BUSY); CPU time of Photos over the window is printed too. Takes screenshots
# (C:\S10eTmp\photos-<n>.png). Run as administrator over SSH; the app and the screenshots run in the user session.
param([string]$Clip = 'C:\S10eTmp\mfc\test.mp4', [int]$Seconds = 8)
$o = Get-CimInstance Win32_Process -Filter "name='explorer.exe'" | Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner
$videos = "C:\Users\$($o.User)\Videos"
New-Item -ItemType Directory -Force $videos | Out-Null
$file = Join-Path $videos (Split-Path $Clip -Leaf)
Copy-Item $Clip $file -Force
Get-Process Photos -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
& C:\S10eTmp\runuser2.ps1 -Cmd "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\wake.ps1" -Wait 20 | Out-Null
& C:\S10eTmp\runuser2.ps1 -Cmd "start `"`" `"ms-photos:viewer?fileName=$file`"" -Wait 15 | Out-Null
Start-Sleep 4
$p = Get-Process Photos -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { 'Photos did not start'; return }
$cpu0 = $p.TotalProcessorTime
$probe = & C:\S10eTmp\mfc\mfcdec2.exe C:\S10eTmp\mfc\c720.h264 -q 2>&1 | Select-Object -First 2
"second MFC session while Photos plays: $($probe -join ' | ')"
& C:\S10eTmp\runuser2.ps1 -Cmd "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\shot.ps1 -Out C:\S10eTmp\photos-1.png" -Wait 20 | Out-Null
Start-Sleep ([Math]::Max(1, $Seconds - 4))
& C:\S10eTmp\runuser2.ps1 -Cmd "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\shot.ps1 -Out C:\S10eTmp\photos-2.png" -Wait 20 | Out-Null
$p.Refresh()
"Photos pid $($p.Id): CPU $([int]($p.TotalProcessorTime - $cpu0).TotalMilliseconds) ms over ~$Seconds s, working set $([int]($p.WorkingSet64 / 1MB)) MB"
Get-Process Photos -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$after = & C:\S10eTmp\mfc\mfcdec2.exe C:\S10eTmp\mfc\c720.h264 -q 2>&1 | Select-Object -Last 1
"MFC after Photos closed: $after"
