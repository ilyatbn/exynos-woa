#!/bin/bash
# pacingsoak.sh TAG [MINUTES=40] [EVERY=5] - repeat pacingab.sh (1 round) every EVERY minutes for MINUTES, one summary
# line per round (swap-chain present rates + vsync/s and presents/s over the 30 s idle window) -> traces/g18-soak-TAG.txt.
# Stops early if the composition rate drops below 45/s (the regression) so it can be inspected at once.
TAG=${1:?tag}; M=${2:-40}; E=${3:-5}; S=traces/g18-soak-$TAG.txt; : > "$S"
end=$(( $(date +%s) + M * 60 ))
n=0
while [ "$(date +%s)" -lt "$end" ]; do
  n=$((n + 1))
  out=$(bash tools/phone/g18/pacingab.sh "$TAG-r$n" 1 2>&1)
  rates=$(echo "$out" | grep -o '= [0-9.]*/s' | tr -d '=/s ' | paste -sd' ')
  hb=$(echo "$out" | grep HB | tail -2)
  v=$(echo "$hb" | awk '{for(i=1;i<=NF;i++) if($i ~ /^v/) print strtonum("0x" substr($i,2))}' | paste -sd' ')
  t=$(echo "$hb" | awk '{print $1+0}' | paste -sd' ')
  vs=$(echo "$v $t" | awk '{ if ($4 > $3) printf "%.1f", ($2-$1)*1000/($4-$3); else print "?" }')
  echo "$(date +%T) round $n present rates [$rates] idle vsync/s $vs" | tee -a "$S"
  first=$(echo "$rates" | awk '{print $1+0}')
  if awk "BEGIN{exit !($first < 45)}"; then echo "REGRESSION seen at round $n" | tee -a "$S"; break; fi
  sleep $(( E * 60 - 45 ))
done
