#!/usr/bin/env bash
# Stage the M17 kernel-debug switch on the ESP from TWRP (notes/m17-usb-kd.md T4): extra-kd.cmd (or
# extra-kd-off.cmd with "off") as \S10EDIAG\extra.cmd, plus the ARM64 kdnet.exe set in \S10EDIAG\kd\.
# The S10e agent runs it as SYSTEM on the next Windows boot; the debugger attaches on the boot after that.
set -euo pipefail
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
REPO=$(cd "$(dirname "$0")/../.." && pwd)
D="/c/Program Files (x86)/Windows Kits/10/Debuggers/arm64"
S=extra-kd.cmd; [ "${1:-}" = off ] && S=extra-kd-off.cmd
T=$(mktemp)
$A shell 'umount /tmp/esp 2>/dev/null; mkdir -p /tmp/esp; mount -t vfat /dev/block/by-name/cache /tmp/esp && mkdir -p /tmp/esp/S10EDIAG/kd'
sed 's/\r*$/\r/' "$REPO/drivers/S10eDiag/esp/$S" > "$T"
$A push "$(cygpath -m "$T")" /tmp/esp/S10EDIAG/extra.cmd >/dev/null
for f in kdnet.exe kdnet-dev-ext.dll VerifiedNICList.xml; do $A push "$(cygpath -m "$D/$f")" /tmp/esp/S10EDIAG/kd/ >/dev/null; done
$A shell 'rm -f /tmp/esp/WINRUN.FLG; ls -la /tmp/esp/S10EDIAG /tmp/esp/S10EDIAG/kd; sync; umount /tmp/esp'
rm -f "$T"
echo "staged $S"
