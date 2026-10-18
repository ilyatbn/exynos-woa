# tlogrecent.ps1 [-Minutes 5] [-Pattern threaded] - tlogs written in the last N minutes (this boot: the phone clock
# resets at boot, so older boots' files can carry later times - compare with "now"), with their first Pattern line.
param([int]$Minutes = 5, [string]$Pattern = 'threaded')
$now = Get-Date; "now $($now.ToString('HH:mm:ss'))"
Get-ChildItem C:\S10eTmp\tlog -Filter 'malikm-*.txt' | Where-Object { $_.Name -notlike '*stderr*' -and $_.LastWriteTime -le $now -and $_.LastWriteTime -ge $now.AddMinutes(-$Minutes) } |
  Sort-Object LastWriteTime | ForEach-Object {
    $m = Select-String -Path $_.FullName -Pattern $Pattern | Select-Object -First 1 | ForEach-Object { $_.Line }
    '{0} {1} B {2} | {3}' -f $_.Name, $_.Length, $_.LastWriteTime.ToString('HH:mm:ss'), $m }
