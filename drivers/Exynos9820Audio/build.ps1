# Build Exynos9820Audio (ARM64, ACX 1.0) into a temp work dir; package/sign is a separate step (A3).
# Same MSBuild workarounds as drivers/S10eBattery/build-package.ps1 (VS18 + WDK 26100):
# VisualStudioVersion=17.0, SkipPackageVerification, ApiValidator_Enable=false, SignMode=Off.
param([string]$Work = (Join-Path $env:TEMP 'exynos9820audio-build'))
$ErrorActionPreference = 'Stop'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$proj = Join-Path $PSScriptRoot 'Exynos9820Audio.vcxproj'
& $msbuild $proj /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:VisualStudioVersion=17.0 `
    /p:SkipPackageVerification=true /p:ApiValidator_Enable=false "/p:OutDir=$Work\" "/p:IntDir=$Work\obj\" `
    /nologo /v:minimal | Select-Object -Last 30
if ($LASTEXITCODE -ne 0) { throw "msbuild $LASTEXITCODE" }
Get-Item (Join-Path $Work 'Exynos9820Audio.sys') | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName).Hash, $_.Name }
