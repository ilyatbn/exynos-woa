@echo off
rem wutemp-off.cmd (run as SYSTEM via systask.ps1) - back to the wuoff.cmd state: DoSvc + UsoSvc disabled and stopped.
sc config DoSvc start= disabled > C:\S10eTmp\wutemp.log 2>&1
sc config UsoSvc start= disabled >> C:\S10eTmp\wutemp.log 2>&1
sc stop DoSvc >> C:\S10eTmp\wutemp.log 2>&1
sc stop UsoSvc >> C:\S10eTmp\wutemp.log 2>&1
sc qc DoSvc | findstr START_TYPE >> C:\S10eTmp\wutemp.log
sc qc UsoSvc | findstr START_TYPE >> C:\S10eTmp\wutemp.log
