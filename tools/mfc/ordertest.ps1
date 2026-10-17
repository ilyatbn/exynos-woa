# ordertest.ps1 [-Clip C:\S10eTmp\mfc\c1080.h264] - G17 V6b1 (notes/g17-dxva-mfc.md): decode the clip with mfcdec2 in
# display order and with -order (MFC_OPEN_DECODE_ORDER, driver 1.0.0.7+), then compare: same set of picture hashes,
# frame-type sequences of both, and display order unchanged against C:\S10eTmp\mfc\base-c1080.txt (driver 1.0.0.6).
param([string]$Clip = 'C:\S10eTmp\mfc\c1080.h264', [string]$Base = 'C:\S10eTmp\mfc\base-c1080.txt')
$exe = 'C:\S10eTmp\mfc\mfcdec2.exe'
$disp = & $exe $Clip
$ord = & $exe $Clip -order
function Frames($lines) { $lines | Where-Object { $_ -match '^frame' } }
function FHash($l) { ($l -split ' us, ')[1] }
function FType($l) { if ($l -match 'type (\d)') { $matches[1] } }
$d = Frames $disp; $o = Frames $ord
"display: $($d.Count) frames, types $((($d | ForEach-Object { FType $_ }) | Select-Object -First 16) -join '')"
"order:   $($o.Count) frames, types $((($o | ForEach-Object { FType $_ }) | Select-Object -First 16) -join '')"
$disp | Select-Object -Last 1
$ord | Select-Object -Last 1
$hs = @($d | ForEach-Object { FHash $_ } | Sort-Object); $ho = @($o | ForEach-Object { FHash $_ } | Sort-Object)
"same picture set: $((Compare-Object $hs $ho).Count -eq 0)"
if (Test-Path $Base) {
  $b = @(Frames (Get-Content $Base) | ForEach-Object { FHash $_ })
  $dd = @($d | ForEach-Object { FHash $_ })
  "display order identical to baseline: $(($b -join ',') -eq ($dd -join ','))"
}
