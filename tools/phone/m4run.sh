#!/bin/bash
# m4run.sh IMAGE [GICC...] - one M4 park-stub test cycle (notes/m4-cores.md): Windows -> TWRP (totwrp.cmd), flash IMAGE
# with bootflash.sh (backup + readback + WINRUN.FLG removal), reboot, wait for SSH, print the UEFILOG PARK STRESS /
# ATOMIC / PMU lines and the live mailbox counters (m4mb.sh, default GICC 3 6 7). Run from the repo root.
set -u
IMG=${1:?usage: m4run.sh IMAGE [GICC...]}; shift
A=/c/Users/ilya/code/platform-tools/adb.exe
. local/phone.env
if [ "$($A get-state 2>/dev/null)" != recovery ]; then
  ssh -o ConnectTimeout=10 s10e 'C:\S10eTmp\totwrp.cmd' < /dev/null | tail -1
  for i in $(seq 1 60); do [ "$($A get-state 2>/dev/null)" = recovery ] && break; sleep 5; done
fi
bash tools/phone/bootflash.sh "$IMG" 2>&1 | tail -1 || exit 1
$A reboot; echo "rebooted $(date +%T)"; sleep 45
for i in $(seq 1 80); do (exec 3<>/dev/tcp/$WINIP/22) 2>/dev/null && { echo "ssh up $(date +%T)"; break; }; sleep 5; done
sleep 5
ssh -o ConnectTimeout=10 s10e 'findstr /c:"PARK: STRESS gicc" /c:"PARK: ATOMIC" /c:"PARK: PMU" S:\UEFILOG.TXT' < /dev/null | grep -v scratch
[ $# -eq 0 ] && set -- 3 6 7
bash tools/phone/m4mb.sh "$@"
