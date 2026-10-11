# Build + package Exynos9820Wlan (M12 WiFi milestone-1 probe, ARM64, test-signed) into $Out (sys, inf, cat).
# Same flow as drivers/Exynos9820Hsi2c/build-package.ps1 (notes/m12-driver-packages.md): VS18 MSBuild with
# VisualStudioVersion=17.0 (only DriverKit.Build.Tasks.17.0 ships), SkipPackageVerification (InfVerif.dll load),
# ApiValidator_Enable=false (aitstatic exit 193), SignMode=Off; then signtool + Inf2Cat by hand. notes/m12-wifi-m1.md.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$kit = 'C:\Program Files (x86)\Windows Kits\10'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$proj = Join-Path $PSScriptRoot 'Exynos9820Wlan.vcxproj'
& $msbuild $proj /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:VisualStudioVersion=17.0 `
    /p:SkipPackageVerification=true /p:ApiValidator_Enable=false /nologo /v:minimal | Select-Object -Last 5
if ($LASTEXITCODE -ne 0) { throw "msbuild $LASTEXITCODE" }
$pkg = Join-Path $PSScriptRoot 'ARM64\Release'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Get-ChildItem $Out -File | Remove-Item
Copy-Item (Join-Path $pkg 'Exynos9820Wlan.sys'), (Join-Path $pkg 'Exynos9820Wlan.inf') $Out
$sign = "$kit\bin\10.0.26100.0\x64\signtool.exe"
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'Exynos9820Wlan.sys') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign sys" }
& "$kit\bin\10.0.26100.0\x86\Inf2Cat.exe" /driver:$Out /os:10_RS3_ARM64,10_NI_ARM64,10_GE_ARM64 /uselocaltime | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { throw "inf2cat" }
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'exynos9820wlan.cat') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sign cat" }
Get-ChildItem $Out | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName).Hash.Substring(0,16), $_.Name }
