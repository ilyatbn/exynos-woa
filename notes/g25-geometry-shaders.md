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
