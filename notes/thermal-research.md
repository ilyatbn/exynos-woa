# Thermal research - read temperatures from Windows + keep the S10e safe under load

Scope: read-only research (2026-10-14). No code, no commits, no phone access (user is in Windows).
Markers: **FACT** = read from a cited source; *(inference)* = my reasoning; VERIFIED / UNVERIFIED on constants.

## Progress log

- 2026-10-14 P0: existing notes. FACT notes/cpu-driver-9820.md F6: thermal zones BIG/MID/LITTLE/G3D,
  power_allocator, BIG trips control 83 C passive, alert0..3 95/100/105/110, hot 115, hyst 5. TMU sensors
  reached via acpm_tmu (ACPM IPC ch 0x9). notes/m12-acpm.md: ACPM IPC protocol (mailbox 0x15900000, SRAM
  0x02039000, initdata 0x7000) already implemented in UEFI (Beyond0LteTouchPowerDxe); channel users:
  cmu 0, devfreq 1, acpm 4, dvfs 5, tmu 9, sci 10.
- 2026-10-14 P1 (kernel source, LineageOS android_kernel_samsung_exynos9820 branch lineage-22.2, raw.githubusercontent;
  local copies %TEMP%\thermal-src\):
  - FACT drivers/thermal/samsung/exynos_acpm_tmu.h: IPC request types TMU_IPC_INIT 0x01, READ_TEMP 0x02,
    AP_SUSPEND 0x04, CP_CALL 0x08, AP_RESUME 0x10. 16-byte message (4 x u32):
    REQ  w0 = ctx u16 | fw_use u16 (fw_use = upper 16 bits = where acpm_ipc puts the seq number);
         w1 byte0 = type, byte1 rsvd, byte2 = tzid, byte3 rsvd2; w2, w3 = 0.
    RESP w1 byte0 type, byte1 ret (s8), byte2 tzid, **byte3 temp (u8, whole deg C)**; w2 byte0 = stat
         (threshold level), byte1/2 used by AP_RESUME print (cold cnt / acpm irq).
    INIT response ret bits: CAP_APM_IRQ 0x1, CAP_APM_DIVIDER 0x2.
  - FACT exynos_acpm_tmu.c: every call = acpm_ipc_send_data(ch, {cmd=msg, response=true, indirection=false});
    channel from DT node "acpm_tmu" property acpm-ipc-channel (stock dt.dtb: **9**). READ_TEMP:
    req.type=2, req.tzid=tz -> *temp = resp.temp, *stat = resp.stat. No conversion on the AP side: ACPM returns
    calibrated degrees C. (inference: ACPM firmware applies the TRIMINFO calibration itself.)
  - FACT exynos_tmu.c (9820 path, CONFIG_EXYNOS_ACPM_THERMAL): exynos98X0_tmu_read() = ONLY the ACPM READ_TEMP
    with tzid = data->tzd->id (exynos_tmu.c:834-857). The AP never reads CURRENT_TEMP over MMIO on 9820.
    BUT probe (exynos_tmu.c:1502-1597) still does direct MMIO setup per TMU instance before the first read:
    reads TRIMINFO(3/4/5) bits[21:18] (BGRI/VREF/VBEI trim, id 0 only), exynos98X0_tmu_initialize() writes the
    rising/falling THD codes for every NON-passive trip, highest trip first (interrupt_count 0 -> bit_off 16 =
    RISE7 slot = the "hot" trip), then exynos98X0_tmu_control(on): CONTROL ref-voltage/slope from TRIMINFO,
    CONTROL1 NUM_PROBE, AVG_CON, COUNTER_VALUE0/1 = 0x028A, TRIM0, INTEN, then tmu_core_enable =
    CONTROL |= THERM_TRIP_EN (bit 12) | CORE_EN (bit 0). After the first device: exynos_acpm_tmu_set_init().
    9820 register offsets (exynos_tmu.c:124-151): TRIMINFO(p) p*4, CONTROL 0x50, CONTROL1 0x54, STATUS 0x80,
    CURRENT_TEMP1_0 0x84, THD_TEMP0 0xD0, INTEN0 0xF0, INTPEND0 0xF8, per-sensor stride 0x50, TRIM0 0x5C,
    EMUL_CON 0xB0, AVG_CON 0x58, COUNTER_VALUE0/1 0x74/0x78. TEMP_SHIFT 16, GPU zone id 3.
- 2026-10-14 P2 (stock dt.dtb, own FDT parser in scratch, %TEMP%\opencode\stock\dt.dtb):
  - FACT TMU nodes: /BIG@10090000 (id 0, sensors 0x7c0, balance, hotplug_in 91 / out 96), /MID@10090000
    (id 1, sensors 0x20, max), /LITTLE@10090000 (id 2, sensors 0x10), all compatible samsung,exynos9820-tmu,
    reg <0x10090000 0x800>, interrupt SPI 0x1de (478); /G3D@100A0000 (id 3, sensors 0x21e, balance),
    /ISP@100A0000 (id 4, sensors 0x100), compatible samsung,exynos9810-tmu, reg <0x100A0000 0x800>, SPI 0x1df (479).
    So two TMU blocks: TMU_TOP 0x10090000, TMU_SUB 0x100A0000 (size 0x800 each). DT-sourced, NOT in Exynos9820.h ->
    UNVERIFIED by the repo rule until added from the DT / live /proc/device-tree read.
    All: tmu_efuse_value 0x37, first_point_trim 25, second_point_trim 85, default_temp_offset 50,
    default_trip_temp 115, cal_type 0, sensor_type 8.
  - FACT /thermal-zones trips (milli-C): BIG and MID: cold 20000, switch-on 55000, control 83000 (passive),
    alert0..3 95000/100000/105000/110000, hot 115000; polling 50 ms passive / 1000 ms.
    LITTLE: alert0..6 20000/76000/81000/86000/91000/96000/101000, hot 115000 (no polling, IRQ only).
    G3D: cold 20000, switch-on 78000, control 88000 (passive), alert0 105000, alert1 110000, alert2/3/hot 115000;
    polling-passive 100 ms. ISP: alert0..6 20000/91000/96000/101000/101000x3, hot 115000.
    Hysteresis 5000 (switch-on 2000). Governor power_allocator (sustainable-power 0 -> taken from ECT).
  - *(inference)* tzid order sent to ACPM = thermal_zone registration id; DT ids 0..4 = BIG, MID, LITTLE, G3D, ISP
    and EXYNOS_GPU_THERMAL_ZONE_ID = 3 agree. UNVERIFIED that the kernel tz->id equals the DT id (other zones
    could register first) -> TWRP read: `for z in /sys/class/thermal/thermal_zone*; do echo $z $(cat $z/type); done`.
- 2026-10-14 P3 (repo state, Windows side):
  - FACT no Windows driver speaks ACPM today (`rg -il acpm drivers/` -> only a comment in S10eBattery/Hsi2c.c).
    ACPM IPC clients exist only in UEFI: Beyond0LteTouchPowerDxe (validated), copies in Beyond0LteGpuProbeDxe/
    GpuAcpm.c (FREQ_REQ ch 0), PcieDxe, UsbHostDxe/Pmic.c. CPU clocks are fixed by UEFI FREQ_REQ on ch 0
    (notes/m15-perf.md runs 31/32: LITTLE idx 2 = 1456000 kHz, MID 1794000) and persist into Windows.
  - FACT drivers/MaliG76Wddm/adapter.c:295 "No power management: nothing under Windows changes the G3D domain or
    its clock (the firmware set 260 MHz)". GPU clock = UEFI FREQ_REQ {id 10 (dvfs_g3d), 260000 kHz} on ACPM ch 0
    (notes/gpu-m16-g3don.md H1-H3). G3D DVFS table (DT, notes/gpu-odin-rom.md F2): 702/650/572/433/377/325/260/200/
    156 MHz. => lowering the GPU clock = the same FREQ_REQ with a lower table rate (e.g. 156000/200000). No G3D
    power-domain change needed.
- 2026-10-14 P4 (battery temperature):
  - FACT drivers/S10eBattery already reads MAX77705 FG TEMPERATURE reg 0x08 (S10eBattery.h:101; Max77705.c:66-82
    milli-C = hi*1000 + lo*39/10) and reports it as BatteryTemperature (Miniclass.c:106-107, 313).
  - FACT **that register is not a measurement on this phone**: stock dtbo battery node battery,thermal_source = <2>
    = SEC_BATTERY_THERMAL_SOURCE_ADC (enum order FG 0, CALLBACK 1, ADC 2, CHG_ADC 3: drivers/battery_v2/include/
    sec_charging_common.h:649-657). max77705_fuelgauge.c:1248-1258: thermal_source != FG -> CONFIG_REG high byte
    bit0 set (inference: CONFIG.TEX = host-supplied temperature), and set_property(POWER_SUPPLY_PROP_TEMP)
    (max77705_fuelgauge.c:2177-2200) -> max77705_fg_write_temp() writes the ADC-derived temp INTO reg 0x08
    (fuelgauge.c:325-341). => Under Windows nobody writes it: S10eBattery reports the LAST value Android/TWRP wrote
    (stale), until the gauge resets. *(inference)* This is why it can look plausible (e.g. ~37 C) while being wrong.
  - FACT the real battery temperature source: AP ADC /adc@15C40000 (samsung,exynos-adc-v3, reg 0x15C40000/0x100,
    SPI 0x54, clock gate_adcif) channel 4 ("adc-temp"; adc-chg-temp ch 3, adc-in-bat ch 0xb, adc-wpc-temp 4,
    adc-usb-temp ch 1); sec_thermistor AP ch 0, WF ch 2, PA ch 7 (dtbo fragment@model). Conversion table
    battery,temp_table_adc / temp_table_data (tenths C): 0x130(304)->90.0 C ... 0x3ff(1023)->50.0 C ...
    0x8af(2223)->20.0 ... 0xc1f(3103)->0.0 ... 0xe4e(3662)->-20.0 (23 points, linear interpolation in sec_battery).
    Samsung's charging limits (dtbo): temp_high_threshold 50.0 C (recovery 48.0), highlimit 70.0 (rec 68.0),
    chg_high_temp 46.0 (rec 44.0), low 0.0 (rec 2.0). ADC base is DT-only -> UNVERIFIED until in Exynos9820.h.
  - FACT exynos_adc.c (same kernel) exynos-adc-v3: CON1 +0x00 (soft reset bit2, non-reset bit1, EN_START bit0),
    CON2 +0x04 (C_TIME(6) <<4, ACH_SEL [3:0]), INT_EN +0x10, INT_ST +0x14 (write 1 clears), DAT +0x08 (12-bit,
    ADC_V3_DAT_FLAG bit31). Conversion = set ACH_SEL, CON1 |= EN_START, wait IRQ/flag, read DAT & 0xFFF.
    *(inference)* a polled Windows read is ~20 lines, BUT the gate_adcif clock is CMU-owned (DT clocks <0x2 0x6e>);
    whether it is running under Windows is UNVERIFIED -> first touch must be read-only (CON2/VER) from UEFI with the
    usual progress-word guard, like other first MMIO touches.
- 2026-10-14 P5 (hardware/firmware protection without an OS):
  - FACT (kernel) the only code that sets TMU THERM_TRIP_EN (CONTROL bit 12) and the 8 rising thresholds is the
    Linux driver at probe (tmu_core_enable, exynos_tmu.c:522-532). The highest non-passive trip of every zone is
    `hot` = 115 C and lands in the RISE7 slot (interrupt_count 0, bit_off 16). *(inference, classic Exynos TMU
    design)* THERMTRIP = hardware signal to the PMU when the RISE7 threshold is crossed with TRIP_EN set -> forced
    power-off/reset, independent of software. => Under Linux the hardware cut-off is 115 C.
  - UNKNOWN whether sboot or the ACPM firmware programs TRIP_EN / thresholds / CORE_EN before an OS runs. Nothing in
    the kernel suggests it (the kernel unconditionally disables and re-programs, exynos98X0_tmu_control:
    tmu_core_disable first). TWRP cannot answer it (its kernel re-programs at probe). Closing read = from UEFI or
    a Windows driver, READ-ONLY: TMU_TOP 0x10090000 + 0x50 (CONTROL: bit0 CORE_EN, bit12 THERM_TRIP_EN),
    +0x54 CONTROL1, +0xD0.. THD (RISE7 = bits[24:16] of THD+0), same on TMU_SUB 0x100A0000.
  - FACT sec_debug_reset_reason.c:35-49: the reset-reason decoder knows PMIC PWROFFSRC bit TSD (PMIC die thermal
    shutdown) and RST_STAT bits (WDT, PINRESET, SWRESET, ...); it lists NO TMU/thermtrip bit. *(inference)* a
    thermtrip power-off would show in debug_history as an unexplained power-on, not as a named reason.
  - FACT ACPM TMU protocol: INIT returns capabilities CAP_APM_IRQ / CAP_APM_DIVIDER; AP_SUSPEND/RESUME report
    "acpm irq" and "cold cnt" (exynos_acpm_tmu.c resume print). On 9810 the AP itself turned ACPM `stat == 2` into a
    cpufreq cap (exynos_tmu.c:842-854) - i.e. ACPM REPORTS, the AP THROTTLES. *(inference)* ACPM's own job is
    sensing + cold-temperature handling while the AP sleeps; no source shows ACPM throttling CPU/GPU by itself.
    The GPU side is the same: gpex_thermal.c only applies a max-clock lock requested by the AP thermal framework.
  - FACT S2MPS19 PMIC has a die thermal shutdown (TSD) - protects the PMIC, not the SoC; threshold not in source.
  - Risk conclusion *(inference)*: running unmanaged, the ONLY guaranteed protections are PMIC TSD and the
    battery/charger's own limits. A SoC hardware trip at 115 C exists only if someone armed it - UNVERIFIED for
    our boot path. Silicon damage is unlikely at our fixed mid clocks (LITTLE 1456 of 1950, MID 1794 of 2314, BIG
    cores off, GPU 260 of 702 MHz), but skin/battery temperature is the real hazard: Samsung stops charging at
    battery 50 C (dtbo temp_high_threshold) and nothing under Windows enforces that.
- 2026-10-14 P6 (our ACPI today): FACT firmware/Platform/Beyond0LtePkg/AcpiTables = Dsdt.asl, Fadt, Gtdt, Madt,
  Pptt, Dbg2. `rg -i "ThermalZone|_TMP|_PSS|_CPC|PEP|_PSV|_CRT|_LPI|_PPC"` over AcpiTables -> no hits. Dsdt has
  CPU0-7 (ACPI0007), UFS0, I2C0, TSP0, XHC0, UFN0, WLAN, BAT0, AUD0, GPU0. No PEP, no _CPC, no _PSS, no PAGR
  (ACPI000C), no thermal zone. => Windows today has no perf-state control of the CPUs at all (the measured
  fixed 1456/1794 MHz is just what UEFI's FREQ_REQ left).
- 2026-10-14 P7 (Windows thermal model, learn.microsoft.com):
  - Source: https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/design-guide
    ("Thermal management in Windows"). FACTS quoted from it:
    * Thermal zones must be in ACPI. Zone objects: _TZD (devices in zone) / _PSL (processors, optional);
      _PSV (start throttling), _HOT (hibernate; without hibernate -> critical shutdown), _CRT (critical shutdown,
      "At least one thermal zone on a system must have _CRT"); _TC1/_TC2 gains; _TSP sample interval in tenths of
      a second (>0); _TZP polling; _TMP; Notify 0x80 (threshold crossed), 0x81 (thresholds changed); _DTI optional.
    * Temperatures in tenths of a degree Kelvin. Algorithm DP[%] = _TC1*(Tn - Tn-1) + _TC2*(Tn - Tt);
      Pn = max(Pn-1 - DP, MTL). Microsoft _DSM UUID 14d399cd-7a27-4b18-8fb4-7cb7b9f4e500: fn 1 minimum throttle
      limit (%), fn 2 = reference to the temperature sensor device (+ _DEP on it), fn 3 overthrottle threshold.
    * Two ways to report temperature: (1) a temperature sensor driver implementing IOCTL_THERMAL_READ_TEMPERATURE
      (CTL_CODE(FILE_DEVICE_BATTERY, 0x24, METHOD_BUFFERED, FILE_READ_ACCESS), input THERMAL_WAIT_READ
      {Timeout ms (0 = now, -1 = forever), LowTemperature, HighTemperature}, output ULONG tenths of K; ACPI keeps
      ONE outstanding wait-IOCTL and completes on a threshold crossing; must handle cancel); (2) AML _TMP +
      Notify 0x80, "limited to resources accessible through ACPI operation regions".
    * Cooling devices: a device listed in the zone (_TZD) whose driver answers a query for
      GUID_THERMAL_COOLING_INTERFACE (THERMAL_COOLING_INTERFACE v1: ActiveCooling(BOOLEAN), PassiveCooling(ULONG
      Percentage)) is registered as a cooling device; ACPI calls PassiveCooling with the zone's percentage and the
      device maps it to its own action. Multiple zones -> the minimum percentage wins.
    * Processors: throttling goes to the processor power manager (PPM) - built-in, but only effective if the PPM
      has a perf-control path (on ARM64: a PEP handling PEP_NOTIFY_PPM_PERF_SET -
      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/pepfx/ns-pepfx-_pep_ppm_perf_set ,
      https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/ppm-notifications ; or CPPC _CPC). We have
      neither. Processor aggregator ACPI000C in a zone -> core parking as thermal mitigation (works without perf
      states, but "work affinitized to a parked core will cause the core to run").
  - *(inference)* Simplest CPU mitigation that works with what we have: a project-local thermal device in _TZD
    whose driver implements GUID_THERMAL_COOLING_INTERFACE and maps Percentage -> ACPM FREQ_REQ caps on the
    LITTLE/MID (and G3D) domains itself, bypassing PPM. A PEP is the "right" long-term answer but is a large project.
  - *(inference)* AML-only _TMP is not practical: the temperature needs the ACPM queue protocol (seq numbers,
    queue front/rear in SRAM, mailbox IRQ bits); doing that from AML would race any driver on the same mailbox.
- 2026-10-14 P8 (debug hooks in the kernel for verification): exynos_tmu.c:1829-1845 debugfs dir
  `exynos-thermal` with `log_print` (enables the "[acpm_tmu] data 0:.. 1:.. 2:.. 3:.." dmesg line per READ_TEMP =
  raw reply words), `ipc_dump1/2`, `emul_call`. SEC_PM sysfs attr `curr_temp` prints "BIG,MID,LITTLE,G3D,ISP"
  in DT-id order (exynos_tmu.c:1851-1880).

## Conclusions (Q1-Q5)

1. **TMU read path.** FACT: on 9820 Linux reads temperature ONLY through ACPM IPC channel 9 (READ_TEMP, type 2,
   tzid 0..4 = BIG/MID/LITTLE/G3D/ISP, reply byte w1[31:24] = whole deg C, already calibrated). The AP never reads
   CURRENT_TEMP; it does do a one-time MMIO setup of both TMU blocks (0x10090000 TOP, 0x100A0000 SUB) from their
   TRIMINFO registers + thresholds + TRIP_EN/CORE_EN, then TMU_IPC_INIT. Simplest Windows path: port the
   device-validated UEFI ACPM client (Beyond0LteTouchPowerDxe / GpuAcpm.c) into a KMDF driver and send READ_TEMP on
   channel 9. Open risk: whether the TMU cores are enabled when no Linux ran (sboot/ACPM state UNVERIFIED) -> the
   first step must read CONTROL (bit0/bit12) and try READ_TEMP before deciding to replicate the kernel MMIO init.
2. **Battery.** S10eBattery already reports MAX77705 FG reg 0x08, but on this phone that register is host-written
   (thermal_source = ADC): the value is stale under Windows. Real source = AP ADC 0x15C40000 ch 4 + the dtbo table.
3. **Protection without an OS.** Linux arms the TMU hardware trip at 115 C; nobody is known to arm it on our
   boot path; ACPM only reports (no evidence of autonomous throttling); PMIC TSD protects the PMIC only. Treat the
   phone as UNPROTECTED under Windows until the CONTROL read shows TRIP_EN set.
4. **Windows model.** ThermalZone + _TMP via a sensor driver (IOCTL_THERMAL_READ_TEMPERATURE, _DSM fn 2 + _DEP),
   _PSV/_CRT/_TC1/_TC2/_TSP; cooling via GUID_THERMAL_COOLING_INTERFACE on our own device; processor throttling via
   PPM needs a PEP/_CPC we do not have.
5. **GPU.** Clock = ACPM ch 0 FREQ_REQ {10, kHz}; table 702..156 MHz; KMD has no clock code. Lowering = same call
   with 200000/156000 (both DT table rows). *(inference)* safe while the GPU runs (Samsung's DVFS changes it live
   through the same cal_dfs_set_rate), but should be serialized with any other ch 0 user in ONE driver.

## Recommended staged plan

Stage 0 (UEFI, read-only, one boot): in an existing DXE (FV is 99% full - prefer extending GpuProbe/TouchPower
over a new DXE) log: TMU_TOP/SUB +0x50 CONTROL, +0x54 CONTROL1, +0xD0 THD0 (RISE7 code), TRIMINFO(0..5); ACPM ch 9
queue params (q_len, elem size, ap_poll) and READ_TEMP for tz 0..4. Decides: is the TMU running and is the 115 C
hardware trip armed. New Exynos9820.h entries (TMU_TOP/SUB, ADC) first, sourced from DT + a TWRP /proc/iomem read.

Stage 1 (Windows, read-only logging): new KMDF `S10eThermal` (ACPI device SAMS9829-style project-local _HID,
resources: ACPM mailbox page + SRAM window). Polls READ_TEMP tz 0..4 every 1 s, publishes to registry/ETW and a
user tool (gfxperf can print them). Only ch 9 traffic. If Stage 0 shows the TMU off, add the kernel's MMIO init
(exynos98X0_tmu_control + initialize with the DT trips) as a separate, guarded task - it is what arms TRIP_EN.

Stage 2 (ACPI reporting): same driver implements IOCTL_THERMAL_READ_TEMPERATURE; DSDT gets ThermalZones TZBG
(BIG/MID max), TZLT (LITTLE), TZGP (G3D), each _DSM fn 2 -> \_SB.TMU0, _DEP, _CRT = 105 C (below the 115 C
hardware trip; mirrors BIG alert2), _HOT unset (no hibernate), _TSP 10 (1 s). Battery zone from the ADC later.
Validation: Windows "Thermal Zone Information" perf counters show the same values as TWRP at idle.

Stage 3 (passive cooling): add the S10eThermal device to each zone's _TZD; implement GUID_THERMAL_COOLING_INTERFACE;
map Percentage -> ACPM ch 0 FREQ_REQ caps: LITTLE/MID down the DT ladders, G3D 260 -> 200 -> 156 MHz. _PSV = 83 C
CPU (DT control-temp) / 88 C GPU; _TC1 2, _TC2 3 to start (MS example); minimum throttle _DSM fn 1 ~ 30 %.
Moves GPU clock control out of "UEFI only" - the KMD stays clock-free. Optional cheap extra: PAGR ACPI000C in the CPU
zone for core parking. Battery: stop/limit charging at 46-50 C via the charger driver (Samsung's dtbo limits).

## Constants table

| Constant | Value | Status / source |
|---|---|---|
| ACPM mailbox / SRAM / initdata | 0x15900000 / 0x02039000 / 0x7000 | VERIFIED (Exynos9820.h:343-347) |
| ACPM TMU channel | 9 | DT stock dt.dtb /acpm_tmu; UNVERIFIED live -> TWRP `xxd /proc/device-tree/acpm_tmu/acpm-ipc-channel` |
| ACPM DVFS request channel | 0 | VERIFIED (m15 runs 31/32, live DT) |
| READ_TEMP type / reply layout | 0x02; temp = w1[31:24] deg C | kernel source FACT; on-device UNVERIFIED -> TWRP log_print dmesg |
| tzid map | 0 BIG,1 MID,2 LITTLE,3 G3D,4 ISP | DT ids FACT; tz->id equality UNVERIFIED -> TWRP thermal_zone*/type |
| TMU_TOP / TMU_SUB | 0x10090000 / 0x100A0000, 0x800 | stock DT; UNVERIFIED (not in Exynos9820.h) -> TWRP /proc/iomem, /proc/device-tree/BIG@10090000/reg |
| TMU CONTROL/THD offsets | 0x50 / 0xD0, TRIP_EN bit12, CORE_EN bit0 | kernel source; UNVERIFIED on hw |
| TMU IRQ | SPI 478 (TOP), 479 (SUB) | stock DT; UNVERIFIED |
| Trips | CPU control 83, GPU control 88, alerts 95-110, hot 115 C | stock DT FACT |
| G3D FREQ_REQ | id 10, kHz, table 702000..156000 | notes/gpu-m16-g3don.md; 260000 in use (device-validated) |
| ADC | 0x15C40000/0x100, ch 4 battery, SPI 0x54 | stock DT/dtbo; UNVERIFIED |
| Battery temp table | 23 pts, 304->90.0 C .. 3662->-20.0 C | dtbo FACT |
| FG reg 0x08 | host-written on this phone | kernel source FACT (thermal_source 2) |

## TWRP reads that close the open items (all read-only)
```
for z in /sys/class/thermal/thermal_zone*; do echo $z $(cat $z/type) $(cat $z/temp); done
xxd /proc/device-tree/acpm_tmu/acpm-ipc-channel
xxd /proc/device-tree/BIG@10090000/reg; xxd /proc/device-tree/G3D@100A0000/reg; xxd /proc/device-tree/adc@15C40000/reg
grep -i -E "10090000|100a0000|15c40000" /proc/iomem
cat /sys/class/power_supply/battery/temp; cat /sys/class/power_supply/max77705-fuelgauge/temp 2>/dev/null
dmesg | grep -i -E "acpm_tmu|exynos-tmu|tmu" | head -50
echo 1 > /sys/kernel/debug/exynos-thermal/log_print; sleep 2; dmesg | grep "acpm_tmu\] data" | tail; echo 0 > /sys/kernel/debug/exynos-thermal/log_print
```
(log_print only toggles a printk; it is the one write in the list.) The TRIP_EN question cannot be answered from
TWRP (Linux re-programs the TMU) - only Stage 0.
- Review (main session): 'FV 99% full' checked against the 10-14 build log - FVMAIN_COMPACT 99% (0x680 B free), FVMAIN itself 33%; the compressed FV is the limit.
