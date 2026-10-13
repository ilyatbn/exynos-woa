@echo off
rem S10eAgent extra step (M16 hardware path H2, notes/gpu-hw-path.md): Panfrost on the real Mali, one process at a time,
rem in a boot whose mode is already GpuEnable 1 (the registry keeps it; DWM pinned to softpipe by the machine
rem GALLIUM_DRIVER). d3dtri (gpu\d3dtri.exe) "tri" = offscreen triangle + pixel check; "cube" = spinning cube with fps.
rem   1. session 0: tri with GALLIUM_DRIVER=softpipe (control: the program and the adapter work) and =panfrost
rem      (MALIKM_TRACE=1: the shim's trace on stderr), each followed by the KMD's job/fault counters (m5test prints them
rem      before its own job).
rem   2. the user's session (run-as-user.ps1): tri + cube 20 s with panfrost (shows on the panel, fps per second).
rem   3. MaliG76Wddm's PRAM trace + trace registry, marks of the key lines.
rem Output: out\last\h2\ and a numbered copy in out\h2-history\<n>\. Changes nothing persistent.
set H=%L%\h2
mkdir "%H%" > nul 2>&1
set T=C:\S10eTmp
if not exist %T% mkdir %T%
for %%F in (d3dtri.exe m5test.exe) do copy /y "%ESP%\S10EDIAG\gpu\%%F" %T%\%%F > nul 2>&1
copy /y "%ESP%\S10EDIAG\run-as-user.ps1" %T%\run-as-user.ps1 > nul 2>&1
reg query HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters > "%H%\mode.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v GALLIUM_DRIVER >> "%H%\mode.txt" 2>&1
pnputil /enum-devices /instanceid "ACPI\SAMS9828\0" >> "%H%\mode.txt" 2>&1
s10ediag mark h: h2 start
%T%\m5test.exe > "%H%\m5test-0.txt" 2>&1

s10ediag mark h: tri softpipe s0
set GALLIUM_DRIVER=softpipe
%T%\d3dtri.exe tri > "%H%\tri-s0-softpipe.txt" 2>&1
call echo exit %%ERRORLEVEL%% >> "%H%\tri-s0-softpipe.txt"

s10ediag mark h: tri panfrost s0
set GALLIUM_DRIVER=panfrost
set MALIKM_TRACE=1
%T%\d3dtri.exe tri > "%H%\tri-s0-panfrost.txt" 2> "%H%\tri-s0-panfrost-stderr.txt"
call echo exit %%ERRORLEVEL%% >> "%H%\tri-s0-panfrost.txt"
set MALIKM_TRACE=
set GALLIUM_DRIVER=
%T%\m5test.exe > "%H%\m5test-1.txt" 2>&1

s10ediag mark h: user session panfrost tri + cube
> %T%\h2-user.ps1 echo $env:GALLIUM_DRIVER = 'panfrost'
>> %T%\h2-user.ps1 echo C:\S10eTmp\d3dtri.exe tri ^> C:\S10eTmp\h2-tri-user.txt 2^>^&1
>> %T%\h2-user.ps1 echo C:\S10eTmp\d3dtri.exe cube 20 ^> C:\S10eTmp\h2-cube-user.txt 2^>^&1
for %%F in (h2-tri-user.txt h2-cube-user.txt) do if exist %T%\%%F del /f /q %T%\%%F
powershell -NoProfile -ExecutionPolicy Bypass -File %T%\run-as-user.ps1 -Script %T%\h2-user.ps1 -Wait 240 > "%H%\user-task.txt" 2>&1
for %%F in (h2-tri-user.txt h2-cube-user.txt) do if exist %T%\%%F copy /y %T%\%%F "%H%\%%F" > nul
%T%\m5test.exe > "%H%\m5test-2.txt" 2>&1

s10ediag peek 0x8FE04000 0x4000 > "%H%\maliwddm-pram-peek.txt" 2>&1
reg export "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9828\0\Device Parameters\MaliG76Wddm" "%H%\trace.reg" /y > nul 2>&1
wevtutil qe System /q:"*[System[Provider[@Name='Display']]]" /c:20 /rd:true /f:text > "%H%\ev-display.txt" 2>&1
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /c:10 /rd:true /f:text > "%H%\app-crashes.txt" 2>&1
for %%F in (tri-s0-softpipe.txt tri-s0-panfrost.txt h2-tri-user.txt) do for /f "usebackq delims=" %%L in (`findstr /i "PASS FAIL TIMEOUT pixels" "%H%\%%F"`) do s10ediag mark "h: %%F %%L"
for /f "usebackq delims=" %%L in (`findstr /i "cube: TIMEOUT first" "%H%\h2-cube-user.txt"`) do s10ediag mark "h: cube %%L"
for %%F in (m5test-1.txt m5test-2.txt) do for /f "usebackq delims=" %%L in (`findstr /c:"jobs done" "%H%\%%F"`) do s10ediag mark "h: %%F %%L"
rem numbered history copy (first free number, as extra-gfxperf.cmd)
set HH=%ESP%\S10EDIAG\out\h2-history
mkdir "%HH%" > nul 2>&1
set N=
for /l %%I in (1,1,999) do if not defined N if not exist "%HH%\%%I\" set N=%%I
mkdir "%HH%\%N%" > nul 2>&1
copy /y "%H%\*.*" "%HH%\%N%\" > nul 2>&1
s10ediag mark "h: done, history %N%"
s10ediag flush > nul 2>&1
