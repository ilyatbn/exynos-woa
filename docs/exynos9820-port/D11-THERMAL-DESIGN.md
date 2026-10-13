# D11 — Thermal management (sensors, trip points, cooling): design

Evidence: `notes/thermal.md` (T1 = shipped DTB, T2 = the vendor TMU driver, T3 = the Windows
contract, T4 = constants). Prior facts not re-derived here: the zone/trip table
(`notes/thermal.md` T1, correcting `notes/cpu-driver-9820.md` F6).
Constants: `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h`, block "Thermal (D11)".

## 1. Definition of done + rung ladder

Done means: **Windows sees real temperatures for this SoC and has a hard stop it will act on.**
Not in scope: Samsung's `power_allocator` governor semantics, the battery charge policy (U7), and
any attempt to reproduce the vendor's per-zone cooling graphs.

| Rung | Observation | Where | Proves |
|---|---|---|---|
| R0 | *(done, no device)* five TMU windows, five zones, full trip table, **no `critical` trip** | dumps | what the vendor's own stack was told, and what it was *not* told |
| R1 | `[TMU] R1: top con=%x thd0=%x pend=%x code0=%x -> %d C` on the debug serial, **zero writes** | UEFI | whether sboot/ACPM left the TMU's trip engine armed, and what the raw sensor codes look like in our boot state |
| R2 | the MMIO-derived °C agrees with a known-good reading (Android's `/sys/class/thermal/thermal_zone*/temp`, task `TH01`) within a few degrees | device | the one-point calibration math is right — this is the rung that makes every later number trustworthy |
| R3 | Windows: `Win32_PerfRawData_Counters_ThermalZoneInformation` gains an instance with a sane Kelvin `Temperature` | Windows | the ACPI thermal zone is described correctly and the in-box ACPI driver is polling `_TMP` |
| R4 | under load, `_PSV` crossed → `PercentPassiveLimit` drops below 100 and/or cores park (ACPI000C) | Windows | passive cooling actually does something |
| R5 | with an **injected** temperature (never a real over-temperature), `_HOT` hibernates and `_CRT` shuts down | Windows | the hard stop works — the safety claim this subsystem exists for |

Nothing above a rung is claimed until that rung is observed.

## 2. Hardware contract (all VERIFIED unless marked)

| Item | Value | Source |
|---|---|---|
| TMU TOP window | `0x10090000` + `0x800` — serves BIG (id 0), MID (id 1), LITTLE (id 2) | `base.dtb` |
| TMU SUB window | `0x100A0000` + `0x800` — serves G3D (id 3), ISP (id 4) | `base.dtb` |
| Interrupts | cells `0x1de`/`0x1df` = **SPI 478/479** → **GSIV 510/511** | `base.dtb`; +32 rule proven in `notes/video-mfc.md` T5 |
| Read a temperature | `CUR_TEMP0 (0x84) + (n/2)*4`, halfword `(n%2)`, mask `0x1FF`, sensor stride `0x50` | `exynos_tmu.c:1218-1224,140,153,198` |
| Code → °C | cal_type = bit 23 of `TRIMINFO(0)`; shipped DT says **one-point**: `°C = code − temp_error1 + 25`, `temp_error1 = TRIMINFO(n) & 0x1FF`, falling back to **55** when it reads 0 | `exynos_tmu.c:373-400,453-465`; `thermal_exynos.h:22` |
| Trip engine | `CONTROL (0x50)` bit 12 `TRIP_EN`, bit 0 `CORE_EN`; thresholds `THD0 (0xD0) + p*0x50`; enables `INTEN0 (0xF0) + p*0x50`; pending `INTPEND0 (0xF8) + p*0x50`; 8 trips max per TMU | `exynos_tmu.c:522-531,137-144,1105-1113` |
| Vendor trip values | BIG/MID passive **83 000**, hot **115 000**; G3D passive **88 000**, hot 115 000; LITTLE/ISP hot 115 000; `tmu_default_trip_temp = 115 °C` — **milli-°C**, a third unit (ACPI wants tenths of K, the TMU registers want whole °C codes) | `base.dtb` |
| ACPM path | channel **9**, 16-byte message, `READ_TEMP 0x02`, `AP_SUSPEND 0x04` / `AP_RESUME 0x10` | `exynos_acpm_tmu.h`; `base.dtb /acpm_tmu` |
| What a fired trip *does* in hardware | **UNVERIFIED** — the vendor writes `0x028A` into `COUNTER0[15:0]`/`COUNTER1[31:16]` but the source never says what the counter gates | closes: R1 (read `CONTROL`/`INTPEND`) + Samsung's TMU TRM |
| TMU clock gate | **UNVERIFIED**, and deliberately not needed: the vendor says the SFR is reachable over the Q-CH with the clock off | closes: `include/dt-bindings/clock/exynos9820.h` |

## 3. Two halves — who owns what

**Firmware half, part A — observation.** `firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteThermalDxe/`
maps both TMU windows and prints, with **zero writes**: `CONTROL`, `THD0..7`, `INTPEND0..7`,
`CUR_TEMP0..` and the calibrated °C for each of the five ids. That single line answers the safety
question this subsystem exists for: *is anything already protecting this SoC when no thermal
driver runs?*

**Firmware half, part B — the real deliverable: describe the zones in AML.** No driver of ours is
required for Windows to do thermal management; it needs a `ThermalZone` object. In
`firmware/Platform/Beyond0LtePkg/AcpiTables/Dsdt.asl`:

```
OperationRegion (TMUR, SystemMemory, 0x10090000, 0x800)   // TMU TOP
Field (TMUR, DWordAcc, Lock, Preserve) { … CUR at 0x84, TRIMINFO at 0x00 … }
ThermalZone (TZ00) {                 // BIG — sensors 6..10 per `sensors = <0x7C0>`
  Method (_TMP) {                    // tenths of **Kelvin**, not Celsius
      … code = one of the five BIG sensors (see the table below)
      … c   = code - (TRIMINFO(n) & 0x1FF or 55) + 25
      … Return ((c * 10) + 2732)   }  // spec: K = °C + 273.2
  Method (_PSV) { Return (3562) }    // 83.0 °C from the vendor DT, in tenths of K
  Method (_HOT) { Return (3882) }    // 115.0 °C — OS may hibernate
  Method (_CRT) { Return (3932) }    // 120.0 °C — OS **must** shut down (DECISION)
  Name (_PSL, Package () { …the eight processors… })
  Name (_TC1, 4)  Name (_TC2, 3)  Name (_TSP, 15)   // placeholders, see below
}
```

**Which register holds which zone's sensor** (arithmetic from
`exynos_tmu.c:1218-1224`: sensor *n* sits at `CUR_TEMP0 (0x84) + (n/2)*4`, halfword `n%2`;
the sensor lists are the `sensors` masks read in `notes/thermal.md` T1):

| Zone | window | sensors | register offsets to read |
|---|---|---|---|
| BIG | TOP | 6,7,8,9,10 | `0x90` lo+hi, `0x94` lo+hi, `0x98` lo |
| MID | TOP | 5 | `0x8C` hi |
| LITTLE | TOP | 4 | `0x8C` lo |
| G3D | SUB | 1,2,3,4,9 | `0x84` hi, `0x88` lo+hi, `0x8C` lo, `0x94` hi |
| ISP | SUB | 8 | `0x94` lo |

**The gap this exposes:** four of the five zones have **more than one sensor** (BIG has five), and
the DT says how to combine them with `sensing_mode = "balance"` or `"max"` — a policy the **ACPM
firmware** implements and the GPL source never does. A pure-AML `_TMP` therefore has to choose:
read one named sensor, or take the max of the set in AML. The design's first cut is **max of the
zone's sensors** (the safe direction for a safety threshold), stated as a decision, with
`"balance"` left UNVERIFIED (P8).

**The unit is the trap, and it is Kelvin.** ACPI §11.1: "All thermal zone objects must report
temperatures in Kelvin when reporting absolute temperature values… Kelvin is expressed as follows:
`θ/K = T/(degrees Celsius) + 273.2`", and `_TMP`'s return value is "an Integer containing the
current temperature of the thermal zone (**in tenths of degrees Kelvin**)"
(`uefi.org/specs/ACPI/6.5/11_Thermal_Management.html`). The vendor's DT trips are in
**milli-°C** (`83000` = 83 °C) — a different unit again. Converting straight from the DT number
into `_PSV` would tell Windows the passive threshold is 8.3 K. Every value in the block above is
`(°C + 273.2) × 10`, using the spec's own 273.2 constant rather than 273.15 so the AML matches the
specification's arithmetic; the 0.05 K difference is below the sensor's resolution (P2).

`_CRT` is **not** in the vendor DT at all — there is no `critical` trip anywhere in it (T1) — so
120 °C is a **DECISION**, placed above the vendor's `hot` 115 °C so that hibernate fires first and
shutdown is the last resort. It must be stated as a decision, not passed off as a read.

`_TC1 = 4`, `_TC2 = 3`, `_TSP = 15` are lifted from the ACPI specification's *example* (the spec
itself labels its example constants "bogus"). They are placeholders: the real passive-cooling
constants for a fanless phone SoC are an open item, and the honest first move is to ship `_PSV` +
`_PSL` + `ACPI000C` (core parking, which does not use `_TC1/_TC2` at all) and leave the
throttling-equation constants until R4 shows what Windows actually does with them.

`_TZD` is deliberately **omitted** in the first cut: it lists the *non-processor* devices in a
zone, and the devices that actually generate the heat here (the cpufreq domains, the Mali, the
ISP) have no Windows driver to be members yet. Listing something like `\_SB.GPIO` would be noise.

**Role-latch contract.** The ACPM owns the sensors (reads go over IPC; `AP_SUSPEND`/`AP_RESUME`
prove it runs without the AP). Therefore: **our AML only reads.** No `CONTROL`, `THD`, `INTEN` or
`COUNTER` write happens anywhere in D10/D11 firmware; the TMU windows appear in the thermal zone's
`OperationRegion` and are **not** handed to any `_HID` device, so no Windows class driver can
reprogram them. If a later cooling driver needs to write them, that is a new decision with its own
rung.

**Windows half.** Nothing to write for R3/R4 — the in-box ACPI driver evaluates `_TMP`/`_PSV`/
`_HOT`/`_CRT` and drives passive cooling, including **core parking via `ACPI000C`**
(`bringup/acpi-defined-devices`). Only if R4 turns out to be too coarse does a driver appear, and
then through `GUID_THERMAL_COOLING_INTERFACE` (`kernel/device-level-thermal-management`).

## 4. Verification layering

| Layer | What it does | Rung | Reference |
|---|---|---|---|
| Observation | print TMU state at DXE entry | R1 | our own `Beyond0LteModemDxe` pattern |
| Calibration | AML arithmetic reproducing `code − efuse + 25` | R2 | `exynos_tmu.c:373-400` (GPL — transliterate) |
| Zone description | `ThermalZone` + `_TMP/_PSV/_HOT/_CRT/_PSL/_TZD/_TC1/_TC2/_TSP` | R3 | ACPI 6.5 §11; `acpi-defined-devices` |
| Core parking | `ACPI000C` in the zone | R4 | same page ("Windows will park many cores when `_PSV` is crossed") |
| Hard stop | `_CRT` → OS shutdown | R5 | ACPI §11.4.4 |

## 5. License posture

- **Adoptable**: nothing needed — the Windows side is in-box.
- **Reference only (GPL-2.0)**: `drivers/thermal/samsung/exynos_tmu.c`, `exynos_acpm_tmu.{c,h}` —
  the register offsets and the calibration formula are transliterated into AML/C with the citation
  beside them; no code is pasted.
- **Firmware/blobs**: none. The ACPM firmware is not read, copied or committed.
- Contributed code here: BSD-2-Clause-Patent.

## 6. Risks / open questions (each names its closing read)

| # | Risk or unknown | Closing read | Cost if wrong |
|---|---|---|---|
| P1 | Is the TMU already armed by sboot/ACPM, or is nothing protecting the SoC under Windows? | **R1**, zero writes: `CONTROL` bit 12 and `INTPEND` | this is the headline question; the answer decides how urgent `_CRT` is |
| P2 | The MMIO code→°C math may be wrong (efuse read back as 0, two-point trimming actually in use, sensor_type 8 quirks) — **and the three unit systems (milli-°C in the DT, whole °C in the TMU registers, tenths of K in ACPI) are a live trap** | **R2** against a known-good Android reading (`tasks/TH01-*.md`) | a wrong temperature makes Windows throttle at the wrong time — or never; a unit slip puts `_PSV` at 8 K and the zone is permanently "overheating" |
| P3 | Reading TMU registers from AML while the ACPM also owns them | R1 shows whether reads are side-effect free (they are reads of status registers) | a read that clears a pending bit could disarm a vendor trip |
| P4 | `_CRT` value is a DECISION, not a read | cross-check against the TMU's `tmu_default_trip_temp = 115` and any TRM limit | too low → spurious shutdowns; too high → the protection arrives late |
| P5 | Does Windows on ARM64 actually poll ACPI thermal zones, or is that x86 legacy? | **R3** — the perf-counter instance either exists or it does not | if it does not, D11 needs a real driver and the plan changes |
| P6 | Two zones (LITTLE, ISP) are never polled even by Linux | `tasks/TH01-*.md` runtime view | a zone we describe may have no live sensor behind it |
| P7 | Battery/cell safety (the part that can actually be damaged) is not in this subsystem | U7 + `notes/battery-driver.md` | describing CPU zones does not protect the cell; say so, do not imply it |
| P8 | `sensing_mode = "balance"` / `"max"` — how the vendor combines a zone's several sensors — is implemented inside the ACPM firmware, not in any source we have; `"balance"` is **UNVERIFIED** in both cases | read all of a zone's sensors at once in R1 and compare them against the ACPM-reported value (and against Android's per-zone reading in `TH01`) | picking the wrong sensor under-reports the hot spot and the threshold arrives late; picking the max is conservative but may park cores too early |

## 7. What the tester does first

```
# after flashing an FD that carries Beyond0LteThermalDxe (never this repo's GCC FD):
grep -E '\[TMU\]' <serial log>
```

Expected: `[TMU] R1: top con=… thd0=… pend=… code0=… -> NN C`. Rollback: a reboot — the module
writes nothing. **No deliberate over-temperature test is ever run on the device**; R5 is verified
with an injected value, not with heat. Task files: `tasks/TH01-thermal-live-reads.md` (the
known-good comparison for R2) and `tasks/TH02-tmu-r1-observe.md` (R1).
