#!/bin/bash
# gamebench.sh WxH NAME [SETTLE=60] [CLICK="fx fy" + 45 s more] - G20 (notes/g20-scaled-modes.md): Game Dev Story fps at one desktop resolution.
# Sets the mode (s10emode, not saved), wakes the screen, starts the game in the user's session, waits SETTLE s, then
# samples 3x kmdstats (presents/s over 5 s), coretop (per-core CPU), gpuengine (KairoGames GPU %), dumps the scan-out
# to traces/wddm2/NAME.png, and closes the game. Same scene both runs = the game's own save/autostart.
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../../.." && pwd)
mode=${1:?WxH}; name=${2:?NAME}; settle=${3:-60}; click=$4
s() { ssh s10e "$@" < /dev/null; }
s 'taskkill /f /im KairoGames.exe >nul 2>&1 & exit 0'
"$H/mode.sh" "$mode"
scp -q -O "$H/wake.ps1" "$H/gamestart.ps1" s10e:C:/S10eTmp/ < /dev/null
s 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\run-as-user.ps1 C:\S10eTmp\wake.ps1 20' > /dev/null
s 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\run-as-user.ps1 C:\S10eTmp\gamestart.ps1 20' > /dev/null
echo "started at $mode, settling $settle s"; sleep "$settle"
if [ -n "$click" ]; then
  echo "$click" | ssh s10e 'powershell -NoProfile -Command "$input | Set-Content C:\S10eTmp\click.args"'
  scp -q -O "$H/click.ps1" s10e:C:/S10eTmp/ < /dev/null
  s 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\run-as-user.ps1 C:\S10eTmp\click.ps1 20' > /dev/null
  s 'type C:\S10eTmp\click.out'; sleep 45
fi
s 'tasklist /fi "imagename eq KairoGames.exe" /fo csv /nh'
for i in 1 2 3; do s 'C:\S10eTmp\kmdstats.exe' | head -1; done
s 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\coretop.ps1 -Seconds 5' | tail -8
s 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\gpuengine.ps1 -Name KairoGames -Seconds 5'
s 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\gpuengine.ps1 -Name dwm -Seconds 3'
bash "$R/tools/phone/wddm2/wddm2dbg.sh" fb "$name" | tail -1
s 'taskkill /f /im KairoGames.exe >nul 2>&1 & exit 0'
