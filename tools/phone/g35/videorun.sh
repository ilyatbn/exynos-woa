#!/bin/bash
# videorun.sh BROWSER TAG [WAIT] - G35: kill the screensaver, run browservideo.ps1 in the user session, probe the MFC
# (mfcdec2 on c720.h264: fails while another process holds the MFC's one session) ~WAIT-5 s into playback, fetch the
# shot -> traces/g35-TAG.png. Run from the repo root.
B=${1:?browser msedge|brave}; TAG=${2:?tag}; W=${3:-20}
scp -O -q tools/phone/g35/browservideo.ps1 s10e:C:/S10eTmp/ < /dev/null
ssh s10e 'taskkill /f /im scrnsave.scr >nul 2>&1' < /dev/null
ssh s10e "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\runuser2.ps1 -Cmd \"powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\browservideo.ps1 -Browser $B -Wait $W\" -Wait $((W+40))" < /dev/null &
sleep $((W > 10 ? W - 2 : W))
echo "MFC probe: $(ssh s10e 'C:\S10eTmp\mfc\mfcdec2.exe C:\S10eTmp\mfc\c720.h264 -q 2>&1' < /dev/null | head -2 | tr '\r\n' '  ')"
wait
scp -O -q "s10e:C:/S10eTmp/g35/$B.png" "traces/g35-$TAG.png" && echo "saved traces/g35-$TAG.png"
