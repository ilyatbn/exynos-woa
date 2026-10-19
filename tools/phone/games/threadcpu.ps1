# threadcpu.ps1 [-Name DaveTheDiver] [-Seconds 10] [-Top 8] - per-thread CPU use of a process over an interval (two
# samples of each thread's TotalProcessorTime): a thread near 100 % of one core is the frame's bottleneck. Also the
# process total and the number of cores. G32 (notes/games-unity.md): is Dave the Diver bound by the game's own
# (emulated) thread, our UMD's driver thread, or the GPU?
param([string]$Name = 'DaveTheDiver', [int]$Seconds = 10, [int]$Top = 8)
$p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no $Name"; exit 1 }
$a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
$p0 = $p.TotalProcessorTime.TotalMilliseconds
Start-Sleep $Seconds
$p.Refresh()
$rows = foreach ($t in $p.Threads) {
  try {
    $ms = $t.TotalProcessorTime.TotalMilliseconds
    if ($a.ContainsKey($t.Id)) { [pscustomobject]@{ Tid = $t.Id; Pct = [math]::Round(($ms - $a[$t.Id]) * 100 / ($Seconds * 1000), 1); Prio = $t.CurrentPriority } }
  } catch {}
}
"{0}: process {1:N0} % of one core over {2} s, {3} cores" -f $Name, (($p.TotalProcessorTime.TotalMilliseconds - $p0) * 100 / ($Seconds * 1000)), $Seconds, [Environment]::ProcessorCount
$rows | Sort-Object Pct -Descending | Select-Object -First $Top | ForEach-Object { "  tid {0,6}  {1,5} %  prio {2}" -f $_.Tid, $_.Pct, $_.Prio }
