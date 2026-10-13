# TH01 — live reads: what temperatures the running device reports, and under what names

Status: open — **needs-phone** (`adb devices` empty on this Linux box, 2026-10-13; adb is
`/usr/bin/adb`, on PATH). The S10e is `RF8M21HYDSZ` / `SM_G970F`, the only device. A **TWRP root
shell** turns several name-only reads into value reads.

## Goal

Establish from the running device how many thermal sensors it exposes, what each is called, what
temperatures they report at idle and under load, and which policy files the build carries — closing
U2 (sensor inventory) and U8 (policy data) in `notes/thermal.md`, and giving the first real
numbers for the safety question U1.

## Done when

`/sys/class/thermal/thermal_zone*/type` and `…/temp` are captured for **every** zone (TWRP root),
**and** `/proc/device-tree/thermal-zones/` names are listed. All output pasted into
`notes/thermal.md` under a dated heading.

## Do this

Unprivileged shell (names and one level only — property contents and deeper `/sys` are
SELinux-blocked, `notes/s10e-live-reads.md` §9):

```
adb shell 'ls /proc/device-tree/thermal-zones/'
adb shell 'for z in /proc/device-tree/thermal-zones/*/; do echo "== $z"; ls -l "$z"; done'
adb shell 'ls /proc/device-tree/ | grep -iE "thermal|tmu|thermistor|acpm"'
adb shell 'ls /sys/class/thermal/ 2>/dev/null'
adb shell 'ls /sys/class/hwmon/ 2>/dev/null'
adb shell 'cat /proc/interrupts | grep -iE "tmu|thermal|acpm|monitor"'
adb shell 'ls /vendor/etc/ | grep -iE "thermal"'
adb shell 'ls /vendor/firmware/ | grep -iE "thermal|tmu"'      # names only, never commit a blob
```

TWRP root shell (values, not just names — enter by Vol Up + Bixby + Power so the reset history
stays clean):

```
adb shell 'for t in /sys/class/thermal/thermal_zone*; do echo "$t $(cat $t/type) $(cat $t/temp)"; done'
adb shell 'for t in /sys/class/thermal/thermal_zone*; do echo "== $t"; cat $t/trip_point_*_type $t/trip_point_*_temp 2>/dev/null; done'
adb shell 'cat /sys/class/power_supply/battery/temp 2>/dev/null'
adb shell 'cat /sys/class/power_supply/battery/status 2>/dev/null'
adb shell 'ls -l /sys/class/thermal/cooling_device*/type 2>/dev/null'
```

Take the idle reading **and** a loaded one (run something heavy in Android for a minute, then
re-read). The delta is the evidence that the sensors actually move — a zone that never changes is
a dead or virtual sensor, and that distinction decides what Windows can trust.

## What it proves / if it fails

| Output | Meaning | Next step |
|---|---|---|
| four+ `thermal_zone*` entries with distinct `type` names and rising `temp` under load | **U2 closes**: the sensor inventory is real and live; the numbers become the baseline for the trip design | compare each `type` against the DT zone names in `notes/cpu-driver-9820.md` F6 |
| a zone whose `temp` never moves under load | virtual/dead sensor, or it is fed by ACPM and ACPM is not reporting | check `acpm_tmu` (ch 0x9) liveness in the vendor source before trusting it |
| `/sys/class/thermal` empty in TWRP | TWRP's kernel has no thermal driver — a **TWRP-boot** value, not a stock-Android one | re-read from booted Android, and say which boot each number came from |
| `battery/temp` readable | opens U7 with a real number (the cell is the part that can actually be damaged) | pair with the charger's current limit in `notes/battery-driver.md` |
| nothing at all | the reads are blocked or the device is not attached | do not infer absence — re-check `adb devices` first |

## Known

- The DT zones and their trip points are already read: `BIG`/`MID`/`LITTLE`/`G3D`, passive
  control-temp **83 000** milli-°C, hot **115 000**, governor `power_allocator`, polling 50 ms —
  `notes/cpu-driver-9820.md` F6. This task is about the *runtime* view, not the DT again.
- TMU reads go over ACPM IPC, not MMIO — same source. A Windows driver cannot simply poke a TMU
  register; that is what U4/U5 have to resolve.
- `sec_thermistor@0/2/7` exist as live platform nodes — `notes/dock-desktop.md:175-177`.

## Next actions

1. Paste the raw output into `notes/thermal.md` under a dated heading, verbatim, labelled with the
   boot state (stock Android vs TWRP).
2. Map each `thermal_zoneN` to its DT zone and cooling device.
3. Queue the constants task once the sensor list is fixed.

## Rollback / Do NOT

Read-only: `ls`, `cat` only. No writes to `/sys/**`, no forcing a trip, **no deliberate
over-temperature test** — the point of this subsystem is to avoid ever needing one. No partition
writes, nothing under `/dev/block/**` touched. Safe on the only device.
