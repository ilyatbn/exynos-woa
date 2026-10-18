@echo off
rem wutemp-on.cmd (run as SYSTEM via systask.ps1) - Delivery Optimization + Update Orchestrator back on for a Store install
rem (user 2026-10-19: re-enable for a sec, install, disable again). wutemp-off.cmd undoes it. Log C:\S10eTmp\wutemp.log.
sc config DoSvc start= demand > C:\S10eTmp\wutemp.log 2>&1
sc config UsoSvc start= demand >> C:\S10eTmp\wutemp.log 2>&1
sc start DoSvc >> C:\S10eTmp\wutemp.log 2>&1
sc start UsoSvc >> C:\S10eTmp\wutemp.log 2>&1
sc qc DoSvc | findstr START_TYPE >> C:\S10eTmp\wutemp.log
