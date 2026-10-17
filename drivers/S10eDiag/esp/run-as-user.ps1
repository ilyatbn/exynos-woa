# Runs a PowerShell script in the logged-on (explorer.exe owner's) interactive session via a one-shot scheduled task,
# waits for it, deletes the task. Called as SYSTEM from extra-*.cmd. usage: run-as-user.ps1 <script> <waitSeconds>
param([string]$Script, [int]$Wait = 45)
$o = Get-CimInstance Win32_Process -Filter "name='explorer.exe'" | Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner
if (-not $o -or -not $o.User) { "no explorer.exe owner - nobody logged on"; exit 1 }
$u = "$($o.Domain)\$($o.User)"
"user $u"
$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$Script`""
$p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Limited
# The default task settings do not start on battery: the task stays Queued (2026-10-18, phone on battery while the
# USB-C port held a keyboard dongle) - and this script then deleted it unrun.
$st = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName S10eUserRun -Action $a -Principal $p -Settings $st -Force | Out-Null
Start-ScheduledTask -TaskName S10eUserRun
"started $(Get-Date -Format HH:mm:ss.fff)"
$t0 = Get-Date
do { Start-Sleep 1; $s = (Get-ScheduledTask -TaskName S10eUserRun).State } while ($s -eq 'Running' -and ((Get-Date) - $t0).TotalSeconds -lt $Wait)
"state $s after $([int]((Get-Date) - $t0).TotalSeconds) s, last result $((Get-ScheduledTaskInfo -TaskName S10eUserRun).LastTaskResult)"
Unregister-ScheduledTask -TaskName S10eUserRun -Confirm:$false
