#!/bin/bash
# gldeploy.sh [DIR] - copy the Mali OpenGL ICD set (DIR = s10e-artifacts\mali-gl-x from make-arm64x.ps1 -Name maligl:
# maligl.dll ARM64X forwarder, maligl_arm64.dll, maligl_ec.dll) into the running Windows' System32 over SSH and check
# the sha256s. A loaded DLL cannot be overwritten: the processes named in KILL (default: Stardew Valley, gltest,
# gltest64 - our test processes) are stopped first. notes/gl-icd.md.
D=${1:-/c/Users/ilya/code/s10e-artifacts/mali-gl-x}
KILL=${KILL:-"Stardew Valley,gltest,gltest64"}
for f in maligl.dll maligl_arm64.dll maligl_ec.dll; do [ -f "$D/$f" ] || { echo "missing $D/$f"; exit 1; }; done
ssh s10e "powershell -NoProfile -Command \"'$KILL'.Split(',') | ForEach-Object { Stop-Process -Name \$_ -Force -ErrorAction SilentlyContinue }\"" < /dev/null
scp -O -q "$D/maligl.dll" "$D/maligl_arm64.dll" "$D/maligl_ec.dll" s10e:C:/S10eTmp/ || exit 1
ssh s10e 'cmd /c "copy /y C:\S10eTmp\maligl.dll C:\Windows\System32\ >nul && copy /y C:\S10eTmp\maligl_arm64.dll C:\Windows\System32\ >nul && copy /y C:\S10eTmp\maligl_ec.dll C:\Windows\System32\ >nul"' < /dev/null || { echo "copy failed (DLL still loaded?)"; exit 1; }
bad=0
for f in maligl.dll maligl_arm64.dll maligl_ec.dll; do
  want=$(sha256sum "$D/$f" | cut -d' ' -f1)
  got=$(ssh s10e "powershell -NoProfile -Command \"(Get-FileHash C:\\Windows\\System32\\$f).Hash.ToLower()\"" < /dev/null | tr -d '\r')
  [ "$want" = "$got" ] && echo "$f ok ${want:0:8}" || { echo "$f MISMATCH want ${want:0:8} got ${got:0:8}"; bad=1; }
done
exit $bad
