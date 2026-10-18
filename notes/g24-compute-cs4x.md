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

## Review of commits 0a50093 / 044d8d7 / a038369 / 23b6071 (AGENTS rule 4)
- Checked mechanically: the exported series 0111-0119 `git am`s cleanly onto 5c8935c (0110) and the result is
  byte-identical to the Mesa working branch (empty `git diff --stat`). 0110 (diagnosis switch) is superseded inside
  0114 (D3D10UMD_NO_CS4X hides the cap; diagnosis only).
- 0119 claim "blitter binds slots 0-1 only, never unbinds trailing": u_blitter.c set_sampler_views calls are (0,2,0),
  (0,1,0) x4; restore passes trailing=count only when nothing was saved -> holds.
- 0118: exec slab pages >= WSLAB_EXEC_PAGES pre-marked, free_pages = 2048; wsubdead frees only real pages; the
  64-bit path is untouched (#if !_WIN64); exec BOs > 2 MB keep their own allocation. Holds.
- 0116: GS-with-stream-output on this driver now silently loses SO (logged once per shader) - before it called NULL,
  so no regression; real fix = G25.
- 0117 found a real bug of 0114 by crash (uninitialised SRV::buffer / Shader::extra) - the 0114 review missed that the
  DDI private memory is not zeroed. Lesson: every new field of a runtime-allocated object needs an init on every
  create path.
- Verification after the series: d3dcs 7/7 PASS x86 (d3dcs-4) and ARM64 (mali-umd-0119 in System32, old one kept as
  maliumd.dll.old0110); d3dbc 63/63 PASS x86 + ARM64.
- FOLLOW-UP (open): the ARM64 UMD now advertises CS 4.x to every process (DWM, browsers) - browsers (ANGLE) may take
  compute paths at FL 10_1; check Edge/Brave GPU pages and a WebGL page with mali-umd-0119 before the next reboot
  loads it into DWM.
- FOLLOW-UP (open): Mali THREAD_MAX_WORKGROUP_SZ 384 < D3D's 768 per group - a CS with > 384 threads is not handled.
- User launch from the desktop shortcut: fullscreen STAYS UP and reaches the game ("im already ingame"), but the image
  is sideways + squashed (user photo: title screen drawn in panel portrait orientation while the phone is held
  landscape). tlog malikm-SPEED2.EXE-984: every present = BltDXGI 1520x720 -> 720x1520 primary, Rotate 1.
  MS Learn DXGI_DDI_ARG_BLT (https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_arg_blt):
  "Rotate: A DXGI_DDI_MODE_ROTATION-typed value that identifies the orientation of the display mode." -> DXGI says the
  fullscreen mode is IDENTITY although the desktop is 1520x720 DMDO_90 (s10emode list; KMD OfferRotation=1). So the
  fullscreen mode set dropped the path rotation (or DXGI reports it so); the UMD blit then stretches unrotated.
  KMD Commit does not log the committed path rotation (dev->Rotation only from UpdateActiveVidPnPresentPath).
  d3dfs test tool written (fullscreen 1520x720, logs output rotation + GDI orientation before/during/after; green
  top-left quadrant) - NOT run yet: the user is playing (a fullscreen steal would crash NFSU2).
- d3dfs (tools: drivers/MaliG76Wddm/tools/d3dfs, tools/phone/games/d3dfsuser.ps1; foreground via a synthetic Alt):
  windowed DXGI output rotation 2 (ROTATE90) = right. SetFullscreenState E_FAIL: UMD tlog "primary 1520x720 rotation 2
  flags 0x0 mode 720x1520" then "CB FAIL SetDisplayMode hr 0x80004005". MS Learn "Supporting Rotation in a Display
  Miniport Driver" (https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver):
  "The Microsoft DirectX graphics kernel subsystem uses non-rotated surface dimensions to create the shared primary
  surface." The UMD created the kernel primary at the resource size (1520x720). DXGI_DDI_PRIMARY_DESC docs:
  DXGI_DDI_PRIMARY_NONPREROTATED (0x2) "The primary really represents the DXGI_DDI_MODE_ROTATION_IDENTITY-type rotation
  ... because applications will handle the output orientation". DXGKARG_DESCRIBEALLOCATION.Rotation: "reserved".
- Mesa 0120 (log primary flags + mode size) + 0121 (kernel primary at the mode's non-rotated size when Rotation is
  90/270 and the resource is transposed): d3dfs SetFullscreenState OK, fullscreen 1, output rotation 2, SetDisplayMode
  OK, presents = flips (flags 0x2) of the 1520x720 primaries. On-panel orientation: asked the user (webcam busy).
- 0121 (malikm_primary_present accepts a transposed kernel primary on the GPU path): the 0120 size check had failed
  every rotated present -> CPU flip fallback of the unrotated primary; user: "flickered between portrait and
  landscape". After 0121: all presents "scanout present N rotated 2 ... present 0"; user: no flicker, green
  bottom-right (expected top-left).
- 0122: rotation quad 90/270 swapped - MS Learn "All rotation degrees are defined in the counter-clockwise
  direction"; the clockwise guess was never exercised (DWM pre-rotates into 720x1520 primaries: 0 "rotated" presents in
  the dwm tlogs). User: "upper left corner now" -> rotation-unaware DXGI exclusive fullscreen correct on the rotated desktop.
- dgVoodoo/NFSU2 (gamefg.ps1 launch, tlog SPEED2-3356): primary 720x1520 rotation 2 flags 0x2 (NONPREROTATED) mode
  720x1520; BltDXGI 1520x720 -> 720x1520 Stretch, Rotate IDENTITY. Per the docs the app handles the orientation with
  NONPREROTATED - dgVoodoo does not, so exclusive fullscreen is sideways by the app's own request (would be on any
  portrait-native panel). dgVoodoo's documented option FullscreenAttributes = fake (screen-size window, no mode
  switch, DWM composes + rotates) set in C:\Games\src\dgVoodoo.conf (backup dgVoodoo.conf.bak-exclusive). Presents
  then: windowed 1520x720 to DWM. User check pending.
- User 2026-10-18: "dont forget to port all these changes to the x64 platform when we get it working" -> the ARM64 UMD
  (same Mesa source) gets 0120-0122 + all G24 patches on its next deploy; it has 0111-0119 already (mali-umd-0119).
- Review a921f37: 0120 only changes primaries whose resource is transposed to a 90/270 mode - DWM's (720x1520 for mode
  720x1520) and dgVoodoo's NONPREROTATED ones are not (checked in the dwm / SPEED2-3356 tlogs). 0121 keeps the CPU
  copy path strict, so a rotated fullscreen primary in a non-RGBA8 format (or MSAA) still fails its present - open,
  no test case yet. 0122 touches only the quad path, which DWM never takes (0 "rotated" presents).
- After the reboot (keyboard dongle; ARM64 UMD 0119 now in DWM, desktop fine): user "its now starting and crashing" in
  fake fullscreen. 3 WER events 30 s apart, same fault as the exclusive-fullscreen run: SPEED2+0x6562b
  `mov esi,[eax+4]` with eax=0xAAAAAAAA (the game's freed-memory fill) = a use-after-free in game code, right after
  dgVoodoo recreates its present buffers at 640x480 (tlog malikm-SPEED2.EXE-3100: 10500 presents at ~9 ms/frame, then
  the 640x480 shared BOs). The windowed run (no mode change) played for minutes. Web search: nothing for this address.
  Next config: FullScreenMode=false + WindowedAttributes=borderless, fullscreensize (no mode change, screen-size
  window); conf backups .bak-exclusive / .bak-fake. tools/phone/games/dgvconf.ps1 for conf edits.
- G25 scoping (2026-10-18): dgVoodoo's two GS disassembled on the PC (tools/mesa-build/dxbcdis/dxbcdis.py: tlog token
  dump -> DXBC container with the DXBC checksum -> D3DDisassemble; zero checksum = E_FAIL): both gs_4_0,
  dcl_inputprimitive point, dcl_outputtopology trianglestrip, dcl_maxout 4, 4 emits = D3D9 POINT SPRITES (size from
  cb0[96]/[97] = point size min/max + attenuation A/B/C, quad around the projected position;
  traces/games/nfsu2-gs0.txt, nfsu2-gs1.txt). Without a GS the sprites rasterize as 1-pixel points (particles).
  Mesa src/poly (Asahi: gallium/drivers/asahi/agx_state.c drives poly_nir_lower_gs -> count / copy / pre-GS compute
  shaders + CL kernels poly/cl/geometry.cl) is the general GS-via-compute path; our Panfrost build already has the CL
  precompile toolchain (libpan). Multi-day port - G25 stays open.
- x86 UMD packaging (HANDOFF 58 #2): MaliG76Wddm.inf 1.0.70.1 now carries maliumd.dll (DIRID 11, System32) and
  maliumd32.dll ("10, SysWow64") with COPYFLG_IN_USE_TRY_RENAME + UserModeDriverNameWow x4 - MS Learn "General x64 INF
  Information" (DispWow = 10, SysWow64; r2umd32.dll,,,0x00004000) and "Loading a User-Mode Display Driver" (UMD to
  %systemroot%\system32). build-package.ps1 takes -Umd / -Umd32 and signs both DLLs; package
  s10e-artifacts/mwddm-1.0.70.1 (UMD 0127 both arches, KMD code unchanged since 1.0.69.1). InfVerif: /v VALID, /u
  VALID; /w (Windows Driver isolation: files only under DIRID 13) reports ERROR 1322 for both UMDs - the documented
  display UMD layout is System32/SysWOW64 and no display-UMD driver-store loading is documented, so the documented layout
  stays. Dependency noted: the UMD imports VCRUNTIME140.dll (UCRT is OS) - follow-up: static CRT so a clean system needs
  no VC redistributable. NOT installed on the phone (a display-driver reinstall unattended risks the working display;
  even pnputil /add-driver could let PnP pick 1.0.70.1 at the next enumeration) - install when the user is around.
- Review ce91932: catalog covers maliumd.dll + maliumd32.dll (signtool verify /c: 'File is signed in catalog', WDK test cert; 'not verified' on the PC = untrusted test root, as for the .sys); MaliG76Wddm.sys carries no version resource (unchanged practice), DriverVer 1.0.70.1. The previous INF passes /w, the new one fails /w only for the two documented UMD copies.
