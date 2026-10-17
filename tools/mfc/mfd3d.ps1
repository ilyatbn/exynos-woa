# mfd3d.ps1 [-File C:\S10eTmp\mfc\c720.mp4] [-Log 60] - G17 V6b (notes/g17-dxva-mfc.md): Media Foundation decode
# with a D3D11 device manager (mftest -d3d11) on the Mali, compared with the plain system-memory decode (frame hashes),
# then the UMD's video log of the -d3d11 run. (The H.264 profile is exposed natively since 0091.)
# -Rows N / -Cols N hash only the first N rows / columns (1080 of 1088 rows, 854 of 864 columns: padding under the
# crop is not defined output).
param([string]$File = 'C:\S10eTmp\mfc\c720.mp4', [int]$Log = 60, [int]$Rows = 0, [int]$Cols = 0)
$extra = @(); if ($Rows) { $extra += @('-rows', "$Rows") }; if ($Cols) { $extra += @('-cols', "$Cols") }
$exe = 'C:\S10eTmp\mfc\mftest2.exe'
$tl = 'C:\S10eTmp\tlog'
New-Item -ItemType Directory -Force $tl | Out-Null
Get-ChildItem Env: | Where-Object Name -like 'D3D10UMD_*' | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$sw = & $exe $File @extra
$env:MALIKM_TGSI_DIR = $tl
$hw = & $exe $File -d3d11 @extra 2>&1
Remove-Item Env:MALIKM_TGSI_DIR -ErrorAction SilentlyContinue
function Frames($l) { @($l | Where-Object { "$_" -match '^frame' } | ForEach-Object { ("$_" -split 'x\d+ ')[1] }) }
$hw | Where-Object { "$_" -notmatch '^frame' } | ForEach-Object { "d3d11: $_" }
$sw | Select-Object -Last 1 | ForEach-Object { "sysmem: $_" }
$a = Frames $sw; $b = Frames $hw
"frames sysmem $($a.Count) d3d11 $($b.Count); identical hashes in order: $(($a -join ',') -eq ($b -join ','))"
$sw | Where-Object { "$_" -match '^frame' } | Select-Object -First 3 | ForEach-Object { "sysmem $_" }
$hw | Where-Object { "$_" -match '^frame' } | Select-Object -First 3 | ForEach-Object { "d3d11  $_" }
$f = Get-ChildItem $tl -Filter 'malikm-mftest2*' | Where-Object Name -notlike '*stderr*' | Sort-Object LastWriteTime | Select-Object -Last 1
if ($f) {
  "UMD log $($f.Name):"
  Get-Content $f.FullName | Select-String 'video|CalcPrivateResource|CreateResource dim|SetError' | Select-Object -First $Log |
    ForEach-Object { '    ' + ($_.Line -replace '\s+', ' ') }
}
