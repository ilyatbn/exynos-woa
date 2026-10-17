# drvhash.ps1 -Name <service> - which .sys a kernel service runs from, its SHA256, and every DriverStore copy of it
# (checks that a TWRP sys-deploy.sh swap is the file Windows actually loads).
param([Parameter(Mandatory)][string]$Name)
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='$Name'"
"service $Name state $($svc.State) path $($svc.PathName)"
$p = $svc.PathName -replace '^\\\?\?\\', ''
if ($p -match '^\\SystemRoot\\') { $p = $p -replace '^\\SystemRoot', $env:SystemRoot }
if (Test-Path $p) { "loaded file  $((Get-FileHash $p).Hash) $((Get-Item $p).LastWriteTime)" }
Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository" -Recurse -Filter "$Name.sys" -ErrorAction SilentlyContinue |
  ForEach-Object { "driverstore  $((Get-FileHash $_.FullName).Hash) $($_.FullName)" }
