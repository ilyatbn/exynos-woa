#!/bin/bash
# umdswap.sh KIND DLL TAG - swap one UMD file on the phone over SSH, like ecswap.sh but for every flavour:
#   KIND arm64 -> System32\maliumd_arm64.dll (behind the ARM64X forwarder maliumd.dll; loaded by dwm and every
#                 ARM64 process, so the new file takes effect at the next Windows boot)
#   KIND ec    -> System32\maliumd_ec.dll   (x64 processes, next process start)
#   KIND x86   -> SysWOW64\maliumd32.dll    (WOW64 x86 processes, next process start)
# The live file is renamed to <name>.pre-<TAG>-<n> (a loaded DLL can be renamed, not overwritten), DLL is copied in,
# sha256 checked. Rollback: rename a saved one back. notes/games-unity.md.
KIND=${1:?kind arm64|ec|x86}; DLL=${2:?dll}; TAG=${3:?tag}
case $KIND in
  arm64) DIR='C:\\Windows\\System32'; NAME=maliumd_arm64.dll ;;
  ec)    DIR='C:\\Windows\\System32'; NAME=maliumd_ec.dll ;;
  x86)   DIR='C:\\Windows\\SysWOW64'; NAME=maliumd32.dll ;;
  *) echo "kind?"; exit 1 ;;
esac
want=$(sha256sum "$DLL" | cut -d' ' -f1)
scp -O -q "$DLL" "s10e:C:/S10eTmp/$KIND-$TAG.dll" || exit 1
# NAME is expanded by bash (a PowerShell "$f.pre" would be a property access on $f, i.e. null)
ssh s10e "powershell -NoProfile -Command \"\$d='$DIR'; \$n=0; while (Test-Path \"\$d\\$NAME.pre-$TAG-\$n\") { \$n++ }; Rename-Item \"\$d\\$NAME\" \"$NAME.pre-$TAG-\$n\" -ErrorAction Stop; Copy-Item 'C:\\S10eTmp\\$KIND-$TAG.dll' \"\$d\\$NAME\"; Get-ChildItem \"\$d\\$NAME*\" | Where-Object { \$_.Name -notlike '*.old*' } | ForEach-Object { \$_.Name + ' ' + (Get-FileHash \$_.FullName).Hash.Substring(0,12).ToLower() }\"" < /dev/null
got=$(ssh s10e "powershell -NoProfile -Command \"(Get-FileHash $DIR\\$NAME).Hash.ToLower()\"" < /dev/null | tr -d '\r')
[ "$want" = "$got" ] && echo "$NAME = $TAG ${want:0:8} OK" || { echo "MISMATCH want ${want:0:8} got ${got:0:8}"; exit 1; }
