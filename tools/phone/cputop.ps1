# cputop.ps1 [-Seconds 10] [-Top 8] - CPU time per process over an interval (all instances of a name summed), as % of
# one core: what keeps the CPU busy during a test (G17 pacing, notes/g17-dxva-mfc.md).
param([int]$Seconds = 10, [int]$Top = 8)
function Snap { $h = @{}; Get-Process | ForEach-Object { try { $h[$_.Id] = @($_.Name, $_.TotalProcessorTime.TotalMilliseconds) } catch {} }; $h }
$a = Snap
Start-Sleep $Seconds
$b = Snap
$byName = @{}
foreach ($id in $b.Keys) {
  if (-not $a.ContainsKey($id)) { continue }
  $d = $b[$id][1] - $a[$id][1]
  $n = $b[$id][0]
  $byName[$n] = $byName[$n] + $d
}
"cores: $([Environment]::ProcessorCount); % of one core over $Seconds s:"
$byName.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top |
  ForEach-Object { '  {0,-28} {1,6:N1} %' -f $_.Key, ($_.Value / ($Seconds * 10)) }
