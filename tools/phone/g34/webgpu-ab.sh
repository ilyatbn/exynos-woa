#!/bin/bash
# webgpu-ab.sh TAG - G34 diagnosis (notes/g34-fl11.md): does Edge's WebGPU requestAdapter depend on FL 11_0? Runs the
# standalone webgpu-samples rotatingCube page once with the user env D3D10UMD_FL11_0=0 (11_0 hidden; diagnosis switch,
# AGENTS.md rule 12) and once without, restarting Edge each time; shots -> traces/g34-webgpu-TAG-{fl10,fl11}.png.
# The env value is removed again at the end.
TAG=${1:?tag}; URL='https://webgpu.github.io/webgpu-samples/sample/rotatingCube/'
run() {
  ssh s10e 'taskkill /f /im msedge.exe >nul 2>&1' < /dev/null; sleep 3
  ssh s10e "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\runuser2.ps1 -Cmd \"powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\browsercheck.ps1 -Url $URL -Wait 25\" -Wait 100" < /dev/null
  scp -O -q s10e:C:/S10eTmp/browser-webgl.png "traces/g34-webgpu-$TAG-$1.png" && echo "saved traces/g34-webgpu-$TAG-$1.png"
}
ssh s10e 'reg add HKCU\Environment /v D3D10UMD_FL11_0 /t REG_SZ /d 0 /f' < /dev/null
run fl10
ssh s10e 'reg delete HKCU\Environment /v D3D10UMD_FL11_0 /f' < /dev/null
run fl11
ssh s10e 'reg query HKCU\Environment /v D3D10UMD_FL11_0 2>nul || echo env removed' < /dev/null
