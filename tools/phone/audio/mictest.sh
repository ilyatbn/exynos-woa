#!/bin/bash
# mictest.sh [MS=3000] [TONE_HZ] [--install DIR] - one Exynos9820Audio MicSelfTest run (notes/d8-mic.md): optional driver
# install from DIR (pnputil), sets Parameters\MicTest (one-shot, the driver clears it), restarts the audio device while
# the PC plays TONE_HZ on its speakers (tools/phone/audio/pctone.py, quiet), then prints the mic lines of the run log and
# analyses the raw file (micanalyze.py). Raw file kept as traces/audio/mic-<time>.raw.
R=/c/Users/ilya/code/exynos-woa; MS=${1:-3000}; TONE=$2; OUT=$R/traces/audio; mkdir -p $OUT
if [ "$3" = "--install" ]; then
  ssh s10e "mkdir C:\S10eTmp\audpkg 2>nul & del /q C:\S10eTmp\audpkg\*" < /dev/null
  scp -O -q "$4"/* s10e:C:/S10eTmp/audpkg/ && ssh s10e 'pnputil /add-driver C:\S10eTmp\audpkg\Exynos9820Audio.inf /install' < /dev/null | tail -2
  sleep 6
fi
ssh s10e "del C:\Windows\System32\LogFiles\Exynos9820Audio-mic.raw 2>nul & reg add HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters /v MicTest /t REG_DWORD /d $MS /f >nul" < /dev/null
VOL='powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\ilya\code\exynos-woa\tools\phone\audio\pcvolume.ps1'
if [ -n "$TONE" ]; then
  # the PC speaker endpoint is normally muted: unmute at a modest level for the tone only, restore afterwards
  OLD=$($VOL | tr -d ''); OLDM=$(echo "$OLD" | awk '{print $2}'); OLDL=$(echo "$OLD" | awk '{print $4}')
  $VOL -Level ${PCLEVEL:-0.30} -Mute 0 > /dev/null
  python $R/tools/phone/audio/pctone.py $(( MS / 1000 + 8 )) --freq $TONE & TP=$!; sleep 1
fi
ssh s10e 'pnputil /restart-device ACPI\SAMS9827\0' < /dev/null | tail -1
sleep $(( MS / 1000 + 4 ))
[ -n "$TONE" ] && { wait $TP; $VOL -Level $OLDL -Mute $OLDM; }
scp -O -q $R/tools/phone/audio/miclog.ps1 s10e:C:/S10eTmp/ && ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\miclog.ps1' < /dev/null | cut -c1-260
T=$(date +%H%M%S)
if scp -O -q s10e:C:/Windows/System32/LogFiles/Exynos9820Audio-mic.raw $OUT/mic-$T.raw 2>/dev/null; then
  python $R/tools/phone/audio/micanalyze.py $OUT/mic-$T.raw --freq ${TONE:-1000}
fi
