# (user session, via run-as-user.ps1) browsercheck.ps1 - G24/G26 follow-up: does a browser still run on the ARM64 UMD
# (CS 4.x advertised, BC -> ETC2)? Opens a WebGL page in a new Edge window, waits, lists Edge's GPU processes and
# their start times (a GPU-process crash restarts it), then opens edge://gpu (typeurl.ps1) for a screenshot
# (shot.ps1). -> C:\S10eTmp\browsercheck.out. notes/g26-bc-etc2.md.
param([string]$Url = 'https://get.webgl.org/', [int]$Wait = 20)
$out = 'C:\S10eTmp\browsercheck.out'
"start $(Get-Date -Format HH:mm:ss)" | Set-Content $out
Start-Process -FilePath msedge -ArgumentList '--new-window', $Url
Start-Sleep -Seconds $Wait
Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -match '--type=gpu-process' } |
  ForEach-Object { "gpu-process pid $($_.ProcessId) started $($_.CreationDate)" } | Add-Content $out
& C:\S10eTmp\shot.ps1 -Out C:\S10eTmp\browser-webgl.png
& C:\S10eTmp\typeurl.ps1 -Url 'edge://gpu' -Title 'Edge'
Start-Sleep -Seconds 6
& C:\S10eTmp\shot.ps1 -Out C:\S10eTmp\browser-gpu.png
"end $(Get-Date -Format HH:mm:ss)" | Add-Content $out
