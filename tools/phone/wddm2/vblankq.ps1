# vblankq.ps1 [-N 240] - G17 pacing (notes/g17-dxva-mfc.md): drivers/MaliG76Wddm/tools/vblank in the user's session,
# once at normal priority and once with the waiting thread TIME_CRITICAL (wake-up latency vs the vsync source).
param([int]$N = 240)
foreach ($mode in '', 'rt') {
  "--- priority $(if ($mode) { 'time critical' } else { 'normal' })"
  & C:\S10eTmp\userrun.ps1 -Exe C:\S10eTmp\vblank.exe -Args "$N $mode" -Wait 60
}
