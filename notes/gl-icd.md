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
