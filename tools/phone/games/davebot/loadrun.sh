#!/bin/bash
# loadrun.sh TAG [cold] - G33 (notes/games-unity.md): one Dave load-time run. "cold" first deletes Dave's shader cache
# (%LOCALAPPDATA%\MaliUMD\ShaderCache\DaveTheDiver.exe-*). Runs botbench.sh TAG 30 with loadprof.ps1 sampling the
# game next to it, then prints the screen-state timeline (bot log), the load phases in seconds and the cache size.
# Outputs: traces/games/bench-TAG.* (botbench) + traces/games/loadprof-TAG.txt.
R=/c/Users/ilya/code/exynos-woa; T=${1:?tag}; OUT=$R/traces/games
CACHE='C:\Users\S10e\AppData\Local\MaliUMD\ShaderCache'
if [ "$2" = cold ]; then
  ssh s10e "powershell -NoProfile -Command \"Stop-Process -Name DaveTheDiver -Force -ErrorAction SilentlyContinue; Start-Sleep 2; Get-ChildItem $CACHE -Directory -Filter 'DaveTheDiver*' -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force; 'cache cleared'\"" < /dev/null
fi
scp -O -q "$R/tools/phone/games/loadprof.ps1" "$R/tools/phone/games/davebot/cachesize.ps1" s10e:C:/S10eTmp/ || exit 1
( sleep 15; ssh s10e "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\S10eTmp\\loadprof.ps1 -Out C:\\S10eTmp\\loadprof-$T.txt -Seconds 1200" < /dev/null ) &
# the game update's story dialogue starts right after the water HUD and is never advanced (it would progress the
# save): end the run 3 s after the water state instead of letting the bot time out
( for i in $(seq 1 240); do sleep 5
    l=$(ssh -o ConnectTimeout=8 s10e 'type C:\S10eTmp\bot\bot.log' < /dev/null 2>/dev/null | tr -d '\r')
    echo "$l" | grep -q "auto FAIL\|auto done" && break
    if echo "$l" | grep -q "state water"; then sleep 3; ssh s10e 'taskkill /f /im DaveTheDiver.exe >nul 2>&1' < /dev/null; break; fi
  done ) &
bash "$R/tools/phone/games/davebot/botbench.sh" "$T" 30 > /dev/null 2>&1
wait
scp -O -q "s10e:C:/S10eTmp/loadprof-$T.txt" "$OUT/loadprof-$T.txt"
L=$OUT/bench-$T.botlog.txt
grep -v tick "$L" | grep "auto"
# phase seconds from the bot log (HH:MM:SS.fff timestamps)
awk '
  function s(t) { split(t, a, ":"); return a[1] * 3600 + a[2] * 60 + a[3] }
  /auto: game pid/ { st = s($7) }
  /auto: state title/ && !ti { ti = s($1) }
  /M LD 760 493/ && !co { co = s($1) }
  /auto: state boat/ && !bo { bo = s($1) }
  /D 39 0/ && !dv { dv = s($1) }
  /auto: state water/ && !wa { wa = s($1) }
  END { printf "phases: start->title %.1f s, Continue->water %.1f s", ti - st, wa - co
        if (bo && dv) printf " (boat route: Continue->boat %.1f s, dive->water %.1f s)", bo - co, wa - dv
        printf "\n" }' "$L"
grep -m2 "median\|frames" "$OUT/bench-$T.txt"
ssh s10e "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\S10eTmp\\cachesize.ps1 -Filter DaveTheDiver*" < /dev/null
