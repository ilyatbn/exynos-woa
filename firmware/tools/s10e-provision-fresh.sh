#!/usr/bin/env bash
# Provision a freshly transferred Windows image (S10eHarness transfer) from TWRP, no elevated PC needed:
# swaps the UFS miniport, stages S10eDiag + touch packages in C:\S10eProv and places SetupComplete.cmd
# (s10e-setupcomplete.cmd) which installs them as SYSTEM at the end of Windows Setup. Refreshes the ESP's
# S10EDIAG scripts (no extra.cmd) and clears \WINRUN.FLG. notes/m16-usb.md.
# Usage: s10e-provision-fresh.sh <Exynos9820Ufs.sys> <s10ediag build dir> <hsi2c pkg dir> <touch pkg dir> <test cert>
set -euo pipefail
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
REPO=$(cd "$(dirname "$0")/../.." && pwd)
UFS=${1:?ufs sys}; DIAG=${2:?s10ediag dir}; HSI2C=${3:?hsi2c pkg}; TOUCH=${4:?touch pkg}; CERT=${5:?test cert}
W=/tmp/win
U=$W/Windows/System32/DriverStore/FileRepository/exynos9820ufs.inf_arm64_4efdc4525d11d997/Exynos9820Ufs.sys
m() { cygpath -m "$1"; }

$A shell 'umount /data 2>/dev/null; umount /tmp/win 2>/dev/null; umount /tmp/esp 2>/dev/null; mkdir -p /tmp/win /tmp/esp; true'
$A shell "mount.ntfs /dev/block/by-name/userdata $W >/dev/null 2>&1; mount | grep -q '$W.*rw' && [ -f $U ]" \
  || { echo "NTFS not mounted rw or UFS driver path missing - nothing pushed"; exit 1; }

$A push "$(m "$UFS")" "$U" >/dev/null
$A shell "mkdir -p $W/S10eProv/s10ediag $W/S10eProv/drivers/Exynos9820Hsi2c $W/S10eProv/drivers/S6SY761TouchBeyond0 $W/Windows/Setup/Scripts"
for f in S10eDiag.sys s10ediag.exe S10eAgent.exe s10erotate.exe s10eperf.exe s10etouchlog.exe s10ereboot.exe s10escale.exe s10ekeys.exe s10eshadercache.exe; do
  $A push "$(m "$DIAG/$f")" "$W/S10eProv/s10ediag/$f" >/dev/null
done
for f in "$HSI2C"/*; do $A push "$(m "$f")" "$W/S10eProv/drivers/Exynos9820Hsi2c/" >/dev/null; done
for f in "$TOUCH"/*; do $A push "$(m "$f")" "$W/S10eProv/drivers/S6SY761TouchBeyond0/" >/dev/null; done
$A push "$(m "$CERT")" "$W/S10eProv/drivers/wdktestcert.cer" >/dev/null
T=$(mktemp)
sed 's/\r*$/\r/' "$REPO/firmware/tools/s10e-setupcomplete.cmd" > "$T"
$A push "$(m "$T")" "$W/Windows/Setup/Scripts/SetupComplete.cmd" >/dev/null
$A shell "cd $W && sha256sum ${U#$W/} S10eProv/s10ediag/* S10eProv/drivers/*/* Windows/Setup/Scripts/*; cd /; sync"
# Right after the pushes ntfs-3g can still report busy (first run, 2026-10-11); retry before giving up.
$A shell "for i in 1 2 3 4 5; do umount $W 2>/dev/null && exit 0; sleep 2; done; exit 1" || { echo "NTFS still mounted"; exit 1; }
sha256sum "$UFS"

# ESP: the agent's scripts (CRLF), no extra step, no boot-pair flag.
$A shell 'mount -t vfat /dev/block/by-name/cache /tmp/esp && mkdir -p /tmp/esp/S10EDIAG'
sed 's/\r*$/\r/' "$REPO/drivers/S10eDiag/esp/collect.cmd" > "$T"; $A push "$(m "$T")" /tmp/esp/S10EDIAG/collect.cmd >/dev/null
sed 's/\r*$/\r/' "$REPO/drivers/S10eDiag/esp/desktop.cmd" > "$T"; $A push "$(m "$T")" /tmp/esp/S10EDIAG/desktop.cmd >/dev/null
$A shell 'rm -rf /tmp/esp/S10EDIAG/extra.cmd /tmp/esp/S10EDIAG/drivers /tmp/esp/WINRUN.FLG; ls -la /tmp/esp/S10EDIAG; sync; umount /tmp/esp'
rm -f "$T"
echo provisioned
