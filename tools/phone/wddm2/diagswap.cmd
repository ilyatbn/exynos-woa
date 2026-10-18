@echo off
rem (SYSTEM task) C:\S10eTmp\S10eDiag-new.sys into System32\drivers (loaded at the next boot). Like kmdswap.cmd: the
rem loaded image is renamed to .prev (rollback: copy it back from TWRP), then the new one is copied in.
set D=C:\Windows\System32\drivers
if exist "%D%\S10eDiag.sys.prev" del /f /q "%D%\S10eDiag.sys.prev"
move /y "%D%\S10eDiag.sys" "%D%\S10eDiag.sys.prev" > C:\S10eTmp\diagswap.log 2>&1
copy /y C:\S10eTmp\S10eDiag-new.sys "%D%\S10eDiag.sys" >> C:\S10eTmp\diagswap.log 2>&1
certutil -hashfile "%D%\S10eDiag.sys" SHA256 >> C:\S10eTmp\diagswap.log 2>&1
