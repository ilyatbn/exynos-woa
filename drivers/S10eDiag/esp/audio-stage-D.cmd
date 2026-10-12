@echo off
rem audio stage D: AmpInit 1 AmpEnable 1 AboxEnable 1, play 1
s10ediag mark a: stage D
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" /v AmpInit /t REG_DWORD /d 1 /f > nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" /v AmpEnable /t REG_DWORD /d 1 /f > nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" /v AboxEnable /t REG_DWORD /d 1 /f > nul
set PLAY=1
