@echo off
rem S10eAgent extra step (M16 H5, notes/gpu-hw-path.md): a stability soak of the GPU desktop - the d3dtri cube for
rem 300 s (blt model) then 120 s (flip model) in the user session, with the KMD job/fault counters before and after,
rem the scan-out dumped every ~100 s (s10ediag dumpmmio, decoded on the PC by tools/phone/fb2png.py), the Mesa tlogs
rem and the crash events. Keeps the H4b re-stride armed (gpu\restride.flg) like extra-gpu-h3.cmd.
rem Output: out\last\soak\ (+ out\soak-fb-<n>.bin), numbered copy in out\soak-history\<n>\.
set X=%L%\soak
mkdir "%X%" > nul 2>&1
set T=C:\S10eTmp
if not exist %T% mkdir %T%
for %%F in (d3dtri.exe m5test.exe) do copy /y "%ESP%\S10EDIAG\gpu\%%F" %T%\%%F > nul 2>&1
copy /y "%ESP%\S10EDIAG\gpu\s10ediag.exe" %T%\s10ediagx.exe > nul 2>&1
copy /y "%ESP%\S10EDIAG\run-as-user.ps1" %T%\run-as-user.ps1 > nul 2>&1
s10ediag mark x: soak start
reg query HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters > "%X%\mode.txt" 2>&1
%T%\m5test.exe > "%X%\m5test-0.txt" 2>&1
for %%F in (soak-cube.txt soak-flip.txt) do if exist %T%\%%F del /f /q %T%\%%F
> %T%\soak-user.ps1 echo $env:GALLIUM_DRIVER = 'panfrost'
>> %T%\soak-user.ps1 echo C:\S10eTmp\d3dtri.exe cube 300 ^> C:\S10eTmp\soak-cube.txt 2^>^&1
>> %T%\soak-user.ps1 echo $env:D3DTRI_FLIP = '1'
>> %T%\soak-user.ps1 echo C:\S10eTmp\d3dtri.exe cube 120 ^> C:\S10eTmp\soak-flip.txt 2^>^&1
del /q "%ESP%\S10EDIAG\out\soak-fb-*.bin" > nul 2>&1
start "fb-dump" /b cmd /c "for %%N in (1 2 3 4) do (timeout /t 100 /nobreak > nul & %T%\s10ediagx.exe dumpmmio 0xCA000000 9922560 %ESP%\S10EDIAG\out\soak-fb-%%N.bin >> %X%\fbdump.txt 2>&1 & %T%\m5test.exe > %X%\m5test-t%%N.txt 2>&1)"
s10ediag mark x: soak cube 300 s + flip 120 s
powershell -NoProfile -ExecutionPolicy Bypass -File %T%\run-as-user.ps1 -Script %T%\soak-user.ps1 -Wait 520 > "%X%\user-task.txt" 2>&1
for %%F in (soak-cube.txt soak-flip.txt) do if exist %T%\%%F copy /y %T%\%%F "%X%\%%F" > nul
%T%\m5test.exe > "%X%\m5test-1.txt" 2>&1
wevtutil qe Application /q:"*[System[(EventID=1000 or EventID=1002)]]" /c:10 /rd:true /f:text > "%X%\app-crashes.txt" 2>&1
wevtutil qe System /q:"*[System[Provider[@Name='Display'] or (EventID=4101)]]" /c:20 /rd:true /f:text > "%X%\ev-display.txt" 2>&1
mkdir "%X%\tlog" > nul 2>&1
copy /y C:\S10eTmp\tgsi\malikm-*.txt "%X%\tlog\" > nul 2>&1
del /q C:\S10eTmp\tgsi\malikm-*.txt > nul 2>&1
reg export "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9828\0\Device Parameters\MaliG76Wddm" "%X%\trace.reg" /y > nul 2>&1
for %%F in (soak-cube.txt soak-flip.txt) do for /f "usebackq delims=" %%L in (`findstr /c:"cube:" /c:"TIMEOUT" "%X%\%%F"`) do s10ediag mark "x: %%F %%L"
for %%F in (m5test-0.txt m5test-1.txt) do for /f "usebackq delims=" %%L in (`findstr /c:"jobs done" "%X%\%%F"`) do s10ediag mark "x: %%F %%L"
rem keep the re-stride for the next boot, as extra-gpu-h3.cmd
if exist "%ESP%\S10EDIAG\gpu\restride.flg" (
  reg add HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters /v ScanoutStride /t REG_DWORD /d 1088 /f > "%X%\next-stride.txt" 2>&1
) else (
  reg delete HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters /v ScanoutStride /f > "%X%\next-stride.txt" 2>&1
)
set HH=%ESP%\S10EDIAG\out\soak-history
mkdir "%HH%" > nul 2>&1
set N=
for /l %%I in (1,1,999) do if not defined N if not exist "%HH%\%%I\" set N=%%I
mkdir "%HH%\%N%" > nul 2>&1
copy /y "%X%\*.*" "%HH%\%N%\" > nul 2>&1
s10ediag mark "x: soak done, history %N%"
s10ediag flush > nul 2>&1
