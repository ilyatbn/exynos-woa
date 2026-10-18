#!/bin/bash
# pramget.sh NAME - from TWRP: pull \WINPRAM*.BIN (saved by the firmware on the boot after an unclean Windows run) from
# the ESP (by-name/cache, vfat) to traces/pram/NAME/, then print the S10eDiag ring (+0xA000: "BC <code>" = bugcheck)
# and the tail of the KMD DDI trace (+0x4000). HANDOFF 54: "WINPRAM decode = dd + strings". Run from the repo root.
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
N=${1:?usage: pramget.sh NAME}; D=traces/pram/$N
mkdir -p "$D"
$A shell 'mkdir -p /tmp/esp; mountpoint -q /tmp/esp || mount -t vfat /dev/block/by-name/cache /tmp/esp; ls -la /tmp/esp/ | grep -i -E "pram|pcsamp|\.flg"'
for f in $($A shell 'ls /tmp/esp/ | grep -i -E "^(WINPRAM|PCSAMP).*\.BIN$"' | tr -d '\r'); do $A pull "/tmp/esp/$f" "$D/" >/dev/null && echo "pulled $f"; done
$A shell 'umount /tmp/esp 2>/dev/null; true'
# strings(1) is not in Git Bash: printable runs >= 6 chars of FILE[OFF:OFF+LEN]
STR='import re,sys; d=open(sys.argv[1],"rb").read()[int(sys.argv[2],0):int(sys.argv[2],0)+int(sys.argv[3],0)]; [print(m.decode()) for m in re.findall(rb"[ -~]{6,}", d)]'
for f in "$D"/WINPRAM*.BIN; do
  [ -f "$f" ] || continue
  echo "== $f ($(stat -c %s "$f") bytes)"
  echo "-- S10eDiag ring (+0xA000):"; python -c "$STR" "$f" 0xA000 0x6000 | tail -25
  echo "-- KMD trace (+0x4000) tail:"; python -c "$STR" "$f" 0x4000 0x4000 | tail -15
done
