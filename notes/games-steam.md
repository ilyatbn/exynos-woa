# Steam / indie games on the S10e (2026-10-19)

## Stardew Valley (Steam 413150, x64 .NET 6 / MonoGame DesktopGL = OpenGL)
- User: Steam installed, "so far it doesnt even launch". Event 1026: NoSuitableGraphicsDeviceException ->
  PlatformNotSupportedException "MonoGame requires either ARB_framebuffer_object or EXT_framebuffer_object" - no
  OpenGL ICD for the Mali adapter, Windows' GDI OpenGL 1.1 has no FBOs. Steam itself (x64 steamwebhelper) runs.
- User chose "Both": Microsoft "OpenCL, OpenGL, and Vulkan Compatibility Pack" (Store 9NQPSL29BFFF, Mesa GL on D3D12;
  no D3D12 GPU here -> WARP, CPU rendering) now, a real Panfrost OpenGL ICD as the next milestone.
- Install: winget msstore 0x80004002 (also earlier for Game Bar). The Store app is not installed; UsoSvc / DoSvc /
  WaaSMedicSvc disabled by tools/phone/wuoff.cmd. User: "re-enable windows update for a sec, install it, then disable
  it again". sc config on DoSvc = error 5 even as SYSTEM (protected) -> tools/phone/wutemp-reg.cmd 3 (registry Start,
  SYSTEM task), reboot, UsoSvc demand (wutemp-on.cmd), winget install -> "Successfully installed"; then
  wutemp-reg.cmd 4 + wutemp-off.cmd: DoSvc Start 4, UsoSvc Start 4, both STOPPED, wuauserv manual as before,
  WU policies untouched. Scripts: tools/phone/games/glcompat-install.ps1, tools/phone/storestate.ps1,
  tools/phone/games/recentcrash.ps1.
- Stardew launch test after the install: not run to the end - the user was playing NFSU2 at the time (scan-out
  traces/wddm2/stardew1.png = NFSU2 pause menu); user to try.
