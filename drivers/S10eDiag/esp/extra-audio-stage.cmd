@echo off
rem S10eAgent extra step (D8): change the Exynos9820Audio hardware stage (\S10EDIAG\audio-stage.cmd writes Parameters)
rem without reinstalling, then test. The driver reads Parameters at PrepareHardware (boot), so:
rem Pass 1: call audio-stage.cmd, clean restart. Pass 2 (marker C:\S10eTmp\audstage.flg): quiet WASAPI/volume-key test
rem (audio-test.ps1: amplitude 0.03, ~10 %% volume) in the user's session + driver log. The PC mic listens (cycle.sh).
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
set W=%L%\audio
mkdir "%W%"
if exist C:\S10eTmp\audstage.flg goto pass2
s10ediag mark a: stage pass 1
if exist "%ESP%\S10EDIAG\audio-stage.cmd" call "%ESP%\S10EDIAG\audio-stage.cmd"
reg query "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" > "%W%\params-set.txt" 2>&1
echo pass2 > C:\S10eTmp\audstage.flg
s10ediag mark a: clean restart into the new stage
shutdown /r /t 0 /f
rem Never return to collect.cmd while Windows shuts down (see extra-audio-swap.cmd).
:holdrestart
ping -n 30 127.0.0.1 > nul
goto holdrestart
:pass2
del /f /q C:\S10eTmp\audstage.flg
s10ediag mark a: stage pass 2 - quiet test
copy /y "%ESP%\S10EDIAG\audio-test.ps1" "%TMP%\audio-test.ps1" > nul
copy /y "%ESP%\S10EDIAG\run-as-user.ps1" "%TMP%\run-as-user.ps1" > nul
del /q "%TMP%\audiotest.txt" > nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File "%TMP%\run-as-user.ps1" -Script C:\S10eTmp\audio-test.ps1 -Wait 60 > "%W%\task.txt" 2>&1
copy /y "%TMP%\audiotest.txt" "%W%\audiotest.txt" > nul 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Audio-run.txt" "%W%\run.txt" > nul 2>&1
for /f "usebackq delims=" %%L in ("%TMP%\audiotest.txt") do s10ediag mark "t: %%L"
s10ediag mark a: done
