# gpuclocktest.ps1 - does an S10ePower G3D request change the Mali's real clock? Starts d3dtri cube (60 s, user session,
# not waited for), then for each rate: request it, measure with gpuclock.exe (cycle counter / QPC, 1 s). Ends at 260 MHz.
param([int[]]$Rates = @(156000, 260000, 433000, 702000))
& C:\S10eTmp\runuser2.ps1 -Cmd "C:\S10eTmp\d3dtri.exe cube 60 > C:\S10eTmp\cube.out 2>&1" -Wait 3 | Out-Null
Start-Sleep 5
foreach ($r in $Rates) {
  & C:\S10eTmp\s10epowerctl.exe set g3d $r | Out-Null
  Start-Sleep 1
  $m = (& C:\S10eTmp\gpuclock.exe 1000 2) -join ' / '
  "requested $r kHz -> $m"
}
& C:\S10eTmp\s10epowerctl.exe set g3d 260000 | Out-Null
