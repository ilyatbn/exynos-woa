#!/bin/bash
# audbuild.sh VERSION - bump Exynos9820Audio to VERSION (Driver.c AUD_DRIVER_VERSION + INF DriverVer), build the package
# into ../s10e-artifacts/audio-VERSION, install it on the phone (pnputil) and wait for the device restart.
V=${1:?version}; R=/c/Users/ilya/code/exynos-woa; D=$R/drivers/Exynos9820Audio
sed -i -E "s/(#define AUD_DRIVER_VERSION \")[0-9.]+\"/\1$V\"/" $D/Driver.c
sed -i -E "s/^(DriverVer   = [0-9/]+,)[0-9.]+/\1$V/" $D/Exynos9820Audio.inf
grep -h "AUD_DRIVER_VERSION \"\|^DriverVer" $D/Driver.c $D/Exynos9820Audio.inf
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w $D/build-package.ps1)" -Out "$(cygpath -w $R/../s10e-artifacts/audio-$V)" 2>&1 \
  | grep -E "error|warning C|Exynos9820Audio.sys$" || { echo "build failed"; exit 1; }
[ -f $R/../s10e-artifacts/audio-$V/Exynos9820Audio.sys ] || { echo "no package"; exit 1; }
ssh s10e "mkdir C:\S10eTmp\audpkg 2>nul & del /q C:\S10eTmp\audpkg\*" < /dev/null
scp -O -q $R/../s10e-artifacts/audio-$V/* s10e:C:/S10eTmp/audpkg/ && \
  ssh s10e 'pnputil /add-driver C:\S10eTmp\audpkg\Exynos9820Audio.inf /install' < /dev/null | tr -d '\r' | tail -1
sleep 8
