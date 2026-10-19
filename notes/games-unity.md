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
