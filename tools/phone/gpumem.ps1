# gpumem.ps1 - GPU memory as dxgkrnl accounts it (perf counters "GPU Adapter Memory" / "GPU Process Memory"): per
# adapter dedicated / shared usage, and the processes using the most. G17 pacing (notes/g17-dxva-mfc.md): does the
# working set of DWM + the app exceed the segment budget (VidMm paging thrash: MAP/UNMAP_APERTURE_SEGMENT ~40/s)?
# -Name dwm -Samples N -Every S: watch one process's Total Committed / Shared Usage over time instead.
param([string]$Name = '', [int]$Samples = 6, [int]$Every = 20)
if ($Name) {
  for ($i = 0; $i -lt $Samples; $i++) {
    $ids = (Get-Process $Name -ErrorAction SilentlyContinue).Id
    foreach ($id in $ids) {
      $c = Get-Counter "\GPU Process Memory(pid_$($id)_*)\Total Committed", "\GPU Process Memory(pid_$($id)_*)\Shared Usage" -ErrorAction SilentlyContinue
      $v = $c.CounterSamples | Where-Object { $_.CookedValue -gt 0 }
      '{0:HH:mm:ss} {1} pid {2}: {3}' -f (Get-Date), $Name, $id, (($v | ForEach-Object { '{0} {1:N1} MB' -f ($_.Path -replace '.*\\', ''), ($_.CookedValue / 1MB) }) -join ', ')
    }
    if ($i -lt $Samples - 1) { Start-Sleep -Seconds $Every }
  }
  return
}
$a = Get-Counter '\GPU Adapter Memory(*)\Dedicated Usage', '\GPU Adapter Memory(*)\Shared Usage', '\GPU Adapter Memory(*)\Total Committed' -ErrorAction SilentlyContinue
foreach ($v in $a.CounterSamples) { if ($v.CookedValue -gt 0) { '{0,-90} {1,8:N1} MB' -f $v.Path, ($v.CookedValue / 1MB) } }
$p = Get-Counter '\GPU Process Memory(*)\Shared Usage', '\GPU Process Memory(*)\Dedicated Usage', '\GPU Process Memory(*)\Total Committed' -ErrorAction SilentlyContinue
$p.CounterSamples | Where-Object { $_.CookedValue -gt 8MB } | Sort-Object CookedValue -Descending | Select-Object -First 12 |
  ForEach-Object { '{0,-90} {1,8:N1} MB' -f $_.Path, ($_.CookedValue / 1MB) }
