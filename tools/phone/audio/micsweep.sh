#!/bin/bash
# micsweep.sh NAME=VALUE... -- run mictest.sh once per setting list: each argument is "Var=v1,v2,..." over the
# Exynos9820Audio Parameters (MicVariant / MicWdma / MicWdmaCtrl / MicWdmaLimit); prints the key result lines.
# Diagnosis for notes/d8-mic.md (WDMA bring-up); every value is reset to 0 at the end.
R=/c/Users/ilya/code/exynos-woa; P='HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters'
VAR=${1%%=*}; VALS=${1#*=}; shift
for f in "$@"; do ssh s10e "reg add $P /v ${f%%=*} /t REG_DWORD /d ${f#*=} /f >nul" < /dev/null; done
for v in ${VALS//,/ }; do
  ssh s10e "reg add $P /v $VAR /t REG_DWORD /d $v /f >nul" < /dev/null
  echo "== $VAR=$v $*"
  bash $R/tools/phone/audio/mictest.sh 1000 2>&1 | grep "variant\|poll [12] \|frames of\|rms\|strongest" | cut -c1-200
done
for n in MicVariant MicWdma MicWdmaCtrl MicWdmaLimit; do ssh s10e "reg add $P /v $n /t REG_DWORD /d 0 /f >nul" < /dev/null; done
