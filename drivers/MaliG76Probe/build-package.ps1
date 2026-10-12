# Build + package MaliG76Probe (ARM64, KMDF 1.15, test-signed) into $Out (sys, inf, cat). notes/gpu-m16-g9b.md.
# Same flow as drivers/S10eBattery/build-package.ps1 (VS18 MSBuild with VisualStudioVersion=17.0, SkipPackageVerification,
# ApiValidator_Enable=false, SignMode=Off; then signtool + Inf2Cat by hand with the WDK test certificate).
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$kit = 'C:\Program Files (x86)\Windows Kits\10'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$proj = Join-Path $PSScriptRoot 'MaliG76Probe.vcxproj'
$work = Join-Path $env:TEMP 'maliG76probe-build'
& $msbuild $proj /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:VisualStudioVersion=17.0 `
    /p:SkipPackageVerification=true /p:ApiValidator_Enable=false "/p:OutDir=$work\" "/p:IntDir=$work\obj\" `
    /nologo /v:minimal | Select-Object -Last 8
if ($LASTEXITCODE -ne 0) { throw "msbuild $LASTEXITCODE" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Get-ChildItem $Out -File | Remove-Item
Copy-Item (Join-Path $work 'MaliG76Probe.sys'), (Join-Path $PSScriptRoot 'MaliG76Probe.inf') $Out
$sign = "$kit\bin\10.0.26100.0\x64\signtool.exe"
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'MaliG76Probe.sys') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign sys" }
& "$kit\bin\10.0.26100.0\x86\Inf2Cat.exe" /driver:$Out /os:10_RS3_ARM64,10_NI_ARM64,10_GE_ARM64 /uselocaltime | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { throw "inf2cat" }
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'MaliG76Probe.cat') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign cat" }
Get-ChildItem $Out | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName).Hash, $_.Name }
