@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd): install Exynos9820Audio (ACX speaker endpoint, ACPI\SAMS9827)
rem and run audio-test.ps1 in the logged-on user's session (tones + volume keys; the PC mic listens). notes/d8-audio-win-m1.md.
rem Package expected at %ESP%\S10EDIAG\drivers\Exynos9820Audio\ ; audio-test.ps1 at %ESP%\S10EDIAG\audio-test.ps1.
rem Staged bring-up (run a1 powered the phone off during the install): %ESP%\S10EDIAG\audio-stage.cmd pre-sets
rem Parameters (the INF AddReg is NOCLOBBER 0x00010003, so existing values win) and may set PLAY=0.
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
if not exist "%TMP%" mkdir "%TMP%"
set W=%L%\audio
mkdir "%W%"
set D=%TMP%\s10e-audio
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%ESP%\S10EDIAG\drivers\Exynos9820Audio" "%D%" > "%W%\xcopy.txt" 2>&1
copy /y "%ESP%\S10EDIAG\audio-test.ps1" "%TMP%\audio-test.ps1" > nul
if exist "%ESP%\S10EDIAG\install-drivers.cmd" call "%ESP%\S10EDIAG\install-drivers.cmd"
set PLAY=1
if exist "%ESP%\S10EDIAG\audio-stage.cmd" call "%ESP%\S10EDIAG\audio-stage.cmd"
reg query "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" > "%W%\params.txt" 2>&1
s10ediag mark a: pnputil add audio
pnputil /add-driver "%D%\Exynos9820Audio.inf" /install > "%W%\pnputil-add.txt" 2>&1
s10ediag mark a: pnputil returned
rem already installed on an earlier run: restart so PrepareHardware reads this stage's Parameters
pnputil /restart-device "ACPI\SAMS9827\0" > "%W%\restart.txt" 2>&1
s10ediag mark a: device restarted
ping -n 20 127.0.0.1 > nul
s10ediag mark a: enum
pnputil /enum-devices /deviceid "ACPI\SAMS9827" /properties > "%W%\dev-audio.txt" 2>&1
pnputil /enum-devices /class MEDIA > "%W%\media.txt" 2>&1
pnputil /enum-devices /class AudioEndpoint > "%W%\endpoints.txt" 2>&1
sc query Exynos9820Audio > "%W%\sc.txt" 2>&1
sc query audiosrv >> "%W%\sc.txt" 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Audio-run.txt" "%W%\run-before.txt" > nul 2>&1
if "%PLAY%"=="0" goto noplay
s10ediag mark a: user-session tone test
del /q "%TMP%\audiotest.txt" > nul 2>&1
copy /y "%ESP%\S10EDIAG\run-as-user.ps1" "%TMP%\run-as-user.ps1" > nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%TMP%\run-as-user.ps1" -Script C:\S10eTmp\audio-test.ps1 -Wait 60 > "%W%\task.txt" 2>&1
s10ediag mark a: tone test waited
copy /y "%TMP%\audiotest.txt" "%W%\audiotest.txt" > nul 2>&1
:noplay
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Audio-run.txt" "%W%\run.txt" > nul 2>&1
rem run audB5: the ESP copy was lost when the restart did not flush - mirror the key lines into the PRAM mark ring
for /f "usebackq delims=" %%L in ("%TMP%\audiotest.txt") do s10ediag mark "t: %%L"
findstr /c:"stream" /c:"trace" /c:"Abox" /c:"Amp" "%SystemRoot%\System32\LogFiles\Exynos9820Audio-run.txt" > "%TMP%\runkey.txt" 2>nul
for /f "usebackq delims=" %%L in ("%TMP%\runkey.txt") do s10ediag mark "r: %%L"
s10ediag mark a: done
