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
