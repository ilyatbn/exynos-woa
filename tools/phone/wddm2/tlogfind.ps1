# tlogfind.ps1 [-Exe LogonUI] [-Pattern threaded] [-Count 6] - the newest malikm tlogs of an executable (by write
# time, stderr files excluded) and the lines matching Pattern in each - e.g. which LogonUI instances ran threaded
# (notes/perf-threaded-context.md). The phone clock resets at boot: compare against "now".
param([string]$Exe = 'LogonUI', [string]$Pattern = 'threaded', [int]$Count = 6)
"now $(Get-Date -Format 'MM-dd HH:mm:ss')"
Get-ChildItem C:\S10eTmp\tlog -Filter "malikm-$Exe*.txt" | Where-Object { $_.Name -notlike '*stderr*' } |
  Sort-Object LastWriteTime -Descending | Select-Object -First $Count | ForEach-Object {
    $m = Select-String -Path $_.FullName -Pattern $Pattern | Select-Object -First 2 | ForEach-Object { $_.Line }
    '{0} {1} B {2} | {3}' -f $_.Name, $_.Length, $_.LastWriteTime.ToString('MM-dd HH:mm:ss'), ($m -join ' ; ')
  }
