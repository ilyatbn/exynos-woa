@echo off
rem S10eAgent extra step (M16 G10 bisect): set the MaliG76Wddm mode for the next boot WHILE GPU0 IS HIDDEN (nogpu.flg
rem on the ESP -> the driver is not loaded, so a hanging mode cannot run), then remove nogpu.flg and restart Windows
rem cleanly; the second pass (marker C:\S10eTmp\gpumode2.flg) collects the driver state via gpu\extra-gpu-g10.cmd.
rem Mode from the ESP: \S10EDIAG\gpu\full.flg -> FullAdapter 1 GpuEnable 0; gpu.flg -> GpuEnable 1; neither -> 0/0.
set G=%L%\gpu
mkdir "%G%" > nul 2>&1
set KEY=HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters
if exist C:\S10eTmp\gpumode2.flg goto collect
set FULL=0
set GPU=0
if exist "%ESP%\S10EDIAG\gpu\full.flg" set FULL=1
if exist "%ESP%\S10EDIAG\gpu\gpu.flg" set GPU=1
rem Stage the MaliG76Wddm package on the ESP into the driver store while GPU0 is hidden (no live driver swap); PnP
rem picks the higher DriverVer when GPU0 is enumerated on the next boot. BEFORE the mode is written: the INF's
rem MW_Parameters AddReg resets GpuEnable/FullAdapter to 0 on every install (run g13full ran display-only because the
rem install came after the reg add).
rem Opt-in only (\S10EDIAG\gpu\stage.flg): run g13fullb showed the staged install completes when GPU0 reappears on the
rem next boot and resets the mode to 0/0 again, so a mode test must not stage a package.
if exist "%ESP%\S10EDIAG\gpu\stage.flg" if exist "%ESP%\S10EDIAG\gpu\MaliG76Wddm\MaliG76Wddm.inf" pnputil /add-driver "%ESP%\S10EDIAG\gpu\MaliG76Wddm\MaliG76Wddm.inf" /install > "%G%\pnputil-stage.txt" 2>&1
s10ediag mark "g: set mode full %FULL% gpu %GPU% (GPU0 hidden this boot)"
reg add "%KEY%" /v GpuEnable /t REG_DWORD /d %GPU% /f > "%G%\set-mode.txt" 2>&1
reg add "%KEY%" /v FullAdapter /t REG_DWORD /d %FULL% /f >> "%G%\set-mode.txt" 2>&1
rem The full adapter's PreemptCommand never reports DMA_PREEMPTED (notes/gpu-full-adapter-hang.md suspect 5): turn
rem preemption off BEFORE the first full-mode boot (extra-gpu-g10.cmd only did it one boot later).
set /a ANYFULL=%GPU%+%FULL%
if %ANYFULL% GTR 0 reg add "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Scheduler" /v EnablePreemption /t REG_DWORD /d 0 /f >> "%G%\set-mode.txt" 2>&1
if %ANYFULL% EQU 0 reg delete "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Scheduler" /v EnablePreemption /f >> "%G%\set-mode.txt" 2>&1
rem G13: boot-time ETW of Microsoft-Windows-DxgKrnl (its event 494 carries dxgkrnl's own diagnostic text, the way the
rem original project found its VidPN/start failures - wddm-mali-g52 docs/results/m5.2-pe-run*-dxgkrnl-etw.txt) for the
rem full-mode boot: why dxgkrnl creates the primary, destroys it and commits a VidPN with 0 paths (run g17full).
rem An AutoLogger session (starts at the next boot), stopped and decoded in pass 2. Full modes only.
if %ANYFULL% GTR 0 logman delete "autosession\MaliDxgk" > nul 2>&1
if %ANYFULL% GTR 0 logman create trace "autosession\MaliDxgk" -p {802EC45A-1E99-4B83-9920-87C98277BA9D} 0xffffffffffffffff 5 -o C:\S10eTmp\dxgk.etl -bs 1024 -nb 16 256 -max 32 -mode Sequential > "%G%\etw-create.txt" 2>&1
if exist "%ESP%\S10EDIAG\nogpu.flg" del /f /q "%ESP%\S10EDIAG\nogpu.flg"
echo pass2 > C:\S10eTmp\gpumode2.flg
s10ediag mark g: clean restart into the new mode
shutdown /r /t 0 /f
rem Never return to collect.cmd while Windows shuts down: a killed ping let collect.cmd write torecovery.flg
rem before the restart, so the firmware went to TWRP instead of pass 2 (run g15full).
:holdrestart
ping -n 30 127.0.0.1 > nul
goto holdrestart
goto end
:collect
del /f /q C:\S10eTmp\gpumode2.flg
s10ediag mark g: pass 2 - collect
rem MaliG76Wddm's PRAM DDI trace (WINPRAM + 0x4000, 16 KiB, drivers/MaliG76Wddm/pram.c) of THIS boot, live (peek only: dumpram uses MmCopyMemory, which failed with 59 on this RAM in run g13fullc).
s10ediag peek 0x8FE04000 0x4000 > "%G%\maliwddm-pram-peek.txt" 2>&1
rem The boot ETW (pass 1): stop, remove the AutoLogger, decode; dxgkrnl's diagnostic lines (event 494
rem EventDescription + data, and every event carrying a Status) into dxgk-msgs.txt, the raw .etl kept too.
s10ediag mark g: etw stop
logman stop MaliDxgk -ets > "%G%\etw-stop.txt" 2>&1
logman delete "autosession\MaliDxgk" >> "%G%\etw-stop.txt" 2>&1
if exist C:\S10eTmp\dxgk.etl copy /y C:\S10eTmp\dxgk.etl "%G%\dxgk.etl" > nul 2>&1
if exist C:\S10eTmp\dxgk.etl tracerpt C:\S10eTmp\dxgk.etl -o "%G%\dxgk.xml" -of XML -y > "%G%\etw-tracerpt.txt" 2>&1
if exist C:\S10eTmp\dxgk.etl tracerpt C:\S10eTmp\dxgk.etl -o "%G%\dxgk.csv" -of CSV -y >> "%G%\etw-tracerpt.txt" 2>&1
if exist "%G%\dxgk.csv" findstr /r /c:"DxgKrnl, *DCStart *, *494," "%G%\dxgk.csv" > "%G%\dxgk-msgs.txt" 2>&1
if exist C:\S10eTmp\dxgk.etl del /f /q C:\S10eTmp\dxgk.etl
rem G13: can the D3D runtimes create a device on the Mali adapter (DWM needs D3D11; maliumd.dll is Mesa d3d10umd
rem built D3D10.0-only)? d3dprobe prints per adapter the UMD names for DX9/10/11, the WDDM version and the HRESULT of
rem D3D11CreateDevice / D3D10CreateDevice; MALIKM_TRACE=1 adds the Mesa shim's trace (stderr) from inside the probe.
s10ediag mark g: d3dprobe
set MALIKM_TRACE=1
if exist "%ESP%\S10EDIAG\gpu\d3dprobe.exe" "%ESP%\S10EDIAG\gpu\d3dprobe.exe" > "%G%\d3dprobe.txt" 2> "%G%\d3dprobe-stderr.txt"
set MALIKM_TRACE=
for /f "usebackq delims=" %%L in (`findstr /i "adapter D3D11CreateDevice D3D10CreateDevice UMD WDDM" "%G%\d3dprobe.txt"`) do s10ediag mark "d: %%L"
s10ediag mark g: dxdiag
dxdiag /whql:off /t "%G%\dxdiag.txt"
ping -n 46 127.0.0.1 > nul
for /f "usebackq delims=" %%L in (`findstr /i /c:"Card name" /c:"DDI Version" /c:"Feature Levels" /c:"Driver Model" /c:"User Mode Driver" "%G%\dxdiag.txt"`) do s10ediag mark "x: %%L"
call "%ESP%\S10EDIAG\gpu\extra-gpu-g10.cmd"
for %%R in (trace) do if exist "%G%\%%R.reg" for /f "usebackq delims=" %%L in (`findstr /i "Mode Started Fail Seen_Create GpuQuieted ResContract Presents" "%G%\%%R.reg"`) do s10ediag mark "t: %%L"
:end
s10ediag mark g: done
