@echo off
rem S10eAgent extra step (M16 hardware path H3/H4, notes/gpu-hw-path.md): measure the desktop in a GpuEnable 1 boot -
rem with DWM on whatever renderer this boot gave it - and set the renderer for the NEXT boot.
rem   - what DWM runs on: the machine GALLIUM_DRIVER of this boot (softpipe pin or none = Panfrost by Mesa 0020), the
rem     UMD modules DWM loaded, the KMD job counters before/after (DWM on Panfrost = jobs grow with no test running);
rem   - gfxperf 120 s in the user session (DWM fps, per-CPU load + clock, GPU engine use, top processes) with a 20 s
rem     d3dtri cube on Panfrost from t=40 s (a 3D app under the desktop compositor);
rem   - DWM crash events and a live minidump of dwm (stacks), Desktop Window Manager / Display events;
rem   - next boot: \S10EDIAG\gpu\softpipe.flg -> machine GALLIUM_DRIVER=softpipe (DWM on the CPU), absent -> deleted
rem     (DWM on the Mali). A new UMD in gpu\umd-new\ is swapped in first, as extra-gpu-h2.cmd does.
rem Output: out\last\h3\ and a numbered copy in out\h3-history\<n>\.
set X=%L%\h3
mkdir "%X%" > nul 2>&1
set T=C:\S10eTmp
if not exist %T% mkdir %T%
for %%F in (d3dtri.exe m5test.exe gfxperf.exe) do copy /y "%ESP%\S10EDIAG\gpu\%%F" %T%\%%F > nul 2>&1
for %%F in (run-as-user.ps1 livedump.ps1 dwm-sampler.ps1) do copy /y "%ESP%\S10EDIAG\%%F" %T%\%%F > nul 2>&1
if exist "%X%\umd-swap.txt" del /f /q "%X%\umd-swap.txt"
for %%U in (maliumd.dll libgallium_d3d10.dll) do if exist "%ESP%\S10EDIAG\gpu\umd-new\%%U" (
  if exist "%SystemRoot%\System32\%%U.old" del /f /q "%SystemRoot%\System32\%%U.old" >> "%X%\umd-swap.txt" 2>&1
  if exist "%SystemRoot%\System32\%%U" move /y "%SystemRoot%\System32\%%U" "%SystemRoot%\System32\%%U.old" >> "%X%\umd-swap.txt" 2>&1
  copy /y "%ESP%\S10EDIAG\gpu\umd-new\%%U" "%SystemRoot%\System32\%%U" >> "%X%\umd-swap.txt" 2>&1
  certutil -hashfile "%SystemRoot%\System32\%%U" SHA256 >> "%X%\umd-swap.txt" 2>&1
)
s10ediag mark x: h3 start
reg query HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters > "%X%\mode.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v GALLIUM_DRIVER >> "%X%\mode.txt" 2>&1
pnputil /enum-devices /instanceid "ACPI\SAMS9828\0" >> "%X%\mode.txt" 2>&1
for /f "usebackq tokens=1,2,3" %%A in (`findstr /i "GALLIUM_DRIVER GpuEnable" "%X%\mode.txt"`) do s10ediag mark "x: %%A %%C"
powershell -NoProfile -Command "Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { 'dwm pid ' + $_.Id + ' start ' + $_.StartTime + ' cpu ' + $_.CPU; $_.Modules | Where-Object { $_.ModuleName -match 'maliumd|gallium|d3d10warp|basicrender' } | ForEach-Object { '  ' + $_.FileName } }" > "%X%\dwm-modules.txt" 2>&1
%T%\m5test.exe > "%X%\m5test-0.txt" 2>&1
rem a crashing test leaves a WER minidump next to dwm/LogonUI's (C:\S10eTmp\dumps, armed by extra-gpu-mode.cmd)
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\d3dtri.exe" /v DumpFolder /t REG_EXPAND_SZ /d C:\S10eTmp\dumps /f > nul 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\d3dtri.exe" /v DumpType /t REG_DWORD /d 1 /f > nul 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\d3dtri.exe" /v DumpCount /t REG_DWORD /d 2 /f > nul 2>&1
rem the D3D10 sampling path (Mesa 0044: SAMPLE / SAMPLE_I / SVIEWINFO through tgsi_to_nir) by pixels, on Panfrost
set GALLIUM_DRIVER=panfrost
%T%\d3dtri.exe tex > "%X%\tex-s0.txt" 2>&1
call echo exit %%ERRORLEVEL%% >> "%X%\tex-s0.txt"
set GALLIUM_DRIVER=
for /f "usebackq delims=" %%L in (`findstr /c:"tex ->" /c:"WRONG" /c:"TIMEOUT" "%X%\tex-s0.txt"`) do s10ediag mark "x: %%L"

> %T%\h3-user.ps1 echo $g = Start-Process -FilePath C:\S10eTmp\gfxperf.exe -ArgumentList '120' -RedirectStandardOutput C:\S10eTmp\h3-gfxperf.txt -WindowStyle Hidden -PassThru
>> %T%\h3-user.ps1 echo Start-Sleep -Seconds 40
>> %T%\h3-user.ps1 echo $env:GALLIUM_DRIVER = 'panfrost'
>> %T%\h3-user.ps1 echo C:\S10eTmp\d3dtri.exe cube 20 ^> C:\S10eTmp\h3-cube.txt 2^>^&1
rem H4: a pure DWM composition load (GDI square moving every 33 ms) at t=70 s for 25 s, while dwm-sampler.ps1 (SYSTEM,
rem started below) takes 8 stack dumps of dwm 1 s apart from t=78 s
>> %T%\h3-user.ps1 echo Start-Sleep -Seconds 8
>> %T%\h3-user.ps1 echo C:\S10eTmp\d3dtri.exe gdi 25 ^> C:\S10eTmp\h3-gdi.txt 2^>^&1
>> %T%\h3-user.ps1 echo $g.WaitForExit(150000) ^| Out-Null
for %%F in (h3-gfxperf.txt h3-cube.txt h3-gdi.txt) do if exist %T%\%%F del /f /q %T%\%%F
if exist "%X%\dwmsamples" rmdir /s /q "%X%\dwmsamples"
start "dwm-sampler" /b cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File %T%\dwm-sampler.ps1 -Out %X%\dwmsamples -Delay 80 -Count 8 -Interval 1000 > %X%\dwm-sampler.txt 2>&1"
s10ediag mark x: gfxperf 120 s + cube at 40 s
powershell -NoProfile -ExecutionPolicy Bypass -File %T%\run-as-user.ps1 -Script %T%\h3-user.ps1 -Wait 200 > "%X%\user-task.txt" 2>&1
for %%F in (h3-gfxperf.txt h3-cube.txt h3-gdi.txt) do if exist %T%\%%F copy /y %T%\%%F "%X%\%%F" > nul
%T%\m5test.exe > "%X%\m5test-1.txt" 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File %T%\livedump.ps1 -Out "%X%\dump" -Names dwm > "%X%\livedump.txt" 2>&1
wevtutil qe Application /q:"*[System[(EventID=1000 or EventID=1002)]]" /c:10 /rd:true /f:text > "%X%\app-crashes.txt" 2>&1
wevtutil qe Application /q:"*[System[Provider[@Name='Desktop Window Manager']]]" /c:20 /rd:true /f:text > "%X%\ev-dwm.txt" 2>&1
wevtutil qe System /q:"*[System[Provider[@Name='Display']]]" /c:20 /rd:true /f:text > "%X%\ev-display.txt" 2>&1
rem Mesa 0050 timing logs (one per process with the UMD loaded, DWM's included) from MALIKM_TGSI_DIR; the ones no
rem process holds open any more (earlier boots, exited tests) are deleted after the copy
mkdir "%X%\tlog" > nul 2>&1
copy /y C:\S10eTmp\tgsi\malikm-*.txt "%X%\tlog\" > nul 2>&1
del /q C:\S10eTmp\tgsi\malikm-*.txt > nul 2>&1
rem H4 zero-copy scan-out research (READ ONLY): the DPU_DMA channels G0..VGRF at 0x19071000 + n*0x1000 (Samsung
rem kernel cal_9820/regs-dpp.h: IDMA_ENABLE +0x0, IN_CON +0x8, SRC_SIZE +0x10 (width 15:0, height 29:16), SRC_OFFSET
rem +0x14, IMG_SIZE +0x18, IN_BASE_ADDR_Y +0x40) - which channel scans 0xCA000000, with which source width
for %%C in (1 2 3 4 5 6) do s10ediag peek 0x1907%%C000 0x60 >> "%X%\dpu-dma-peek.txt" 2>&1
s10ediag peek 0x8FE04000 0x4000 > "%X%\maliwddm-pram-peek.txt" 2>&1
reg export "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9828\0\Device Parameters\MaliG76Wddm" "%X%\trace.reg" /y > nul 2>&1

rem summary: DWM fps median/max, mean CPU, top process (as extra-gfxperf.cmd), the cube's total, KMD job counters
powershell -NoProfile -Command "$l = Get-Content '%X%\h3-gfxperf.txt' -ErrorAction SilentlyContinue; $f = @($l | %% { if ($_ -match 'dwm +([0-9.]+) fps') { [double]$matches[1] } } | Sort-Object); $c = @($l | %% { if ($_ -match 'cpu +([0-9.]+)%%') { [double]$matches[1] } }); $t = @($l | %% { if ($_ -match ' top (\S+)=') { $matches[1] } } | Group-Object | Sort-Object Count -Descending); 'samples {0} dwm fps median {1} max {2} cpu mean {3:N0}%% top {4}' -f $f.Count, ($(if ($f.Count) { $f[[int]($f.Count / 2)] } else { 'n/a' })), ($(if ($f.Count) { $f[-1] } else { 'n/a' })), ($(if ($c.Count) { ($c | Measure-Object -Average).Average } else { 0 })), ($(if ($t.Count) { $t[0].Name } else { 'n/a' }))" > "%X%\summary.txt" 2>&1
findstr /c:"cube:" /c:"TIMEOUT" "%X%\h3-cube.txt" >> "%X%\summary.txt" 2>nul
for %%F in (m5test-0.txt m5test-1.txt) do for /f "usebackq delims=" %%L in (`findstr /c:"jobs done" "%X%\%%F"`) do echo %%F %%L>> "%X%\summary.txt"
for /f "usebackq delims=" %%L in ("%X%\summary.txt") do s10ediag mark "x: %%L"

rem renderer for the NEXT boot
if exist "%ESP%\S10EDIAG\gpu\softpipe.flg" (
  reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v GALLIUM_DRIVER /t REG_SZ /d softpipe /f > "%X%\next-boot.txt" 2>&1
  s10ediag mark x: next boot DWM softpipe
) else (
  reg delete "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v GALLIUM_DRIVER /f > "%X%\next-boot.txt" 2>&1
  s10ediag mark x: next boot DWM panfrost
)
set HH=%ESP%\S10EDIAG\out\h3-history
mkdir "%HH%" > nul 2>&1
set N=
for /l %%I in (1,1,999) do if not defined N if not exist "%HH%\%%I\" set N=%%I
mkdir "%HH%\%N%" > nul 2>&1
copy /y "%X%\*.*" "%HH%\%N%\" > nul 2>&1
s10ediag mark "x: done, history %N%"
s10ediag flush > nul 2>&1
