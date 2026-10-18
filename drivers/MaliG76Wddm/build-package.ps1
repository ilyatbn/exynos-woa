# Build + package MaliG76Wddm (ARM64, WDM + displib, test-signed) into $Out (sys, inf, cat). notes/gpu-m16-g10.md.
# Same flow as drivers/MaliG76Probe/build-package.ps1 (VS18 MSBuild with VisualStudioVersion=17.0, SkipPackageVerification,
# ApiValidator_Enable=false, SignMode=Off; then signtool + Inf2Cat by hand with the WDK test certificate).
# -Umd / -Umd32: the Mesa UMD build directories (tools/mesa-build: maliumd.dll ARM64, maliumd32.dll x86); since 1.0.70.1
# the package carries both (MaliG76Wddm.inf MW_UmdFiles / MW_UmdWow), signed and in the catalog. Since 1.0.71.1 -Umd is
# a tools/mesa-build/make-arm64x.ps1 output: maliumd.dll (ARM64X forwarder), maliumd_arm64.dll, maliumd_ec.dll (G27).
param([Parameter(Mandatory)][string]$Out, [int]$Build = 1, [Parameter(Mandatory)][string]$Umd,
      [Parameter(Mandatory)][string]$Umd32)
$ErrorActionPreference = 'Stop'
$kit = 'C:\Program Files (x86)\Windows Kits\10'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$proj = Join-Path $PSScriptRoot 'MaliG76Wddm.vcxproj'
$work = Join-Path $env:TEMP 'maliG76wddm-build'
& $msbuild $proj /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:VisualStudioVersion=17.0 `
    /p:SkipPackageVerification=true /p:ApiValidator_Enable=false "/p:MwBuild=$Build" "/p:OutDir=$work\" "/p:IntDir=$work\obj\" `
    /nologo /v:minimal | Select-Object -Last 8
if ($LASTEXITCODE -ne 0) { throw "msbuild $LASTEXITCODE" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Get-ChildItem $Out -File | Remove-Item
Copy-Item (Join-Path $work 'MaliG76Wddm.sys'), (Join-Path $PSScriptRoot 'MaliG76Wddm.inf') $Out
$umdFiles = 'maliumd.dll', 'maliumd_arm64.dll', 'maliumd_ec.dll'
Copy-Item ($umdFiles | ForEach-Object { Join-Path $Umd $_ }) $Out
Copy-Item (Join-Path $Umd32 'maliumd32.dll') $Out
$sign = "$kit\bin\10.0.26100.0\x64\signtool.exe"
foreach ($f in @('MaliG76Wddm.sys', 'maliumd32.dll') + $umdFiles) {
  & $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out $f) | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "sign $f" }
}
& "$kit\bin\10.0.26100.0\x86\Inf2Cat.exe" /driver:$Out /os:10_RS3_ARM64,10_NI_ARM64,10_GE_ARM64 /uselocaltime | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { throw "inf2cat" }
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'MaliG76Wddm.cat') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign cat" }
Get-ChildItem $Out | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName).Hash, $_.Name }
