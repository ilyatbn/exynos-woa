# memcommit.ps1 - system memory commit: committed bytes vs commit limit, available RAM, page file(s). GPU aperture
# allocations are backed by system memory, so a full commit charge fails them (G-game, notes/g17-dxva-mfc.md).
$c = Get-Counter '\Memory\Committed Bytes', '\Memory\Commit Limit', '\Memory\Available MBytes'
foreach ($s in $c.CounterSamples) {
  $n = $s.Path -replace '.*\\', ''
  if ($n -eq 'available mbytes') { '{0,-18} {1,10:N0} MB' -f $n, $s.CookedValue } else { '{0,-18} {1,10:N0} MB' -f $n, ($s.CookedValue / 1MB) }
}
$pf = Get-CimInstance Win32_PageFileUsage
if ($pf) { $pf | ForEach-Object { 'pagefile {0}: {1} MB allocated, {2} MB used' -f $_.Name, $_.AllocatedBaseSize, $_.CurrentUsage } }
else { 'no page file' }
