@echo off
rem gfxtools.cmd (run as SYSTEM) - install the "Graphics Tools" optional feature (D3D11/DXGI debug layers:
rem d3d11_3SDKLayers.dll, dxgidebug.dll) for G15 S4 diagnostics. Windows Update is off on the phone
rem (tools/phone/wuoff.cmd); optional features come from it, so access is opened for the download only and
rem wuoff.cmd's settings are put back afterwards (NoAutoUpdate stays set the whole time). Log: C:\S10eTmp\gfxtools.log
set L=C:\S10eTmp\gfxtools.log
set P=HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate
echo gfxtools > %L%
reg delete "%P%" /v DisableWindowsUpdateAccess /f >> %L% 2>&1
reg delete "%P%" /v DoNotConnectToWindowsUpdateInternetLocations /f >> %L% 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Services\wuauserv" /v Start /t REG_DWORD /d 3 /f >> %L% 2>&1
net start wuauserv >> %L% 2>&1
powershell -NoProfile -Command "Add-WindowsCapability -Online -Name Tools.Graphics.DirectX~~~~0.0.1.0 | Format-List | Out-String" >> %L% 2>&1
net stop wuauserv >> %L% 2>&1
call C:\S10eTmp\wuoff.cmd >> %L% 2>&1
dir C:\Windows\System32\d3d11_3SDKLayers.dll C:\Windows\System32\dxgidebug.dll >> %L% 2>&1
echo GFXTOOLS_DONE >> %L%
