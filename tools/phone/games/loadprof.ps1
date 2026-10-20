# loadprof.ps1 [-Name DaveTheDiver] [-Seconds 600] [-Step 5] [-Out C:\S10eTmp\loadprof.txt] - G33 (notes/games-unity.md):
# where a game's loading time goes. Every Step seconds one line: time, game CPU % of one core, the 3 busiest threads
# (% and the module of their start address), the game's read MB (Win32_Process ReadTransferCount, cache hits
# included) and the physical-disk read MB (PerfDisk _Total), both per step. Waits up to 5 min for the game to start;
# ends when it exits or after -Seconds. Run over SSH next to davebot (the bot log has the screen states to line up).
param([string]$Name = 'DaveTheDiver', [int]$Seconds = 600, [int]$Step = 5, [string]$Out = 'C:\S10eTmp\loadprof.txt')
$t0 = Get-Date
while (-not ($p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1)) {
  if (((Get-Date) - $t0).TotalSeconds -gt 300) { "no $Name" | Set-Content $Out; exit 1 }
  Start-Sleep -Milliseconds 500
}
"# $Name pid $($p.Id) started $($p.StartTime.ToString('HH:mm:ss.fff')); step $Step s" | Set-Content $Out
function Mods { $m = @(); try { foreach ($x in $p.Modules) { $m += ,@([int64]$x.BaseAddress, [int64]$x.ModuleMemorySize, $x.ModuleName) } } catch {}; $m }
function ModOf([int64]$a) { foreach ($x in $script:mods) { if ($a -ge $x[0] -and $a -lt $x[0] + $x[1]) { return $x[2] } }; return '?' }
function Disk { (Get-CimInstance Win32_PerfRawData_PerfDisk_PhysicalDisk -Filter "Name='_Total'").DiskReadBytesPersec }
function GameRead { (Get-CimInstance Win32_Process -Filter "ProcessId=$($p.Id)").ReadTransferCount }
$mods = Mods; $modsAt = Get-Date
$th = @{}; foreach ($t in $p.Threads) { try { $th[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
$cpu0 = $p.TotalProcessorTime.TotalMilliseconds; $d0 = Disk; $r0 = GameRead; $last = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
  Start-Sleep $Step
  $p.Refresh(); if ($p.HasExited) { "# exited $((Get-Date).ToString('HH:mm:ss.fff'))" | Add-Content $Out; break }
  if (((Get-Date) - $modsAt).TotalSeconds -gt 30) { $mods = Mods; $modsAt = Get-Date }   # modules load late
  $now = Get-Date; $dt = ($now - $last).TotalMilliseconds; $last = $now
  $cpu = $p.TotalProcessorTime.TotalMilliseconds; $d = Disk; $r = GameRead
  $rows = foreach ($t in $p.Threads) {
    try {
      $ms = $t.TotalProcessorTime.TotalMilliseconds; $prev = if ($th.ContainsKey($t.Id)) { $th[$t.Id] } else { 0 }
      $th[$t.Id] = $ms
      [pscustomobject]@{ Id = $t.Id; Pct = ($ms - $prev) * 100 / $dt; Start = [int64]$t.StartAddress }
    } catch {}
  }
  $top = ($rows | Sort-Object Pct -Descending | Select-Object -First 3 | ForEach-Object { '{0}:{1:N0}%:{2}' -f $_.Id, $_.Pct, (ModOf $_.Start) }) -join ' '
  '{0} cpu {1,4:N0}% read {2,6:N1} MB disk {3,6:N1} MB threads {4} | {5}' -f $now.ToString('HH:mm:ss'), (($cpu - $cpu0) * 100 / $dt),
    (($r - $r0) / 1MB), (($d - $d0) / 1MB), $p.Threads.Count, $top | Add-Content $Out
  $cpu0 = $cpu; $d0 = $d; $r0 = $r
}
