#!/bin/bash
# kmd-deploy.sh DIR - from TWRP, swap a MaliG76Wddm build (DIR = s10e-artifacts\mali-kmd-NN with MaliG76Wddm.sys) into
# the DriverStore folder the service loads it from (notes/gpu-hw-path.md: the hive's ImagePath), so the next boot runs
# it. The .sys it replaces is kept as C:\S10eTmp\kmd-prev\MaliG76Wddm.sys (roll back by running this on that folder).
# Same rules as umd-deploy.sh: read-write only if ntfs-3g gives rw on its own, never fsck/ntfsfix; sha256 checked on
# the device; unmounts.
set -e
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
D=$(cygpath -m "${1:?usage: kmd-deploy.sh DIR}")
R=/tmp/win/Windows/System32/DriverStore/FileRepository/malig76wddm.inf_arm64_6557c1d415c2ef25
[ -f "$D/MaliG76Wddm.sys" ] || { echo "missing $D/MaliG76Wddm.sys"; exit 1; }
$A shell 'umount /tmp/win 2>/dev/null; umount /data 2>/dev/null; mkdir -p /tmp/win; mount.ntfs /dev/block/by-name/userdata /tmp/win >/dev/null 2>&1; true'
if ! $A shell 'mount | grep -q " /tmp/win .*(rw"'; then
  $A shell 'umount /tmp/win 2>/dev/null; true'
  echo "NTFS not mounted read-write (unclean?) - nothing copied"; exit 2
fi
if ! $A shell "[ -f $R/MaliG76Wddm.sys ]"; then
  $A shell 'umount /tmp/win; true'; echo "no $R/MaliG76Wddm.sys - DriverStore folder changed?"; exit 4
fi
$A shell "mkdir -p /tmp/win/S10eTmp/kmd-prev && cp $R/MaliG76Wddm.sys /tmp/win/S10eTmp/kmd-prev/MaliG76Wddm.sys"
$A push "$D/MaliG76Wddm.sys" $R/MaliG76Wddm.sys >/dev/null
want=$(sha256sum "$D/MaliG76Wddm.sys" | cut -d' ' -f1)
got=$($A shell "sha256sum $R/MaliG76Wddm.sys" | cut -d' ' -f1 | tr -d '\r')
prev=$($A shell "sha256sum /tmp/win/S10eTmp/kmd-prev/MaliG76Wddm.sys" | cut -d' ' -f1 | tr -d '\r')
$A shell 'sync; umount /tmp/win'
echo "previous $prev"
[ "$got" = "$want" ] && echo "deployed $want" || { echo "HASH MISMATCH: want $want got $got"; exit 3; }
