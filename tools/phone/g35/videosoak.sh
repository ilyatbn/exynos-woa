#!/bin/bash
# videosoak.sh TAG [MINUTES=40] [EVERY=3] - G18/G35: Edge plays g35/play.html (looped clip, hardware decode) for MINUTES;
# every EVERY minutes: the page's frame counter (browsertitle.ps1; must grow ~24/s), Edge's GPU-process pid (a new pid =
# the GPU process was replaced), and DWM pacing (pacingab.sh, 1 round). One line per check -> traces/g35-vsoak-TAG.txt.
TAG=${1:?tag}; M=${2:-40}; E=${3:-3}; S=traces/g35-vsoak-$TAG.txt; : > "$S"
scp -O -q tools/phone/g35/browservideo.ps1 tools/phone/g35/browsertitle.ps1 tools/phone/g35/browserprocs.ps1 s10e:C:/S10eTmp/ < /dev/null
ssh s10e 'taskkill /f /im msedge.exe >nul 2>&1' < /dev/null; sleep 2
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\runuser2.ps1 -Cmd "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\browservideo.ps1 -Browser msedge -Wait 5" -Wait 40' < /dev/null > /dev/null
title() { ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\runuser2.ps1 -Cmd "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\browsertitle.ps1" -Wait 20 >nul & type C:\S10eTmp\title.txt' < /dev/null | grep -o 'g35 [0-9]*' | head -1 | awk '{print $2}'; }
end=$(( $(date +%s) + M * 60 )); n=0
while [ "$(date +%s)" -lt "$end" ]; do
  n=$((n + 1))
  f1=$(title); sleep 10; f2=$(title)
  fps=$(awk -v a="${f1:-0}" -v b="${f2:-0}" 'BEGIN{ d=b-a; if (d<0) d=b; printf "%.1f", d/14 }')   # ~14 s between reads
  gpu=$(ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\browserprocs.ps1' < /dev/null | awk '/gpu-process/{print $1"@"$3}' | paste -sd,)
  rates=$(bash tools/phone/g18/pacingab.sh "$TAG-r$n" 1 2>&1 | grep -o '= [0-9.]*/s' | tr -d '=/s ' | paste -sd' ')
  echo "$(date +%T) check $n frames $f1->$f2 (~$fps/s) gpu-process $gpu dwm swap rates [$rates]" | tee -a "$S"
  sleep $(( E * 60 - 80 > 10 ? E * 60 - 80 : 10 ))
done
