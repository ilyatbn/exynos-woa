#!/bin/bash
# cdpvideo.sh TAG [EXTRA...] - G35 diagnosis (notes/g35-browser-video.md): start the CDP Edge instance (separate
# profile C:\S10eTmp\edgeprof, wddm2/edgecdp.ps1) with EXTRA diagnosis switches (e.g. --no-sandbox, --enable-logging
# --v=1), read SystemInfo.getInfo (decode profiles) -> traces/g35-cdp-TAG.txt, fetch the profile's chrome_debug.log
# -> traces/g35-cdp-TAG-debug.log, then stop that instance. Tracing only (AGENTS rule 12).
TAG=${1:?tag}; shift; EXTRA=$(IFS=,; echo "$*")   # comma-joined: edgecdp.ps1 -Extra splits on commas
scp -O -q tools/phone/wddm2/edgecdp.ps1 tools/phone/g35/browserprocs.ps1 s10e:C:/S10eTmp/ < /dev/null
ssh s10e 'del C:\S10eTmp\edgeprof\chrome_debug.log 2>nul' < /dev/null
ssh s10e "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\runuser2.ps1 -Cmd \"powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\edgecdp.ps1 -Extra $EXTRA\" -Wait 20" < /dev/null
sleep 15
ssh -N -L 9222:127.0.0.1:9222 s10e & TUN=$!
sleep 3
python tools/phone/wddm2/cdp_gpuinfo.py 9222 > "traces/g35-cdp-$TAG.txt" 2>&1
kill $TUN
grep -A12 "videoDecoding" "traces/g35-cdp-$TAG.txt"
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\browserprocs.ps1' < /dev/null | grep -i "gpu-process"
ssh s10e 'type C:\S10eTmp\edgeprof\chrome_debug.log' < /dev/null > "traces/g35-cdp-$TAG-debug.log" 2>/dev/null
echo "debug log: $(wc -l < "traces/g35-cdp-$TAG-debug.log") lines"
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\runuser2.ps1 -Cmd "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\edgecdp.ps1 -Stop" -Wait 20' < /dev/null > /dev/null
