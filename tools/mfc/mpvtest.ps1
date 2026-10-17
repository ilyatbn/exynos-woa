# mpvtest.ps1 [-File C:\S10eTmp\mfc\test.mp4] [-Seconds 15] [-Opts "..."] - G17 (notes/g17-dxva-mfc.md): play a clip in
# mpv (C:\S10eTmp\mpv) in the user's session, no audio, for -Seconds, with mpv's log file; print what decoded and
# rendered it (hwdec, vo / adapter / feature level) and the frame statistics (dropped / delayed frames, errors).
# -Opts adds mpv options (e.g. "--hwdec=d3d11va --vo=gpu --gpu-api=d3d11 --d3d11-warp=no").
param([string]$File = 'C:\S10eTmp\mfc\test.mp4', [int]$Seconds = 15, [string]$Opts = '')
$log = 'C:\S10eTmp\mpv-test.log'
Remove-Item $log -ErrorAction SilentlyContinue
$args = "--no-audio --end=$Seconds --log-file=$log --msg-level=all=v $Opts $File"   # no quotes: they break cmd quoting through runuser2 (paths without spaces)
& C:\S10eTmp\userrun.ps1 -Exe C:\S10eTmp\mpv\mpv.com -Args $args -Wait ($Seconds + 60) | Select-Object -Last 3
if (-not (Test-Path $log)) { 'no mpv log'; return }
Get-Content $log | Select-String -Pattern 'Using hardware decoding|hwdec|Using software decoding|\[vo/gpu.*(adapter|Feature level|Selected|d3d11)|VO: \[|Video \(|Dropped|delayed|drop|error|Error|failed|Failed' |
  Where-Object { $_.Line -notmatch 'Setting option|Option ' } | Select-Object -First 40 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(170, $_.Line.Length)) }
