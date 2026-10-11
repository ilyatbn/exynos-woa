# D8 hardware buttons - agent polling design (main, 2026-10-11)

User spec: power short = screen on/off; power long = popup Reboot / Recovery / Shutdown; volume up/down = nothing
for now; Bixby = Task Manager; everything configurable (an app later; a config file now).

## Why not the drivers-branch design (Exynos9820Gpio GpioClx + DSDT ACPI0011 / hidinterrupt)
- hidinterrupt hands POWER to the Windows power manager (fixed actions, no long-press popup) - not controllable.
- Exynos9820Gpio was never built (vcxproj lacks msgpioclxstub.lib) and its _CRS claims GSIV 45 = the touch
  controller's EINT (Exclusive in TSP0) -> arbiter conflict risk on the one input that works today
  (notes/drivers-branch-port-plan.md). Kept on the branch for a later interrupt-driven variant.

## Design
- S10eAgent (LocalSystem) thread KeysThread: IOCTL_S10EDIAG_READ_PHYS (MMIO, 4 B) every 25 ms of
  ALIVE gpa0 DAT 0x15850004 (VOLUP b3, VOLDN b4, BIXBY b6) and gpa2 DAT 0x15850044 (POWER b4); active-low
  (Exynos9820.h D8 constants, VERIFIED from the shipped DT; DAT = bank +4 per the alive bank type).
- 2-sample debounce (50 ms); long press fires at long_press_ms (default 700) while held, short on release.
- C:\ProgramData\S10e\buttons.ini (written with defaults if absent, re-read on every press):
  actions none | screen | powermenu | wake | taskmgr | reboot | recovery | shutdown | run:<cmd>.
- Desktop actions run in the console session via WTSQueryUserToken + CreateProcessAsUser:
  s10ekeys.exe screen (GUID_CONSOLE_DISPLAY_STATE -> SC_MONITORPOWER 2 / wake), powermenu (topmost popup,
  recovery drops C:\S10eTmp\req\recovery.req for the agent), wake; Taskmgr.exe; run: commands.
- UNVERIFIED on device: DAT polarity/bits (first log line prints both DAT words at idle - expect bits set),
  whether SC_MONITORPOWER blanks this BasicDisplay panel.
- Deployed (TWRP): boot #87 (4e6dec2e…) + S10eAgent.exe (a16fc8d9…) + s10ekeys.exe (d1a1e80a…) into System32 = build hashes; stale req files cleared. Button test pending.
- Test 1 (user, boot #87): power short #1 turned the screen OFF; short #2 did NOT turn it back on; power long =
  power menu works; Bixby does nothing. (Hub: unchanged - only when attached before boot -> parked.)
- Test 1 logs: agent idle DAT gpa0 0x5E / gpa2 0x14 -> all four key bits high when released (polarity + pins
  confirmed). s10ekeys.log: "screen: display state 1 -> off" then no second screen line -> the 2nd press never
  reached the helper (agent log of that boot overwritten by the user's reboots, so unknown whether detected).
- Fix (deployed same session): agent keeps agent.log.prev (local + ESP); logs every key down/up with hold time;
  Taskmgr launched with the user's linked (elevated) token (highestAvailable refuses the filtered one - INFERRED
  cause of "Bixby does nothing"); helper logs on start; wake = SetThreadExecutionState(ES_DISPLAY_REQUIRED) + input +
  SC_MONITORPOWER -1.
