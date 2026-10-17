# G24 - compute shaders 4.x (DirectCompute 4.x) in the UMD

User 2026-10-17: "Proceed" (after G23 found dgVoodoo's FL10 D3D11 backend requires CS 4.x - D3D10_X_HARDWARE_OPTIONS;
with it advertised (diagnosis switch) dgVoodoo calls CreateComputeShader x4 and its init error disappears).
The user's next game is D3D10 (D3D10 DDI, same frontend - BC emulation applies).

## Scope (MS Learn, rule 13)
https://learn.microsoft.com/en-us/windows/win32/direct3d11/direct3d-11-advanced-stages-compute-shader
"Using Compute Shader on Direct3D 10.x Hardware": cs_4_0/cs_4_1; <= 768 threads per group, X/Y <= 768, Z = 1;
dispatch Z = 1; "Only one unordered-access view can be bound to the shader"; "Only RWStructuredBuffers and
RWByteAddressBuffers are available as unordered-access views"; groupshared <= 16KB per group, a thread writes only its
own region (<= 256 B, indexed by SV_GroupIndex / SV_GroupThreadID), reads anywhere; "No atomic instructions"; "No
double-precision values". (Raw / structured buffer SRVs: the "Plus_RawAndStructuredBuffers_Via_Shader_4_x" part.)

## Progress log
- Research (code): tgsi_to_nir already maps TGSI BUFFER LOAD/STORE -> load_ssbo/store_ssbo (binding = buffer index),
  BARRIER -> nir_barrier(execution WORKGROUP only - no memory semantics), CS sysvals THREAD_ID/BLOCK_ID/BLOCK_SIZE,
  CS_FIXED_BLOCK_* properties. MISSING: TGSI_FILE_MEMORY (shared) declaration + LOAD/STORE ("unexpected file"),
  MEMBAR. Panfrost: create_compute_state asserts "TGSI kernels unsupported" (NIR only) and takes
  cso->static_shared_mem; caps max_shader_buffers 16 for compute/fragment (0 for VS - side effects); compute true.
  d3d10umd: DXBC parser table covers D3D10.1 opcodes only (D3D11 opcodes default to zero entries = no operands); the
  TGSI translator has VS/GS/PS only; all D3D11 CS DDIs are stubs (d3d11_stubs.inc). WDK opcode numbers:
  DCL_THREAD_GROUP 155, DCL_UAV_RAW 157, DCL_UAV_STRUCTURED 158, DCL_TGSM_RAW 159, DCL_TGSM_STRUCTURED 160,
  DCL_RESOURCE_RAW 161, DCL_RESOURCE_STRUCTURED 162, LD_RAW 165, STORE_RAW 166, LD_STRUCTURED 167,
  STORE_STRUCTURED 168, SYNC 190 (flags bits 11-14: threads_in_group, TGSM, UAV group, UAV global); operand types
  UAV 30, TGSM 31, vThreadID 32, vThreadGroupID 33, vThreadIDInGroup 34, vThreadIDInGroupFlattened 36; CS type 5.
- Plan (one commit each): C1 ttn shared memory + barrier semantics + MEMBAR; C2 parser CS opcodes; C3 translator CS
  (ureg compute, sysvals, raw/structured LOAD/STORE on BUFFER/MEMORY, sync) + metadata (SSBO slot map, shared size);
  C4 DDIs (CreateComputeShader via tgsi_to_nir -> NIR CSO, CsSet*, UAV + raw/structured buffer SRV objects,
  Dispatch/DispatchIndirect, ClearUnorderedAccessView*, caps CS4x on natively); C5 d3dcs test vs WARP + dgVoodoo.
- C1-C4 implemented in Mesa (0111 ttn shared/MEMBAR, 0112 parser, 0113 translator, 0114 DDIs); x86 build deployed.
  d3dcs32 run 2 (traces/games/d3dcs-2.txt): raw ok, raw2 ok vs WARP; structured -> Panfrost assert
  `ctx->ssbo_mask[st] & (1 << ssbo_id)` (pan_cmdstream.c:1186) = SSBO slot unbound. Cause: a structured SRV
  (D3D11_SRV_DIMENSION_BUFFER, format UNKNOWN) arrives as D3D10DDIRESOURCE_BUFFER, CreateBufferExShaderResourceView
  only took BUFFEREX -> fell to the sampler-view path, v->buffer NULL. Fix: accept BUFFER + UNKNOWN too (Buffer.First/NumElements).
- x86 rebuilt with the BUFFER-dimension fix (maliumd32 sha b92382b2...), deployed to SysWOW64. d3dcs32 run 3
  (traces/games/d3dcs-3.txt, tools/phone/games/d3dcsrun.sh): raw, raw2, structured, groupshared, grid2d, texcb,
  clear all ok -> "0 of 7 tests differ -> PASS" vs WARP. Next: NFSU2 via dgVoodoo.
- NFSU2 run 1 (native caps, traces/games/nfsu2-cs4x-1.txt): alive 95 s (was: init error), then AV: dgVoodoo D3D9 -> d3d11 CreateGeometryShader -> maliumd32!CreateGeometryShader+0x9e calls NULL (dump traces/games/SPEED2.EXE.1420.dmp).
- 0116 GS guard (Panfrost has no GS: create/bind/delete_gs_state NULL) -> x86 mali-umd-0115-x86 deployed; GS tokens dumped to the tlog.
- run 2 (0115): alive ~70 s, then abort: pipe_reference count != -1 in DestroyShaderResourceView (dump SPEED2.EXE.3740.dmp, at dxgi ResizeBuffers): G24 SRV::buffer uninitialised for texture SRVs; Shader::extra freed for all stages. Fixed (Mesa 0117), mali-umd-0117-x86 deployed.
- run 3 (0117): alive ~60 s, GS 0/1 created (1112 / 1205 dwords, unbound); then the GAME's own int 3 (SPEED2+0x4052e, no driver frames; dump SPEED2.EXE.6772.dmp).
- run 3 tlog (traces/games/malikm-SPEED2.EXE-6772.txt): 35x "CB FAIL Allocate hr 0x8876017c" + "create-bo FAIL size 4096
  panfrost flags 0x0" during CreateVertexShader bursts, then the game's int 3. 0x0 = executable BO (shader pool), which
  the 32-bit slab sub-allocator (0102, wsub_alloc) skips: every exec BO is its own allocation with a permanent CPU view
  (32 MB of the WOW64 address space each, vatest). dgVoodoo's many shaders exhaust it (~4 GB / 32 MB = ~128 BOs).
  KMD side checked: segment 2.5 GB, GPU VA 32 MB..4 GB, exec ranges inside one 16 MB window (MkVaAlloc).
  Fix (Mesa 0118): exec slabs too - 8 MB allocations without NOEXEC (fit any 16 MB window with 8 MB free), sub-allocated
  like the no-exec ones.
- run 4 (0118): no allocation failure (slab 1 = 8 MB exec); abort: u_blitter saved_sampler_views (32) < num_views in
  Panfrost blit-from-staging at UpdateSubresourceUP (dgVoodoo binds an SRV slot > 31; D3D10 has 128). Fix 0119 clamps
  the save (blitter binds slots 0-1 only, no trailing unbind). mali-umd-0119-x86 deployed.
- run 5 (0119): RUNS - user: "its running but windowed" (21000 submits, ~100 MB BOs). dgVoodoo.conf FullScreenMode false (set during init debugging) -> true; relaunched.
- run 6 (FullScreenMode true): user "it started then quit". tlog malikm-SPEED2.EXE-2460: 1520x720 fullscreen swapchain,
  8 presents (rotated blit, ok), then 640x480 present buffers (= fullscreen left, focus loss) and the game's own AV
  (eax=aaaaaaaa, SPEED2+0x6562b, no driver frames; dump SPEED2.EXE.2460.dmp). Hypothesis: the scheduled-task launch
  never owns the foreground -> device lost -> NFSU2's known alt-tab crash. Desktop shortcut SPEED2.lnk made
  (tools/phone/games/gameshortcut.ps1) for a user launch.
