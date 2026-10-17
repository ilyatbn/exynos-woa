# mpvbisect.ps1 - G17 / D10 T4 (notes/g17-dxva-mfc.md): mpv's frames render black on the Mali UMD. Run test.mp4 for 4 s
# per mpv render-option set (diagnosis only) and print the TEMP pixel line the UMD logs for the presented frame
# (needs a UMD with the gdi_present pixel log and machine-wide umdtrace).
param([string]$SetList = '--fbo-format=rgba8~--profile=fast~--dither-depth=no~--profile=fast --fbo-format=rgba8 --dither-depth=no --deband=no')
$Sets = ($SetList -split '~') | ForEach-Object { $_.Replace('+', ' ') }   # sets separated by ~, spaces written as + (cmd quoting)
foreach ($s in $Sets) {
  Get-ChildItem C:\S10eTmp\tlog -Filter 'malikm-mpv*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
  & C:\S10eTmp\mfc\mpvtest.ps1 -Seconds 4 -Opts $s | Out-Null
  $f = Get-ChildItem C:\S10eTmp\tlog -Filter 'malikm-mpv*' | Where-Object Name -notlike '*stderr*' | Sort-Object LastWriteTime | Select-Object -Last 1
  $px = if ($f) { (Get-Content $f.FullName | Select-String 'TEMP gdi frame 3 ').Line } else { 'no log' }
  '{0,-75} {1}' -f $s, $(if ($px) { ($px -replace '^.*pixels: ', '') } else { '(no frame 3)' })
}
