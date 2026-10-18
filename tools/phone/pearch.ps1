# pearch.ps1 PATH... - PE machine of each file (x86 / x64 / ARM64 / ARM64X-or-EC hint from the CHPE load config is not
# read - 0x8664 means x64 or ARM64EC). Used to find x64 apps for G27 (notes/g27-x64-apps.md).
param([Parameter(ValueFromRemainingArguments)][string[]]$Paths)
foreach ($p in $Paths) {
  foreach ($f in Get-Item $p -ErrorAction SilentlyContinue) {
    $b = [IO.File]::ReadAllBytes($f.FullName)
    $pe = [BitConverter]::ToInt32($b, 0x3c); $m = [BitConverter]::ToUInt16($b, $pe + 4)
    $n = @{ 0x14c = 'x86'; 0x8664 = 'x64'; 0xaa64 = 'ARM64' }[[int]$m]
    '{0,-6} 0x{1:x4} {2}' -f $n, $m, $f.FullName
  }
}
