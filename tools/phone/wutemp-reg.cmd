@echo off
rem wutemp-reg.cmd START (SYSTEM via systask.ps1) - DoSvc Start value directly (the SCM refuses ChangeServiceConfig on it, error 5 even as SYSTEM; read at boot). 3 = on demand, 4 = disabled.
reg add HKLM\SYSTEM\CurrentControlSet\Services\DoSvc /v Start /t REG_DWORD /d %1 /f > C:\S10eTmp\wutemp.log 2>&1
reg query HKLM\SYSTEM\CurrentControlSet\Services\DoSvc /v Start >> C:\S10eTmp\wutemp.log 2>&1
