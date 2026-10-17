#!/bin/bash
# bootflash.sh IMAGE [BACKUP_DIR] - from TWRP: back up the current boot partition (same length as IMAGE) to BACKUP_DIR
# (default s10e-artifacts/boot-backups), print its sha256 and which known s10e-artifacts image it matches, flash IMAGE
# to by-name/boot, verify the readback, remove /cache/WINRUN.FLG (AGENTS.md / m14 pitfall). Never touches anything
# but by-name/boot. Roll back: bootflash.sh <the backup file>.
set -e
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
IMG=${1:?usage: bootflash.sh IMAGE [BACKUP_DIR]}
ART=/c/Users/ilya/code/s10e-artifacts
BK=${2:-$ART/boot-backups}
mkdir -p "$BK"
$A get-state 2>/dev/null | grep -q recovery || { echo "phone not in TWRP"; exit 1; }
size=$(stat -c %s "$IMG")
[ $((size % 4096)) -eq 0 ] || { echo "image size $size not a multiple of 4096"; exit 1; }
blocks=$((size / 4096))
want=$(sha256sum "$IMG" | cut -d' ' -f1)
stamp=$(date +%Y%m%d-%H%M%S)
$A shell "dd if=/dev/block/by-name/boot of=/tmp/boot-backup.img bs=4096 count=$blocks 2>/dev/null"
$A pull /tmp/boot-backup.img "$(cygpath -m "$BK")/boot-before-$stamp.img" >/dev/null   # adb is a Windows program: Windows paths
cur=$(sha256sum "$BK/boot-before-$stamp.img" | cut -d' ' -f1)
echo "current boot: $cur -> $BK/boot-before-$stamp.img"
for f in "$ART"/*.img; do
  [ "$(stat -c %s "$f")" -eq "$size" ] || continue
  [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$cur" ] && echo "  = $(basename "$f")"
done
$A push "$(cygpath -m "$IMG")" /tmp/boot-new.img >/dev/null
$A shell "dd if=/tmp/boot-new.img of=/dev/block/by-name/boot bs=4096 2>/dev/null; sync"
got=$($A shell "dd if=/dev/block/by-name/boot bs=4096 count=$blocks 2>/dev/null | sha256sum" | cut -d' ' -f1 | tr -d '\r')
$A shell 'rm -f /cache/WINRUN.FLG /tmp/boot-new.img /tmp/boot-backup.img; true'
[ "$got" = "$want" ] && echo "flashed $want (readback OK)" || { echo "READBACK MISMATCH: want $want got $got"; exit 3; }
