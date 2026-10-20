# killruns.ps1 (PC side) - kill leftover Dave run scripts (botbench.sh, loadrun.sh, loadspikes.sh and their ssh /
# sleep children): TaskStop of a background Bash task leaves its children running, and a leftover botbench.sh kills
# the game / restarts the phone bot in the middle of the next run (notes/games-unity.md G33). Run it as its own command
# (its command line must not contain those script names, or it would match the caller).
$p = Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'botbench\.sh|loadrun\.sh|loadspikes\.sh|loadprof\.ps1' -and $_.Name -match '^(bash|ssh|sleep)\.exe$' }
$p | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
"killed $(@($p).Count)"
