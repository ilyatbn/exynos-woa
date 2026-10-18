#!/bin/bash
# iddswap.sh PKGDIR [warp|mali] - G18 remote-monitor PoC: replace the S10eIdd (IddSampleDriver) package on the phone
# (stop the stream task, uninstall + delete every IddSampleDriver package - the software device keeps its old driver
# otherwise -, add PKGDIR's, set the RenderWarp diagnosis switch), then start the stream (iddstream.cmd: virtual
# monitor + S10eIddView on 8090, 300 s) and print iddstate.ps1 after 25 s. Run from the repo root.
set -u
P=${1:?usage: iddswap.sh PKGDIR [warp|mali]}; MODE=${2:-mali}
ssh s10e 'taskkill /f /im IddSampleApp.exe >nul 2>&1 & taskkill /f /im S10eIddView.exe >nul 2>&1 & schtasks /delete /tn S10eIddStream /f >nul 2>&1' < /dev/null
for o in $(ssh s10e 'pnputil /enum-drivers' < /dev/null | tr -d '\r' | awk '/Published Name/{p=$3} /Original Name/ && tolower($3)=="iddsampledriver.inf"{print p}'); do
  ssh s10e "pnputil /delete-driver $o /uninstall /force" < /dev/null | tail -1
done
ssh s10e 'mkdir C:\S10eTmp\idd2 2>nul' < /dev/null
scp -q -O "$P"/* tools/phone/rec/iddview/S10eIddView.exe tools/phone/rec/iddstream.cmd tools/phone/rec/iddstate.ps1 s10e:C:/S10eTmp/idd2/ < /dev/null
ssh s10e 'pnputil /add-driver C:\S10eTmp\idd2\IddSampleDriver.inf' < /dev/null | grep -i "added\|fail"
if [ "$MODE" = warp ]; then V=1; else V=0; fi
ssh s10e "reg add HKLM\SOFTWARE\S10eIdd /v RenderWarp /t REG_DWORD /d $V /f" < /dev/null | tail -1
ssh s10e 'schtasks /create /tn S10eIddStream /tr "cmd /c C:\S10eTmp\idd2\iddstream.cmd" /sc once /st 00:00 /ru SYSTEM /f >nul 2>&1 & schtasks /run /tn S10eIddStream' < /dev/null | tail -1
sleep 25
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\idd2\iddstate.ps1' < /dev/null
