@echo off
rem S10eAgent extra step (D8): test-only - the driver is already installed and loaded at boot (no pnputil add, no
rem device restart), then the WASAPI/volume-key test in the user's session and the driver/endpoint state.
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
set W=%L%\audio
mkdir "%W%"
copy /y "%ESP%\S10EDIAG\audio-test.ps1" "%TMP%\audio-test.ps1" > nul
copy /y "%ESP%\S10EDIAG\run-as-user.ps1" "%TMP%\run-as-user.ps1" > nul
pnputil /enum-devices /class AudioEndpoint /properties > "%W%\endpoints.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\SAMS9827" /properties > "%W%\dev-audio.txt" 2>&1
sc query audiosrv > "%W%\sc.txt" 2>&1
sc query AudioEndpointBuilder >> "%W%\sc.txt" 2>&1
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render" /s > "%W%\mmdevices.txt" 2>&1
s10ediag mark a: test-only
del /q "%TMP%\audiotest.txt" > nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File "%TMP%\run-as-user.ps1" -Script C:\S10eTmp\audio-test.ps1 -Wait 60 > "%W%\task.txt" 2>&1
copy /y "%TMP%\audiotest.txt" "%W%\audiotest.txt" > nul 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Audio-run.txt" "%W%\run.txt" > nul 2>&1
for /f "usebackq delims=" %%L in ("%TMP%\audiotest.txt") do s10ediag mark "t: %%L"
s10ediag mark a: done
