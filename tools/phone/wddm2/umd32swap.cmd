@echo off
rem (SYSTEM task) C:\S10eTmp\umd32\maliumd32.dll into SysWOW64 (the x86 UMD for WOW64 games). Like umdswap.cmd: the
rem current one is renamed to maliumd32.dll.<random>.old (works while loaded), then the new one is copied in.
set L=C:\S10eTmp\umd32swap.log
echo umd32swap > %L%
if exist C:\Windows\SysWOW64\maliumd32.dll move /y C:\Windows\SysWOW64\maliumd32.dll C:\Windows\SysWOW64\maliumd32.dll.%RANDOM%%RANDOM%.old >> %L% 2>&1
copy /y C:\S10eTmp\umd32\maliumd32.dll C:\Windows\SysWOW64\maliumd32.dll >> %L% 2>&1
certutil -hashfile C:\Windows\SysWOW64\maliumd32.dll SHA256 >> %L% 2>&1
