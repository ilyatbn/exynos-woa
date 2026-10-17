# actestu.ps1 [-Dev] - run actest.ps1 in the logged-on user's session (runuser2.ps1) and print its output
# (G17 V6d, notes/g17-dxva-mfc.md). Run actest.ps1 -Setup once before (administrator).
param([switch]$Dev)
$out = 'C:\S10eTmp\ac\out.txt'
Remove-Item $out -ErrorAction SilentlyContinue
$d = ''; if ($Dev) { $d = ' -Dev' }
& C:\S10eTmp\runuser2.ps1 -Cmd "powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\mfc\actest.ps1$d > $out 2>&1" -Wait 120
Get-Content $out -ErrorAction SilentlyContinue
