@echo off
rem Installs every driver package under %ESP%\S10EDIAG\install\<name>\ with pnputil (Windows does the NTFS writes -
rem 2026-10-12: TWRP-side DriverStore swaps ran fsck.ntfs on a dirty volume and Windows then reported NTFS corruption).
rem Called from extra-*.cmd with ESP / L / TMP set. Results in %L%\install\.
set I=%L%\install
mkdir "%I%" > nul 2>&1
for /d %%P in ("%ESP%\S10EDIAG\install\*") do (
  s10ediag mark "i: %%~nP"
  if exist "%TMP%\inst-%%~nP" rd /s /q "%TMP%\inst-%%~nP"
  xcopy /e /i /y "%%P" "%TMP%\inst-%%~nP" > nul 2>&1
  for %%F in ("%TMP%\inst-%%~nP\*.inf") do pnputil /add-driver "%%F" /install > "%I%\%%~nP.txt" 2>&1
  s10ediag mark "i: %%~nP done"
)
