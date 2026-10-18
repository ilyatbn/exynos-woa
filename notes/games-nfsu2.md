# Games: Need for Speed Underground 2 (Direct3D 9) on the S10e

User 2026-10-17: game copied to C:\Games\src (src.zip, sha256 3cfc69db..., 1261 files / 1,815,634,329 B), pillarbox
mod (NFSU2Pillarbox.zip: .asi/.ini/.c) extracted into C:\Games\src\SCRIPTS next to the existing
NFSUnderground2.WidescreenFix; ASI loader = the game folder's dinput8.dll. User: "the game won't launch at all, no
errors". SPEED2.EXE = 32-bit x86 (timestamp 0x420aad27).

## Progress log
- 22:23 tools/phone/games/gamelaunch.sh (+ gamelaunch.ps1, appevents.ps1): SPEED2.EXE dies ~2 s after start,
  Application Error 1000: c0000005 at SPEED2.EXE+0x1d2790 (same offset in the user's own attempts at 19:21).
- 22:24 isolation: dinput8.dll (ASI loader) renamed -> SAME crash -> not the mods (WidescreenFix / Pillarbox).
  Loader restored.
- 22:25 WER LocalDumps (documented: HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\SPEED2.EXE,
  DumpType 2, DumpFolder C:\S10eTmp\dumps; left in place, only affects SPEED2) -> full dump
  traces/games/SPEED2.EXE.7436.dmp, cdb (traces/games/cdb7436.txt):
  - fault: `mov ecx,[eax]` with eax = 0 (NULL read) in the game's own code, ebx 0x1e0 / edi 0x280 = 640x480 -> the
    display / device setup path;
  - d3d9.dll loaded; **maliumd32.dll loaded and UNLOADED 4 times** (unloaded-module list) -> the D3D9 runtime tries our
    UMD (UserModeDriverNameWow), finds no D3D9 DDI (our UMD exports only the D3D10/11 OpenAdapter10 / OpenAdapter10_2),
    device creation fails, the game uses the NULL device without checking -> silent crash.
  => Root cause: no Direct3D 9 support in our driver (expected; research request R9 in
  docs/exynos9820-port/research-requests.md). Not a resolution problem - the pillarbox question comes after this.
- Options: (1) native: a D3D9 user-mode DDI in our UMD (d3dumddi.h OpenAdapter + the D3D9 DDI; Mesa has no D3D9 UMD -
  its "nine" state tracker implements the D3D9 COM API on Gallium, not the DDI) - a large project; (2) D3D9On12 needs a
  D3D12 driver - no; (3) user-side wrapper in the game folder translating D3D9 -> D3D11 (e.g. dgVoodoo2) - would run
  on our D3D11 driver; not a native fix (AGENTS.md rule 12), only the user's own workaround for this game.
- 22:30 dgVoodoo2 2.87.5 (GitHub release, zip sha256 5ffde692...; MS/arm64_chpe_x86/D3D9.dll = CHPE, native ARM64 code in an x86 DLL) + dgVoodoo.conf (OutputAPI d3d11_fl10_1, ScalingMode stretched_ar, watermark off) into C:\Games\src. Game got further (ws 190 MB) then abort() (exit 3, 0x40000015 in ucrtbase): our UMD's stderr = 'NIR validation failed TTN': txf on a 2D texture with a 1-component offset. Root cause (Mesa tgsi_to_nir.c): nir_tex_instr_src_size(instr, n) is called before instr->src[n].src_type is set, and nir_tex_instr_create gc_allocs the sources uninitialised -> garbage size. Same pattern at the TXD ddx/ddy sites and in our 0077 sample path. Fix 0106: set src_type first at all four sites.
- 22:33 Mesa 0106 (x86 8f1dca84, live in SysWOW64): abort gone; game now crashes at SPEED2+0x1d2527 (NULL device
  again, dump traces/games/SPEED2.EXE.2400.dmp). UMD log: dgVoodoo's D3D11 device created, caps queried, one buffer,
  then dropped after ~1.5 s with no UMD error. FullScreenMode false: same.
- 22:40 tools/phone/games/dbgcap.c (OutputDebugString capture, documented DBWIN protocol; first build wrote into the
  read-only view and died on the first message - fixed, self-test odstest2.ps1 OK) + dgVoodoo DEBUG D3D9.dll
  (dgVoodoo2_87_5_dbg.zip, arm64_chpe_x86): traces/games/dbgcap-1.txt:
    "[dgVoodoo] ERROR: DDBase: Cannot initialize the DDI object."
    "[dgVoodoo] ERROR: Direct3D9 Initialization failed: No any output device is found. Invalid adapter or an adapter
     with unplugged/invalid display output is specified in the config?"
- 22:45 tools/phone/games/dxgienum.c (ARM64 + x86 builds, documented DXGI enumeration), run in the user session
  (session 0 over ssh has no outputs at all): our adapter has 1 output (\.\DISPLAY1, attached, rotation 2), but
  GetDisplayModeList(SCALING) lists modes ONLY for B8G8R8A8_UNORM(_SRGB) (1520x720 x2 + 2280x1080); R8G8B8A8_UNORM,
  R8G8B8A8_SRGB, B8G8R8X8, R10G10B10A2, B5G6R5 = 0 modes. Same for 32-bit. Our UMD's CheckFormatSupport answers 0x1f for
  both 28 (R8G8B8A8) and 87 (B8G8R8A8), so the difference is the KMD: every source mode is D3DDDIFMT_A8R8G8B8 (= DXGI
  B8G8R8A8). dgVoodoo (and D3D11 games that enumerate RGBA modes) find "no output".
  Docs (rule 13): learn.microsoft.com IDXGIOutput::GetDisplayModeList gives no format mapping rule; D3DKMDT_GDISURFACEDATA
  lists D3DDDIFMT_A8B8G8R8 among WDDM formats; the D3D11.1 DISPLAY / WDDM3 DISPLAYABLE format-support bits are newer
  than our UMD's header level. => fix in the KMD: offer every source mode in A8B8G8R8 as well, and handle an RGBA
  primary at present (UMD GPU blit converts formats by itself; the KMD CPU copy must swap R/B).
- 22:53 KMD 1.0.69.1 (s10e-artifacts\mali-kmd-69, sys aba061b9; Parameters\RgbaModes default 1, every source mode also
  A8B8G8R8; KMD CPU copy swaps R/B for an RGBA source) deployed from TWRP (prev af205f42), boot OK: dxgienum now lists
  R8G8B8A8_UNORM(_SRGB) modes 1520x720 x2 + 2280x1080, same as B8G8R8A8. dgVoodoo STILL fails ("Cannot initialize the
  DDI object" first, then "no output device"); MaxTraceLevel 2 adds nothing.
- 22:58 What reached our UMD before dgVoodoo gave up (traces/games/malikm-SPEED2.EXE-2400.txt): CheckFormatSupport of
  formats 70-84 = 0x0, i.e. BC1-BC5 (DXT1/3/5, ATI1/2) NOT supported. FL 10.x requires BC1-BC5; dgVoodoo needs them for
  D3D9 DXT textures. Panfrost takes BC support from TEXTURE_FEATURES_0; new tool drivers/MaliG76Wddm/tools/gpuinfo
  (MW_ESCAPE_GPU_INFO): GPU_ID 0x7211, TEXTURE_FEATURES0 = 0xf7fe007e -> bits 1-6, 17-26, 28-31 set; bits 7-16 CLEAR.
  Panfrost genxml v7 texture format enum: 7 BC1 .. 16 BC7 => THIS CHIP HAS NO BCn TEXTURE DECODE (Samsung
  integration); ETC2 (1-4, 17-19) and ASTC (20-23) are present.
  => Native fix (G22 next): BC1-BC5 (and BC6H/BC7) emulation in the UMD - report the formats, back BC resources with
  RGBA8 (BC4/5: R8/RG8), decompress on upload (UpdateSubresourceUP, Unmap of written maps, copies in texel space) with
  Mesa's util_format S3TC/RGTC/BPTC unpackers. Needed by most D3D10/11 PC games too, not only dgVoodoo.

## USB-C to A dongle: keyboard does not light up (user, 2026-10-17 ~22:55)
- tools/phone/usbstate.ps1: ACPI\PNP0D10\0 (XHC0, the USB host controller) known to Windows but NOT present; no USB
  devices; BCD debug No. tools/phone/uefilogusb.ps1 (S:\UEFILOG.TXT, last written 10-15 11:47 - newer images no longer
  write it): "UsbHost: KD device bring-up Success, UFN0 visible to Windows, XHC0 hidden" - the M14/M17 KD image family
  (BEYOND0LTE_USB_KD_DEVICE 1) keeps the USB-C port in DEVICE mode for KDNET: no VBUS boost, no xHCI -> a passive
  OTG dongle gets no 5 V. Host mode itself worked in Windows on 10-11 (notes/m16-usb.md runs 13-16: dongle keyboard +
  mouse, dock, USB stick; OTG_ILIM 1.2 A); MILESTONES M10 rows are stale.
- Built (not flashed) a host image: s10e-artifacts/boot-host-gpuon-nowdt.img = HEAD 5c445be + build-only
  GPU_POWER_ON 1 + WINDOWS_WATCHDOG 0 (the HANDOFF §39 interactive recipe; USB_KD_DEVICE 0 is HEAD's default).
  Flashing needs the user's go-ahead; back up the current boot partition first (its image is not recorded).
- 23:08-23:13 FLASHED (user: "if you don't need the debug firmware for the game, proceed"): tools/phone/bootflash.sh
  (backup -> flash -> readback -> rm WINRUN.FLG). The phone had been on **boot-vidc2.img** (b94fcbfb, backup
  s10e-artifacts/boot-backups/boot-before-20261017-230830.img). First host image (HEAD defaults) would have dropped
  Bluetooth / MFC (BT_UART_PROBE and MFC_* default 0 in HEAD; vidc2 = btmfc3 + VIDC gate with them on) - replaced
  before booting by **boot-host-btmfc.img** (6db74a58) = HEAD 5c445be + GPU_POWER_ON 1, WINDOWS_WATCHDOG 0,
  BT_UART_PROBE 1, MFC_PROBE/POWER/FWBOOT 1, USB_KD_DEVICE 0 (host). Boot OK: ACPI\PNP0D10\0 xHCI + USB 3.0 root hub
  OK; Bluetooth radio + paired devices, Exynos9820 MFC Video Codec, BCM4375 BT transport, Wi-Fi, Mali, audio, touch
  OK (tools/phone/devcheck.ps1); only problem devices = CPU 6/7 (parked M4s, as before). Keyboard test = user.

## 2026-10-18 recorded runs (tools/phone/rec screen recorder + tools/phone/games/gamedrive.ps1 key script)
- nfs1 (traces/rec/nfs1, x86 UMD 0128): game fullscreen ~23 s (black frame), then Windows' "Get an app to open this
  'ms-gamingoverlay' link" popup (~27 s) takes the foreground, the game minimises (taskbar icon 39-58 s) and crashes
  at SPEED2+0x6562b (21:42:01, pid 0x10D8 = our 4312) - the known fault when its fullscreen swapchain loses the
  foreground. = the user's "annoying link popup ... minimize the game". Esc presses at 31/34/36 s did not clear it.
- tools/phone/games/gamebarstate.ps1: GameConfigStore GameDVR_Enabled 1 (Windows default), no Xbox / GamingOverlay
  package, no ms-gamingoverlay protocol handler -> Windows tries to open Game Bar for a detected game and this image
  has no Game Bar app. Not our driver. Native fixes for the user to choose: install Xbox Game Bar (stock state) or
  turn Game Bar / captures off in Settings > Gaming. For the race test only (diagnosis switch, AGENTS 12):
  GameDVR_Enabled = 0 for the run, restored to 1 afterwards.
- nfs2 (GameDVR_Enabled 0 as a diagnosis switch): popup still there (even at 11.5 s), game gone again -> that value is
  not the trigger; restored to 1 at once. Research: MS Q&A https://learn.microsoft.com/en-us/answers/a/1429604 - the
  popup appears "when opening a game after removing Xbox Game Bar"; fix = reinstall Xbox Game Bar
  (`winget install 9NZKPSTSNW4P`) = back to stock Windows (AGENTS 12 native). Third-party pages (windowsdigitals.com,
  macmyths.com): Game Bar triggers fire on game launch / Win+G / controller; the handler is missing after removal.
- winget install 9NZKPSTSNW4P (tools/phone/games/gamebarinstall.ps1, user session): "Failed to install or upgrade
  Microsoft Store package. Error code: 0x80004002" (E_NOINTERFACE) - this image's Store install path does not work.
- nfs3: the popup of nfs2 was still up (OpenWith.exe, no close button, persists) -> no information. OpenWith killed.
- nfs4 (GameDVR_Enabled 0 + AppCaptureEnabled 0 = the Settings > Gaming capture toggle, diagnosis switch): NO popup;
  game ran fullscreen 10-75 s (GDI frames black - GDI cannot capture a fullscreen D3D game; scan-out dump needed).
  It crashed at 21:54:52 (+0x6562b, pid 6684) ~1 min after the recording ended: the recorder restored the screensaver,
  which then started (idle long passed) and took the foreground -> any foreground loss kills the game (dgVoodoo
  fullscreen), the popup was just the usual cause. Settings restored exactly (gamedvrrestore.ps1: GameDVR_Enabled 1,
  AppCaptureEnabled removed). Open: user's choice of native fix; race test needs the game kept in the foreground
  (popup off, no screensaver) + a way to see fullscreen frames (scan-out dumps or a DXGI desktop-duplication capture).
- 2026-10-18 user: "Okay to switch it off permanently" -> Settings > Gaming capture off (GameDVR_Enabled 0 +
  AppCaptureEnabled 0, gamedvr.ps1) applied for good - the chosen native fix (a normal user setting, no flag on the
  game). Reboot next (also loads the new S10eDiag), then the race test.
- 15:40 after reboot: ring shows 'AuxKlib init 0000000000000000' (new S10eDiag loaded, init OK). nfs5: -dup capture, no keys, capture off for good.
- 15:53:48 nfs6: gamedrive (screensaver held off) via runapp, Esc x3 at 25-31 s, scan-out dumps
- 15:57:34 nfs6 (0128 x86): game alive ~260 MB but the WHOLE panel black (scan-out extrema 0 x3 incl. after Enter; GDI black). Bisect: x86 0123 into SysWOW64 (new wddm2dbg umd32swap), relaunch (nfs7).
- nfs6/nfs7 black screens: NOT the UMD (0123 x86 the same) - TWO scrnsave.scr were running (one from my scrnsave /s
  test, one already running when gamedrive started - SPI_SETSCREENSAVEACTIVE 0 does not stop a running one); with the
  screensaver up GetForegroundWindow = 0, GDI black, scan-out black. taskkill -> scan-out shows the NFSU2 title screen
  ("Press Enter Key", 4:3 window, rendered on Mali) (traces/wddm2/nfs7c/d).
- Input: every sendkeys.ps1 run (a new process in the session) takes the foreground for a moment and the game
  minimises (not a crash, exit-free) -> all input now from the one gamedrive process (CLICK added, closes a running
  screensaver at start). A CLICK during the intro (t+18 s) made the game exit cleanly (code 0).
- nfs9 (0123 x86, gamedrive: CLICK at 60 s, ENTER x2): intro video at ~53 s and, after Enter, a menu screen (grey panel,
  yellow labels, "MAIN" at the right) - game now fullscreen 16:9 - BOTH dumps with regular vertical black stripes
  (~30 px) over the image (traces/wddm2/nfs9a/b). The windowed 4:3 title dumps had none. Unknown whether the panel
  shows stripes (real rendering bug in the fullscreen path) or the dump misreads the fullscreen scan-out buffer -
  needs the user's eyes or a second capture method. x86 UMD back to 0128 afterwards (umd32swap).
- Still open: the race itself (menu path Quick Race -> track -> car -> race not yet driven).
- Stripe check (column analysis, middle band of the upright 2280x1080 image, columns fully black < 8): nfs9a 794 black
  columns, nfs9b 1238, only 410 shared; runs a: 229/12, 319/31, 382/51, 607/21, 699/30, 790/29 ...; b: 146/49, 232/39,
  323/38, 419/45, 509/48 ... -> positions and widths differ between dumps, spacing ~90-95 px in both. A static
  rendering defect would sit in the same columns; this leans to a capture-timing artefact (the uncached dump reading
  a buffer that is being rewritten in strips), not proven. The user's look at the panel decides.
- 2026-10-18 ~19:1x user back: dgVoodoo.conf switched to exclusive fullscreen (FullScreenMode=true, WindowedAttributes empty; previous kept as dgVoodoo.conf.bak-borderless) - user launches from the desktop icon after the hub reboot.
- ~19:3x user: exclusive fullscreen is portrait + bars again (the documented dgVoodoo NONPREROTATED case, g24 notes l.107) and 'you fixed it yesterday already' -> dgVoodoo.conf restored to the borderless, fullscreensize config (the working state); MS Learn DXGI_DDI_PRIMARY_DESC: NONPREROTATED = 'applications will handle the output orientation by rotating, for example, the viewport and projection matrix' (dgVoodoo does not). Next: screenshot of the user's icon launch to see what 'windowed' means.
- ~19:4x user: "it worked fine before in full screen" (yesterday's "upper left corner now ... nice" = exclusive
  fullscreen with x86 0120-0122) -> regression suspect 0123-0128. Bisect: exclusive fullscreen config again + x86
  0122 in SysWOW64 (20e38827). Pitfall: wddm2dbg umd32swap's SYSTEM task stayed "Queued" - the phone is on battery
  (hub without PD power: 94 %, BatteryStatus 1) and schtasks /create tasks default to AC-only; the stale log made the
  swap look done. Swap done directly in the admin ssh session; wddm2dbg task() now uses systask.ps1
  (AllowStartIfOnBatteries). User test pending.
- User test with x86 0122 + exclusive fullscreen: "portrait with bars" -> NOT a 0123-0128 regression. Correction of
  the premise: yesterday's "upper left corner now ... nice" was the d3dfs test tool (its corner rectangles), not
  NFSU2; NFSU2 exclusive fullscreen was already sideways then (g24 notes: dgVoodoo NONPREROTATED primary, MS Learn
  DXGI_DDI_PRIMARY_DESC: "applications will handle the output orientation" - dgVoodoo does not) -> the borderless
  config was the answer. Restored: x86 0128 (systask path works on battery, hashes match) + borderless config. Next:
  see what the user calls "windowed" with that config (screenshot during an icon launch).
- User: in a race (loads now = G26 memory fix validated in game), extremely slow; lowering the in-game overall quality reset the device and dgVoodoo went exclusive fullscreen (portrait + bands; scan-out traces/wddm2/nfsuser1.png - the bands are REAL). Cause per dgVoodoo docs (VOGONS t.1413822): forced windowed needs FullScreenMode=false AND AppControlledScreenMode=false; ours had AppControlledScreenMode=true -> set false (backup dgVoodoo.conf.bak-appctl). Open: the bands in the exclusive-fullscreen present path (our bug), in-race speed (profile).
- AppControlledScreenMode=false -> user: black screen, game exited; WER 14:36:44 (phone clock) SPEED2+0x6562b = the known game use-after-free when dgVoodoo recreates its present buffers at an unexpected size (g24 notes). Reverted to AppControlledScreenMode=true (dgVoodoo.conf.bak-appctl). The quality setting the user lowered is saved by the game, so the launch should not need another device reset.
- Correction: the user's NEXT launch (pid 1160, started before the revert, so with AppControlledScreenMode=false) worked, and a quality change kept it windowed - the black-screen exit was likely the relaunch colliding with the still-running fullscreen instance. AppControlledScreenMode=false re-applied. User tests in a race next.

## In-race performance (2026-10-18, user: "cpu/gpu dont peak but the framerate is pretty horrible")
- threadtop: SPEED2 main thread 98 % of one core (cpu4 = A75 MID 96 % in coretop), everything else idle -> single
  thread CPU-bound; the GPU waits. DWM 18 %.
- UMD frame profile (Mesa 0104, MALIKM_TGSI_DIR on; previous instance pid 1160 = the user's race; umdlog.ps1 now has
  -ProcId / -Match because the newest-by-time pick chose a log from an earlier boot):
  frame 85-111 ms (~9-12 fps), inside the UMD 56-77 ms/frame (66-69 %); per 120 frames: DrawIndexed ~110k calls
  ~5000 ms (~920 draws/frame, ~44 us each = ~40 ms/frame), ResourceMap ~180k calls ~930 ms (~8 ms/frame), _Present
  ~350 ms, Flush ~240 ms; ioctls: 0x40 (submit) 1680 calls ~580 ms.
- The x86 UMD (maliumd32) runs EMULATED inside the x86 process (no ARM64EC/CHPE path for x86 processes), as do the
  game and dgVoodoo. Lever: per-draw CPU cost in DrawIndexed (d3d10umd state resolve incl. the 0125-0128 GS/SO
  per-draw checks, vs panfrost job/descriptor emission). Next: finer timers inside DrawIndexed.
- In-race section timers (x86 0130p, pid 4448): DrawIndexed 117k calls 5526 ms of which DrawIndexed:draw_vbo 5378 ms (97 %) = Panfrost; ResolveState not in the top 8. Panfrost has no threaded_context in this tree; d3d10umd creates a plain context (Device.cpp:178). All UMD builds are debugoptimized with b_ndebug=if-release = asserts ON (MESA_DEBUG 0). Test: x86 0130n = 0130p + -Db_ndebug=true (9eca4967) in SysWOW64. User question 'will DX12 solve this' -> no: the cost is Panfrost per-draw CPU work in an emulated x86 process; a D3D12 UMD does not exist for this GPU.
- Asserts-off x86 0130n (pid 3040): frame 85-97 ms, DrawIndexed 109k calls 4834 ms = 44.3 us/draw (0130p: 47.1) - ~6 %, noise level; user 'feels the same'. Asserts are not the cost. Next: section timers inside panfrost draw_vbo.
- x86 0130s (32ca4991) = asserts off + Panfrost per-draw section timers (pan:prepare_draw, update_state_3d, shader_state_vs/fs, varyings, launch_draw via umd_section_ns) + report top 14 + malikm_tlog line 256 -> 1200 B (the 'app umd top' lines were truncated at 255 chars). In SysWOW64; user race next.
- Panfrost sections (x86 0130s, pid 4384, ~1240 draws/frame, frame 105-122 ms): per draw draw_vbo 46.6 us = shader_state_fs 10.9, prepare_draw 10.8, launch_draw 9.1, varyings 5.2, shader_state_vs 5.2, update_state_3d 2.0 (sum 43.2). Spread, no single hotspot. Threaded context would overlap ~35 ms game vs ~70 ms driver (still emulated x86) -> ~1.4x at best. Next: redundant work in prepare_draw / shader_state_fs (dgVoodoo ~1600 maps/frame may dirty FS state every draw).
