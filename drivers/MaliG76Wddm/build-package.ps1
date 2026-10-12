# Build + package MaliG76Wddm (ARM64, WDM + displib, test-signed) into $Out (sys, inf, cat). notes/gpu-m16-g10.md.
# Same flow as drivers/MaliG76Probe/build-package.ps1 (VS18 MSBuild with VisualStudioVersion=17.0, SkipPackageVerification,
# ApiValidator_Enable=false, SignMode=Off; then signtool + Inf2Cat by hand with the WDK test certificate).
param([Parameter(Mandatory)][string]$Out, [int]$Build = 1)
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
$sign = "$kit\bin\10.0.26100.0\x64\signtool.exe"
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'MaliG76Wddm.sys') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign sys" }
& "$kit\bin\10.0.26100.0\x86\Inf2Cat.exe" /driver:$Out /os:10_RS3_ARM64,10_NI_ARM64,10_GE_ARM64 /uselocaltime | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { throw "inf2cat" }
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'MaliG76Wddm.cat') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign cat" }
Get-ChildItem $Out | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName).Hash, $_.Name }
