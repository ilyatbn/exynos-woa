# Thermal management on the S10e (sensors, trip points, cooling) — research + Windows driver

Task: research the S10e's thermal stack — what sensors exist, what trip points the vendor uses,
what can actually *cool* the SoC, and what Windows would do with any of it. The stated worry is
safety: a docked phone-desktop under load must not be left with no thermal control. The pointer
that opened the topic is `github.com/ianmacd/d2s` → `drivers/thermal` (the same Exynos 9820 vendor
kernel tree already inventoried for D10 — see `notes/video-mfc.md`). Mode: **research**.
Started 2026-10-13. Follows `notes/cpu-driver-9820.md` (D7 — its F6 section already read the
thermal zones out of the shipped DTB) and `notes/dock-desktop.md` (M14).

## Progress log

### 2026-10-13 — T0: framing — what the plan of record already says about heat (fact, cited)

Do **not** re-derive any of this; it is already read from the shipped dump:

- **Thermal zones are in the shipped DTB** — `notes/cpu-driver-9820.md` F6: zones `BIG`
  (`BIG_THERMAL`), `MID` (`MID_THERMAL`), `LITTLE` (`LITTLE_THERMAL`), `G3D` (GPU); polling 50 ms
  passive / 1000 ms; governor `power_allocator`; BIG trips in milli-°C: cold 20 000, switch-on
  55 000, **control-temp 83 000 (passive)**, alert0 95 000, alert1 100 000, alert2 105 000,
  alert3 110 000, **hot 115 000**; hysteresis 5 000. Cooling maps bind the three `cpufreq/domain@*`
  domains (BIG→`domain@2`, MID→`domain@1`, LITTLE→`domain@0`).
- **TMU reads are not MMIO** — same section: the sensors are reached through `acpm_tmu`
  (channel `0x9`) over **ACPM IPC**; `acpm_ipc` sits at `reg 0x15900000+0x1000` (and
  `0x2039000+0x30000`), `acpm` at `0x15830000+0x1000` with `fvmap_offset 0xa400`.
- **Samsung's own device-freq/thermal stack is on this board** — `notes/dock-desktop.md:175-177`:
  `sec_thermistor@0/2/7`, `battery`, `vbat`, `hall` are live platform nodes, and the frequency/
  thermal machinery is `devfreq_*` (`17000010.devfreq_mif` …).
- **The plan of record already flags the gap** — `MILESTONES.md:323` (M14): "Measure thermals under
  a desktop workload … **needs-phone** — Samsung's own thermal stack (`devfreq_*`,
  `sec_thermistor@*`) has no Windows equivalent"; `notes/battery-driver.md:60` repeats it.
- **D7's design row already assumes a thermal clamp** — `DRIVERS-MILESTONES.md:151`: the Windows CPU
  driver is to carry a "thermal clamp from DT trips" (still `todo`).

### 2026-10-13 — T1: shipped-DTB walk — five TMUs, five zones, and **no critical trip** (fact)

Method: raw DTB through `tools/dump-tools/fdt.py` (Samsung v40 layout — the decompiled `base.dts`
garbles phandles, `notes/modem-wwan.md` T1). Walkers kept at `/tmp/opencode/thermal/`
(`dump_thermal.py`, `dump_subtree.py`, `thermal_trips.py`); output `thermal-dtb.txt`,
`thermal-zones.txt`. Overlays: `dumps/hhxj6/dtbo.img` split with `fdt.split_blobs`.

**Five TMU controller nodes** in `base.dtb` (not four — see the correction below):

| Node | compatible | reg | interrupts (raw) | SPI → GSIV | id | `sensors` mask | sensing_mode |
|---|---|---|---|---|---|---|---|
| `/BIG@10090000` | `samsung,exynos9820-tmu` | `0x10090000 +0x800` | `<0 0x1de 0>` | 478 → **510** | 0 | `0x7C0` | balance |
| `/MID@10090000` | `samsung,exynos9820-tmu` | `0x10090000 +0x800` | `<0 0x1de 0>` | 478 → 510 | 1 | `0x20` | max |
| `/LITTLE@10090000` | `samsung,exynos9820-tmu` | `0x10090000 +0x800` | `<0 0x1de 0>` | 478 → 510 | 2 | `0x10` | max |
| `/G3D@100A0000` | `samsung,exynos9810-tmu` | `0x100A0000 +0x800` | `<0 0x1df 0>` | 479 → **511** | 3 | `0x21E` | balance |
| `/ISP@100A0000` | `samsung,exynos9810-tmu` | `0x100A0000 +0x800` | `<0 0x1df 0>` | 479 → 511 | 4 | `0x100` | max |

GSIV arithmetic is the repo rule (DT cell = SPI, GSIV = SPI + 32), now proven twice — see the
"SPI-vs-GSIV" section under T3 of `notes/video-mfc.md`.

Every TMU node carries the same calibration/trip properties (decimal):
`samsung,tmu_cal_type = 0`, `tmu_default_temp_offset = 50`, **`tmu_default_trip_temp = 115`**,
`tmu_efuse_value = 55`, `tmu_first_point_trim = 25`, `tmu_second_point_trim = 85`,
`tmu_sensor_type = 8`, `tmu_gain = 0`, `tmu_noise_cancel_mode = 4`, `tmu_reference_voltage = 0`.
`/BIG@10090000` additionally: `hotplug_enable = 1`, `hotplug_in_threshold = 91`,
`hotplug_out_threshold = 96`.

**ACPM side** (the coprocessor that owns the sensors): `/acpm_tmu` → `acpm-ipc-channel = 9`;
`/acpm_ipc` → `reg = <0 0x15900000 0x1000>, <0 0x2039000 0x30000>`, `dump-base 0x203c000`,
`interrupts = <0 0x28 0>` (SPI 40 → GSIV 72); `/acpm` → `reg 0x15830000+0x1000`,
`reg-names = timer_apm`, `fvmap_offset 0xa400`.

**Five thermal zones**, full trip table (milli-°C, from `base.dtb`):

| Zone | governor | polling / passive | trips (temp, type) | cooling device |
|---|---|---|---|---|
| `BIG` | power_allocator | 1000 / 50 | 20000 active, 55000 active, **83000 passive**, 95000/100000/105000/110000 active, **115000 hot** | `/cpufreq/domain@2` (bounds `0xffffffff,0xffffffff`) |
| `MID` | power_allocator | 1000 / 50 | identical to BIG | `/cpufreq/domain@1` |
| `LITTLE` | *(none)* | **0 / 0** | 20000, 76000, 81000, 86000, 91000, 96000, 101000 active, **115000 hot** | `/cpufreq/domain@0` (8 maps, bounds `0,0`) |
| `G3D` | power_allocator | **0** / 100 | 20000 active, 78000 active, **88000 passive**, 105000/110000/115000/115000 active, **115000 hot** | **`/mali@18500000`** |
| `ISP` | *(none)* | **0 / 0** | 20000, 91000, 96000, 101000×4 active, **115000 hot** | **`/fimc_is@17190000`** |

- **CORRECTION to `notes/cpu-driver-9820.md` F6**: it lists four zones (BIG/MID/LITTLE/G3D).
  The shipped `base.dtb` has **five** — `ISP` is the fifth, and it cools the camera ISP
  (`/fimc_is@17190000`), not a CPU domain. F6 is also incomplete on the GPU: `G3D`'s cooling device
  is the Mali node, which F6 did not state.
- **The safety headline: there is no `critical` trip anywhere in the DT.** A whole-tree scan of
  every `/thermal-zones/*/trips/*` node finds **0 trips of type `critical`** and **5 of type `hot`**
  — `big-hot`, `mid-hot`, `little-hot`, `gpu-hot`, `isp-hot`, all at **115 000** milli-°C (raw
  property bytes `b'hot\x00'` for all five). Nothing in this DT asks the OS for a critical
  power-off.
- Two zones (`LITTLE`, `ISP`) have **`polling-delay = 0` and no governor** — the AP's thermal
  framework does not poll them at all; their trips only matter if something else watches them.
- **Thermistors are enabled on this board**: every one of the 7 `dtbo` overlays sets
  `sec_thermistor@0` (id 0), `sec_thermistor@2` (id 1), `sec_thermistor@7` (id 2) to
  `status = "okay"`, `compatible = "samsung,sec-thermistor"`. What they guard and what ADC reads
  them is still open (U3).

### 2026-10-13 — T2: the vendor TMU driver — MMIO map, calibration math, and what the AP actually programs (fact)

Source: the Exynos 9820 vendor kernel clone (`ianmacd/d2s`, Linux 4.14.113, Note 10 family —
same SoC, different product; see `notes/video-mfc.md` for the tree's provenance and caveats).
`CONFIG_THERMAL=y`, `CONFIG_EXYNOS_THERMAL=y`, `CONFIG_EXYNOS_ACPM_THERMAL=y`
(`arch/arm64/configs/exynos9820-d2s_defconfig:2782,2811,2812`);
`CONFIG_THERMAL_EMERGENCY_POWEROFF_DELAY_MS=0` (`:2783`); governors `step_wise` +
`power_allocator` (`:2792,2795`), `CONFIG_THERMAL_EMULATION=y` (`:2801`).

**9820 TMU register map** (`drivers/thermal/samsung/exynos_tmu.c:129-153`, the block selected by
`CONFIG_SOC_EXYNOS9820`): `CONTROL 0x50`, `CONTROL1 0x54`, `STATUS 0x80`,
`CURRENT_TEMP1_0 0x84`, `THD_TEMP0 0xD0`, `INTEN0 0xF0`, `INTPEND0 0xF8`,
per-sensor stride `SENSOR_OFFSET 0x50`, `TRIMINFO(p) = p*4`, `TRIM0 0x5C`, `EMUL_CON 0xB0`,
`AVG_CON 0x58`, `COUNTER_VALUE0 0x74`, `COUNTER_VALUE1 0x78`, `TEMP_SHIFT 16`,
`TEMP_MASK 0x1ff` (9-bit codes), `TRIMINFO_85_P0_SHIFT 9`, `CALIB_SEL_SHIFT 23 / MASK 0x1`
(`:196-199`).

**A temperature is readable over plain MMIO** — the vendor's own debugfs does exactly that
(`exynos_tmu.c:1218-1224`): for sensor *n*, `readl(base + 0x84 + (n/2)*4) >> ((n%2)*16) & 0x1ff`,
then convert the code to °C. The conversion (`code_to_temp_with_sensorinfo`, `:373-400`):

- `cal_type` = bit 23 of `TRIMINFO(0)`; `TYPE_ONE_POINT_TRIMMING = 0`,
  `TYPE_TWO_POINT_TRIMMING = 1` (`include/dt-bindings/thermal/thermal_exynos.h:22-23`) —
  **the shipped DT sets `samsung,tmu_cal_type = 0`**, i.e. the one-point branch.
- `temp_error1 = TRIMINFO(sensor) & 0x1ff`; if it reads 0, the driver falls back to
  `pdata->efuse_value & 0x1ff` (`:453-465`) — DT value **55**.
- one-point: `temp = code - temp_error1 + first_point_trim` (DT `first_point_trim` **25**);
  two-point: `temp = (code - temp_error1) * (85 - 25) / (temp_error2 - temp_error1) + 25`.
- result clamped to `EXYNOS_MIN_TEMP … EXYNOS_MAX_TEMP` (the two bounds are not in the block read
  — U9).

**But the AP's normal read path is the ACPM, not MMIO**: `SOC_ARCH_EXYNOS9820` wires
`tmu_read = exynos98X0_tmu_read` (`:1105-1113`), which calls
`exynos_acpm_tmu_set_read_temp(data->tzd->id, &temp, &stat)` (`:839`) and returns whole °C —
the framework multiplies by 1000 at `:694`. The IPC contract is fully specified in
`exynos_acpm_tmu.h`: a **16-byte message**, request types `TMU_IPC_INIT 0x01`,
`TMU_IPC_READ_TEMP 0x02`, `TMU_IPC_AP_SUSPEND 0x04`, `TMU_IPC_CP_CALL 0x08`,
`TMU_IPC_AP_RESUME 0x10`; response fields `ret`, `tzid`, `temp` (u8), `stat`; capability bits
`CAP_APM_IRQ 0x1`, `CAP_APM_DIVIDER 0x2`. Transport is `acpm_ipc_send_data(channel, &config)`
(`soc/samsung/acpm_ipc_ctrl.h`), channel 9 per the DT.
**`AP_SUSPEND`/`AP_RESUME` are direct evidence that the ACPM keeps monitoring while the AP is
asleep** — i.e. the sensor side does not depend on the AP OS being alive.

**What the AP does program** (this is the part that matters for a Windows design):
`exynos98X0_tmu_initialize`/`_control` write the DT trip temperatures into the TMU's own threshold
registers — `THD(p) = 0xD0 + p*0x50`, rising/falling threshold pairs packed 16 bits each,
`PASSIVE` trips deliberately skipped (`:481-500`) — enable the per-trip interrupts in
`INTEN(p) = 0xF0 + p*0x50` (`:560-590`), set `COUNTER_VALUE0` field `EN_TEMP_SEN_OFF`
(bits 0-15) and `COUNTER_VALUE1` field `CLK_SENSE_ON` (bits 16-31) to **`0x028A`**
(`:646-652`, defines `:228-233`), and finally `tmu_core_enable()` sets **bit 12 `TRIP_EN`** and
bit 0 `CORE_EN` of `CONTROL` (`:522-531`). So the TMU's trip engine is armed **in hardware**;
what the hardware then *does* (interrupt only, or clock-stop / sensor-off) is **not stated** in
this source → U1 stays partly open.

### 2026-10-13 — T3: the Windows contract — an ACPI thermal zone is an inbox feature (U5/U6 CLOSED)

- **Windows' thermal model *is* the ACPI thermal zone**, and Microsoft ships the support for
  SoC platforms: "For SoC-based platforms, Windows provides built-in drivers to support the
  ACPI-defined devices … To support ACPI thermal management, the system designer logically
  partitions a hardware platform into one or more physical regions called thermal zones."
  A zone lists `_TZD` (devices) and `_PSL` (processors); sets `_PSV` (OS starts passive cooling),
  `_HOT` (**OS hibernates**), `_CRT` (**OS shuts down**); describes passive behaviour with
  `_TC1`/`_TC2`/`_TSP`; and reports temperature by **"`_TMP` for firmware-reported temperature, or
  `_HID` and `_CRS` for loading a temperature sensor driver"**.
  — `learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-defined-devices`
- **Core parking is available to us for free**: "The platform can indicate to the operating system
  that processor cores in the thermal zone should be idled (instead of throttled). This is done by
  including the Processor Aggregator device (**ACPI000C**) in one or more thermal zones.
  **Windows will park many cores when the thermal zone's `_PSV` is crossed.**" — same page.
  (Our DSDT already publishes PPTT and the 8 GICC/`_MAT` processors — `notes/cpu-driver-9820.md`.)
- **Driver-side hook** if AML is not enough: "Starting with Windows 8, Windows supports thermal
  management for kernel-mode device drivers … A relatively simple way for a device driver to
  support thermal management is to implement the `GUID_THERMAL_COOLING_INTERFACE` driver
  interface." — `learn.microsoft.com/en-us/windows-hardware/drivers/kernel/device-level-thermal-management`
- **Units and semantics** are the ACPI spec's: `_TMP`/`_PSV`/`_HOT`/`_CRT` return **tenths of a
  degree**; "If the `_TMP` object returns a value greater than or equal to the value returned by
  the `_CRT` object then **OSPM must shut the system down**"; `_HOT` → may transition to S4;
  passive cooling uses `ΔP = _TC1·(Tn − Tn−1) + _TC2·(Tn − Tt)` sampled every `_TSP`.
  — `uefi.org/specs/ACPI/6.5/11_Thermal_Management.html` (§11.1, §11.4)
- **Observable from user mode** (a verification rung, not a hope): the in-box performance-counter
  set `Win32_PerfRawData_Counters_ThermalZoneInformation` exposes `Temperature` (Kelvin),
  `PercentPassiveLimit` and `ThrottleReasons` per thermal zone — so "the zone exists and Windows is
  polling it" is checkable without any driver of ours.
- **Green field in our tree**: `grep ThermalZone` over `firmware/` finds it only inside edk2's own
  AML encoder library (`MdeModulePkg/…/Aml.c`, `DynamicTablesPkg/…/AmlCodeGen.c`) — **no thermal
  zone is described anywhere in the 9810 or 9820 platform tables today.**

### 2026-10-13 — T4: constants frozen into `Exynos9820.h` + memory map (VERIFIED unless marked)

Landed in `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` (new "Thermal (D11)"
block) plus two `PlatformMemoryMapLib` entries (`TMU_TOP`, `TMU_SUB`, mapped Device).

| Constant | Value | Status | Source |
|---|---|---|---|
| `EXYNOS_TMU_TOP_BASE/SIZE` | `0x10090000` / `0x800` | VERIFIED | `base.dtb /BIG@10090000 reg` |
| `EXYNOS_TMU_SUB_BASE/SIZE` | `0x100A0000` / `0x800` | VERIFIED | `base.dtb /G3D@100A0000 reg` |
| `EXYNOS_TMU_TOP_GSIV` / `_SUB_GSIV` | **510** / **511** | VERIFIED | DT cells 478/479 (SPI) + the +32 rule |
| `TMU_REG_CONTROL/CONTROL1/STATUS/CUR_TEMP0/THD0/INTEN0/INTPEND0/TRIMINFO(p)/TRIM0/AVG_CON/COUNTER0/COUNTER1/EMUL_CON` | `0x50/0x54/0x80/0x84/0xD0/0xF0/0xF8/p*4/0x5C/0x58/0x74/0x78/0xB0` | VERIFIED (mirror) | `exynos_tmu.c:129-153` |
| `TMU_SENSOR_STRIDE` / `TMU_TEMP_SHIFT` / `TMU_TEMP_MASK` | `0x50` / `16` / `0x1FF` | VERIFIED (mirror) | `exynos_tmu.c:140,153,198` |
| `TMU_CALIB_SEL_SHIFT` / `TMU_TRIP_EN_SHIFT` / `TMU_CORE_EN_SHIFT` | `23` / `12` / `0` | VERIFIED (mirror) | `exynos_tmu.c:196,174,164` |
| `TMU_TRIM_EFUSE_VALUE` / `FIRST_POINT` / `SECOND_POINT` / `DEFAULT_TEMP_OFFSET` / `DEFAULT_TRIP_TEMP` | `55 / 25 / 85 / 50 / 115 °C` | VERIFIED (DT) | `base.dtb` TMU nodes |
| `EXYNOS_ACPM_TMU_IPC_CHANNEL` | `9` | VERIFIED | `base.dtb /acpm_tmu acpm-ipc-channel` |
| the `0x028A` counter value | — | **deliberately NOT defined as a usable constant** — semantics UNVERIFIED (U1) | `exynos_tmu.c:233,646-652` |
| TMU clock gate id | — | **UNVERIFIED** — closes by: `include/dt-bindings/clock/exynos9820.h` grep for the TMU gate (the D10 pass found the CMU bank for MFC at `0x1a240000`; the TMU gate is not needed for a read-only observation) | — |

**Build evidence** (Linux verification build, regenerated recipe
`/tmp/opencode/build-uefi-linux.sh` from `notes/d1-build-env.md`, gcc 12.3.1): exit **0**;
`BEYOND0LTE_UEFI.fd` **7 340 032 B**; `FVMAIN_COMPACT` 421 888 B total, **24 B free** (auto-sized
FV, always near full — the FD itself has MBs of slack); `FVMAIN` 47 % full. Five warnings, all
pre-existing (`FuelGauge.c:209`, three `DebugLib.h:383` instances of it, `PeilessSec.c:169`);
zero errors. **Verification-only FD — never flash.**

### What is NOT yet known (each needs a source, not a guess)

- U1 (**the safety question, partly open**): the TMU trip engine is armed in hardware
  (`TRIP_EN`, `exynos_tmu.c:529`) and thresholds come from the DT, but **what the hardware does
  when a trip fires** — interrupt only, clock-stop, or sensor-off — is not stated in the GPL
  source, and the DT has **no `critical` trip**. Closes by: (a) reading `CONTROL`/`THD`/`INTPEND`/
  `COUNTER_VALUE` at DXE entry with **zero writes** (the firmware-half rung R1 below), and
  (b) Samsung's TMU TRM section for the 9820 (the `EN_TEMP_SEN_OFF` / `CLK_SENSE_ON` fields,
  value `0x028A`).
- U2 **CLOSED** (T1): five TMU controllers, five zones, sensor masks, GSIVs 510/511.
- U3: what the three `sec_thermistor` channels guard and which ADC reads them — closes by: the
  vendor charger/fuel-gauge source in the same tree (`drivers/power/supply`,
  `notes/battery-driver.md`) + the DTB's `sec-thermistor` subnodes (they carry only `id` + `status`
  in the overlays; the base DTB side still unread).
- U4: which cooling devices Windows can actually reach — cpufreq domains (D7's driver, `todo`),
  Mali (`/mali@18500000`, D5b), ISP (`/fimc_is@17190000`, no milestone) — closes by: the design
  pass; today **none of the three has a Windows driver**, which is the honest state of passive
  cooling on this board.
- U5/U6 **CLOSED** (T3).
- U7: the battery-side limits (charge current, over-temperature stop) — closes by:
  `notes/battery-driver.md` + the vendor `max77705`/charger source; not attempted this pass.
- U8: Samsung's thermal *policy* files — `/vendor/firmware` in the shipped dump has **no**
  thermal/TMU/ACPM blob among its 37 entries (`debugfs -R 'ls /firmware'
  dumps/hhxj6/vendor/vendor.raw`), so the ACPM firmware is loaded from another partition; the
  `thermal-engine` XMLs the D7 notes expected live in `super.img`/vendor and are unread here.
- U9: `EXYNOS_MIN_TEMP` / `EXYNOS_MAX_TEMP` clamp bounds — closes by: reading the defines outside
  the block already read in `exynos_tmu.c`.

### Constants frozen (all VERIFIED unless marked)

Nothing has entered `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` yet — that is
the next task, and it must land with the memory-map entries in the same commit.

| Constant | Value | Status | Source |
|---|---|---|---|
| `EXYNOS_TMU_TOP_BASE` / size | `0x10090000` / `0x800` | VERIFIED | `base.dtb` `/BIG@10090000` `reg` |
| `EXYNOS_TMU_SUB_BASE` / size | `0x100A0000` / `0x800` | VERIFIED | `base.dtb` `/G3D@100A0000` `reg` |
| TMU TOP GSIV / SUB GSIV | **510** / **511** | VERIFIED (DT cells 478/479 = SPI) + the twice-proven +32 rule | `base.dtb` `interrupts` |
| `TMU_REG_CONTROL` / `CURRENT_TEMP1_0` / `THD_TEMP0` / `INTEN0` / `INTPEND0` / `TRIMINFO(0)` / `COUNTER_VALUE0/1` | `0x50` / `0x84` / `0xD0` / `0xF0` / `0xF8` / `0x00` / `0x74`,`0x78` | VERIFIED (mirror) | `exynos_tmu.c:129-153` |
| temp code mask / shift / sensor stride | `0x1ff` / `16` / `0x50` | VERIFIED (mirror) | `exynos_tmu.c:198,153,140` |
| one-point trim: `efuse_value` / `first_point_trim` / `default_temp_offset` | 55 / 25 / 50 | VERIFIED (DT) | `base.dtb` TMU nodes |
| `tmu_default_trip_temp` | **115 °C** | VERIFIED (DT) | `base.dtb` TMU nodes |
| ACPM TMU IPC channel | 9 | VERIFIED | `base.dtb` `/acpm_tmu` `acpm-ipc-channel` |
| ACPM IPC windows | `0x15900000+0x1000`, `0x2039000+0x30000` | VERIFIED (already in D7 notes) | `notes/cpu-driver-9820.md` |
| `COUNTER_VALUE` field value `0x028A` | written by the vendor driver | VERIFIED that it is written; **UNVERIFIED what it does** | `exynos_tmu.c:233,646-652` |
| TMU clock gate id | — | **UNVERIFIED** — closes by: `include/dt-bindings/clock/exynos9820.h` grep for `GATE_TMU`/`GATE_SYSMMU_*TMU*` | — |

## What this means (design conclusions, marked as inference)

- **The cheapest real safety layer is AML, not a driver.** A `ThermalZone` object whose `_TMP`
  reads the TMU MMIO window through an `OperationRegion (SystemMemory, 0x10090000, 0x800)` and does
  the one-point calibration in AML arithmetic (`(code - efuse) + 25`, ×10 for tenths of a degree)
  gives Windows a firmware-reported temperature; `_PSV`/`_HOT`/`_CRT` from the DT trips then give
  passive cooling, hibernate and **OS-forced shutdown** with no Windows driver of ours at all.
  This is inference from the cited contract, and it is exactly what the ACPI spec's basic thermal
  zone example does (`uefi.org/specs/ACPI/6.5/11_Thermal_Management.html` §11.7).
- **The hazard is fighting the ACPM.** The ACPM owns the sensors (reads go to channel 9, and
  `AP_SUSPEND`/`AP_RESUME` prove it runs without the AP). If our UEFI or a Windows driver writes
  `CONTROL`/`THD`/`INTEN`, it may reprogram a block a running coprocessor also owns. Therefore the
  first firmware-half rung is **observation only** — read `CONTROL`, `THD_TEMP0..7`, `INTPEND0..7`,
  `CURRENT_TEMP1_0..` and `COUNTER_VALUE0/1` at DXE entry with **zero writes**, print them, and let
  that decide whether the trip engine is already armed by sboot/ACPM. Same discipline as the D9
  `Beyond0LteModemDxe` slice.
- **Passive cooling has nothing to cool with yet.** All three cooling devices in the DT
  (cpufreq domains, Mali, ISP) are driverless on Windows. So an ACPI thermal zone on its own buys
  *policy* (`_PSV` → park cores via ACPI000C, `_HOT` → hibernate, `_CRT` → shutdown) but not
  fine-grained throttling; the D7 CPU driver is what turns `_PSL`/ACPI000C into real frequency
  control. That ordering should be stated, not assumed.
- **The DT's own protection is weaker than it looks**: no `critical` trip, and two zones are not
  even polled. Do not tell the tester "the vendor trips protect the phone"; the vendor trips
  protect the vendor's Linux stack.

## Dead ends (do not repeat)

- `grep ThermalZone` over `firmware/` — only edk2's AML library, no platform thermal zone anywhere
  (9810 or 9820). Established 2026-10-13; reopens only if a Mu-Silicium or edk2-exynos fork is found
  to publish one.
- `/vendor/firmware` in the shipped dump contains **no** ACPM/TMU/thermal blob (37 entries listed,
  all audio/Wi-Fi/camera/amp/MFC) — so "the ACPM firmware is in /vendor/firmware" is false for this
  build. Closes elsewhere: the ACPM image must come from another partition (U8).
- The decompiled `dumps/hhxj6/base.dts` is unusable for interrupt/reg cells (phandles garbled into
  the property values) — use the raw DTB via `fdt.py`. Re-confirmed this pass.

## Sources

- `dumps/hhxj6/base.dtb`, `dumps/hhxj6/dtbo.img` — raw walk via `tools/dump-tools/fdt.py`; walkers
  in `/tmp/opencode/thermal/`.
- `dumps/hhxj6/vendor/vendor.raw` — `debugfs -R 'ls /firmware'` (names only).
- `github.com/ianmacd/d2s` @ `d1d4fb4aa` (Linux 4.14.113, Exynos 9820, Note 10 family) —
  `drivers/thermal/samsung/exynos_tmu.c`, `exynos_acpm_tmu.c/.h`,
  `include/dt-bindings/thermal/thermal_exynos.h`, `arch/arm64/configs/exynos9820-d2s_defconfig`,
  `arch/arm64/boot/dts/exynos/exynos9820.dts`. GPL-2.0 → reference-only.
- `learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-defined-devices`
- `learn.microsoft.com/en-us/windows-hardware/drivers/kernel/device-level-thermal-management`
- `uefi.org/specs/ACPI/6.5/11_Thermal_Management.html`
- WMI/perf-counter shape: `Win32_PerfRawData_Counters_ThermalZoneInformation` (via wutils.com
  class reference, cross-check pending against learn.microsoft.com).
- Repo prior art: `notes/cpu-driver-9820.md` F6 (corrected above), `notes/dock-desktop.md:175-177`,
  `notes/battery-driver.md:60`, `MILESTONES.md:323`.

## Review of the design doc (harsh-critic pass, 2026-10-13)

- Checked: the TMU windows/offsets in §2 against `Exynos9820.h` (all match); the claim that no
  `ThermalZone` exists in our tables (`grep ThermalZone firmware/ --include=*.asl*` → only edk2's
  AML library); `ACPI000C` really is absent from `Dsdt.asl` today.
- Found — **a unit bug in the design's own AML example.** It returned `_PSV 8300` / `_HOT 1150` /
  `_CRT 1200`, i.e. tenths of **Celsius**. ACPI §11.1 and §11.4.18 require **tenths of Kelvin**
  ("All thermal zone objects must report temperatures in Kelvin when reporting absolute
  temperature values… θ/K = T/(°C) + 273.2"; `_TMP` returns "tenths of degrees Kelvin"). As
  written, Windows would have read a passive threshold of 8.3 K and treated the phone as
  perpetually overheating. Corrected to 3562 / 3882 / 3932, with the conversion spelled out and
  the three-unit problem (milli-°C in the DT, whole °C in the TMU registers, tenths of K in ACPI)
  added to P2.
- Found — `_TC1 = 4`, `_TC2 = 3` were copied from the ACPI specification's example, which the
  spec itself labels "bogus constants". Now marked as placeholders, with the reason the first cut
  skips them (core parking via `ACPI000C` does not use the throttling equation).
- Found — `_TZD` listed `\_SB.GPIO`, which is not a heat source and has no driver. Removed from
  the first cut with the reason.
- Found — the `_CRT` sentence contradicted itself ("above 115 and below 115 plus margin"). Now
  stated plainly: 120 °C, a DECISION, above the vendor `hot` so hibernate precedes shutdown.
- Found — the `_TMP` **sketch read the wrong sensor**. It showed `(CUR0 >> 16) & 0x1FF` and called
  it "sensor 0 of this window", but the zone that sketch labelled (`BIG`) has `sensors = <0x7C0>` =
  sensors **6..10**, which sit at `0x90/0x94/0x98`, not at `0x84`. Corrected, and the whole
  zone→register map is now in the design (derived arithmetic from `exynos_tmu.c:1218-1224` plus the
  T1 masks): BIG `0x90`lo+hi, `0x94`lo+hi, `0x98`lo · MID `0x8C`hi · LITTLE `0x8C`lo ·
  G3D `0x84`hi, `0x88`lo+hi, `0x8C`lo, `0x94`hi · ISP `0x94`lo.
- Found — the sketch **hid a real gap**. Four of five zones have several sensors and the DT says how
  to combine them (`sensing_mode = "balance"` / `"max"`) — but that combining is done by the **ACPM
  firmware**, and no source we have implements it. A pure-AML `_TMP` must pick a policy; the design
  now states the first cut (**max of the zone's sensors**, conservative for a safety threshold) and
  adds **P8** for the unverified `"balance"`.
- Fixed: all four in the design doc (this is a docs-only fix; no code, no constants changed).
- Not affected: the R0–R5 ladder, the read-only rule for the TMU, the ACPM ownership argument,
  the licence posture, P1/P3/P5/P6/P7.
