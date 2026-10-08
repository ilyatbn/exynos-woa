# S6SY761TouchBeyond0 — Exynos 9820 (Galaxy S10e) touch package

**Never built, never signed, never run on a 9820 device.** This folder is a copy of
`../S6SY761Touch`, the gesture-report candidate validated on the Galaxy S9+. Under
this repo's golden rule nothing here may be called `Ready` until it has been
installed and observed on an S10e.

**Origin.** The upstream driver is TheMorc's (Richard Gráčik's)
[S6SY761Touch](https://github.com/TheMorc/S6SY761Touch), GPL-2.0, itself built on
Microsoft's vhidmini2 KMDF sample (MS-PL) and a fork of Gustave Monce's
[nt36xxx_win](https://github.com/edk2-porting/nt36xxx_win), porting the Linux
`sec_ts` / `s6sy761` driver by Andi Shyti and Samsung Electronics. Their copyright
notices are kept in the sources; see the repository's `NOTICE`.

**What actually differs on the S10e, and what does not.** Verified live
(`notes/dock-desktop.md` §14):

| Thing | State on the S10e |
|---|---|
| HSI2C bus the panel IC hangs off | **identical** — `touchscreen@48` is a child of `hsi2c@104B0000` |
| Panel slave address | **identical** — `0x48` |
| Panel family | **`y761`** — the blob is `tsp_sec/y761_beyond0_phole.bin`, so this driver's name is not wrong for this model (`y771` is only the kernel source family) |
| `sec_ts` command opcodes used here (`0x60`, `0x61`, `0x62`, `0x10`) | **identical** to the 9820 list in `notes/exynos9820-research.md` §8 |
| ACPI binding | unchanged — `ACPI\SCSY0761`, which the 9820 firmware (M8) must emit |
| **Panel geometry in the HID descriptor** | **still the S9+'s** — see the block above `G_DefaultReportDescriptor` in `Device.c`; this is the one thing that genuinely has to change |
| Fingerprint-on-glass | **does not apply** — the S10e's own touch node has no `support_fod` |

The fork lineage is `../S6SY761Touch`, whose own baseline is
`..\S6SY761TouchGpio\RetainedV24`; that frozen baseline is not modified by either
package. The starting `Device.c` SHA-256 of the 9810 candidate is
`3a6027c60c9c1f4967d66d0dc049ed120059c7a778886a9884c3ba325984be38` — it identifies
the **9810** source, not a validated 9820 binary. The INF version is `6.0.5058.3`.

## Report-delivery changes

These are the changes the 9810 candidate made, inherited unchanged by this copy:

* Preserve finger-down position and scan time. Only consecutive movement-only
  reports with identical active-contact sets can replace one another.
  Presses, releases and an unknown contact's first MOVE remain boundaries.
* Hold an entire 64-event controller FIFO burst without the old 32-report
  overflow reset. A full queue can still coalesce its last movement report.
  Sustained overload retains the existing counted all-up recovery.
* Do not consume a report when a HID read buffer is too short. The next valid
  read receives the same down/up transition instead of silently losing it.

These fix reproducible software defects under batching/backpressure. They do
not establish the cause of a particular physical gesture failure. No guessed
debounce, coordinate smoothing, gesture timeout, synthetic release or Windows
registry setting is introduced. Windows continues to recognize gestures.
Electrical noise, controller/I2C errors, Windows gesture configuration and
physical usability still require observations on the phone.

**One addition for this model:** the S10e's touch node declares
`sync-reportrate-120`, so the coalescing rules above are exercised at a higher
report rate than on the S9+. Re-qualifying them on the S10e is a task, not an
assumption.

## Build and regression coverage

Use the existing VS/WDK installation; choose a new output directory outside
the repository:

```powershell
.\Build-Touch.ps1 -OutputDirectory "$env:TEMP\touch-build"
```

The script builds and executes production C through the existing touch WDF/SPB
host mocks, then builds an actual ARM64 SYS, validates the INF and generates an
**unsigned** catalog/package. It does not install, sign, flash, reboot or change
device trust. `touch-build.json` records the source/package/compiler identities
and explicit hardware/deployment limitations.

Nine host groups cover retained down coordinates/timestamps, a three-second
hold and scan-time rollover, two taps with moves in one FIFO batch, all 64 FIFO
transitions, full-queue movement coalescing and ring wrap, short buffers on down
and up, ten contacts, I2C/overload recovery, cancellation and completion reentry.
The same tests compile against untouched v24 as a negative control: five
selected cases must fail at their specific regression assertions.
The shared mock clock defaults to its previous value, so the existing GPIO
migration's descriptor/report equivalence suite remains unchanged.

Before installation, confirm the current phone's installed package and Windows
gesture settings, back up the package with a verified recovery route, and use
the established signing/deployment gates. Do not install alongside a GPIOClx
owner of the legacy ALIVE bank. Acceptance needs real repeated double-taps,
stationary holds, dragging and multitouch, including under load; host report
tests alone cannot qualify Windows gesture recognition.
