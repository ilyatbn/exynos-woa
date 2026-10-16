@echo off
set L=C:\S10eTmp\umdswap.log
echo umdswap > %L%
for %%F in (maliumd.dll libgallium_d3d10.dll) do (
  if exist C:\Windows\System32\%%F.prev del /f /q C:\Windows\System32\%%F.prev >> %L% 2>&1
  move /y C:\Windows\System32\%%F C:\Windows\System32\%%F.prev >> %L% 2>&1
  copy /y C:\S10eTmp\umd\%%F C:\Windows\System32\%%F >> %L% 2>&1
  certutil -hashfile C:\Windows\System32\%%F SHA256 >> %L% 2>&1
)
