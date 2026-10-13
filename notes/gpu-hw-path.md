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
