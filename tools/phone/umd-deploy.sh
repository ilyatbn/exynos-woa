#!/bin/bash
# umd-deploy.sh DIR - from TWRP, install a Mesa UMD build (DIR = s10e-artifacts\mali-umd-NNNN with maliumd.dll and
# libgallium_d3d10.dll) into Windows\System32 on the Windows NTFS (userdata), so DWM loads it at the next boot
# (notes/gpu-hw-path.md). Mounts read-write only if ntfs-3g gives rw on its own - never runs fsck/ntfsfix (an
# unclean journal after a hard reset means: boot Windows once cleanly first). Verifies sha256 on the device, clears
# C:\S10eTmp\dumps (WER LocalDumps of the next boot), unmounts.
set -e
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
D=$(cygpath -m "${1:?usage: umd-deploy.sh DIR}")
for f in maliumd.dll libgallium_d3d10.dll; do [ -f "$D/$f" ] || { echo "missing $D/$f"; exit 1; }; done
$A shell 'umount /tmp/win 2>/dev/null; umount /data 2>/dev/null; mkdir -p /tmp/win; mount.ntfs /dev/block/by-name/userdata /tmp/win >/dev/null 2>&1; true'
if ! $A shell 'mount | grep -q " /tmp/win .*(rw"'; then
  $A shell 'umount /tmp/win 2>/dev/null; true'
  echo "NTFS not mounted read-write (unclean?) - nothing copied"; exit 2
fi
for f in maliumd.dll libgallium_d3d10.dll; do $A push "$D/$f" /tmp/win/Windows/System32/$f >/dev/null; done
want=$(sha256sum "$D/maliumd.dll" | cut -d' ' -f1)
got=$($A shell 'sha256sum /tmp/win/Windows/System32/maliumd.dll /tmp/win/Windows/System32/libgallium_d3d10.dll' | cut -d' ' -f1 | sort -u | tr -d '\r')
$A shell 'rm -f /tmp/win/S10eTmp/dumps/*.dmp; sync; umount /tmp/win'
[ "$got" = "$want" ] && echo "deployed $want" || { echo "HASH MISMATCH: want $want got $got"; exit 3; }
