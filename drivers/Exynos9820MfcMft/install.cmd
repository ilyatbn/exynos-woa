@echo off
rem install.cmd [uninstall] - run elevated on the phone, next to Exynos9820MfcMft.dll (D10 V4).
rem Install: copy the DLL to %SystemRoot%\System32, register it (CLSID + MFTRegister), back up the Media Foundation
rem preferred H.264 decoder (default: Microsoft H264 Video Decoder MFT, msmpeg2vdec.dll) and prefer this MFT instead.
rem Uninstall: restore the backed-up preferred value and unregister. Nothing else in the system is changed.
setlocal
set K=HKLM\SOFTWARE\Classes\MediaFoundation\Transforms\Preferred
set V={34363248-0000-0010-8000-00AA00389B71}
set B=HKLM\SOFTWARE\exynos-woa\MfcMft
set ME={6A2E8C51-0B7D-4C3E-8F14-982AC0DE0D11}
if /i "%~1"=="uninstall" goto uninstall
copy /y "%~dp0Exynos9820MfcMft.dll" "%SystemRoot%\System32\Exynos9820MfcMft.dll" || exit /b 1
regsvr32 /s "%SystemRoot%\System32\Exynos9820MfcMft.dll" || exit /b 1
reg query "%B%" /v PreferredH264 > nul 2>&1 && goto prefer
for /f "tokens=3" %%A in ('reg query "%K%" /v %V% 2^>nul ^| findstr /i "REG_SZ"') do reg add "%B%" /v PreferredH264 /t REG_SZ /d %%A /f > nul
:prefer
reg add "%K%" /v %V% /t REG_SZ /d %ME% /f > nul || exit /b 1
reg query "%K%" /v %V%
exit /b 0
:uninstall
for /f "tokens=3" %%A in ('reg query "%B%" /v PreferredH264 2^>nul ^| findstr /i "REG_SZ"') do reg add "%K%" /v %V% /t REG_SZ /d %%A /f > nul
regsvr32 /s /u "%SystemRoot%\System32\Exynos9820MfcMft.dll"
reg query "%K%" /v %V%
exit /b 0
