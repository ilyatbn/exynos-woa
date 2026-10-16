# swapmatrix.ps1 [-Frames 6] - run d3dswap (drivers/MaliG76Wddm/tools/d3dswap) in the user session for each swap chain
# kind and print how far it got: which configurations die on the NULL DXGI tracked fence (G15 S4).
param([int]$Frames = 6, [string]$Cfg)
$cfgs = if ($Cfg) { @($Cfg) } else { @('comp seq 2', 'comp seq 3', 'comp disc 2', 'hwnd seq 2', 'hwnd disc 2', 'hwnd disc 3', 'comp seq 2 warp') }
foreach ($c in $cfgs) {
  Remove-Item C:\S10eTmp\d3dswap.log -ErrorAction SilentlyContinue
  & C:\S10eTmp\runuser2.ps1 -Cmd "C:\S10eTmp\d3dswap.exe $Frames $c > C:\S10eTmp\d3dswap.log 2>&1" -Wait 40 | Out-Null
  $log = Get-Content C:\S10eTmp\d3dswap.log -ErrorAction SilentlyContinue | Where-Object { $_ -notlike 'malikm*' }
  $last = ($log | Select-Object -Last 1)
  $pres = ($log | Where-Object { $_ -like 'frame*' } | Select-Object -First 1)
  "{0,-12} last: {1}   first: {2}" -f $c, $last, $pres
}
