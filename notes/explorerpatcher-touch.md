# ExplorerPatcher + touch-friendliness research (Win11 23H2 22631.6199 ARM64, S10e)

Research only, no device changes.

## Progress log

- 2026-10-14: file created; starting web research (EP README/wiki/releases, MS docs).

### F1 - EP releases / ARM64 / 22631 (GitHub API, releases page)
Source: https://github.com/valinet/ExplorerPatcher/releases
- Latest: 26100.8457.70.4 (2026-07-06, marked **prerelease**); latest stable 26100.8457.70.3 (2026-06-20).
  Every release ships `ep_setup.exe` (x64) and `ep_setup_arm64.exe` (ARM64 native). README: "ep_setup_arm64.exe if your device uses a Snapdragon processor".
- 70.x "Tested on OS builds" includes **22631.7079 (x64)**; ARM64 testing listed only on 26100.6899 / 28000.x. So 23H2 is tested but only on x64; 23H2 ARM64 is not in any tested list. 22631.6199 (KB5068865, Nov 2025) is older than the tested 22631.7079.
- 69.5 item 5: "Start10: Fixed Windows 10 Start menu not opening on 22H2/23H2 ARM64" (c08b0a6) -> there were real ARM64-on-23H2 bugs; fixed since 69.5 (Nov 2025).
- 69.3: "Fixed Windows 10 Alt+Tab and Windows 10 taskbar Win+X functionality on ARM64 226x1" -> another ARM64 22631-specific fix.
- 69.6 known issue: "On Nickel (Windows 11 22H2/23H2), when the new Windows 11 Start menu is enabled, using Windows 10 or Windows 10 (ExplorerPatcher) taskbar will crashloop explorer.exe." (new Start menu is a 24H2+/feature-flag thing; on stock 22631 it should be off, but don't enable it via ViVeTool.)
- 69.x: "On Windows 11 22H2+, Windows 10 (ExplorerPatcher) taskbar a.k.a. ep_taskbar is now used by default." i.e. installing EP immediately replaces the Win11 taskbar.
- Release notes CAUTION: turn Smart App Control off first or explorer.exe may not start; 3rd-party AV exclusions.
- Release artifacts mention `dxgi.dll` -> EP's injection vector is a proxy dxgi.dll in C:\Windows loaded by explorer. RELEVANT for our homebrew WDDM/Mesa stack: EP sits in explorer's DXGI load path.
- No KB5068865-specific breakage report found in a search (elevenforum/neowin) - absence of evidence only.

### F2 - EP install/uninstall/recovery/updates/symbols (wiki raw pages)
Sources: https://github.com/valinet/ExplorerPatcher/wiki/Installer-How-To , /wiki/Frequently-asked-questions , /wiki/Configure-updates , /wiki/Symbols
- Install: run `ep_setup_arm64.exe`, elevates, restarts explorer; Win10 taskbar appears immediately (default style).
- Uninstall (5 ways): Properties > Uninstall; Settings > Apps; Control Panel > Programs; `ep_setup.exe /uninstall`; rename to `ep_uninstall.exe`. `ep_setup.exe /extract [dir]` to inspect files without installing.
- Black screen recovery (FAQ verbatim): Ctrl+Shift+Esc > Run new task > `explorer.exe`, **hold Ctrl+Alt+Shift while pressing Enter** until taskbar shows -> explorer starts without EP; then revert/uninstall. NOTE for the phone: needs a keyboard (we have no physical one; USB-C OTG keyboard or the remote/RDP path). Safe Mode is NOT mentioned in the wiki verbatim.
- FAQ lists EP files: `C:\Program Files\ExplorerPatcher`, `%APPDATA%\ExplorerPatcher`, `C:\Windows\dxgi.dll`, plus StartMenuExperienceHost / ShellExperienceHost SystemApps folders -> EP drops a proxy **C:\Windows\dxgi.dll** (explorer loads it from its own dir before System32). Manual last-resort removal (my inference, from TWRP/offline NTFS): delete/rename C:\Windows\dxgi.dll (and the dxgi.dll copies in the two SystemApps folders).
- Updates: **auto-check ON by default** at every explorer start (logon) if online, contacting github.com/valinet/ExplorerPatcher/releases/latest (and api.github.com for prereleases); default action = notify only, not auto-install. Disable: Properties > Updates > "Do not check for updates". Independent of Windows Update. "no telemetry".
- Symbols: if the build lacks built-in symbol data, EP downloads PDBs for twinui.pcshell.dll / StartDocked.dll from Microsoft's symbol server (msdl) at explorer start and caches in registry; can be disabled in Properties > Advanced > "Enable symbols download". Win+X menu etc. need it.

### F3 - EP source facts (shallow clone of master, 2026-10-14; scratchpad, not executed)
- ep_setup.c installs proxy `dxgi.dll` (EP's arm64 DLL renamed) into `C:\Windows` and into `C:\Windows\SystemApps\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy` (+ ShellExperienceHost per FAQ), plus `wincorlib.dll` (ep_startmenu) w/ symlink `wincorlib_orig.dll` in the Start host folder. So EP runs inside explorer.exe AND StartMenuExperienceHost by sitting in their DXGI load path -> every DXGI call from the shell passes through EP's forwarder before reaching System32\dxgi.dll and our Mesa/WDDM UMD. Extra risk surface on our fragile D3D11 composition stack (a crash there = no shell).
- Taskbar style options (en-US.rc): "Windows 11 (default)", "Windows 10" (explorer's built-in legacy taskbar, still present on 22631), "Windows 10 (ExplorerPatcher)" = ep_taskbar reimplementation.
- **Start menu "Display mode": Default / Start menu / "Full screen Start"** (only with Win10 Start style). Implemented by writing policy `HKCU\SOFTWARE\Policies\Microsoft\Windows\Explorer\ForceStartSize` DWORD (1=menu, 2=full screen) (GUI.c:411). This is the single genuinely touch-oriented EP option (Win10 tablet-style full-screen Start with live tiles). Requires the Win10 Start menu (Start10), which depends on StartTileData.dll remnants (fine on 22631; removed only in 26xxx.8474+).
- TwinUIPatches.cpp references `DWMTARGET_LAUNCHERFULLSCREEN` / `_fullScreen` -> EP's Start10 animation patches handle full-screen launcher.
- All-features wiki: Win10 taskbar has option "Show touch keyboard button"; Alt-Tab styles Win11 / Win10 / NT / Simple Window Switcher (SWS); "Disable rounded corners for application windows"; "Disable the Windows 11 context menu" (classic menus directly).

### F4 - Win11 touch-optimized ("tablet") taskbar
- Shipped in KB5022913 (22621.1344, Moment 2, Feb 2023) -> present in 23H2 22631 (same servicing branch). MS text: "touch-optimized taskbar for 2-in-1 devices that you can use as a tablet... collapsed and expanded... swipe up and down on the bottom... automatically change... when you disconnect or fold back the keyboard... only works on devices that can be used as tablets and is on by default... Settings > Personalization > Taskbar > Taskbar behaviors ... 'Optimize taskbar for touch interactions when this device is used as a tablet'."
  Source: https://support.microsoft.com/en-au/topic/february-28-2023-kb5022913-os-build-22621-1344-preview-3e38c0d9-924d-4f3f-b0b6-3bd49b2657b9
- Registry (pureinfotech): HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced  ExpandableTaskbar DWORD 1/0. https://pureinfotech.com/enable-disable-taskbar-tablets-windows-11/
- Gate is device posture: needs Windows to consider the machine a convertible/tablet in slate posture (ConvertibleSlateMode=0 already true on the phone, notes/touch-keyboard.md) - also chassis type / PNP0C60 indicator (to verify, see F5). The setting is only shown on "devices that can be used as tablets"; ExpandableTaskbar=1 alone doesn't create posture.

### F5 - How Windows decides "tablet posture" (learn.microsoft.com) - KEY lever for the phone
Source: https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/settings-for-better-tablet-experiences
- "Windows 11 provides optimized experiences when tablets and 2-in-1 devices are in tablet posture. For example, the taskbar will provide larger space among the icons".
- Needs: SMBIOS Enclosure Type Tablet 1Eh / Convertible 1Fh / Detachable 20h, OR DeviceForm Tablet 2 / Convertible 5 / Detachable 6; AND ConvertibleSlateMode = 0. "If one of the Enclosure Type and DeviceForm is set as above, and CSM is 0, Windows will provide tablet experiences by default until a keyboard is attached."
- "For a tablet device or an attachable device without a keyboard, Windows will stay in desktop mode" if that info is missing.
- **"As of Windows 11, version 22H2, convertibility can be explicitly opted in or out using HKLM\System\CurrentControlSet\Control\PriorityControl\ConvertibilityEnabled registry DWORD. ... non-zero value indicates that the device is convertible. This value, if present, takes precedence over the Enclosure Type and DeviceForm checks."** -> applies to 23H2.
- Our firmware: no project SMBIOS chassis/enclosure code found (`rg -i chassis firmware` outside edk2 -> nothing; only Star2Lte PlatformBootManagerLib mentions smbios). Phone already has ConvertibleSlateMode=0 (notes/touch-keyboard.md). So the missing piece is probably ConvertibilityEnabled=1 (or DeviceForm=2, or SMBIOS type 3 enclosure 1Eh in our UEFI = durable fix). UNVERIFIED on device: current ConvertibilityEnabled / DeviceForm values; read with `reg query HKLM\SYSTEM\CurrentControlSet\Control\PriorityControl` and `reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OEM" /v DeviceForm` (DeviceForm reg location is the commonly-cited one, not stated on the MS page).
- DeviceForm values (https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-deployment-deviceform): 1 Phone, 2 Tablet, 5 Convertible, 6 Detachable. Use 2 (Tablet), not 1 (Phone) - only 2/5/6 count for tablet posture.
- Effect once in tablet posture: touch-optimized taskbar (ExpandableTaskbar default on), larger icon spacing, touch keyboard auto-invoke, gesture-friendlier behaviors. Reboot (or at least sign-out) needed for PriorityControl changes to be picked up - inference; ConvertibleSlateMode is read live via WM_SETTINGCHANGE "ConvertibleSlateMode" normally from the GPIO driver.

### F6 - Touch double-tap / press-and-hold / mouse double-click settings
- HKCU\Software\Microsoft\Wisp\Touch (Pen and Touch control panel, Touch tab):
  - TouchModeN_DtapDist DWORD 0..100 (0 tightest, 50 balanced, 100 widest) - https://www.ninjaone.com/blog/adjusting-touch-double-tap-spatial-tolerance/
  - TouchModeN_DtapTime DWORD 0..100 slider (double-tap speed) - https://www.elevenforum.com/t/change-touch-double-tap-speed-in-windows-11.6322/ (403 to fetch; direction from search summary: 0 slow .. 100 fast; UNVERIFIED direction -> set via the control panel slider instead)
  - TouchMode_hold DWORD 1/0 press-and-hold right-click on/off (phone already has 1, notes/m12-touch.md)
  - TouchModeN_HoldTime_BeforeAnimation / TouchModeN_HoldTime_Animation: press-and-hold "Speed" and "Duration" sliders - https://www.elevenforum.com/t/change-touch-press-and-hold-speed-and-duration-in-windows-11.6325/
  - registry edits: sign out/restart (guides); Control Panel UI applies immediately.
  - Control Panel path: Control Panel > Hardware and Sound > Pen and Touch > Touch > Double-tap / Press and hold > Settings (`control.exe tabletpc.cpl`).
- Mouse double-click (applies to promoted mouse in classic Win32): HKCU\Control Panel\Mouse DoubleClickSpeed (REG_SZ ms, default 500), DoubleClickWidth / DoubleClickHeight (REG_SZ px, default 4). Live via SystemParametersInfo SPI_SETDOUBLECLICKTIME (max 5000 ms) / SPI_SETDOUBLECLKWIDTH / SPI_SETDOUBLECLKHEIGHT with SPIF_UPDATEINIFILE|SPIF_SENDCHANGE; raw registry edit needs sign-out. https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow
- CRITICAL for this phone: notes/touch-quality.md shows the "double taps" the user sees are phantom tip-up/press pairs from the S6SY761 driver (TouchQueueAllUpReport on I2C error/overflow). Widening DtapDist / slowing DtapTime would turn MORE of those phantom pairs into double-clicks. If anything: tighter tolerance + faster DtapTime reduces accidental opens; the real fix is in the driver.

### F7 - EP option-by-option verdict for touch on a 540x1140-epx portrait phone (analysis)
- Taskbar "Windows 10" / "Windows 10 (ExplorerPatcher)": mouse-era taskbar, ~40 epx tall vs Win11 48 epx; no touch-optimized collapsed/expanded mode (that is Win11-taskbar only, F4); "never combine" labels waste the 540-epx width. Net NEGATIVE for touch. Only plus: draggable to any edge, "Show touch keyboard button" (Win11 has that too).
- Start "Windows 10" + Display mode "Full screen Start" (ForceStartSize=2): the one real touch win - Win8/10 tablet-style full-screen tiles. But: still StartMenuExperienceHost (XAML/composition, already seen crash-looping on this phone, notes/touch-keyboard.md) PLUS EP's dxgi.dll/wincorlib.dll shims injected into it; the same policy key ForceStartSize is a stock Win10 GPO but the stock Win11 Start ignores it (inference), so it only works through EP.
- Alt-Tab style: irrelevant without a keyboard (touch app switching = 3-finger swipe up / Task View).
- "Disable Windows 11 context menu" (classic menu directly): removes the "Show more options" second tap, and classic Win32 menus get wider touch padding when opened by long-press (pureinfotech / winaero: Win10 adaptive context menus). Mild positive for touch, but the full legacy menu is long on a 1140-epx-tall screen. Win11 XAML menu is also touch-spaced when opened by touch.
- "Disable rounded corners": cosmetic; DWM work saved is negligible. No real perf gain.
- Weather (ep_weather, WebView2) - extra RAM/GPU on 6 GB/Mesa; leave off.
- Risk side: EP proxies dxgi.dll into explorer + Start host (F3), patches by pattern-matching / downloads symbols (F2), ARM64 23H2 not in tested list (F1), recovery needs Ctrl+Alt+Shift (keyboard) (F2). On a phone with a homebrew GPU stack and no keyboard this is a real lockout risk.
- VERDICT: EP is a net negative for touch overall; the only touch-relevant feature is full-screen Win10 Start. Not recommended as a first step; Windows' own tablet posture (F5) gives the touch-optimized taskbar without injection.

### F8 - Built-in Win11 23H2 touch/smoothness settings (no EP)
- "Tablet mode" was removed in Win11; replaced by automatic **tablet posture** (F5) - driven by ConvertibleSlateMode + convertibility (Enclosure/DeviceForm/ConvertibilityEnabled).
- Touch-optimized taskbar: HKCU\...\Explorer\Advanced ExpandableTaskbar=1 (default on) - only acts in tablet posture (F4/F5). Collapsed = thin bar; swipe up from bottom edge expands.
- Edge swipes (support.microsoft.com touch gestures, https://support.microsoft.com/en-us/windows/touch-gestures-for-windows-a9d28305-4818-a5df-4e2b-e5590f850741): right edge = notification center, left edge = widgets; 3-finger up = Task View, 3-finger down = desktop, 3-finger left/right = last app; 4-finger = desktops. Policy to disable edge swipes: HKLM\SOFTWARE\Policies\Microsoft\Windows\EdgeUI AllowEdgeSwipe=0 (leave absent). Widgets (left swipe) is WebView2-heavy - consider removing/disabling Widgets on 6 GB.
- Text size: HKCU\Software\Microsoft\Accessibility TextScaleFactor DWORD 100..225 (Settings > Accessibility > Text size; applies live via Settings, registry edit needs sign-out). Only scales text, not hit targets; many Win32 apps ignore it. https://learn.microsoft.com/en-us/uwp/api/windows.ui.viewmanagement.uisettings.textscalefactor
- Display scaling is what actually grows touch targets. Installer writes LogPixels=275 (~286 %) (docs/installer/architecture.md); GPU notes mention 200 % (540x1140 epx) - live value UNVERIFIED. At 438 ppi: 200 % -> a 40-epx target = 4.6 mm; 286 % -> 6.6 mm; 300 % -> 6.9 mm (MS touch guideline ~7.5 mm / 40x40 epx at normal density). Trade-off: 300 % leaves 360x760 epx, many Win32 dialogs overflow. 250-286 % is the sensible band. Needs sign-out for full effect.
- Explorer compact view: HKCU\...\Explorer\Advanced UseCompactMode DWORD 0 = touch spacing (default 0; 22H2+ may default compact on non-touch devices). Settings live after Explorer reopen.
- Touch indicator (circles at touch points): HKCU\Control Panel\Cursors ContactVisualization / GestureVisualization (1/0x1f on, 0/0 off) - Settings > Accessibility > Mouse pointer and touch > Touch indicator. Off saves a tiny bit of DWM work; ON is useful while debugging the phantom-tap driver issue.
- Transparency: HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize EnableTransparency DWORD 0 (live; removes acrylic/mica blur = less GPU composition work on Mesa).
- Animations: Settings > Accessibility > Visual effects > Animation effects Off = SPI_SETCLIENTAREAANIMATION FALSE (live). Plus SystemPropertiesPerformance.exe "Adjust for best performance" (writes UserPreferencesMask + HKCU\...\Explorer\VisualEffects VisualFXSetting=2; apply through the dialog, raw reg needs sign-out). Keep "Smooth edges of screen fonts" on for legibility.
- Touch keyboard: HKCU\Software\Microsoft\TabletTip\1.7 TouchKeyboardTapInvoke DWORD 2 = always show on text-field tap (currently absent, notes/touch-keyboard.md). Tablet posture also enables auto-invoke.
- Double-tap / hold: F6. Recommendation given phantom taps: do NOT widen; optionally tighten DtapDist a little (e.g. 30) and leave DtapTime default until the driver fix lands.

## Ranked recommendation (top 5)
1. Put Windows into tablet posture: HKLM\SYSTEM\CurrentControlSet\Control\PriorityControl ConvertibilityEnabled=1 (keep ConvertibleSlateMode=0), reboot. Gives touch-optimized taskbar, spacing, keyboard auto-invoke. Risk low (registry, documented). Revert: delete value + reboot. Durable variant: SMBIOS Type 3 enclosure 1Eh in our UEFI.
2. Display scaling 250-286 % + TextScaleFactor 115-125 %. Risk low; revert in Settings.
3. Transparency off + Animation effects off + "best performance" visual effects. Risk none; reduces composition load on the Mesa/WDDM stack. Revert: toggles.
4. Touch keyboard TouchKeyboardTapInvoke=2; touch indicator on while debugging taps; leave double-tap tolerance default/tighter (not wider). Risk none.
5. ExplorerPatcher: only if full-screen Start is wanted - install ep_setup_arm64.exe 70.3 (stable), immediately set taskbar back to Windows 11, Start = Windows 10 + Display mode Full screen Start, Updates = do not check, Advanced = symbols download off. Have a USB-OTG keyboard attached (Ctrl+Alt+Shift recovery) and know the offline fallback (delete C:\Windows\dxgi.dll + the copy in StartMenuExperienceHost folder from TWRP/mass storage). Risk medium-high. Revert: `ep_setup_arm64.exe /uninstall`.

- 2026-10-14: research complete; final report sent to caller.
