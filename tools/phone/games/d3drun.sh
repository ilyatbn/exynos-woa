#!/bin/bash
# d3drun.sh EXE [ARGS...] - copy a drivers/MaliG76Wddm/tools/<name>/<EXE> test binary to C:\S10eTmp, run it there with
# its output unbuffered to a file (a GPU fault can drop the session before ssh returns), print the output and the
# exit code. G27: the x64 builds (*64x.exe) run emulated on the ARM64EC UMD, the plain ones on ARM64, *32 on x86.
EXE=${1:?usage: d3drun.sh EXE [ARGS]}; shift
SRC=$(ls drivers/MaliG76Wddm/tools/*/"$EXE" 2>/dev/null | head -1)
[ -n "$SRC" ] || { echo "no $EXE under drivers/MaliG76Wddm/tools"; exit 1; }
scp -q -O "$SRC" s10e:C:/S10eTmp/ < /dev/null || exit 1
ssh s10e "cmd /c \"cd /d C:\S10eTmp && $EXE $* > C:\S10eTmp\$EXE.out 2>&1 && echo exit 0 >> C:\S10eTmp\$EXE.out || echo exit NONZERO >> C:\S10eTmp\$EXE.out\"" < /dev/null
ssh s10e "type C:\S10eTmp\$EXE.out" < /dev/null
