#!/bin/bash
# glshot.sh NAME [DELAY] - GDI screenshot of the phone's user session into traces/gl/NAME.png. The blank screensaver
# (scrnsave.scr, ScreenSaveTimeOut 120 s) returns whenever nobody touches the phone and makes CopyFromScreen fail
# ("The handle is invalid") / the scan-out black, so it is killed right before the shot (notes/gl-icd.md,
# notes/brightness-research.md). DELAY s after the kill lets DWM compose a frame again. Needs shot.ps1, userrun.ps1 in
# C:\S10eTmp.
N=${1:?name}; D=${2:-2}
OUT=/c/Users/ilya/code/exynos-woa/traces/gl
mkdir -p "$OUT"
ssh s10e 'taskkill /f /im scrnsave.scr >nul 2>&1' < /dev/null
sleep "$D"
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\userrun.ps1 -Exe powershell.exe -Args "-NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\shot.ps1" -Wait 40' < /dev/null
scp -O -q s10e:C:/S10eTmp/shot.png "$OUT/$N.png" && echo "$OUT/$N.png"
