#!/bin/bash
# soak.sh MINUTES [OUT] - GPU stack soak before a release tag: every 2 min record DWM pid/start, Application Error
# count since start, KMD heartbeat; prints a verdict (DWM restarted? new crashes?). Run while the phone is idle/used.
M=${1:-20}; OUT=${2:-/c/Users/ilya/code/exynos-woa/traces/wddm2/soak.txt}
H=$(cd "$(dirname "$0")" && pwd)
t0=$(date +%s); : > "$OUT"
start=$(timeout 30 ssh s10e 'powershell -NoProfile -Command "(Get-Date).ToString(''o'')"' </dev/null | tr -d '\r')
dwm0=$(timeout 30 ssh s10e 'powershell -NoProfile -Command "(Get-Process dwm).Id"' </dev/null | tr -d '\r')
echo "soak start $start dwm $dwm0" | tee -a "$OUT"
while [ $(( $(date +%s) - t0 )) -lt $(( M * 60 )) ]; do
  sleep 120
  line=$(timeout 40 ssh s10e "powershell -NoProfile -Command \"\$d=(Get-Process dwm -ErrorAction SilentlyContinue).Id; \$c=@(Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000;StartTime=[datetime]'$start'} -ErrorAction SilentlyContinue).Count; 'dwm ' + \$d + ' crashes ' + \$c\"" </dev/null | tr -d '\r')
  hb=$(bash "$H/wddm2dbg.sh" hb 1 2>/dev/null | cut -c1-80)
  echo "$(date +%H:%M:%S) $line | $hb" | tee -a "$OUT"
done
last=$(tail -1 "$OUT")
case "$last" in *"dwm $dwm0 crashes 0"*) echo "VERDICT OK: DWM $dwm0 alive, 0 crashes over $M min" | tee -a "$OUT";;
  *) echo "VERDICT CHECK: $last" | tee -a "$OUT";; esac
