#!/usr/bin/env bash
# Install S10eDiag (driver + agent + CLI) into the Windows image on the S10e, from TWRP.
# Usage: s10e-install-diag.sh <build dir with S10eDiag.sys s10ediag.exe S10eAgent.exe>
# notes/m11-s10ediag.md. Needs an elevated shell (reg load) and the phone in TWRP.
set -euo pipefail
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
B=${1:?build dir}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
W=$(mktemp -d "${TEMP:-/tmp}/s10ediag-hive.XXXX")

$A shell 'umount /data 2>/dev/null; umount /tmp/win 2>/dev/null; umount /tmp/esp 2>/dev/null; mkdir -p /tmp/win /tmp/esp; true'
# A forced reset leaves NTFS's journal dirty and ntfs-3g refuses rw (W37) - ntfsfix resets it (Windows chkdsks).
if ! $A shell 'mount.ntfs /dev/block/by-name/userdata /tmp/win 2>&1 && mount | grep -q "/tmp/win.*rw"'; then
  $A shell 'umount /tmp/win 2>/dev/null; fsck.ntfs /dev/block/by-name/userdata | tail -1; mount.ntfs /dev/block/by-name/userdata /tmp/win'
  $A shell 'mount | grep -q "/tmp/win.*rw"' || { echo "NTFS not writable"; exit 1; }
fi

C=/tmp/win/Windows/System32/config
for f in SYSTEM SYSTEM.LOG1 SYSTEM.LOG2; do $A pull "$C/$f" "$W/$f" >/dev/null; done
cp "$W/SYSTEM" "$W/SYSTEM.orig"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$REPO/firmware/tools/s10e-hive-diag.ps1")" -Dir "$(cygpath -w "$W")"
for f in SYSTEM SYSTEM.LOG1 SYSTEM.LOG2; do $A push "$(cygpath -m "$W/$f")" "$C/$f" >/dev/null; done

$A push "$(cygpath -m "$B/S10eDiag.sys")" /tmp/win/Windows/System32/drivers/S10eDiag.sys >/dev/null
$A push "$(cygpath -m "$B/s10ediag.exe")" /tmp/win/Windows/System32/s10ediag.exe >/dev/null
$A push "$(cygpath -m "$B/S10eAgent.exe")" /tmp/win/Windows/System32/S10eAgent.exe >/dev/null
$A shell "cd /tmp/win/Windows/System32 && sha256sum config/SYSTEM drivers/S10eDiag.sys s10ediag.exe S10eAgent.exe; cd /; sync; umount /tmp/win"
(cd "$W" && sha256sum SYSTEM) ; (cd "$B" && sha256sum S10eDiag.sys s10ediag.exe S10eAgent.exe)

# ESP side: the script the agent runs (CRLF for cmd.exe).
$A shell 'mount -t vfat /dev/block/by-name/cache /tmp/esp && mkdir -p /tmp/esp/S10EDIAG'
sed 's/\r*$/\r/' "$REPO/drivers/S10eDiag/esp/collect.cmd" > "$W/collect.cmd"
$A push "$(cygpath -m "$W/collect.cmd")" /tmp/esp/S10EDIAG/collect.cmd >/dev/null
$A shell 'ls -la /tmp/esp/S10EDIAG; sync; umount /tmp/esp'
echo "installed; hive work dir $W"
