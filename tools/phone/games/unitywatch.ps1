# unitywatch.ps1 [-Exe DaveTheDiver] [-Log "nexon\DAVE THE DIVER"] - one status line for a running Unity game: alive /
# gone, working set, CPU s, window title, newest Application Error (1000) for it in the last 10 min, Display TDRs
# (4101) in the last 10 min, and the last non-stack line of its Player.log. Poll it from the PC. notes/games-unity.md.
param([string]$Exe = 'DaveTheDiver', [string]$Log = 'nexon\DAVE THE DIVER')
$p = Get-Process $Exe -ErrorAction SilentlyContinue | Select-Object -First 1
$o = Get-CimInstance Win32_Process -Filter "name='explorer.exe'" | Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner
$pl = "C:\Users\$($o.User)\AppData\LocalLow\$Log\Player.log"
$since = (Get-Date).AddMinutes(-10)
$err = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $since } -ErrorAction SilentlyContinue |
       Where-Object { $_.Properties[0].Value -like "$Exe*" } | Select-Object -First 1
$tdr = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 4101; StartTime = $since } -ErrorAction SilentlyContinue).Count
$last = if (Test-Path $pl) {
  Get-Content $pl -Tail 60 | Where-Object { $_ -and $_ -notmatch '^(UnityEngine\.|\s|\[ line|.*:\(|0x0000)' } | Select-Object -Last 1
}
$state = if ($p) { "alive ws {0} MB cpu {1:N0} s title [{2}] responding {3}" -f [int]($p.WS / 1MB), $p.CPU, $p.MainWindowTitle, $p.Responding } else { 'GONE' }
$e = if ($err) { "APPCRASH {0} module {1} code {2} offset {3}" -f $err.TimeCreated.ToString('HH:mm:ss'), $err.Properties[3].Value, $err.Properties[6].Value, $err.Properties[7].Value } else { 'no appcrash' }
"{0} {1}; {2}; TDR {3}; log: {4}" -f (Get-Date -Format 'HH:mm:ss'), $state, $e, $tdr, $last
