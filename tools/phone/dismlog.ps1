# dismlog.ps1 [-Tail 600] - the error lines of the newest DISM / CBS servicing logs (optional feature installs).
param([int]$Tail = 600)
foreach ($f in 'C:\Windows\Logs\DISM\dism.log', 'C:\Windows\Logs\CBS\CBS.log') {
  "=== $f"
  Get-Content $f -Tail $Tail -ErrorAction SilentlyContinue |
    Select-String -Pattern 'Error|failed|0x8007|0x800f|Access' | Select-Object -Last 20 |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(230, $_.Line.Length)) }
}
