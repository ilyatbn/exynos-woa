# parhash.ps1 [-N 4] [-Baseline C:\S10eTmp\hash-w38.txt] - run N hashcheck.ps1 instances AT THE SAME TIME (each
# reads the fixed file set uncached at 4 KiB / 64 KiB / 1 MiB) so several large reads are in flight at once (UFS
# multi-slot test, notes/ufs-async-plan.md stage 4); every instance's output must equal the baseline. Also prints
# the wall time. Read-only.
param([int]$N = 4, [string]$Baseline = "C:\S10eTmp\hash-w38.txt")
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sw = [Diagnostics.Stopwatch]::StartNew()
$procs = foreach ($i in 1..$N) {
  Start-Process powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$here\hashcheck.ps1`" -Out C:\S10eTmp\parhash-$i.txt" -PassThru -WindowStyle Hidden
}
$procs | Wait-Process
$base = Get-Content $Baseline
$bad = 0
foreach ($i in 1..$N) {
  $got = Get-Content "C:\S10eTmp\parhash-$i.txt"
  if (Compare-Object $base $got) { $bad++; "instance $i DIFFERS:"; $got } else { "instance $i matches baseline" }
}
"{0} instances, {1} differing, {2:N1} s" -f $N, $bad, $sw.Elapsed.TotalSeconds
