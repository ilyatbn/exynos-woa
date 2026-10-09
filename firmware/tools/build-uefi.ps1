<#
.SYNOPSIS
  Build the Star2LtePkg EDK2 UEFI platform firmware (Exynos 9810 / Galaxy S9+).

.DESCRIPTION
  Wraps the EDK2 `build.py` invocation with the environment this workspace needs:
  clang/lld (CLANGDWARF) for AARCH64, the in-tree BaseTools, and a PACKAGES_PATH
  that spans both the edk2 submodule and the workspace root (where Platform/ and
  Silicon/ live).

  The stock BaseTools `build` wrapper has a stale hardcoded path on this machine,
  so we call build.py directly with PYTHONPATH set.

.PARAMETER Module
  Optional single module INF (workspace-relative) to build in isolation, e.g.
  Silicon\Exynos9810Pkg\Library\Exynos9810SerialPortLib\Exynos9810SerialPortLib.inf

.PARAMETER Target   DEBUG (default) or RELEASE.
.PARAMETER Toolchain Default CLANGDWARF (clang + lld, DWARF/ELF flow).
.PARAMETER UsbDebugMode
  Off preserves the ext209 control. Snapshot enables read-only CMU/Q-channel
  telemetry. Hid selects the native polling transport profile.
.PARAMETER Clean    Run `cleanall` instead of building.

.EXAMPLE
  .\tools\build-uefi.ps1                 # full platform FD
  .\tools\build-uefi.ps1 -Module Silicon\Exynos9810Pkg\Library\Exynos9810SerialPortLib\Exynos9810SerialPortLib.inf
#>
[CmdletBinding()]
param(
  [string]$Module = "",
  [ValidateSet("DEBUG", "RELEASE")] [string]$Target = "DEBUG",
  [string]$Toolchain = "CLANGPDB",
  [ValidateSet("Off", "Snapshot", "Hid")] [string]$UsbDebugMode = "Off",
  [ValidateRange(0, 4)] [int]$RamStage = 0,
  [ValidateSet("Star2LtePkg", "Beyond0LtePkg")] [string]$Platform = "Star2LtePkg",
  [switch]$Clean
)
$ErrorActionPreference = "Stop"

$ws   = Split-Path -Parent $PSScriptRoot
$edk2 = Join-Path $ws "edk2"
$dsc  = "Platform\$($Platform)\$($Platform).dsc"

$llvm = "C:\Program Files\LLVM\bin\"
if (-not (Test-Path (Join-Path $llvm "clang.exe"))) {
  throw "clang not found at $llvm - install LLVM or edit this script."
}

$env:WORKSPACE       = $edk2
$env:EDK_TOOLS_PATH  = Join-Path $edk2 "BaseTools"
$env:BASE_TOOLS_PATH = $env:EDK_TOOLS_PATH
$env:PACKAGES_PATH   = "$edk2;$ws"
$env:CLANG_BIN       = $llvm
$env:CLANGDWARF_BIN  = $llvm
$env:PYTHON_COMMAND  = "python"   # BinWrappers\*.bat (Trim, GenFds, ...) need this
# The BinWrappers are .bat files, so PYTHON_COMMAND is resolved by cmd.exe - and the
# WindowsApps python shim is NOT executable from cmd.exe even though PowerShell and
# `where` find it (symptom: every .S file dies at Trim with "'python' is not
# recognized"). Probe with cmd and fall back to the absolute path.
cmd /c "python --version >nul 2>nul"
if ($LASTEXITCODE -ne 0) {
  $pyCandidates = @()
  $pyCandidates += Get-ChildItem -Path "C:\Users\ilya\AppData\Local\Microsoft\WindowsApps" -Filter "python.exe" -EA SilentlyContinue | ForEach-Object { $_.FullName }
  $pyCandidates += Get-ChildItem -Path "C:\Python*" -Filter "python.exe" -EA SilentlyContinue | ForEach-Object { $_.FullName }
  $py = $pyCandidates | Where-Object { $p = $_; Test-Path $p } | Select-Object -First 1
  if ($py) { $env:PYTHON_COMMAND = '"' + $py + '"' }
}
# The WindowsApps python shim resolves in PowerShell but NOT inside cmd.exe, which is
# what the BinWrappers\WindowsLike\*.bat runtimes invoke it from ('python' is not
# recognized -> every .S file fails at Trim). Fall back to the absolute path.
$pythonExe = (Get-Command python -ErrorAction SilentlyContinue) | ForEach-Object { $_.Source }
if (-not $pythonExe) {
  $pythonExe = "C:\Users\ilya\AppData\Local\Microsoft\WindowsApps\python.exe"
}
if (Test-Path $pythonExe) {
  $env:PYTHON_COMMAND = '"' + $pythonExe + '"'
}
$env:PYTHONPATH      = Join-Path $edk2 "BaseTools\Source\Python"
# PATH needs BOTH: Bin\Win32 (compiled GenFfs/GenFv/...) AND BinWrappers\WindowsLike
# (the .bat wrappers like Trim.bat that preprocess .S asm files). Missing the latter
# gives: "process_begin: CreateProcess(NULL, Trim ...) failed / make (e=2)".
$env:PATH            = (Join-Path $edk2 "BaseTools\Bin\Win32") + ";" +
                       (Join-Path $edk2 "BaseTools\BinWrappers\WindowsLike") + ";" + $env:PATH

# CLANGPDB/CLANGDWARF drive the per-module build with GNU make (NOT nmake):
#   tools_def: *_CLANGPDB_*_MAKE_PATH = ENV(CLANG_HOST_BIN)make
# Leave CLANG_HOST_BIN empty and put make on PATH (avoids a space-in-path in the
# MAKE_PATH string). Install with: winget install GnuWin32.Make
if (-not (Get-Command make -ErrorAction SilentlyContinue)) {
  $makeCandidates = @("C:\Program Files (x86)\GnuWin32\bin\make.exe",
                      "C:\Program Files\GnuWin32\bin\make.exe")
  $make = $makeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $make) { throw "GNU make not found - run: winget install GnuWin32.Make" }
  $env:PATH = (Split-Path -Parent $make) + ";" + $env:PATH
  Write-Host "==> using make: $make" -ForegroundColor DarkGray
}

# The ACPI tables (AcpiTables.inf -> Dsdt.aml) compile with the iasl ACPI compiler:
#   tools_def: *_CLANGPDB_*_ASL_PATH = iasl   (bare name -> must be on PATH)
# It is fetched into a versioned temp dir (acpica-iasl-YYYYMMDD). If it is not on
# PATH the build dies at AcpiTables with: '"iasl"' is not recognized. Auto-locate it.
if (-not (Get-Command iasl -ErrorAction SilentlyContinue)) {
  $iaslCandidates = @()
  $iaslCandidates += Get-ChildItem -Path $env:TEMP -Filter "acpica-iasl-*" -Directory -EA SilentlyContinue |
                     Sort-Object Name -Descending |
                     ForEach-Object { Join-Path $_.FullName "iasl.exe" }
  $iaslCandidates += @(
    (Join-Path $ws "reference\Project-Mu-Silicium\Silicium-ACPI\Compiler\iasl.exe"),
    (Join-Path $ws "reference\Mu-Silicium\Silicium-ACPI\Compiler\iasl.exe"),
    "C:\ASL\iasl.exe",
    "C:\Program Files\iasl\iasl.exe"
  )
  $iasl = $iaslCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $iasl) {
    throw "iasl (ACPI compiler) not found on PATH or in `$env:TEMP\acpica-iasl-*. " +
          "Fetch ACPICA iasl for Windows and put its folder on PATH."
  }
  $env:PATH = (Split-Path -Parent $iasl) + ";" + $env:PATH
  Write-Host "==> using iasl: $iasl" -ForegroundColor DarkGray
}

$buildpy = Join-Path $edk2 "BaseTools\Source\Python\build\build.py"
$usbDebugModeValue = switch ($UsbDebugMode) {
  "Off"      { 0 }
  "Snapshot" { 1 }
  "Hid"      { 2 }
}
$buildArgs = @(
  "-a", "AARCH64",
  "-t", $Toolchain,
  "-p", $dsc,
  "-b", $Target
)
if ($Platform -eq "Star2LtePkg") {
  # The STAR2LTE_* knobs only exist in the 9810 platform; the 9820 DSC has no
  # such DEFINEs, so passing them there is a build error, not a no-op.
  $buildArgs += @("-D", "STAR2LTE_USB_DEBUG_MODE=$usbDebugModeValue")
  $buildArgs += @("-D", "STAR2LTE_RAM_STAGE=$RamStage")
}
if ($Module) { $buildArgs += @("-m", $Module) }
if ($Clean)  { $buildArgs += "cleanall" }

Set-Location $edk2
Write-Host "==> python build.py $($buildArgs -join ' ')" -ForegroundColor Cyan
python $buildpy @buildArgs
exit $LASTEXITCODE
