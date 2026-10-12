@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd, or `call` it from there): M16 G10/G11 MaliG76Wddm.
rem notes/gpu-m16-g10.md "Validation procedure". Called from collect.cmd with ESP / OUT / L set.
rem ESP layout: \S10EDIAG\gpu\MaliG76Wddm\{MaliG76Wddm.sys,MaliG76Wddm.inf,malig76wddm.cat}, \S10EDIAG\gpu\m5test.exe,
rem   \S10EDIAG\gpu\maliumd.dll (G12 Mesa build; needed by the full-adapter modes), \S10EDIAG\drivers\wdktestcert.cer.
rem Mode for the NEXT boot (the driver reads Services\MaliG76Wddm\Parameters once, at load):
rem   neither flag             GpuEnable 0 FullAdapter 0  display-only (default)
rem   \S10EDIAG\gpu\full.flg    FullAdapter 1              full WDDM adapter, GPU half off
rem   \S10EDIAG\gpu\gpu.flg     GpuEnable 1                full adapter + GPU half; m5test runs when that mode is live
rem Rescue (no Windows needed): \S10EDIAG\nogpu.flg on the ESP hides GPU0 -> the driver never loads.
rem SPDX-License-Identifier: GPL-2.0
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
if not exist "%TMP%" mkdir "%TMP%"
set G=%L%\gpu
mkdir "%G%" > nul 2>&1
set SRC=%ESP%\S10EDIAG\gpu
set D=%TMP%\s10e-gpu
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%SRC%" "%D%" > "%G%\xcopy.txt" 2>&1
set KEY=HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters
set DEV=HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9828\0

echo --- state of this boot
s10ediag mark g: state
s10ediag peek 0x8FE0F010 > "%G%\gpu0-word.txt" 2>&1
pnputil /enum-devices /instanceid "ACPI\SAMS9828\0" /drivers > "%G%\pnp-gpu0.txt" 2>&1
pnputil /enum-devices /class Display /drivers > "%G%\pnp-display.txt" 2>&1
sc query MaliG76Wddm > "%G%\sc.txt" 2>&1
reg query "%KEY%" > "%G%\parameters-this-boot.txt" 2>&1
reg export "%DEV%\Device Parameters\MaliG76Wddm" "%G%\trace.reg" /y > nul 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Scheduler" /v EnablePreemption > "%G%\preemption.txt" 2>&1
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Format-List Name,Status,DriverVersion,CurrentHorizontalResolution,CurrentVerticalResolution,InstalledDisplayDrivers" > "%G%\video.txt" 2>&1
for %%F in ("%D%\MaliG76Wddm\*" "%D%\m5test.exe" "%D%\maliumd.dll") do if exist "%%~F" certutil -hashfile "%%~F" SHA256 >> "%G%\hashes.txt" 2>&1

echo --- maliumd.dll (UMD named by the INF) into System32 when the ESP carries it
if exist "%D%\maliumd.dll" copy /y "%D%\maliumd.dll" "%SystemRoot%\System32\maliumd.dll" > "%G%\umd-copy.txt" 2>&1

echo --- install once (the device then starts at once: display-only replaces Basic Display live)
sc query MaliG76Wddm > nul 2>&1
if errorlevel 1 (
  s10ediag mark g: install
  certutil -addstore -f Root "%ESP%\S10EDIAG\drivers\wdktestcert.cer" > "%G%\cert.txt" 2>&1
  certutil -addstore -f TrustedPublisher "%ESP%\S10EDIAG\drivers\wdktestcert.cer" >> "%G%\cert.txt" 2>&1
  pnputil /add-driver "%D%\MaliG76Wddm\MaliG76Wddm.inf" /install > "%G%\pnputil-install.txt" 2>&1
  ping -n 21 127.0.0.1 > nul
  pnputil /enum-devices /instanceid "ACPI\SAMS9828\0" /drivers > "%G%\pnp-gpu0-after-install.txt" 2>&1
  reg export "%DEV%\Device Parameters\MaliG76Wddm" "%G%\trace-after-install.reg" /y > nul 2>&1
)

echo --- m5test when the GPU half is live (Parameters of THIS boot say GpuEnable 1)
reg query "%KEY%" /v GpuEnable 2> nul | find "0x1" > nul
if not errorlevel 1 (
  s10ediag mark g: m5test
  "%D%\m5test.exe" > "%G%\m5test.txt" 2>&1
  rem %ERRORLEVEL% would be expanded when the block is parsed; call defers it to after m5test ran
  call echo exit %%ERRORLEVEL%% >> "%G%\m5test.txt"
  reg export "%DEV%\Device Parameters\MaliG76Wddm" "%G%\trace-after-m5test.reg" /y > nul 2>&1
)

echo --- mode for the next boot
s10ediag mark g: next mode
set GPU=0
set FULL=0
if exist "%SRC%\full.flg" set FULL=1
if exist "%SRC%\gpu.flg" set GPU=1
reg add "%KEY%" /v GpuEnable /t REG_DWORD /d %GPU% /f > "%G%\parameters-next-boot.txt" 2>&1
reg add "%KEY%" /v FullAdapter /t REG_DWORD /d %FULL% /f >> "%G%\parameters-next-boot.txt" 2>&1
set /a ANYFULL=%GPU%+%FULL%
if %ANYFULL% GTR 0 (
  rem the original INF's system-wide setting: the driver never preempts a DMA buffer (viogpu3d does the same)
  reg add "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Scheduler" /v EnablePreemption /t REG_DWORD /d 0 /f >> "%G%\parameters-next-boot.txt" 2>&1
) else (
  reg delete "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Scheduler" /v EnablePreemption /f >> "%G%\parameters-next-boot.txt" 2>&1
)
reg query "%KEY%" >> "%G%\parameters-next-boot.txt" 2>&1
s10ediag mark g: done
