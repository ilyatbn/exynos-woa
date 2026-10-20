# (user session, via run-as-user.ps1) edgecdp.ps1 - start Edge for the DevTools reads of cdp_webgl.py / cdp_gpuinfo.py:
# --remote-debugging-port 9222 with a separate --user-data-dir (Edge refuses remote debugging on the default profile,
# notes/gpu-umd-d3d11.md). Tracing only (AGENTS rule 12) - never a fix. -Stop (edgecdpstop.ps1 through run-as-user) closes that instance again.
param([switch]$Stop, [string]$Extra = '')   # -Extra: more diagnosis switches, comma-separated, no spaces (G35: --no-sandbox,--enable-logging)
$prof = 'C:\S10eTmp\edgeprof'
Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -match [regex]::Escape($prof) } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
if ($Stop) { return }
$args2 = @('--remote-debugging-port=9222', "--user-data-dir=$prof", '--no-first-run') + @($Extra -split ',' | Where-Object { $_ }) + @('about:blank')
Start-Process -FilePath msedge -ArgumentList $args2
