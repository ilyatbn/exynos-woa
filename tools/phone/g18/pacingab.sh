#!/bin/bash
# pacingab.sh TAG [ROUNDS=3] - G18 P4b root-cause A/B (notes/g18-hdmi-prep.md): DWM pacing under a fixed load on the
# live KMD. Each round: swaprate.ps1 (d3dswap 300 frames, composition + window swap chain, present rate ~60/s = DWM
# composes every vblank) while the KMD heartbeat is sampled (vsync / present counters); then a 30 s idle sample.
# Output -> traces/g18-pacing-TAG.txt. Run on both KMDs with the same TAG prefix and compare.
TAG=${1:?tag}; R=${2:-3}; OUT=traces/g18-pacing-$TAG.txt
scp -O -q tools/phone/wddm2/swaprate.ps1 s10e:C:/S10eTmp/ < /dev/null
ssh s10e 'taskkill /f /im scrnsave.scr >nul 2>&1' < /dev/null
{
  echo "KMD $(ssh s10e 'certutil -hashfile C:\Windows\System32\DriverStore\FileRepository\malig76wddm.inf_arm64_6557c1d415c2ef25\MaliG76Wddm.sys SHA256' < /dev/null | sed -n 2p | cut -c1-8)  $(date +%T)"
  for i in $(seq 1 "$R"); do
    echo "=== round $i load"
    bash tools/phone/wddm2/wddm2dbg.sh hb 1
    ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\swaprate.ps1' < /dev/null
    bash tools/phone/wddm2/wddm2dbg.sh hb 1
    echo "=== round $i idle 30 s"
    bash tools/phone/wddm2/wddm2dbg.sh hb 1; sleep 30; bash tools/phone/wddm2/wddm2dbg.sh hb 1
  done
} 2>&1 | tee "$OUT"
