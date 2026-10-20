#!/bin/bash
# micgaintest.sh [GAINS="0 24 48"] - mic boost check through the Windows endpoint (notes/d8-mic.md): for each gain
# (0.5 dB steps) sets it with the app's CLI (S10eSpeakerTrim.exe mic N, the AUD_PROPERTY_MICGAIN property), records
# 3 s with wascap.exe while the PC plays a quiet 1 kHz tone, and analyses the 1 kHz level (micanalyze.py --float).
# The tone level must stay constant across runs: the PC speaker level/mute are set once and restored at the end.
R=/c/Users/ilya/code/exynos-woa; OUT=$R/traces/audio; mkdir -p $OUT; GAINS=${1:-"0 24 48"}
VOL='powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\ilya\code\exynos-woa\tools\phone\audio\pcvolume.ps1'
OLD=$($VOL | tr -d ''); OLDM=$(echo "$OLD" | awk '{print $2}'); OLDL=$(echo "$OLD" | awk '{print $4}')
$VOL -Level ${PCLEVEL:-0.30} -Mute 0 > /dev/null
for G in $GAINS; do
  ssh s10e "C:\S10eTmp\S10eSpeakerTrim.exe mic $G" < /dev/null | tr -d '\r'
  python $R/tools/phone/audio/pctone.py 6 --freq 1000 & TP=$!; sleep 1
  ssh s10e 'C:\S10eTmp\wascap.exe 3 C:\S10eTmp\wascap-g.raw' < /dev/null | tr -d '\r' | tail -1
  wait $TP
  scp -O -q s10e:C:/S10eTmp/wascap-g.raw $OUT/wascap-g$G.raw && \
    python $R/tools/phone/audio/micanalyze.py $OUT/wascap-g$G.raw --float --ch 2 --skip 0.3 | grep -o '^ch.*1000 Hz band [-0-9.]* dB'
done
$VOL -Level $OLDL -Mute $OLDM
