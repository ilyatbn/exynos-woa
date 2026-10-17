# gputest.ps1 [-From 260000] [-To 702000] [-Seconds 25] [-StopC 88] - C5 GPU part (notes/clocks-dvfs.md): for each
# G3D ladder step, request it via S10ePower, run d3dtri cube in the user session (runuser2) for -Seconds and record its
# fps summary + temperatures; stop at -StopC (DT GPU control temp 88 C), a failed request, present failures or a cube
# that does not finish. The screen saver is killed before each run (memory cube-fps-screensaver). Ends at -From.
# Needs the governor stopped. Log: C:\S10eTmp\gputest.log
param([int]$From = 260000, [int]$To = 702000, [int]$Seconds = 25, [int]$StopC = 88)
$ladder = @(156000, 200000, 260000, 325000, 377000, 433000, 572000, 650000, 702000)
$ctl = 'C:\S10eTmp\s10epowerctl.exe'
$log = 'C:\S10eTmp\gputest.log'
"gputest $From..$To kHz, $Seconds s per step, stop at $StopC C - $(Get-Date)" | Out-File $log
function Temps { $l = (& $ctl state | Select-String 'temp MID').Line
  if ($l -match 'MID (\d+) LITTLE (\d+) G3D (\d+)') { return @([int]$matches[1], [int]$matches[2], [int]$matches[3]) }; return @(255, 255, 255) }
foreach ($k in ($ladder | Where-Object { $_ -ge $From -and $_ -le $To })) {
  & $ctl set g3d $k | Out-Null
  if ($LASTEXITCODE -ne 0) { "set $k FAILED" | Tee-Object -Append $log; break }
  Get-Process scrnsave.scr -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Remove-Item C:\S10eTmp\cube.out -ErrorAction SilentlyContinue
  & C:\S10eTmp\runuser2.ps1 -Cmd "C:\S10eTmp\d3dtri.exe cube $Seconds > C:\S10eTmp\cube.out 2>&1" -Wait ($Seconds + 30) | Out-Null
  $saver = [bool](Get-Process scrnsave.scr -ErrorAction SilentlyContinue)
  $sum = (Get-Content C:\S10eTmp\cube.out -ErrorAction SilentlyContinue | Select-String '^cube:').Line
  $t = Temps
  $line = '{0} g3d {1} kHz | {2} | MID {3} LITTLE {4} G3D {5} C{6}' -f (Get-Date -Format HH:mm:ss), $k, $sum, $t[0], $t[1], $t[2], $(if ($saver) { ' | SCREENSAVER RAN - fps void' } else { '' })
  $line | Tee-Object -Append $log
  if (-not $sum) { 'STOP: cube did not finish' | Tee-Object -Append $log; break }
  if ($sum -match '(\d+) present failures' -and [int]$matches[1] -gt 0) { 'STOP: present failures' | Tee-Object -Append $log; break }
  if ($t[2] -ne 255 -and $t[2] -ge $StopC) { "STOP: G3D $($t[2]) C" | Tee-Object -Append $log; break }
}
& $ctl set g3d $From | Out-Null
"done, back at $From kHz - $(Get-Date)" | Tee-Object -Append $log
