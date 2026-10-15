@echo off
rem install.cmd [uninstall] - run elevated on the phone, next to Exynos9820MfcMft.dll (D10 V4).
rem Install: copy the DLL to %SystemRoot%\System32, register it (CLSID + MFTRegister), back up the Media Foundation
rem preferred H.264 decoder (default: Microsoft H264 Video Decoder MFT, msmpeg2vdec.dll) and prefer this MFT instead.
rem Uninstall: restore the backed-up preferred value and unregister. Nothing else in the system is changed.
setlocal
if /i "%~1"=="uninstall" goto uninstall
copy /y "%~dp0Exynos9820MfcMft.dll" "%SystemRoot%\System32\Exynos9820MfcMft.dll" || exit /b 1
regsvr32 /s "%SystemRoot%\System32\Exynos9820MfcMft.dll" || exit /b 1
rem the Preferred key is TrustedInstaller-owned: prefer.ps1 takes ownership briefly and restores the descriptor
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0prefer.ps1" || exit /b 1
exit /b 0
:uninstall
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0prefer.ps1" -Restore
regsvr32 /s /u "%SystemRoot%\System32\Exynos9820MfcMft.dll"
exit /b 0
