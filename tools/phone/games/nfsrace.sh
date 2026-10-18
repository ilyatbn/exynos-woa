#!/bin/bash
# nfsrace.sh NAME [EXTRA_S=40] - automated NFSU2 race run (notes/games-nfsu2.md): start the game through gamedrive.ps1
# replaying the user's keyrec recording (C:\S10eTmp\gamekeys.rec, "0 PLAY"), wait until the replay is done plus EXTRA_S
# seconds of racing, dump the scan-out (traces/wddm2/NAME.png - does the race render right?), print the UMD frame
# profile of that game process, close the game. Needs MALIKM_TGSI_DIR on (umdtrace.ps1). Run from the repo root.
set -u
N=${1:?usage: nfsrace.sh NAME [EXTRA_S]}; EXTRA=${2:-40}
T="$(cygpath -u "$TEMP")"
ssh s10e 'taskkill /f /im SPEED2.EXE >nul 2>&1 & taskkill /f /im keyplay.exe >nul 2>&1 & taskkill /f /im scrnsave.scr >nul 2>&1' < /dev/null
printf '%s\r\n' "0 PLAY C:\S10eTmp\gamekeys.rec" > "$T/gk.txt"; scp -q -O "$T/gk.txt" s10e:C:/S10eTmp/gamekeys.txt < /dev/null
sleep 2
bash tools/phone/rec/runapp.sh 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\S10eTmp\gamedrive.ps1' 10 > /dev/null
for i in $(seq 1 60); do
  out=$(ssh s10e 'type C:\S10eTmp\gamedrive.out' < /dev/null 2>/dev/null | tr -d '\r')
  echo "$out" | grep -q "PLAY done\|EXITED" && break
  sleep 5
done
echo "$out" | tail -3
sleep "$EXTRA"
P=$(ssh s10e 'powershell -NoProfile -Command "(Get-Process SPEED2 -ErrorAction SilentlyContinue | Select-Object -First 1).Id"' < /dev/null | tr -d '\r')
echo "game pid ${P:-gone}"
[ -n "$P" ] && bash tools/phone/wddm2/wddm2dbg.sh fb "$N" 2>&1 | tail -1
[ -n "$P" ] && ssh s10e "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\umdlog.ps1 -Exe SPEED2 -ProcId $P -Tail 6 -Match \"app frames|app umd top|app umd calls\"" < /dev/null
ssh s10e 'taskkill /f /im SPEED2.EXE >nul 2>&1' < /dev/null
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\crashevents.ps1' < /dev/null | head -1
