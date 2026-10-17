#!/bin/bash
# mpvrate.sh [OPTS...] - G17 (notes/g17-dxva-mfc.md), run from the PC: play test.mp4 in mpv on the phone (user session,
# mpvshot.ps1 -After 0, no audio) once per OPTS set and print the KMD rates over 10 s of playback (kmdstats: DWM's
# presents/s = composition rate, GPU jobs/s) and the GPU clock. An OPTS set is mpv options with spaces written as +
# ("--profile=fast"); "-" = mpv's own config only. Diagnosis tool: the option sets are not solutions (AGENTS rule 12).
[ $# -eq 0 ] && set -- -
for o in "$@"; do
  [ "$o" = - ] && o=
  oa=; [ -n "$o" ] && oa="-Opts $o"
  ssh s10e "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\S10eTmp\\userrun.ps1 -Exe powershell.exe -Args \"-NoProfile -ExecutionPolicy Bypass -File C:\\S10eTmp\\mfc\\mpvshot.ps1 -After 0 -Play 22 $oa\" -Wait 60" > /dev/null 2>&1 &
  t0=$SECONDS
  until ssh s10e 'tasklist /fi "imagename eq mpv.exe" /nh' 2>/dev/null | grep -qi mpv.exe; do
    [ $((SECONDS - t0)) -gt 60 ] && { echo "${o:-(config)}: mpv did not start"; wait; continue 2; }
  done
  sleep 4
  r=$(ssh s10e 'C:\S10eTmp\kmdstats.exe 10 & C:\S10eTmp\gpuclock.exe' 2>&1 | tr -d '\r')
  echo "${o:-(config)}: $(echo "$r" | grep -o 'presents [0-9.]*\|jobs [0-9.]*' | tr '\n' ' ')$(echo "$r" | grep -o 'GPU clock [0-9.]* MHz')"
  wait
done
