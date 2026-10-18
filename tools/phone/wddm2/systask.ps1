# systask.ps1 NAME CMDFILE [WAITSEC] - run C:\S10eTmp\CMDFILE once as SYSTEM and wait for it (wddm2dbg.sh task).
# schtasks /create tasks keep the default "start only on AC power": on battery (phone on a hub without PD power)
# they stay Queued forever and the swap silently does not happen (2026-10-18 umd32swap). This one allows batteries.
param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][string]$Cmd, [int]$Wait = 8)
$a = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c C:\S10eTmp\$Cmd"
$p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $Name -Action $a -Principal $p -Settings $s -Force | Out-Null
Start-ScheduledTask -TaskName $Name
$t0 = Get-Date
do { Start-Sleep 1; $st = (Get-ScheduledTask -TaskName $Name).State } while ($st -ne 'Ready' -and ((Get-Date) - $t0).TotalSeconds -lt $Wait)
"task $Name state $st, last result $((Get-ScheduledTaskInfo -TaskName $Name).LastTaskResult)"
Unregister-ScheduledTask -TaskName $Name -Confirm:$false
