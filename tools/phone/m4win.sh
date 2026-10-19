#!/bin/bash
# m4win.sh IMAGE [MINUTES] - Windows-level M4 trial (notes/m4-cores.md): TWRP (totwrp.cmd), flash IMAGE (bootflash.sh),
# reboot, then every 30 s for MINUTES (default 6): logical processors + uptime over SSH, or adb state. A falling uptime =
# Windows restarted (death); adb=recovery = the stay.flg safeguard gave up after 2 deaths. Run from the repo root.
set -u
IMG=${1:?usage: m4win.sh IMAGE [MINUTES]}; MIN=${2:-6}
A=/c/Users/ilya/code/platform-tools/adb.exe
. local/phone.env
if [ "$($A get-state 2>/dev/null)" != recovery ]; then
  ssh -o ConnectTimeout=10 s10e 'C:\S10eTmp\totwrp.cmd' < /dev/null | tail -1
  for i in $(seq 1 60); do [ "$($A get-state 2>/dev/null)" = recovery ] && break; sleep 5; done
fi
bash tools/phone/bootflash.sh "$IMG" 2>&1 | tail -1 || exit 1
$A reboot; t0=$(date +%s); echo "rebooted $(date +%T)"
while [ $(( $(date +%s) - t0 )) -lt $((MIN * 60)) ]; do
  sleep 30
  s=$($A get-state 2>/dev/null)
  if [ "$s" = recovery ]; then echo "$(date +%T) +$(( $(date +%s) - t0 ))s TWRP (safeguard)"; break; fi
  r=$(timeout 25 ssh -o ConnectTimeout=8 s10e 'powershell -NoProfile -c "\"procs $((Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors) up $([int]([Environment]::TickCount/1000))s\""' < /dev/null 2>/dev/null | tail -1)
  echo "$(date +%T) +$(( $(date +%s) - t0 ))s ${r:-no ssh}"
done
