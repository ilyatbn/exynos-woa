# coretop.ps1 [-Seconds 5] - per-core CPU load (% Processor Time, % DPC Time, % Interrupt Time) averaged over an
# interval: cpu0-3 = A55 LITTLE, cpu4-5 = A75 MID. G17 pacing (notes/g17-dxva-mfc.md): which cores carry playback.
param([int]$Seconds = 5)
$c = Get-Counter -Counter '\Processor(*)\% Processor Time', '\Processor(*)\% DPC Time', '\Processor(*)\% Interrupt Time' `
                 -SampleInterval 1 -MaxSamples $Seconds
$avg = @{}
foreach ($s in $c) { foreach ($v in $s.CounterSamples) { $avg[$v.Path] += $v.CookedValue / $Seconds } }
foreach ($i in 0..5 + '_total') {
  $p = $avg.Keys | Where-Object { $_ -match "processor\($i\)\\% processor time" }
  $d = $avg.Keys | Where-Object { $_ -match "processor\($i\)\\% dpc time" }
  $q = $avg.Keys | Where-Object { $_ -match "processor\($i\)\\% interrupt time" }
  'cpu{0,-7} busy {1,5:N1} %  dpc {2,5:N1} %  isr {3,5:N1} %' -f $i, $avg[$p], $avg[$d], $avg[$q]
}
