#!/usr/bin/env bash
# Replace the Windows UFS miniport (Exynos9820Ufs.sys) in the S10e image, from TWRP.
# Usage: s10e-swap-ufs.sh <signed Exynos9820Ufs.sys>
# The driver lives only in its DriverStore folder (the service ImagePath points there). Refuses to push unless
# the NTFS volume is mounted read-write (run 26: a missing mount point made adb push land in TWRP's tmpfs).
# Also clears \WINRUN.FLG on the ESP so the next boot goes into Windows. notes/m15-perf.md.
set -euo pipefail
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
SYS=${1:?signed Exynos9820Ufs.sys}
D=/tmp/win/Windows/System32/DriverStore/FileRepository/exynos9820ufs.inf_arm64_4efdc4525d11d997/Exynos9820Ufs.sys

$A shell 'umount /data 2>/dev/null; umount /tmp/win 2>/dev/null; umount /tmp/esp 2>/dev/null; mkdir -p /tmp/win /tmp/esp; true'
if ! $A shell 'mount.ntfs /dev/block/by-name/userdata /tmp/win >/dev/null 2>&1; mount | grep -q "/tmp/win.*rw"'; then
  # A forced/hard reset leaves the NTFS journal dirty; ntfsfix resets it (Windows chkdsks on its next boot).
  $A shell 'umount /tmp/win 2>/dev/null; fsck.ntfs /dev/block/by-name/userdata | tail -1; mount.ntfs /dev/block/by-name/userdata /tmp/win'
fi
$A shell "mount | grep -q '/tmp/win.*rw' && [ -f $D ]" || { echo "NTFS not mounted rw or driver path missing - nothing pushed"; exit 1; }
$A push "$(cygpath -m "$SYS")" "$D" >/dev/null
$A shell "sha256sum $D; sync; umount /tmp/win"
sha256sum "$SYS"
$A shell 'mount -t vfat /dev/block/by-name/cache /tmp/esp && rm -f /tmp/esp/WINRUN.FLG; sync; umount /tmp/esp'
echo "swapped"
