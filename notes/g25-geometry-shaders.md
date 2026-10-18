# G25 - geometry shaders on a GPU without a geometry stage (Panfrost / Mali-G76)

Why: FL 10_x requires GS; dgVoodoo's D3D11 backend (NFSU2) uses two gs_4_0 point-sprite expanders (point in,
trianglestrip out, maxout 4; notes/g24-compute-cs4x.md G25 scoping). Since Mesa 0116 a GS gets no handle and is
skipped -> sprites are 1-pixel points. User goal: games correct, natively (AGENTS rule 12).

## Design (fixed slots + primitive restart; no CL kernels, no prefix sums)
Per draw with an emulated GS bound (n = input primitives, vpp = vertices per input primitive, maxout = GS
dcl_maxout):
1. VS pass: a stream-output variant of the bound VS captures every VS output register (r -> dwords 4r..4r+3) into
   buffer A; rasterizer discard. XFB writes assembled primitives (indices and strips resolved): A = n * vpp vertices.
2. GS pass: clear index buffer I (n * 2 * maxout uint32) to 0xFFFFFFFF; dispatch the GS as a compute shader, one
   invocation per input primitive p: inputs v[k][r] = A[(p * vpp + k) * vs_stride + 16 r]; outputs collected in
   locals; emit -> store the locals to B[p * maxout + count] (gs_stride per vertex) and I[p * 2 maxout + icount++] =
   p * maxout + count, count++ (emits past maxout dropped, as D3D specifies); cut -> I[... icount++] = restart.
   The GS's constant buffers are bound to the compute stage.
3. Draw pass: a passthrough VS (in r = B attribute r, out = the GS's output semantics) draws I as an indexed
   point list / line strip / triangle strip (the GS output topology) with primitive restart, n * 2 * maxout indices.
4. Restore the application's state (VS, vertex/index buffers, topology, rasterizer, SO targets).
Limits first version: no GS samplers/SRVs, no SV_PrimitiveID / GS instancing / multiple streams, no SO from the GS.

## Tasks (one commit each)
- G25.1 d3dgs test tool (points -> quads GS, a strip with cut, triangle passthrough) vs WARP.
- G25.2 GS -> compute lowering (NIR) at CreateGeometryShader.
- G25.3 VS stream-output variant + draw orchestration + passthrough VS.

## Progress log
- G25.1 d3dgs (drivers/MaliG76Wddm/tools/d3dgs) baseline, x86 UMD 0123 (GS skipped since 0116): sprites DIFF 560/4096
  (Mali lights 16 pixels = 1-pixel points, WARP 576), cut DIFF 821 (Mali 1 pixel, WARP 820), passthrough DIFF 1595
  (same triangles, colour not inverted: the GS never runs). = NFSU2's symptom, reproduced.
- G25.2 + G25.3 merged into one commit (the translator alone cannot be verified without the draw): Mesa 0125 -
  ShaderTGSI.c gs_as_cs (compute ureg, GS_BLOCK 64; prologue = primitive / active / clamped index + loads of every
  declared input of every vertex from buffer 0; outputs as temporaries with their TGSI semantic recorded; EMIT/CUT =
  guarded stores to buffer 1 / index buffer 2); Shader.cpp creates the compute CSO when the driver has no
  create_gs_state; GsEmu.cpp three-pass draw; GS/CS constant buffers + rasterizer now tracked (Device).
- First device run: no crash, I and B correct (GS_DUMP), but A all zeros -> stream output wrote nothing. Cause found in
  Panfrost: its XFB variant is built from nir->xfb_info, which tgsi_to_nir never fills from
  pipe_shader_state.stream_output -> stream output was dead for every TGSI VS (also plain D3D10 SO). Fix = Mesa 0124
  pan_xfb_info_from_tgsi (output o -> the output variable with driver_location == register_index).
- With 0124 + 0125 (x86 mali-umd-0124-x86): d3dgs sprites 0/4096 differ (lit 576/576), cut 0 (820/820), passthrough 0
  (1595/1595) -> PASS pixel-exact vs WARP. Regression: d3dcs 7/7, d3dbc lossy 126/126.

## Review of d15fbca / Mesa 0124 + 0125 (AGENTS rule 4)
- Mechanical: patches 0111-0125 `git am` cleanly onto 5c8935c and reproduce the Mesa branch byte for byte.
- FOUND (real limit): Panfrost's XFB (pan_cmdstream.c panfrost_launch_xfb) has "TODO: XFB with index buffers" and writes
  outputs per VERTEX of the draw, not per assembled primitive. Pass 1 therefore matches the compute pass's layout
  (A = primitives * vpp in primitive order) only for NON-INDEXED LIST topologies (points / lines / triangles / *_adj
  lists). Indexed draws and strips/fans with a GS bound draw wrong geometry. -> follow-up G25.4: capture per vertex
  (non-indexed point draw over the referenced vertex range) and let the compute pass resolve primitive vertex k of
  primitive p through the topology and the application's index buffer.
- FOUND (ordering): pass 1 (XFB writes A) -> pass 2 (compute reads A) has no explicit barrier; it works in d3dgs
  (Panfrost orders the jobs in one batch) but the gallium contract wants pipe->memory_barrier between them ->
  follow-up G25.5.
- Checked: Device::bound_rs starts NULL (memset) and is restored after pass 1; the D3D runtime binds a default
  rasterizer at device creation, so NULL is not expected - if it ever is, pass 1's discard state stays bound: G25.5
  guards it.
- Checked: tracked constant buffers hold references (released at the next bind / DestroyDevice).
- Not tested yet: instancing, GS with constant buffers > slot 0, GS output line strips / points.
- G25.4 test first: d3dgs + idx-sprites / strip / idx-tris (x86 0125): idx-sprites DIFF 288 (8 sprites, wrong points), strip DIFF 1392, idx-tris DIFF 1377 - the review's prediction confirmed; the 3 original cases still pixel-exact.
- G25.4 (Mesa 0126): pass 1 = non-indexed point draw over the referenced vertices ([start, start+count) of a plain
  draw; the vertex buffers' extent of an indexed one; instanced alike); the GS prologue resolves element -> index ->
  vertex -> A slot (topology code 0 list / 1 line strip(+adj) / 2 triangle strip / 3 fan; buffer 3 = the application's
  index buffer, 16-bit lanes extracted; CONST[15][0..2] parameters). Bug on the way: a buffer LOAD with writemask .z
  fills it from address + 8 (component c <- address + 4c) - the index fetch now loads .x and moves it.
  Result (x86 mali-umd-0126-x86): d3dgs 6/6 pixel-exact vs WARP - sprites 576, cut 820, passthrough 1595, idx-sprites
  288, strip 1995 (confirms odd triangle-strip triangles reach the GS as (i, i+2, i+1)), idx-tris 2704 lit pixels.
  Regression: d3dcs 7/7, d3dbc lossy 126/126.
- Review of ca0b848 / Mesa 0126: checked - the index buffer is bound from offset 0 and Draw.cpp folds ib_offset into
  draw.start, so element addressing is right (idx-tris with base vertex 1 passes); out-of-range fetches are clamped
  (index address to the buffer, A element to the captured count). Costs / limits noted: an indexed GS draw transforms
  the whole vertex-buffer extent in pass 1 (up to 1M vertices) - fine for sprites, a perf risk for large shared VBs;
  strip cuts in the application's index buffer are still not split. Open: G25.5 (barrier, null rasterizer guard).
- G25.5 (Mesa 0127): memory_barrier(STREAMOUT | SHADER_BUFFER) after pass 1; bound_rs guard (verified the runtime binds a default rasterizer right after CreateDevice: d3dfs DDI trace CreateRasterizerState -> SetRasterizerState before any draw). d3dgs 6/6 still.
- NFSU2 with x86 0127 (gamefg launches): both dgVoodoo GS compile to compute (13 / 16 output registers); no GS draw in
  the menus (no particles there); BO memory ~75 MB. Run pid 4160 hit the game's own fault +0x6562b (known), pid 5808
  ended without an error event. OpenWith watcher (tools/phone/games/openwithwatch.ps1): no OpenWith in that run - the
  user's "annoying link" popup is not reproduced; crashevents.ps1 lists the game's WER events. Particles in a race =
  user test.
- ARM64 0127 (Mesa 0111-0127) deployed to System32 (previous: maliumd.dll.old0123 / .tmp0123): d3dgs 6/6 pixel-exact,
  d3dcs 7/7, d3dbc lossy 126/126 on ARM64. Browser: GDI screenshots came back all white and the scan-out dump black -
  the display was asleep (DWM presenting normally, no errors) - so the check went display-independent:
  tools/phone/wddm2/cdp_webgl.py over DevTools (Edge with --remote-debugging-port + a separate --user-data-dir, tracing
  only; recent Chromium refuses the debug port on the default profile): WebGL clear readback 51,102,153 (= 0.2/0.4/0.6),
  triangle readback 255,128,0, renderer "ANGLE (... Arm Mali-G76 MP12 ... Direct3D11 vs_4_1 ps_4_1, D3D11)";
  cdp_gpuinfo featureStatus = baseline (webgl/webgpu/gpu_compositing/video enabled, rasterization unavailable_off).
