# tlogq.ps1 -Id PID [-Pattern 'app ioctl top'] [-Last 4] [-Width 900] - lines of a game's UMD tlog
# (C:\S10eTmp\tlog\malikm-<exe>-PID.txt) matching a pattern; avoids quoting regexes through ssh. notes/games-unity.md.
param([Parameter(Mandatory)][int]$Id, [string]$Pattern = 'app ioctl top', [int]$Last = 4, [int]$Width = 900)
$f = Get-ChildItem "C:\S10eTmp\tlog\malikm-*-$Id.txt" | Select-Object -First 1
if (-not $f) { "no tlog for pid $Id"; exit 1 }
Select-String -Path $f.FullName -Pattern $Pattern | Select-Object -Last $Last |
  ForEach-Object { $_.Line.Substring(0, [Math]::Min($Width, $_.Line.Length)) }
