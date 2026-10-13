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
if %ANYFULL% GTR 0 logman create trace "autosession\MaliDxgk" -p {802EC45A-1E99-4B83-9920-87C98277BA9D} 0xffffffffffffffff 5 -o C:\S10eTmp\dxgk.etl -bs 1024 -nb 16 256 -max 64 -mode Sequential > "%G%\etw-create.txt" 2>&1
rem G13 (run g28full): also DWM's and DXGI's own providers (GUIDs from `logman query providers` on Windows 11):
rem Microsoft-Windows-Dwm-Core {9E9BBA3C-2E38-40CB-99F4-9E8281425164}, Microsoft-Windows-DXGI {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}.
if %ANYFULL% GTR 0 logman update trace "autosession\MaliDxgk" -p {9E9BBA3C-2E38-40CB-99F4-9E8281425164} 0xffffffffffffffff 5 -o C:\S10eTmp\dxgk.etl >> "%G%\etw-create.txt" 2>&1
if %ANYFULL% GTR 0 logman update trace "autosession\MaliDxgk" -p {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9} 0xffffffffffffffff 5 -o C:\S10eTmp\dxgk.etl >> "%G%\etw-create.txt" 2>&1
if %ANYFULL% GTR 0 logman query "autosession\MaliDxgk" >> "%G%\etw-create.txt" 2>&1
rem run g29full: without -o, `logman update` rewrote the AutoLogger FileName to <current dir>\dxgk.etl (verified on
rem the build PC) and pass 2 found no C:\S10eTmp\dxgk.etl. The creation log is kept on C: too (pass 2 copies it).
if %ANYFULL% GTR 0 reg query "HKLM\SYSTEM\CurrentControlSet\Control\WMI\Autologger\MaliDxgk" /s >> "%G%\etw-create.txt" 2>&1
if %ANYFULL% GTR 0 copy /y "%G%\etw-create.txt" C:\S10eTmp\etw-create.txt > nul 2>&1
rem G13: deploy the UMD (maliumd.dll + libgallium_d3d10.dll from \S10EDIAG\gpu) into System32 NOW, while GPU0 is hidden
rem and no process has the Mali adapter's UMD loaded (in pass 2 DWM holds maliumd.dll: "being used by another process",
rem run g13fullc). Hashes before/after into gpu\umd-deploy.txt.
if exist "%G%\umd-deploy.txt" del /f /q "%G%\umd-deploy.txt"
for %%U in (maliumd.dll libgallium_d3d10.dll) do if exist "%ESP%\S10EDIAG\gpu\%%U" (
  copy /y "%ESP%\S10EDIAG\gpu\%%U" "%SystemRoot%\System32\%%U" >> "%G%\umd-deploy.txt" 2>&1
  certutil -hashfile "%SystemRoot%\System32\%%U" SHA256 >> "%G%\umd-deploy.txt" 2>&1
)
for /f "usebackq delims=" %%L in (`findstr /i /v "certutil SHA256" "%G%\umd-deploy.txt"`) do s10ediag mark "umd: %%L"
rem g43 (notes/gpu-full-adapter-hang.md): earlier full-mode boots show explorer / StartMenuExperienceHost / SearchHost
rem dying in abort() (ucrtbase 0xc0000409, WER P9 7). WER LocalDumps (learn.microsoft.com "Collecting User-Mode Dumps":
rem per-exe key, DumpFolder / DumpCount / DumpType 1 = minidump) leaves a stack of any such crash in pass 2.
if exist C:\S10eTmp\dumps rmdir /s /q C:\S10eTmp\dumps
mkdir C:\S10eTmp\dumps > nul 2>&1
icacls C:\S10eTmp\dumps /grant *S-1-1-0:(OI)(CI)M >> "%G%\set-mode.txt" 2>&1
for %%X in (explorer.exe StartMenuExperienceHost.exe SearchHost.exe dwm.exe LogonUI.exe) do (
  reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\%%X" /v DumpFolder /t REG_EXPAND_SZ /d C:\S10eTmp\dumps /f >> "%G%\set-mode.txt" 2>&1
  reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\%%X" /v DumpCount /t REG_DWORD /d 2 /f >> "%G%\set-mode.txt" 2>&1
  reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\%%X" /v DumpType /t REG_DWORD /d 1 /f >> "%G%\set-mode.txt" 2>&1
)
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
rem g44: by ~149 s the user's processes were gone from session 1 (no crash). Snapshot at the start of pass 2 (~69 s)
rem too, to time the teardown against tasks.txt later.
tasklist /v /fo csv > "%G%\tasks-early.txt" 2>&1
for %%X in (explorer.exe sihost.exe LogonUI.exe) do for /f "usebackq delims=" %%L in (`tasklist /fo table /nh /fi "imagename eq %%X"`) do s10ediag mark "p0: %%L"
rem MaliG76Wddm's PRAM DDI trace (WINPRAM + 0x4000, 16 KiB, drivers/MaliG76Wddm/pram.c) of THIS boot, live (peek only: dumpram uses MmCopyMemory, which failed with 59 on this RAM in run g13fullc).
s10ediag peek 0x8FE04000 0x4000 > "%G%\maliwddm-pram-peek.txt" 2>&1
rem The boot ETW (pass 1): stop, remove the AutoLogger, decode; dxgkrnl's diagnostic lines (event 494
rem EventDescription + data, and every event carrying a Status) into dxgk-msgs.txt, the raw .etl kept too.
s10ediag mark g: etw stop
logman stop MaliDxgk -ets > "%G%\etw-stop.txt" 2>&1
logman delete "autosession\MaliDxgk" >> "%G%\etw-stop.txt" 2>&1
if exist C:\S10eTmp\etw-create.txt copy /y C:\S10eTmp\etw-create.txt "%G%\etw-create.txt" > nul 2>&1
rem a misplaced ETL (an older pass 1 without -o on the updates) is moved to where it is expected
if not exist C:\S10eTmp\dxgk.etl if exist "%SystemRoot%\System32\dxgk.etl" move /y "%SystemRoot%\System32\dxgk.etl" C:\S10eTmp\dxgk.etl > nul 2>&1
if exist C:\S10eTmp\dxgk.etl copy /y C:\S10eTmp\dxgk.etl "%G%\dxgk.etl" > nul 2>&1
rem Run g35full: the XML decode was 461 MB and filled the 600 MB ESP (most of out\last came back 0-byte). No XML any
rem more (decode the copied .etl on the PC); the CSV stays on C: and only the findstr extracts go to the ESP.
if exist C:\S10eTmp\dxgk.etl tracerpt C:\S10eTmp\dxgk.etl -o C:\S10eTmp\dxgk.csv -of CSV -y > "%G%\etw-tracerpt.txt" 2>&1
if exist C:\S10eTmp\dxgk.csv findstr /r /c:"DxgKrnl, *DCStart *, *494," C:\S10eTmp\dxgk.csv > "%G%\dxgk-msgs.txt" 2>&1
rem DxgKrnl present results (event 184) and every DWM / DXGI event of level error or warning (CSV Level column 2/3),
rem plus their text-bearing rows, into dwm-msgs.txt - DWM's own reason for re-creating or stalling.
if exist C:\S10eTmp\dxgk.csv findstr /r /c:"DxgKrnl, *Info *, *184," C:\S10eTmp\dxgk.csv > "%G%\dxgk-present.txt" 2>&1
if exist C:\S10eTmp\dxgk.csv findstr /r /c:"^Microsoft-Windows-Dwm-Core" /c:"^Microsoft-Windows-DXGI" C:\S10eTmp\dxgk.csv > "%G%\dwm-all.txt" 2>&1
if exist "%G%\dwm-all.txt" findstr /r /c:"^[^,]*,[^,]*,[^,]*,[^,]*,[^,]*, *[23]," /c:"fail" /c:"error" /c:"0x8" "%G%\dwm-all.txt" > "%G%\dwm-msgs.txt" 2>&1
for /f "usebackq delims=" %%L in (`findstr /n "^" "%G%\dwm-msgs.txt" ^| findstr /b "1: 2: 3: 4: 5:"`) do s10ediag mark "e: %%L"
if exist C:\S10eTmp\dxgk.etl del /f /q C:\S10eTmp\dxgk.etl
if exist C:\S10eTmp\dxgk.csv del /f /q C:\S10eTmp\dxgk.csv
rem G13: can the D3D runtimes create a device on the Mali adapter (DWM needs D3D11; maliumd.dll is Mesa d3d10umd
rem built D3D10.0-only)? d3dprobe prints per adapter the UMD names for DX9/10/11, the WDDM version and the HRESULT of
rem D3D11CreateDevice / D3D10CreateDevice; MALIKM_TRACE=1 adds the Mesa shim's trace (stderr) from inside the probe.
s10ediag mark g: d3dprobe
set MALIKM_TRACE=1
if exist "%ESP%\S10EDIAG\gpu\d3dprobe.exe" "%ESP%\S10EDIAG\gpu\d3dprobe.exe" > "%G%\d3dprobe.txt" 2> "%G%\d3dprobe-stderr.txt"
set MALIKM_TRACE=
for /f "usebackq delims=" %%L in (`findstr /i "adapter D3D11CreateDevice D3D10CreateDevice UMD WDDM" "%G%\d3dprobe.txt"`) do s10ediag mark "d: %%L"
rem G13 (run g19full): d3dprobe and dxdiag above/below run as SYSTEM in session 0, which owns no display (EnumOutputs
rem 0x887A0022 there may be normal). Repeat the probe in the logged-on user's session (run-as-user.ps1), and list the
rem modules DWM actually loaded (maliumd.dll / libgallium_d3d10.dll = DWM's device is on the Mali adapter).
s10ediag mark g: dwm modules + user-session d3dprobe
powershell -NoProfile -Command "Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { 'dwm pid ' + $_.Id; $_.Modules | Where-Object { $_.ModuleName -match 'maliumd|gallium|d3d10|d3d11|dxgi|warp|basicrender' } | ForEach-Object { '  ' + $_.FileName } }" > "%G%\dwm-modules.txt" 2>&1
copy /y "%ESP%\S10EDIAG\run-as-user.ps1" C:\S10eTmp\run-as-user.ps1 > nul 2>&1
copy /y "%ESP%\S10EDIAG\gpu\d3dprobe.exe" C:\S10eTmp\d3dprobe.exe > nul 2>&1
if exist C:\S10eTmp\d3dfill.exe del /f /q C:\S10eTmp\d3dfill.exe
copy /y "%ESP%\S10EDIAG\gpu\d3dfill.exe" C:\S10eTmp\d3dfill.exe > nul 2>&1
> C:\S10eTmp\gpu-user.ps1 echo C:\S10eTmp\d3dprobe.exe ^> C:\S10eTmp\d3dprobe-user.txt 2^>^&1
>> C:\S10eTmp\gpu-user.ps1 echo Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Screen]::AllScreens ^| Format-List DeviceName,Primary,Bounds ^| Out-File -Append C:\S10eTmp\d3dprobe-user.txt
rem G13 d3dfill (notes/gpu-full-adapter-hang.md): D3D11 on the Mali, exclusive fullscreen, red/green/blue/white 8 s each
rem with present counts + back-buffer readback - a colour on the webcam means render + present + scanout work without DWM.
if exist C:\S10eTmp\d3dfill.exe >> C:\S10eTmp\gpu-user.ps1 echo C:\S10eTmp\d3dfill.exe 8 ^> C:\S10eTmp\d3dfill-user.txt 2^>^&1
if exist C:\S10eTmp\d3dfill-user.txt del /f /q C:\S10eTmp\d3dfill-user.txt
if exist C:\S10eTmp\d3dprobe-user.txt del /f /q C:\S10eTmp\d3dprobe-user.txt
rem g43: run-as-user found no explorer.exe at ~135 s although the agent saw it up for 30 s at 64 s - which processes
rem (session, user, state) exist right before the user task.
tasklist /v /fo csv > "%G%\tasks.txt" 2>&1
for %%X in (explorer.exe LogonUI.exe) do for /f "usebackq delims=" %%L in (`tasklist /fo table /nh /fi "imagename eq %%X"`) do s10ediag mark "p: %%L"
rem g45: logon never starts in full mode (LogonUI up 300 s, no session logon). Live stack minidumps of the logon
rem chain and DWM (livedump.ps1, MiniDumpWriteDump without full memory) show what they wait on.
if exist "%ESP%\S10EDIAG\livedump.ps1" powershell -NoProfile -ExecutionPolicy Bypass -File "%ESP%\S10EDIAG\livedump.ps1" -Out "%G%\live" -Names LogonUI,dwm,winlogon > "%G%\livedump.txt" 2>&1
for /f "usebackq delims=" %%L in ("%G%\livedump.txt") do s10ediag mark "l: %%L"
powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\run-as-user.ps1 -Script C:\S10eTmp\gpu-user.ps1 -Wait 240 > "%G%\user-task.txt" 2>&1
if exist C:\S10eTmp\d3dprobe-user.txt copy /y C:\S10eTmp\d3dprobe-user.txt "%G%\d3dprobe-user.txt" > nul
if exist C:\S10eTmp\d3dfill-user.txt copy /y C:\S10eTmp\d3dfill-user.txt "%G%\d3dfill-user.txt" > nul
for /f "usebackq delims=" %%L in (`findstr /i "hr readback presents TIMEOUT done" "%G%\d3dfill-user.txt"`) do s10ediag mark "f: %%L"
for /f "usebackq delims=" %%L in (`findstr /i "maliumd gallium" "%G%\dwm-modules.txt"`) do s10ediag mark "w: %%L"
rem g43: Application.evtx is exported by collect.cmd at ~66 s, before this step; the crash events (Application Error
rem 1000) of the whole boot are taken again here, plus the WER LocalDumps armed in pass 1.
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /c:30 /rd:true /f:text > "%G%\app-crashes.txt" 2>&1
if exist C:\S10eTmp\dumps mkdir "%G%\dumps" > nul 2>&1
if exist C:\S10eTmp\dumps copy /y C:\S10eTmp\dumps\*.dmp "%G%\dumps\" > nul 2>&1
dir /b "%G%\dumps" > "%G%\dumps.txt" 2>&1
for /f "usebackq" %%N in (`find /c ".dmp" ^< "%G%\dumps.txt"`) do s10ediag mark "a: %%N minidumps"
rem g44: winlogon never logged its logon notifications in full mode and the user session vanished without a crash.
rem The session / logon channels of this boot (newest 150 events each, text) say who ended it and when.
wevtutil qe Microsoft-Windows-Winlogon/Operational /c:150 /rd:true /f:text > "%G%\ev-winlogon.txt" 2>&1
wevtutil qe Microsoft-Windows-TerminalServices-LocalSessionManager/Operational /c:150 /rd:true /f:text > "%G%\ev-lsm.txt" 2>&1
wevtutil qe "Microsoft-Windows-User Profile Service/Operational" /c:150 /rd:true /f:text > "%G%\ev-profile.txt" 2>&1
wevtutil qe Microsoft-Windows-Shell-Core/Operational /c:150 /rd:true /f:text > "%G%\ev-shellcore.txt" 2>&1
rem Security: 4624 is mostly service logons (type 5); keep the interactive ones (type 2) and every logoff/failure.
wevtutil qe Security /q:"*[System[(EventID=4624)] and EventData[Data[@Name='LogonType']='2']]" /c:10 /rd:true /f:text > "%G%\ev-security.txt" 2>&1
wevtutil qe Security /q:"*[System[(EventID=4634 or EventID=4647 or EventID=4625)]]" /c:20 /rd:true /f:text >> "%G%\ev-security.txt" 2>&1
for /f "usebackq delims=" %%L in (`findstr /i "adapter output D3D11CreateDevice EnumOutputs DeviceName Bounds" "%G%\d3dprobe-user.txt"`) do s10ediag mark "u: %%L"
s10ediag mark g: dxdiag
dxdiag /whql:off /t "%G%\dxdiag.txt"
ping -n 46 127.0.0.1 > nul
for /f "usebackq delims=" %%L in (`findstr /i /c:"Card name" /c:"DDI Version" /c:"Feature Levels" /c:"Driver Model" /c:"User Mode Driver" "%G%\dxdiag.txt"`) do s10ediag mark "x: %%L"
call "%ESP%\S10EDIAG\gpu\extra-gpu-g10.cmd"
for %%R in (trace) do if exist "%G%\%%R.reg" for /f "usebackq delims=" %%L in (`findstr /i "Mode Started Fail Seen_Create GpuQuieted ResContract Presents" "%G%\%%R.reg"`) do s10ediag mark "t: %%L"
:end
s10ediag mark g: done
rem Run g35full: the full-mode clean restart hangs and the agent's hard reset follows; half of out\last\gpu (FAT ESP)
rem came back as 0-byte files (trace.reg, pnp-gpu0.txt, ...). Flush every volume now, before collect.cmd restarts.
s10ediag flush > nul 2>&1
