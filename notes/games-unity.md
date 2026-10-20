# G32: Unity games on the Mali - Dave the Diver, Schedule I (2026-10-19)

User: "im gonna try another game too ... dave the diver", then goal "run dave the diver without crashing".
Both are Unity (D3D11 on Windows), x64 -> maliumd_ec.dll (ARM64EC UMD behind the maliumd forwarder), not the GL ICD.

## Dave the Diver (Steam 1868140, Unity 6000.0.52 URP, IL2CPP, v1.0.6.2113.steam)
- Player.log (C:\Users\S10e\AppData\LocalLow\nexon\DAVE THE DIVER\Player.log, copy traces/games/dave-player.log):
  "Direct3D 11.0 [level 10.1]", Renderer "Arm Mali-G76 MP12 (Galaxy S10e, G10/G11)", VRAM 2955 MB;
  kGfxThreadingModeSplitJobs not supported on D3D11 -> ClientWorkerJobs; shaders not supported on this GPU:
  Hidden/CoreSRP/CoreCopy, Hidden/VoxelizeShader, Hidden/Universal/HDRDebugView; "RenderTexture.Create failed: format
  unsupported for random writes - R16 SFloat" (LensFlareCommonSRP, UAV = FL 11 feature). Reached LogoManager, then
  "Crash!!!".
- Crash 1 (12:08 phone time, launched by the user through Steam): Application Error 1000, maliumd_ec.DLL c0000005
  offset 0x80416c. Unity crash report (tools/phone/games/unitycrash.ps1 -> traces/games/dave-crash.dmp), cdb with the
  0148 EC PDB (s10e-artifacts\mali-g12-ec at the time, maliumd_ec a11ad698):
    READ AV at 000001bd`b9ba9000 (page end) in memcpy_unaligned, ldp q0,q1,[x1,#0x50] - x1 = the copy SOURCE
    util_copy_rect <- panfrost_ptr_unmap+0x5cc <- u_transfer_helper_transfer_unmap <- util_resource_copy_region
    <- tc_batch_execute (driver thread). Rows 0x17c0 B (1520 px x 4 B), 0x2d0 = 720 rows.
  = a CopySubresourceRegion of a 1520x720 4-byte depth/stencil texture (URP's depth copy) on the CPU path
  (panfrost_resource_copy_region keeps depth/stencil on util_resource_copy_region, H4); the destination's write-map
  goes through u_transfer_helper, and panfrost_ptr_unmap's u-interleaved -> linear conversion (after
  LAYOUT_CONVERT_THRESHOLD whole overwrites) copies from trans->map past its end. Root cause of the over-read inside
  the transfer-helper path not pinned down (the GPU path below avoids it entirely).
- Fix Mesa 0160: same-format, same-sample-count depth/stencil copies go through pctx->blit with the format's Z/S mask
  (panfrost's blitter does ZS), both resources DEPTH_STENCIL-capable. Also removes ~36 ms of uncached CPU copy per
  full-screen depth copy. Build ec: maliumd_ec 889d474a (s10e-artifacts\mali-umd-0160-ec, PDB kept there).
  Live: System32\maliumd_ec.dll = 0160; rollback = System32\maliumd_ec.dll.0148 (a11ad698). Not yet in the ARM64 /
  x86 UMDs (port after validation, memory rule).
- steam_appid.txt (1868140) next to DaveTheDiver.exe since the shortcut work (notes/games-steam.md).

## Progress log
- 20:25 0160 deployed live (rename trick: the old DLL renamed, new copied); waiting for the user's launch.
- 20:35 0160 result: past the depth-copy crash, through LogoManager / LoadGameData / InitAfterSaveSystem to the title
  ("it launched and got to the main menu then exited", user). Crash 2 (12:23:32 and 12:27:49 phone time, reproducible
  at DR.Title.Start): 1000 maliumd_ec.DLL c0000409 offset 0x7920dc = abort (no Unity report: fail-fast). Shim stderr
  capture (C:\S10eTmp\tlog\malikm-DaveTheDiver.exe-<pid>-stderr.txt): "Assertion failed: count != 1, file
  u_inlines.h, line 83" (+ "malikm-wddm: allocation list full (256 slots): 27 live BOs not listed" - OPEN, separate).
  WER LocalDumps (tools/phone/wddm2/werdump.ps1 -Exe DaveTheDiver.exe) -> DaveTheDiver.exe.6772.dmp, cdb with the 0160
  PDB: abort <- _wassert <- pipe_reference_described <- pipe_resource_reference <- util_copy_constant_buffer <-
  panfrost_set_constant_buffer <- tc_batch_execute (driver thread) = a freed constant buffer re-referenced.
  Cause: Mesa 26's threaded context records set_constant_buffer with the bare buffer pointer (tc_set_constant_buffer,
  same upstream) and relies on frontends dropping references through pipe_context::resource_release (TC queues it
  behind the recorded calls; st/mesa and u_upload_mgr do). d3d10umd (TC enabled by us in G29) dropped them with
  pipe_resource_reference(&r, NULL) -> e.g. DestroyResource of a constant buffer freed it while queued draws named it.
- Mesa 0161: DeviceReleaseResource (State.h) = pipe_resource_release + NULL, used for DestroyResource's resource and
  cb_buf, the renamed CB slice at WRITE_DISCARD and in CbMaterialize, UAV / buffer-SRV / RTV / DSV destroy. Sampler
  views already went through sampler_view_release; index / vertex buffers are referenced by TC itself.
  EC build maliumd_ec fd4215b2 (s10e-artifacts\mali-umd-0161-ec), live via tools/phone/games/ecswap.sh (keeps
  maliumd_ec.dll.pre-0161-0 = 0160). Result: title screen held > 7 min with no crash (was ~10 s); user loading a save
  and playing (12:41 phone time, watcher bebi14zr2: alive, no appcrash, no TDR).
- Dave needs the Steam client: with steam_appid.txt it still exits after ~2 s (code 0x35) and starts
  steam://run/1868140 (Steam DRM - not bypassed). Stardew does not.
- 20:55 user: "its running ... extremely low fps", then "the actual ingame is all black" (menu visible).
  Measured without touching the screen: KMD heartbeat 5 s: ~6.4 presents/s, ~390 submissions/s (~60 per frame),
  vsync 58/s. threadtop: one thread 103 % of a core (the driver thread), the rest idle; cores 4/5 ~75 %. Shim tlog:
  "submit 527000 (1868 allocations listed, 1868 live BOs): Render mean 2493 max 7411 us", BOs own 1866 (922 MB) ->
  ~390 x 2.3-2.5 ms = the whole driver thread in the runtime's Render walking an 1866-entry allocation list (H2's
  "list every live BO" from WDDM 1.x).
  Docs (AGENTS 13): MS Learn "GPU virtual memory in WDDM 2.0": "In physical mode, the scheduling model remains the
  same as it is with WDDM v1.x. The UMD continues to generate the allocation and patch location lists"; "Driver
  residency in WDDM 2.0" / Residency overview: "residency is moved to an explicit list on the device instead of the
  per-command buffer list. VidMm ensures that all allocations on a particular device residency requirement list are
  resident before any contexts belonging to that device are scheduled". The shim makes every BO resident at creation
  (0069; normal, slab, opened and shared paths call w_resident).
- Mesa 0162 (shim): with residency on (resid 1) and no MakeResident failure so far, a submission lists only the
  batch's own BOs (Panfrost's bo_handles); WDDM 1.x / any MakeResident failure keeps the full list; MALIKM_LIST_ALL=1
  forces it (diagnosis). EC build maliumd_ec cf0f5516 (s10e-artifacts\mali-umd-0162-ec) deployed live
  (ecswap.sh, previous kept as maliumd_ec.dll.pre-0162-0) - takes effect at the next game start. NOT yet validated.
- Black in-game: Unity logs only the start-up "not supported on this GPU" shaders: Hidden/CoreSRP/CoreCopy,
  Hidden/VoxelizeShader, Hidden/Universal/HDRDebugView (+ R16 SFloat UAV render texture). INFERENCE: these are
  "#pragma target 4.5" shaders = feature level 11_0 on D3D11; URP's camera-target copy without CoreCopy leaves the
  3D view black while the UI overlay draws. FL 11_0 = the open D4 step of notes/gpu-umd-d3d11.md (SM5 shaders, UAVs at
  all stages, tessellation decision). Driver log during play: SetError 0x887b0001 (DXGI_DDI_ERR_WASSTILLDRAWING, a
  DO_NOT_WAIT map / query poll answer - normal) only.
- 21:10 0162 validated (game relaunched by us through Steam; user: "it already looks faster", then "got ingame and it
  actually got into the level"): submissions list 12-17 allocations, Render mean 103-179 us (was 2300-2500); app
  pacing: present gaps <=50 ms 54, <=100 ms 55, <=250 ms 7 per 120 frames (~12 fps, was ~6); no thread pegged (two
  at ~39 %), GPU engine 53 % (gpuengine.ps1 -Name DaveTheDiver), no appcrash, no TDR. Per 120 frames: ~25k
  DrawIndexed (~210/frame), ~6k SetRenderTargets (~52/frame, URP 2D lights), ~6.9k get_batch_slow (new Panfrost
  batches), ~48k ResourceMap (41k CB discards = renamed), ~2000 submissions/s; "idle 31-63 ms via event" full waits
  in the log. Next for fps: what forces the full waits / per-RT batch reloads (CPU and GPU both half idle =
  serialisation).
- User: background missing at another place in-game = the FL 11_0 shaders (above).
- OPEN: 0160-0162 are only in the EC UMD (x64 apps). Port to the ARM64 UMD (DWM, native apps) and x86 after the play
  session - an ARM64 UMD swap needs a reboot (DWM holds it).
- 21:20 user play notes: the area that was dark rendered after returning to the boat (day -> night scene change);
  changing the in-game brightness froze the game for a while, then it recovered ("its back working"). During the
  stall: responding True, no appcrash, no TDR; afterwards heartbeat ~34 presents/s (whole screen), umd top per 120
  frames normal (Present 1.6-1.9 s, ~30k draws). INFERENCE: settings change = Unity recreating render targets /
  compiling shader variants under emulation, not a driver hang. User is collecting which scenes render wrong.
- 21:25 user: leaving a dive back to shore shows a white, then grey screen for up to ~10 s before the loading screen.
  Not yet attributed (INFERENCE: synchronous scene load under emulation, no presents); to sample threadtop + GPU
  engine during the next transition.
- 21:27 sampled during the dive-exit load (user: "its loading"): threadtop tid 7660 84.9 % (main/loading thread),
  tid 6380 30.7 % (busiest during gameplay = render/driver side), GPU engine 30 %, responding, no appcrash, no TDR
  => the white/grey screen + load are CPU-bound on the game's loading thread under emulation, not a driver stall.
- 21:32 farm load (user: "it does" take long): threadtop tid 7660 80 % Wait PageIn, tid 4324 68 % Executive, GPU 3 %;
  Memory: Available 101 MB of 5911 MB, Pages Input/sec 12 533 (~50 MB/s), committed 8.4 of 10.8 GB => RAM
  exhaustion, not the driver. Private MB: DaveTheDiver 3296, steamwebhelper x6 878 (WS 1136), MsMpEng 350,
  explorer 312; GPU BOs ~830 MB resident (0069 residency). taskkill steamwebhelper (the game needs only steam.exe):
  Steam respawned 6 helpers at once, 589 MB private; Available 463 MB, Pages Input 181/s, game alive.
- 21:35 restaurant load (user: "sample it"): Available 105 MB, Pages Input 7 853/s; threads 7472 / 7824 / 6380 each
  ~12 % all Wait PageIn; Unity log "Total: 19925 ms (FindLiveObjects 1696 ms ... MarkObjects 18081 ms)" = its
  unused-asset GC walking paged-out memory. RAM shortage again; no appcrash, no TDR. RAM saving deferred by the user
  ("we'll think about that later"). Candidates noted for then: our resident GPU BOs (~830 MB pinned under 0069
  residency, Panfrost BO cache), Steam web helpers (~0.6-0.9 GB), Defender (350 MB).
- 21:45 day (boat) scene black again (user photo + traces/gl/dave-day-black.png): UI correct, 3D view black, the
  letterbox strips left/right still show the underwater scene (stale, uncleared) => the world camera outputs black.
  Re-think: the 3 unsupported shaders are RenderGraph's CoreCopy (log: "RenderGraph is now disabled" = compatibility
  path), probe-volume VoxelizeShader and HDRDebugView - none needed for normal drawing, so FL 11_0 is probably NOT the
  cause. Player.log around the scene: only HDRDebugView repeats, "Water2D/Metaballs_RegularFresnel: fallback 'UnLit'
  not found", SushiBarInteriorRenderTexture load. Shim stderr empty, no driver warnings. => a shader / format our
  driver renders wrongly (black) rather than refuses. Next: user toggles graphics options (post-processing, quality,
  HDR/lighting) to localise the pass.
- 21:55 Mesa 0163 = d3d10umd FrameCap.cpp: one-frame pass log armed by C:\S10eTmp\framecap.flg (checked every 30
  presents), lines "fc pass N: rt.. zs..; clears; draws; ps {ptr/TGSI hash}; blend; tex {PS slots 0-3}" + "fc copy";
  tools/phone/games/framecap.sh NAME. EC build 702dbe44 (s10e-artifacts\mali-umd-0163-ec), live (pre-0163-0 = 0162).
- Capture day-black (traces/games/framecap-day-black.txt, 59 passes, 4 copies): scene into 643399390
  r11g11b10_float 1280x720 + z32_float_s8x24 (passes 7, 9, 11: 33 + 1 + 43 draws), r32_float depth copies (8, 42, 47,
  52), SSAO-like r16_unorm / rgba16f 640x360 chain (17-21), bloom r11g11b10_float 640x360 down to 20x11 and up (22-37),
  pass 38 = uber post: 64339A7D0 + bloom 34446E220 + LUT 343B9B600 (r8g8b8a8 1024x32) -> 643399390, pass 43 draws 34
  more into it, pass 54 final blit into the 1520x720 r8g8b8a8_srgb back buffer + UI, pass 56 a full-screen copy.
  Pass 6 builds the colour-grading LUT 343B9B600 1024x32 from four r16_float 128x1 textures (curves).
  HYPOTHESIS: the LUT comes out black (r16_float curve textures wrong) -> pass 38 grades the whole scene to black;
  the UI is drawn after -> visible. Next: a capture of a scene that renders (dive / restaurant) for comparison.
- 22:05 capture boat-ok (after a dive, renders; traces/games/framecap-boat-ok.txt, 38 passes): SAME LUT pass 6 (same
  resources 43B9B600 / r16_float curves) -> the LUT hypothesis is wrong. The difference: no depth-of-field
  (black frame passes 17-21: CoC r16_unorm 1280x720 from r32_float depth, MRT r16_unorm + rgba16f 640x360, two
  rgba16f blurs, composite into 64339A7D0) and no bloom (22-37); the uber pass reads the DoF output in the black frame,
  the plain scene in the working one. User: the boat scene is black when reached directly (load / time skip), renders
  after a dive -> the game enables DoF+bloom on the direct path.
- Mesa 0164: capture reads back 3 pixels of rt0/rt1 after each pass with draws/clears ("fc px" lines). EC build
  (s10e-artifacts\mali-umd-0164-ec) live; needs a game restart.
- 22:30 ROOT CAUSE of the black boat scene. Capture day-black2 with 0164 readback (traces/games/framecap-day-black2.txt):
  inputs fine (pass 16 scene 0.16-0.31, pass 17 CoC 0 / 0.33), pass 18 (URP Gaussian DoF prefilter, MRT rt0 r16_unorm
  CoC + rt1 rgba16f colour) rt1 = +inf at every sample -> blur 19-20 inf, composite 21 inf, bloom 22-32 inf, 33-37 NaN,
  uber 38 = 0 -> black (the UI is drawn after). URP's prefilter writes coc and colour*coc - no division, cannot make
  inf; min precision is already ignored (ShaderParse.c).
  Repro drivers/MaliG76Wddm/tools/d3dmrt (one draw, target 0 = 0.33, target 1 = (0.25,0.5,0.75,1), 7 format pairs,
  vs WARP; build-d3dmrt.cmd x64/arm64): Mali 0/7 - SV_Target1 never written in any format pair, even with no target 0
  (stays at the clear value; garbage = inf in the game). BIFROST_MESA_DEBUG=shaders (diagnosis): the compiled FS
  issues BLEND for both outputs (blend_descriptor_0 / _1) -> the shader is right.
  panfrost pan_cmdstream.c create_blend_state: `so->pan.rt_count = blend->max_rt + 1` and info[c] only for c <= max_rt;
  the rest stay zero = disabled (panfrost_emit_blend: !so->info[i].enabled -> BLEND_MODE_OFF). p_state.h:
  "max_rt:3 /* index of max rt, Ie. # of cbufs minus 1 */"; st/mesa sets it (st_atom_blend.c:237); d3d10umd's
  CreateBlendState / CreateBlendState1 memset the state and never set it -> max_rt 0 -> only target 0 ever written.
- Mesa 0165: max_rt = PIPE_MAX_COLOR_BUFS - 1 in both (a D3D blend state does not know the bound target count).
  EC maliumd_ec fe23bb07 (s10e-artifacts\mali-umd-0165-ec) live: d3dmrt64x 7/7 match. Game check next. Port to the
  ARM64 and x86 UMDs with 0160-0162 (any D3D MRT app was affected).
- 22:45 user: "its working" (boat scene renders with 0165). Next goal: performance ("utilizing more of that gpu/cpu").
  Dive profile (active diving, 120 frames ~7 s): frame 60.5 ms (~16 fps), inside the UMD 31.5 ms/frame on the game's
  render thread; GPU 3D 67 %; threads 1132 75 % (render), 3292 71 % (driver), 6372 37 %; RAM 261 MB free.
  app umd top: _Present 1795 ms (sync-object wait 0xc3 1690 ms = ~14 ms/frame GPU wait), PsSetShaderResources 36360
  calls 1129 ms (9.4 ms/frame, 31 us/call), Flush 436, DrawIndexed 347, ResourceMap 283; driver thread:
  pan:shader_state_fs 1204, pan:sub:textures 1093, prepare_draw 791, get_batch_slow 6000 calls 758; SUBMIT (0x40) 11760
  calls 1239 ms = 98 submissions/frame at ~105 us (two per Panfrost batch: vertex/tiler + fragment).
  Mechanics: Panfrost's threaded context has no create_fence -> every flush = tc_sync (render thread waits for the
  driver thread to drain), then Present waits for the whole frame on the GPU (DxgiFns "the frame is finished first")
  because the KMD runs a Blt present's CPU copy in MwSubmitCommand (command.c), not after earlier jobs complete.
  Plan: (1) bind only used SRV slots, (2) merge each batch's two submissions, (3) KMD: present copy after the earlier
  jobs -> drop the UMD's GPU wait.
- Mesa 0166: SetShaderResources binds slots 0 .. highest bound (Device::sampler_view_count), unbinding trailing ones
  that were in use (was all 128 slots every call). EC maliumd_ec 27f49200 (s10e-artifacts\mali-umd-0166-ec) live;
  d3dmrt64x 7/7, d3dbc64x lossy 0/126 differ PASS (= baseline; the exact mode fails by design since G26).
- 23:00 0166 measured in the dive (game relaunched): PsSetShaderResources gone from the top list (was 1129 ms/120 frames);
  frame 52.5 ms (was 60.5, ~19 fps); the render thread now waits in Flush 1561 ms/120 (tc_sync on the driver thread) +
  Present GPU wait 1493 ms; GPU 67 %. => the driver thread is the bottleneck: pan:shader_state_fs 1128 ms (mostly
  pan:sub:textures 1025 ms, 33598 calls ~30 us - panfrost_batch_read_rsrc flushes the batch that rendered a sampled
  texture, i.e. submissions counted there), prepare_draw 683, get_batch_slow 653, SUBMIT 11754 calls 1264 ms.
- Mesa 0167: one submission per Panfrost batch - malikm_drm.c SUBMIT body -> wddm_submit_jobs(subs, bolists, n) (the
  ioctl = n 1), new malikm_submit_pair(fd, vt, fs); pan_jm.c (_WIN32): jm_submit_batch arms a thread-local jm_pair when
  the batch has tiler + draws + fragment, jm_submit_jc holds the vertex/tiler submit and sends both via the pair call
  (ENOSYS -> one by one; a held submit without its fragment is sent alone at done:). The KMD runs a submission's
  chains in order (command.c Kick: next chain after the previous is reaped). EC maliumd_ec ed5e4ed3
  (s10e-artifacts\mali-umd-0167-ec) live; d3dmrt / d3dbc lossy below.
  0167 regression: d3dmrt64x 7/7 match, d3dbc64x lossy 0/126 differ PASS.
- 23:15 0167 measured in the dive: frame 46.5 ms (~21.5 fps; was 52.5), submissions 5880/120 frames (was 11754 = one
  per batch now), SUBMIT 851 ms (was 1264), Flush wait 1192 (was 1561), GPU 66 %, Present GPU wait 1494 ms
  (~12.5 ms/frame) = the next target (KMD: present copy after the earlier jobs).
- Benchmark automation (user: "lets do another key recording where you do this yourself"; "look at the lows"):
  keyrec/keyplay v2 = + mouse buttons (WH_MOUSE_LL, "<ms> M LD|LU|RD|RU <x> <y>" physical pixels, both DPI-aware;
  keyplay's injected clicks tagged dwExtraInfo KEYPLAY_EXTRA "KEYP" and skipped by keyrec); touch taps arrive as mouse
  events (verified: the user's two menu taps recorded at 141.0 / 144.3 s). keyrec-start.ps1 = detached start.
  Recording traces/games/davekeys.rec (790 events, F12): taps 140-144 s, keys 209-214 s (boat -> dive), 286-369 s
  (diving: Shift held, D, Space, Ctrl, S, A, W). Player.log markers (traces/games/dave-rec.log): title
  "DR.Title.<Start>", boat "BGM/BGM_Lobby", dive "ondestroy called singleton LobbyMainCanvas".
  Mesa 0168: FrameCap.cpp FrameTimesPresent - while C:\S10eTmp\frametimes.on exists every present appends its
  interval (us) to frametimes.txt. tools/phone/games/davebench.ps1 (user session: launch, per segment wait marker +
  6 s log silence, keyplay, frame times during seg3, close) + davebench.sh (split the recording at > 30 s gaps,
  push, run detached, stats: mean fps, median / p95 / p99 / max ms, 1 % / 0.1 % low fps -> traces/games/bench-TAG.*).
  EC build 9b75aceb (s10e-artifacts\mali-umd-0168-ec) live. First run "0168-run1" started.
- 23:30 davebench run 1 (0168): the route replayed by itself (seg1 taps did NOT register - the user tapped; seg2 boat ->
  dive OK; seg3 replayed 662 events) but no frametimes.txt: the user quit 16 s into seg3 (still loading) before the UMD
  saw the flag (checked every 30 presents). Fixes: keyplay holds a replayed press >= 150 ms and moves the cursor a frame
  before it (touch taps record down+up in the same ms; Unity at ~20 fps missed them); davebench-start.ps1 (the inline
  nested-quote launch never started the bench).
- Fix #3 (Present waits ~12.5 ms/frame for the GPU because the KMD copies a Blt present in MwSubmitCommand, possibly
  before earlier jobs are done): KMD (drivers/MaliG76Wddm): Parameters\DeferPresent (default 1) - MwSubmitCommand stores
  the present in its MW_SUBMISSION (HasPresent); Kick stops at a head submission whose present is pending (own + every
  earlier chain done), sets PresentAtHead + NeedDpc; MwDpcRoutine -> MwPresentDpc: SyncPresentTake (snapshot the head's
  op + fence under the interrupt lock), DoPresent at DISPATCH (as the vsync latch does), SyncPresentDone (same fence
  still at the head -> HasPresent off, DeferredPresents++, Kick completes it and starts the next); SyncReset clears
  PresentAtHead. New escape MW_ESCAPE_CAPS (7) -> MW_CAPS_DEFERRED_PRESENT. Allocations stay valid: dxgkrnl keeps a DMA
  buffer's allocations until its fence completes (after the copy). Build s10e-artifacts\mwddm-g32-defer\MaliG76Wddm.sys
  89088 B test-signed d69c01cd.
  Mesa 0169: shim malikm_kmd_caps() (MW_ESCAPE_CAPS once, under the Present's selected device binding; older KMD refuses
  -> 0); DxgiFns _Present skips the flush+fence_finish when MW_CAPS_DEFERRED_PRESENT (the flush at the top of _Present
  already drained the threaded context and submitted the frame before the runtime present is queued behind it).
  EC 35ea28a6 (s10e-artifacts\mali-umd-0169-ec) live on the OLD KMD = old behaviour (caps 0); d3dmrt 7/7.
  Battery 16 % on AC (BatteryStatus 2) -> KMD swap via TWRP next.
- 00:05 KMD d69c01cd (DeferPresent) deployed via totwrp + kmd-deploy.sh (prev aba061b9 in C:\S10eTmp\kmd-prev),
  booted OK (adapter OK), EC 0169 live. Menu frame window after boot: 17 ms/frame (~59 fps) at the title.
  Bench automation lessons: (1) the phone clock restarts at each boot -> the old Player.log looked newer than the
  launch and the markers fired at once (two bench instances ran) -> davebench.ps1 moves the old Player.log aside;
  title marker needs 10 s of log silence; davebench-stop.ps1. (2) Injected MOUSE clicks only hover the title menu
  (scan-out traces/wddm2/dave-menu-fb.png: cursor on New Game, highlighted); keyboard Enter does nothing (user);
  InjectTouchInput (keyplay: left taps as touch by default, KEYPLAY_MOUSE=1 for mouse) pressed Continue (user: "that
  did the trick"). (3) The boat part depends on the day's story state (an AfterNewMissionFinished popup ate the
  replayed keys) -> new recording traces/games/davekeys2.rec (980 events): taps 27 s / 72 s while loading (dropped -
  one is in the taskbar corner), Ctrl 123 s + Continue tap (765,493) 135.7 s, boat keys 198-213 s, dive 256.8-387 s;
  davebench.sh SPLIT="120:140,190:230,250:" windows.
- Battery 7 % (on AC, draining under the game) -> bench postponed until charged; nothing running on the phone.
- 12:18 first full automated run (davekeys2.rec, SPLIT windows, battery ignored per user): seg1 Continue touch tap
  worked, seg2 boat keys 227 events, seg3 dive started 12:22:31. User: a fish pushed Dave to the surface menu, then the
  low-battery popup -> run stopped manually (davebench-stop.ps1), so the dive part is not a clean route.
  KMD DeferPresent d69c01cd + EC 0169, traces/games/bench-0169-kmddefer.{txt,frametimes.txt}:
  whole 119.6 s: 2728 frames, mean 22.8 fps, median 41.0 ms, p95 63.3, p99 89.7, max 195.1, 1 % low 8.3 fps, 0.1 % 5.1.
  first 60 s (diving, before the interruption): mean 24.2 fps (41.2 ms/frame, was 46.5 with 0167), median 39.1 ms,
  p99 69.1 ms, 1 % low 10.3 fps. Indicative only (route interrupted, low battery).
  Crash watch afterwards: APPCRASH maliumd_ec.DLL c0000409 offset 0x7920dc + TDR 1 around the stop (12:25-12:27) -
  open lead: was it the forced kill or a real fail-fast? (check the dump / WER report next session).
  Spike analysis of that run (PC only, phone paused): most frames 30-50 ms (2095 of 2728); 45 frames > 80 ms. During
  the dive (0-60 s) the slow frames are isolated 190 ms spikes about every 13-16 s (34.4, 47.4, 63.3 s), which looks
  like a periodic stall (GC / streaming / a driver flush?), not steady load. After 78 s (surface menu, battery popup)
  they come in clusters of 80-180 ms frames. Next: catch one of those 190 ms spikes with the frame capture or a timed
  driver-thread profile.
  APPCRASH symbolized offline (s10e-artifacts\mali-umd-0169-ec PDB, llvm-symbolizer --relative-address):
  RVA 0x7920dc = right after the epilogue of the static UCRT's _chvalidchk_l (ctype.h:234; the isxxx() table check)
  - "autibsp; ret" at 0x7920d4/d8. So the c0000409 points at a return-address check in a ctype call (stack
  corruption, or a ctype call with an out-of-range char from our code?). It does NOT look like an effect of the forced
  kill. Unverified until the dump is read: which caller (stack), which process (WER report time 12:27:49 is 2.5 min
  after the game was gone at 12:25:09). Next session: fetch the WER report + dump from the phone.
  None of our patches (tools/mesa-build/patches) add ctype calls, so the caller is upstream Mesa code (env/option
  parsing, shader cache, etc.) or a corrupted return address - only the dump's stack can tell.
  Phone back on Wi-Fi (user: reads OK, no game runs), battery 5 % on the charger. tools/phone/games/werlast.ps1 (new)
  read the WER reports: AppCrash 12:23:40 and 12:27:59 (phone clock), both maliumd_ec.DLL (timestamp 6ad6501d = the 0169
  EC) c0000409 offset 0x7920dc, Exception Data 0x7 = FAST_FAIL_FATAL_APP_EXIT, which is what the UCRT's abort() raises
  (so the RVA is abort's __fastfail inside the static CRT; the _chvalidchk_l symbol was the nearest public symbol).
  Earlier 12:08:22 = the known c0000005 at 0x80416c (older EC 6ad538ef). No .dmp (LocalDumps not configured).
  UMD tlogs C:\S10eTmp\tlog\malikm-DaveTheDiver.exe-7228/-5008.txt both END with "allocation ... gone" + "binding 0
  (device ...) dropped" = a normal device teardown, no error line after it -> the abort() comes during process exit,
  after the D3D device is gone (a forced Stop-Process = TerminateProcess would not run that path, so these are the
  game's own exits). Next: WER LocalDumps for DaveTheDiver.exe (documented registry key) to get the abort's stack.
  MS Learn abort (learn.microsoft.com/cpp/c-runtime-library/reference/abort): "By default, in non-debug builds of desktop
  or console apps, abort then invokes the Windows Error Reporting Service mechanism" - matches the WER fast-fail report.
  WER LocalDumps (learn.microsoft.com/windows/win32/wer/collecting-user-mode-dumps, "To create a per-application
  setting, create a new key for your application under ...\LocalDumps") set on the phone: LocalDumps\DaveTheDiver.exe
  DumpFolder C:\S10eTmp\dumps (Users modify), DumpType 1 (mini), DumpCount 5. The next abort leaves a .dmp.
- Mesa 0170 (spike breakdown, diagnosis only, active only while frametimes.on exists): FrameTimesPresent snapshots the
  UMD section/entry tick table (Debug.cpp umd_entry_snapshot, no reset) every present; a frame > 100 ms appends
  "# spike <us>: <top 10 entry points/sections, ms during that frame>" to frametimes.txt (umd_entry_diff_report;
  umd_entry_report's 120-present reset bumps a generation -> marked "(report reset)", then the delta is the post-reset
  part only). davebench.sh stats skip '#' lines and print the spike lines.
  EC build s10e-artifacts\mali-umd-0170-ec 4e837eb3 (build-mesa-g76 -Stage ec), live via ecswap.sh (prev 35ea28a6 =
  maliumd_ec.dll.pre-0170-0). Regression: d3dmrt64x 7/7 match, d3dbc64x lossy 0/126 differ PASS (the no-argument mode
  says 63/63 differ = the exact-compare mode, not the baseline used before).
  Review of 9220116: the "63/63 differ = exact-compare mode" claim checked: d3dbc.cpp header (no argument = BCn decode
  compared within one 8-bit step; "lossy" = the ETC2/EAC transcode PSNR check) and notes/g27-x64-apps.md:106 "d3dbc64x
  plain 63/63 DIFF = expected since G26" -> correct, the plain mode is not a baseline since the ETC2 transcode. Patch
  checked: first frame after the flag appears has us 0 (no diff against a stale snapshot); a reset between snapshots is
  flagged and handled (delta = post-reset value; cur < prev also falls back); '# spike' string present in the built DLL.
  Nothing to fix.
- User: "reboot to twrp to speed up the charging" -> totwrp.cmd (clean restart, ESP flag) -> TOTWRP_OK, Windows went
  down; adb sees nothing (phone on a wall charger, not on the PC's USB), so TWRP is not confirmed from here.
- Mesa 0171 (diagnosis): "pan:sub:textures" split into pan:sub:tex:update_view (panfrost_update_sampler_view) and
  pan:sub:tex:read_rsrc (panfrost_batch_read_rsrc: submits the batch still writing a sampled texture). Shows in the
  120-present "app umd top" lines and in 0170's spike lines. EC build s10e-artifacts\mali-umd-0171-ec e2eee16d (section string present in
  the DLL); not deployed yet (phone in TWRP). Review of 72fd677: the patch adds the timers only in the PAN_ARCH >= 6
  path (G76 = v7, so that is the one used); the later PAN_SEC macro block is #ifndef-guarded, no redefinition.
- ARM64 port of 0160-0171 (pending task "port to ARM64"): build-mesa-g76 -Stage cross,collect ->
  s10e-artifacts\mali-umd-0171-arm64 maliumd.dll 976cc0c0 (0170/0171 strings present); make-arm64x.ps1 with the 0171 EC
  -> s10e-artifacts\mali-umd-0171-x (forwarder maliumd.dll + maliumd_arm64.dll + maliumd_ec.dll e2eee16d). Built only:
  the ARM64 UMD is loaded by every ARM64 process (dwm), so it is swapped from TWRP + reboot, then the regression set
  (d3dmrt / d3dbc lossy ARM64 + x64, cube) before anything else.
- 2026-10-20 resume (user: "continue ... you are unattended"): Windows up, battery 60 % charging, Steam running.
  EC 0171 e2eee16d live (ecswap, prev 4e837eb3 = .pre-0171-0); d3dmrt64x 7/7, d3dbc64x lossy 0/126 PASS. ARM64 0171
  set not deployed yet (TWRP swap later). Full bench started (davebench.sh davekeys2.rec 0171, SPLIT 120:140,190:230,250:).
  User: the first launch after a reboot is slower (Steam start etc.) - davebench waits on Player.log markers with a
  600 s timeout per segment (title + 10 s quiet), so no fixed delay depends on it.
- Bench 0171 run 1 (12:00): seg1 marker reached, keyplay "game NOT foreground" x4 + InjectTouchInput down/up failed
  error 5 -> stuck at the main menu (user: "stuck on the main menu"). Cause: scrnsave.scr was running (user: "the screen
  is dark (screensaver)") - it started during the ~2.5 min load after the reboot (davebench.sh kills it only before
  the launch). keyplay probe (new: "keyplay probe" logs the foreground owner; failures now log it too) saw foreground
  NULL = the screensaver's own desktop. (My first check with fgwin.ps1 via userrun reported cmd.exe - that was
  userrun's own cmd /c console, a self-inflicted reading; tools/phone/games/fgwin.ps1 kept, but run keyplay probe
  hidden instead.) MS Learn SetForegroundWindow / InjectTouchInput read (no UIPI text on InjectTouchInput; "The injected
  input is sent to the desktop of the session where the injection process is running").
  Fix in davebench.ps1: SPI_SETSCREENSAVEACTIVE FALSE with fWinIni 0 for the run (SystemParametersInfo page: "zero if
  you do not want to update the user profile"), back to TRUE at the end; scrnsave killed at the start and before each
  segment. SetThreadExecutionState page: "This function does not stop the screen saver from executing". Caveat: a run
  killed by davebench-stop.ps1 leaves screen saving off until the next logon (not persisted).
  tools/phone/games/proctree.ps1 (new): process tree by name/pid.
- davebot (user 2026-10-20: "figure out how to play the game ... check the screenshots"): tools/phone/games/davebot/
  davebot.ps1 = ONE hidden long-lived process in the user session (started by davebot-start.ps1 via userrun, which
  returns at once) - screenshots in-process (no cmd window in front), input through keyplay, screensaver off for the
  session. bot.sh start|shot|key|tap|launch|log|quit on the PC. Walked through by hand with the user's hints:
  title -> tap Continue (760,493); boat -> D (scan 20; the arrow key did nothing) until the yellow DIVE prompt
  (640,490 ffe025), hold Space 3 s; dive loading screen = flat 2e2e2e; water = O2 gauge fill 00aeef at
  (185,628)/(230,628)/(207,675). Signatures from sample.ps1 over traces/games/bot/*.png. auto.req (Auto) plays the
  whole route by state; swim pattern D 4 s / S 1.5 s / A 4 s / S 1.5 s (SwimRec, swim.py).
- First bot measurement (manual route, swim 100 s, EC 0171): traces/games/bench-bot1-0171.*: 2639 frames / 120.5 s,
  mean 21.9 fps, median 41.9 ms, p95 63.9, p99 102.9, max 2605 ms, 1 % low 3.2 fps, 0.1 % 0.5 fps (route differs from
  the davekeys2 runs, not comparable). 28 spike lines (> 100 ms). Kinds:
  (a) shader creation at first use: SetBlendState 100.6 ms, CreatePixelShader 36.8 / 58.0, DrawIndexed:ResolveState
      92.7 (variant compile at draw) - hitches when new things appear.
  (b) ResourceMap / Map:vb:discard 45-118 ms - a WRITE_DISCARD map that waits.
  (c) pan:sub:tex:read_rsrc up to 118.8 ms (= nearly all of pan:sub:textures; update_view is not in the top 10) -
      the writer-batch submit inside read tracking blocks; in the 160 ms spike it overlaps Map:vb:discard 117.5
      (render thread waiting on the driver thread that is stuck in that submit).
  (d) 0.95 / 1.16 / 2.6 s frames where the UMD sections are a small part -> outside the UMD (game / KMD / present).
  Next: where the submit blocks (ioctl report SUBMIT max in the tlog), then fix (b)/(c); (a) = shader cache.
- Ioctl report of the bot1 run (tlogq.ps1, new; tlog pid 7672), per 120 presents: SUBMIT (nr 0x40) 5880 calls
  ~900 ms (49/frame, ~7.5 ms/frame), MAP (0x100) 7964 calls 580-740 ms (66/frame, ~6 ms/frame) + MMAP_BO (0x43) 7965 +
  munmap (0x101) 7966, CREATE_BO (0x42) 46-149 calls 50-127 ms (~0.85 ms each -> the Map:vb:discard stalls are new BOs
  for replaced buffer storage, plausibly). Cause of the MAP churn (pan_bo.c): panfrost_bo_unreference munmaps every BO
  before parking it in the BO cache; reuse maps it again (2 shim calls + KMD MAP).
- Mesa 0172 (pan_bo.c, 64-bit Windows only, PAN_KEEP_CACHED_MAPS): cached BOs keep their CPU mapping, unmapped in
  panfrost_bo_free (covers evict_all / stale eviction / purged fetch); safe because the shim's MADVISE never discards
  pages (retained = 1). Also: a failed munmap logs instead of abort() on Windows - candidate for the exit-time
  fast-fail 7 crash (unverified: needs the LocalDumps dump). EC build s10e-artifacts\mali-umd-0172-ec 071cf1e3.
- bot2 (first fully automatic run, EC 0171): title tap (keyplay "fixed 1, failed 1" but it worked) -> loading 49 s ->
  boat -> 2 D steps -> DIVE prompt -> Space -> loading; the route works by state.
- bot2 = baseline for the automatic route, EC 0171 (traces/games/bench-bot2-0171.*): 2399 frames / 101.9 s, mean
  23.5 fps, median 41.6 ms, p95 52.9, p99 64.5, max 176.0, 1 % low 10.4 fps, 0.1 % 6.6 fps, 11 frames > 100 ms; swim
  ticks 10, stuck 0 (diff 5 at two ticks - the < 4 threshold is close, unverified), not-in-water 0. Spikes 110-125 ms
  with the UMD sections adding to only ~30 ms; one 176 ms = DrawIndexed:ResolveState 88 + CreatePixelShader 35 (shader
  compile at first use).
- EC 0172 071cf1e3 live (ecswap; prev e2eee16d = .pre-0172-0); d3dmrt64x 7/7, d3dbc64x lossy 0/126 PASS. bot3 started.
- Shader-compile hitches (spike class a): the build has -Dshader-cache=disabled, and upstream meson.build refuses it on
  Windows ("Shader Cache does not currently work on Windows"). A disk cache would remove the first-use compiles on every
  later run; it needs a port of util/disk_cache_os.c's file cache to Windows - recorded as a later item, not started.
- bot3 (EC 0172, auto route; traces/games/bench-bot3-0172.*): 2369 frames / 103.0 s, mean 23.0 fps, median 41.2 ms,
  p95 56.1, p99 76.6, max 1422.9, 1 % low 6.0, 0.1 % 1.3, 14 > 100 ms; swim not-in-water 2 ticks at the end (route
  not clean). 0172 works as meant - tlog pid 6908: MAP (0x100) 3-19 calls per 120 frames (was 7964), MMAP_BO 3-19 -
  but the frame time did not move (median 41.2 vs 41.6): the maps were not on the critical path. A MAP of a NEW BO
  now costs 15-20 ms each (19 calls 294 ms) = the w_lock of a fresh allocation.
  Per frame (tlog app frames): 43-49 ms, inside the UMD 18-21.6 ms (42-44 %); top: Flush 1 call/frame 8.8 ms (the app
  thread waits for the driver thread: no TC create_fence, and runtime callbacks - pfnRenderCb - run on the app thread
  via the ddi pump, notes/perf-threaded-context.md), shader_state_fs 8 ms (tex:read_rsrc 1270 calls/frame), prepare_draw
  4.7, get_batch_slow 4.4 (49 calls = the batch submits), SUBMIT ioctl 49/frame ~7.8 ms.
  The rest of the frame (~25 ms) is the game itself (x64 Unity under emulation) - not ours.
- Plan 0173 (shim submit queue): SUBMIT / malikm_submit_pair append to a pending queue (copied submit + BO list,
  queued mark on the BOs); every other shim entry (wddm_lock_acquire) flushes it first unless the call is whitelisted
  (SUBMIT, MADVISE, GET_PARAM, CREATE_BO, MMAP_BO, GET_BO_OFFSET, MAP; WAIT_BO only when its BO is not queued); queue
  cap; Panfrost's panfrost_flush drains it at its end. One Render per flush instead of per batch. Estimate from the
  0167 numbers (a merged chain saves ~2/3 of a Render): ~3-4 ms/frame of the app thread.
- Mesa 0173 (shim submit queue, as planned above): malikm_drm.c wq_* - wq_submit validates (jc, BO handles) and queues
  copies; wq_flush = one wddm_submit_jobs for all queued chains (clears the BOs' queued marks); wddm_lock_acquire =
  raw acquire + flush unless the thread set wq_skip (wddm_ioctl: MAP/UNMAP, SUBMIT, MADVISE, GET_PARAM, CREATE_BO,
  MMAP_BO, GET_BO_OFFSET, WAIT_BO; malikm_submit_pair); WAIT_BO on a queued BO: poll -> EBUSY, blocking -> flush then
  wait; caps WQ_MAX 16 chains, wcmd_size, and the queued BO-handle total <= walloc_size (a merged list over the
  allocation list would fail E2BIG and lose the work); queue tied to its binding (flush on a binding change);
  pan_context.c panfrost_flush calls malikm_submit_flush at its end. MALIKM_SUBMIT_QUEUE=0 = old behaviour
  (diagnosis only). tlog "submit queue: flush N, M chains (X per Render)". EC 503c9931 live (prev 071cf1e3 =
  .pre-0173-0); d3dmrt64x 7/7, d3dbc64x lossy 0/126 PASS. bot4 started.
  Review of 3dfe0ec (0173): lock acquisitions outside wddm_lock_acquire - only malikm_ddi_pump's T1c
  TryAcquire + mk_drain(1) (deferred destroy/unlock of the driver thread): those come from GEM_CLOSE / unlock calls
  that went through wddm_ioctl, which flushed the queue first, so a destroy always follows the Render that names the
  BO. Known semantic change: a Render failure of a queued chain is only logged ("submit queue: N chains failed"),
  Panfrost already saw success. wq_n read without the lock in malikm_submit_flush: same thread as the submits.
- bot4 (EC 0173): HUNG after 21.6 s (566 frames, swim diffs 0, "stuck 7"). Before the hang the queue worked: tlog pid
  5200 "submit queue ... 9.2 per Render on average", frames 36.7-41 ms with the UMD at 12.4-14 ms/frame (was 18-21).
  Hang: 14x SetError 0x887b0001 (WASSTILLDRAWING - all from QueryGetData, the only source in the UMD: Unity polls
  event queries) then "tc: driver thread waits > 2 s for the DDI thread to run callback op 4" (MK_RENDER). Bug: a
  poll (WAIT_BO timeout 0) on a queued BO returned EBUSY without sending the queue, and a query "not ready" never
  made the queued work go out -> the work the game waits for stays queued.
- Mesa 0174: WAIT_BO on a queued BO flushes also when polling; QueryGetData's two WASSTILLDRAWING returns call
  malikm_submit_flush. EC 38cde172.
- bot5 (EC 0174): hung during the save load right after Continue (state "loading" 12 min, auto FAIL): tlog pid 6552
  ends with 16x WASSTILLDRAWING, the 120-present report, 2 more, then "tc: driver thread waits > 2 s for the DDI
  thread to run callback op 4" again - the poll fix was not the (whole) cause. The DDI-thread snapshot that follows
  that line is x86-only (nothing logged for the EC UMD).
- Mesa 0175 (diagnosis): mk_snapshot_ddi_thread for x64 / ARM64EC (Rip/Rsp, module+offset stack scan - our process,
  module names only), and the > 2 s line now says when the DDI thread last entered a DDI (mk_ddi_tick) and the queue
  length. EC 71966a35 live; d3dmrt64x 7/7, d3dbc64x lossy 0/126. Next: rerun to catch the hang with the stack.
- bot6 (EC 0175 = queue + poll fix + snapshot): completed, no hang (traces/games/bench-bot6-0175.*): 2429 frames /
  103.0 s, mean 23.6 fps, median 40.4 ms, p95 57.1, p99 82.0, max 528.8, 1 % low 8.0, 0.1 % 2.7, 13 > 100 ms; first
  60 s 25.0 fps / median 38.6 ms; swim not-in-water 4 at the end. tlog pid 2100: 9.3-9.8 chains per Render, the UMD
  10.8-14 ms per frame (bot3 18-21.6) - ~6 ms less on Unity's render thread, but the frame stays ~40 ms (median 40.4
  vs 41.6): the render thread is no longer the limit. Next: per-thread CPU (threadcpu.ps1, new) + GPU engine load
  (tools/phone/gpuengine.ps1) during the swim to see whether the game's own thread or the GPU bounds the frame.
  The hang of bot4/bot5 did not recur in 1 run (0174 fixed polls; unverified that it is gone).
- bot7 (EC 0175; traces/games/bench-bot7-0175.*): 2519 frames / 101.5 s, mean 24.8 fps, median 38.7 ms, p95 59.7,
  p99 83.0, max 185.9, 1 % low 9.1, 0.1 % 5.4, 14 > 100 ms (best so far). Mid-swim samples (20 s into the swim):
  threadcpu.ps1: process 277 % of one core (6 cores), top thread 88.1 % (game main thread, presumably), next 56.7 %,
  then 32 / 25 / 22 / 17 / 16 %; gpuengine.ps1: DaveTheDiver 3D engine 92.13 %, copy 0 %. => the GPU is now the
  bottleneck (92 %), with the game's main thread close behind (88 %). Clocks stay as they are (memory: no GPU clock
  increase before Windows thermal reading + throttling exist). Next: less GPU work per frame (AFBC, tile
  load/store per batch, per-chain GPU time from the KMD).
- GPU-bound -> less GPU work. d3d10umd had pfnDiscard = generated stub (no-op, untimed - invisible in the call counts);
  Panfrost preloads an RT's old contents whenever a batch draws to it and the level is valid (pan_job.c), and its
  invalidate_resource only dropped the current batch's store.
- Mesa 0176: D3D11Tiers.inc Discard (11.1 / WDDM 1.3 / 2.0 tables): whole-resource discards (no rects; RTV/DSV only
  when covering a single-level single-layer texture; not buffers) -> pipe->invalidate_resource (TC-queued);
  Panfrost panfrost_invalidate_resource (Windows) also zeroes the valid bits (not for AFBC) -> no preload next time.
  LOG_ENTRYPOINT, so "Discard" shows in the call counts (does Unity call it at all?). EC 01a005f7 live; d3dmrt64x 7/7,
  d3dbc64x lossy 0/126 PASS.
- bot8 (EC 0176): hung in the save load again; 0175's snapshot caught it (tlog pid 4456): "driver thread waits > 2 s
  ... op 4 (DDI thread last seen 2500 ms ago, submit queue 0)"; DDI thread stack (scan, symbolized with the 0176 PDB):
  ResourceMap (Map:vb:discard) -> tc_buffer_map -> tc_improve_map_buffer_flags -> tc_invalidate_buffer ->
  panfrost_resource_create_with_modifier -> panfrost_bo_create -> simple_mtx_lock -> futex_wait (KERNELBASE wait).
  => deadlock: the DDI thread waits for a Panfrost BO lock (cache) that the driver thread holds while the shim makes
  it send a Render that only the DDI thread can run. With 0173 any non-whitelisted shim entry flushed: Panfrost's
  cache put -> stale eviction -> GEM_CLOSE under the cache lock -> flush -> Render. ("queue 0": wq_flush zeroes wq_n
  before submitting.) 0174's flush on a WAIT_BO poll had the same hazard (cache fetch polls cached BOs, which may still
  be queued, under the cache lock). bot4/bot5 were most likely this too, not the polls.
- Mesa 0177: GEM_CLOSE + the no-op syncobj calls (CREATE/DESTROY/RESET/SIGNAL/IMPORT_SEQ) never flush; GEM_CLOSE of a
  queued BO is deferred and done in wq_flush after the submit; WAIT_BO poll on a queued BO -> EBUSY without flushing
  (blocking waits still flush); QueryGetData's flush (0174, DDI thread) stays. Flush points now: queue full,
  blocking WAIT_BO, SYNCOBJ_WAIT, EXPORT_SEQ, panfrost_flush end, query not ready, other entries (present etc.).
  EC 54b378d8 live; d3dmrt64x 7/7, d3dbc64x lossy 0/126 PASS. Also found: C:\S10eTmp\dumps has the LocalDumps of the
  12:08 / 12:23 / 12:28 crashes (exit aborts) - to read later.
- bot9 (EC 0177; traces/games/bench-bot9-0177.*): completed, clean route (stuck 0, not-in-water 0): 2429 frames /
  102.3 s, mean 23.7 fps, median 39.3 ms, p95 53.7, p99 75.7, max 3328.5, 1 % low 3.9, 0.1 % 0.5, 12 > 100 ms. No
  hang. The 3.3 s frame = pan:sub:get_batch_slow 3229.6 ms (Panfrost out of batch slots -> submits the oldest -> the
  shim's Render waits for the DDI thread, which did not enter a DDI for ~3 s) - same marshalling stall class as the
  1-2.6 s frames of bot1 (before the queue), not new. tlog pid 4032: no "Discard" anywhere - Unity does not call
  DiscardView here, 0176 is inert for Dave (kept: correct for apps that do).
  Summary of the automatic-route runs (median / mean): bot2 0171 41.6 ms / 23.5 fps; bot3 0172 41.2 / 23.0;
  bot6 0175 40.4 / 23.6; bot7 0175 38.7 / 24.8; bot9 0177 39.3 / 23.7 - run-to-run spread ~2 ms, so 0172-0177 gave
  ~1-2 ms (render-thread UMD time -6 ms/frame, but the GPU is at 92 % and the game's main thread at 88 %).
- Mesa 0178 (diagnosis): pan_job.c panfrost_batch_submit, Windows: per 2000 batches a tlog line "pan batches 2000:
  draws, batches without draws, fb MP, colour preload / clear / store MP, zs preload / store MP, msaa". EC eaee8f31
  live; d3dmrt64x 7/7, d3dbc64x lossy 0/126. bot10 next (counters + numbers).
- bot10 (EC 0178; traces/games/bench-bot10-0178.*): 2519 frames / 102.1 s, mean 24.7 fps, median 38.9 ms, p95 53.5,
  p99 75.6, max 299.4, 1 % low 8.6, 0.1 % 3.6, 11 > 100 ms, clean route. Counters (tlog pid 1056), per 2000 batches
  (~41 frames): ~16700 draws, 0 empty batches, fb 1094 MP, colour preload 775 / clear 0 / store 775 MP, zs preload
  595 / store 552 MP, no MSAA. Per frame ~19 MP colour + ~14.5 MP depth PRELOADED and as much stored - roughly
  250 MB/frame of tile traffic (~6 GB/s at 25 fps) - and NOT ONE batch clear.
  Cause: d3d10umd ClearRenderTargetView / ClearDepthStencilView -> pipe->clear_render_target / clear_depth_stencil;
  Panfrost implements those with u_blitter quads (pan_resource.c), i.e. a batch that preloads then overwrites.
- Mesa 0179: a D3D clear of a currently bound view (same texture / level / layers / format as the fb attachment) with
  the framebuffer exactly the view's size -> pipe->clear (Panfrost panfrost_clear: free batch clear while the batch
  has no draws, quad otherwise); else the old path. EC bdc71b00 live; d3dmrt64x 7/7, d3dbc64x lossy 0/126 PASS.
- bot11 (EC 0179): the game died ~1 min in (auto FAIL game gone). WER: maliumd_ec.DLL c0000005 at 0xeb030 =
  tc_clear (u_threaded_context.c:4952 "p->color = *color") - 0179's depth/stencil fast clear passed color NULL; the
  threaded context copies it unconditionally. Regression gap: d3dmrt64x / d3dbc64x never clear a bound DSV (they
  passed). LocalDumps worked: C:\S10eTmp\dumps\DaveTheDiver.exe.7108.dmp.
- Mesa 0180: a zeroed pipe_color_union instead of NULL. EC 8b139429 live; d3dmrt64x 7/7, d3dbc64x lossy 0/126.
- bot12 (EC 0180): 2729 frames / 102.7 s, mean 26.6 fps, median 36.3 ms, p99 63.7 - BUT the dive screenshot at the
  water-state moment (traces/games/bot/bot12-end.png) shows only the HUD on a WHITE scene; bot6 (0175) at the same
  moment shows the full dive scene -> 0179/0180 break rendering; the speed-up is not valid. Live EC rolled back to
  0178 (eaee8f31, ecswap tag 0178r). (Checked: a clear-only batch still marks its attachments valid in
  emit_fragment_job, so "no preload after a clear-only batch" is not the cause.)
- Mesa 0181 (diagnosis): C:\S10eTmp\fastclear.txt mask (bit 0 colour, bit 1 depth/stencil fast clear; no file = 3) -
  a file because Steam relaunches the game with its own environment. EC cbf627a3 live with mask 1 (colour only).
- bot13 (EC 0181, fastclear.txt = 1: colour fast clears only, 60 s swim): renders correctly (bot13-colour-end.png =
  the full dive scene) and 1709 frames / 61.2 s, mean 27.9 fps, median 34.6 ms, p95 45.2, p99 59.2, 6 > 100 ms
  (compare bot6 first 60 s: 25.0 fps, median 38.6). => the depth/stencil fast clear broke the image.
- Mesa 0182: depth/stencil fast clear only when the D3D flags clear every aspect of the format (depth+stencil, or
  depth on a depth-only format) - a depth-only clear of a depth+stencil target (stencil preloaded beside a depth
  clear) is the suspect. EC 51f48673 live, no fastclear.txt (mask 3); d3dmrt64x 7/7, d3dbc64x lossy 0/126.
- bot14 (EC 0182, both fast clears): RENDERS CORRECTLY (bot14-end.png, a kelp-forest layout this time) and 2609
  frames / 102.2 s, mean 25.5 fps, median 38.4 ms, p95 47.5, p99 59.2, max 164.0, 1 % low 11.3, 0.1 % 7.0 (best lows
  so far; layouts differ between runs, so medians are noisy). Counters (tlog pid 4880) per 2000 batches: colour preload
  378 MP (was 775), clear 270, store 648; zs preload 514 (was 595), store 560; ~49 batches without draws (clear-only
  batches: the clear's framebuffer changed before any draw - written out and preloaded again). UMD 10.9-11.8 ms/frame.
- Mesa 0183 (diagnosis): OutputMerger.cpp ClearStat - every 500 clears a tlog line "clears 500: Clear:rt:fast n,
  Clear:rt:slow:unbound|size|off n, Clear:zs:fast n, Clear:zs:slow:unbound|depth-only|stencil-only|size|off n".
  EC a7a19fb6 live; d3dmrt64x 7/7, d3dbc64x lossy 0/126.
- bot15 (EC 0183, 60 s swim): 1559 frames / 61.5 s, mean 25.3 fps, median 38.0 ms, p99 70.6, renders right.
  Clear paths (tlog pid 1480), per 500 clears: Clear:rt:fast 200, Clear:zs:fast 67 (depth+stencil), Clear:zs:slow:
  stencil-only 234; no depth-only, no unbound / size misses. So 0180's white dive came from STENCIL-ONLY fast clears
  (the only partial kind present). Likely mechanism (not verified in Panfrost's code path end to end): with a combined
  depth+stencil format a stencil-only batch clear leaves depth unread (the preload test needs draws/read of that
  aspect), and writing the stencil back writes the whole ZS buffer, depth included, from an uninitialised tile ->
  depth testing fails afterwards. A correct stencil-only fast clear would still have to preload depth, so it would save
  little; it stays on the u_blitter path (0182). The remaining zs preload (~514 MP / 2000 batches) is mostly that.
- State after the clear work: colour fast clears halve the colour preloads; frame medians 34.6-38.4 ms depending on the
  map layout (bot13/14/15), lows best so far in bot14 (1 % 11.3 fps, max 164 ms).
- Mesa 0184 (diagnosis): the batch counter line also gives colour attachments (AFBC / linear) and zs (AFBC) - is AFBC
  (framebuffer compression, ~half the tile traffic) used for Dave's render targets? EC 0f3426a0 live; d3dmrt64x 7/7,
  d3dbc64x lossy 0/126.
- bot16 (EC 0184, 60 s): 1589 frames / 60.9 s, mean 26.1 fps, median 37.4 ms, p99 62.6, max 116.5, 1 % low 13.3,
  0.1 % 8.6 (best lows), renders right. Counters (tlog pid 7464) per 2000 batches: colour attachments 1513 - AFBC 244,
  linear 49, the rest tiled; zs 829 - AFBC 0. AFBC works (has_afbc) but Dave's targets are mostly formats Panfrost
  does not compress on v7: scene r11g11b10_float + z32_float_s8x24 (pan_resource.c panfrost_should_afbc: "We can't do
  AFBC(Z32)+S8 on v7-"), bloom r11g11b10_float, rgba16f / r16_unorm / r32_float (framecap-day-black.txt). The game's
  format choice - no native driver lever. Remaining bandwidth: stencil-only clears (quad, depth preloaded),
  ~1.2 clear-only batches per frame, depth stored after every batch (D3D gives no "don't need" hint).
- Exit-time abort solved (dump C:\S10eTmp\dumps\DaveTheDiver.exe.6772.dmp, 12:28, EC 0169; cdb x64 on the PC +
  llvm-symbolizer over a raw stack scan, our DLL only): the crashing thread's stack holds the CRT's "Assertion
  failed: %Ts, file %Ts" and the wide string L"!\"invalid boolean bit-size\"" = pan_nir_lower_bool_to_bitsize.c
  get_bool_convert_opcode UNREACHABLE -> assert -> abort() -> fast-fail 7. The UMD builds are debugoptimized, so asserts
  are on. Which bit size: unknown (Bifrost v7 lowers all int64 - lower_int64_options ~0 - so a 64-bit bcsel should not
  get there; my first guess "64-bit bcsel" was wrong and reverted before building).
- Mesa 0185 (Windows): the bool resize handles the two missing NIR sizes instead of aborting - 64 -> i2i64, 1 ->
  ine(x, 0) in resize_bool_alu_source - and logs the first occurrence (mesa_logw -> the game's -stderr tlog; the file is
  also in the host panfrost_compile, which has no malikm shim). (First build failed: no nir_op_i2b1 in this NIR.)
  EC fdfdba20 live; d3dmrt64x 7/7, d3dbc64x lossy 0/126.
- bot17 (EC 0185): renders right (bot17-end.png; the same coral layout as bot6), 2639 frames / 102.0 s, mean 25.9 fps,
  median 36.9 ms, p95 52.6, p99 71.1, max 561.4, 8 > 100 ms. Same layout as bot6 (0175: 23.6 fps / median 40.4) ->
  the fast clears give ~-3.5 ms per frame (~9 %). Overall on this layout since bot2 (0171): 41.6 -> 36.9 ms median,
  23.5 -> 25.9 fps.
- CORRECTION (review of the fastclear.txt runs): `echo 1> C:\S10eTmp\fastclear.txt` over ssh/cmd is "echo" with
  handle 1 redirected - the file got "ECHO is on.", fscanf failed, mask = 3. So bot13 ("colour only") really ran
  0180's behaviour (colour + all depth/stencil fast clears, stencil-only included) and rendered RIGHT; bot12 (0180)
  rendered white once. The conclusions "the depth/stencil fast clear broke it" / "stencil-only caused the white dive"
  are NOT established - one white capture vs one good one with the same behaviour. 0182 (full-aspect zs only) stays
  the safe default. Flag files are now written with PowerShell Set-Content.
- Mesa 0186 (diagnosis): fastclear.txt bit 2 = stencil-only fast clears too (default off). EC 1a7169cc live; mask 7
  for the next run (several runs needed: the white capture may be intermittent).
- bot18 (EC 0186, mask 7 really set): renders right (bot18-stencil-end.png, dense coral layout), 1559 frames / 62.0 s,
  mean 25.1 fps, median 38.6, p99 61.7. Counters (pid 1440): Clear:zs:fast 100 / 500 (was 67), "stencil-only slow"
  ~200 / 500 (0183's label chain is order-dependent - those were not rejected for being stencil-only), 0 batches
  without draws (was 49). Stencil-only fast clears of bound views: no visible damage in this run.
- Mesa 0187: unbound / differently sized views -> ClearViaTempFb (framebuffer with only that view, pipe->clear,
  device fb restored): a clear-only batch that writes the target once instead of u_blitter's preload+quad+store
  batch; fastclear.txt bit 3 (colour needs bits 0+3, zs bits 1+3); independent reason labels. EC 2ad2723a live, mask
  15; d3dmrt64x 7/7, d3dbc64x lossy 0/126 (with mask 15 in place).
- bot19 (EC 0187, mask 15): renders right; 2459 frames / 103.1 s, mean 23.8, median 38.5, p99 97.3, 24 > 100 ms - but
  no "tempfb" clear happened at all (counters pid 3772: rt:fast 200, zs:fast 100, zs:slow:stencil-only 200 per 500),
  so 0187 changed nothing here and the worse p99 is run-to-run noise (this route's spread is large). With the new
  independent labels, the 200 slow ones fail the aspect test, not "unbound": hypothesis - D3D clears with DEPTH|
  STENCIL on stencil-less depth buffers (flags != the format's aspects).
- Mesa 0188: flags &= the format's aspects before the fast-path tests (D3D clears nothing there anyway). EC de11bcf8
  live (mask 15); d3dmrt64x 7/7, d3dbc64x lossy 0/126. Next run must show zs:fast up / stencil-only gone.
- bot20 (EC 0188, mask 15): hypothesis confirmed - clears 500: Clear:rt:fast 200, Clear:zs:fast 300, nothing slow.
  Renders right (bot20-end.png, the kelp layout of bot14). 2669 frames / 102.1 s, mean 26.1 fps, median 37.0 ms, p95
  47.2, p99 60.5, max 178.3, 1 % low 11.5, 0.1 % 6.6, 7 > 100 ms (bot14, same layout, 0182: 25.5 / 38.4 / 59.2).
- Mesa 0189: default fast-clear mask 7 without the flag file (colour + full zs + stencil-only: rendered right in bot18
  and bot20); bit 3 (temp-fb clears) stays off - never exercised by Dave. fastclear.txt removed from the phone. EC
  f5b4e506 live; d3dmrt64x 7/7, d3dbc64x lossy 0/126.
- bot21 (EC 0189, defaults): 2609 frames / 103.3 s, mean 25.3 fps, median 37.9, p99 73.8, 7 > 100 ms. Counters
  (bot20, pid 6912): zs preload 387 MP / 2000 batches (was ~495), but ~310 / 2000 batches are clear-only now (Unity
  clears depth, then switches framebuffers before drawing: the clear is stored, the next pass preloads it). Mid-swim:
  process 338 % of a core; top thread 94.2 % (prio 9), next 64.4 % (prio 8), 41 / 36 / 31 / 26 %; GPU 3D 89.7 %.
  => two limits at once: one thread at ~94 % and the GPU at ~90 %. threadcpu.ps1 now prints thread names
  (GetThreadDescription) to tell Unity's threads from Mesa's.
