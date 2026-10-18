# G27.3 (notes/g27-x64-apps.md): the ARM64X pure forwarder maliumd.dll, so one UMD name serves ARM64 processes and x64
# (emulated) processes. Steps from MS Learn "Build Arm64X Files", section "Building an Arm64X pure forwarder DLL"
# (https://learn.microsoft.com/en-us/windows/arm/arm64x-build): empty arm64 + /arm64EC objects, two DEF files, two import
# libs, then `link /dll /noentry /machine:arm64x /defArm64Native:<arm64 def> /def:<x64 def>`.
#   -Arm64 <dir>  build-mesa-g76 collect output (maliumd.dll, ARM64)
#   -Ec <dir>     build-mesa-g76 -Stage ec output (maliumd_ec.dll, ARM64EC)
#   -Out <dir>    gets maliumd.dll (forwarder), maliumd_arm64.dll, maliumd_ec.dll
# The export list is read from the ARM64 DLL (dumpbin /exports) and must be identical in the EC DLL.
param([Parameter(Mandatory)][string]$Arm64, [Parameter(Mandatory)][string]$Ec, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
function Check($what) { if ($LASTEXITCODE) { throw "$what failed ($LASTEXITCODE)" } }

# the Arm64 developer environment (MS Learn: "from the Arm64 developer command prompt")
$vs = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsamd64_arm64.bat'
$lines = cmd /c "`"$vs`" >nul 2>nul && set"
Check 'vcvars'
foreach ($l in $lines) { if ($l -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }

function Exports($dll) {
  $names = dumpbin /nologo /exports $dll | ForEach-Object {
    if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)') { $Matches[1] }
  }
  Check "dumpbin $dll"
  @($names | Sort-Object)
}

$a64 = Join-Path $Arm64 'maliumd.dll'
$ec = Join-Path $Ec 'maliumd_ec.dll'
$ea = Exports $a64
$ee = Exports $ec
if ($ea.Count -lt 2) { throw "no exports read from $a64" }
$diff = Compare-Object $ea $ee
if ($diff) { $diff | Format-Table | Out-String | Write-Host; throw 'ARM64 and ARM64EC export lists differ' }

New-Item -ItemType Directory -Force $Out | Out-Null
$tmp = Join-Path $Out 'arm64x-obj'
New-Item -ItemType Directory -Force $tmp | Out-Null
Get-ChildItem $tmp -File | Remove-Item -Force   # a 0-byte .lib from a failed run makes link /lib fail (LNK1136)
Push-Location $tmp
try {
  Set-Content -Encoding ascii empty.cpp ''
  cl /nologo /c /Foempty_arm64.obj empty.cpp; Check 'cl arm64'
  cl /nologo /c /arm64EC /Foempty_x64.obj empty.cpp; Check 'cl arm64ec'
  (@('EXPORTS') + ($ea | ForEach-Object { "    $_ = maliumd_ec.$_" })) | Set-Content -Encoding ascii maliumd_x64.def
  (@('EXPORTS') + ($ea | ForEach-Object { "    $_ = maliumd_arm64.$_" })) | Set-Content -Encoding ascii maliumd_arm64.def
  link /lib /nologo /machine:x64 /def:maliumd_x64.def /out:maliumd_x64.lib; Check 'link /lib x64'
  link /lib /nologo /machine:arm64 /def:maliumd_arm64.def /out:maliumd_arm64.lib; Check 'link /lib arm64'
  link /nologo /dll /noentry /machine:arm64x /defArm64Native:maliumd_arm64.def /def:maliumd_x64.def `
    empty_arm64.obj empty_x64.obj /out:maliumd.dll maliumd_arm64.lib maliumd_x64.lib; Check 'link arm64x'
} finally { Pop-Location }

Copy-Item (Join-Path $tmp 'maliumd.dll') $Out -Force
Copy-Item $a64 (Join-Path $Out 'maliumd_arm64.dll') -Force
Copy-Item $ec (Join-Path $Out 'maliumd_ec.dll') -Force
Push-Location $Out
Get-ChildItem -File maliumd*.dll | Get-FileHash -Algorithm SHA256 |
  ForEach-Object { '{0}  {1}' -f $_.Hash.ToLower(), (Split-Path -Leaf $_.Path) } | Set-Content -Encoding ascii SHA256SUMS
& { foreach ($f in 'maliumd.dll', 'maliumd_arm64.dll', 'maliumd_ec.dll') {
      "== $f"; dumpbin /nologo /headers $f | Select-String 'machine \('; dumpbin /nologo /exports $f | Select-String 'forwarded' | Select-Object -First 3 } } |
  Set-Content -Encoding ascii dumpbin-arm64x.txt
Pop-Location
"$($ea.Count) exports forwarded"
Get-Content (Join-Path $Out 'SHA256SUMS')
Get-Content (Join-Path $Out 'dumpbin-arm64x.txt')
