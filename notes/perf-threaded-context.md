# Threaded context for the Mali UMD (Panfrost + d3d10umd) - NFSU2 performance step 2

User (2026-10-18): "what else can we do? add threading to something else? split the work?" -> "continue with the
threading you were doing". Goal: run Panfrost (draw building, ~53 ms of NFSU2's ~84 ms frame on the game thread) on a
second core via Mesa's u_threaded_context; the game thread only enqueues. Expected bound ~84 -> ~55 ms (the driver
thread is still emulated x86 in 32-bit games); also helps every D3D app (DWM, browsers) on ARM64.

## Facts (2026-10-18)
- Panfrost in this tree has no threaded_context (grep: no threaded_context_create / replace_buffer_storage); drivers
  that do: crocus, iris, freedreno, radeonsi, zink, d3d12. d3d10umd creates `screen->context_create(screen, NULL, 0)`
  (Device.cpp:178) and is pure Gallium (no casts to panfrost_context).
- u_threaded_context.h contract: subclass threaded_resource / threaded_transfer / threaded_query; unsynchronized
  buffer maps arrive in the APP thread with TC_TRANSFER_MAP_THREADED_UNSYNC; driver must honour
  TC_TRANSFER_MAP_NO_INFER_UNSYNCHRONIZED and TC_TRANSFER_MAP_NO_INVALIDATE (no own DISCARD reallocation - TC does it
  with replace_buffer_storage, which the driver must implement); screen functions taking a ctx unwrap with
  threaded_context_unwrap_sync; without a create_fence callback flushes are synchronous (tc_sync, then
  pipe->flush in the CALLING thread).
- winsys include/win32-compat/malikm_drm.c: when a runtime device exists (`ddi.on`, every D3D app) all BO create /
  destroy / lock / unlock / residency / submit go through runtime callbacks: pfnAllocateCb x7, pfnDeallocateCb,
  pfnLockCb x5, pfnUnlockCb x3, pfnMakeResidentCb, pfnCreatePagingQueueCb, pfnRenderCb, pfnSignalSynchronization-
  Object2Cb, pfnEscapeCb. State under wddm_lock (SRW) etc.
- MS Learn rules (AGENTS 13):
  - https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_setasynccallbackscb :
    "DirectX 10 and later versions of the runtime set the pfnSetAsyncCallbacksCb member ... to NULL" and "Drivers can
    call the runtime's callback functions only from a worker thread if the runtime previously set the
    pfnSetAsyncCallbacksCb member of D3DDDI_DEVICECALLBACKS to non-NULL".
  - https://learn.microsoft.com/windows-hardware/drivers/display/changes-from-direct3d-10 : free-threaded rules apply
    only after D3D11DDICAPS_FREETHREADED, "otherwise, the previous heavily restricted rules apply"; "Only a single
    thread can work against an HCONTEXT at a time"; "The driver must call pfnRenderCb on the thread that called the
    driver's Flush(D3D10) function".
  => The Panfrost worker thread must NEVER call a runtime callback itself.

## Design
1. Callback marshalling in the winsys: the thread that enters the DDI ("DDI thread") is recorded per device entry.
   A runtime-callback wrapper running on another thread posts the call into a one-slot mailbox and waits; the DDI
   thread executes it (a) at every DDI entry, (b) while it waits for the worker (TC sync / fence waits) and (c) while
   it waits for a winsys lock (lock helper = TryAcquire + pump, so a worker that holds wddm_lock while marshalling
   cannot deadlock the DDI thread). All runtime callbacks thus run on the DDI thread, one at a time.
   Expected rare on the worker: flushes are synchronous (TC calls pipe->flush and synchronized maps on the app
   thread); the worker only submits on batch-slot eviction / job limits / blits and allocates on pool growth / BO
   cache misses.
2. Panfrost: panfrost_resource gets `union { struct pipe_resource base; struct threaded_resource tc; }` (tc.b aliases
   base, no rename of rsrc->base) + threaded_resource_init/deinit; panfrost_transfer gets threaded_transfer;
   panfrost_query threaded_query.
3. Panfrost maps: THREADED_UNSYNC -> separate transfer slab (transfer_pool_unsync), no ctx state touched;
   NO_INVALIDATE -> skip the DISCARD new-BO/shadow path; NO_INFER_UNSYNCHRONIZED -> no valid-range unsync.
   replace_buffer_storage: dst takes src's BO (+ plane base), old BO unreferenced, then
   panfrost_dirty_state_for_rsrc (0131) + batch BO tracking checked.
4. panfrost_create_context: wrap with threaded_context_create when PIPE_CONTEXT_PREFER_THREADED; d3d10umd asks for it
   (MALIUMD_NO_TC=1 = diagnosis switch only, AGENTS 12) and pumps the mailbox at DDI entry.
5. Validation: x86 d3d test apps, NFSU2 automated race (frame time, correctness screenshot), then ARM64/EC + DWM.

## Tasks (one commit each)
- T1 winsys mailbox + DDI-thread pump + lock helper (inert without a worker).
- T2 Panfrost threaded_resource/transfer/query subclassing (no behaviour change).
- T3 Panfrost map flags + replace_buffer_storage.
- T4 TC wrap + d3d10umd flag/pump; x86 build.
- T5 device tests (x86 apps, NFSU2 race), then ARM64/EC + DWM.

## Progress log
- T1 (Mesa f4f3b33): tools/mesa-build/tc/t1_marshal.py applied. malikm_drm.c: w_escape/create/resident/lock/
  render/destroy/unlock -> *_raw + wrappers; on a driver thread (TLS mk_worker, set via tc_driver_thread_hook) with
  ddi.on: result calls posted to a one-slot mailbox (mk_call, waits on an auto-reset event, logs if > 2 s),
  destroy/unlock queued (mk_defer_op); malikm_ddi_pump runs them on a DDI thread; DDI-thread direct calls drain the
  queue first (program order). 20 wddm_lock acquisitions -> wddm_lock_acquire (TryAcquire + pump + yield on DDI
  threads). u_threaded_context.c: tc_driver_thread_hook / tc_wait_hook (NULL = upstream), tc_fence_wait (pump + 1 ms
  timed waits) in _tc_sync, renderpass waits, and before util_queue_add_job (slot next+2 = oldest queueable job, so
  the ring-full block also pumps). Exports: malikm_marshal_enable, malikm_mark_worker_thread, malikm_ddi_pump,
  malikm_marshal_report. x86 build OK.
- T1 review: grep - only the helper acquires wddm_lock directly; no *_raw caller outside mk_exec/wrappers; lock audit
  (above) - blocking marshals happen under wddm_lock + submit_lock / per-shader locks only, frees under
  bo_map_lock / bo_cache.lock / handle_to_bo.lock are deferred (never block). FINDING -> T1b: malikm_primary_create,
  shared_create, set_display_mode, open_shared, close_shared, shared_bo_create call ddi.cb directly (app thread: OK)
  but without mk_direct() -> queued destroy/unlock could run after them (order). Also known limits: tc_fence_wait's
  1 ms timed wait may round up to the timer tick (15.6 ms) when a callback is pending during a sync - measure
  (malikm_marshal_report counts).
- T1b (Mesa 4416b63): mk_direct() after wddm_lock_acquire in malikm_primary_create, shared_create, set_display_mode, open_shared, close_shared, shared_bo_create (the six direct ddi.cb users, all on DDI threads: d3d10umd CreateResource/Present/OpenResource; Panfrost resource_create runs on the app thread under TC). x86 build OK. Review: diff = 6 identical inserted lines, each after the function's own lock acquisition (python asserted inside the function body).
- T2 (Mesa d3e410e, tools/mesa-build/tc/t2_subclass.py): panfrost_resource/panfrost_transfer = anonymous union
  { base; threaded_resource/threaded_transfer tc } (tc.b aliases base, no renames); threaded_resource_init in
  resource_from_handle + create_with_modifier (the only two CALLOC_STRUCT(panfrost_resource)), deinit in destroy;
  panfrost_query starts with struct threaded_query (rzalloc zeroes it); transfers + queries rzalloc(NULL) instead of
  children of the context; create_query zeroes its result buffer via panfrost_bo_mmap + memset (this Panfrost does
  no CPU cache maintenance anywhere, same as the old pipe_buffer_write path) instead of pipe_buffer_write on the
  driver context. x86 build OK.
- T2 review: grep - no other panfrost_resource allocations, no whole-struct copies of resources (only image.planes
  memcpy). FINDING -> T2b: create_sampler_view (pan_cmdstream.c rzalloc(pctx, panfrost_sampler_view)) and
  create_stream_output_target (pan_context.c rzalloc(pctx, ...)) are directly-executed under TC (app thread) and
  still allocate under the context; plus audit every TC-direct function (create_*_state, create_sampler_view,
  sampler_view_destroy, stream_output_target_destroy, create_query, get_query_result(flushed), unsync buffer map)
  for context state use.
- T2b (Mesa 1c31f87, tools/mesa-build/tc/t2b_direct.py + link_shader guard): ctx->tc_pipe (NULL until T4) +
  panfrost_tc_sync() (threaded_context_unwrap_sync). Audit of the TC-direct functions (u_threaded_context.h list +
  tc_link_shader): rasterizer / DSA / blend / sampler state / vertex elements create = no context use (OK);
  create_sampler_view: own ralloc root, tc sync before legalize + descriptor from ctx->descs (rare); stream output
  target: own root; create_shader_state: xfb variant -> sync, default-variant precompile skipped under TC (bind ->
  panfrost_update_shader_variant compiles on the driver thread, pan_shader.c:411/449); create_compute_state -> sync;
  link_shader -> skipped under TC; get_query_result with tc.flushed -> no flush_writer / flush_all_batches (the TC
  flush was synchronous, batches submitted; BO wait is screen-level). x86 build OK.
- T2b review: sampler_view_destroy (direct) = BO unreference (atomic + device locks) + ralloc_free of its own root
  now - OK. Remaining TC-direct: resource_create/destroy (screen, device locks only), unsynchronized buffer_map -> T3.
- T3 (Mesa a1837b9, tools/mesa-build/tc/t3_maps.py): ptr_map - TC_TRANSFER_MAP_NO_INFER_UNSYNCHRONIZED skips the
  "uninitialized range -> UNSYNCHRONIZED" upgrade; TC_TRANSFER_MAP_NO_INVALIDATE forces create_new_bo/copy_resource
  off (joins the separate-stencil bail -> synchronized writes flush + wait); TC_TRANSFER_MAP_THREADED_UNSYNC (app
  thread) skips the index min/max cache invalidate (unmap, driver thread, invalidates the same box:
  pan_resource.c ~2512 verified). With TC flags the unsync path touches no ctx state (any_batch_reads / flush only
  under !UNSYNCHRONIZED). panfrost_replace_buffer_storage: BO swap as the DISCARD path (batches hold BO refs:
  panfrost_batch_add_bo_old -> panfrost_bo_reference), plane.base, dst index cache emptied,
  panfrost_dirty_state_for_rsrc (0131). Not yet registered (T4). x86 build OK (only pre-existing warnings).
- T3 review: TC_* bits are 1<<28..31, never set by d3d10umd itself (grep) - no change without TC. Open: delete_buffer_id
  ignored (only meaningful with is_resource_busy, not implemented -> every DISCARD of a busy buffer = new storage).
- T4 (Mesa c9615c7, tools/mesa-build/tc/t4_enable.py): panfrost_screen.transfer_pool (slab parent, panfrost_transfer
  size) created/destroyed with the screen; panfrost_create_context wraps with threaded_context_create(gallium,
  &transfer_pool, panfrost_replace_buffer_storage, NULL options, NULL) when PIPE_CONTEXT_PREFER_THREADED and sets
  ctx->tc_pipe. d3d10umd Device.cpp: hooks (tc_driver_thread_hook = malikm_mark_worker_thread, tc_wait_hook =
  malikm_ddi_pump), malikm_marshal_enable, flag; tlog "device: context %p threaded %d". Debug.cpp: malikm_ddi_pump at
  every outermost DDI entry. DxgiFns.cpp: "app tc callbacks: ..." line after "app ioctl top" (per 120 presents).
  MALIUMD_NO_TC=1 = diagnosis switch only. x86 build OK; DLL contains the new strings.
- T4 review: threaded_context_create accepts NULL options/out (checked); destroy goes tc -> driver; hooks are
  process-global (set once, idempotent). DEVIATION recorded: MS Learn "Supporting Multiple Processors" says a driver
  doing its own MP optimizations "should disable" them for windowed apps / single-core machines (fair sharing; text
  for the D3D9 runtime policy) - kept on for all apps on this 8-core phone; revisit if DWM/desktop power suffers.
- T5 run 1 (x86 tc1 = T4, f27d3848): every D3D11 process aborted in threaded_context_create:
  "Assertion failed: pipe->screen->caps.prefer_real_buffer_in_constbuf0". Upstream TC also takes NO user buffers:
  tc_set_constant_buffer "frontend must handle this" (UNREACHABLE), tc_set_vertex_buffers asserts !is_user_buffer.
  d3d10umd used a static float[4] user VB in unbound slots and user constant buffers in GsEmu.cpp (params) and
  VideoProc.cpp (CSC constants).
- T4b (Mesa dc4b908, tools/mesa-build/tc/t4b_userbufs.py): panfrost caps->prefer_real_buffer_in_constbuf0 = true (every
  TC driver sets it; d3d10umd does not read it); Device::dummy_vb (16 zero bytes, PIPE_USAGE_IMMUTABLE) in every
  unbound VB slot (stride 0 -> same zeros); GsEmu/VideoProc constants via u_upload_data_ref(pipe->const_uploader) +
  u_upload_unmap, reference dropped after set_constant_buffer. Review: GsEmu.cpp:69 skips stride-0 VBs (dummy) as
  before; no other user buffers in d3d10umd (grep has_user_indices/user_buffer/.user =).
- T5 run 2 (x86 tc2 = T4b, cbc607da): d3dvbring32 + d3dtri32 tri crash, AV maliumd32+0x61696a =
  u_transfer_helper_transfer_unmap/handle_transfer (llvm-symbolizer on libgallium_d3d10.pdb), read of a float-looking
  address. Diagnosis builds (tc2d/e/f): VEH x86 stack scan (return-address candidates -> tc batch_execute), then
  map/unmap tracing: transfer 0726D008 (the dummy-VB upload at device creation) unmapped ONCE by the app, but its
  queued unmap EXECUTED TWICE, all on the application thread. Cause: T2b panfrost_tc_sync is also reached from
  inside batch execution - u_blitter (the ResourceCopy blit) calls create_sampler_view on the driver context ->
  threaded_context_unwrap_sync -> _tc_sync re-executes the batch that is running (on the driver thread it would
  deadlock). Note: pipe_transfer.usage is a 24-bit field, the TC_* bits do not survive in transfer->usage.
- T5a (Mesa 5c4ba9b, tools/mesa-build/tc/t5_reentry.py): u_threaded_context.c thread-local tc_executing_depth around
  batch_execute in tc_batch_execute (driver thread + direct execution in _tc_sync), threaded_context_executing();
  panfrost_tc_sync syncs only when it is false. Mesa 70608e4: VEH x86 stack scan kept (tlog "  stack scan: ..." after an
  access violation; symbolize with llvm-symbolizer --obj=maliumd32.dll --pdb=libgallium_d3d10.pdb --relative-address).
- T5 run 3 (x86 tc3d = T5a + tracing): d3dtri32 tri PASS, d3dvbring32 PASS (all 9 patterns incl. discard), tlog
  "device: context ... threaded 1". These tests sync at every readback, so their queued calls run on the
  application thread (direct execution) - the driver thread proper is exercised by the game run next.
- T5a review: depth is per thread, so a worker-thread blit and an app-thread TC-direct create cannot confuse each
  other; TC-direct calls made by the app OUTSIDE batch execution still sync (create_sampler_view etc.). Tracing
  removed (diff = T5a only, two stray blank lines removed).
- x86 tc3 (T5a, no tracing) in SysWOW64; automated race nfs-tc1 (user idle 517 s).
- nfs-tc1 (x86 tc3, a481e966): game alive 119 s but stuck on the title screen (scan-out traces/wddm2/nfs-tc1.png:
  title art, correct). tlog: "device: context threaded 1"; frames 120-480 at 9-36 ms (title: ~14 draws/frame),
  "app tc callbacks: ... render 58" in the first 120 frames (worker submissions marshalled), then the log stops
  after frame 480 and "tc: driver thread waits > 2 s for the DDI thread to run callback op 4" (render) - the game
  thread stopped entering the DDI while the driver thread waited for it = deadlock (likely at the first replayed
  Enter = title -> menu, dgVoodoo swap-chain/device change). Diagnosis build: on that wait the winsys suspends the
  last DDI thread and logs eip + a module-tagged stack scan (module names/offsets only, AGENTS rule 11).
- nfs-tc2 (x86 tc3s = + DDI-thread snapshot, 66a813b7): SPEED2 exited 0x3 (abort) during loading; stderr log:
  "Assertion failed: panfrost_bo_size(src->bo) >= panfrost_bo_size(dst->bo)" (my T3 assert - a guess: BO sizes are
  page-rounded and the BO cache can return a larger BO for the old storage). T5b (Mesa 7501a0e): assert src BO >=
  dst->width0 instead. Mesa 9c8ca93: the DDI-thread snapshot diagnostic (kept: only runs after a > 2 s wait).
- nfs-tc3 (x86 tc4 = T5b, f3a12340): through 720 title frames, then the same deadlock; snapshot of DDI thread 5756:
  maliumd32 frames (llvm-symbolizer) futex_wait <- do_futex_fence_wait (u_queue.c:132) <-
  util_queue_fence_wait_timeout <- tc_flush (my tc_fence_wait, inlined) <- d3d10umd Flush <- D3D11 runtime. The
  pumping wait never woke up: Mesa's Windows futex_wait (src/util/futex.c) returned ETIMEDOUT as a positive value,
  while do_futex_fence_wait treats only "r < 0 && errno == ETIMEDOUT" as a timeout (Linux convention) -> it loops,
  the recomputed timeout goes negative and the DWORD wraps to ~49 days = every timed util_queue_fence wait on
  Windows waits forever. T5c (Mesa 8615924): futex_wait returns -1 with errno ETIMEDOUT (EINVAL otherwise), deadline in
  the past -> 0 ms. Only timed caller: u_queue.c (others pass NULL). Clocks: both os_time_get_nano and futex use
  timespec_get(TIME_MONOTONIC).
- nfs-tc4 (x86 tc5 = T5c, 1f6c416c): SPEED2 c0000005 +0x6562b at the first replayed Enter (present 8) = the known not-foreground crash of the automation (also without TC); retry.
- nfs-tc5 (x86 tc5 = T5c, 1f6c416c): FIRST FULL RACE ON THE THREADED CONTEXT - game alive after the 119 s replay, no
  deadlock, no crash, no "tc:" waits. Race frames 80-83 ms (before TC 84 ms) = no gain yet. Per 120 frames
  (frame 3480): app thread "inside the UMD" 50 ms/frame of which ResourceMap 194726 calls 4275 ms = 35.6 ms/frame
  (22 us/map, was 6.3 us before TC); driver-thread sections pan:prepare_draw 1394 + launch_draw 1129 + varyings 652
  + shader_state_vs/fs 632 + ... ~42 ms/frame; pan:sub:get_batch_slow 841 calls 1245 ms = 1.5 ms per new batch,
  ~7/frame = 10 ms/frame; tc callbacks: render ~960 / 120 frames (8 submissions/frame marshalled from the driver
  thread, the only marshalled op). bytes_mapped_limit / bytes_replaced_limit are 0 (no TC-forced flushes).
- Why maps got slower: without is_resource_busy, TC treats every buffer as busy and implements each DISCARD map as
  "new pipe_resource (screen->resource_create) + queued replace_buffer_storage" on the application thread;
  Panfrost's own DISCARD path only swapped in a cached BO. dgVoodoo DISCARD-maps ~1.6x per draw (dynamic CBs).
- Next (ordered): (1) cheap DISCARD under TC - d3d10umd upload ring for dynamic buffers (Map DISCARD -> CPU
  shadow / u_upload suballocation, rebind buffer+offset) or a fast buffer resource_create path; (2) the batch init
  cost on the driver thread (1.5 ms per new batch, 7/frame); (3) launch_draw / varyings.
- Mesa 1fb850a: ResourceMap per-kind section timers. nfs-tc6 died at the first Enter again (+0x6562b, 2 of 3 runs) ->
  keyplay.c: EnsureForeground retries Alt + SetForegroundWindow every 100 ms for up to 3 s until the game owns the
  foreground, before the event; C:\S10eTmp\keyplay.log ("foreground fixed N, failed N").
- nfs-tc7 (x86 tc5m, dd97d26b; new keyplay: "606 events, foreground fixed 0, failed 0"): race reached, frame 76.6 ms.
  Per 120 frames: ResourceMap 175360 calls 4096 ms, of which Map:cb:discard 124308 calls 3974 ms (1036/frame, 32 us
  each = 33 ms/frame) and Map:vb:nooverwrite 25548 calls 38 ms (1.5 us). => the dynamic-constant-buffer DISCARD is
  the whole map cost under TC (each = new pipe_resource + replace_buffer_storage).
- T6 plan: rename DYNAMIC constant buffers into pipe->const_uploader at WRITE_DISCARD (u_upload_alloc_ref, rebind
  every slot it is bound to with buffer+offset, u_upload_unmap at Unmap); materialize (copy back) before copies,
  UpdateSubresource or other maps. Works with and without TC.
- T6 (Mesa e4b97d1, tools/mesa-build/tc/t6_cbrename.py + fixes): Resource::cb_rename (Usage DYNAMIC && BindFlags ==
  CONSTANT_BUFFER), cb_buf/cb_offset/cb_mapped; Device::bound_cb/_off/_size [VS,PS,GS,CS][16]; CbBind (all CB binds:
  SetConstantBuffers, CsSetConstantBuffers AND the D3D11.1 SetConstantBuffers11 path with FirstConstant/NumConstants
  - the first T6 build missed that one: renamed slices never got bound, the shader read the old zero storage);
  ResourceMap WRITE_DISCARD -> u_upload_alloc_ref(pipe->const_uploader, width0, 256) + CbRebindAll; Unmap ->
  u_upload_unmap; CbMaterialize (resource_copy_region back) before ResourceCopy/CopyRegion (src+dst),
  UpdateSubResourceUP and any other map; DestroyResource drops cb_buf and clears bound_cb.
- New regression test drivers/MaliG76Wddm/tools/d3dcbdyn (DYNAMIC CB, Map DISCARD between draws, bound once / rebound;
  4 strips read back): first T6 build FAIL (all 0x00000000, also with MALIUMD_NO_TC=1), WARP PASS; fixed build PASS
  with and without TC; d3dtri32 tri + d3dvbring32 PASS.
- nfs-tc9 (x86 tc6f = T6, e1fae195): full race, frame 28.6-34.0 ms (~30 fps; was 84 ms before TC = 2.8x), inside the
  UMD 10.5-14.4 ms/frame; scan-out traces/wddm2/nfs-tc9.png renders correctly (car, HUD 4th/4 lap 1/3, minimap,
  Gemini neon). Intermediate T6 run nfs-tc8 (D3D11.1 path missed): 38 ms but black.
- x64/ARM64 port (2026-10-18 ~23:55, user: "apply everything relevant to the x64 driver too and commit. tag alpha8"):
  same Mesa tree (HEAD e4b97d1 = 0001-0146), build-mesa-g76 -Stage cross,collect (ARM64) + -Stage ec (ARM64EC, in a
  FRESH powershell process - a second build-mesa-g76 call in the same process captures the first one's VS environment
  as its "clean" one and vcvarsamd64_arm64 exits 255; also never run vcvars-based builds in parallel), make-arm64x ->
  s10e-artifacts\mali-umd-0146-x (forwarder 19d7076b, maliumd_arm64 8a8c7a72, maliumd_ec fb100f0b), deployed with
  wddm2dbg umdxswap (no reboot: DWM/explorer keep the boot-time driver).
  Tests on the new set: d3dcbdyn (ARM64) PASS, d3dcbdyn64x (x64 emulated, EC) PASS, d3dtri tri PASS, d3dvbring PASS,
  d3dcs 0/7 differ (ARM64) + d3dcs64x 0/7 differ; Edge (new GPU process) WebGL clear 51,102,153,255 draw
  255,128,0,255, renderer ANGLE Mali-G76 D3D11. DWM on the threaded driver = after the next reboot (pending).
- Mesa commits exported as tools/mesa-build/patches/0132-0146.
- alpha8 DWM check (reboot 2026-10-18 23:55): dwm/explorer/SearchHost load maliumd_arm64 0146 ("device: context ...
  threaded 1" in the dwm tlog), desktop renders (scan-out traces/wddm2/alpha8-desktop.png). BUT LogonUI.exe (pid
  1344) crashed at its exit after login: 0xc0000374 (heap corruption) in ntdll+0x12b118 - never before in the
  LogonUI history (tools/phone/wddm2/crashhist.ps1: older ones are Windows.UI.Xaml c000027b). LogonUI has two D3D
  devices (presents 1536x736 and 1024x576 in its tlog). Cause found in T1: a queued (deferred) w_destroy/w_unlock
  did not record its device binding and was drained under whatever binding was current (also from the pump while
  the driver thread held wddm_lock) -> pfnDeallocateCb/pfnUnlockCb on the wrong runtime device.
- T1c: mk_req.bind = cur_bind at queue time (callers hold wddm_lock); mk_drain(may_switch): with the lock owned
  (mk_direct, pump after TryAcquire, malikm_unbind_device before the binding goes) every op runs under its own
  binding (bind_switch, restored), an op of a vanished binding is dropped + logged; while the posting driver thread
  holds the lock (pump with a request) only ops of the current binding run, the rest stay queued in order.
  alpha8 tag waits for: tests + reboot + LogonUI clean.
