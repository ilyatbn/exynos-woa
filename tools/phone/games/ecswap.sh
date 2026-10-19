#!/bin/bash
# ecswap.sh DLL TAG - live swap of System32\maliumd_ec.dll (the ARM64EC UMD, loaded only by x64 processes) over SSH.
# The live file is renamed to maliumd_ec.dll.<TAG-of-previous>-<n> (a loaded DLL can be renamed, not overwritten;
# running x64 processes keep their mapping), DLL is copied in, sha256 checked. Rollback: rename a saved one back.
# notes/games-unity.md.
DLL=${1:?dll}; TAG=${2:?tag}
want=$(sha256sum "$DLL" | cut -d' ' -f1)
scp -O -q "$DLL" "s10e:C:/S10eTmp/maliumd_ec-$TAG.dll" || exit 1
ssh s10e "powershell -NoProfile -Command \"\$d='C:\\Windows\\System32'; \$n=0; while (Test-Path \"\$d\\maliumd_ec.dll.pre-$TAG-\$n\") { \$n++ }; Rename-Item \"\$d\\maliumd_ec.dll\" \"maliumd_ec.dll.pre-$TAG-\$n\"; Copy-Item 'C:\\S10eTmp\\maliumd_ec-$TAG.dll' \"\$d\\maliumd_ec.dll\"; Get-ChildItem \"\$d\\maliumd_ec*\" | ForEach-Object { \$_.Name + ' ' + (Get-FileHash \$_.FullName).Hash.Substring(0,12).ToLower() }\"" < /dev/null
got=$(ssh s10e "powershell -NoProfile -Command \"(Get-FileHash C:\\Windows\\System32\\maliumd_ec.dll).Hash.ToLower()\"" < /dev/null | tr -d '\r')
[ "$want" = "$got" ] && echo "maliumd_ec.dll = $TAG ${want:0:8} OK" || { echo "MISMATCH want ${want:0:8} got ${got:0:8}"; exit 1; }
