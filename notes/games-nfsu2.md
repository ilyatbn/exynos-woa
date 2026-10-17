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
