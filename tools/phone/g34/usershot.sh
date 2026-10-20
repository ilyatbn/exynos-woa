#!/bin/bash
# usershot.sh OUT.png - screenshot of the phone's user session (wddm2/shot.ps1 run there through runuser2.ps1),
# copied to OUT.png on the PC. Needs `wddm2dbg.sh push` once per boot. notes/g34-fl11.md.
OUT=${1:?out.png}
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\runuser2.ps1 -Cmd "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\shot.ps1 -Out C:\S10eTmp\usershot.png" -Wait 40' < /dev/null
scp -O -q s10e:C:/S10eTmp/usershot.png "$OUT" && echo "saved $OUT"
