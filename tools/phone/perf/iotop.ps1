# iotop.ps1 [-Seconds 15] [-Launch brave|steam|<exe>] [-Out file] - which processes read during a window:
# per-process ReadTransferCount / ReadOperationCount deltas (Win32_Process: ALL reads, cache hits included) next to
# the physical disk delta (PerfDisk _Total: real device reads). -Launch starts an app at t=0 (run it in the user
# session, tools/phone/rec/runapp.sh). Top 12 by bytes. notes/app-launch-slowness.md.
param([int]$Seconds = 15, [string]$Launch = "", [string]$Out = "")
function Snap { $h = @{}; Get-CimInstance Win32_Process | % { $h["$($_.ProcessId)"] = @($_.Name, [uint64]$_.ReadTransferCount, [uint64]$_.ReadOperationCount) }; $h }
function Disk { $d = Get-CimInstance Win32_PerfRawData_PerfDisk_PhysicalDisk | ? Name -eq "_Total"; @([uint64]$d.DiskReadBytesPersec, [uint64]$d.DiskReadsPersec) }
$a = Snap; $d0 = Disk
if ($Launch) {
  $exe = $Launch
  if (-not (Test-Path $exe)) { $exe = (Get-ChildItem "$env:ProgramFiles", "${env:ProgramFiles(x86)}" -Recurse -Depth 4 -Filter "$Launch.exe" -ErrorAction SilentlyContinue | Select -First 1).FullName }
  Start-Process $exe
}
Start-Sleep $Seconds
$b = Snap; $d1 = Disk
$rows = foreach ($k in $b.Keys) {
  $n = $b[$k]; $o = $a[$k]; if (-not $o) { $o = @($n[0], 0, 0) }
  [pscustomobject]@{ Pid = $k; Name = $n[0]; MB = [math]::Round(($n[1] - $o[1]) / 1MB, 1); Ops = $n[2] - $o[2] }
}
$r = @("{0} window {1}s launch='{2}': physical disk {3:N1} MB in {4} reads" -f (Get-Date -f HH:mm:ss), $Seconds, $Launch, (($d1[0] - $d0[0]) / 1MB), ($d1[1] - $d0[1]))
$r += $rows | Sort-Object MB -Descending | Select-Object -First 12 | Format-Table -AutoSize | Out-String -Width 120
if ($Out) { $r | Add-Content $Out } else { $r }
