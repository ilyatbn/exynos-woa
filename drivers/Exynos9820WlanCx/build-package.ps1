# Build + package Exynos9820WlanCx (M12 M9 WiFiCx client, ARM64, test-signed) into $Out (sys, inf, cat).
# Same flow as drivers/Exynos9820Wlan/build-package.ps1 (VS18 MSBuild with VisualStudioVersion=17.0,
# SkipPackageVerification, ApiValidator_Enable=false, SignMode=Off; signtool + Inf2Cat by hand, WDK test cert).
# Prints the full sha256 of every packaged file. notes/m12-wifi-wificx.md.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$kit = 'C:\Program Files (x86)\Windows Kits\10'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$proj = Join-Path $PSScriptRoot 'Exynos9820WlanCx.vcxproj'
& $msbuild $proj /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:VisualStudioVersion=17.0 `
    /p:SkipPackageVerification=true /p:ApiValidator_Enable=false /nologo /v:minimal | Select-Object -Last 15
if ($LASTEXITCODE -ne 0) { throw "msbuild $LASTEXITCODE" }
$pkg = Join-Path $PSScriptRoot 'ARM64\Release'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Get-ChildItem $Out -File | Remove-Item
Copy-Item (Join-Path $pkg 'Exynos9820WlanCx.sys'), (Join-Path $pkg 'Exynos9820WlanCx.inf') $Out
$sign = "$kit\bin\10.0.26100.0\x64\signtool.exe"
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'Exynos9820WlanCx.sys') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign sys" }
& "$kit\bin\10.0.26100.0\x86\Inf2Cat.exe" /driver:$Out /os:10_RS3_ARM64,10_NI_ARM64,10_GE_ARM64 /uselocaltime | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { throw "inf2cat" }
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'exynos9820wlancx.cat') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign cat" }
Get-ChildItem $Out -File | ForEach-Object { '{0}  {1,7}  {2}' -f (Get-FileHash $_.FullName).Hash, $_.Length, $_.Name }
