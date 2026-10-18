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
  6 x86      G-game (notes/g17-dxva-mfc.md): the 32-bit x86 UMD for WOW64 processes (MS Learn "WDDM 64-Bit Issues":
             UserModeDriverNameWow, SysWOW64). clang-cl --target=i686-pc-windows-msvc + MSVC x86 libs, a cross
             build that runs its own x86 panfrost_compile.exe on the x64 host (-Dprecomp-compiler=enabled,
             needs_exe_wrapper = false): the precompiled shader blobs memcpy Mesa structs, so the compiler that
             writes them must have the x86 layout. Collected to $Out-x86 (maliumd32.dll + libgallium_d3d10.dll).
  7 ec       G27 (notes/g27-x64-apps.md): the ARM64EC UMD for x64 (emulated) processes, behind an ARM64X forwarder
             (make-arm64x.ps1). clang-cl --target=arm64ec-pc-windows-msvc, link /machine:arm64ec with the MSVC
             arm64ec libs (MS Learn "Get started with Arm64EC": the Arm64 developer environment, /arm64EC,
             /MACHINE:ARM64EC); the x64 host panfrost_compile.exe fits (ARM64EC uses the x64 ABI). Static CRT like
             the others. Collected to $Out-ec (maliumd_ec.dll).
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
  if ($s -notin 'all', 'source', 'libpan', 'host', 'cross', 'collect', 'x86', 'ec') { throw "unknown stage '$s'" }
}

$MesaOrig = Join-Path $Mali 'mesa-26.2'          # never modified
$Mesa = Join-Path $Mali 'mesa-26.2-g76'          # patched working copy
$Ref = Join-Path $Mali 'wddm-mali-g52'
$Work = Join-Path $Mali 'g12-work'
$Venv = Join-Path $Mali 'venv'
$Libpan = Join-Path $Work 'libpan-prebuilt'
$HostBuild = Join-Path $Work 'build-host-x64'
$CrossBuild = Join-Path $Work 'build-arm64'
$X86Build = Join-Path $Work 'build-x86'
$EcBuild = Join-Path $Work 'build-arm64ec'
$Flex = Join-Path $Work 'winflexbison'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
function Want($s) { ($Stage -contains 'all' -and $s -notin 'x86', 'ec') -or $Stage -contains $s }   # x86 / ec only on request
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
  # G24 packaging: the UMD links the CRT statically (b_vscrt=mt) - its only non-OS import was VCRUNTIME140.dll, which a
  # clean system does not have (a driver must not need the VC redistributable). Applied to existing build dirs too.
  meson configure $CrossBuild -Db_vscrt=mt; Check 'cross meson configure b_vscrt'
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

if (Want 'x86') {
  Use-VcVars 'vcvarsamd64_x86.bat'   # first: it resets the environment (x64-hosted tools, x86 libs)
  Common-Tools
  Remove-Item env:CC, env:CXX -ErrorAction SilentlyContinue
  $cl = (Join-Path $Llvm 'clang-cl.exe') -replace '\\', '/'
  $lib = (Join-Path $Llvm 'llvm-lib.exe') -replace '\\', '/'
  $rc = (Join-Path $Llvm 'llvm-rc.exe') -replace '\\', '/'
  New-Item -ItemType Directory -Force $Work | Out-Null
  $x86cross = Join-Path $Work 'x86-windows.cross'
  @"
[binaries]
c = ['$cl', '--target=i686-pc-windows-msvc']
cpp = ['$cl', '--target=i686-pc-windows-msvc']
c_ld = 'link'
cpp_ld = 'link'
ar = '$lib'
windres = '$rc'

[properties]
needs_exe_wrapper = false

[host_machine]
system = 'windows'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
"@ | Set-Content -Encoding ascii $x86cross
  if (-not (Test-Path (Join-Path $X86Build 'build.ninja'))) {
    meson setup $X86Build $Mesa --backend=ninja --cross-file $x86cross -Dbuildtype=debugoptimized -Dprecomp-compiler=enabled @MesonOpts
    Check 'x86 meson setup'
  }
  meson configure $X86Build -Db_vscrt=mt; Check 'x86 meson configure b_vscrt'   # static CRT, as the ARM64 UMD
  ninja -C $X86Build -k 0 2>&1 | Tee-Object (Join-Path $Work 'build-x86.log')
  Check 'x86 build'
  $umd = Join-Path $X86Build 'src\gallium\targets\d3d10umd\libgallium_d3d10.dll'
  if (-not (Test-Path $umd)) { throw 'x86 libgallium_d3d10.dll was not built' }
  $o86 = "$Out-x86"
  New-Item -ItemType Directory -Force $o86 | Out-Null
  Copy-Item $umd, ($umd -replace '\.dll$', '.pdb') $o86 -Force
  Copy-Item $umd (Join-Path $o86 'maliumd32.dll') -Force
  Push-Location $o86
  Get-ChildItem -Path * -File -Include *.dll, *.pdb | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { '{0}  {1}' -f $_.Hash.ToLower(), (Split-Path -Leaf $_.Path) } | Set-Content -Encoding ascii SHA256SUMS
  & { dumpbin /nologo /headers libgallium_d3d10.dll | Select-String 'machine \(' ; dumpbin /nologo /dependents libgallium_d3d10.dll } |
    Set-Content -Encoding ascii dumpbin-x86.txt
  Pop-Location
  Get-Content (Join-Path $o86 'SHA256SUMS')
  Get-Content (Join-Path $o86 'dumpbin-x86.txt')
}

if (Want 'ec') {
  Use-VcVars 'vcvarsamd64_arm64.bat'   # the Arm64 developer environment (MS Learn: Arm64EC is built from it)
  Common-Tools
  # meson (1.12.1) canonicalises clang-cl's target 'arm64ec' to machine 'arm' (compilers/mixins/visualstudio.py:
  # `elif 'arm' in target`), so every link / lib rule got /MACHINE:arm (LNK1423), and lib/llvm-lib accept EC objects
  # only with /machine:arm64ec. Own venv with that one mapping added; the shared venv stays stock.
  $VenvEc = Join-Path $Mali 'venv-ec'
  if (-not (Test-Path "$VenvEc\Scripts\meson.exe")) {
    & python -m venv $VenvEc; Check 'venv-ec'
    & "$VenvEc\Scripts\python.exe" -m pip install -q meson==1.12.1 ninja mako pyyaml packaging; Check 'pip ec'
  }
  $vsmix = Join-Path $VenvEc 'Lib\site-packages\mesonbuild\compilers\mixins\visualstudio.py'
  $src = [IO.File]::ReadAllText($vsmix)
  if ($src -notmatch "'arm64ec' in target") {
    $anchor = "        elif 'aarch64' in target:"
    if (-not $src.Contains($anchor)) { throw "meson visualstudio.py: no aarch64 anchor ($vsmix)" }
    $src = $src.Replace($anchor, "        elif 'arm64ec' in target:  # exynos-woa G27`n            self.machine = 'arm64ec'`n$anchor")
    [IO.File]::WriteAllText($vsmix, $src)
  }
  $env:PATH = "$VenvEc\Scripts;$env:PATH"
  Remove-Item env:CC, env:CXX -ErrorAction SilentlyContinue
  $env:PATH = "$HostBuild\src\panfrost\clc;$env:PATH"   # panfrost_compile.exe (x64 ABI = the ARM64EC ABI)
  # the MSVC arm64ec CRT / runtime libraries first (vcvarsamd64_arm64 lists lib\arm64)
  $ecLib = ($env:LIB -split ';' | Where-Object { $_ -match '\\VC\\Tools\\MSVC\\[^\\]+\\lib\\arm64$' } |
            Select-Object -First 1) -replace '\\arm64$', '\arm64ec'
  if (-not $ecLib -or -not (Test-Path $ecLib)) { throw "no MSVC arm64ec library directory ($ecLib)" }
  $env:LIB = "$ecLib;$env:LIB"
  $cl = (Join-Path $Llvm 'clang-cl.exe') -replace '\\', '/'
  $lib = (Join-Path $Llvm 'llvm-lib.exe') -replace '\\', '/'
  $rc = (Join-Path $Llvm 'llvm-rc.exe') -replace '\\', '/'
  New-Item -ItemType Directory -Force $Work | Out-Null
  $eccross = Join-Path $Work 'arm64ec-windows.cross'
  @"
[binaries]
c = ['$cl', '--target=arm64ec-pc-windows-msvc']
cpp = ['$cl', '--target=arm64ec-pc-windows-msvc']
c_ld = 'link'
cpp_ld = 'link'
ar = '$lib'
windres = '$rc'

[built-in options]
c_link_args = ['/machine:arm64ec']
cpp_link_args = ['/machine:arm64ec']

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
"@ | Set-Content -Encoding ascii $eccross
  if (-not (Test-Path (Join-Path $EcBuild 'build.ninja'))) {
    meson setup $EcBuild $Mesa --backend=ninja --cross-file $eccross -Dbuildtype=debugoptimized -Dprecomp-compiler=system @MesonOpts
    Check 'ec meson setup'
  }
  meson configure $EcBuild -Db_vscrt=mt; Check 'ec meson configure b_vscrt'   # static CRT, as the ARM64 UMD
  ninja -C $EcBuild -k 0 2>&1 | Tee-Object (Join-Path $Work 'build-arm64ec.log')
  Check 'ec build'
  $umd = Join-Path $EcBuild 'src\gallium\targets\d3d10umd\libgallium_d3d10.dll'
  if (-not (Test-Path $umd)) { throw 'arm64ec libgallium_d3d10.dll was not built' }
  $oec = "$Out-ec"
  New-Item -ItemType Directory -Force $oec | Out-Null
  Copy-Item $umd, ($umd -replace '\.dll$', '.pdb') $oec -Force
  Copy-Item $umd (Join-Path $oec 'maliumd_ec.dll') -Force
  Push-Location $oec
  Get-ChildItem -Path * -File -Include *.dll, *.pdb | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { '{0}  {1}' -f $_.Hash.ToLower(), (Split-Path -Leaf $_.Path) } | Set-Content -Encoding ascii SHA256SUMS
  & { dumpbin /nologo /headers libgallium_d3d10.dll | Select-String 'machine \(' ; dumpbin /nologo /dependents libgallium_d3d10.dll } |
    Set-Content -Encoding ascii dumpbin-ec.txt
  Pop-Location
  Get-Content (Join-Path $oec 'SHA256SUMS')
  Get-Content (Join-Path $oec 'dumpbin-ec.txt')
}
