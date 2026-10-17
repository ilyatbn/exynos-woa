# threadtop.ps1 -Name Photos [-Seconds 5] [-Top 8] - CPU time per thread of a process over an interval, as % of one
# core, with each thread's start address module (System.Diagnostics: StartAddress) - is one thread pegged?
param([Parameter(Mandatory)][string]$Name, [int]$Seconds = 5, [int]$Top = 8)
$p = Get-Process $Name -ErrorAction SilentlyContinue | Sort-Object WorkingSet64 -Descending | Select-Object -First 1
if (-not $p) { "no $Name"; return }
$a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
Start-Sleep $Seconds
$p.Refresh()
$rows = foreach ($t in $p.Threads) {
  try {
    $d = $t.TotalProcessorTime.TotalMilliseconds - $(if ($a.ContainsKey($t.Id)) { $a[$t.Id] } else { 0 })
    [pscustomobject]@{ Id = $t.Id; Pct = $d / ($Seconds * 10); Prio = $t.PriorityLevel; State = $t.ThreadState; Wait = $t.WaitReason }
  } catch {}
}
"$Name pid $($p.Id), $($p.Threads.Count) threads, % of one core over $Seconds s:"
$rows | Sort-Object Pct -Descending | Select-Object -First $Top | ForEach-Object {
  '  tid {0,-6} {1,6:N1} %  {2,-12} {3,-10} {4}' -f $_.Id, $_.Pct, $_.Prio, $_.State, $_.Wait }
