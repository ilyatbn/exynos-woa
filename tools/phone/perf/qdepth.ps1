# qdepth.ps1 [-Seconds 30] [-Launch steam|brave|<exe>] [-Out file] - disk queue depth while an app starts (run in the
# user session via runapp.sh): samples PhysicalDisk(_Total) "Current Disk Queue Length" every 50 ms and prints a
# histogram (how often 0, 1, 2, 3+ requests were outstanding/queued). With MaxNumberOfIO = 1 the miniport holds one,
# so samples >= 2 mean the app/OS issue parallel I/O that a deeper queue could overlap. notes/ufs-async-plan.md.
param([int]$Seconds = 30, [string]$Launch = "", [string]$Out = "")
if ($Launch) {
  $exe = $Launch
  if (-not (Test-Path $exe)) { $exe = (Get-ChildItem "$env:ProgramFiles", "${env:ProgramFiles(x86)}" -Recurse -Depth 4 -Filter "$Launch.exe" -ErrorAction SilentlyContinue | Select -First 1).FullName }
  Start-Process $exe
}
$raw0 = Get-CimInstance Win32_PerfRawData_PerfDisk_PhysicalDisk -Filter "Name='_Total'"
$h = @{}; $n = 0; $sum = 0
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
  $q = [int](Get-CimInstance Win32_PerfRawData_PerfDisk_PhysicalDisk -Filter "Name='_Total'").CurrentDiskQueueLength
  $k = [Math]::Min($q, 4); $h[$k] = 1 + [int]$h[$k]; $n++; $sum += $q
  Start-Sleep -Milliseconds 50
}
$raw1 = Get-CimInstance Win32_PerfRawData_PerfDisk_PhysicalDisk -Filter "Name='_Total'"
$dt = [double]($raw1.Timestamp_Sys100NS - $raw0.Timestamp_Sys100NS)
$avgQ = ([double]$raw1.AvgDiskQueueLength - [double]$raw0.AvgDiskQueueLength) / $dt          # PERF_COUNTER_100NS_QUEUELEN_TYPE
$idle = ([double]$raw1.PercentIdleTime - [double]$raw0.PercentIdleTime) / $dt * 100.0         # PERF_PRECISION_100NS_TIMER
$reads = [uint64]$raw1.DiskReadsPersec - [uint64]$raw0.DiskReadsPersec
$extra = " | raw: avg queue {0:N2}, idle {1:N1} %, reads {2}" -f $avgQ, $idle, $reads
$r = "{0} launch='{1}' samples {2}, mean {3:N2}: q0 {4} q1 {5} q2 {6} q3 {7} q4+ {8}" -f (Get-Date -f HH:mm:ss), $Launch, $n, ($sum / [Math]::Max(1, $n)),
  [int]$h[0], [int]$h[1], [int]$h[2], [int]$h[3], [int]$h[4]
$r += $extra
if ($Out) { $r | Add-Content $Out } else { $r }
