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
if exist "%ESP%\S10EDIAG\gpu\MaliG76Wddm\MaliG76Wddm.inf" pnputil /add-driver "%ESP%\S10EDIAG\gpu\MaliG76Wddm\MaliG76Wddm.inf" /install > "%G%\pnputil-stage.txt" 2>&1
s10ediag mark "g: set mode full %FULL% gpu %GPU% (GPU0 hidden this boot)"
reg add "%KEY%" /v GpuEnable /t REG_DWORD /d %GPU% /f > "%G%\set-mode.txt" 2>&1
reg add "%KEY%" /v FullAdapter /t REG_DWORD /d %FULL% /f >> "%G%\set-mode.txt" 2>&1
rem The full adapter's PreemptCommand never reports DMA_PREEMPTED (notes/gpu-full-adapter-hang.md suspect 5): turn
rem preemption off BEFORE the first full-mode boot (extra-gpu-g10.cmd only did it one boot later).
set /a ANYFULL=%GPU%+%FULL%
if %ANYFULL% GTR 0 reg add "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Scheduler" /v EnablePreemption /t REG_DWORD /d 0 /f >> "%G%\set-mode.txt" 2>&1
if %ANYFULL% EQU 0 reg delete "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Scheduler" /v EnablePreemption /f >> "%G%\set-mode.txt" 2>&1
if exist "%ESP%\S10EDIAG\nogpu.flg" del /f /q "%ESP%\S10EDIAG\nogpu.flg"
echo pass2 > C:\S10eTmp\gpumode2.flg
s10ediag mark g: clean restart into the new mode
shutdown /r /t 0 /f
ping -n 120 127.0.0.1 > nul
goto end
:collect
del /f /q C:\S10eTmp\gpumode2.flg
s10ediag mark g: pass 2 - collect
rem MaliG76Wddm's PRAM DDI trace (WINPRAM + 0x4000, 16 KiB, drivers/MaliG76Wddm/pram.c) of THIS boot, live.
s10ediag dumpram 0x8FE04000 0x4000 "%G%\maliwddm-pram.bin" > "%G%\maliwddm-pram.txt" 2>&1
s10ediag peek 0x8FE04000 0x4000 > "%G%\maliwddm-pram-peek.txt" 2>&1
call "%ESP%\S10EDIAG\gpu\extra-gpu-g10.cmd"
for %%R in (trace) do if exist "%G%\%%R.reg" for /f "usebackq delims=" %%L in (`findstr /i "Mode Started Fail Seen_Create GpuQuieted ResContract Presents" "%G%\%%R.reg"`) do s10ediag mark "t: %%L"
:end
s10ediag mark g: done
