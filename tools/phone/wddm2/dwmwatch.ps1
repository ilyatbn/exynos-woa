# dwmwatch.ps1 [-Frames 300] [-Cfg "comp seq 2"] [-NoRedirect] - start d3dswap in the user session and sample the DWM
# PID every second while it runs and for 15 s after: does DWM survive while a redirected composition swap chain is
# live, and does it fall over when the app exits? (G15 S4, behavioural test - our own tools only.)
param([int]$Frames = 300, [string]$Cfg = 'comp seq 2', [switch]$NoRedirect)
$pre = if ($NoRedirect) { 'set D3D10UMD_NO_REDIRECT=1&& ' } else { '' }
Remove-Item C:\S10eTmp\runuser2.done -ErrorAction SilentlyContinue
$job = Start-Job -ScriptBlock { param($c) & C:\S10eTmp\runuser2.ps1 -Cmd $c -Wait 120 } -ArgumentList "${pre}C:\S10eTmp\d3dswap.exe $Frames $Cfg > C:\S10eTmp\d3dswap.log 2>&1"
$t0 = Get-Date
for ($i = 0; $i -lt 90; $i++) {
  $app = Get-Process d3dswap -ErrorAction SilentlyContinue
  $dwm = (Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ','
  $last = (Get-Content C:\S10eTmp\d3dswap.log -ErrorAction SilentlyContinue | Where-Object { $_ -notlike 'malikm*' } | Select-Object -Last 1)
  "{0,4:n0}s app {1,-5} dwm {2,-6} {3}" -f ((Get-Date) - $t0).TotalSeconds, $(if ($app) { 'up' } else { 'gone' }), $dwm, $last
  if (-not $app -and $i -gt 5) { $gone = if ($gone) { $gone + 1 } else { 1 }; if ($gone -gt 15) { break } }
  Start-Sleep 1
}
Remove-Job $job -Force -ErrorAction SilentlyContinue
