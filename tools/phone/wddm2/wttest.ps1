# wttest.ps1 [-Wait 30] - S4 (notes/gpu-wddm2-design.md): start Windows Terminal in the logged-on user's session and
# report whether it is still alive after -Wait seconds, plus any new crash events / dumps. On the WDDM 1.3 adapter it
# died in d3d11!NDXGI::CDevice::WaitTrackedFenceImpl (NULL tracked fence) - WDDM 2.0 monitored fences should fix it.
param([int]$Wait = 30)
$t0 = Get-Date
Get-Process WindowsTerminal -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
& C:\S10eTmp\runuser2.ps1 -Cmd 'start "" wt.exe' -Wait 15 | Out-Null
Start-Sleep $Wait
$p = Get-Process WindowsTerminal -ErrorAction SilentlyContinue
if ($p) { "WindowsTerminal ALIVE pid $($p.Id) after $Wait s, cpu $([math]::Round($p.CPU,1)) s, mainwindow '$($p.MainWindowTitle)'" } else { "WindowsTerminal NOT running after $Wait s" }
Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000; StartTime=$t0} -ErrorAction SilentlyContinue | ForEach-Object { $m = $_.Message -split "`n"; "{0:T} {1} | {2} | {3}" -f $_.TimeCreated, $m[0].Trim(), $m[1].Trim(), $m[3].Trim() }
Get-ChildItem C:\S10eTmp\dumps -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0 } | ForEach-Object { "new dump " + $_.Name }
