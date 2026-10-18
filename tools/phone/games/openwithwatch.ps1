# (SSH, SYSTEM) openwithwatch.ps1 [-Seconds 60] - log the command line of every OpenWith.exe / browser process that
# starts while a game runs (NFSU2: "an annoying link pops up and minimizes the game" -> the focus loss crashes it).
# -> C:\S10eTmp\openwith.out. notes/g25-geometry-shaders.md.
param([int]$Seconds = 60)
$out = 'C:\S10eTmp\openwith.out'
"watch $(Get-Date -Format HH:mm:ss)" | Set-Content $out
$seen = @{}
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
  Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(OpenWith|msedge|brave|explorer|rundll32|iexplore)\.exe$' -and -not $seen[$_.ProcessId] } |
    ForEach-Object { $seen[$_.ProcessId] = 1; "$(Get-Date -Format HH:mm:ss) $($_.Name) pid $($_.ProcessId) parent $($_.ParentProcessId): $($_.CommandLine)" | Add-Content $out }
  Start-Sleep -Milliseconds 500
}
