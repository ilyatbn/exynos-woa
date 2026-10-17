# clocktest.ps1 -Domain little|mid|g3d [-From kHz] [-To kHz] [-Seconds 60] [-StopC 83] - C5 (notes/clocks-dvfs.md):
# step one domain up its ladder under sustained load, logging temperatures; stop at the first problem. Needs
# S10ePower with AllowSend=1 + AllowTmu=1 (temperatures), and a governor that is STOPPED (sc stop S10eGovernor) so it
# does not fight the test. Each step: request the rate, run the load for -Seconds, sample s10epowerctl every 5 s,
# measure the effective CPU speed with s10eperf cpu (dependent-op chain ~ GHz). Stops when a temperature reaches
# -StopC, a request fails, or the rate is not granted. Always ends by requesting -From again.
# Output: C:\S10eTmp\clocktest-<domain>.log (one line per sample).
param([Parameter(Mandatory)][ValidateSet('little', 'mid', 'g3d')][string]$Domain, [int]$From = 0, [int]$To = 0,
      [int]$Seconds = 60, [int]$StopC = 83)
$ladders = @{
  little = @(442000, 546000, 650000, 806000, 949000, 1053000, 1157000, 1300000, 1456000, 1586000, 1742000, 1846000, 1950000)
  mid    = @(507000, 650000, 754000, 845000, 962000, 1066000, 1222000, 1404000, 1508000, 1586000, 1690000, 1794000, 1898000, 2002000, 2106000, 2210000, 2314000)
  g3d    = @(156000, 200000, 260000, 325000, 377000, 433000, 572000, 650000, 702000)
}
$boot = @{ little = 1456000; mid = 1794000; g3d = 260000 }
if ($From -eq 0) { $From = $boot[$Domain] }
if ($To -eq 0) { $To = $ladders[$Domain][-1] }
$ctl = 'C:\S10eTmp\s10epowerctl.exe'
$log = "C:\S10eTmp\clocktest-$Domain.log"
"clocktest $Domain $From..$To kHz, $Seconds s per step, stop at $StopC C - $(Get-Date)" | Out-File $log

function Temps { $s = & $ctl state; $l = ($s | Select-String 'temp MID').Line
  if ($l -match 'MID (\d+) LITTLE (\d+) G3D (\d+)') { return @([int]$matches[1], [int]$matches[2], [int]$matches[3]) }; return @(255, 255, 255) }
function Load([int]$sec) {
  # CPU load pinned to the domain's cores (LITTLE cpu0-3, MID cpu4-5); GPU: the cube/gfx load is not scripted yet
  $mask = if ($Domain -eq 'little') { 0x0F } else { 0x30 }
  $n = if ($Domain -eq 'little') { 4 } else { 2 }
  $jobs = 1..$n | ForEach-Object {
    Start-Job -ScriptBlock { param($s, $m) [System.Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity = [IntPtr]$m
      $end = (Get-Date).AddSeconds($s); $x = 0; while ((Get-Date) -lt $end) { for ($i = 0; $i -lt 100000; $i++) { $x = $x -bxor $i } } } -ArgumentList $sec, $mask }
  return $jobs
}

$steps = $ladders[$Domain] | Where-Object { $_ -ge $From -and $_ -le $To }
$ok = $true
foreach ($k in $steps) {
  $r = & $ctl set $Domain $k
  if ($LASTEXITCODE -ne 0) { "set $k FAILED: $r" | Tee-Object -Append $log; $ok = $false; break }
  $jobs = Load $Seconds
  $t0 = Get-Date
  while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep 5
    $t = Temps
    $line = '{0} {1} {2} kHz  MID {3} C  LITTLE {4} C  G3D {5} C' -f (Get-Date -Format HH:mm:ss), $Domain, $k, $t[0], $t[1], $t[2]
    $line | Tee-Object -Append $log
    if (($t | Measure-Object -Maximum).Maximum -ge $StopC -and ($t | Measure-Object -Maximum).Maximum -ne 255) {
      "STOP: $StopC C reached at $k kHz" | Tee-Object -Append $log; $ok = $false; break }
  }
  $jobs | Stop-Job -PassThru | Remove-Job
  if (-not $ok) { break }
  if ($Domain -ne 'g3d') { (& C:\S10eTmp\s10eperf.exe cpu) | Select-String 'cpu[0-9]' | ForEach-Object { "  perf $_" } | Tee-Object -Append $log }
}
& $ctl set $Domain $From | Out-Null
"done ok=$ok, back at $From kHz - $(Get-Date)" | Tee-Object -Append $log
