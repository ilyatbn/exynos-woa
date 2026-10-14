# run-user cmd line in the logged-on user's session (one-shot scheduled task). Waits for a marker file written when
# the command ends: from an SSH session the task State reads "Ready" while the instance still runs, and
# Unregister-ScheduledTask then kills it (0xC000013A after ~20 s, 2026-10-14).
param([string]$Cmd, [int]$Wait = 60)
$o = Get-CimInstance Win32_Process -Filter "name='explorer.exe'" | Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner
$u = "$($o.Domain)\$($o.User)"
$done = 'C:\S10eTmp\runuser2.done'
Remove-Item $done -ErrorAction SilentlyContinue
$a = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/d /c $Cmd & echo done> $done"
$p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Limited
$s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 2) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName S10eUserRun2 -Action $a -Principal $p -Settings $s -Force | Out-Null
Start-ScheduledTask -TaskName S10eUserRun2
$t0 = Get-Date
while (-not (Test-Path $done) -and ((Get-Date) - $t0).TotalSeconds -lt $Wait) { Start-Sleep 2 }
"done marker: $(Test-Path $done) after $([int]((Get-Date) - $t0).TotalSeconds) s"
Unregister-ScheduledTask -TaskName S10eUserRun2 -Confirm:$false
