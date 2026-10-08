# SPDX-License-Identifier: GPL-2.0-only
#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$WdkRoot
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
$repo = Split-Path (Split-Path $PSScriptRoot)
$baseline = Join-Path (Split-Path $PSScriptRoot) 'S6SY761TouchGpio\RetainedV24'
if (-not $WdkRoot) { $WdkRoot = Join-Path $repo 'tools\wdk-mirror' }
$WdkRoot = [IO.Path]::GetFullPath($WdkRoot).TrimEnd('\')
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
if ((Test-Path -LiteralPath $output) -or $output.Equals($repo, [StringComparison]::OrdinalIgnoreCase) -or
    $output.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose a new output directory outside the source tree.'
}
foreach ($name in @('CL','_CL_','LINK','_LINK_')) {
    if ([Environment]::GetEnvironmentVariable($name)) { throw "Untracked compiler override: $name" }
}
function Identity([string]$Path) {
    [ordered]@{ path=[IO.Path]::GetFullPath($Path); bytes=(Get-Item -LiteralPath $Path).Length
        sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$provenance = Get-Content -LiteralPath (Join-Path (Split-Path $baseline) 'RetainedV24.sha256.json') -Raw | ConvertFrom-Json
foreach ($file in $provenance.files) {
    $actual = Identity (Join-Path $baseline $file.file)
    if ($actual.bytes -ne $file.bytes -or $actual.sha256 -cne $file.sha256) {
        throw "Retained baseline drift: $($file.file)"
    }
}
$helper = Join-Path $repo 'windows-drivers\Exynos9810Hsi2c\Arm64BuildEvidence.ps1'
$mockRoot = Join-Path (Split-Path $baseline) 'tests'
$sources = @(
    Get-ChildItem -LiteralPath $PSScriptRoot -File -Recurse |
        Sort-Object FullName | ForEach-Object { Identity $_.FullName }
    foreach ($name in @('TouchMock.c','TouchMock.h','TouchHostHooks.h','HostThreads.c','HostThreads.h')) {
        Identity (Join-Path $mockRoot $name)
    }
    Identity $helper
    Identity (Join-Path (Split-Path $baseline) 'RetainedV24.sha256.json')
    foreach ($file in $provenance.files) { Identity (Join-Path $baseline $file.file) }
)
New-Item -ItemType Directory -Path $output | Out-Null
function Build([string]$Project, [string]$Directory, [string[]]$Properties) {
    New-Item -ItemType Directory -Path $Directory | Out-Null
    & $msbuild $Project /t:Build /p:Configuration=Release "/p:OutDir=$Directory\" `
        "/p:IntDir=$Directory\obj\" /nologo /v:normal @Properties *> (Join-Path $Directory 'build.log')
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath (Join-Path $Directory 'build.log') -Tail 35 | Out-Host
        throw "Build failed ($LASTEXITCODE): $Project"
    }
}
function Run([string]$Directory, [string]$Case = '') {
    $tag = if ($Case) { $Case } else { 'all' }
    $parameters = @{
        FilePath=(Join-Path $Directory 'TouchGestureTests.exe')
        NoNewWindow=$true; PassThru=$true
        RedirectStandardOutput=(Join-Path $Directory "$tag.stdout")
        RedirectStandardError=(Join-Path $Directory "$tag.stderr")
    }
    if ($Case) { $parameters.ArgumentList = $Case }
    $process = Start-Process @parameters
    if (-not $process.WaitForExit(60000)) {
        Stop-Process -Id $process.Id -Force
        throw "Test exceeded its 60-second bound: $tag"
    }
    return $process.ExitCode
}
$testProject = Join-Path $PSScriptRoot 'tests\TouchGestureTests.vcxproj'
$hostBuild = Join-Path $output 'host'
Build $testProject $hostBuild @('/p:Platform=x64', "/p:WdkRoot=$WdkRoot")
if ((Run $hostBuild) -ne 0) {
    Get-Content -LiteralPath (Join-Path $hostBuild 'all.stderr') | Out-Host
    throw 'Gesture regression tests failed.'
}
$tests = Get-Content -LiteralPath (Join-Path $hostBuild 'all.stdout') -Raw | ConvertFrom-Json
if ($tests.groups -ne 9 -or $tests.failures -ne 0 -or $tests.hardwareAccess -ne $false -or $tests.checks -le 0) {
    throw 'Invalid gesture test result.'
}
$control = Join-Path $output 'retained-control'
Build $testProject $control @('/p:Platform=x64', "/p:WdkRoot=$WdkRoot",
    "/p:DriverRoot=$baseline", '/p:GestureCandidate=0')
$regressions = @(
    @{ name='down-origin'; assertion='InputReportCount == 2' },
    @{ name='hold'; assertion='InputReportCount == 2' },
    @{ name='double-tap'; assertion='InputReportCount == 6' },
    @{ name='full-fifo'; assertion='InputReportCount == 64' },
    @{ name='short-buffer'; assertion='InputReportCount == 1' }
)
foreach ($case in $regressions) {
    $code = Run $control $case.name
    $errorText = Get-Content -LiteralPath (Join-Path $control "$($case.name).stderr") -Raw
    if ($code -ne 1 -or $errorText -notmatch '^FAIL \d+:' -or
        -not $errorText.Contains($case.assertion)) {
        throw "Retained control did not reproduce the expected regression: $($case.name)"
    }
}
$arm = Join-Path $output 'arm64'
Build (Join-Path $PSScriptRoot 'S6SY761TouchBeyond0.vcxproj') $arm @('/p:Platform=ARM64',
    "/p:WDKContentRoot=$WdkRoot\", '/p:ApiValidator_Enable=false', '/p:SignMode=Off')
. $helper
$inputs = Get-Arm64BuildInputs -Directory $arm -Binary (Join-Path $arm 'S6SY761TouchBeyond0.sys') `
    -BuildLog (Join-Path $arm 'build.log') -TranslationUnits 1
$package = Join-Path $output 'unsigned-package'
New-Item -ItemType Directory -Path $package | Out-Null
Copy-Item -LiteralPath (Join-Path $arm 'S6SY761TouchBeyond0.sys') -Destination $package
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'S6SY761TouchBeyond0.inf') -Destination $package
& (Join-Path $WdkRoot 'Tools\10.0.26100.0\x64\InfVerif.exe') /w `
    (Join-Path $package 'S6SY761TouchBeyond0.inf') *> (Join-Path $output 'infverif.log')
if ($LASTEXITCODE -ne 0) { throw 'INF validation failed.' }
& (Join-Path $WdkRoot 'bin\10.0.26100.0\x86\Inf2Cat.exe') "/driver:$package" `
    /os:10_VB_ARM64 /uselocaltime *> (Join-Path $output 'inf2cat.log')
if ($LASTEXITCODE -ne 0) { throw 'Catalog generation failed.' }
foreach ($name in @('S6SY761TouchBeyond0.sys','S6SY761TouchBeyond0.cat')) {
    if ((Get-AuthenticodeSignature -LiteralPath (Join-Path $package $name)).Status -ne 'NotSigned') {
        throw "Expected unsigned output: $name"
    }
}
foreach ($source in $sources) {
    if ((Identity $source.path).sha256 -cne $source.sha256) {
        throw "Source changed during build: $($source.path)"
    }
}
$receipt = [ordered]@{
    schema='s6sy761-touch-gestures-build-v1'; result='PASS_OFFLINE_ONLY'
    timestampUtc=[DateTime]::UtcNow.ToString('o'); msbuild=Identity $msbuild
    tests=$tests; retainedRegressions=@($regressions.name); retainedProvenance=$provenance
    sources=$sources; arm64BuildInputs=$inputs; buildHelper=Identity $helper
    package=$package; files=@(Get-ChildItem -LiteralPath $package -File |
        Sort-Object Name | ForEach-Object { Identity $_.FullName })
    signed=$false; deployed=$false; hardwareQualified=$false
    service='S6SY761TouchBeyond0'; hardwareId='ACPI\SCSY0761'; kmdf='1.15'
    remaining=@('Confirm live installed package identity and gesture settings',
        'Back up current driver/package and arrange verified recovery/rollback',
        'Sign with the existing device trust and deploy through the gated workflow',
        'Measure physical double-taps, sustained holds, dragging and multitouch on Windows 11')
}
$receipt | ConvertTo-Json -Depth 20 |
    Set-Content -LiteralPath (Join-Path $output 'touch-build.json') -Encoding utf8NoBOM
Write-Output "TOUCH_GESTURE_GROUPS=$($tests.groups) RETAINED_REGRESSIONS=$($regressions.Count) PACKAGE=$package"
Write-Output 'OFFLINE_ONLY=1 SIGNED=0 DEPLOYED=0 HARDWARE_QUALIFIED=0'
