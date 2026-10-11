# Battery driver M1 — KMDF battery miniclass (battc) on the MAX77705 gauge

Started 2026-10-12. Goal: the Windows battery indicator. A KMDF battery
miniclass driver (battc.sys, shape of Microsoft's `power/battery` simbatt
sample, MIT) that owns HSI2C45 itself and reports the MAX77705 fuel gauge,
plus the ACPI device it binds to. **No device access in this task** (another
session owns the phone): everything here is build-verified only, nothing is
device-validated (AGENTS.md §7).

## Progress log

### 2026-10-12 — setup
- Worktree branch `worktree-agent-ab7fa23c25d1f0805` was 268 commits behind
  `main`; fast-forwarded to `main` 95b6d40 (no local changes) before starting.
- Read: `notes/battery-driver.md` (register map, conversions), `FuelGauge.c`,
  `Pmic.c` (polled HSI2C45 master), `UsbHw.h` §5/§6, `Exynos9820.h` HSI2C45 /
  CMGP block, `Dsdt.asl`, `PlatformMemoryMapLib.c:179-181` (HSI2C45,
  CMGP_USI page, CMGP_GPIO page are all already in the UEFI memory map).
- Verified constants (Exynos9820.h:477-502): `EXYNOS_PMIC_HSI2C_BASE 0x15D60000`
  size `0x1000`; `EXYNOS_CMGP_SYSREG_USI_PAGE 0x15C22000` size `0x1000`, USI03
  mode at `0x15C22030`, I2C mode value `4`; `EXYNOS_CMGP_GPIO_BASE 0x15C30000`
  size `0x1000`, gpm12 CON `+0x180`, gpm13 CON `+0x1A0`, function `2`.
- History check: Pmic.c's comment "run 5: with the bus left configured, Windows
  bugchecked 0x14C" — `notes/m16-usb.md` runs 6-8 later showed **0x14C was not
  caused by the bus state** (run 6 restored the bus and still 0x14C; run 7 the
  run-4 image too). So a Windows driver keeping HSI2C45 configured is not a
  known hazard; we still restore the handoff state on D0Exit.

### 2026-10-12 — charger status registers (vendor source, fetched today)
Source: LineageOS `android_kernel_samsung_exynos9820` branch `lineage-18.1`,
`include/linux/mfd/max77705-private.h:73-80`, `drivers/battery_v2/include/charger/max77705_charger.h:74-115`,
`drivers/battery_v2/max77705_charger.c:133-197` (`max77705_get_vbus_state`, `max77705_get_charger_state`).
All `[K]` (verified in vendor source), client `0x69`:

| Reg | Addr | Field | Meaning |
|---|---|---|---|
| CHG_INT_OK | `0xB2` | bit6 CHGIN_OK, bit5 WCIN_OK, bit3 BAT_OK | status (not read-to-clear; CHG_INT `0xB0` is) |
| CHG_DETAILS_00 | `0xB3` | bits 6:5 CHGIN_DTLS | 0 `<UVLO`, 1 `<MBAT+CHGIN2SYS`, 2 `>OVLO`, **3 = VBUS valid** |
| CHG_DETAILS_01 | `0xB4` | bits 3:0 CHG_DTLS | 0,1,2 CHARGING; 3,4 FULL; 5,6,7 NOT_CHARGING; 8,0xA,0xB DISCHARGING (vendor's own mapping) |

Read as one 2-byte transfer at `0xB3` (auto-increment gives DETAILS_00, DETAILS_01) —
obeys the "never 1-byte reads on this bus" rule (Pmic.c:Max77705RegRead, M16/M17 run 9).

Gauge registers added (same header, enum `max77705_fuelgauge_reg`): `DESIGNCAP_REG 0x18` [K],
`CYCLES_REG 0x17` [K] (vendor **writes** it — 0 / 0xA0 — in several paths, so it is not a usable
cycle count; not reported). Vendor `max77705_fg_read_temp()` (fuelgauge.c:326-345) returns
**milli-degC** (`hi*1000 + lo*39/10`); our UEFI `FuelGauge.c` divides by 10 and prints it as
tenths -> "370.0C" is really 37.0 C (the UEFILOG TEMP bug = a /10 vs /100 unit slip, not a
register error). The Windows driver converts milli-degC -> tenths of Kelvin.

### 2026-10-12 — design decision (driver owns HSI2C45; battc miniclass)
- **Bus ownership:** the driver maps HSI2C45 + the CMGP USI page + the CMGP GPIO page from
  its own ACPI `_CRS` and ports `Pmic.c` verbatim (Hsi2cBusSetup -> Hsi2cReset -> polled
  Hsi2cXfer; release restores mode/pins/USI_CON). Rejected alternative: a SpbCx client on an
  `Exynos9820Hsi2c` instance for bus 45 — the Windows HSI2C driver does CMU/QCH bring-up for
  PERIC0 USI03 that does not apply to the ACPM-owned CMGP block, it would need a variant, and
  two drivers would then share the CMGP pins. One owner, polled, PASSIVE_LEVEL, no interrupt.
- **Resource check:** `PrepareHardware` refuses to start (STATUS_DEVICE_CONFIGURATION_ERROR)
  unless the three memory resources are exactly the Exynos9820.h VERIFIED bases/sizes, in order.
- **Polling:** WDFTIMER (periodic, 20 s, DISPATCH) -> WDFWORKITEM (PASSIVE) does the I2C reads,
  under a bus wait-lock; results go into a cached snapshot under a state wait-lock. Battery
  class queries only copy the cache (never touch the bus). After each poll, if the reported
  state changed, `BatteryClassStatusNotify`. `SetStatusNotify` returns STATUS_NOT_SUPPORTED
  (simbatt does the same; battc then polls QueryStatus).
- **Power:** D0Entry = bus setup + one synchronous poll + timer start; D0Exit = timer stop,
  work item flush, bus release (bootloader/UEFI state put back, Pmic.c:PmicBusRelease order).
- **Battery tag:** valid only after a gauge read succeeded with STATUS.absent clear; otherwise
  QueryTag fails STATUS_NO_SUCH_DEVICE and Windows shows no battery (fail-safe).
- **Units:** Windows wants mWh/mW. Capacities from the gauge are mAh (word x fg_resistor/2,
  fg_resistor = 2 `[K-default, UNVERIFIED]`). mWh = mAh x `S10E_BATT_NOMINAL_MV_UNVERIFIED`
  (3850) / 1000 — **UNVERIFIED** constant (closes by reading the battery label / vendor DT
  `battery,*` nominal voltage); it scales Wh numbers only, never the percentage.
  RemainingCapacity = FullChargedCapacity x SOCREP / 1000, so Windows' % == gauge SOCREP
  exactly (not REMCAP/FULLCAP rounding). Rate mW = AVG_CURRENT mA x VCELL mV / 1000.
- **AC adapter:** no ACPI0003 device. The composite battery derives AC from
  `BATTERY_POWER_ON_LINE` in each battery's PowerState (simbatt reports AC this way).
  ON_LINE = CHGIN_DTLS == 3.
- **FADT:** `PreferredPmProfile` is already `EFI_ACPI_6_3_PM_PROFILE_TABLET`
  (Fadt.aslc:52); nothing to change for a battery to be shown.

### 2026-10-12 — task 1: DSDT `\_SB.BAT0` (`SAMS9826`) — firmware build
- `_HID` check: `rg SAMS982` -> SAMS9820 I2C0, SAMS9821 UFN0 (main) / GPIO (drivers branch, to be
  renumbered SAMS9822), SAMS9823/9824 reserved for D8 audio, SAMS9825 WLAN
  (`notes/drivers-branch-port-plan.md`). SAMS9826 unused anywhere. Neither main's nor
  origin/drivers' DSDT claims 0x15D60000 / 0x15C22000 / 0x15C30000 (no resource conflict).
- `_CRS`: three `Memory32Fixed` = Exynos9820.h VERIFIED bases/sizes; no interrupt, `_STA 0x0F`.
- **Build environment findings** (this worktree had no `firmware/edk2`):
  - Made a private edk2 workspace: `firmware/edk2/` (gitignored) with junctions to main's edk2
    subdirs, a copied `Conf/` (deleted `Conf/.cache`) and its own `Build/` — so this build never
    writes into main's `firmware/edk2/Build` (another session's flash images live there).
  - `make` (GnuWin32 3.81) runs `Trim` via CreateProcess: no `Trim.exe`, only `Trim.bat`
    -> "process_begin: CreateProcess(NULL, Trim ...) failed". Fix: `firmware/edk2/Wrap/{Trim,AmlToC}`
    bash shebang scripts (`exec python <edk2>/BaseTools/Source/Python/Trim/Trim.py "$@"`) first on
    PATH plus `C:\Program Files\Git\usr\bin` (make execs `#!` scripts through it). The stock
    PosixLike wrapper fails here (`ModuleNotFoundError: Common` — it builds a ':'-joined PYTHONPATH).
  - MAX_PATH: from the worktree path, `NonDiscoverableDeviceRegistrationLib.lib` (~265 chars) is
    invisible to make 3.81 ("No rule to make target"). Fix: build through a short junction
    `C:\Users\ilya\code\b9w -> <worktree>\firmware` (invoke `b9w\tools\build-uefi.ps1`).
- **Result:** `build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG` -> exit 0, `- Done -`.
  `Dsdt.aml` 1639 -> 1725 B (contains `SAMS9826`). FVMAIN_COMPACT free: baseline (HEAD DSDT,
  same workspace) 1224 B, with BAT0 1152 / 1168 B (two runs; the known ±24 B drift, m8-build.md).
  **FVMAIN_COMPACT is 99 % full (~1.1 KiB left)** — a risk for any further firmware additions.

### 2026-10-12 — commit ba1d486 review (task 1, harsh critic)
- DSDT `_CRS` vs `Exynos9820.h:477-499`: rg of both — all three bases/sizes match. ✔
- `EXYNOS_CMGP_SYSREG_USI_PAGE_SIZE` is **not** tagged VERIFIED in the header (it is the page
  granule, the DT extent is 4 B at +0x30). The DSDT comment does not claim VERIFIED; the driver
  header draft did -> corrected before the driver commit (comment now says "one page").
- 9810 tree untouched: `git diff HEAD~1 --stat -- firmware/Platform/Star2LtePkg firmware/Silicon/Exynos9810Pkg` empty. ✔
- The DSDT comment "Windows takes the AC state from BATTERY_POWER_ON_LINE: no ACPI0003" is an
  **inference** (the simbatt sample reports AC exactly this way; Windows has no AC device here).
  Device check: `powercfg /batteryreport` / tray icon shows "plugged in" with a charger attached.
- Nothing found that needs a follow-up commit.

### 2026-10-12 — task 2: driver `drivers/S10eBattery` — build
Files: `S10eBattery.h` (constants, context), `Hsi2c.c` (Pmic.c port), `Max77705.c` (read-only
reading, vendor conversions verbatim + plausibility gate), `Miniclass.c` (battc callbacks, poll,
diagnostics), `Driver.c` (simbatt WDF plumbing + resources/power/poll), `S10eBattery.inf`,
`S10eBattery.vcxproj`, `build-package.ps1` (copy of the Hsi2c flow).

Pre-commit self-review changes:
- **Plausibility gate** added (`S10E_PLAUSIBLE_VCELL_{MIN,MAX}_MV` 2500/5000, plus "FULLCAP and
  DESIGNCAP both 0"): the known failure mode of this bus is reads returning 0. A zero SOC reaching
  Windows would fire the **critical battery action (hibernate/shutdown)**. A rejected reading keeps
  the last good one (or no battery at all before the first good one). These are physical
  plausibility limits, deliberately loose, not device constants.
- Diagnostics: the registry snapshot records the *attempted* reading on failure (raw values of a
  rejected read are what a device test needs).

Diagnostics written by the driver (`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9826\0\Device Parameters`,
on the first good poll, on every reported change, on the first 3 failures and every 32nd):
`PollOk PollFail LastStatus FgStatus SocTenths VcellMv AvgCurrentMa TempMilliC RemCapMah FullCapMah
DesignCapMah ChgDetails (DTLS_00 | DTLS_01<<8, 0xFFFFFFFF = charger not read) PowerState
HandoffUsiMode HandoffGpm12 HandoffGpm13 HandoffUsiCon`.

Build: `build-package.ps1 -Out C:\Users\ilya\code\s10e-artifacts\battery-m1\S10eBattery`
-> msbuild exit 0 with **/W4 /WX** (0 warnings), ARM64 (`dumpbin /headers`: AA64), imports
BATTC.SYS (BatteryClassInitializeDevice/Ioctl/StatusNotify/SystemControl/QueryWmiDataBlock/Unload),
WMILIB.SYS, WDFLDR.SYS (KMDF 1.15), HAL, ntoskrnl `MmMapIoSpaceEx`. Inf2Cat (10_RS3/NI/GE_ARM64)
clean; .sys and .cat signed with the WDK test cert (signer "WDKTestCert ilya,134359747320405644";
host `signtool verify /pa` fails only on root trust, as for the existing packages).
`infverif /w /v S10eBattery.inf` -> **INF is VALID**.

### 2026-10-12 — commit b53bc52 review (task 2, harsh critic)
Checked mechanically:
- MMIO write sequence of `Hsi2c.c` vs `Pmic.c` (grep of every MmioWrite / I2cWr / pin write, in
  order): reset, setup, xfer and release sequences identical (file order differs only). ✔
- Register constants: scratch helper `cmpdefs.py` over `UsbHw.h` vs `S10eBattery.h`: 43/43 shared
  names equal; `Pmic.c` error bits 6/6 equal. Charger `0xB3`, `0x60>>5`, `0x0F`, `DESIGNCAP 0x18`
  equal to the vendor headers fetched today. ✔
- Read-only: the only write transfer is the 1-byte register pointer inside `S10eMax77705ReadN`
  (rg `S10eHsi2cXfer(` -> 2 call sites, both there). ✔
- Every guessed value is named `*_UNVERIFIED` (fg_resistor, nominal mV). ✔
Found -> follow-up tasks (own commits):
- **F1** `PrepareHardware` required the `_CRS` windows *in order*; a reordered translated list would
  have failed start (Code 10). Fix: match by base, each exactly once, all three required.
- **F2** `Hsi2c.c` header says "same register sequence" but the `Hsi2cDump` diagnostic reads of
  Pmic.c are not ported (reads only, no behaviour change) — the comment must say so.
Accepted (no change): "LION" chemistry string is cosmetic (Windows convention for Li-ion);
`FullChargedCapacity` is frozen per tag (until reboot / driver restart) so Windows % stays == SOCREP.

### 2026-10-12 — F1 fixed
`Driver.c:S10eBattPrepareHardware` matches memory descriptors by base against the 3 expected windows
(size must match, duplicates and missing windows refuse start). Rebuilt: exit 0, /W4 /WX clean.

### 2026-10-12 — F2 fixed
`Hsi2c.c` header now says the Hsi2cDump diagnostic reads are not ported. Rebuilt: `S10eBattery.sys`
hash prefix 322CA9517A3B9DA3 identical before/after (/Brepro) — proof the change is comment-only.
Review of 0e1dd7a (F1): failure path unmaps `Map[i]` with `Expected[i].Size` — index-consistent with
the by-base placement; a duplicate base hits `Map[k] != NULL` -> refuse. ✔

### 2026-10-12 — task 3: `drivers/S10eDiag/esp/extra-battery.cmd`
Modeled on `extra-touch.cmd` (fresh `C:\S10eTmp`, copy to NTFS, hashes, test cert, pnputil). Adds:
**powercfg BATACTIONCRIT/BATACTIONLOW = 0 (do nothing) on AC and DC before the install** — a bogus
low reading must not hibernate/shut down the phone mid-test (`powercfg-battery-before.txt` keeps the
previous values to restore); `s10ediag peek` of USI_CMGP3 mode / gpm12 / gpm13 / HSI2C45 CTL before
and after (single words, the extra-touch rule); 70 s settle (3 polls); `pnputil /enum-devices`
(`ACPI\SAMS9826`, class Battery), `sc query`, the driver's `Device Parameters` snapshot (twice,
60 s apart), `Win32_Battery`, `root\wmi` BatteryStatus/StaticData/Temperature,
`powercfg /batteryreport` (html + xml), System + Kernel-PnP evtx, setupapi.dev.log.

## Device test procedure (for the session that owns the phone)

Prerequisites: firmware built from this branch (DSDT `BAT0`); the package
`C:\Users\ilya\code\s10e-artifacts\battery-m1\S10eBattery\` (sys/inf/cat) — or rebuild with
`drivers\S10eBattery\build-package.ps1 -Out <dir>`.
1. Flash the firmware as usual (HANDOFF flow; only `boot`). Boot Windows once; nothing binds
   `ACPI\SAMS9826` yet -> an "Unknown device" in pnputil is expected and harmless.
2. Stage on the ESP: `\S10EDIAG\drivers\S10eBattery\{S10eBattery.sys,S10eBattery.inf,s10ebattery.cat}`
   (+ `\S10EDIAG\drivers\wdktestcert.cer`, already there for touch), and
   `drivers\S10eDiag\esp\extra-battery.cmd` -> `\S10EDIAG\extra.cmd`.
3. Boot Windows; S10eAgent runs collect.cmd -> extra.cmd. Results in `%OUT%\last\battery\`.
4. Pass criteria, in order:
   - `pnputil-battery.txt`: added + installed; `dev-battery.txt`: Started, driver S10eBattery, no problem code.
   - `battery-params.txt`: `PollOk` > 0, `PollFail` 0, `SocTenths` close to a fresh UEFILOG `[FG] SOC`
     of the same charge state (same register, same conversion). Android's own % is Samsung-scaled
     (capacity_min/max, notes/battery-driver.md) so a few % delta vs Android is expected — **no
     gauge-vs-Android comparison has been recorded yet**. `VcellMv` ~3500-4400. `FullCapMah`: recorded
     UEFI reads are 2460 (notes/m16-usb.md run, SOC 99.2 %) and 3000 (UEFILOG cited in this task's
     brief, not committed) — the gauge's learned value moves, so accept ~2000-3200.
     `ChgDetails` != 0xFFFFFFFF.
   - `ChgDetails` low byte bits 6:5 == 3 iff a charger is plugged; `PowerState` 1 (ON_LINE) /
     5 (ON_LINE|CHARGING) plugged, 2 (DISCHARGING) on battery.
   - `wmi-batterystatus.txt` RemainingCapacity / `win32-battery.txt` EstimatedChargeRemaining == SOC.
   - Tray shows the battery icon with that percentage; plug/unplug flips the icon within ~20 s.
   - `usi-cmgp3-mode-after` = 4, gpm12/gpm13 low nibble = 2 (driver holds the bus in D0).
5. Failure triage: Code 10 + `PollFail` > 0 -> `LastStatus` (0xC00000B5 timeout, 0xC000000E no ACK
   (STATUS_NO_SUCH_DEVICE), 0xC000009C data error incl. the plausibility gate, 0x80000011 master
   busy, 0xC0000013 gauge says battery absent; values from ntstatus.h) and the raw values in
   `battery-params.txt`; no `Device Parameters` at all -> PrepareHardware refused (resources) or the
   driver never loaded (setupapi.dev.log). A bugcheck on install -> read the PRAM mark ring
   (`b: ...` marks) and AGENTS.md §9; then remove with `pnputil /delete-driver oemNN.inf /uninstall`.
6. After validation: restore the battery actions from `powercfg-battery-before.txt` if desired,
   and close the UNVERIFIED items below.

## Open / UNVERIFIED
- `MAX77705_FG_RESISTOR_UNVERIFIED = 2` (mA/mAh scale; not %): read the S10e DT battery node
  (`fuelgauge,fg_resistor`) from TWRP `/proc/device-tree` (names readable; value via root dump).
- `S10E_BATT_NOMINAL_MV_UNVERIFIED = 3850` (mWh scale only): battery label EB-BG970ABU or DT.
- Gauge TEMPERATURE validity (Samsung uses a board thermistor for its own UI) — compare Windows'
  BatteryTemperature with Android's battery temp.
- Whether Windows derives AC from BATTERY_POWER_ON_LINE without an ACPI0003 device (inference).
- Clocks: HSI2C45 gates are ACPM-owned; assumed still running under Windows (UEFI found them running
  after sboot; nothing in Windows gates them). If not, the first MMIO read could fault -> step 5.

### 2026-10-12 — commit 6d4b185 review (task 3, harsh critic)
- `ACPI\SAMS9826\0` instance path: `_UID Zero` -> "0" (I2C0 `_UID 10` shows as `\a` in
  extra-touch.cmd, i.e. hex) ✔. `s10ediag peek <phys>` syntax: S10eDiagCtl.c:122 ✔. NTSTATUS values
  in the triage list: ntstatus.h grep ✔ (STATUS_NO_MEDIA_IN_DEVICE is 0xC0000013, written correctly).
- `.cmd` line endings: the file was written LF; `.gitattributes` has `*.cmd eol=crlf` -> re-checked
  out, now CRLF (`file`: "with CRLF line terminators"). No content change.
- **F3** (fixed in the next commit): the pass criteria claimed "UEFI read 88.2 % vs Android" and
  "UEFILOG B42: 3000" — neither is in the repo. The only committed gauge read is notes/m16-usb.md
  (`SOC 99.2% ... FULLCAP_REP 2460`); the 88.2 % / 3000 read is from the task brief. No Android
  comparison exists. Criteria rewritten (FULLCAP range, compare against a fresh UEFILOG).
- **F4**: `S10eBattery.h` fg_resistor comment cites "UEFILOG FULLCAP_REP 3000" as support — must
  cite both recorded values (2460 committed, 3000 from the brief).

### 2026-10-12 — F4 fixed
`S10eBattery.h` fg_resistor comment cites both recorded FULLCAP_REP reads (2460 committed in
m16-usb.md, 3000 from the brief). Rebuilt: `S10eBattery.sys` hash prefix 322CA9517A3B9DA3 unchanged
(comment only). Review of 787c3d7 (F3): notes-only, criteria now match committed evidence. ✔

### 2026-10-12 — cleanup + rebuild recipe
- Removed the 34 junctions in this worktree's `firmware/edk2/` (and `C:\Users\ilya\code\b9w`) with
  `cmd /c rmdir` (never follows the link; main's `firmware/edk2/MdePkg` checked intact afterwards) so
  no later recursive delete of the worktree can reach main's edk2. Kept: `Build/`, `Conf/`, `Wrap/`.
- To rebuild the firmware from a worktree: (1) junction every top-level dir of main's
  `firmware/edk2` except `Build`/`Conf`/`.git` into `<wt>/firmware/edk2/`, copy `Conf/` and delete
  `Conf/.cache`; (2) `New-Item -ItemType Junction C:\Users\ilya\code\b9w -Target <wt>\firmware`;
  (3) in PowerShell: `$env:PATH = 'C:\Users\ilya\code\b9w\edk2\Wrap;C:\Program Files\Git\usr\bin;' + $env:PATH`
  then `powershell -File C:\Users\ilya\code\b9w\tools\build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG`.
  From main's own checkout none of this is needed (its Build/ already holds the .S objects).
- Driver package (final): `C:\Users\ilya\code\s10e-artifacts\battery-m1\S10eBattery\`
  `S10eBattery.sys` 322CA9517A3B9DA3…, `S10eBattery.inf` A51F68BDD4BC18CF…, `s10ebattery.cat`
  AC6392D21796D295… (SHA256 prefixes).

## Status
Build-verified (firmware DEBUG exit 0; driver /W4 /WX, infverif VALID, test-signed). **Not
device-validated** — MILESTONES row stays `doing`; the device test procedure above is the next step.
- 2026-10-12 device session: merged to main (9c92c88); boot #89 (dd4a193c…) flashed = #88 + DSDT BAT0; ESP
  drivers\S10eBattery staged + extra-battery.cmd as extra.cmd; s10ereboot.exe with confirmation deployed.
- Run batt1 (boot #89, user): battery icon shows but never changes. Diagnostics: 6 polls OK, SOC 53.0 -> 52.9 %,
  VCELL 3865 mV, AVG_I -182/-100 mA, ChgDetails 0x3BE0 = DETAILS_00 0xE0 (CHGIN_DTLS 3, VBUS valid) +
  DETAILS_01 0x3B (CHG_DTLS 0xB) -> PowerState 3 (online + discharging). Kernel max77705_chg_get_wdtmr_status:
  CHG_DTLS 0x0B = charger WATCHDOG EXPIRED -> charging off. Linux kicks CNFG_06 (0xBD) WDTCLR[1:0] = 01 on every
  health poll; under Windows nobody did -> the phone does NOT charge in Windows, icon stuck at "plugged, not charging".
- Fix: driver kicks the charger WDT every 20 s poll (RMW of WDTCLR only), logs ChgCnfg00/06 + WdtKicked; UEFI
  charger-mode loop kicks every 10 s. Package s10e-artifacts\battery-m2 (built + signed).
- Run batt2 (boot #91 + battery-m2 driver swapped into DriverStore, unattended cycle 262 s): ChgDetails 0x31E0 ->
  CHG_DTLS 1 (charging), PowerState 5 (online + charging), ChgCnfg00 0x15 (WDTEN + mode 5 CHG+BUCK), ChgCnfg06 0x0C,
  WdtKicked 1, SOC 53.5 %, AVG_I -13 mA (PC USB ~ load). Charging under Windows FIXED. Diagnostics are saved only
  on change, so PollOk stayed 1 in a flat 80 s window (expected). Unplug -> icon change: needs the user (cable).

### 2026-10-12 — "icon does not change on unplug" (user report after batt2): instrumentation (1.0.0.2)
- Offline SYSTEM hive (pulled from TWRP, dirty -> `reg load` refuses "corrupt"; scanned vk records directly,
  scratchpad vkscan.py): only ONE copy of the diagnostics exists, PollOk 1 = the first poll of boot #93
  (SOC 63.9 %, PowerState 5, ChgDetails 0x31E0). The values are overwritten in place, so the user's unplug left
  no trace -> cannot tell whether the poll saw it or the class driver never re-queried.
- Code review of the path: periodic 20 s timer -> passive work item -> S10eBattPoll -> Notify on PowerState /
  percent / tag change -> BatteryClassStatusNotify; SetStatusNotify returns STATUS_NOT_SUPPORTED (simbatt
  pattern). Nothing visibly wrong.
- 1.0.0.2 adds: NotifyCount, QueryStatusCount, QueriedPowerState (Device Parameters), a 16-slot history
  `HistNN` = "n= t= ps= soc= chg= i= q= qps=" written on every notify, and a diagnostics save every 15 polls
  (5 min) so a re-query after the last notify shows up. Package s10e-artifacts\battery-m3 (sys 4B0214D4…).
- Reading after the user's unplug test: pull SYSTEM, `python -I vkscan.py` for DWORDs; HistNN are REG_SZ (grep
  the hive for UTF-16 "n=").
  - If a Hist line with ps=0 or 2 (no ON_LINE) exists, the poll saw the unplug.
  - If q does not rise after it, the class driver never re-queried, which means the batclass/compbatt wiring.
  - If no such line exists, the reading itself did not change (CHGIN_DTLS stayed 3).
- battstat run (boot #93 + 1.0.0.2, cable to the PC, 6 x 60 s): PollOk 15 at 5 min, QueryStatusCount 12,
  QueriedPowerState 5; Windows view (Win32_Battery / root\wmi BatteryStatus): PowerOnline True, Charging True,
  64 %, Voltage 3948 -> 3942 mV across samples. **The driver -> batclass -> Windows path is live**; Windows
  re-reads status. So an unplug that does not change the icon means the *reading* did not change.
- Likely cause (code reading, UNVERIFIED on the device): booted with no cable, Beyond0LteUsbHostDxe falls back
  to OTG boost (CNFG_00 mode 0xE, Pmic.c Max77705...OtgOn, charger engine OFF) and nothing in Windows turns it
  off. The phone then drives VBUS itself, so CHGIN_DTLS probably stays "valid" -> "plugged in" forever, and a
  charger plugged in later does not charge (engine off).
- 1.0.0.3: mode 0xE/0xF (UsbHw.h:320-321 [K]) -> report off line + discharging (BuildStatus), with a
  Cnfg00Read valid flag so a failed read cannot look like "boost off". Package battery-m4.
  - `Guided` until the user boots without a cable and the Hist lines show ps=2 with CNFG_00 mode E.
  - Charging after a later plug-in needs an attach-driven mode switch (OTG off -> CHG+BUCK). That is policy
    with hardware risk, so it goes to parkinglot/usb-host-hotplug-and-power.md.
