# decbench.ps1 [-File ...] [-Frames 480] [-Start 60] - G17 (notes/g17-dxva-mfc.md): raw decode throughput of mpv's
# decoders, no display (--vo=null --untimed --no-audio): exynosmfc-copy (MFC + CPU copy) vs software (hwdec=no).
# Runs from SSH (no window needed). Prints frames per second for each.
param([string]$File = 'C:\Users\S10e\Videos\bbb1080.mov', [int]$Frames = 480, [int]$Start = 60)
foreach ($hw in 'exynosmfc-copy', 'no') {
  $t = Measure-Command {
    & C:\S10eTmp\mpv\mpv.com --no-config "--hwdec=$hw" --vo=null --no-audio --untimed "--frames=$Frames" "--start=$Start" $File 2>&1 | Out-Null
  }
  '{0,-16} {1} frames in {2:N2} s = {3:N1} fps' -f $hw, $Frames, $t.TotalSeconds, ($Frames / $t.TotalSeconds)
}
