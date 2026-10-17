# dgvconf.ps1 [-Conf C:\Games\src\dgVoodoo.conf] [-Backup <suffix>] Key=Value ... - set dgVoodoo.conf keys (exact key
# name at line start), print the resulting lines. G24 NFSU2 display-mode experiments (notes/g24-compute-cs4x.md).
param([Parameter(Position = 0, ValueFromRemainingArguments)] [string[]]$Set, [string]$Conf = 'C:\Games\src\dgVoodoo.conf',
      [string]$Backup = '')
if (-not (Test-Path $Conf)) { throw "no $Conf" }
if ($Backup) { Copy-Item $Conf "$Conf.bak-$Backup" -Force }
$c = Get-Content $Conf
foreach ($kv in $Set) {
  $k, $v = $kv -split '=', 2
  $c = $c | ForEach-Object { if ($_ -match "^$([regex]::Escape($k))(\s+)=") { "$k$($Matches[1])= $v" } else { $_ } }
}
$c | Set-Content $Conf -Encoding ascii
$keys = ($Set | ForEach-Object { [regex]::Escape(($_ -split '=', 2)[0]) }) -join '|'
Select-String -Path $Conf -Pattern "^($keys)\s" | ForEach-Object Line
