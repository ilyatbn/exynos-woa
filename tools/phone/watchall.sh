#!/bin/bash
# watchall.sh [kd.log] - prints one line whenever the phone's state changes (Monitor source):
#   adb (twrp/android/none), Windows ping, SSH port 22, and - with a kdrun log - KD connect / successful break-ins /
#   "no successful break-in for KDGAP s" (a frozen target often shows no error at all).
# Address comes from local/phone.env (gitignored); never printed.
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
. /c/Users/ilya/code/exynos-woa/local/phone.env 2>/dev/null
KD=$1; KDGAP=${KDGAP:-45}
last=""; lastkd=""; lastok=$(date +%s); gapsaid=0
while :; do
  a=$($A devices 2>/dev/null | awk 'NR>1 && $2 {print $2; exit}'); a=${a:-none}
  raw=down; [ -n "$WINIP" ] && ping -n 1 -w 800 "$WINIP" 2>/dev/null | grep -q "TTL=" && raw=up
  # debounce: Wi-Fi power save (~10 s naps when idle) and KD break-ins drop pings - flip only after 4 equal
  # readings in a row (~20 s)
  if [ "$raw" = "$prevraw" ]; then same=$((same+1)); else same=1; fi; prevraw=$raw
  if [ -z "$p" ] || [ $same -ge 4 ]; then p=$raw; fi
  s=closed; [ "$p" = up ] && (exec 3<>/dev/tcp/$WINIP/22) 2>/dev/null && s=open
  cur="adb=$a win-ping=$p ssh=$s"
  [ "$cur" != "$last" ] && { echo "$(date +%T) $cur"; last=$cur; }
  if [ -n "$KD" ] && [ -f "$KD" ]; then
    k=$(grep -a "^==== kdrun .*\(connected\|timed break-in\|Hit breakpoint\|limit\|reset\)" "$KD" | tail -1)
    if [ -n "$k" ] && [ "$k" != "$lastkd" ]; then echo "$(date +%T) KD: ${k#==== kdrun }"; lastkd=$k; lastok=$(date +%s); gapsaid=0; fi
    if [ -n "$lastkd" ] && [ $gapsaid = 0 ] && [ $(( $(date +%s) - lastok )) -gt $KDGAP ]; then
      echo "$(date +%T) KD: no successful break-in for ${KDGAP}s - target likely frozen"; gapsaid=1; fi
  fi
  sleep 5
done
