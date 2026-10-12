@echo off
rem S10eAgent extra step: update MaliG76Wddm LIVE in display-only mode (GPU0 visible, Parameters 0/0, no flags in
rem \S10EDIAG\gpu) from \S10EDIAG\gpu\MaliG76Wddm - the way the first install replaced Basic Display. Run this boot
rem before any full-mode test, so the full mode never starts on an old build. notes/gpu-full-adapter-hang.md.
set G=%L%\gpu
mkdir "%G%" > nul 2>&1
set INF=%ESP%\S10EDIAG\gpu\MaliG76Wddm\MaliG76Wddm.inf
s10ediag mark g: update MaliG76Wddm live
reg query "HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters" > "%G%\update-parameters.txt" 2>&1
pnputil /enum-devices /instanceid "ACPI\SAMS9828\0" /drivers > "%G%\update-before.txt" 2>&1
if exist "%INF%" pnputil /add-driver "%INF%" /install > "%G%\update-pnputil.txt" 2>&1
ping -n 21 127.0.0.1 > nul
pnputil /enum-devices /instanceid "ACPI\SAMS9828\0" /drivers > "%G%\update-after.txt" 2>&1
for /f "usebackq delims=" %%L in (`findstr /i "Driver Version Status" "%G%\update-after.txt"`) do s10ediag mark "u: %%L"
rem The new build's PRAM DDI trace of this boot (WINPRAM + 0x4000, drivers/MaliG76Wddm/pram.c).
s10ediag peek 0x8FE04000 0x4000 > "%G%\update-pram-peek.txt" 2>&1
s10ediag mark g: update done
