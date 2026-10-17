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
