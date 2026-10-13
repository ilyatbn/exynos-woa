# M16 hardware GPU path - from softpipe to the Mali actually rendering (2026-10-13 ->)

Goal (user, 2026-10-13 ~23:50): "have the gpu working smoothly in windows. it should not crash. it should be smooth
enough to run basic 3d workloads (and windows)." Unattended; the PC webcam faces the phone (tools/phone/camshot.ps1,
camwatch.sh).

Start state: boot #115 (358e52bd..), MaliG76Wddm 1.0.34.1, UMD mali-umd-0039 (softpipe in maliumd.dll), mode in the
registry FullAdapter 1 GpuEnable 0; full mode composes the desktop on the CPU (DWM 0.4-0.7 fps, notes/
gpu-full-adapter-hang.md "User test boots 5-8"). ESP: nogpu.flg (interactive fast desktop), stay.flg, gpu\full.flg,
gpu\bench.flg, extra.cmd = extra-wifi-gfx.cmd, extra.cmd.off = extra-gpu-mode.cmd (the harness).

## Plan (each step = its own task + commit; nothing Ready without device validation)
H1. G11 on device: GpuEnable 1 with DWM pinned to softpipe (system env GALLIUM_DRIVER=softpipe - patch 0020 keeps
    the env override), m5test WRITE_VALUE through D3DKMT -> "PASS: the Mali ran a job submitted through WDDM",
    irqs > 0, no fault, desktop still up. Never run before (G11 built 2026-10-12).
H2. Panfrost in one process: d3dfill / d3dprobe with GALLIUM_DRIVER=panfrost (clear colours, then a triangle) while
    DWM stays on softpipe. Expect the libpan/Bifrost compiler, BO/MMU and job-chain bugs here.
H3. DWM on Panfrost (drop the env pin): desktop composed by the Mali; gfxperf DWM fps vs softpipe.
H4. Smooth: clocks (G3D 260 MHz start OPP, no DVFS), present path cost (CPU blits), job throughput, cache
    maintenance; measured with gfxperf each step.
H5. No crashes: TDR/fault recovery, long runs (unattended loop), basic 3D workload (spinning cube) for minutes.
Side items (GPU-mode bugs seen by the user): desktop wider than the panel in full mode (tray off-screen, photo
2026-10-13 23:40); clean restart hang in full mode; LogonUI combase E_INVALIDARG crashes.

## Progress log
- 2026-10-13 23:55 - state check: G11 (GpuEnable 1) never ran on the device (gpu-m16-g10.md "Validation procedure";
  gpu-full-adapter-hang.md: every run since was mode 1 = FullAdapter, GPU off). maligpu.c bring-up re-read: same
  order as the G9b probe that PASSED (soft reset, SHADER_CONFIG TLS hash, L2/shader/tiler PWRON, AS0 LPAE, JS IRQ
  mask) - not changed before the first run. Mesa 0020: Panfrost becomes the default when GPU_INFO Valid, but
  GALLIUM_DRIVER still overrides (d3d10_gdi.c debug_get_option).
- H1 prep: extra-gpu-mode.cmd pass 1 pins GALLIUM_DRIVER=softpipe machine-wide when \S10EDIAG\gpu\softpipe.flg exists
  (deleted otherwise), so DWM stays on softpipe while m5test uses the GPU through D3DKMT.
- 23:52 run h1a started: ESP extra.cmd = extra-gpu-mode.cmd (md5 7893be20..), flags gpu\{full,gpu,softpipe,bench}.flg + nogpu.flg + noaudio.flg, no stay.flg; battery 100 %; adb reboot from TWRP. Expect: boot A GPU0 hidden -> pass 1 writes GpuEnable 1 FullAdapter 1 + GALLIUM_DRIVER=softpipe, deletes nogpu.flg, clean restart -> boot B GPU half up -> pass 2 (d3dprobe, d3dfill, dxdiag, m5test) -> TWRP.
- 23:55 h1a boot 1 went straight to TWRP at 23:54: UEFILOG 'previous boot ran Windows - saving its PRAM, then recovery' (WINRUN.FLG from the user's last hard-reset Windows boot, no stay.flg = unattended pair). Expected; WINRUN.FLG gone -> adb reboot again.
- **H1 PASS (run h1a, 2026-10-13 23:55 -> 2026-10-14 00:06, s10e-artifacts/runs/h1a, 37 webcam shots cam/h1a).**
  Boot 1 = pass 1 (GPU0 hidden, mode written), boot 2 = GpuEnable 1 FullAdapter 1. trace.reg: Mode 2, Step 6
  (READY), GpuUp 1, GpuId 0x72110000, PaBits 0x28 (40), ShaderPresent 0x770077 (12 cores), L2Present 1, JsPresent 7,
  CoherencyFeatures 0. gpu\m5test.txt: "PASS: the Mali ran a job submitted through WDDM" - target 0xc0ffee52,
  job header status 0x1, stats `jobs done 1 failed 0 timedout 0 js 0x0 fault 0x0 irqs 1 resets 0`, exit 0 ->
  the job IRQ is delivered (not only the 1 ms poll). DWM loaded maliumd.dll (softpipe pinned), d3dfill presented
  every phase (white 129 frames / 8 s). Then clean return to TWRP (00:06:39). = G11 device-validated (1 boot).
