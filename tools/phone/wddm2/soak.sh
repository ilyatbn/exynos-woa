#!/bin/bash
# soak.sh MINUTES [OUT] - GPU stack soak before a release tag: every 2 min record DWM pid, Application Error count
# since the start (phone-side soakq.ps1) and the KMD heartbeat; prints a verdict (DWM restarted? new crashes?).
M=${1:-20}; OUT=${2:-/c/Users/ilya/code/exynos-woa/traces/wddm2/soak.txt}
H=$(cd "$(dirname "$0")" && pwd)
scp -q -O "$H/soakq.ps1" s10e:C:/S10eTmp/ < /dev/null
PS='powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\soakq.ps1'
t0=$(date +%s); : > "$OUT"
dwm0=$(timeout 30 ssh s10e "$PS -Start" </dev/null | tr -d '\r' | awk '{print $2}')
echo "$(date +%H:%M:%S) soak start, dwm $dwm0, $M min" | tee -a "$OUT"
while [ $(( $(date +%s) - t0 )) -lt $(( M * 60 )) ]; do
  sleep 120
  line=$(timeout 40 ssh s10e "$PS" </dev/null | tr -d '\r')
  hb=$(bash "$H/wddm2dbg.sh" hb 1 2>/dev/null | cut -c1-80)
  echo "$(date +%H:%M:%S) $line | $hb" | tee -a "$OUT"
done
last=$(grep "crashes" "$OUT" | tail -1)
case "$last" in *"dwm $dwm0 crashes 0"*) echo "VERDICT OK: DWM $dwm0 alive, 0 crashes over $M min" | tee -a "$OUT";;
  *) echo "VERDICT CHECK: $last" | tee -a "$OUT";; esac
