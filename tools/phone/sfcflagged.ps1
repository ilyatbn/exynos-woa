# sfcflagged.ps1 - files the most recent sfc run flagged ("Repairing file" / "Cannot repair") in CBS.log, newest run only
$lines = Get-Content C:\Windows\Logs\CBS\CBS.log
$start = 0
for ($i = $lines.Count - 1; $i -ge 0; $i--) { if ($lines[$i] -like '*Beginning system scan*' -or $lines[$i] -like '*[SR] Verifying 100 components*' -and $start -eq 0) { } }
$runStart = ($lines | Select-String -SimpleMatch 'Initializing online with Windows opt-in' | Select-Object -Last 1).LineNumber
if (-not $runStart) { $runStart = [Math]::Max(0, $lines.Count - 20000) }
$lines[$runStart..($lines.Count - 1)] | Where-Object { $_ -like '*[SR] Repairing file*' -or $_ -like '*[SR] Cannot repair*' } |
  ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) }
