#!/bin/bash
# camwatch.sh TAG [OUTDIR] [MAXSEC] - webcam shot (camshot.ps1) every ~20 s while the phone is NOT in TWRP
# (starts once it has left TWRP), until it is back in TWRP or MAXSEC (default 2700) passes.
# Shots: OUTDIR/cam-TAG-<i>-<hhmmss>-<state>.jpg (OUTDIR default s10e-artifacts/cam).
H=$(cd "$(dirname "$0")" && pwd)
O=${2:-C:/Users/ilya/code/s10e-artifacts/cam}; mkdir -p "$O"; MAX=${3:-2700}
t0=$(date +%s); left=0; i=0
while [ $(( $(date +%s) - t0 )) -lt "$MAX" ]; do
  s=$(bash "$H/phonestate.sh" | awk '{print $2}')
  if [ "$s" != twrp ]; then left=1; i=$((i+1)); powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -m "$H/camshot.ps1")" -Out "$(cygpath -m "$O")/cam-$1-$i-$(date +%H%M%S)-$s.jpg" >/dev/null 2>&1
  elif [ $left = 1 ]; then echo "back in twrp $(date +%T), $i shots"; exit 0; fi
  sleep 15
done
echo "timeout, $i shots"
