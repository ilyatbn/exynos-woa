# Touch keyboard on Windows (S10e) - investigation

User report 2026-10-14: "i enabled the touch keyboard but i dont see it working when in input
menus. honestly now the on screen touch keyboard is frozen."

Goal: why the touch keyboard (TextInputHost) does not auto-invoke on text-field tap; which keyboard
is "frozen" and why; make Windows behave like a tablet.

Rules: reversible settings only, record old value + revert command before every change. No reboot,
no explorer/dwm kill, no drivers.

## Progress log
- 13:28 (phone time) processes: TextInputHost 6700, TabTip 7276, ctfmon 7264 all alive since boot
  (13:17:55-59); osk.exe NOT running -> the "frozen on-screen keyboard" is the modern touch keyboard
  (TextInputHost), not osk.exe. Responding=True from session 0 (unreliable for session-1 windows).
- Keyboard class: 3x "HID Keyboard Device" (HID\VID_0C.., VID_04..) all Status Unknown = phantom
  (old USB dock devices, not present). Win32_Keyboard returns nothing -> no keyboard present. Mice:
  2 phantom. Present HID: "HID-compliant touch screen" + "Samsung SEC-S6SY761 Touchscreen" only.
- User SID S-1-5-21-1989328923-3768966206-2800666768-1000 (hive loaded under HKU).
- HKCU\Software\Microsoft\TabletTip\1.7: EdgeTargetDockedState=1, TipbandDesiredVisibility=1,
  OptimizedKeyboardRelative{X,Y}PositionOnScreen=0x5ee6/0x168d4. NO EnableDesktopModeAutoInvoke,
  NO TouchKeyboardTapInvoke.
- HKCU ...\ImmersiveShell TabletMode=0. HKLM PriorityControl ConvertibleSlateMode=0.
- HKLM\SOFTWARE\Microsoft\TabletTip\1.7: EnableDoubleTapSpace/PredictionSpaceInsertion/TextPrediction=1.
  No HKLM\SOFTWARE\Policies\Microsoft\TabletTip.
- Web (elevenforum/geekrewind, Win11 >= 22621.1926): Settings "Show the touch keyboard" dropdown =
  HKCU\Software\Microsoft\TabletTip\1.7 TouchKeyboardTapInvoke DWORD: 0 Never, 1 When no keyboard
  attached, 2 Always. Value ABSENT on the phone.
- learn.microsoft.com (gpiobuttons ConvertibleSlateMode, "Considerations for convertibles"):
  PriorityControl\ConvertibleSlateMode 0 = slate (no keyboard), 1 = laptop. Phone has 0 = slate. OK.
- Application log (Id 1000/1001/1002): earlier boot 08:23:31-08:26:28 crash loop: StartMenuExperienceHost
  then TextInputHost.exe 623.24300.0.0 every ~6 s, ucrtbase.dll 0xc0000409 offset 0x7550c (fail-fast /
  abort). Same signature as StartMenu -> generic XAML/composition abort, during the first-logon
  TrustedInstaller CreateSession (74-190 s). 08:25:47/51 StoreAgentDownloadFailure
  "TabTip-StartAppInstallWithTelemetryAsync" 80004002. 08:43 RADAR_PRE_LEAK TextInputHost + explorer.
  CURRENT boot (since 13:17): no TextInputHost/TabTip crash or hang (1002) events; only WindowsTerminal
  d3d11.dll 0x46f4 crashes (known, notes/gpu-hw-path.md).
- 13:29 screenshot (notes/img-touchkbd-frozen-1329.png, framebuffer 0xca000000): the MODERN TOUCH
  KEYBOARD (floating/undocked layout, Esc/Tab/Ctrl/Alt/&123, mic key) is shown at the bottom of the
  desktop above the taskbar; clock 1:29 PM current. Taskbar has the touch-keyboard tray icon. Desktop
  shortcut "On-Screen Keyboard.cmd" (S10eDiag, notes/m11-s10ediag.md:90) exists; osk.exe NOT running.
- 13:32:58 window enum in session 1 (C:\S10eTmp\kbwin.ps1, scratchpad copy): TextInputHost pid 6700
  class Windows.UI.Core.CoreWindow "Windows Input Experience" rect 0,0,540,1140 visible,
  IsHungAppWindow=False, SendMessageTimeout(WM_NULL,ABORTIFHUNG,3 s)=OK -> UI thread NOT hung.
  Other windows: WindowsTerminal 7260 (116,123,671,741) SMTO FAILED; Settings (ApplicationFrameHost
  9048 + SystemSettings 8908 CoreWindow, SMTO FAILED - likely suspended UWP). Neither Terminal nor
  Settings visible on the 13:29 screenshot -> check cloaking / whether the screen is stale.
- 13:34 screenshot (notes/img-touchkbd-1334.png): screen LIVE (clock 1:34, user opened Start, search
  box focused, Pinned grid empty), touch keyboard still drawn at the same place, identical frame.
  A runuser2 rerun returned the stale 13:32:58 output (done marker after 2 s, task already gone -
  probably collided with another session's S10eUserRun2 task) -> rerun later.
- TextInputHost 6700 loaded modules include maliumd.dll (our Mesa UMD), d3d11, dxgi, dcomp,
  Windows.UI.Xaml, wuceffects -> keyboard XAML renders on the HW GPU path.
- System log since boot 13:17: no dxgkrnl/Display TDR or device-removed events.
- 13:35 UIA (kbuia.ps1 in user session): RootElement children with TextInputHost pid = 0 found in
  4.4 s; then AutomationElement.FocusedElement HUNG > 85 s (task killed by runuser2 at 90 s) -> the
  focused element's provider (Start search / whatever had focus) did not answer UIA.
- Coordinator 17:34(their clock): synthetic taps (C:\S10eTmp\s10etap.exe) on the keyboard keys give no
  highlight and no text in a focused Start search box -> keyboard does not react to input.
- User: keyboard worked on 2026-10-13 -> regression suspected (UMD 0064-0066, KMD 1.0.38-1.0.43,
  KB5068865 at ~15:45 on 10-14 replaced d3d11/dxgkrnl/DWM).
- 13:37 thread states TextInputHost 6700: 35 UserRequest, 4 EventPairLow, 1 ExecutionDelay (tid 2920,
  most CPU, but 0 CPU over 5 s) - nothing spinning. 13:38 comsvcs MiniDump full -> 188 MB file with
  StreamDirectoryRva=0 (incomplete, cdb refuses) - useless; switched to C:\S10eTmp\livedump.ps1.
- !! 13:38:27-13:38:34 the user session was LOGGED OFF and a new one started (session 2, explorer
  "delaying system shutdown", Winlogon 7002 then 7001). Not caused deliberately by this investigation
  (only a comsvcs MiniDump of TextInputHost ran at ~13:38; MiniDump does not log off) - origin unknown
  (user or another agent).
- Earlier this boot: shutdown.exe "restart" initiated on behalf of S10e at 13:27:36, 13:31:07,
  13:31:45 - each logged Winlogon 7002 logoff, sshd 7034, EventLog 6006 stopped, Kernel-Power 109
  "Power Action Reboot", Kernel-General 13 "shutting down" - BUT no new boot (Kernel-General 12 only
  at 13:16:55; dwm/explorer/TextInputHost start times 13:17). So restarts are being requested and
  NOT completing (the session/services restart?) - suspicious state for any service the keyboard needs.
- CORRECTION: the 13:19/13:20/13:27/13:31 "restarts" are PREVIOUS boots - the phone clock comes up at
  ~13:16 every boot (no RTC), so boots overlap in time. Session-0 csrss started 13:17:26 -> no reboot
  since; but session 1 was logged off at 13:38:27 and session 2 created 13:38:29 (dwm 3448, explorer
  5436, TextInputHost 6460, TabTip 8268, ctfmon 4888 all 13:38:3x-54). Phone clock lags the PC by ~4 h.
- Services: TextInputManagementService Running (auto); PenService_3a3286 stopped (manual);
  MsKeyboardFilter disabled. No TabletInputService on Win11.
- 13:41 screenshot session 2 (notes/img-touchkbd-1341.png): touch keyboard drawn again at the same
  place right after the new logon (floating layout), desktop only, nothing focused.
- CHANGE 13:41: HKU\S-1-5-21-...-1000\Software\Microsoft\TabletTip\1.7 TouchKeyboardTapInvoke
  was ABSENT -> REG_DWORD 2 (Always show on text-field tap). Read back 0x2.
  Revert: reg delete "HKCU\Software\Microsoft\TabletTip\1.7" /v TouchKeyboardTapInvoke /f
- Main session set PriorityControl ConvertibilityEnabled=1 (not this task), reboot pending.

## Main session, 2026-10-14 ~18:00-18:20 (agent stopped while stalled)
- User: tablet posture (ConvertibilityEnabled 1) + TouchKeyboardTapInvoke 2 -> keyboard now POPS UP on text-field
  tap, but its keys still ignore taps (user + s10etap). Touch-optimised taskbar shows (collapses to a bar).
- UMD bisect: maliumd/libgallium 0063 (05:57 build) swapped in for new processes, TextInputHost restarted: keys blank,
  still frozen -> not a 0064-0066 regression. Restored 0066 (sha 36ec8d74 verified). The "worked yesterday" sessions
  most likely ran without the GPU (the GPU_POWER_ON 0 image, Basic Render/WARP).
- cdb ARM64 (Windows Kits Debuggers\arm64 -> C:\S10eTmp\dbg) non-invasive (-pv) stack dumps of TextInputHost, no
  symbols on the phone (srv* hung on the phone's network - killed; use -y C:\S10eTmp\nosym). Fresh process: all idle.
  Frozen (user popped it up and tapped): thread "InputPanel" (the keyboard UI thread) in KERNELBASE!SleepEx under
  edgehtml, identical in two dumps 5 s apart. edgehtml.dll pulled to the PC + public PDB (symchk) + cdb -z ln:
  GlobalWndProc -> GlobalWndOnPaintPriorityMethodCall -> HTML5EventLoopDriver -> ... CPaintBeat::OnPaintTimer ->
  CPaintBeat::ProcessRenderingLoop -> CPaintController::RunRenderingLoop -> CDoc::PaintInPlace -> CDoc::PaintWorker ->
  **CPaintController::CanPaintNow** -> SleepEx (loop). EdgeHTML's paint gate never opens on our GPU path; the UI thread
  never returns to process input. No thread is inside d3d11/dxgi/maliumd at the moment of the dump.
  Dumps: s10e-artifacts\runs\tih-stacks-1.txt (fresh), tih-stacks-frozen.txt, tih-stacks-frozen2.txt.
- Next: workaround = EdgeHTML software rendering (IE "UseSWRender"); root cause = what CanPaintNow waits for (D3D
  device state / occlusion / DComp) - disassemble CanPaintNow with the PDB.
