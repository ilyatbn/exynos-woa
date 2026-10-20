#!/bin/bash
# cdplong.sh TAG [SECONDS=1000] - G35: the video-stall repro with diagnostics: CDP Edge instance with --no-sandbox (the
# GPU process writes its UMD log), play.html for SECONDS with the DevTools Media log (cdp_nav.py) -> traces/g35-long-TAG-
# media.txt, then every GPU-process UMD log of that instance -> traces/g35-long-TAG-gpu-<pid>.txt. Tracing only (rule 12).
TAG=${1:?tag}; W=${2:-1000}
scp -O -q tools/phone/wddm2/edgecdp.ps1 tools/phone/g35/browserprocs.ps1 s10e:C:/S10eTmp/ < /dev/null
ssh s10e 'taskkill /f /im msedge.exe >nul 2>&1' < /dev/null; sleep 2
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\runuser2.ps1 -Cmd "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\edgecdp.ps1 -Extra --no-sandbox" -Wait 20' < /dev/null > /dev/null
sleep 12
t0=$(ssh s10e 'powershell -NoProfile -Command "Get-Date -Format o"' < /dev/null | tr -d '\r')
ssh -N -L 9222:127.0.0.1:9222 s10e & TUN=$!; sleep 3
timeout $((W + 120)) python tools/phone/g35/cdp_nav.py file:///C:/S10eTmp/g35/play.html 9222 "$W" > "traces/g35-long-$TAG-media.txt" 2>&1
kill $TUN
for g in $(ssh s10e "powershell -NoProfile -Command \"Get-ChildItem C:\S10eTmp\tlog -Filter malikm-msedge.exe-*.txt | Where-Object { \$_.LastWriteTime -gt [datetime]'$t0' -and \$_.Name -notmatch 'stderr' } | ForEach-Object { \$_.Name -replace '\D', '' }\"" < /dev/null | tr -d '\r'); do
  ssh s10e "type C:\S10eTmp\tlog\malikm-msedge.exe-$g.txt" < /dev/null > "traces/g35-long-$TAG-gpu-$g.txt" 2>/dev/null
  ssh s10e "type C:\S10eTmp\tlog\malikm-msedge.exe-$g-stderr.txt" < /dev/null > "traces/g35-long-$TAG-gpu-$g-stderr.txt" 2>/dev/null
done
echo "done $(date +%T)"; grep -i "error\|warning\|page:" "traces/g35-long-$TAG-media.txt" | head
