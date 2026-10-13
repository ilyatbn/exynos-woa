#!/bin/bash
# phonestate.sh           - one line: time, state, details
# phonestate.sh wait S T  - poll every 10 s until state S (twrp|windows|android|any-change) or T seconds; prints
#                           every state change; exit 0 when reached, 1 on timeout.
# States: twrp (adb recovery), android (adb device), sideload/unauthorized/offline (adb other),
#         windows (no adb, Windows answers ping on the laptop hotspot), dark (none of these: booting, UEFI, hung,
#         download mode or off).
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
WINIP=${WINIP:-192.168.137.90}     # phone's Windows WiFi (MAC a8:db:03:06:3c:ef, notes/m12-wifi-wificx.md)

state() {
  local d
  d=$($A devices 2>/dev/null | awk 'NR>1 && $2 {print $2; exit}')
  case "$d" in
    recovery) echo twrp; return ;;
    device) echo android; return ;;
    "") ;;
    *) echo "$d"; return ;;
  esac
  if ping -n 1 -w 1000 "$WINIP" 2>/dev/null | grep -q "TTL="; then echo windows; else echo dark; fi
}

details() {
  case "$1" in
    twrp) echo "bat $($A shell cat /sys/class/power_supply/battery/capacity | tr -d '\r')%" \
               "esp $($A shell df /cache | tr -d '\r' | tail -1 | awk '{print $(NF-1)}')" \
               "flags: $($A shell 'cd /cache/S10EDIAG && ls *.flg gpu/*.flg 2>/dev/null' | tr -d '\r' | tr '\n' ' ')" ;;
    windows) echo "ping ok $WINIP" ;;
    *) echo "" ;;
  esac
}

if [ "$1" = wait ]; then
  want=$2; limit=${3:-1800}; t0=$(date +%s); last=""
  while :; do
    s=$(state)
    if [ "$s" != "$last" ]; then echo "$(date +%T) $s $(details "$s")"; fi
    if [ "$want" = "$s" ] || { [ "$want" = any-change ] && [ -n "$last" ] && [ "$s" != "$last" ]; }; then exit 0; fi
    last=$s
    [ $(( $(date +%s) - t0 )) -ge "$limit" ] && { echo "$(date +%T) timeout after ${limit}s, state $s"; exit 1; }
    sleep 10
  done
fi
s=$(state); echo "$(date +%T) $s $(details "$s")"
