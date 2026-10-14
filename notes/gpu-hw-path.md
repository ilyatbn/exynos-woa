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
- 01:25 run h3c started: DWM on Panfrost with UMD 0043 (no pin).
- **Run h3c (01:25-01:35, runs/h3c, UMD 0043):** DWM/LogonUI got past the viewport assert - KMD counted 66 GPU jobs
  from them before the tests (0 in h3b), no fault - then still crashed 0xc0000409 (logon never completed).
  LogonUI.exe.4300.dmp: wassert <- maliumd!ttn_src_for_file_and_index (tgsi_to_nir.c:748 UNREACHABLE "bad src
  file") <- ttn_get_src <- ttn_emit_instruction <- tgsi_to_nir <- panfrost_create_shader_state <- d3d10umd
  CreatePixelShader. ttn_get_src skips only NULL/SAMPLER/IMAGE/BUFFER operands; d3d10umd's D3D10 sampling
  opcodes (SAMPLE, SAMPLE_B/C/C_LZ/D/L, SAMPLE_I, SVIEWINFO, ShaderTGSI.c:1448-1776) carry a SAMPLER_VIEW operand,
  and tgsi_to_nir implements none of them (only GL TEX/TXB/TXL/TXD/TXF/TG4/LODQ/TXQ with combined samplers). Any
  D3D shader that samples a texture died here.
- Mesa 0044 (tgsi_to_nir): ttn_sample + ttn_sviewinfo, semantics copied from softpipe tgsi_exec (exec_sample,
  exec_sample_d, exec_txf, exec_txq): src0 coord, src1 SVIEW (target from its DECL, result re-swizzled by the
  SVIEW operand's swizzle), src2 SAMP as a SEPARATE sampler variable (texture_deref binding = SVIEW, sampler_deref
  binding = SAMP; nir_lower_samplers lowers each from its own var.binding), src3.x compare/bias/lod, src3/src4
  ddx/ddy, SAMPLE_C_LZ lod 0, SAMPLE_I integer coords + mip in src0.w (no lod for buffers), SVIEWINFO txs +
  query_levels (1 for buffer/MSAA). SAMPLER_VIEW operands skipped in ttn_get_src. First build failed
  (ttn_swizzle is a token-pasting macro) -> nir_swizzle with an array. Build mali-umd-0044 maliumd.dll
  4f78b651..., copied to System32 from TWRP (sha256 verified).
- d3dtri `tex` mode (2x2 texture via Sample / Load / GetDimensions, quadrant pixel check) PASS on WARP x64;
  ARM64 d3dtri-3 on the ESP; extra-gpu-h3.cmd runs it in session 0 on Panfrost first.
- 01:42 run h3d started: DWM on Panfrost, UMD 0044.
- **Run h3d (01:42-01:52, runs/h3d, UMD 0044):** d3dtri tex on Panfrost: "Unhandled intrinsic
  load_vertex_id_zero_base / Assertion failed bifrost_compile.c:2233" (0042 now prints asserts) - SV_VertexID is
  declared VERTEXID_NOBASE by d3d10umd and tgsi_to_nir emits load_vertex_id_zero_base, which Panfrost does not
  implement (it lowers only load_vertex_id, pan_nir_lower_vertex_id.c). DWM + LogonUI still 0xc0000409; WER dumps:
  dwm.exe.4016.dmp = PS: tgsi_to_nir.c:656 (0044 numbering) UNREACHABLE("bad system value"); LogonUI.exe.4088.dmp =
  VS: tgsi_to_nir.c:571 assert(!indirect) on a TEMPORARY without an array var. KMD: 86 jobs from LogonUI/DWM
  before the tests, no fault.
- d3d10umd sources: indexable temps (DCL_INDEXABLE_TEMP) are declared as plain ureg temporaries ("XXX: Add true
  indexable temps to gallium"), relative access = indirect plain temp (fine for tgsi_exec, not for ttn); system
  values PRIMID/VIEWPORT_INDEX/LAYER/CLIPDIST are declared with ureg_DECL_system_value; ttn's sysval switch lacks
  PRIMID/LAYER/VIEWPORT_INDEX. Also found: ttn keeps ONE addr_reg (dst asserts index == 0) while our 0036 uses
  ADDR[0..2] round-robin -> silent aliasing of two relative operands in one instruction.
- Mesa 0045: VERTEXID_NOBASE -> vertex_id - base_vertex when !vertex_id_zero_based; Panfrost ctx->base_vertex =
  draw->start for non-indexed draws (only feeds the sysval; = draw_vs_exec.c basevertex) so the value equals
  softpipe's (indexed: index; non-indexed: i). Build mali-umd-0045 3c04ec3d.... Mesa 0046: ttn addr_regs[4] (decl
  per register, src/dst by index); sysvals PRIMID -> load_primitive_id, LAYER -> load_layer_id (both in Bifrost),
  VIEWPORT_INDEX -> 0; d3d10umd DCL_INDEXABLE_TEMP -> ureg_DECL_array_temporary + ureg_dst_array_offset. Build
  mali-umd-0046 (0040-0046) maliumd.dll dafba629....
- 01:57 run h3e started: UMD 0046 via umd-deploy.sh.
- **Run h3e (01:57-02:07, runs/h3e, UMD 0046):** tex VS now compiles (vertex id fixed); PS: "Assertion failed:
  !c->cap_position_is_sysval && c->input_var_position, tgsi_to_nir.c:679" - d3d10umd declares SV_Position (and
  FACE/PCOORD) as fragment INPUTS, Panfrost has fs_position_is_sysval; the decl path already creates a
  SYSTEM_VALUE_FRAG_COORD variable then, only the read path asserted. DWM 0xc0000409 again (same family).
- Static audit (all ureg_*/TGSI_OPCODE_* d3d10umd emits vs ttn op_trans + switch): untranslated = EMIT/ENDPRIM (GS,
  Panfrost has none), BGNSUB/CAL/ENDSUB (subroutines), SWITCH/CASE/DEFAULT/ENDSWITCH (D3D switch) - not done yet,
  wait for a dump that needs them. Register files: indirect IMMEDIATE (D3D icb[r]) asserted in ttn; d3d10umd bug:
  icb[imm + r] dropped the imm part (indexed from sx->imms).
- Mesa 0047: ttn input FACE/POSITION/PCOORD accepted on sysval screens (load the var; FACE in GL float form).
  Mesa 0048: ttn indirect IMMEDIATE through an "imm_arr" shader_temp array stored after the last immediate;
  d3d10umd icb index keeps the immediate part. (First 0048 build: Mesa 26 has nir_def_instr(), no ->parent_instr.)
  Build mali-umd-0048 (0040-0048) maliumd.dll 9da69cbf....
- 02:11 run h3f started: UMD 0048.
- **Run h3f (02:11-02:20, runs/h3f, UMD 0048):** **DWM no longer crashes** (Application log: only LogonUI), KMD 242
  jobs from logon/desktop processes before the tests, no fault; logon still not completed (no explorer at 300 s).
  LogonUI.exe.2932.dmp: wassert <- panfrost_resource_from_handle (pan_resource.c:373 `assert(whandle->type ==
  WINSYS_HANDLE_TYPE_FD)`) <- d3d10umd CreateResource (0037's MISC_SHARED path: CPU-memory SHARED handle, a
  softpipe-only import) <- d3d11 CreateCompositionBuffer <- dcomp CAtlasSurfacePool. d3dtri tex: access violation
  0xC0000005 at "draw" (no dump: no LocalDumps key for d3dtri). Panfrost import: pan_kmod_bo_import ->
  drmPrimeFDToHandle (shim: ENOSYS stub) + lseek(fd, SEEK_END) for the size; VA from GET_BO_OFFSET.
- Mesa 0049: shim wbo entries can be runtime_owned (shared/opened allocations: GEM_CLOSE forgets, never destroys);
  wbo_add() factored out of CREATE_BO; malikm_shared_bo_create (allocation with the runtime resource handle, not
  locked) and malikm_bo_from_allocation (opened allocation) register the allocation at its KMD GPU VA
  (ALLOC_INFO escape) and return pseudo fd 0x40000000|handle; drmPrimeFDToHandle decodes it;
  malikm_prime_fd_size replaces lseek in pan_kmod_bo_import (_WIN32). d3d10umd CreateResource/OpenResource:
  non-softpipe screens import the BO (WINSYS_HANDLE_TYPE_FD, linear, pitch) instead of CPU memory. Build
  mali-umd-0049 3314f7e3.... extra-gpu-h3.cmd: WER LocalDumps for d3dtri.exe.
- 02:25 run h3g started: UMD 0049.
- **Run h3g (02:25-02:33, runs/h3g, UMD 0049): LOGON COMPLETES WITH DWM ON THE MALI.** explorer.exe at 37 s,
  desktop up; DWM loaded maliumd.dll (no softpipe pin); KMD 896 jobs from logon/DWM before the tests, 5348 after,
  `failed 0 fault 0x0 resets 0`. Webcam 02:28-02:30: wallpaper + desktop + taskbar on the panel (composed by
  Panfrost), plus a dark window with a title bar top-left (likely a console of the harness). gfxperf: idle desktop
  CPU 26-46 % (softpipe baseline 60-99 %), dwm not in the top 4 at idle; cube on Panfrost 36.1 fps. LogonUI still
  crashes repeatedly (crash events) but the logon goes through; tex still 0xC0000005 (no dump: key armed by this
  run, next run has it).
- **The H4 problem:** DWM composition is still ~0.9 frames/s (gfxperf) and the KMD saw only 23 presents in the
  ~3-minute measurement (presents 128 -> 151) while d3dtri rendered 722 frames: DWM presents very rarely. dwm.exe
  100-117 % CPU for ~3 s at the cube start, then idle. Per-frame present path: pipe_texture_map(READ) of the
  Panfrost render target (GPU wait + possibly de-tiling blit + wait) -> per-pixel swizzle copy out of an uncached
  CPU mapping -> present_copy memcpy into the present allocation -> KMD CPU blt to the GOP fb. Suspect a wait
  timing out (wddm_idle: 10 s WaitForSingleObject on the EnqueueCpuEvent completion event).
- Mesa 0050: per-process timing log (MALIKM_TGSI_DIR, machine-wide = C:\S10eTmp\tgsi): idle waits >= 20 ms (event
  or poll, result), frontbuffer map/convert ms, present copy lock/memcpy/unlock ms, every 500th submit. Build
  mali-umd-0050 135bc8d4.... extra-gpu-h3.cmd collects tlog\malikm-*.txt.
- 02:36 run h3h started: UMD 0050 (timing log).
- **Run h3h (02:36-02:45, runs/h3h, UMD 0050) - timing logs (h3/tlog):** logon again OK (explorer at 44 s), 0
  faults. dwm.exe: 91 presented frames in ~175 s; per frame map 0-16 ms, **convert 47-94 ms** (per-pixel copy out
  of the uncached Panfrost mapping), present memcpy 0-16 ms; only ONE idle wait >= 20 ms (32 ms) -> the 10 s
  event-timeout theory is wrong. Frame gaps: min 250, median 437 ms when busy (2-4 fps); 3-31 s gaps = idle.
  d3dtri cube: 646 frames (frontbuffer 540x1140 convert 31 ms each), but **DWM drew no frame during the cube**
  (DWM frames at 151.5 / 165.6 / 171.3 s only) -> windowed presents of an app do not reach the composition.
  gfxperf: idle CPU mean 78 % this boot (Windows Update / Defender busy). LogonUI keeps crashing in the
  background (not blocking logon); tex 0xC0000005 (dump pending).
- **H4a plan (zero-copy present):** the panel scans the GOP fb (0xCA000000, 1080x2280 BGRX, pitch 4320). Map its
  pages into the GPU MMU (KMD, fixed GPU VA), expose it by a new escape; the UMD imports it as a linear Panfrost
  resource and DWM's present becomes a GPU copy of its back buffer into it (no CPU map/convert/memcpy); the KMD
  skips its CPU blt for primaries marked "GPU-presented". Windowed apps keep the CPU path (only the compositor may
  write scanout).
- KMD 1.0.35.1 built (s10e-artifacts\mali-kmd-35, not deployed yet): MwMapScanout maps the GOP fb (A->Fb.PhysicAddress,
  FbSize) NoExec at a MkVaAlloc VA after GpuUp; MW_ESCAPE_SCANOUT (4) returns VA/size/pitch/format and marks an
  allocation GpuPresented; DoPresent skips the CPU blt/latch for a GpuPresented source to scanout (PresentGpu,
  STATS' former Reserved0). Loaded .sys path (hive): DriverStore\FileRepository\malig76wddm.inf_arm64_6557c1d415c2ef25.
- **Obstacle for the zero-copy present:** Panfrost v7 requires 64-byte-aligned linear row pitch
  (pan_linear_or_tiled_row_align_req = 64 except some YUV; strict WSI import fails "WSI pitch not properly
  aligned"); the GOP fb pitch is 4320 = 67.5 x 64. Options: (a) DECON/DPP source width 1088 px -> pitch 4352
  (needs verified DPP/IDMA registers - no Samsung DPU source locally), (b) a custom 2-rows-per-line copy pass
  (2160x1140 view, pitch 8640 = 135 x 64) inside Panfrost, (c) GPU blit to an aligned staging + CPU row copy.
  Decision deferred until the DWM profile says how much of the frame the copies are.
- Next run h3i: Mesa 0051 (event log: shared create/open path + every present's HRESULT, build mali-umd-0051
  32d9674c...), d3dtri gdi mode (DWM load without app D3D), dwm-sampler.ps1 (8 stack dumps of dwm 1 s apart during
  the gdi phase).
- 02:53 run h3i started (UMD 0051, d3dtri-4, sampler).
- **Run h3i (02:53-03:01, runs/h3i, UMD 0051) - the profile:** logon OK, 0 faults. Event log: OpenResource of shared
  surfaces works (Panfrost BOs, e.g. 1088x2304 pitch 4352, 832x1024, 576x576); all presents hr 0 (DWM "present
  primary", d3dtri "present blt surface"). DWM frames/10 s: logon 11-29, cube (151-171 s) 4/10 s although d3dtri
  presented 643 frames, gdi animation (180-205 s, 519 paints = ~20/s) 15-19/10 s = **DWM ~1.5-2 fps under load**.
  dwm-sampler (8 stack dumps 1 s apart, DWM 0.5-0.65 s CPU per s): DWM Compositor Thread = 2x idle (WaitForWork),
  **5x in maliumd!_RotateResourceIdentities -> util_resource_copy_region -> util_copy_box -> memcpy** (dxgi
  CDXGISwapChain::PresentImplCore after DWM's PresentDWM), 1x in d3d10_pan_flush_frontbuffer. d3d10umd rotates the
  swap chain's buffers by copying each (DxgiFns.cpp, "XXX: Copying is not very efficient") and Panfrost's
  resource_copy_region IS util_resource_copy_region (pan_resource.c: CPU copy through transfers) -> several 10 MB
  CPU copies out of uncached GPU memory per DWM frame.
- Mesa 0052: panfrost_resource_copy_region - same-format, same-sample, non-ZS, non-compressed textures that are
  render-target/sampler capable go through pctx->blit (GPU); the rest stays util_resource_copy_region. Build
  mali-umd-0052 fe2db478....
- 03:05 run h3j started (UMD 0052).
- **Run h3j (03:05-03:13, runs/h3j, UMD 0052): GPU copies tripled DWM's rate.** DWM frames/10 s: logon 73 (was 29),
  gdi animation 42-65 (was 15-19) = **~5-6.5 fps**; KMD 2686 jobs before the tests (was 896), 8936 after, 0 faults.
  Per DWM frame now (gdi phase): gap median 156 ms = frontbuffer **convert ~105 ms** (CPU readback of the rendered
  frame from uncached memory, 32-bit loads + swizzle) + GPU idle waits 30-47 ms + present memcpy 0-16 ms. The
  readback is now 2/3 of the frame.
- **Zero-copy research:** Samsung kernel cal_9820/regs-dpp.h ([ianmacd/d2s](https://github.com/ianmacd/d2s/blob/master/drivers/video/fbdev/exynos/dpu20/cal_9820/regs-dpp.h)):
  DPU_DMA base 0x19070000, channels G0 +0x1000, G1 +0x2000, VG0 +0x3000, VG1 +0x4000, VGF0 +0x5000, VGRF +0x6000;
  per channel IDMA_ENABLE +0x0 (SRESET bit24, SFR_UPDATE_FORCE bit4), IDMA_IN_CON +0x8, IDMA_SRC_SIZE +0x10
  (WIDTH 15:0, HEIGHT 29:16), IDMA_SRC_OFFSET +0x14, IDMA_IMG_SIZE +0x18, IDMA_IN_BASE_ADDR_Y +0x40. UNVERIFIED on
  this phone: h3 now peeks 0x19071000..0x19076000 (0x60 each, read only) to find the channel scanning 0xCA000000.
  If SRC_WIDTH is the line stride in pixels, 1088 -> pitch 4352 = 68 x 64 B (Panfrost-aligned); the fb carve-out
  (Exynos9820.h EXYNOS_FB_SIZE = 9849600, page-rounded reservation) must grow to >= 4352 x 2280 = 9922560 B.
- 03:15 run h3k started (DPU peek).
- **Run h3k (runs/h3k, DPU peek, read only):** G0 0x19071000 is the scan-out channel: IDMA_ENABLE 0x4,
  IN_CON 0x02000000, SRC_SIZE 0x08e80438 (h 2280, w 1080), IMG_SIZE 0x08e80438, IN_BASE_ADDR_Y 0xca000000.
  Channels G1/VG0/VG1/VGF0/VGRF (+0x2000..+0x6000) idle. Samsung dpp_reg.c (dma_reg_set_src_size):
  SRC_SIZE = the full buffer f_w x f_h, i.e. the line stride in pixels; IMG_SIZE = the cropped image -> width 1088
  in SRC_SIZE with IMG_SIZE left at 1080 should scan 1080 px out of 4352-byte lines.
- Samsung cal_9820/regs-decon.h: DECON base (decon_f) 0x19030000; SHADOW_REG_UPDATE_REQ +0x0060 (GLOBAL bit31,
  DQE bit28, WIN(n) bit n, FOR_DECON 0x3f); HW_SW_TRIG_CONTROL +0x0070 (SW_TRIG_EN bit8, HW_TRIG_EN bit0,
  HW_SW_TRIG_HS_STATUS bit28). decon_reg.c commits window/DPP changes with
  decon_write_mask(SHADOW_REG_UPDATE_REQ, ~0, GLOBAL or WIN(idx)). Whether IDMA SRC_SIZE is latched by that
  request or by IDMA_ENABLE SFR_UPDATE_FORCE (bit4) is not stated: the KMD tries the DECON GLOBAL request, then
  re-reads SRC_SIZE and the camera says whether the panel follows. All UNVERIFIED on this phone until that run.
- Firmware #116 (H4b): EXYNOS_FB_RESERVED_SIZE 10 MiB (0xCA000000..0xCA9FFFFF) replaces the page-rounded
  EXYNOS_FB_SIZE in the carve-out list and in the MMU map. Note: Beyond0LteFbSerialPortLib keeps its FB_CON_STATE
  at EXYNOS_FB_BASE + EXYNOS_FB_SIZE, which a 4352-pitch scan-out covers - harmless after ExitBootServices
  (firmware-only state), but the re-stride must never happen while the firmware console is live.
  Image s10e-artifacts\boot116-fb10m-v2.img sha256 740d97fe...; not flashed yet (phone runs #115).
- Review of a1dd2e2: no reservation between 0xCA965000 and 0xD0000000 (rg of Exynos9820.h RSVD bases: seclog 0xC0000000, modem_spare 0xD0000000) - OK; SimpleFramebufferDxe still reports FrameBufferSize = EXYNOS_FB_SIZE (correct: the GOP mode is unchanged, the KMD maps the larger range itself); fixed a stale 'page-rounded' comment in PlatformMemoryMapLib.c (own commit).
- **TWRP DT reads (2026-10-14, root):** decon_f@0x19030000 reg 0x19030000 size 0x10000 (VERIFIED); dsi_mode 0,
  trig_mode 0 (DECON_HW_TRIG), psr_mode 2 (= DECON_MIPI_COMMAND_MODE in dpu20 decon.h: the panel refreshes on TE
  with the HW trigger, so shadow registers latch at the next TE frame). dpp@0x19071000 reg: DPU_DMA ch 0x19071000
  +0x1000, DPP 0x19021000 +0x1000, DMA common 0x19070000 +0x110; src_f_w range {16, 65534, step 1} -> 1088 is a
  legal full width. DPU sysmmus 0x190A0000/0x190B0000/0x190C0000 exist (Linux probes them; it enables them only when
  decon attaches, and keeps s3cfb.bootloaderfb=0xca000000 mapped). Our DPU address 0xCA000000 in IN_BASE_ADDR_Y is
  the PA as given by sboot -> sysmmu bypass assumed at hand-off (UNVERIFIED; a fault would show as a frozen or black
  panel, recoverable by disarming).
- **KMD 1.0.36.1 (H4b, KMD re-stride; s10e-artifacts\mali-kmd-36, sys sha256 95c188d8...):** Parameters\ScanoutStride
  (default 0); = 1088 and FbMatchesGop -> MwRestride (display.c): refuses if [0xCA000000, +4352*2280) overlaps any
  MmGetPhysicalMemoryRanges RAM (firmware #115, RestrideStep 5); maps G0 and DECON pages, requires SRC = IMG =
  2280<<16|1080 and base 0xCA000000 (else step 2); remaps the fb at 9922560 B, moves rows bottom-up, zeroes the
  32 B tails, writes SRC_SIZE 2280<<16|1088, sets DECON SHADOW_REG_UPDATE_REQ bit 31 and polls up to 200 ms for it to
  clear. Cleared -> Fb.Pitch 4352 (step 0). Not cleared -> SRC_SIZE and rows put back (step 4). Logs
  RestrideSrcBefore/Img/Base/ShadowReq/TrigControl/ShadowMs/ShadowAfter/SrcAfter. Not validated on the device.
- extra-gpu-h3.cmd: marks the KMD's Restride*/ScanoutStride/FbPitch/Build trace values (type | findstr, since reg export is UTF-16 - tested on the PC with a fake export) and arms the NEXT boot: gpu\restride.flg -> Parameters\ScanoutStride 1088, absent -> deleted.
- 03:36 deploy for run h3l: firmware #116 flashed (boot readback 740d97fe...), KMD 1.0.36.1 swapped into the DriverStore
  by tools/phone/kmd-deploy.sh (previous .sys fdd3adb3... kept as C:\S10eTmp\kmd-prev), ESP extra.cmd =
  extra-gpu-h3 (md5 e79ea5a7...), gpu\restride.flg set. h3l = baseline with #116 + KMD 36, ScanoutStride still absent
  (expect "RestrideStep" absent, ScanoutStride 0); its script arms 1088 for h3m.
- 03:37 run h3l: firmware #116 boots to Windows (ping at 03:37:00, ~80 s after reboot as before).
- **Mesa 0053 (H4b UMD half, build mali-umd-0053, maliumd a2c7f60f...):** shim malikm_scanout_bo (MW_ESCAPE_SCANOUT
  once per process; only if GpuVa != 0 and Pitch % 64 == 0) registers the scan-out as a "fixed" BO (h WBO_FIXED:
  never listed in submissions, never locked/mapped, GEM_CLOSE forgets it); d3d10_gdi imports it as a linear
  B8G8R8A8 render target (resource_from_handle, PIPE_BIND_SCANOUT); d3d10_pan_flush_frontbuffer: a runtime primary
  (d3d10_present_alloc) of the panel's size on a GPU screen -> pipe->blit into it, flush + fence_finish (<= 100 ms),
  then malikm_primary_present(bgrx NULL): marks the kprim GPU-presented (MW_ESCAPE_SCANOUT hAllocation, once) and
  calls pfnPresentCb with no copy. Anything else (no re-stride, other size, MSAA, failure) -> the old CPU path.
  Requires KMD 1.0.35.1+ (escape 4) - KMD 36 is deployed. MALIKM_NO_SCANOUT=1 turns it off. tlog lines "scanout:".
  KMD jobs end with JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE (maligpu.c:439), so the frame is in DRAM for the DPU.
- Review of 112b664 / 0053: checked - the fixed BO is skipped in both submission loops (the blit job lists it in
  bo_handles), MAP refuses it, GEM_CLOSE forgets it; panfrost kmod import only does PrimeFDToHandle + GET_BO_OFFSET
  (va from wbo) and lseek size (malikm_prime_fd_size = pitch*h = 9922560 with the re-stride); a landscape/odd-size
  primary, MSAA, a softpipe process or a failed mark falls back to the CPU path. Known limits (not bugs): once a
  primary is marked GPU-presented a later CPU-path present of it would be skipped by the KMD (stale frame) - only if
  the GPU path fails after having worked; tearing (no vblank sync of the blit). Nothing to fix.
- **Run h3l (03:35-03:43, runs/h3l; #116 + KMD 36, ScanoutStride absent, UMD 0052):** boots fine on #116, KMD 36
  Build 0x24, FbMatchesGop 1, FbPitch 4320, ScanoutStride 0 (no re-stride, as intended), ScanoutGpuVa 0x2000000
  (MwMapScanout OK); 0 GPU faults (8970 jobs), cube 30.9 fps, tex still exits 0xC0000005, LogonUI crash events
  (as before). gfxperf's "dwm fps" counter reads 0.7 in every run (h3j/h3k too) - not a measure; DWM's frame rate
  comes from the tlog (frontbuffer lines). next-stride: ScanoutStride 1088 armed.
- 03:46 h3m deploy: UMD 0053 into System32 by umd-deploy.sh (a2c7f60f..., also in ESP gpu\umd-new - same files).
  h3m = first re-stride boot + GPU present. Expect trace RestrideStep 0, tlog "scanout: ... imported: yes".
- 03:46 h3m webcam (scratchpad h3m-cam1/2.jpg): panel lit, blue/white image with evenly spaced full-width horizontal dark bands, unchanged over 27 s - looks like OLED PWM vs rolling shutter, not a stride shear (that would tear diagonally); undecided until the trace.
- **Run h3m (03:44-03:52, runs/h3m; #116, KMD 36 with ScanoutStride 1088, UMD 0053) - H4b WORKS:**
  KMD trace: RestrideSrcBefore/Img 0x08e80438, Base 0xca000000, TrigControl 0x3061, ShadowReq 0 before; after the
  GLOBAL request it cleared in 10 ms, SrcAfter 0x08e80440 (width 1088), RestrideStep 0, Restrided 1 (the
  DECON SHADOW_REG_UPDATE_REQ +0x60 bit 31 latches IDMA SRC_SIZE - now VERIFIED by this run). UMD tlog: "scanout: va
  0x2000000 1080x2280 pitch 4352 format 22 ... imported: yes", primary 0x40001300 GPU-presented, every present
  "present 0", blit+wait 0-16 ms (first 47). DWM presents per 10 s (tlog "present primary", h3m dwm pid 1212 vs h3l
  pid 1232 with UMD 0052): logon 403/414 vs 67/59 (**~41 fps vs ~6.5**); gdi phase (190-210 s, app 532 paints/25 s =
  21/s) 211/214 vs 65/66 (**DWM keeps up with the app**); cube phase (150-180 s) 35-74 vs 8-22 - the cube's own
  presents (31.9 fps, 0 failures) still do not drive DWM. 0 GPU faults (29330 jobs). Webcam: panel lit with a blue
  and white image (Windows' blue wallpaper/lock screen colours) under even horizontal PWM bands; no shear visible.
  tex still 0xC0000005; LogonUI crash events as before.
- **H4c - windowed D3D presents never reached DWM:** h3m KMD stats: 1696 DxgkDdiPresent calls = DWM's 1633 primary
  presents + 68 boot blts, so d3dtri's 637 presents (pfnPresentCb hr 0, "present blt surface") never reached the KMD,
  and no process opened a shared surface during the cube. MS Learn "DXGI Presentation Path": windowed with DWM on,
  DXGI opens a shared surface (render target for the app, texture for DWM) and calls the UMD's **BltDXGI** (Flags.Present)
  to copy the back buffer into it. d3d10umd's _Blt (DxgiFns.cpp) was LOG_UNSUPPORTED + S_OK - frames dropped.
- Mesa 0054 (build mali-umd-0054, maliumd b180f5bb...): _Blt = one pipe->blit (src subresource whole -> dst rect,
  LINEAR filter when stretched, resolve/convert by blit), flushed; tlog "BltDXGI n: ..." and "PresentDXGI n: ..."
  (first 8 and every 300th) with sizes, formats, flags, primary/opened handles - so the next run shows the real
  windowed path even if this alone does not fix it. Rotation (Blt->Rotate) not handled, only logged.
