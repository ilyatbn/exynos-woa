# GL1: native OpenGL ICD for the Mali (Panfrost over WGL) (2026-10-19)

User: "Work on opengl native support". Motivation: Stardew Valley (MonoGame DesktopGL) needs ARB/EXT_framebuffer_object;
Windows' GDI OpenGL 1.1 has none (notes/games-steam.md). The MS GL/CL/Vulkan compat pack (GLOn12) is installed but
there is no D3D12 GPU here -> WARP (CPU).

## Docs (AGENTS 13)
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/loading-an-opengl-installable-client-driver :
  "The OpenGL runtime determines the name, version, and flags that are associated with the OpenGL ICD by calling the
  D3DKMTQueryAdapterInfo function with the KMTQAITYPE_UMOPENGLINFO value"; key
  HKLM\System\CurrentControlSet\Control\Class\{Adapter GUID}\0000\ (the same key as UserModeDriverName):
  OpenGLDriverName REG_SZ, OpenGLVersion REG_DWORD, OpenGLFlags REG_DWORD ("bit 0 (0x00000001) is set for
  compatibility. When bit 1 (0x00000002) is set, the OpenGL runtime does not call the ICD's finish function before
  the runtime calls the ICD's swap-buffers function"); WOW64: OpenGLDriverNameWow / OpenGLVersionWow / OpenGLFlagsWow.
  Nothing on the page about Arm64 / x64-emulated processes (open question for the ARM64EC ICD, step GL4).

## Design
- Mesa already has the ICD: the WGL frontend (src/gallium/frontends/wgl, stw_*) + target src/gallium/targets/wgl
  (libgallium_wgl.dll, exports the Drv* ICD entry points - the same DLL the GLOn12 pack ships for d3d12). Its
  wgl.c picks llvmpipe / d3d12 / zink / softpipe; Panfrost is to be added (Mesa patch, like d3d10_gdi.c's
  panfrost branch: driconf from driinfo_panfrost.h, glsl types are set up by the GL frontend itself).
- Kernel access: the malikm shim (include/win32-compat/malikm_drm.c) has had a D3DKMT path from the start
  (wddm_init: EnumAdapters2 -> the adapter answering MW_ESCAPE_GPU_INFO -> D3DKMTCreateDevice / CreateContext;
  w_* calls use kmt.* when no runtime device is bound, ddi.on == 0). malikm_open() falls back to it when
  \\.\MaliG52 is absent. An ICD has no D3D runtime, so it runs exactly this path (what real vendor ICDs do:
  D3DKMT thunks). RISK: that path has not run since the runtime binding (H5, 0060) became the norm.
- Present, stage 1: screen->flush_frontbuffer = map the back buffer + GDI StretchDIBits into the window (the
  reference's original d3d10_pan_flush_frontbuffer, 0014). One CPU copy per frame; correct under DWM (GDI
  redirection). Stage 2 (later): GPU present.
- Build: a separate build dir (g12-work\build-arm64-gl) with -Dopengl=true, so the shipping UMD build is untouched.

## Plan (one commit each)
- GL1 Mesa patch: panfrost in targets/wgl/wgl.c + build stage 'gl' (ARM64) -> libgallium_wgl.dll.
- GL2 tools: native ARM64 glinfo test (window, ChoosePixelFormat, wglCreateContext, GL strings, clear + triangle,
  glReadPixels check, SwapBuffers) + ICD registry install script (OpenGLDriverName in the adapter's class key).
- GL3 device test: glinfo on the Mali ICD; fix what breaks in the shim's non-runtime path.
- GL4 x64 apps (Stardew): ARM64EC / ARM64X ICD as for maliumd_ec.
- GL5 GPU present (no CPU copy).

## Progress log
- 18:40 design; phone up on r4 + w47 (writetest wt9 verify OK after boot).
- 18:55 Mesa 0150 (wgl.c: panfrost screen via malikm_open, GDI present; meson: GALLIUM_PANFROST + driver_panfrost),
  build stage `gl` (g12-work\build-arm64-gl, -Dopengl=true, d3d10umd off) -> s10e-artifacts\mali-g12-gl\maligl.dll.
- First build: two failures in code the D3D-only build never compiled. (1) win_flex "end of file in string" on
  glcpp-lex.l / glsl_lexer.ll: the working copy is CRLF (Mesa `* text=auto`, Windows native eol) -> the gl stage
  adds `*.l/*.ll/*.y/*.yy eol=lf` to .git/info/attributes and re-checks those out (no source change). (2)
  lower_precision.cpp: arm_neon.h (pulled in by <vector> -> wchar.h on ARM64) typedefs a global float16_t that
  clashes with ir.h's `using float16_t = mesa::float16_t` -> Mesa 0151 includes <vector> first.
- tools: drivers/MaliG76Wddm/tools/gltest (gltest.cpp + build-gltest.cmd, built OK: GL strings, cases default / tri /
  fbo / glsl / core 3.1, swap-loop fps), tools/phone/gl/glicd.ps1 (OpenGLDriverName in the class key).
- Phone: class key 0000 = the Mali (UserModeDriverName maliumd.dll x4, UMDs live in System32); OpenGLDriverName
  absent. Basic Render has no UserModeDriverName entry listed there.
- 19:20 build OK after 0151 (GLSL: qualified mesa::float16_t, the global alias in ir.h clashed with arm_neon.h's
  float16_t under clang-cl aarch64 - an include-order fix was not enough, any C++ header pulls arm_neon in via UCRT
  wchar.h) and 0152 (wgl.c: no-op umd_section_ns, Panfrost's timing hook lives in the D3D10 frontend); zlib
  subproject static (the GL build imported z-1.dll). maligl.dll ARM64 24 993 792 B, imports KERNEL32/USER32/GDI32/
  api-ms-win-core-synch only, exports DrvCopyContext..DrvValidateVersion (19 Drv*, incl. DrvPresentBuffers).
- Baseline gltest (no ICD): pixel format GENERIC, "Microsoft Corporation / GDI Generic / 1.1.0", default + tri PASS,
  fbo / glsl / core FAIL = Stardew's failure reproduced natively. 3616 fps swaps (CPU GDI GL).
- glicd.ps1 -Install (class key 0000: OpenGLDriverName maligl.dll, OpenGLVersion 1, OpenGLFlags 1) -> no change:
  gltest's new KMTQAITYPE_UMOPENGLINFO dump shows name '' for both adapters (Mali + Basic Render), status 0 =>
  DxgKrnl does not re-read the key live (INFERENCE: read at adapter start, as for the INF-installed values). Reboot.
- Review of 1dec61d (GL1): wgl.c's Panfrost path skipped glsl_type_singleton_init_or_ref(), which the D3D10 path takes
  before its screen. Checked mechanically: stw_device.c init_screen() calls winsys create_screen before any context,
  and Mesa's one-time init that takes the glsl reference is main/context.c:233 (context creation) -> the screen could
  compile NIR without glsl types. Fixed in Mesa 0153 (own commit). Other checks: zlib now static (dumpbin: OS
  imports only); the driver order falls back to softpipe if Panfrost fails (GL works but reports softpipe - the
  renderer string tells); lexer eol attributes are local to .git/info, no source change. maligl.dll with 0153 sha256
  07121c48...
- 19:35 after the reboot UMOPENGLINFO = 'maligl.dll' version 1 flags 0x1 on the Mali adapter (Basic Render '') =>
  the class-key values are read at adapter start (confirmed). gltest in the user session (idle check first), maligl
  07121c48 (0150-0153): pixel format 99 (ICD, depth 24 stencil 8), "Mesa / Mali-G76 MC12 (Panfrost) / 3.1 Mesa
  26.2.4", GLSL 1.40, ARB_ + EXT_framebuffer_object yes, **GLTEST 5/5 PASS** (default, tri, fbo, glsl, core 3.1),
  swap loop 64.4 fps at 256x256 (GDI present). The shim's D3DKMT (non-runtime) path works as-is.
- 19:50 GL4 start. Baseline x64: gltest64.exe (build-gltest.cmd ... x64) with only the ARM64 maligl.dll registered:
  UMOPENGLINFO names maligl.dll in the x64 process too, but the pixel format is GENERIC / GDI Generic 1.1 (an x64
  process cannot load the pure ARM64 DLL; opengl32 falls back silently) - Stardew's case.
- Build: stage `glec` (build-arm64ec-gl, the ec stage's cross file + env, now a function Use-EcEnv shared by ec and
  glec) -> maligl_ec.dll; make-arm64x.ps1 -Name maligl makes maligl.dll an ARM64X pure forwarder to maligl_arm64.dll
  / maligl_ec.dll (the G27.3 maliumd scheme).
- 20:05 glec build OK: maligl_ec.dll "8664 machine (x64) (ARM64X)" (ARM64EC), OS imports only, sha256 76381e30...;
  make-arm64x.ps1 -Name maligl -> s10e-artifacts\mali-gl-x: maligl.dll forwarder 1e7743f3... (ARM64X, exports
  forwarded to maligl_arm64.*), maligl_arm64.dll = the gl stage's 07121c48..., maligl_ec.dll. All three in System32
  (OpenGLDriverName stays maligl.dll). gltest64.exe (x64, emulated): ICD pixel format 99, "Mali-G76 MC12 (Panfrost)"
  GL 3.1, **GLTEST 5/5 PASS**, 63.5 fps swaps; gltest.exe (ARM64) through the forwarder: 5/5 PASS, 64.3 fps.
- Review of e2b8efd (GL4): make-arm64x.ps1 regression - run with the default -Name on mali-umd-0148-x's
  maliumd_arm64/maliumd_ec: forwarder maliumd.dll ARM64X, exports forwarded to maliumd_arm64.* (as before); the ec
  stage body references none of the variables moved into Use-EcEnv ($VenvEc, $ecLib, $vsmix: rg shows none);
  Use-EcEnv is defined before the first stage that calls it. Nothing found.
- 20:15 **Stardew Valley (x64 .NET 6 / MonoGame DesktopGL) runs on the Mali**: launched directly (gamelaunch.ps1
  via userrun.ps1, Steam not running), after ~3-4 min of emulated .NET start-up it loads SDL2.dll, OPENGL32.DLL,
  GLU32.dll, maligl.dll, maligl_ec.DLL (tools/phone/gl/glproc.ps1). KMD heartbeat during the title screen: vsync
  ~58/s, ~85 GPU submissions/s, runtime presents flat (GL presents through GDI, stage 1). Screenshots
  traces/gl/stardew4.png (local, traces/ is gitignored; title screen: sky, clouds, buttons) and stardew5.png 1 min later (the STARDEW logo has
  dropped in = animating). No 1026 / 1000 events (recentcrash.ps1).
  Pitfalls on the way: the first screenshots were blank / black because the blank screensaver (scrnsave.scr) was
  running (CopyFromScreen "The handle is invalid", scan-out all zero) - taskkill /f /im scrnsave.scr first (as the
  cube-fps rule in notes/brightness-research.md says). The cmd.exe window in the shots is the screenshot task's own
  console. tools/phone/gl/glfront.ps1 minimises console windows + raises the game window.
- 20:40 GL5 measurements. Mesa 0154 = present timing in the malikm tlog ("gl present WxH ...: fps, map mean, present
  mean" per 120 presents). gltest 256x256: 64 fps (vsync, WGL swap interval 1), present 1.5-2.2 ms. **Stardew
  1520x720 fullscreen: 22-24 fps, map 0.03 ms, present 37.5-38 ms per frame** = the present is the whole frame.
  Cause: the allocation's CPU view is write-combined (KMD memory.c: segment CacheCoherent FALSE) and StretchDIBits
  read the 4.4 MB frame straight from the map (uncached narrow reads, ~115 MB/s). KMD presents are CPU blts too
  (command.c DoPresent from the MDL's kernel view), so D3DKMTPresent would only move the copy.
- Mesa 0155: rows memcpy'd out of the map into one cached buffer (wide loads), RGBA swizzle done there, then
  StretchDIBits; buffer under an SRWLOCK (windows of different threads). The log adds "copy mean". gldeploy.sh =
  repo deploy script (stops our GL test processes, copies the three DLLs, checks sha256). Build 0155:
  maligl 24d5c801 / maligl_arm64 ddab3525 / maligl_ec 6ce3c6ce.
- 20:55 0155 measured, Stardew 1520x720: 23.0-23.8 fps, **copy mean 36.1-36.9 ms**, present 36.9-37.7 ms -> no gain:
  memcpy reads the map at ~121 MB/s too. StretchDIBits itself from cached memory = ~0.7 ms. => the uncached read is
  latency-bound per core. Kept (0156 builds on the cached buffer). Next: 0156 = the copy split over 4 parts (thread
  pool), to test memory-level parallelism; the real fix is a present that never reads the frame with the CPU.
- 21:10 Mesa 0156 (copy in 4 parts: this thread + 3 thread-pool callbacks; auto-reset event signalled exactly once by
  whichever part reaches 0, the presenter always waits once when parts > 1). gltest 5/5 PASS. **Stardew 1520x720:
  38.8-42.5 fps (was 23), copy mean 18.4-20.4 ms (was 36), present 19-21 ms**. 4 parts give ~2x, not 4x: the read
  is bounded by the shared fabric/DRAM path for uncached traffic too. Build: maligl 59686a98 / maligl_arm64
  1c041b24 / maligl_ec 20a0c523. tools/phone/gl/gltlog.ps1 reads the timing lines of the RUNNING process only
  (polling the newest tlog file picked an older run's file).
  Remaining cost: ~19 ms CPU per 1520x720 frame. Next step that removes it: a GPU-only present (the GL frame never
  read by the CPU) - DXGI swap chain path or KMD present from a cached view; design pending.
