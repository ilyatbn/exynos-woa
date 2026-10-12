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
call "%ESP%\S10EDIAG\gpu\extra-gpu-g10.cmd"
for %%R in (trace trace-after-install) do if exist "%G%\%%R.reg" for /f "usebackq delims=" %%L in (`findstr /i "Mode Started Fail FbMatchesGop Presents CommitStatus" "%G%\%%R.reg"`) do s10ediag mark "%%R: %%L"
for /f "usebackq delims=" %%L in (`findstr /i "Name Status Resolution" "%G%\video.txt"`) do s10ediag mark "v: %%L"
s10ediag mark g: step1 wrapper done
