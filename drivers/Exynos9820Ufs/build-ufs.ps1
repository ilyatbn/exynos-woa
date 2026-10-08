<#
  build-ufs.ps1 -- build Exynos9820Ufs.sys (ARM64) and the UfsDiag.exe helper
  with fixed cl/link command lines, without the WDK MSBuild toolset.

  Requires Visual Studio (ARM64 build tools) plus the Windows 10.0.26100 SDK and
  WDK. Pass -WdkRoot if the WDK is not installed in the default location.
  The script never signs or installs anything.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$SourceDir,
  [Parameter(Mandatory)][string]$OutDir,
  [switch]$NoInstrumentation,
  [switch]$WideUncached,
  [switch]$DmaWindow,
  [string]$WdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
)
$ErrorActionPreference = "Stop"
$PortableKernelContract = $true

$VcVars   = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
$SdkInc   = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
$WdkLib   = Join-Path $WdkRoot "Lib\10.0.26100.0"
$SdkLib   = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"
foreach ($p in @($VcVars, $SdkInc, $WdkLib, $SdkLib)) {
  if (-not (Test-Path $p)) { throw "missing toolchain component: $p" }
}

$SourceDir = (Resolve-Path $SourceDir).Path
$Src = Join-Path $SourceDir "Exynos9820Ufs.c"
if (-not (Test-Path $Src)) { throw "missing driver source: $Src" }
if ($PortableKernelContract -and (Test-Path -LiteralPath $OutDir)) {
  throw "portable candidates require a new output directory: $OutDir"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

$Obj = Join-Path $OutDir "Exynos9820Ufs.obj"
$Pdb = Join-Path $OutDir "Exynos9820Ufs.pdb"
$Sys = Join-Path $OutDir "Exynos9820Ufs.sys"
$Lib = Join-Path $OutDir "Exynos9820Ufs.lib"
$Iobj = Join-Path $OutDir "Exynos9820Ufs.iobj"

# Compiler flags of the validated driver build.
$ClFlags = '/c /nologo /W4 /WX /diagnostics:column /Ox /Oi /Os /Oy- /GL ' +
  '/D _ARM64_ /D ARM64 /D _USE_DECLSPECS_FOR_SAL=1 /D STD_CALL /D _WIN32_WINNT=0x0A00 ' +
  '/D WINVER=0x0A00 /D WINNT=1 /D NTDDI_VERSION=0xA000010 ' +
  '/D _ARM64_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 /GF /Gm- /Zp8 /GS /guard:cf /Gy ' +
  '/fp:precise /Qspectre /Zc:wchar_t- /Zc:forScope /Zc:inline /GR- /external:W4 ' +
  '/wd4064 /wd4627 /wd4366 /wd4603 /wd4986 /wd4987 /analyze- /FC /kernel -cbstring ' +
  '/d1nodatetime /d1import_no_registry /d2AllowCompatibleILVersions'

$ProfileFlags = ''
if ($NoInstrumentation) { $ProfileFlags += ' /D UFS_PERF_INSTRUMENTATION=0' }
if ($WideUncached)      { $ProfileFlags += ' /D UFS_WIDE_UNCACHED_ACCESS=1' }
if ($DmaWindow)         { $ProfileFlags += ' /D UFS_DMA_WINDOW_ENFORCE=1' }
if ($PortableKernelContract) { $ProfileFlags += ' /D UFS_PORTABLE_KERNEL_CONTRACT=1' }
$ClFlags += $ProfileFlags

# Linker flags of the validated driver build.
$LinkFlags = '/VERSION:"10.0" /INCREMENTAL:NO /NOLOGO /WX /NODEFAULTLIB ' +
  '/NODEFAULTLIB:OLDNAMES.LIB /MANIFEST:NO /DEBUG:NONE /SUBSYSTEM:NATIVE,"10.00" ' +
  '/STACK:"0x40000","0x2000" /Driver /OPT:REF /OPT:ICF /LTCG /ENTRY:"GsDriverEntry" ' +
  '/RELEASE /Brepro /MERGE:"_TEXT=.text;_PAGE=PAGE" /MACHINE:ARM64 /guard:cf /kernel ' +
  '/IGNORE:4078,4221,4198 /osversion:10.0'

$LinkLibPaths = @(
  "$SdkLib\km\arm64\storport.lib",
  "$SdkLib\um\arm64\arm64rt.lib",
  "$WdkLib\km\arm64\BufferOverflowFastFailK.lib",
  "$WdkLib\km\arm64\ntoskrnl.lib",
  "$WdkLib\km\arm64\hal.lib",
  "$WdkLib\km\arm64\wmilib.lib"
)
if ($PortableKernelContract) {
  $LinkLibPaths += "$SdkLib\km\arm64\Aux_Klib.lib"
  $LinkFlags += ' /INCLUDE:UfsDiagBinaryContract'
}
$LinkLibs = $LinkLibPaths | ForEach-Object {
  if (-not (Test-Path $_)) { throw "missing import library: $_" }
  "`"$_`""
}

$HelperBuild = ''
if ($PortableKernelContract) {
  $HelperSource = Join-Path $SourceDir 'UfsDiag.c'
  if (-not (Test-Path -LiteralPath $HelperSource)) {
    throw "portable profile requires matched helper source: $HelperSource"
  }
  $Helper = Join-Path $OutDir 'UfsDiag.exe'
  $HelperObj = Join-Path $OutDir 'UfsDiag.obj'
  $HelperBuild = @"
set INCLUDE=$SdkInc\um;$SdkInc\shared;$SdkInc\ucrt;%INCLUDE%
cl.exe /nologo /W4 /WX /O2 /Oi /GL /MT /GS /guard:cf /Zp8 /D UNICODE /D _UNICODE /D _WIN32_WINNT=0x0A00 $ProfileFlags /Fo"$HelperObj" /Fe"$Helper" "$HelperSource" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /LTCG /INCREMENTAL:NO /Brepro /INCLUDE:UfsDiagBinaryContract setupapi.lib cfgmgr32.lib advapi32.lib user32.lib
if errorlevel 1 exit /b 4
"@
}

$Bat = @"
@echo off
call "$VcVars" x64_arm64 >nul
if errorlevel 1 exit /b 1
set INCLUDE=$SdkInc\km;$SdkInc\shared;%INCLUDE%
cl.exe $ClFlags /FI"$SdkInc\shared\warning.h" /Fo"$Obj" "$Src"
if errorlevel 1 exit /b 2
link.exe $LinkFlags /OUT:"$Sys" /IMPLIB:"$Lib" /LTCGOUT:"$Iobj" "$Obj" $($LinkLibs -join ' ')
if errorlevel 1 exit /b 3
$HelperBuild
echo EXACT_BUILD_OK
"@

$BatPath = Join-Path $OutDir "_build-exact.bat"
Set-Content -LiteralPath $BatPath -Value $Bat -Encoding ascii
& $env:ComSpec /c "`"$BatPath`""
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$Info = Get-Item $Sys
Write-Output "DRIVER=$Sys"
Write-Output "DRIVER_BYTES=$($Info.Length)"
Write-Output "DRIVER_SHA256=$((Get-FileHash $Sys -Algorithm SHA256).Hash.ToLowerInvariant())"
if ($PortableKernelContract) {
  function Get-ArtifactRef([string]$Path) {
    $Item = Get-Item -LiteralPath $Path
    [ordered]@{
      path = $Item.FullName
      bytes = $Item.Length
      sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }
  function Get-BinaryContract([string]$Path) {
    $Bytes = [IO.File]::ReadAllBytes($Path)
    $Text = [Text.Encoding]::ASCII.GetString($Bytes)
    $Offset = $Text.IndexOf('UFSKABI1', [StringComparison]::Ordinal)
    if ($Offset -lt 0 -or $Offset + 44 -gt $Bytes.Length -or
        $Text.IndexOf('UFSKABI1', $Offset + 1, [StringComparison]::Ordinal) -ge 0) {
      throw "missing/ambiguous binary ABI identity: $Path"
    }
    $Words = @(0..10 | ForEach-Object { [BitConverter]::ToUInt32($Bytes, $Offset + 4 * $_) })
    if ($Words[2] -ne 44 -or $Words[3] -ne 1) { throw "invalid ABI identity: $Path" }
    [ordered]@{
      signature = $Words[4]; version = $Words[5]; bytes = $Words[6]
      perf = $Words[7]; wide_uncached = $Words[8]; dma_window = $Words[9]
      portable_kernel_contract = $Words[10]
    }
  }
  $Contract = Get-BinaryContract $Sys
  $HelperContract = Get-BinaryContract $Helper
  if (($Contract | ConvertTo-Json -Compress) -cne ($HelperContract | ConvertTo-Json -Compress)) {
    throw 'driver/helper binary ABI identities differ'
  }
  $Receipt = [ordered]@{
    schema_version = 1
    kind = 'ufs-build'
    profile = 'portable-kernel-contract'
    driver = Get-ArtifactRef $Sys
    helper = Get-ArtifactRef $Helper
    diagnostic = $Contract
    sources = [ordered]@{
      driver = Get-ArtifactRef $Src
      driver_header = Get-ArtifactRef (Join-Path $SourceDir 'Exynos9820Ufs.h')
      diagnostic_header = Get-ArtifactRef (Join-Path $SourceDir 'Exynos9820UfsDiag.h')
      helper = Get-ArtifactRef $HelperSource
    }
    build_script = Get-ArtifactRef $PSCommandPath
    command_file = Get-ArtifactRef $BatPath
    libraries = @($LinkLibPaths | ForEach-Object { Get-ArtifactRef $_ })
    signing = 'not-requested'
    hardware_qualified = $false
  }
  $Receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutDir 'ufs-build.json') -Encoding ascii
  Write-Output "HELPER=$Helper"
  Write-Output "HELPER_BYTES=$((Get-Item -LiteralPath $Helper).Length)"
  Write-Output "HELPER_SHA256=$((Get-FileHash -LiteralPath $Helper -Algorithm SHA256).Hash.ToLowerInvariant())"
  Write-Output "DIAGNOSTIC_ABI=$($Contract | ConvertTo-Json -Compress)"
}
Write-Output "BUILD_OK"
