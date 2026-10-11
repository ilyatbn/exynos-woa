# D8 audio — Windows driver design (rung 1: ABOX-direct portcls miniport)

Synthesis task 2026-10-11. This is a DESIGN, not code: nothing here is
validated. Every hardware fact carries a pointer to the verified source
(`notes/d8-audio.md` §1–§3, `notes/d8-audio-linux.md` with file:line refs).
Inference is marked.

## 0. What Windows needs to hear sound

Speaker path (facts from the board DT, `notes/d8-audio.md` §1.2):

```
Windows audio endpoint (kmixer/sysfx — volume lives HERE, free)
  -> our portcls WaveRT miniport (new driver, Exynos9820Audio)
     -> ABOX RDMA channel (DMA, SFRs @0x18C51000+0x100*n)
        -> [ABOX internal router SPUS/SPUM]        <-- THE unknown
           -> ABOX UAIF1 (I2S/DSP-A, @0x18C50510)
              -> CS35L40 x2 (amps, I2C hsi2c_38 @0x40 right/bot, 0x41 left/rcv)
                 -> speakers
```

Capture path:

```
DMIC1/2 -> CS47L93 IN3L/IN4L (digital-mic inputs) -> codec ADC
  -> CS47L93 AIF1 -> ABOX UAIF0 (I2S) -> [router] -> ABOX WDMA -> driver
```

Codec control bus: **SPI** (`&spi_2`, 25 MHz, CS47L93 @cs0, reset gpg1-0,
IRQ gpa2-2 = GSIV 55). Amp control bus: I2C hsi2c_38 (we already have the
HSI2C Windows driver, currently bound to hsi2c_10 only).
(`notes/d8-audio-linux.md` Q3, board dtsi lines 1901/2110-2173.)

## 1. Framework decision (from notes/d8-audio-windows.md Q1)

- **portcls WaveRT miniport**, SimpleAudioSample/SYSVAD skeleton. Rationale:
  portcls.sys is proven in the ARM64 SKU (usbaudio2 is a portcls miniport);
  the sample shows the full non-PCI enumeration + endpoint INF story
  (Class=MEDIA, Include=ks.inf,wdmaudio.inf, AddInterface KSCATEGORY_*).
  ACX is the modern alternative (built for DSP+codec+amp stacks) but has
  less public scaffolding; revisit at rung 2. **(decision, inference-grade)**
- ACPI-enumerated PDO: `Device (AUD0)` `_HID "EXYN9820AUD"` (vendor _HID
  pattern already used: EXYN9820/SAMS9820/SCSY0761/SAMS9821), `_CRS`:
  - Memory32Fixed 0x18C50000+0x10000 (ifr — UAIF/RDMA/WDMA/core SFRs)
  - Memory32Fixed 0x18C10000+0x10000 (audio sysreg)
  - Memory32Fixed 0x18D00000+0x59000 (ABOX SRAM)  [maybe not needed if the
    DSP stays off — decide at implementation]
  - Memory32Fixed 0x18CE0000+0x10000 (ABOX timer)
  - `_DEP` on the HSI2C bus for the amps; codec SPI needs a new SPI bus
    driver first (§4).
- Endpoints in INF: render (speaker) + capture (mic), one WaveRT pin each;
  topology with Volume/KS property handler = kmixer does volume (the user's
  "volume" requirement is satisfied by the class stack once an endpoint
  exists; the physical keys are already done via BTNS/Hidinterrupt).

## 1a. The 3.5 mm jack = the minimal rung-1 target (added 2026-10-11)

Facts in `notes/d8-audio.md` §7. The jack hangs off the CS47L93 itself
(HPOUT2L/R, impedance HPDET + MICDET + moisture on the codec IRQ GSIV 55),
so the jack chain skips the entire amp subsystem:

```
render:  RDMA -> [SPUS/SPUM] -> UAIF0 -> AIF1 -> DAC2 -> HPOUT2 -> jack
capture: jack mic -> IN1BL -> ADC -> AIF1 -> UAIF0 -> WDMA
```

Order of attack inside rung 1 (decision): (1) codec bring-up over the
spi_2 driver (reset gpg1-0, BOOT_DONE poll, FLL 26 MHz->98.304 MHz, AIF1
I2S) — testable with line-out loopback measurements before any ABOX DMA;
(2) jack state -> `KSJACK_DESCRIPTION2.DeviceState` + `IPortEvents`
(connector `eConnTypeMiniPhoneJack`), reading HPDET/MICDET status on the
codec IRQ (GpioInt via Exynos9820Gpio, pin 18 = gpa2-2); (3) the shared
SPUS/SPUM direct-routing experiment (UAIF0 this time, not UAIF1); (4)
WaveRT pins; (5) amps last (they only gate the speaker endpoint, the jack
endpoint works without them — Windows simply shows speakers disconnected).

Moisture detection (`cirrus,moisture-pin/imp`) is codec-native and can be
surfaced later as an extra pin state; not required for audio I/O.

## 2. Bring-up sequence (transliterated from the vendor driver, DSP bypassed)

Order below is FACT-derived from `notes/d8-audio-linux.md` Q2/Q4/Q5 line
refs; the *bypass* parts are INFERENCE and are exactly what must be
proven on the device.

1. Clocks: ABOX CMU is `clock-controller@0x1a240000` (AUD CMU). Gate
   programming on 9820 is ACPM/QCH-owned for some banks (the USB lesson,
   `notes/d1-usb-host.md`) — **audit-then-rely-on-sboot-state** strategy
   first: read UAIF BCLK parents at driver start, do not reprogram unless
   proven. UNVERIFIED.
2. Pad/PMU: ABOX core power NOT needed for a DSP-less path (inference);
   PMU offsets 0x15862d80/2da0/2da4 (core0) recorded in notes if we ever
   need the cores.
3. UAIF1 setup (playback): CTRL1 format = DSP-A + bitclock-inversion,
   slot/width per stream (VERIFIED: abox_if.c:346-417 writes these fields
   from the AP even in the vendor driver); CTRL0 SPK_ENABLE bit (VERIFIED:
   trigger path abox_if.c:419-460 is AP-owned).
4. RDMA channel: BUF_STR/BUF_END/ENABLE — **never written by the vendor AP
   driver** (the DSP does it via IPC PCM_SET_BUFFER). Rung-1 core risk:
   writing them directly + wiring RDMA->UAIF1 through SPUS/SPUM. Mining
   plan: parse `calliope2.dt` (vendor graph blob, on-device) + diff ABOX
   SFR state on Android before/after a playback start (root reads are
   allowed: `/dev/mem`-free approach = the vendor debugfs, or dump via our
   own UEFI diag later). Until then: UNVERIFIED.
5. Codec (capture side): CS47L93 init = power rails (LDO17M 1.8 V
   always-on, LDO18M 1.2 V — check state, don't reprogram PMIC rails;
   PMIC is ACPM-SPEEDY-owned), hard reset gpg1-0 via our GPIO driver,
   soft reset + BOOT_DONE poll (VERIFIED sequence madera-core.c:207-254),
   FLL1 ref 26 MHz -> SYSCLK 98.304 MHz (VERIFIED DT values), AIF1 I2S
   format matching UAIF0.
6. Amps: CS35L40 reset gph1-1 (bot/right) / gph1-2 (rcv/left) via GPIO
   driver, DEVID poll, REVA0 errata patch, wmfw+bin+calib download
   (files live on the Android /vendor partition — load at install time,
   never commit: licensing `notes/d8-audio-windows.md` Q5), DSP1 spk-prot
   enable, I2S/DSP-A receiver config. (VERIFIED sequence cs35l41.c refs
   in linux notes Q5.)

## 3. New dependencies this drags in

- hsi2c_38 facts pre-verified for the amp-bus task (2026-10-11):
  `hsi2c_38 = hsi2c@109A0000 +0x1000`, compatible "samsung,exynos5-hsi2c",
  `default-clk = 200 MHz`, irq `INTREQ__USI17_I2C` = SPI 428 = **GSIV 460**
  (vendor exynos9820.dts:3278-3288 + dt-bindings header). Clocks
  `DOUT_CLK_PERIC1_USI_I2C`/`GATE_USI17_I2C` — the CMU_PERIC1 gate/QCH
  offsets are **UNVERIFIED** (same class as P03); do not write the DSDT
  device until they are pinned from cal-if.
- **Exynos SPI master driver** for spi_2 (USI v2 block, `usi_v2_mode =
  "spi"`) — new small KMDF driver, no prior art in repo. Alternative:
  madera also supports I2C (`madera-i2c.c` exists, disabled in Samsung's
  defconfig) but the S10e wiring is SPI-only (board dtsi) — I2C option is
  electrically unavailable. **(fact)**
- hsi2c_38 instance of the existing HSI2C driver (DSDT device + _HID
  reuse; bus clock gates differ per bus — P03 hazard class).
- The GPIO driver (done, `drivers/Exynos9820Gpio`) provides reset/IRQ pins.

## 4. Honest ladder (mirrors DRIVERS-MILESTONES D8 rows)

| rung | what | cost | status |
|---|---|---|---|
| 0 | USB-C audio dongle under XHC0 (inbox usbaudio2.sys) | zero | works as soon as D1/D3 boot |
| 1 | this design: portcls + ABOX-direct + codec/amp drivers | SPI driver + SPUS/SPUM RE + 2 bus instances | the driver project |
| 2 | Calliope IPC parity (rings + SGI doorbell 0x18CF1F00, proprietary blobs from /vendor at install time) | big + licensing care | parity path, only if rung 1 dead-ends |

Rung 1's go/no-go gate: the SPUS/SPUM direct-routing experiment on the
device (or from calliope2.dt). Until that closes, no audio line is `done`
and nothing is `Ready` (rule 7).

## 5. Progress log

- 2026-10-11: design written from the closed research; no code yet. Next
  implementation-sized tasks when audio is prioritized: (a) mine
  calliope2.dt / SFR diff, (b) spi_2 driver, (c) AUD0 DSDT + portcls
  skeleton with a loopback-only WaveRT pin (no HW) to prove the endpoint
  stack end to end, then (d) real DMA.
