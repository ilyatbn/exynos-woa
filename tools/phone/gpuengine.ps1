# gpuengine.ps1 [-Name dwm] [-Seconds 3] - per-process GPU engine utilisation by adapter (perf counter "GPU Engine"):
# which adapter / engine a process renders on (our Mali vs Microsoft Basic Render = WARP), averaged over -Seconds.
# G17 (notes/g17-dxva-mfc.md): is DWM composing on the Mali or on WARP?
param([string]$Name = 'dwm', [int]$Seconds = 3)
$ids = (Get-Process $Name -ErrorAction SilentlyContinue).Id
foreach ($id in $ids) {
  $c = Get-Counter "\GPU Engine(pid_$($id)_*)\Utilization Percentage" -SampleInterval 1 -MaxSamples $Seconds -ErrorAction SilentlyContinue
  $avg = @{}
  foreach ($s in $c) { foreach ($v in $s.CounterSamples) { $avg[$v.InstanceName] += $v.CookedValue / $Seconds } }
  "$Name pid $id"
  $avg.GetEnumerator() | Sort-Object Name | ForEach-Object { '  {0,-70} {1,6:N2} %' -f $_.Name, $_.Value }
}
Get-CimInstance Win32_VideoController | ForEach-Object { 'adapter: {0} ({1})' -f $_.Name, $_.PNPDeviceID }
