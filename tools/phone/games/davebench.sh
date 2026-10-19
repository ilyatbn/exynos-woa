#!/bin/bash
# davebench.sh [REC] [TAG] - Dave the Diver benchmark (notes/games-unity.md): split the user's keyrec recording (REC,
# default traces/games/davekeys.rec) into the three route segments (gaps > 30 s; title taps / boat -> dive / diving),
# push them with davebench.ps1, start the run detached in the user's session, wait for "done", then print frame-time
# stats of the dive segment (frametimes.txt from Mesa 0168): frames, mean fps, median / p95 / p99 / max frame ms,
# 1 % and 0.1 % low fps (mean of the slowest 1 % / 0.1 % frames). Results + the raw file go to traces/games/bench-TAG.*
REPO=/c/Users/ilya/code/exynos-woa
REC=${1:-$REPO/traces/games/davekeys.rec}; TAG=${2:-$(date +%H%M)}
OUT=$REPO/traces/games
W=$(cygpath -w "$OUT" 2>/dev/null || echo "$OUT")
python - "$REC" "$W" <<'PY'
import sys, os
rec, out = sys.argv[1], sys.argv[2]
ev = [l.split() for l in open(rec) if l.strip() and not l.startswith('#')]
segs, cur, last = [], [], None
for e in ev:
    t = int(e[0])
    if last is not None and t - last > 30000:
        segs.append(cur); cur = []
    cur.append(e); last = t
segs.append(cur)
assert len(segs) == 3, [len(s) for s in segs]
for i, s in enumerate(segs, 1):
    base = int(s[0][0]) - 1000          # first event 1 s after the segment starts
    with open(os.path.join(out, 'dave-seg%d.rec' % i), 'w', newline='\r\n') as f:
        f.write('# dave route segment %d, rebased from %s\n' % (i, os.path.basename(rec)))
        for e in s:
            f.write(' '.join([str(int(e[0]) - base)] + e[1:]) + '\n')
    print('seg%d: %d events, %.1f s' % (i, len(s), (int(s[-1][0]) - int(s[0][0])) / 1000))
PY
scp -O -q "$OUT"/dave-seg1.rec "$OUT"/dave-seg2.rec "$OUT"/dave-seg3.rec "$REPO"/tools/phone/games/davebench.ps1 \
  "$REPO"/tools/phone/games/davebench-start.ps1 "$REPO"/tools/phone/games/keyrec/keyplay.exe s10e:C:/S10eTmp/ || exit 1
ssh s10e 'taskkill /f /im scrnsave.scr >nul 2>&1' < /dev/null
# detached: a user-session task that only starts the bench process (davebench-start.ps1)
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\userrun.ps1 -Exe powershell.exe -Args "-NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\davebench-start.ps1" -Wait 30' < /dev/null
for i in $(seq 1 180); do
  l=$(ssh -o ConnectTimeout=8 s10e 'type C:\S10eTmp\davebench.log' < /dev/null 2>/dev/null | tr -d '\r')
  [ "$l" != "$prev" ] && { echo "$l" | tail -1; prev=$l; }
  echo "$l" | grep -q "done" && break
  sleep 10
done
scp -O -q s10e:C:/S10eTmp/frametimes.txt "$OUT/bench-$TAG.frametimes.txt" || { echo "no frametimes.txt"; exit 1; }
python - "$OUT/bench-$TAG.frametimes.txt" <<'PY' | tee "$OUT/bench-$TAG.txt"
import sys, statistics
v = [int(x) for x in open(sys.argv[1]) if x.strip()]
v = [x for x in v if x > 0]
s = sorted(v)
n = len(v)
def pct(p): return s[min(n - 1, int(p * n))] / 1000
def low(frac):
    k = max(1, int(n * frac))
    worst = s[-k:]
    return 1e6 / (sum(worst) / k)
print('frames %d over %.1f s: mean %.1f fps, median %.1f ms, p95 %.1f ms, p99 %.1f ms, max %.1f ms, 1%% low %.1f fps, 0.1%% low %.1f fps'
      % (n, sum(v) / 1e6, n / (sum(v) / 1e6), pct(0.5), pct(0.95), pct(0.99), s[-1] / 1000, low(0.01), low(0.001)))
PY
