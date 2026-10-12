@echo off
rem S10eAgent extra step (M16 G9b, notes/gpu-m16-g9b.md): first GPU job from Windows. Needs a BEYOND0LTE_GPU_POWER_ON 1
rem image (boot #106) so ACPI GPU0 (SAMS9828) exists. Installs MaliG76Probe (WRITE_VALUE job on JS0 through the G76
rem MMU) and collects its run log; expected "RESULT: PASS (the G76 wrote the value)". Rescue for an IRQ-storm hang:
rem flash boot #107 (GPU0 hidden -> the driver never loads). Package at %ESP%\S10EDIAG\drivers\MaliG76Probe\.
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
set G=%L%\gpu
mkdir "%G%"
s10ediag mark g: status word
s10ediag peek 0x8FE0F010 > "%G%\gpu-status-word.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\SAMS9828" /properties > "%G%\dev-before.txt" 2>&1
set D=%TMP%\s10e-gpu
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%ESP%\S10EDIAG\drivers\MaliG76Probe" "%D%" > "%G%\xcopy.txt" 2>&1
s10ediag mark g: pnputil add MaliG76Probe
pnputil /add-driver "%D%\MaliG76Probe.inf" /install > "%G%\pnputil-add.txt" 2>&1
s10ediag mark g: installed, wait 20 s
ping -n 20 127.0.0.1 > nul
pnputil /enum-devices /deviceid "ACPI\SAMS9828" /properties > "%G%\dev-after.txt" 2>&1
copy /y "%SystemRoot%\System32\LogFiles\MaliG76Probe-run.txt" "%G%\run.txt" > nul 2>&1
for /f "usebackq delims=" %%L in ("%G%\run.txt") do s10ediag mark "g: %%L"
s10ediag mark g: done
