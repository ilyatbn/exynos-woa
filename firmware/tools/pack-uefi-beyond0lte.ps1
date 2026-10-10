<#
.SYNOPSIS
  Package the Beyond0LtePkg EDK II firmware (BEYOND0LTE_UEFI.fd) into a boot
  image for the Galaxy S10e (beyond0lte), wrapped in a container this phone's
  sboot already accepts.

.DESCRIPTION
  The 9810 packer (pack-uefi.ps1) is NOT edited - this is the beyond0lte
  sibling, because the shim, the FD name and the reference container all differ
  per phone. Three steps, same shape as the 9810 pipeline:
    1. Assemble the 9820 shim (tools/beyond0lte-bootshim/bootshim.S) and LINK it
       (a -c-only object can leave the adr/branch offsets unapplied - see
       pack-uefi.ps1:86-88 and the code0 check at :102-107).
    2. Concatenate [shim][FD] -> uefi-beyond0lte-kernel.bin.
    3. Wrap that as the "kernel" of a reference container
       (tools/repack-with-ref.py), which reads page_size/section order from the
       ref itself - the S10e's containers use page_size 2048, not 4096
       (notes/m8-uefi-image.md Step 16).

  There is NO default reference image here: nothing in this checkout is an S10e
  .img, so -Ref is required. The provenance of a good ref is a read off the
  phone (see notes/m8-uefi-image.md Step 19 for the exact dd line).

  This produces an image. It does NOT flash anything: ground rules 5/6 forbid
  writing to the media until Windows booting is established.

.PARAMETER Target     DEBUG (default) or RELEASE - selects which Build/ FD to pack.
.PARAMETER Toolchain EDK II toolchain directory containing the validated FD.
.PARAMETER Ref       Reference boot image this sboot accepts (required).
.PARAMETER Output   Explicit output image.
.PARAMETER UefiBase  Must equal PcdFdBaseAddress in Beyond0LtePkg.dsc:439.
.PARAMETER SkipFreshCheck Skip the stale-FD guard (GNU make 3.81 fails silently).
#>
[CmdletBinding()]
param(
  [ValidateSet("DEBUG", "RELEASE")] [string]$Target = "DEBUG",
  [ValidateSet("CLANGPDB", "CLANGDWARF")] [string]$Toolchain = "CLANGPDB",
  [string]$Ref = "",
  [string]$Output = "",
  [string]$UefiBase = "0x90000000",
  [string]$Dtb = "",
  [string]$ImageSize = "0x800000",
  [switch]$SkipFreshCheck
)
$ErrorActionPreference = "Stop"
if (-not $Ref) { throw "No default reference image for the S10e - pass -Ref (see notes/m8-uefi-image.md Step 19)." }

$ws    = Split-Path -Parent $PSScriptRoot
$llvm  = "C:\Program Files\LLVM\bin"
$clang = Join-Path $llvm "clang.exe"
$lld   = Join-Path $llvm "ld.lld.exe"
$ocopy = Join-Path $llvm "llvm-objcopy.exe"
foreach ($t in @($clang, $lld, $ocopy)) {
  if (-not (Test-Path $t)) { throw "Missing tool: $t (winget install LLVM.LLVM)" }
}

$buildFv = Join-Path $ws "edk2\Build\Beyond0LtePkg\${Target}_${Toolchain}\FV"
$fd = Join-Path $buildFv "BEYOND0LTE_UEFI.fd"
if (-not (Test-Path $fd)) {
  throw "FD not found: $fd`nBuild it first: .\tools\build-uefi.ps1 -Target $Target -Toolchain $Toolchain"
}
if (-not (Test-Path $Ref)) { throw "Reference boot image not found: $Ref" }

if (-not $SkipFreshCheck) {
  $fdTime = (Get-Item $fd).LastWriteTime
  $newer = Get-ChildItem -Path (Join-Path $ws "Platform\Beyond0LtePkg") -Recurse -File -Include *.c,*.h,*.S,*.dsc,*.fdf,*.inf |
    Where-Object { $_.LastWriteTime -gt $fdTime } | Select-Object -First 3
  if ($newer) {
    Write-Warning "FD ($fdTime) is OLDER than these sources - did the build silently fail?"
    $newer | ForEach-Object { Write-Warning ("  " + $_.FullName.Substring($ws.Length + 1) + "  (" + $_.LastWriteTime + ")") }
    throw "Stale FD. Re-run .\tools\build-uefi.ps1 -Target $Target -Toolchain $Toolchain and confirm '- Done -', or pass -SkipFreshCheck."
  }
}

$shimDir = Join-Path $ws "tools\beyond0lte-bootshim"
$shimS   = Join-Path $shimDir "bootshim.S"
$shimObj = Join-Path $shimDir "bootshim.o"
$shimElf = Join-Path $shimDir "bootshim.elf"
$shimBin = Join-Path $shimDir "bootshim.bin"

$fdLen   = (Get-Item $fd).Length
$uefiSize = [int][math]::Ceiling($fdLen / 16) * 16
$uefiSizeHex = "0x{0:X}" -f $uefiSize

Write-Host "==> FD: $fd ($fdLen bytes)" -ForegroundColor Cyan
Write-Host "==> shim: UEFI_BASE=$UefiBase UEFI_SIZE=$uefiSizeHex" -ForegroundColor Cyan

& $clang -target aarch64-none-elf -ffreestanding -nostdlib -mgeneral-regs-only `
  "-DUEFI_BASE=$UefiBase" "-DUEFI_SIZE=$uefiSizeHex" -c $shimS -o $shimObj
if ($LASTEXITCODE -ne 0) { throw "clang failed assembling the shim" }
& $lld -o $shimElf $shimObj "-Ttext=$UefiBase" -e _head --no-relax
if ($LASTEXITCODE -ne 0) { throw "ld.lld failed linking the shim" }
& $ocopy -O binary $shimElf $shimBin
if ($LASTEXITCODE -ne 0) { throw "llvm-objcopy failed" }
$shimLen = (Get-Item $shimBin).Length
Write-Host "==> shim.bin: $shimLen bytes" -ForegroundColor DarkGray

$sb = [System.IO.File]::ReadAllBytes($shimBin)
$code0 = [BitConverter]::ToUInt32($sb, 0)
$magic = [System.Text.Encoding]::ASCII.GetString($sb, 56, 3)
if (($code0 -band 0x9F00001F) -ne 0x10000001) { throw ("shim code0 is not 'adr x1,#imm' (got 0x{0:X8}) - entry broken" -f $code0) }
if (($code0 -band 0x00FFFFE0) -eq 0) { throw "shim 'adr x1' immediate is 0 - _payload offset unresolved" }
if ($magic -ne "ARM") { throw "shim ARM64 header magic missing (got '$magic')" }
Write-Host ("==> shim header OK (code0=0x{0:X8}; magic ARM64)" -f $code0) -ForegroundColor DarkGray

$kernel = Join-Path $buildFv "uefi-beyond0lte-kernel.bin"
$shimBytes = [System.IO.File]::ReadAllBytes($shimBin)
$fdBytes   = [System.IO.File]::ReadAllBytes($fd)
if ($Dtb) {
  if (-not (Test-Path $Dtb)) { throw "DTB file not found: $Dtb" }
  $imgSize = [Convert]::ToInt64(($ImageSize -replace '^0x',''), 16)
  $used = $shimBytes.Length + $fdBytes.Length
  if ($imgSize -lt $used) { throw "ImageSize ($imgSize) < shim+FD ($used) - the DTB would land inside the payload" }
  $pad = [byte[]]::new($imgSize - $used)
  $dtbBytes = [System.IO.File]::ReadAllBytes($Dtb)
  $kernelBytes = [byte[]]$shimBytes + [byte[]]$fdBytes + [byte[]]$pad + [byte[]]$dtbBytes
  Write-Host ("==> kernel ([shim][FD][pad->0x{0:X}][DTB]): {1} ({2} bytes)" -f $imgSize, $kernel, $kernelBytes.Length) -ForegroundColor DarkGray
} else {
  $kernelBytes = [byte[]]$shimBytes + [byte[]]$fdBytes
  Write-Host "==> kernel ([shim][FD]): $kernel ($((Get-Item $kernel).Length) bytes)" -ForegroundColor DarkGray
}
[System.IO.File]::WriteAllBytes($kernel, $kernelBytes)

# 3. Wrap in the reference container.
$bootPartitionBytes = 14080L * 4096   # Validation.cs:12 - BOOT on star2lte AND beyond0lte
$refLen = (Get-Item $Ref).Length
if ($refLen -gt $bootPartitionBytes) {
  throw ("reference container ({0} B) already exceeds BOOT ({1} B)" -f $refLen, $bootPartitionBytes)
}
$kernelLen = (Get-Item $kernel).Length
if ($kernelLen -gt $bootPartitionBytes) {
  throw ("[shim][FD] alone ({0} B) exceeds BOOT ({1} B)" -f $kernelLen, $bootPartitionBytes)
}
$final = if ($Output) { $Output } else { Join-Path $ws "Platform\Beyond0LtePkg\beyond0lte-uefi-boot.img" }
python (Join-Path $ws "tools\repack-with-ref.py") --ref $Ref --kernel $kernel --output $final
if ($LASTEXITCODE -ne 0) { throw "repack-with-ref.py failed" }
if ((Get-Item $final).Length -gt $bootPartitionBytes) {
  throw ("output ({0} B) exceeds BOOT ({1} B) - it would not flash" -f (Get-Item $final).Length, $bootPartitionBytes)
}

Write-Host ""
Write-Host "==> Candidate boot image: $final ($((Get-Item $final).Length) bytes)" -ForegroundColor Green
Write-Host "    NOT flashed - ground rules 5/6 forbid writing to the media until Windows booting is established." -ForegroundColor Yellow
