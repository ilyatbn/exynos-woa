# Thermals, then clocks (2026-10-15)

User: "After bluetooth let's focus on gpu again. I want the desktop look and feel and general ui to offload as much
software rendering to gpu to make it smoother. We also need to ramp up the clock speeds a bit."
Standing rule (memory, user): no GPU/CPU clock increases until Windows reads temperatures and can throttle.
Design source: notes/thermal-research.md (staged plan, constants table).

## Order of work
1. Stage 0 from Windows (no firmware change): S10eDiag peek of TMU_TOP 0x10090000 / TMU_SUB 0x100A0000 CONTROL
   (+0x50: bit0 CORE_EN, bit12 THERM_TRIP_EN), CONTROL1 (+0x54), THD0 (+0xD0, RISE7 = bits[24:16]) - is the TMU
   running, is the 115 C hardware trip armed. (Read-only; an unclocked block could SError -> bugcheck, recoverable.)
2. ACPM ch 9 READ_TEMP prototype as a user-mode tool on S10eDiag peek/poke (port of the UEFI ACPM client), tz 0..4.
3. Driver: S10eThermal (KMDF) polling temps, IOCTL_THERMAL_READ_TEMPERATURE, DSDT ThermalZones, passive cooling via
   GUID_THERMAL_COOLING_INTERFACE -> ACPM ch 0 FREQ_REQ caps (CPU clusters + G3D).
4. Clocks up (only after 3): G3D 260 -> 572 (then 702) MHz, LITTLE/MID toward max, measured with DWM presents/s
   and the cube, temperatures logged.
5. GPU offload of the desktop: what still renders in software (GDI/DirectWrite apps, XAML on WARP?) - inventory.

## Progress log
- 17:10 **Stage 0 answered from Windows** (s10ediag peek over ssh, read-only, no fault):
  - TMU_TOP 0x10090000 and TMU_SUB 0x100A0000 CONTROL (+0x50) = 0x00009503 on both: CORE_EN (bit0) = 1 and
    THERM_TRIP_EN (bit12) = 1 -> the TMU runs and the hardware thermal trip is ARMED on our boot path (someone
    before us - sboot/ACPM - programmed it). CONTROL1 (+0x54) 0x000a0000, +0x58 0x00010016.
  - THD (+0xD0..): 0x017a016d 0x01680163 0x015e0159 0x0154014f (rise), +0xE0 0x0176016c ... (fall).
  - Calibration (two-point, TRIMINFO(p) at p*4: bits[8:0] = 25 C code, bits[17:9] = 85 C code): sensor 0 TOP
    285 / 347 -> RISE7 code 0x17A = 378 -> 25 + (378-285)*60/62 = **115 C** = the DT hot trip. Confirms both the
    register layout and the calibration formula.
  - Current temperatures: +0x84..+0x98 = 11 sensors (low half = sensor 2k, high = 2k+1). TOP codes 0x13a 0x13b 0x139
    0x139 0x13d 0x13d 0x140 0x13e 0x13f 0x13d 0x13d; SUB 0x13c 0x13a 0x139 0x13b 0x139 0x13f 0x13b 0x13b 0x13a 0x139
    0x13b -> ~52-59 C (Windows idle-ish, fixed clocks, BIG cores parked).
  - DT zone masks: TOP BIG 0x7c0 (sensors 6-10), MID 0x20 (5), LITTLE 0x10 (4); SUB G3D 0x21e (1,2,3,4,9), ISP 0x100 (8).
  => No ACPM needed to READ temperatures: plain MMIO + per-sensor trim. (ACPM ch 9 stays the Linux path.)
  Next: s10etemp user tool (zones, max over masks), load test (temperatures must rise), then the driver/ACPI.
- 17:15 tools/phone/s10etemp (ARM64, S10eDiag READ_PHYS, read-only): zone maxima over the DT masks, per-sensor -v.
  Idle: BIG 57-60, MID 54-59, LITTLE 55-57, G3D 54, ISP 53-54 C. **Load test** (ffmpeg software h264 decode,
  all 6 cores, ~40 s): BIG 59.8 -> 63.7, MID up to 60.8, LITTLE up to 58.8, G3D 54 -> 55.9; after the load
  BIG back to 57.4-60.8 within ~15 s. => the MMIO readout tracks real temperature. (Phone clock in the log is
  not local time.)
- 17:20 tools/phone/s10eacpm (ARM64; port of the UEFI ACPM client over S10eDiag READ_PHYS/WRITE32): channel 0 =
  type 1 queue, ap_poll 1, q_len 15, elem 16 (same as UEFI run 30). No-op FREQ_REQs from Windows: G3D (10)
  260000 -> reply 00010000 0003f7a0 (= 260000), LITTLE (2) 1456000 -> 00010000 00163780 (= 1456000) - the same
  echo pattern as the UEFI runs. => **Windows can request clocks through ACPM** (user mode, one process at a time).
  Domain ladders: LITTLE 442k..1950k (13 steps), MID 507k..2400k (2314k/2400k need MIF >= 2093000 - avoid until MIF
  is known), G3D 156k..702k (156/200/260/325/377/433/572/650/702).
