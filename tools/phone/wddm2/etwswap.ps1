# etwswap.ps1 -Cfg "comp seq 2" [-Name mali] - ETW capture (DXGI, D3D11, DWM core, DxgKrnl present/fence keywords)
# around one d3dswap run in the user session; writes C:\S10eTmp\etw-<Name>.etl. Decode on the PC with tracerpt.
# G15 S4: compare a composition swap chain on the Mali adapter (no DXGI tracked fence) with the same on WARP.
param([string]$Cfg = 'comp seq 2', [string]$Name = 'mali', [switch]$Redirect, [switch]$Kernel)
$etl = "C:\S10eTmp\etw-$Name.etl"
Remove-Item $etl -ErrorAction SilentlyContinue
logman stop s10eswap -ets 2>$null | Out-Null
logman start s10eswap -p Microsoft-Windows-DXGI 0xffffffffffffffff 5 -o $etl -ets | Out-Null
logman update s10eswap -p Microsoft-Windows-Direct3D11 0xffffffffffffffff 5 -ets | Out-Null
logman update s10eswap -p Microsoft-Windows-Dwm-Core 0xffffffffffffffff 5 -ets | Out-Null
if ($Kernel) { logman update s10eswap -p Microsoft-Windows-DxgKrnl 0xffffffffffffffff 4 -ets | Out-Null }   # -Kernel: dxgkrnl too (device errors, rejected submissions)
$pre = if ($Redirect) { 'set D3D10UMD_REDIRECT=1&& ' } else { '' }   # -Redirect: only the test process lets DXGI redirect
& C:\S10eTmp\runuser2.ps1 -Cmd "${pre}C:\S10eTmp\d3dswap.exe 4 $Cfg > C:\S10eTmp\d3dswap.log 2>&1" -Wait 40 | Out-Null
logman stop s10eswap -ets | Out-Null
Get-Item $etl | ForEach-Object { "$($_.FullName) $($_.Length) B" }
Get-Content C:\S10eTmp\d3dswap.log | Where-Object { $_ -notlike 'malikm*' } | Select-Object -Last 3
