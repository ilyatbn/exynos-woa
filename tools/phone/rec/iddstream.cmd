@echo off
rem (SYSTEM task) G18 remote monitor PoC: virtual monitor (IddSampleApp, 300 s) + viewer server S10eIddView on 8090 (300 s).
start "" /b C:\S10eTmp\idd2\S10eIddView.exe 8090 300 > C:\S10eTmp\iddview.out 2>&1
C:\S10eTmp\idd2\IddSampleApp.exe 300 > C:\S10eTmp\iddapp.out 2>&1
