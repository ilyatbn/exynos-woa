# G27 - x64 (emulated) apps on our GPU: an ARM64X UMD

Why: x64 applications under emulation on Windows on Arm (most modern games) cannot use the Mali adapter - the UMD is
a plain ARM64 DLL, which an x64/ARM64EC process cannot load (research-requests R10). User goal: games + browsers on
the GPU, natively.

## Research
- MS Learn "Build Arm64X Files" (https://learn.microsoft.com/en-us/windows/arm/arm64x-build), section "Building an
  Arm64X pure forwarder DLL": "An Arm64X pure forwarder DLL is a small Arm64X DLL that forwards APIs to separate DLLs
  depending on their type: Arm64 APIs are forwarded to an Arm64 DLL. x64 APIs are forwarded to an x64 or Arm64EC
  DLL." Built from empty arm64 + /arm64EC objects, two DEF files (EXPORTS X = foo_arm64.X / foo_x64.X), `link /lib`
  import libs, then `link /dll /noentry /machine:arm64x /defArm64Native:<arm64 def> /def:<x64 def> ...`. "When an Arm64
  process loads foo.dll, the operating system immediately loads foo_arm64.dll in its place". For merged Arm64X
  binaries "the Arm64 and Arm64EC configurations must use the same C runtime" (ours: static /MT since 23ef073).
- Prior art: osy/virtio-win-mesa Triton UMD, src/virtio/neptune/triton/tools/make-arm64x.bat (MIT) - exactly this
  forwarder (neptune_umd.dll -> neptune_umd_arm64.dll / neptune_umd_ec.dll), DEFs generated from the UMD's .def.
- Mesa docs: Visual Studio builds for ARM64EC are supported (docs.mesa3d.org/install.html, notes/d5-gpu-display.md).

## Plan (one commit each)
- G27.1 baseline: an x64 build of d3dcs on the phone (what does an emulated x64 app get today?).
- G27.2 ARM64EC Mesa build stage (build-mesa-g76.ps1 -Stage ec) -> maliumd_ec.dll.
- G27.3 ARM64X forwarder (tools/mesa-build/make-arm64x.ps1): maliumd.dll -> maliumd_arm64.dll / maliumd_ec.dll;
  deploy by hand, x64 + ARM64 tests, DWM untouched until the reboot.
- G27.4 INF: the three files to System32.

## Progress log
- G27.1 baseline: d3dcs built for x64 (build-d3dcs.cmd x64 -> d3dcs64x.exe), emulated on the phone: "Mali: D3D11CreateDevice 0x887a0004 FL 0x0" = DXGI_ERROR_UNSUPPORTED - an x64 process gets no device on the Mali adapter (the ARM64 UMD cannot load into it).
- G27.2 ec build, attempt 1 (triple `--target=arm64ec-pc-windows-msvc` in the cross file): ~400 compile failures.
  clang's arm64ec target predefines `__x86_64__`/`__x86_64`/`_M_X64` (the EC ABI is x64's) -> Mesa util/detect_arch.h
  set DETECT_ARCH_X86_64 and SSE paths. Patched: `&& !defined(_M_ARM64EC)` on the X86_64 test.
- attempt 2: 69 failures. All 171 `mmintrin.h` errors ("invalid conversion between vector type '__v2di'") come via
  util/xxhash.h:4343 - the XXH_PREFETCH path for `_MSC_VER && (_M_X64 || _M_IX86)`. (XXH_VECTOR already picks NEON:
  `_M_ARM64EC` is in its NEON list.) Patched: `&& !defined(_M_ARM64EC)` there.
- zlib z-1.dll: `LNK1423: ARM32 is no longer a supported target` - build.ninja has `/MACHINE:arm` on every link and
  llvm-lib rule. Cause (meson 1.12.1 = latest, venv/Lib/site-packages/mesonbuild/compilers/mixins/visualstudio.py
  ~l.110): target canonicalisation `elif 'aarch64' in target ... elif 'arm' in target: self.machine = 'arm'` - clang-cl's
  "Target: arm64ec-pc-windows-msvc" -> 'arm64ec' -> 'arm'. The static linker inherits the same machine.
- Toolchain tests (scratchpad ectest, t.c one export):
  - `clang-cl --target=aarch64-pc-windows-msvc /arm64EC`: "/arm64EC has been overridden by specified target; ignored".
  - `clang-cl /arm64EC` (no triple): Target arm64ec; `link /MACHINE:x64 ... /machine:arm64ec` links fine, dumpbin
    "8664 machine (x64) (ARM64X)" = an ARM64EC image. softintrin.lib not needed for this test (MS Learn
    arm64ec-build: "Use the /arm64EC switch for cl and /MACHINE:ARM64EC for link"; softintrin.lib is implicit for MSVC
    EC objects - llvm/llvm-project PR 89171 - add it only if _mm_* unresolved appear).
  - archives: llvm-lib /MACHINE:x64 on an EC obj: "file machine type arm64ec conflicts with library machine type
    x64"; MSVC lib /MACHINE:x64 or arm64: LNK1392 "Did you mean /machine:arm64x or /machine:arm64ec?". The archive
    machine must be arm64ec -> meson's canonicalisation has to produce it.
  - Decision: the ec stage uses its own venv ($Mali\venv-ec) whose visualstudio.py gets one idempotent line
    (`elif 'arm64ec' in target: self.machine = 'arm64ec'` before the aarch64 test); the shared venv stays stock.
    (Build-tool fix, not a runtime switch - AGENTS 12 n/a.)
- ec build with the venv-ec meson: build.ninja now `/MACHINE:arm64ec` on all 4 rules; full build 0 FAILED.
  s10e-artifacts\mali-umd-0129-ec\maliumd_ec.dll: "8664 machine (x64) (ARM64X)" (= ARM64EC image), sections include
  .a64xrm / .hexpthk, imports only api-ms-win-core-synch, KERNEL32, USER32, GDI32, CFGMGR32 (static CRT, as ARM64).
- G27.3 tools written: tools/mesa-build/make-arm64x.ps1 (MS Learn pure-forwarder steps; export list from dumpbin
  /exports of the ARM64 DLL, must equal the EC DLL's), tools/phone/wddm2/umdxswap.cmd + `wddm2dbg.sh umdxswap DIR`
  (live swap of the 3 files, rollback copy System32\maliumd.dll.pre-g27). ARM64 half rebuilt from the same tree
  (cross,collect -> mali-umd-0129) so both halves are one source.
- Review of 0cac259 (G27.2): rg for direct `__x86_64__|__x86_64|_M_X64|_M_AMD64|__amd64(__)` tests in util, gallium
  aux/d3d10umd/panfrost/winsys, panfrost, compiler, c11 (outside detect_arch/xxhash): only blake3_dispatch.c:130 (inside
  `#if IS_X86`, and upstream blake3_impl.h already excludes _M_ARM64EC from IS_X86 -> dead on EC) and hash_table.c:726
  (`_WIN64 || __x86_64__` = 64-bit, right for EC). Patch 0129 guards are `&& !defined(_M_ARM64EC)` only -> ARM64 / x86
  builds unchanged in behaviour. venv-ec pins meson==1.12.1, the version whose anchor line the patch needs (the
  script throws if the anchor is gone). Nothing to fix.
- G27.3 forwarder built (make-arm64x.ps1 -> s10e-artifacts\mali-umd-0129-x): maliumd.dll 679dfd16 "AA64 machine
  (ARM64) (ARM64X)", 49 exports "forwarded to maliumd_arm64.*"; maliumd_arm64.dll 8efadef8 (= mali-umd-0129 ARM64,
  Mesa 0001-0129); maliumd_ec.dll c64534df (mali-umd-0129-ec). Script fixes on the way: `link /lib` must be the first
  argument (with /nologo first: LNK4044 "unrecognized option '/lib'" + LNK1104); a 0-byte .lib left by that failure
  gave LNK1136 -> the script clears its arm64x-obj directory.
- Deployed live 2026-10-18 ~13:15 (wddm2dbg.sh umdxswap, hashes verified; rollback System32\maliumd.dll.pre-g27 = 0127s;
  DWM keeps its boot-time UMD until the next reboot). Battery 100 % charging.
  - ARM64 d3dcs.exe (through the forwarder): 0 of 7 differ -> PASS.
  - x64 d3dcs64x.exe (emulated): "Mali: D3D11CreateDevice 0x00000000 FL 0xa100" (G27.1 baseline: 0x887a0004),
    0 of 7 differ -> PASS. tools/phone/games/d3dcsrun.sh got an arch argument (x86|arm64|x64).
  - ARM64 Edge (native) on the forwarder: tools/phone/wddm2/edgecdp.ps1 (new; user session, --remote-debugging-port
    + separate profile, tracing only) + cdp_webgl.py: clear 51,102,153,255 draw 255,128,0,255, renderer "ANGLE (...
    Arm Mali-G76 MP12 ... Direct3D11 vs_4_1 ps_4_1, D3D11)". `tasklist /m maliumd*`: the new Edge GPU process (7436)
    has "maliumd.dll, maliumd_arm64.DLL" loaded - the forwarder path, as MS Learn describes. dwm/explorer still hold
    the boot-time maliumd.dll. (umdwhich.ps1 now also matches maliumd_arm64/_ec; from the ssh session it lists nothing
    for other users' processes - use tasklist /m.)
- Review of 3eab80e (G27.3): export counts checked mechanically (dumpbin: 49 functions / 49 names in maliumd.dll ARM64,
  maliumd_ec.dll, forwarder); wddm2dbg.sh committed as LF (+9 lines; the CRLF warning is the working copy only).
  Findings -> follow-ups: F1 umdxswap.cmd swapped the forwarder FIRST - a 64-bit process starting before
  maliumd_arm64/_ec land would get a forwarder without targets -> order now targets first, forwarder last.
  F2 run-as-user.ps1 passes no arguments, so `edgecdp.ps1 -Stop` cannot be run that way (my "stop" call restarted the
  debug Edge instead) -> edgecdpstop.ps1.
