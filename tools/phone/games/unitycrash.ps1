# unitycrash.ps1 [-Company nexon] [-Product "DAVE THE DIVER"] - copy the newest Unity crash report (crash.dmp +
# Player.log from %TEMP%\<company>\<product>\Crashes\Crash_*) of the logged-on user to C:\S10eTmp\unitycrash\, for
# scp -O to the PC (symbolise our UMD frames with the build's PDB). notes/games-steam.md.
param([string]$Company = 'nexon', [string]$Product = 'DAVE THE DIVER')
$o = Get-CimInstance Win32_Process -Filter "name='explorer.exe'" | Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner
$root = "C:\Users\$($o.User)\AppData\Local\Temp\$Company\$Product\Crashes"
$d = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $d) { "no crash report under $root"; exit 1 }
$out = 'C:\S10eTmp\unitycrash'
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -File | Remove-Item -Force
Copy-Item (Join-Path $d.FullName '*') $out
"$($d.Name):"; Get-ChildItem $out | ForEach-Object { "  {0} {1} B" -f $_.Name, $_.Length }
