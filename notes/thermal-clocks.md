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
