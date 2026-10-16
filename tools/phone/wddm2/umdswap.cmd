@echo off
rem (SYSTEM task) C:\S10eTmp\umd\{maliumd,libgallium_d3d10}.dll into System32. A loaded DLL cannot be overwritten or
rem deleted but can be renamed: the current one goes to <name>.<random>.old (a fixed .prev name fails while an older
rem .prev is still loaded, e.g. by DWM), then the new one is copied in. Stale .old files are deleted when unused.
set L=C:\S10eTmp\umdswap.log
echo umdswap > %L%
for %%F in (maliumd.dll libgallium_d3d10.dll) do (
  del /f /q C:\Windows\System32\%%F.*.old >nul 2>&1
  if exist C:\Windows\System32\%%F move /y C:\Windows\System32\%%F C:\Windows\System32\%%F.%RANDOM%%RANDOM%.old >> %L% 2>&1
  copy /y C:\S10eTmp\umd\%%F C:\Windows\System32\%%F >> %L% 2>&1
  certutil -hashfile C:\Windows\System32\%%F SHA256 >> %L% 2>&1
)
