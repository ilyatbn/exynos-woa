@echo off
rem S10eAgent extra step (M16 G13, notes/gpu-full-adapter-hang.md): measure the desktop's frame rate and CPU cost in
rem the mode the phone booted in, WITHOUT restarting Windows (for interactive test boots with stay.flg). gfxperf runs
rem 150 s in the user's session (gfx-user.ps1); \S10EDIAG\gpu\bench.flg adds a d3dfill present loop at t=60 s.
rem Output: out\last\gfx\ and a numbered copy per boot in \S10EDIAG\out\gfx-history\ (kept across boots).
set X=%L%\gfx
mkdir "%X%" > nul 2>&1
s10ediag mark g: gfxperf
reg query HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters > "%X%\mode.txt" 2>&1
pnputil /enum-devices /instanceid "ACPI\SAMS9828\0" >> "%X%\mode.txt" 2>&1
for /f "usebackq tokens=1,2,3" %%A in (`findstr /i "GpuEnable FullAdapter" "%X%\mode.txt"`) do s10ediag mark "x: %%A %%C"
for /f "usebackq tokens=1,*" %%A in (`findstr /i /c:"Status:" "%X%\mode.txt"`) do s10ediag mark "x: GPU0 %%B"
copy /y "%ESP%\S10EDIAG\gpu\gfxperf.exe" C:\S10eTmp\gfxperf.exe > nul 2>&1
copy /y "%ESP%\S10EDIAG\gpu\d3dfill.exe" C:\S10eTmp\d3dfill.exe > nul 2>&1
copy /y "%ESP%\S10EDIAG\gfx-user.ps1" C:\S10eTmp\gfx-user.ps1 > nul 2>&1
copy /y "%ESP%\S10EDIAG\run-as-user.ps1" C:\S10eTmp\run-as-user.ps1 > nul 2>&1
if exist C:\S10eTmp\gfxbench.flg del /f /q C:\S10eTmp\gfxbench.flg
if exist "%ESP%\S10EDIAG\gpu\bench.flg" echo bench > C:\S10eTmp\gfxbench.flg
for %%F in (gfxperf.txt d3dfill-bench.txt) do if exist C:\S10eTmp\%%F del /f /q C:\S10eTmp\%%F
powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\run-as-user.ps1 -Script C:\S10eTmp\gfx-user.ps1 -Wait 200 > "%X%\user-task.txt" 2>&1
for %%F in (gfxperf.txt d3dfill-bench.txt) do if exist C:\S10eTmp\%%F copy /y C:\S10eTmp\%%F "%X%\%%F" > nul
rem Summary: median DWM fps, mean total CPU, the busiest process most often on top.
powershell -NoProfile -Command "$l = Get-Content '%X%\gfxperf.txt' -ErrorAction SilentlyContinue; $f = @($l | %% { if ($_ -match 'dwm +([0-9.]+) fps') { [double]$matches[1] } } | Sort-Object); $c = @($l | %% { if ($_ -match 'cpu +([0-9.]+)%%') { [double]$matches[1] } }); $t = @($l | %% { if ($_ -match ' top (\S+)=') { $matches[1] } } | Group-Object | Sort-Object Count -Descending); 'samples {0} dwm fps median {1} max {2} cpu mean {3:N0}%% top {4}' -f $f.Count, ($(if ($f.Count) { $f[[int]($f.Count / 2)] } else { 'n/a' })), ($(if ($f.Count) { $f[-1] } else { 'n/a' })), ($(if ($c.Count) { ($c | Measure-Object -Average).Average } else { 0 })), ($(if ($t.Count) { $t[0].Name } else { 'n/a' }))" > "%X%\summary.txt" 2>&1
for /f "usebackq delims=" %%L in ("%X%\summary.txt") do s10ediag mark "x: %%L"
for /f "usebackq delims=" %%L in (`findstr /i /c:"frames," "%X%\d3dfill-bench.txt"`) do s10ediag mark "f: %%L"
rem numbered history copy (out\last is replaced every boot)
set H=%ESP%\S10EDIAG\out\gfx-history
mkdir "%H%" > nul 2>&1
rem first free number (no counter file: "echo %N%> f" with a 1-digit N is a 1> redirect and lost the count)
set N=
for /l %%I in (1,1,999) do if not defined N if not exist "%H%\%%I\" set N=%%I
mkdir "%H%\%N%" > nul 2>&1
copy /y "%X%\*.txt" "%H%\%N%\" > nul 2>&1
s10ediag mark "x: history %N%"
s10ediag flush > nul 2>&1
