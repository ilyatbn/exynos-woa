# Build + package the G18 remote-monitor PoC: Microsoft's IddCx sample (video/IndirectDisplay, MS-PL, vendored here with
# the changes marked "exynos-woa") for ARM64, test-signed, into $Out (dll, inf, cat, IddSampleApp.exe).
# notes/g18-remote-monitors-research.md. The WDK 26100 UMDF / IddCx headers trip VS 18 warnings (C6553 in wdfmemory.h,
# C28193 in iddcx.h) under the sample's /WX + /analyze: _CL_ (appended after the project's options) turns both off.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$kit = 'C:\Program Files (x86)\Windows Kits\10'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$sln = Join-Path $PSScriptRoot 'IddSampleDriver.sln'
$env:_CL_ = '/WX- /analyze-'
$ErrorActionPreference = 'Continue'
& $msbuild $sln /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:VisualStudioVersion=17.0 `
    /p:SkipPackageVerification=true /p:ApiValidator_Enable=false /p:RunCodeAnalysis=false /nologo /v:minimal 2>&1 |
    ForEach-Object { "$_" } | Select-String -Pattern ' error |->' | ForEach-Object { $_.Line }
if ($LASTEXITCODE) { throw "msbuild $LASTEXITCODE" }
$ErrorActionPreference = 'Stop'
Remove-Item env:_CL_
$rel = Join-Path $PSScriptRoot 'ARM64\Release'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Get-ChildItem $Out -File | Remove-Item
Copy-Item (Join-Path $rel 'IddSampleDriver\IddSampleDriver.dll'), (Join-Path $rel 'IddSampleDriver\IddSampleDriver.inf'), `
    (Join-Path $rel 'IddSampleApp.exe') $Out
$sign = "$kit\bin\10.0.26100.0\x64\signtool.exe"
foreach ($f in 'IddSampleDriver.dll', 'IddSampleApp.exe') {
  & $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out $f) | Out-Null
  if ($LASTEXITCODE) { throw "sign $f" }
}
& "$kit\bin\10.0.26100.0\x86\Inf2Cat.exe" /driver:$Out /os:10_NI_ARM64,10_GE_ARM64 /uselocaltime | Select-Object -Last 2
if ($LASTEXITCODE) { throw 'inf2cat' }
& $sign sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 (Join-Path $Out 'IddSampleDriver.cat') | Out-Null
if ($LASTEXITCODE) { throw 'sign cat' }
Get-ChildItem $Out | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName).Hash.Substring(0, 16), $_.Name }
