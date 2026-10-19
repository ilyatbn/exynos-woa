# parbench.ps1 [-Bs 1048576] [-MB 32] - N concurrent page-aligned uncached sequential readers (diskbench.ps1 -Only
# aligned) on different large System32 files; prints each stream and the aggregate MiB/s. With UFS multi-slot
# (notes/ufs-async-plan.md stage 4) the aggregate should exceed a single stream. Read-only.
param([int]$Bs = 1048576, [int]$MB = 32)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$files = "edgehtml.dll", "mshtml.dll", "Windows.UI.Xaml.dll", "shell32.dll" | ForEach-Object { "$env:WINDIR\System32\$_" }
$sw = [Diagnostics.Stopwatch]::StartNew()
$i = 0
$procs = foreach ($f in $files) {
  $i++
  Start-Process powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$here\diskbench.ps1`" -File `"$f`" -MB $MB -Only aligned -Bs $Bs" `
    -RedirectStandardOutput "C:\S10eTmp\parbench-$i.txt" -PassThru -WindowStyle Hidden
}
$procs | Wait-Process
$t = $sw.Elapsed.TotalSeconds
1..$i | ForEach-Object { Get-Content "C:\S10eTmp\parbench-$_.txt" | Select-String "seq" }
"{0} streams x {1} MiB in {2:N2} s = {3:N1} MiB/s aggregate (includes process start)" -f $i, $MB, $t, ($i * $MB / $t)
