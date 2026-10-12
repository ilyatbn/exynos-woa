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
set PLAY=1
if exist "%ESP%\S10EDIAG\audio-stage.cmd" call "%ESP%\S10EDIAG\audio-stage.cmd"
reg query "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" > "%W%\params.txt" 2>&1
s10ediag mark a: pnputil add audio
pnputil /add-driver "%D%\Exynos9820Audio.inf" /install > "%W%\pnputil-add.txt" 2>&1
s10ediag mark a: pnputil returned
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
for /f "usebackq delims=" %%U in (`powershell -NoProfile -Command "(Get-CimInstance Win32_Process -Filter \"name='explorer.exe'\" | Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner | ForEach-Object { $_.Domain + '\' + $_.User })"`) do set U=%%U
echo user=%U% > "%W%\task.txt"
schtasks /create /f /tn S10eAudioTest /tr "powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\S10eTmp\audio-test.ps1" /sc once /st 23:59 /ru "%U%" /it >> "%W%\task.txt" 2>&1
schtasks /run /tn S10eAudioTest >> "%W%\task.txt" 2>&1
ping -n 45 127.0.0.1 > nul
s10ediag mark a: tone test waited
schtasks /delete /f /tn S10eAudioTest >> "%W%\task.txt" 2>&1
copy /y "%TMP%\audiotest.txt" "%W%\audiotest.txt" > nul 2>&1
:noplay
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Audio-run.txt" "%W%\run.txt" > nul 2>&1
s10ediag mark a: done
