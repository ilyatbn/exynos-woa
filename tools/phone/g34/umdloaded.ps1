# umdloaded.ps1 - run on the phone: last boot time, which maliumd* modules dwm / explorer / any process have loaded,
# and the hash prefix of each installed UMD file. notes/g34-fl11.md (FL 11_0 default rollout).
"boot " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
foreach ($p in Get-Process -ErrorAction SilentlyContinue) {
    try { $m = $p.Modules | Where-Object { $_.ModuleName -like 'maliumd*' } | ForEach-Object { $_.ModuleName } } catch { continue }
    if ($m) { "{0,-20} {1,6} {2}" -f $p.ProcessName, $p.Id, ($m -join ',') }
}
foreach ($f in 'C:\Windows\System32\maliumd_arm64.dll', 'C:\Windows\System32\maliumd_ec.dll', 'C:\Windows\SysWOW64\maliumd32.dll') {
    $f + ' ' + (Get-FileHash $f).Hash.Substring(0, 8).ToLower()
}
