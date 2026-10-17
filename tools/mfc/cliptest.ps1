# cliptest.ps1 [-Dir C:\S10eTmp\mfc\clips] - G17 robustness (notes/g17-dxva-mfc.md): every .mp4 in Dir through
# mfd3d.ps1 (Media Foundation: Microsoft software decode vs our D3D11 DXVA decode on the MFC, same pipeline), one
# summary line per clip: frame counts, identical in order, and whether the DXVA path was used (decoder texture bound
# for decoding, bind 0x200). -Rows / -Cols: hash only the visible rows / columns (1080 of 1088, 854 of 864).
param([string]$Dir = 'C:\S10eTmp\mfc\clips', [int]$Rows = 0, [int]$Cols = 0)
foreach ($c in Get-ChildItem $Dir -Filter *.mp4 | Sort-Object Name) {
  $o = & C:\S10eTmp\mfc\mfd3d.ps1 -File $c.FullName -Log 0 -Rows $Rows -Cols $Cols
  $same = ($o | Select-String 'identical hashes').Line
  $tex = ($o | Select-String 'output texture').Line
  $dxva = if ($tex -match 'bind 0x200') { 'DXVA' } elseif ($tex) { 'software' } else { '?' }
  '{0,-10} {1} | {2}' -f $c.BaseName, $same, $dxva
}
