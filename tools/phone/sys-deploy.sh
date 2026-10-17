#!/bin/bash
# sys-deploy.sh INFNAME SYSFILE - from TWRP, swap a driver .sys into its DriverStore folder
# (FileRepository/<infname>_arm64_*), so the next Windows boot runs it (memory driver-swap-from-twrp). The replaced
# file is kept as C:\S10eTmp\sys-prev\<sys>. A service that runs from Windows\System32\drivers (copied there at
# install, e.g. Exynos9820Mfc - check with drvhash.ps1) gets that copy replaced too (old one: sys-prev\<sys>.drivers).
# Same rules as kmd-deploy.sh: read-write only if ntfs-3g mounts rw on its own, never fsck/ntfsfix; sha256 checked on the device; unmounts. Example:
#   sys-deploy.sh s6sy761touchbeyond0.inf s10e-artifacts/touch-6.0.5058.7/S6SY761TouchBeyond0.sys
set -e
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
INF=${1:?usage: sys-deploy.sh INFNAME SYSFILE}
S=$(cygpath -m "${2:?usage: sys-deploy.sh INFNAME SYSFILE}")
N=$(basename "$S")
[ -f "$S" ] || { echo "missing $S"; exit 1; }
$A shell 'umount /tmp/win 2>/dev/null; umount /data 2>/dev/null; mkdir -p /tmp/win; mount.ntfs /dev/block/by-name/userdata /tmp/win >/dev/null 2>&1; true'
if ! $A shell 'mount | grep -q " /tmp/win .*(rw"'; then
  $A shell 'umount /tmp/win 2>/dev/null; true'; echo "NTFS not mounted read-write (unclean?) - nothing copied"; exit 2
fi
R=$($A shell "ls -d /tmp/win/Windows/System32/DriverStore/FileRepository/${INF}_arm64_* 2>/dev/null" | tr -d '\r' | head -1)
if [ -z "$R" ] || ! $A shell "[ -f $R/$N ]"; then
  $A shell 'umount /tmp/win; true'; echo "no $N in a ${INF}_arm64_* DriverStore folder"; exit 4
fi
$A shell "mkdir -p /tmp/win/S10eTmp/sys-prev && cp $R/$N /tmp/win/S10eTmp/sys-prev/$N"
$A push "$S" "$R/$N" >/dev/null
want=$(sha256sum "$S" | cut -d' ' -f1)
D=/tmp/win/Windows/System32/drivers/$N
if $A shell "[ -f $D ]"; then
  $A shell "cp $D /tmp/win/S10eTmp/sys-prev/$N.drivers && cp $R/$N $D"
  dgot=$($A shell "sha256sum $D" | cut -d' ' -f1 | tr -d '\r')
  [ "$dgot" = "$want" ] && echo "System32/drivers copy replaced too" || { $A shell 'sync; umount /tmp/win'; echo "System32 drivers HASH MISMATCH: $dgot"; exit 3; }
fi
got=$($A shell "sha256sum $R/$N" | cut -d' ' -f1 | tr -d '\r')
prev=$($A shell "sha256sum /tmp/win/S10eTmp/sys-prev/$N" | cut -d' ' -f1 | tr -d '\r')
$A shell 'sync; umount /tmp/win'
echo "folder $R"; echo "previous $prev"
[ "$got" = "$want" ] && echo "deployed $want" || { echo "HASH MISMATCH: want $want got $got"; exit 3; }
