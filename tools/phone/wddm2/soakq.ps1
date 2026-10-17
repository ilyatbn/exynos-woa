# soakq.ps1 [-Start] [-SinceFile C:\S10eTmp\soak.since] - soak sample for soak.sh: -Start records "now" in the file;
# otherwise prints "dwm <pid> crashes <Application Error 1000 count since the recorded time>".
param([switch]$Start, [string]$SinceFile = 'C:\S10eTmp\soak.since')
if ($Start) { (Get-Date).ToString('o') | Out-File $SinceFile -Encoding ascii; "dwm $((Get-Process dwm).Id)"; return }
$since = [datetime](Get-Content $SinceFile)
$c = @(Get-WinEvent -FilterHashtable @{LogName = 'Application'; Id = 1000; StartTime = $since} -ErrorAction SilentlyContinue).Count
"dwm $((Get-Process dwm -ErrorAction SilentlyContinue).Id) crashes $c"
