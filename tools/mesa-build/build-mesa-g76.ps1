<#
M16 G12 - Mesa 26.2 (gallium d3d10umd + panfrost + softpipe) for Windows ARM64, for the S10e Mali-G76.

Follows wddm-mali-g52 .github/workflows/mesa.yml (job `panfrost`), adapted to an x64 build host:
  1 source   clone the local Mesa 26.2 clone into a working copy (core.autocrlf=false), copy the
             reference's win32-compat shim + include/malikm_ioctl.h + include/maliwddm_abi.h into
             include/win32-compat, apply mesa-patches 0001-0019 after CRLF->LF, commit locally.
  2 libpan   libpan_v*.{spv,cpp,h} on Linux (WSL, user-space conda-forge LLVM 18) - tools/mesa-build/wsl-libpan.sh.
  3 host     panfrost_compile.exe built natively for Windows x64 (clang-cl, MSVC ABI). The reference runs it
             natively on windows-11-arm; it memcpys struct pan_shader_info into the shader blobs, so it
             must have the MSVC layout, not GCC's (notes/gpu-m16-g12-mesa.md step 9).
  4 cross    the ARM64 build: clang-cl --target=aarch64-pc-windows-msvc + MSVC ARM64 libs/link,
             -Dmesa-clc=system (reference stubs) -Dprecomp-compiler=system (step 3's exe).
  5 collect  DLLs -> s10e-artifacts\mali-g12 with SHA256SUMS and dumpbin /headers.
Nothing of Mesa is vendored in this repo; all paths below are external.
#>
param(
  # One or more of all, source, libpan, host, cross, collect. `powershell -File` passes "host,cross"
  # as one string, so commas are split below.
  [string[]]$Stage = 'all',
  [string]$Mali = 'C:\Users\ilya\code\mali',
  [string]$Out = 'C:\Users\ilya\code\s10e-artifacts\mali-g12',
  [string]$Llvm = 'C:\Program Files\LLVM\bin',
  [string]$VsBuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build'
)
# Native tools write to stderr; PowerShell 5.1 turns that into errors under 'Stop'. Failures are
# caught by Check (exit codes) instead.
$ErrorActionPreference = 'Continue'

$Stage = @($Stage | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($s in $Stage) {
  if ($s -notin 'all', 'source', 'libpan', 'host', 'cross', 'collect') { throw "unknown stage '$s'" }
}

$MesaOrig = Join-Path $Mali 'mesa-26.2'          # never modified
$Mesa = Join-Path $Mali 'mesa-26.2-g76'          # patched working copy
$Ref = Join-Path $Mali 'wddm-mali-g52'
$Work = Join-Path $Mali 'g12-work'
$Venv = Join-Path $Mali 'venv'
$Libpan = Join-Path $Work 'libpan-prebuilt'
$HostBuild = Join-Path $Work 'build-host-x64'
$CrossBuild = Join-Path $Work 'build-arm64'
$Flex = Join-Path $Work 'winflexbison'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
function Want($s) { $Stage -contains 'all' -or $Stage -contains $s }
function Check($what) { if ($LASTEXITCODE) { throw "$what failed ($LASTEXITCODE)" } }
function ToWsl($p) { '/mnt/' + $p.Substring(0, 1).ToLower() + ($p.Substring(2) -replace '\\', '/') }

# Import a vcvars environment into this process. The host (x64) and cross (ARM64) stages each need a
# clean one: running vcvarsamd64_arm64 on top of vcvars64 would leave x64 INCLUDE/LIB/PATH entries
# behind, so every call starts again from the environment the script was started with.
$CleanEnv = @{}
Get-ChildItem env: | ForEach-Object { $CleanEnv[$_.Name] = $_.Value }
function Use-VcVars($bat) {
  Get-ChildItem env: | Where-Object { -not $CleanEnv.ContainsKey($_.Name) } | ForEach-Object { Remove-Item "env:$($_.Name)" }
  foreach ($k in $CleanEnv.Keys) { Set-Item -Path "env:$k" -Value $CleanEnv[$k] }
  # vcvars complains about vswhere.exe on stderr; harmless, but PowerShell 5.1 would make it fatal.
  $lines = cmd /c "`"$(Join-Path $VsBuild $bat)`" >nul 2>nul && set"
  Check $bat
  foreach ($l in $lines) { if ($l -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
}

function Common-Tools {
  if (-not (Test-Path "$Venv\Scripts\meson.exe")) {
    & python -m venv $Venv; Check 'venv'
    & "$Venv\Scripts\python.exe" -m pip install -q meson ninja mako pyyaml packaging; Check 'pip'
  }
  if (-not (Test-Path "$Flex\win_flex.exe")) {
    $dl = Join-Path $Work 'dl-winflexbison'
    New-Item -ItemType Directory -Force $dl | Out-Null
    Invoke-WebRequest 'https://github.com/lexxmark/winflexbison/releases/download/v2.5.25/win_flex_bison-2.5.25.zip' -OutFile "$dl\wfb.zip"
    $h = (Get-FileHash "$dl\wfb.zip").Hash
    if ($h -ne '8D324B62BE33604B2C45AD1DD34AB93D722534448F55A16CA7292DE32B6AC135') { throw "winflexbison sha256 $h" }
    Expand-Archive "$dl\wfb.zip" -DestinationPath $Flex -Force
  }
  $env:PATH = "$Venv\Scripts;$Flex;$Ref\ci\clc-stubs;$env:PATH"
  $env:LIBPAN_PREBUILT = $Libpan
}

# Options shared by the host and the cross build: mesa.yml's `panfrost` job configure line.
$MesonOpts = @(
  '-Dplatforms=windows', '-Dgallium-drivers=softpipe,panfrost', '-Dgallium-d3d10umd=true',
  '-Dmesa-clc=system', '-Dvulkan-drivers=', '-Dllvm=disabled',
  '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled', '-Degl=disabled', '-Dglx=disabled', '-Dgbm=disabled',
  '-Dvideo-codecs=', '-Dmicrosoft-clc=disabled', '-Dspirv-to-dxil=false',
  '-Dzstd=disabled', '-Dshader-cache=disabled', '-Dbuild-tests=false',
  '-Dc_args=-DHAVE_FUNC_ATTRIBUTE_PACKED', '-Dcpp_args=-DHAVE_FUNC_ATTRIBUTE_PACKED'
)

if (Want 'source') {
  if (Test-Path $Mesa) { throw "$Mesa exists - delete it to re-create the working copy" }
  git clone -c core.autocrlf=false --no-local $MesaOrig $Mesa; Check 'clone'
  $lf = Join-Path $Work 'patches-lf'
  New-Item -ItemType Directory -Force $lf | Out-Null
  Copy-Item -Recurse "$Ref\win32-compat" "$Mesa\include\win32-compat"
  Copy-Item "$Ref\include\malikm_ioctl.h", "$Ref\include\maliwddm_abi.h" "$Mesa\include\win32-compat\"
  Get-ChildItem "$Ref\mesa-patches" -Filter *.patch | Sort-Object Name | ForEach-Object {
    $t = [IO.File]::ReadAllText($_.FullName) -replace "`r", ''
    [IO.File]::WriteAllText((Join-Path $lf $_.Name), $t)
    Write-Host "applying $($_.Name)"
    git -C $Mesa apply --whitespace=nowarn (Join-Path $lf $_.Name); Check "patch $($_.Name)"
  }
  git -C $Mesa add -A; Check 'add'
  git -C $Mesa -c user.name=g12-build -c user.email=g12@local commit -q -m 'G12: wddm-mali-g52 mesa-patches 0001-0019 (LF) + win32-compat shim'; Check 'commit'
  # exynos-woa's own patches (G13 on, LF like the blobs they patch), each committed on its own.
  Get-ChildItem (Join-Path $Here 'patches') -Filter *.patch -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object {
    Write-Host "applying $($_.Name)"
    git -C $Mesa apply --whitespace=nowarn $_.FullName; Check "patch $($_.Name)"
    git -C $Mesa add -A; Check 'add'
    git -C $Mesa -c user.name=exynos-woa -c user.email=build@local commit -q -m "exynos-woa $($_.Name)"; Check 'commit'
  }
}

if (Want 'libpan') {
  $sh = ToWsl (Join-Path $Here 'wsl-libpan.sh')
  wsl.exe -d Ubuntu -- bash $sh (ToWsl $Mesa) (ToWsl $Libpan); Check 'wsl libpan'
}

if (Want 'host') {
  Use-VcVars 'vcvars64.bat'   # first: it resets the environment
  Common-Tools
  # Meson splits CC like a shell would; a path with a space breaks it, so put LLVM on PATH instead.
  $env:PATH = "$Llvm;$env:PATH"
  $env:CC = 'clang-cl'; $env:CXX = 'clang-cl'
  if (-not (Test-Path "$HostBuild\build.ninja")) {
    meson setup $HostBuild $Mesa --backend=ninja -Dbuildtype=release -Dprecomp-compiler=enabled @MesonOpts; Check 'host meson setup'
  }
  ninja -C $HostBuild src/panfrost/clc/panfrost_compile.exe; Check 'host panfrost_compile'
}

if (Want 'cross') {
  Use-VcVars 'vcvarsamd64_arm64.bat'   # first: it resets the environment
  Common-Tools
  Remove-Item env:CC, env:CXX -ErrorAction SilentlyContinue
  $env:PATH = "$HostBuild\src\panfrost\clc;$env:PATH"   # panfrost_compile.exe (x64, MSVC ABI)
  $cl = (Join-Path $Llvm 'clang-cl.exe') -replace '\\', '/'
  $lib = (Join-Path $Llvm 'llvm-lib.exe') -replace '\\', '/'
  $rc = (Join-Path $Llvm 'llvm-rc.exe') -replace '\\', '/'
  New-Item -ItemType Directory -Force $Work | Out-Null
  $cross = Join-Path $Work 'arm64-windows.cross'
  @"
[binaries]
c = ['$cl', '--target=aarch64-pc-windows-msvc']
cpp = ['$cl', '--target=aarch64-pc-windows-msvc']
c_ld = 'link'
cpp_ld = 'link'
ar = '$lib'
windres = '$rc'

[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
"@ | Set-Content -Encoding ascii $cross
  if (-not (Test-Path "$CrossBuild\build.ninja")) {
    meson setup $CrossBuild $Mesa --backend=ninja --cross-file $cross -Dbuildtype=debugoptimized -Dprecomp-compiler=system @MesonOpts
    Check 'cross meson setup'
  }
  ninja -C $CrossBuild -k 0 2>&1 | Tee-Object (Join-Path $Work 'build-arm64.log')
  Check 'cross build'
}

if (Want 'collect') {
  # The UMD is libgallium_d3d10.dll. wddm-mali-g52's maliwddm.inf names it `maliumd.dll`
  # (HKR,,UserModeDriverName,REG_MULTI_SZ, maliumd.dll x4) and their test scripts copy it to System32
  # under that name, so ship both names (same bytes). Its only non-OS import is VCRUNTIME140.dll; the
  # reference installs vc_redist.arm64.exe for that (tools/windows/m3-test.cmd), so ship that too.
  # zlib's z-1.dll is a meson subproject by-product, not imported by the UMD (dumpbin /dependents).
  Use-VcVars 'vcvarsamd64_arm64.bat'
  $umd = Join-Path $CrossBuild 'src\gallium\targets\d3d10umd\libgallium_d3d10.dll'
  if (-not (Test-Path $umd)) { throw 'libgallium_d3d10.dll was not built' }
  $redist = Get-ChildItem (Join-Path (Split-Path (Split-Path $VsBuild)) 'Redist\MSVC') -Recurse -Filter vc_redist.arm64.exe | Select-Object -First 1
  New-Item -ItemType Directory -Force $Out | Out-Null
  Copy-Item $umd, ($umd -replace '\.dll$', '.pdb') $Out -Force
  Copy-Item $umd (Join-Path $Out 'maliumd.dll') -Force
  if ($redist) { Copy-Item $redist.FullName $Out -Force }
  Push-Location $Out
  Get-ChildItem -Path * -File -Include *.dll, *.pdb, *.exe | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { '{0}  {1}' -f $_.Hash.ToLower(), (Split-Path -Leaf $_.Path) } | Set-Content -Encoding ascii SHA256SUMS
  dumpbin /nologo /headers libgallium_d3d10.dll | Set-Content -Encoding ascii dumpbin-headers.txt
  & { dumpbin /nologo /dependents libgallium_d3d10.dll; dumpbin /nologo /exports libgallium_d3d10.dll } | Set-Content -Encoding ascii dumpbin-deps-exports.txt
  Pop-Location
  Get-Content (Join-Path $Out 'SHA256SUMS')
  Select-String -Path (Join-Path $Out 'dumpbin-headers.txt') -Pattern 'machine \(|subsystem|DLL characteristics' | ForEach-Object Line
}
