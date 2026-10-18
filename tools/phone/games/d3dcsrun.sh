#!/bin/bash
# G24: run the DirectCompute 4.x test (drivers/MaliG76Wddm/tools/d3dcs) on the phone and fetch its output.
# usage: d3dcsrun.sh [out-file] [x86|arm64|x64]   (x86 d3dcs32.exe -> SysWOW64 maliumd32.dll; arm64 d3dcs.exe and
# x64 d3dcs64x.exe (emulated, G27) -> System32 maliumd.dll; output unbuffered to a file
# because a GPU fault can take the session down before ssh returns it)
OUT=${1:-traces/games/d3dcs.txt}
case "${2:-x86}" in arm64) N=d3dcs.exe;; x64) N=d3dcs64x.exe;; *) N=d3dcs32.exe;; esac
EXE=drivers/MaliG76Wddm/tools/d3dcs/$N
[ -f "$EXE" ] && scp -q -O "$EXE" s10e:C:/S10eTmp/ < /dev/null
ssh s10e "cmd /c \"cd /d C:\S10eTmp && del d3dcs.out 2>nul & $N > C:\S10eTmp\d3dcs.out 2>&1\"" < /dev/null
echo "rc $?"
ssh s10e 'type C:\S10eTmp\d3dcs.out' < /dev/null | tee "$OUT"
