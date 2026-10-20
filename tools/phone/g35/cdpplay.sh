#!/bin/bash
# cdpplay.sh TAG [SANDBOX=nosb|sb] [WAIT] - G35: fresh CDP Edge instance (sb = default sandbox, nosb = --no-sandbox so
# the GPU process writes its UMD log), play g35/play.html via cdp_nav.py (media log) -> traces/g35-media-TAG.txt,
# GPU-process UMD log -> traces/g35-gpu-TAG.txt (nosb only), MFC busy probe during playback. Tracing only (rule 12).
TAG=${1:?tag}; SB=${2:-nosb}; W=${3:-15}
EX=''; [ "$SB" = nosb ] && EX='-Extra --no-sandbox'
scp -O -q tools/phone/wddm2/edgecdp.ps1 tools/phone/g35/browserprocs.ps1 tools/phone/g35/browservideo.ps1 s10e:C:/S10eTmp/ < /dev/null
ssh s10e 'taskkill /f /im msedge.exe >nul 2>&1' < /dev/null; sleep 2
ssh s10e "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\runuser2.ps1 -Cmd \"powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\edgecdp.ps1 $EX\" -Wait 20" < /dev/null > /dev/null
sleep 12
ssh -N -L 9222:127.0.0.1:9222 s10e & TUN=$!; sleep 3
python tools/phone/g35/cdp_nav.py file:///C:/S10eTmp/g35/play.html 9222 "$W" > "traces/g35-media-$TAG.txt" 2>&1 &
NAV=$!; sleep $((W - 3))
echo "MFC probe: $(ssh s10e 'C:\S10eTmp\mfc\mfcdec2.exe C:\S10eTmp\mfc\c720.h264 -q 2>&1' < /dev/null | head -1 | tr -d '\r')"
wait $NAV; kill $TUN
grep -i "decoder\|error\|page:" "traces/g35-media-$TAG.txt" | grep -v "Cannot select\|kAudio" | head -20
G=$(ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\browserprocs.ps1' < /dev/null | awk '/gpu-process/{print $1" "$3}')
echo "gpu-process: $G"
for g in $(echo "$G" | awk '{print $1}'); do
  ssh s10e "type C:\S10eTmp\tlog\malikm-msedge.exe-$g.txt" < /dev/null > "traces/g35-gpu-$TAG-$g.txt" 2>/dev/null
  grep "video:" "traces/g35-gpu-$TAG-$g.txt" | grep -v "buffer info\|config count\|buffer type" | tail -12
done
