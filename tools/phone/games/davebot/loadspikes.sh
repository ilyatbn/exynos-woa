#!/bin/bash
# loadspikes.sh TAG - G33 (notes/games-unity.md): UMD time breakdown of Dave's in-game load (title -> Continue ->
# water). Starts the bot's auto route, turns the Mesa 0168/0170 frame-time flag on when the title appears (so only the
# in-game load is measured), kills the game as soon as the water HUD shows (the auto route would then sit in the
# update's story dialogue - never advanced: that would progress the game), and sums the spike lines (frames > 100 ms,
# "# spike <us>: <section> <ms>, ...") per section. -> traces/games/loadspikes-TAG.txt (+ .raw).
R=/c/Users/ilya/code/exynos-woa; T=${1:?tag}; OUT=$R/traces/games
B="bash $R/tools/phone/games/davebot/bot.sh"
ssh s10e 'taskkill /f /im DaveTheDiver.exe >nul 2>&1 & del C:\S10eTmp\frametimes.on C:\S10eTmp\frametimes.txt C:\S10eTmp\bot\bot.log 2>nul' < /dev/null
$B start > /dev/null || exit 1
ssh s10e 'echo 30> C:\S10eTmp\bot\auto.req' < /dev/null
on=0
for i in $(seq 1 240); do                      # up to 20 min
  sleep 5
  l=$(ssh -o ConnectTimeout=8 s10e 'type C:\S10eTmp\bot\bot.log' < /dev/null 2>/dev/null | tr -d '\r')
  if [ $on = 0 ] && echo "$l" | grep -q "state title"; then
    ssh s10e 'powershell -NoProfile -Command "New-Item -ItemType File C:\S10eTmp\frametimes.on -Force | Out-Null"' < /dev/null
    on=1; echo "$(date +%T) title -> frame times on"
  fi
  if echo "$l" | grep -q "state water\|auto FAIL"; then
    ssh s10e 'del C:\S10eTmp\frametimes.on & taskkill /f /im DaveTheDiver.exe >nul 2>&1' < /dev/null
    echo "$(date +%T) $(echo "$l" | grep "state water\|auto FAIL" | head -1)"; break
  fi
done
sleep 3
scp -O -q s10e:C:/S10eTmp/frametimes.txt "$OUT/loadspikes-$T.raw" || exit 1
ssh s10e 'type C:\S10eTmp\bot\bot.log' < /dev/null | tr -d '\r' | grep -v tick > "$OUT/loadspikes-$T.botlog.txt"
{
  echo "# $T: frames $(grep -vc '^#' "$OUT/loadspikes-$T.raw"), spikes $(grep -c '^# spike' "$OUT/loadspikes-$T.raw")"
  # frame time total (us per line, non-comment lines: first field)
  awk '!/^#/ { s += $1 } END { printf "# frame time total %.1f s\n", s / 1e6 }' "$OUT/loadspikes-$T.raw"
  # per-section ms summed over all spike lines
  grep '^# spike' "$OUT/loadspikes-$T.raw" | sed 's/^# spike [0-9]*: //' | tr ',' '\n' |
    awk '{ v = $NF; $NF = ""; sub(/ +$/, ""); sub(/^ +/, ""); s[$0] += v } END { for (k in s) printf "%9.1f ms  %s\n", s[k], k }' |
    sort -rn | head -25
} | tee "$OUT/loadspikes-$T.txt"
