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
- 00:08 run h2a started: extra.cmd = extra-gpu-h2.cmd (md5 62b987b5..), gpu\d3dtri.exe (da1c773c..), mode stays GpuEnable 1 / FullAdapter 1 / machine GALLIUM_DRIVER=softpipe, no nogpu.flg.
- H4 note: present path = 2 CPU copies per frame of 1080x2280x4 (shim present_copy: Panfrost render target -> primary allocation, then KMD DoPresent blt -> GOP fb 0xCA000000); with Panfrost the source is a GPU BO read by the CPU (uncached/WC mapping, CacheCoherent FALSE segment) - expected bottleneck for DWM fps; zero-copy scanout (DECON base address flip) is the H4 target.
- **H2 run h2a (00:08-00:16, runs/h2a, cam/h2a):** session 0: d3dtri tri softpipe PASS, **tri panfrost PASS**
  (pixels identical: red 1352, blue 2744, other 0), KMD `jobs done` 0 -> 9 (8 Panfrost jobs + m5test's), no fault;
  shim "bind to the runtime device ... yes, through its callbacks", "VERSION panfrost 1.1" = first Panfrost frame
  rendered by the S10e's Mali. User session: tri PASS (12 more jobs); **cube hung in its first frame** (d3dtri
  watchdog "TIMEOUT in frames") with **one fault**: `jobs done 21 failed 1 ... js 0x8 fault 0x240001c3 @0x40b8000
  resets 1` = exception 0xC3 TRANSLATION_FAULT, access type 1 = EXECUTE -> the GPU fetched shader code from an
  unmapped VA. KMD stats: 79 allocations, only 44 aperture maps.
- Cause (Mesa pan_jm.c:105-150 read): Panfrost's submit lists batch->bos + the batch's two pools + tiler heap +
  sample positions only - Linux keeps every BO mapped; WDDM 1.x VidMm maps an allocation only while a submission
  lists it (and an unlisted one may be unmapped, its pages may move -> keeping PTEs in the KMD is unsafe).
- Fix A, Mesa 0040 (tools/mesa-build/patches/0040-h2-shim-lists-every-live-bo-per-submit.patch): SUBMIT lists the
  batch BOs (WriteOperation 1) then every other live BO (read), deduped by a per-submit mark; overflow of the
  runtime's list (256 slots, KMD MW_LIST_SIZE) is reported once on stderr. Built mali-umd-0040:
  libgallium_d3d10.dll = maliumd.dll 9c70c544a3dd0257..., ARM64. First build failed on my own edit (a "\n" in a
  string became a real line break through the python heredoc) - fixed before the commit.
- 00:21 run h2b started: UMD 0040 (md5 14a50732..) in gpu\umd-new + gpu\, extra.cmd = extra-gpu-h2.cmd with the UMD swap (md5 6f12642b..).
- **Run h2b (00:21-00:30, runs/h2b, cam/h2b) with UMD 0040:** swap worked (System32 maliumd.dll sha256 9c70c544 =
  0040). **No fault any more** (`failed 0 ... fault 0x0 resets 0` through the whole run) - 0040 fixed the
  residency fault. Session-0 tri panfrost + user tri still PASS. **Cube still hangs in its first frame**, and
  submits NO job (KMD jobs done unchanged across the cube) -> a CPU-side hang inside the first frame's D3D calls
  (UMD/shim), not a GPU hang.
- Next (h2c): extra-gpu-h2.cmd runs the cube as its own process; still alive at 30 s -> livedump.ps1 minidump
  (stacks) of d3dtri from the user session -> cdb on the PC with libgallium_d3d10.pdb (mali-umd-0040).
- 00:31 run h2c started (extra.cmd md5 a0780e8b..).
- **Run h2c (00:31-00:39, runs/h2c):** cube alive at 30 s -> minidump d3dtri-2736.dmp. cdb + mali-umd-0040 PDB:
  main thread in ucrtbase!wassert -> MessageBoxW (assert dialog), from maliumd!panfrost_emit_const_buf
  (pan_cmdstream.c:1639 `assert(!cb->buffer && cb->user_buffer && "only user buffers use this path")`) <-
  panfrost_update_shader_state <- panfrost_draw_vbo <- d3d10umd DrawIndexed. Cause: pan_shader.c:201 makes UBO 0
  pushable because GL binds it as a CPU user buffer; d3d10umd binds cbuffer b0 as a real resource. Every D3D shader
  with a constant buffer hits it (DWM's too). Not a GPU problem, not a deadlock: the "hang" is the modal dialog.
- Mesa 0041: pushable_ubos = sysval UBO only (D3D constants read with UBO loads). Mesa 0042: OpenAdapterCommon sets
  _set_error_mode(_OUT_TO_STDERR) + _set_abort_behavior(0, _WRITE_ABORT_MSG) once per process - a UMD assert now
  prints and aborts (DWM restarts) instead of hanging in a dialog. Build mali-umd-0042 (0040+0041+0042):
  maliumd.dll 508abc96acb585d0..., ARM64.
- 00:43 run h2d started (UMD 0042, md5 28679f11..).
- **H2 PASS (run h2d, 00:43-00:51, runs/h2d, UMD 0042):** session-0 + user tri PASS on Panfrost; **user cube on
  the Mali: 733 frames / 20 s = 36.6 fps (33-47 per second), 0 present failures, first frame centre 0xff7f017f =
  the x64 WARP reference pixel**; KMD after the run `jobs done 4420 failed 0 timedout 0 fault 0x0 irqs 4414
  resets 0`. No assert, no minidump needed. (Webcam not aimed at the phone - visual check missing; the readback
  is the proof.) DWM still softpipe (machine GALLIUM_DRIVER).
- Next: H3 = DWM on Panfrost. GALLIUM_DRIVER is read at process start, so the switch needs one boot to remove the
  machine variable (extra-gpu-h3.cmd, by \S10EDIAG\gpu\softpipe.flg) and the next boot to measure.
- 00:53 run h3a (boot A) started: extra.cmd = extra-gpu-h3.cmd, gpu\softpipe.flg REMOVED -> this boot DWM still softpipe (pin from h1a pass 1), removes the pin for boot B; d3dtri rebuilt build (56a74116..).
- **Run h3a = boot A (00:53-01:01, runs/h3a, cam/h3a):** DWM softpipe (machine GALLIUM_DRIVER=softpipe; DWM loaded
  maliumd.dll + D3D10Warp.dll), GPU half up. gfxperf: DWM 0.7 fps median (0.5-0.9), CPU mean 65 %, all cores ~100 %
  during the cube; cube on Panfrost **37.3 fps, 0 present failures**; KMD `jobs done 4482 failed 0 fault 0x0`.
  d3dtri itself ~90 % of a core at 37 fps (CPU present copies, H4). Webcam (user: camera IS on the phone): logon +
  desktop visible 00:55-00:58; from 00:58:22 the panel is dark = the cube's fullscreen window (clear colour
  0.05/0.05/0.15 is near-black on camera) - next d3dtri build clears brighter. Pin removed for boot B.
- **Run h3b = boot B (01:02-01:12, runs/h3b):** machine GALLIUM_DRIVER gone -> DWM on Panfrost (maliumd.dll loaded).
  **Logon never completed** (agent: LogonUI up, no explorer.exe for 300 s), DWM restarted (2 dwm processes,
  ~0-1.6 s CPU), KMD 0 jobs from DWM. Application log: dwm.exe and LogonUI.exe crash 0xc0000409 in ucrtbase
  +0x76d8c = Mesa 0042's assert->abort. WER LocalDumps (armed by the h1a harness) left LogonUI.exe.4352.dmp (custom,
  139 MB) on C:\S10eTmp\dumps; cdb: ucrtbase!wassert <- maliumd!panfrost_set_viewport_states
  (pan_context.c:497 `assert(num_viewports == 1)`) <- d3d10umd SetViewports (Rasterizer.cpp:87) <- d3d11
  RSSetViewports <- d2d1 CD3DRenderState::ResetState. d3d10umd passes NumViewports + ClearViewports (up to 16);
  Panfrost has one viewport and one scissor (same assert in set_scissor_states:511). Other Panfrost setters'
  limits (16 samplers, 128 sampler views) match what d3d10umd passes.
- Mesa 0043: SetViewports/SetScissorRects pass MIN2(count, screen caps.max_viewports), nothing when 0. Build
  mali-umd-0043 maliumd.dll 3576b65f.... Deployed by **TWRP ntfs-3g rw** (clean NTFS after boot B's shutdown;
  no fsck) into System32 maliumd.dll + libgallium_d3d10.dll (sha256 verified on the device), so DWM loads it at
  the next boot (the in-Windows swap only reaches new processes). gpu\umd-new removed from the ESP; d3dtri-2
  (bright clear 0xff4073d9) on the ESP.
