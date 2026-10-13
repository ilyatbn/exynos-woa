# Runs in the logged-on user's session (run-as-user.ps1, from extra-gfxperf.cmd). notes/gpu-full-adapter-hang.md.
# gfxperf samples DWM's composition rate, per-CPU load + clock, GPU engine use and the top processes once a second
# for 150 s. With C:\S10eTmp\gfxbench.flg (copied from the ESP's gpu\bench.flg) d3dfill draws red/green/blue/white
# full-window for 3 s each starting at t=60 s, so the log shows the desktop idle, under the user, and under a
# steady D3D11 present loop.
$p = Start-Process -FilePath C:\S10eTmp\gfxperf.exe -ArgumentList '150' -RedirectStandardOutput C:\S10eTmp\gfxperf.txt -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 60
if (Test-Path C:\S10eTmp\gfxbench.flg) {
    & C:\S10eTmp\d3dfill.exe 3 > C:\S10eTmp\d3dfill-bench.txt 2>&1
}
$p.WaitForExit()
