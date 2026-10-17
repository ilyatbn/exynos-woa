# mfplayq.ps1 [-File C:\S10eTmp\mfc\c720.mp4] [-Seconds 10] [-Log 60] - G17 V6c (notes/g17-dxva-mfc.md): run
# mfplay (Media Engine frame server on the Mali) with the UMD trace log on, then print its output and the UMD's video
# log lines (decoder, video processor, stubs, errors). (The H.264 profile is exposed natively since 0091.)
param([string]$File = 'C:\S10eTmp\mfc\c720.mp4', [int]$Seconds = 10, [int]$Log = 60, [switch]$NoRead)
$opt = @('-seconds', "$Seconds"); if ($NoRead) { $opt += '-noread' }   # -NoRead: read back frame 0 only
$tl = 'C:\S10eTmp\tlog'
New-Item -ItemType Directory -Force $tl | Out-Null
Get-ChildItem Env: | Where-Object Name -like 'D3D10UMD_*' | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:MALIKM_TGSI_DIR = $tl
& C:\S10eTmp\mfc\mfplay.exe $File @opt 2>&1 | ForEach-Object { "mfplay: $_" }
"exit $LASTEXITCODE"
Remove-Item Env:MALIKM_TGSI_DIR -ErrorAction SilentlyContinue
$f = Get-ChildItem $tl -Filter 'malikm-mfplay*' | Where-Object Name -notlike '*stderr*' | Sort-Object LastWriteTime | Select-Object -Last 1
if ($f) {
  "UMD log $($f.Name):"
  Get-Content $f.FullName | Select-String 'video|SetError' | Where-Object { $_.Line -notmatch 'QueryVideoCapabilities|buffer type|buffer info' } |
    Select-Object -First $Log | ForEach-Object { '    ' + ($_.Line -replace '\s+', ' ') }
}
