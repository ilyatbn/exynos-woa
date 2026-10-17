# vdecmatrix.ps1 - G17 V6a tracing (notes/g17-dxva-mfc.md): run d3dvdec once per D3D10UMD_VID_* setting (UMD tracing
# switches in Video.cpp: NBUF = buffer type count, USAGE / SIZE = GetVideoDecoderBufferInfo answers) and print the
# CreateVideoDecoder result + the UMD's video log lines. Needs C:\S10eTmp\d3dvdec.exe and a UMD with the switches.
param([string[]]$Cases = @('', 'NBUF=0', 'NBUF=1', 'USAGE=1', 'USAGE=2', 'USAGE=3', 'SIZE=65536'))
$tl = 'C:\S10eTmp\tlog'
New-Item -ItemType Directory -Force $tl | Out-Null
foreach ($c in $Cases) {
  foreach ($v in 'D3D10UMD_VID_NBUF', 'D3D10UMD_VID_USAGE', 'D3D10UMD_VID_SIZE') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
  if ($c) { $k, $val = $c -split '=', 2; Set-Item "Env:D3D10UMD_VID_$k" $val }
  $env:MALIKM_TGSI_DIR = $tl
  $out = & C:\S10eTmp\d3dvdec.exe 2>&1 | Select-String 'CreateVideoDecoder|CreateTexture2D|OutputView|PASS'
  $f = Get-ChildItem $tl -Filter 'malikm-d3dvdec*' | Where-Object Name -notlike '*stderr*' | Sort-Object LastWriteTime | Select-Object -Last 1
  "=== case '$c': $($out -join ' | ')"
  if ($f) { Get-Content $f.FullName | Select-String 'video|CalcPrivateResource|CreateResource|SetError' | ForEach-Object { '    ' + ($_.Line -replace '\s+', ' ') } }
}
