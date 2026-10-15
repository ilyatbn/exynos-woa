# Build + package Exynos9820Bt (ARM64, test-signed) into $Out (sys, inf, cat). notes/bt-driver.md (D4c).
# Same MSBuild workarounds as drivers/Exynos9820Hsi2c/build-package.ps1 (VS18 + WDK 26100):
# VisualStudioVersion=17.0, SkipPackageVerification, ApiValidator_Enable=false, SignMode=Off.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$kit = 'C:\Program Files (x86)\Windows Kits\10'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$proj = Join-Path $PSScriptRoot 'Exynos9820Bt.vcxproj'
$work = Join-Path $env:TEMP 'exynos9820bt-build'
& $msbuild $proj /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:VisualStudioVersion=17.0 `
    /p:SkipPackageVerification=true /p:ApiValidator_Enable=false "/p:OutDir=$work\" "/p:IntDir=$work\obj\" `
    /nologo /v:minimal | Select-Object -Last 8
if ($LASTEXITCODE -ne 0) { throw "msbuild $LASTEXITCODE" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Get-ChildItem $Out -File | Remove-Item
Copy-Item (Join-Path $work 'Exynos9820Bt.sys'), (Join-Path $PSScriptRoot 'Exynos9820Bt.inf') $Out
# BCM4375 patch file: copied from the phone (s10e-artifacts/bt-hcd, notes/bt-driver.md B6), never committed.
$hcd = Join-Path $PSScriptRoot '..\..\..\s10e-artifacts\bt-hcd\bcm4375B1_semco.hcd'
if (-not (Test-Path $hcd)) { throw "missing $hcd (pull it from /vendor/firmware)" }
Copy-Item $hcd $Out
$sign = "$kit\bin\10.0.26100.0\x64\signtool.exe"
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'Exynos9820Bt.sys') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign sys" }
& "$kit\bin\10.0.26100.0\x86\Inf2Cat.exe" /driver:$Out /os:10_RS3_ARM64,10_NI_ARM64,10_GE_ARM64 /uselocaltime | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { throw "inf2cat" }
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'exynos9820bt.cat') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign cat" }
Get-ChildItem $Out | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName).Hash.Substring(0,16), $_.Name }
