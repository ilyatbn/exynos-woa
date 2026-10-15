# m14 (notes/m14-smp.md "RAM dump"): prepare C:\S10eMemDump.bin for the firmware's raw RAM dump after a Windows death.
#  - allocates the file and sets its valid data length (so ntfs-3g in TWRP reads what the firmware wrote, not zeros)
#  - stamps every MiB: 'S10EMDMP' + UINT64 MiB index; the firmware only writes an extent whose first and last MiB
#    carry the expected stamps, so it can never write outside this file even if NTFS moved it
#  - writes the cluster runs to <ESP>\S10EDIAG\memdump.ext:  "bpc <bytes per cluster>", "size <bytes>",
#    then one "<lcn hex> <clusters hex>" line per extent, in file order
param([string]$Esp = 'S:')
$ErrorActionPreference = 'Stop'
$Path = 'C:\S10eMemDump.bin'
$Size = 6208MB
$Mib  = 1MB

if (-not (Test-Path $Path) -or ((Get-Item $Path).Length -ne $Size)) {
  if (Test-Path $Path) { Remove-Item -Force $Path }
  fsutil file createnew $Path $Size | Out-Null
}
fsutil file setvaliddata $Path $Size | Out-Null

$Fs = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
$Stamp = New-Object byte[] 16
[System.Text.Encoding]::ASCII.GetBytes('S10EMDMP').CopyTo($Stamp, 0)
for ($i = 0; $i -lt ($Size / $Mib); $i++) {
  [BitConverter]::GetBytes([UInt64]$i).CopyTo($Stamp, 8)
  $Fs.Position = [Int64]$i * $Mib
  $Fs.Write($Stamp, 0, 16)
}
$Fs.Flush($true)
$Fs.Close()

$Bpc = ((fsutil fsinfo ntfsinfo C:) | Select-String 'Bytes Per Cluster') -replace '.*:\s*', ''
$Out = @("bpc $([int]$Bpc)", "size $Size")
foreach ($Line in (fsutil file queryextents $Path)) {
  if ($Line -match 'Clusters:\s*0x([0-9a-fA-F]+)\s+LCN:\s*0x([0-9a-fA-F]+)') {
    $Out += "$($Matches[2]) $($Matches[1])"
  }
}
$Out | Set-Content -Encoding ascii "$Esp\S10EDIAG\memdump.ext"
Write-Output ("memdump-prep: {0} extent(s), bpc {1}" -f ($Out.Count - 2), $Bpc)
