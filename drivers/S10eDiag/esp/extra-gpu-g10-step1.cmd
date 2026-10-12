@echo off
rem S10eAgent extra step (M16 G10 step 1, notes/gpu-m16-g10.md "Validation procedure"): remove MaliG76Probe (G9b, both
rem bind ACPI\SAMS9828), then run \S10EDIAG\gpu\extra-gpu-g10.cmd (installs MaliG76Wddm once, display-only unless
rem \S10EDIAG\gpu\full.flg / gpu.flg exist, collects trace + state). Needs a GPU_POWER_ON 1 image (boot #106).
set G=%L%\gpu
mkdir "%G%" > nul 2>&1
pnputil /enum-drivers > "%G%\enum-drivers.txt" 2>&1
powershell -NoProfile -Command "$t = Get-Content '%G%\enum-drivers.txt' -Raw; $b = $t -split '(?=Published Name:)'; foreach ($x in $b) { if ($x -match 'malig76probe.inf' -and $x -match 'Published Name:\s+(oem\d+\.inf)') { $Matches[1] } }" > "%G%\probe-oem.txt" 2>&1
for /f "usebackq delims=" %%O in ("%G%\probe-oem.txt") do (
  s10ediag mark "g: remove MaliG76Probe %%O"
  pnputil /delete-driver %%O /uninstall /force >> "%G%\probe-remove.txt" 2>&1
)
rem maliumd.dll (copied by extra-gpu-g10.cmd) imports libgallium_d3d10.dll (G12 Mesa build): place it next to it.
if exist "%ESP%\S10EDIAG\gpu\vc_redist.arm64.exe" if not exist "%SystemRoot%\System32\vcruntime140.dll" "%ESP%\S10EDIAG\gpu\vc_redist.arm64.exe" /install /quiet /norestart > "%G%\vcredist.txt" 2>&1
if exist "%ESP%\S10EDIAG\gpu\libgallium_d3d10.dll" copy /y "%ESP%\S10EDIAG\gpu\libgallium_d3d10.dll" "%SystemRoot%\System32\libgallium_d3d10.dll" > "%G%\gallium-copy.txt" 2>&1
call "%ESP%\S10EDIAG\gpu\extra-gpu-g10.cmd"
rem The agent ends the session with a hard reset: force the SYSTEM hive to disk so next boot's Parameters survive
rem (run g10s3a wrote GpuEnable 1, boot g10s3b still read 0 - the lazy hive flush was lost).
powershell -NoProfile -Command "$k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters'); if ($k) { $k.Flush(); $k.Close(); 'flushed' }" > "%G%\regflush.txt" 2>&1
if exist "%G%\m5test.txt" for /f "usebackq delims=" %%L in ("%G%\m5test.txt") do s10ediag mark "m5: %%L"
for %%R in (trace trace-after-install) do if exist "%G%\%%R.reg" for /f "usebackq delims=" %%L in (`findstr /i "Mode Started Fail FbMatchesGop Presents CommitStatus" "%G%\%%R.reg"`) do s10ediag mark "%%R: %%L"
for /f "usebackq delims=" %%L in (`findstr /i "Name Status Resolution" "%G%\video.txt"`) do s10ediag mark "v: %%L"
s10ediag mark g: step1 wrapper done
