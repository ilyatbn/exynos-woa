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
