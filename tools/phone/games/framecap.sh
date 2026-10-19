#!/bin/bash
# framecap.sh NAME [EXE] - one-frame pass log of a running D3D game (Mesa 0163, d3d10umd FrameCap.cpp): creates
# C:\S10eTmp\framecap.flg (the UMD checks it every 30 presents and deletes it), waits for the "fc end" line in the
# game's malikm tlog, and saves the last capture (from "fc begin" to "fc end") to traces/games/framecap-NAME.txt.
# EXE default DaveTheDiver. notes/games-unity.md.
N=${1:?name}; EXE=${2:-DaveTheDiver}
OUT=/c/Users/ilya/code/exynos-woa/traces/games/framecap-$N.txt
ssh s10e 'type nul > C:\S10eTmp\framecap.flg' < /dev/null
for i in $(seq 1 60); do
  ssh s10e "powershell -NoProfile -Command \"\$p = Get-Process '$EXE' -ErrorAction SilentlyContinue | Select-Object -First 1; if (\$p) { Copy-Item \\\"C:\\S10eTmp\\tlog\\malikm-$EXE.exe-\$(\$p.Id).txt\\\" C:\\S10eTmp\\framecap-tlog.txt }\"" < /dev/null
  scp -O -q s10e:C:/S10eTmp/framecap-tlog.txt /tmp/framecap-tlog.txt 2>/dev/null
  if grep -q "fc end" /tmp/framecap-tlog.txt 2>/dev/null; then
    awk '/ fc begin/{buf=""} / fc /{buf=buf $0 "\n"} / fc end/{last=buf} END{printf "%s", last}' /tmp/framecap-tlog.txt > "$OUT"
    echo "$OUT: $(wc -l < "$OUT") lines"
    exit 0
  fi
  sleep 3
done
echo "no capture after 3 min (flag still there? game not presenting?)"; exit 1
