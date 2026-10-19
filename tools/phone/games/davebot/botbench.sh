#!/bin/bash
# botbench.sh TAG [SWIM_S=100] - one screen-driven Dave benchmark (davebot.ps1 Auto, notes/games-unity.md): restart the
# bot, auto route (launch -> Continue -> boat -> dive -> swim with frame times), wait for "auto done"/"auto FAIL", then
# frame-time stats + spike lines into traces/games/bench-TAG.txt (raw: .frametimes.txt, bot log: .botlog.txt).
# The game is always ended by killing it (user: never "return to map" - that saves progress).
R=/c/Users/ilya/code/exynos-woa; T=${1:?tag}; S=${2:-100}; OUT=$R/traces/games
B="bash $R/tools/phone/games/davebot/bot.sh"
ssh s10e 'taskkill /f /im DaveTheDiver.exe >nul 2>&1 & del C:\S10eTmp\frametimes.on C:\S10eTmp\bot\bot.log 2>nul' < /dev/null
$B start > /dev/null || exit 1
ssh s10e "echo $S> C:\\S10eTmp\\bot\\auto.req" < /dev/null
prev=""
for i in $(seq 1 120); do                       # up to 20 min
  sleep 10
  l=$(ssh -o ConnectTimeout=8 s10e 'type C:\S10eTmp\bot\bot.log' < /dev/null 2>/dev/null | tr -d '\r' | grep "auto" | tail -1)
  [ -n "$l" ] && [ "$l" != "$prev" ] && { echo "$l"; prev=$l; }
  echo "$l" | grep -q "auto done\|auto FAIL" && break
done
ssh s10e 'type C:\S10eTmp\bot\bot.log' < /dev/null | tr -d '\r' > "$OUT/bench-$T.botlog.txt"
ssh s10e 'taskkill /f /im DaveTheDiver.exe >nul 2>&1 & del C:\S10eTmp\frametimes.on 2>nul' < /dev/null
grep -q "auto done" "$OUT/bench-$T.botlog.txt" || { echo "run failed - see bench-$T.botlog.txt"; exit 1; }
scp -O -q s10e:C:/S10eTmp/frametimes.txt "$OUT/bench-$T.frametimes.txt" || exit 1
python -I - "$OUT/bench-$T.frametimes.txt" <<'PY' | tee "$OUT/bench-$T.txt"
import sys
lines = open(sys.argv[1]).read().splitlines()
v = [int(x) for x in lines if x.strip() and not x.startswith('#')]
v = [x for x in v if x > 0]
s = sorted(v); n = len(v)
pct = lambda p: s[min(n - 1, int(p * n))] / 1000
def low(f):
    k = max(1, int(n * f)); return 1e6 / (sum(s[-k:]) / k)
print('frames %d over %.1f s: mean %.1f fps, median %.1f ms, p95 %.1f ms, p99 %.1f ms, max %.1f ms, 1%% low %.1f fps, 0.1%% low %.1f fps'
      % (n, sum(v) / 1e6, n / (sum(v) / 1e6), pct(.5), pct(.95), pct(.99), s[-1] / 1000, low(.01), low(.001)))
print('frames > 100 ms: %d' % sum(1 for x in v if x > 100000))
for x in lines:
    if x.startswith('# spike'):
        print(x[:500])
PY
grep "swim done" "$OUT/bench-$T.botlog.txt"
