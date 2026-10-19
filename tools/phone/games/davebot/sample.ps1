# sample.ps1 PNG... - print the colours of davebot's candidate signature points in reference screenshots (PC side,
# System.Drawing), to choose the state signatures in davebot.ps1. notes/games-unity.md.
Add-Type -AssemblyName System.Drawing
$pts = [ordered]@{ title_logo = 560, 240; title_logo2 = 950, 200; continue = 760, 493
  load_bg1 = 300, 300; load_bg2 = 1100, 250; load_bg3 = 600, 450
  boat_coin = 195, 62; boat_coinbar = 300, 62; boat_timeskip = 1300, 224; boat_clock = 1275, 140
  dive_btn = 640, 490; dive_btn2 = 730, 490
  water_o2 = 207, 650; water_o2ring = 170, 650; water_panel = 330, 605 }
foreach ($f in $args) {
  $b = New-Object System.Drawing.Bitmap (Resolve-Path $f).Path
  $s = foreach ($k in $pts.Keys) { $p = $pts[$k]; $c = $b.GetPixel($p[0], $p[1]); '{0}={1:x2}{2:x2}{3:x2}' -f $k, $c.R, $c.G, $c.B }
  "$(Split-Path -Leaf $f): $($s -join ' ')"
  $b.Dispose()
}
