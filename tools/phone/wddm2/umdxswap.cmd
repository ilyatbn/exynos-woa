@echo off
rem (SYSTEM task) G27: the ARM64X UMD set C:\S10eTmp\umdx\{maliumd,maliumd_arm64,maliumd_ec}.dll into System32
rem (maliumd.dll = the pure forwarder, notes/g27-x64-apps.md). Same rename-then-copy as umdswap.cmd; the plain ARM64
rem maliumd.dll in place before the first swap is also kept as maliumd.dll.pre-g27 (rollback: umdswap with that file).
set L=C:\S10eTmp\umdxswap.log
echo umdxswap > %L%
if not exist C:\Windows\System32\maliumd.dll.pre-g27 copy /y C:\Windows\System32\maliumd.dll C:\Windows\System32\maliumd.dll.pre-g27 >> %L% 2>&1
for %%F in (maliumd.dll maliumd_arm64.dll maliumd_ec.dll) do (
  del /f /q C:\Windows\System32\%%F.*.old >nul 2>&1
  if exist C:\Windows\System32\%%F move /y C:\Windows\System32\%%F C:\Windows\System32\%%F.%RANDOM%%RANDOM%.old >> %L% 2>&1
  copy /y C:\S10eTmp\umdx\%%F C:\Windows\System32\%%F >> %L% 2>&1
  certutil -hashfile C:\Windows\System32\%%F SHA256 >> %L% 2>&1
)
