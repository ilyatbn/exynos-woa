#!/bin/bash
# pull-run.sh RUN [STEP] - from TWRP: copy \S10EDIAG\out\last\<STEP> (default h3) and out\agent.log from the ESP, the WER
# LocalDumps (C:\S10eTmp\dumps, read-only NTFS mount) into s10e-artifacts\runs\RUN, then print the key lines.
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
R=${1:?usage: pull-run.sh RUN [STEP]}; STEP=${2:-h3}
D=C:/Users/ilya/code/s10e-artifacts/runs/$R
mkdir -p "$D/werdumps"
$A shell 'mkdir -p /tmp/esp; mountpoint -q /tmp/esp || mount -t vfat /dev/block/by-name/cache /tmp/esp'
$A pull /tmp/esp/S10EDIAG/out/last/$STEP "$D/" >/dev/null
$A pull /tmp/esp/S10EDIAG/out/agent.log "$D/" >/dev/null
$A shell 'umount /data 2>/dev/null; mkdir -p /tmp/win; mountpoint -q /tmp/win || mount.ntfs -o ro /dev/block/by-name/userdata /tmp/win >/dev/null 2>&1; ls /tmp/win/S10eTmp/dumps/ 2>/dev/null' | tr -d '\r' | while read f; do [ -n "$f" ] && $A pull "/tmp/win/S10eTmp/dumps/$f" "$D/werdumps/" >/dev/null; done
$A shell 'umount /tmp/win 2>/dev/null; true'
X="$D/$STEP"
u16() { iconv -f utf-16 -t utf-8 "$1" 2>/dev/null || cat "$1"; }
echo "== $R"
for f in tex-s0.txt h3-cube.txt; do [ -f "$X/$f" ] && { echo "-- $f"; u16 "$X/$f" | tr -d '\r' | grep -a -E "PASS|FAIL|WRONG|ssertion|Unhandled|TIMEOUT|cube:|exit" | tail -4; }; done
[ -f "$X/summary.txt" ] && tr -d '\r' < "$X/summary.txt"
grep -a -E "no explorer|explorer.exe appeared|desktop:" "$D/agent.log" | tr -d '\r' | head -2
[ -f "$X/app-crashes.txt" ] && tr -d '\r\0' < "$X/app-crashes.txt" | grep -a "Faulting application name" | head -4
ls "$D/werdumps"
