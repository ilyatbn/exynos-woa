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
  F2 done: tools/phone/wddm2/edgecdpstop.ps1 (run-as-user) -> wmic: no msedge with edgeprof left; tasklist /m maliumd_arm64.dll: none.
- 13:11 Windows reboot (shutdown /r) to put DWM on the forwarder; rollback = umdswap with System32\maliumd.dll.pre-g27.
- G27.4 INF 1.0.71.1: SourceDisksFiles + MW_UmdFiles (System32, COPYFLG_IN_USE_TRY_RENAME) carry maliumd.dll
  (forwarder), maliumd_arm64.dll, maliumd_ec.dll; UserModeDriverName unchanged (maliumd.dll x4 - the forwarder).
  build-package.ps1: -Umd = make-arm64x output, all three signed. Package s10e-artifacts\mwddm-1.0.71.1 (Umd 0129-x,
  Umd32 0128-x86): Inf2Cat OK (10_RS3/NI/GE ARM64), InfVerif /v "INF is VALID", /u rc 0. NOT installed (display
  driver reinstall waits for an attended session; the live System32 copies are the same builds, unsigned).
- Reboot result (ssh back after ~84 s): `tasklist /m maliumd*`: dwm, explorer, SearchHost, StartMenuExperienceHost,
  TextInputHost all "maliumd.dll, maliumd_arm64.DLL" - DWM runs on the forwarder. GDI screenshot: normal desktop.
  No new Application Error events attributable to this boot (the phone clock resets at boot - event times overlap
  older boots; none name maliumd_arm64/maliumd_ec/dwm). d3dcs x64 + ARM64 again 0/7 differ.
- User 2026-10-18: "Screenshot did not show explorer" - the post-reboot GDI shot (1013x480) had no taskbar. Checked
  (tools/phone/wddm2/shellstate.ps1, taskbarstate.ps1, new): explorer/StartMenuExperienceHost running + responding;
  Shell_TrayWnd visible, rect 0,612-1520,720 on a 1520x720 screen, StuckRects3 byte8 0x7a (no auto-hide). Cause: shot.ps1
  was not DPI-aware - at 150 % VirtualScreen reports the virtualised 1013x480 and CopyFromScreen copied only the
  top-left 1013x480 physical pixels (2/3 of the screen). Fixed (SetProcessDPIAware first): shot 1520x720 shows the
  full desktop incl. taskbar (Start, Explorer, Brave, tray, clock). All earlier shot.ps1 images since the 150 % mode
  (e.g. traces/games/browser-*-0123.png) are crops of the top-left 2/3 - keep that in mind when reading them.
- G27.5 x64 test builds (build-d3dgs/d3dso/d3dbc.cmd x64 -> *64x.exe) run emulated on maliumd_ec.dll via
  tools/phone/games/d3drun.sh (new generic runner; exit status via && / || - `%errorlevel%` in the cmd /c line is
  expanded before the run): d3dgs64x 0 of 6 differ (pixel-exact, GS emulation incl. compute + XFB), d3dso64x 0 of 2,
  d3dbc64x lossy 0 of 126 (ETC2/EAC transcode, thread pool). d3dbc64x plain 63/63 DIFF = expected since G26 (plain mode
  is the bit-exact BC-decode check; the default path re-compresses to ETC2).
- Review of eae5cb0: the .cmd edits left LF endings, but the working copies were LF before (build-d3dcs.cmd untouched is LF too), the repo stores LF, all three built after the edit -> no change needed. d3drun.sh assumes the repo root as cwd, like d3dcsrun.sh.
- Real x64 apps on the phone (tools/phone/pearch.ps1, new - PE machine per file): Program Files is ARM64 (Brave
  ARM64 too) except foobar2000 (x64). tools/phone/games/x64app.ps1 (new) started foobar2000 x64: runs, but
  `tasklist /m maliumd*` lists no UMD for it - its UI is GDI, it never opens a D3D device. Not a UMD test; killed (/f).
- Its shot (and one after g20/wake.ps1) came back all white; `wddm2dbg.sh fb` scan-out (base 0xCA000000) all zero
  (extrema 0) = panel off: the known G20/G21 display-asleep signature + the open "screen slow-wake" item (the input
  wake did not bring it back this time). dwm pid 1328 unchanged and responding, no LogonUI, no new error events ->
  not a forwarder symptom. A real x64 D3D app (game) = next, with the display awake (user, or after slow-wake).

## Remote viewing (user 2026-10-18: "Cant watch im working remotely can you figure out a solution? Some sort of screen recording?")
- Webcam is no option (notes/gpu-hw-path.md: the lit OLED saturates it, it never resolved the panel).
- GDI capture goes white while the display is off; scan-out dump goes black. So: (1) keep the display on during
  unattended tests - AC monitor timeout 0 while on the charger (test-environment setting, reversible; recorded here),
  the recorder also holds SetThreadExecutionState(ES_CONTINUOUS|ES_DISPLAY_REQUIRED) (MS Learn: "Forces the display to be
  on by resetting the display idle timer" / ES_CONTINUOUS "should remain in effect until the next call ... cleared";
  the doc does not say it turns on a display that is already off - G21's slow wake stays open); (2) record frames on
  the phone (DPI-aware GDI, N fps, JPEG) and pack them on the PC (PIL: GIF + contact sheet), sent to the user.
- CORRECTION (13:4x): the white GDI shots + all-zero scan-out after the foobar run were NOT display sleep - VIDEOIDLE
  and STANDBYIDLE are 0 (never) on AC and DC; `tasklist` showed scrnsave.scr (the blank screensaver, the same one
  that voids cube fps). taskkill /f -> shot 1520x720 normal again. The earlier "display asleep" readings (G20/G21
  white shots / black dumps) were probably the screensaver too. MS Learn SetThreadExecutionState: "This function does
  not stop the screen saver from executing." -> the recorder turns the screensaver off with
  SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, FALSE) for its run and restores the previous value afterwards.
- screenrec.ps1 v1 was blocked by Defender/AMSI on the phone ("This script contains malicious content and has been
  blocked by your antivirus software", ScriptContainedMaliciousContent) - screen capture + P/Invoke in one script
  reads like spyware. Not worked around (no obfuscation / splitting to dodge AV). Replaced by a normal compiled test
  tool: tools/phone/rec/screenrec (C++, GDI BitBlt + WIC JPEG, same behaviour), launched by a one-line wrapper. If
  Defender flags the exe as well: stop, the user decides about an exclusion.
- screenrec.exe (C++, /W4 /WX clean, Windows subsystem so no console window lands in the frames) runs - Defender
  did not object. Smoke run `recfetch.sh smoke 10 2`: 21 frames 760x360 (screen 1520x720 x0.5), screensaver-was 1 ->
  restored to 1; GIF 68 KB, contact sheet shows the full desktop + taskbar. Delivery to the user: SendUserFile.
- First real x64 D3D app: mpv x86_64 (shinchiro/mpv-winbuild-cmake 20261018, mpv-x86_64 - not -v3, which needs AVX2)
  in C:\S10eTmp\mpv64, `--vo=gpu --gpu-api=d3d11 --no-audio av://lavfi:testsrc2` (no media file, no sound).
  - Attempt 1 (recorded): Windows "System Error: vulkan-1.dll was not found" - this mpv imports the Vulkan loader.
    Fix: Khronos loader x64 from LunarG vulkan-runtime-components.zip (1.4.363.0, Authenticode valid "LunarG, Inc."),
    app-local next to mpv.exe (no Vulkan ICD exists -> mpv uses D3D11 as asked).
  - mpv log: "Using Direct3D 11 feature level 10_1", "Device Name: Arm Mali-G76 MP12 (Galaxy S10e, G10/G11)", flip
    model, R8G8B8A8; `tasklist /m`: mpv.exe "maliumd.dll, maliumd_ec.DLL". 0 [e] lines, no dropped frames (only the
    EOF pin disconnect). Recording traces/rec/mpv64run (80 s, 161 frames): colour bars + moving lines + counter
    render correctly from t=3.5 s to the 45 s length, then mpv idles ("No file", black).
  - Recorder fixes on the way: every BitBlt failed while scrnsave.scr ran (it starts right after a run restores
    SPI_SETSCREENSAVEACTIVE, the input idle having passed) -> screenrec closes a running screensaver before each frame
    (SPI_GETSCREENSAVERRUNNING; WM_CLOSE to the "Screen-saver" desktop windows or the foreground window if it is a
    *.scr process). The stale hard-error box from attempt 1 cannot be found/closed from the user session
    (tools/phone/rec/dismiss.ps1: FindWindow finds nothing; csrss-owned + UIPI) - needs one OK tap by the user.
- x64 browser: ungoogled-chromium 155.0.8059.79 windows_x64 (portable zip, C:\S10eTmp\chr64; only flag
  --no-first-run, no GPU flags) on https://webglsamples.org/aquarium/aquarium.html, recorded 60 s
  (traces/rec/chr64aq): the GPU process loads "maliumd.dll, maliumd_ec.DLL"; window at ~21 s, page at ~31 s, textures
  streaming 34-41 s (white/blue scenes = not yet loaded), full aquarium from ~44 s: fps counter 24-27 at 500 fish,
  canvas 1024x1024. one "screensaver was running (close path 1)" line, before frame 0 (closed via the Screen-saver desktop, i.e. before the launch); not running afterwards (tasklist) -> number valid; it includes
  the recorder's own 2 fps GDI capture. The stale mpv hard-error box was gone by then.
- screenrec -dup (DXGI Desktop Duplication, CAP=dup in recfetch.sh): duplication reports ModeDesc 1520x720 rotation 2
  (ROTATE90) but the acquired surface is 720x1520 (un-rotated, as MS Learn says) - the first build sized its staging
  texture from ModeDesc and CopyResource silently did nothing (all-black frames, byte sum 0); staging now follows the
  acquired texture. dup3: desktop upright + complete (rotation mapping right). Device = our Mali (FL 10_1, ARM64 UMD).
- dupfs: fullscreen x64 mpv (--fs, 15 s) captured 7-23 s (GDI gives black for this) -> fullscreen capture works.
  !! HARD RESET at the end (~23.5-25 s): Kernel-Power 41, no BugCheck 1001, no dump (= reset outside Windows' bugcheck
  path, like earlier GPU-hang resets); frames.txt logged to 0046 / 23.0 s, frame_0047 truncated, 0048/0049 present
  but empty (lost lazy writes) -> the recorder ran to its end (50 frames). mpv's playback ends ~22 s (start ~7 s +
  15 s) -> suspects: mpv leaving fullscreen / exiting (x64, maliumd_ec) or the duplication teardown at screenrec exit.
  Triage next: A = mpv --fs exit without the recorder; B = duplication of a windowed app, then exit.
