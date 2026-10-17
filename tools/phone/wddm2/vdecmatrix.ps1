# vdecmatrix.ps1 [-Cases 'K=V',...] - G17 tracing (notes/g17-dxva-mfc.md): run d3dvdec once per environment case
# (each K=V sets D3D10UMD_K=V for that run, '' = none) and print the d3dvdec result + the UMD's video log lines.
# Committed UMDs read no D3D10UMD_VIDEO_* switch (0084's VIDEO_DEV gate was removed in 0091). The V6a measurement used
# a tracing build with VID_NBUF / VID_USAGE / VID_SIZE (buffer type count / BufferInfo answers), no longer in the UMD.
# Needs C:\S10eTmp\d3dvdec.exe.
param([string[]]$Cases = @(''))
$tl = 'C:\S10eTmp\tlog'
New-Item -ItemType Directory -Force $tl | Out-Null
foreach ($c in $Cases) {
  Get-ChildItem Env: | Where-Object Name -like 'D3D10UMD_*' | ForEach-Object { Remove-Item "Env:$($_.Name)" }
  if ($c) { $k, $val = $c -split '=', 2; Set-Item "Env:D3D10UMD_$k" $val }
  $env:MALIKM_TGSI_DIR = $tl
  $out = & C:\S10eTmp\d3dvdec.exe 2>&1 | Select-String 'profiles|CreateVideoDecoder|CreateTexture2D|OutputView|PASS'
  $f = Get-ChildItem $tl -Filter 'malikm-d3dvdec*' | Where-Object Name -notlike '*stderr*' | Sort-Object LastWriteTime | Select-Object -Last 1
  "=== case '$c': exit $LASTEXITCODE | $($out -join ' | ')"
  if ($f) { Get-Content $f.FullName | Select-String 'video|CalcPrivateResource|CreateResource|SetError' | ForEach-Object { '    ' + ($_.Line -replace '\s+', ' ') } }
}
