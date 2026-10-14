# Parked: make the Windows shell feel like an Android home screen (user, 2026-10-14)

User: "keep the information on explorerpatcher and how it works. we might hack something together later to make
explorer feel more like an android home screen."

Full research: [notes/explorerpatcher-touch.md](../notes/explorerpatcher-touch.md) (agent report, 2026-10-14). The parts
that matter for a home-screen hack:

## How ExplorerPatcher works (the reusable idea)
- It is a DLL that the shell loads by **proxying a system DLL**: EP installs its own `dxgi.dll` in `C:\Windows`
  (loaded by explorer.exe for the taskbar/desktop) and in the StartMenuExperienceHost folder. Inside explorer it hooks
  functions (taskbar, Start, Alt-Tab, context menus) and swaps in Windows 10 implementations or patches behaviour.
- Settings live in `HKCU\Software\ExplorerPatcher` and Explorer/Start policy values (e.g. `ForceStartSize` 2 = full-screen
  Start). Updater checks GitHub at every logon (notify only; switchable); optional symbol downloads; no telemetry.
- Native ARM64 installer `ep_setup_arm64.exe` in every release; latest stable 26100.8457.70.3 (2026-06-20).
  22631 (23H2) is only listed as tested on x64; ARM64 23H2 fixes exist in older releases (Win10 Start not opening,
  Alt-Tab, Win+X). Known: new Win11 Start + EP Win10 taskbar on 22H2/23H2 = Explorer crash loop.
- Recovery: `ep_setup_arm64.exe /uninstall`; black screen: Ctrl+Alt+Shift while starting explorer (needs a keyboard -
  attach USB-OTG); offline: delete EP's dxgi.dll copies from TWRP.

## Why not EP as-is
- Its Win10 taskbar and menus are mouse-era: net negative for touch. Its only touch-relevant feature is the
  full-screen Win10 Start. Risk on this phone: the dxgi.dll proxy sits in front of DirectX in explorer and
  StartMenuExperienceHost, both of which already stress our Mesa/WDDM stack (StartMenuExperienceHost crash-looped once).

## Ideas for the hack (when picked up)
- A full-screen launcher window (icon grid + dock, swipe pages) as the shell replacement for the desktop, or an app
  that sits on the desktop layer; EP-style injection only if a launcher cannot do it.
- Built-in tablet behaviour first: `ConvertibilityEnabled` = 1 (applied 2026-10-14) for the touch taskbar.
- Gestures: edge swipes / back gesture via a helper reading pointer input; the hardware keys (S10eKeys) for back/home.
- Must stay light on composition (XAML/DComp is where the GPU stack is fragile).
