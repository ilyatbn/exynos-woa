@echo off
rem S10eAgent extra step (D8): replace a boot-crashing Exynos9820Audio without ever loading it.
rem Pass 1 (\S10EDIAG\noaudio.flg present -> AUD0 hidden, no audio driver loaded): pnputil /add-driver the new package
rem   from %ESP%\S10EDIAG\drivers\Exynos9820Audio\ into the store, delete noaudio.flg, clean restart.
rem Pass 2 (marker C:\S10eTmp\audswap.flg): AUD0 present -> PnP binds the best-ranked (newest) package at boot; collect
rem   the device state, run the WASAPI/volume-key test in the user's session and copy the driver's run log.
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
set W=%L%\audio
mkdir "%W%"
if exist C:\S10eTmp\audswap.flg goto pass2
s10ediag mark a: swap pass 1 (AUD0 hidden)
set D=%TMP%\s10e-audio
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%ESP%\S10EDIAG\drivers\Exynos9820Audio" "%D%" > "%W%\xcopy.txt" 2>&1
rem A device keeps its installed package even when a newer one is staged (run audA6s: 1.0.1.4 still loaded), so remove
rem every existing exynos9820audio.inf package first (the device is hidden, nothing is loaded).
pnputil /enum-drivers > "%W%\enum-before.txt" 2>&1
powershell -NoProfile -Command "$t = Get-Content '%W%\enum-before.txt' -Raw; foreach ($x in ($t -split '(?=Published Name:)')) { if ($x -match 'exynos9820audio.inf' -and $x -match 'Published Name:\s+(oem\d+\.inf)') { $Matches[1] } }" > "%W%\old-oem.txt" 2>&1
for /f "usebackq delims=" %%O in ("%W%\old-oem.txt") do (
  s10ediag mark "a: delete old package %%O"
  pnputil /delete-driver %%O /uninstall /force >> "%W%\pnputil-delete.txt" 2>&1
)
pnputil /add-driver "%D%\Exynos9820Audio.inf" > "%W%\pnputil-add.txt" 2>&1
rem Stage Parameters AFTER the delete/add: deleting the old package removed earlier values, and the INF AddReg defaults
rem are AmpInit/AmpEnable/AboxEnable 1 (run audA7 ran with the amps enabled). NOCLOBBER keeps these values at install.
if exist "%ESP%\S10EDIAG\audio-stage.cmd" call "%ESP%\S10EDIAG\audio-stage.cmd"
pnputil /enum-drivers > "%W%\enum-drivers.txt" 2>&1
if exist "%ESP%\S10EDIAG\noaudio.flg" del /f /q "%ESP%\S10EDIAG\noaudio.flg"
echo pass2 > C:\S10eTmp\audswap.flg
s10ediag mark a: clean restart with AUD0 visible
shutdown /r /t 0 /f
ping -n 120 127.0.0.1 > nul
goto end
:pass2
del /f /q C:\S10eTmp\audswap.flg
s10ediag mark a: swap pass 2
pnputil /enum-devices /deviceid "ACPI\SAMS9827" /drivers /properties > "%W%\dev-audio.txt" 2>&1
pnputil /enum-devices /class AudioEndpoint > "%W%\endpoints.txt" 2>&1
copy /y "%ESP%\S10EDIAG\audio-test.ps1" "%TMP%\audio-test.ps1" > nul
copy /y "%ESP%\S10EDIAG\run-as-user.ps1" "%TMP%\run-as-user.ps1" > nul
del /q "%TMP%\audiotest.txt" > nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File "%TMP%\run-as-user.ps1" -Script C:\S10eTmp\audio-test.ps1 -Wait 60 > "%W%\task.txt" 2>&1
copy /y "%TMP%\audiotest.txt" "%W%\audiotest.txt" > nul 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Audio-run.txt" "%W%\run.txt" > nul 2>&1
for /f "usebackq delims=" %%L in ("%TMP%\audiotest.txt") do s10ediag mark "t: %%L"
:end
s10ediag mark a: done
