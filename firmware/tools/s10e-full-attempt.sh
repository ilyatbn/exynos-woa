#!/bin/bash
# full attempt: fresh image transfer + boot image $1 + driver $2 + cycle into $3
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
until $A devices | grep -q recovery; do sleep 5; done
$A shell 'umount /tmp/win 2>/dev/null; umount /tmp/esp 2>/dev/null; true'
"$TEMP/s10eh3/S10eHarness.exe" transfer 'C:\Users\ilya\code\platform-tools\adb.exe' RF8M21HYDSZ 'C:\Users\ilya\code\s10e-artifacts\win-w5' 2>&1 | tail -2
$A push "$(cygpath -w "$1")" /tmp/bootX.img >/dev/null && $A shell "dd if=/tmp/bootX.img of=/dev/block/by-name/boot bs=4096 conv=fsync 2>/dev/null && sync && echo flashed \$(head -c 7587840 /dev/block/by-name/boot | sha256sum | cut -c1-16)"
bash "$TEMP/cycle.sh" "$2" "$3"
$A shell 'mkdir -p /tmp/win; umount /data 2>/dev/null; mount.ntfs -o ro /dev/block/by-name/userdata /tmp/win >/dev/null 2>&1; ls -la /tmp/win/Windows/Panther /tmp/win/Users'
mkdir -p "$3/panther"
for f in setupact.log setuperr.log; do $A pull "/tmp/win/Windows/Panther/$f" "$(cygpath -w "$3/panther/$f")" >/dev/null 2>&1; done
$A pull /tmp/win/Windows/Panther/UnattendGC "$(cygpath -w "$3/panther/UnattendGC")" >/dev/null 2>&1
$A shell 'umount /tmp/win'
grep -a 'progress prev' "$3/UEFILOG.txt"
echo FULL-DONE
