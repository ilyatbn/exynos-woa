@echo off
rem audio stage A: AmpInit 0 AmpEnable 0 AboxEnable 0, play 1
s10ediag mark a: stage A
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" /v AmpInit /t REG_DWORD /d 0 /f > nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" /v AmpEnable /t REG_DWORD /d 0 /f > nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" /v AboxEnable /t REG_DWORD /d 0 /f > nul
set PLAY=1
