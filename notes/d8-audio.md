# D8 — Audio (speakers, mic, volume) + hardware buttons on the S10e

Running notes for the audio/button work stream. Facts carry a source; my
inference is marked **(inference)**. Started 2026-10-11.

Primary source for everything in §1: the shipped Android 12 (HHXJ6) base DTB +
SKU overlay, already extracted in this repo at `dumps/hhxj6/base.dts` and
`dumps/hhxj6/ov00.dts` (beyond0 = S10e EUR overlay). No web needed for §1.

## 1. What the shipped device tree says (FACTS from the dump)

### 1.1 Audio subsystem = "ABOX" (Samsung audio DSP block)

`base.dts:5320-5343` — `abox@18c50000`, `compatible = "samsung,abox"`:

| reg window | reg-name (decoded from the byte-hex `reg-names`) | meaning |
|---|---|---|
| `0x18C50000 +0x10000` | `ifr` | ABOX core SFRs (UAIF/DMA/core regs live here) |
| `0x18C10000 +0x10000` | `sysreg` | audio SYSREG |
| `0x18D00000 +0x59000` | `sram` | ABOX internal SRAM |
| `0x18CE0000 +0x10000` | `timer` | ABOX timer block |

IPC contract with the DSP (`base.dts:5329-5335`):
- `samsung,ipc-tx-offset = 0x2f000`, `ipc-rx-offset = 0x2f300`
- `ipc-tx-ack-offset = 0x2f2fc`, `ipc-rx-ack-offset = 0x2f5fc`
- `ipc-tx-area = <0x1 0x850000 0x4000>`, `ipc-rx-area = <0x1 0x854000 0x4000>`
  (area 1 = the DRAM0 region; area 0 = SRAM — see the `abox-firmware` areas)
- `handshake-area = <0x0 0x2f2e0 0x20>` (in SRAM)
- `samsung,abox-gic = <&abox-gic@18cf0000>` — a dedicated GIC for the DSP
- `iommus = <&sysmmu@18E00000>` — ABOX DMA goes through its own SYSMMU

DSP cores (`base.dts:5345-5396`): TWO cores, `samsung,type = "CA32"`
(Cortex-A32!), firmware blobs named **`calliope_sram.bin` / `calliope_dram.bin`**
(core 0) and `calliope_sram_2.bin` / `calliope_dram_2.bin` (core 1). Core SFR
`abox-core@18c55000` (+0x80 each), PMU power/enable/standby offsets
`0x2d80/0x2da0/0x2da4` and `0x2e00/0x2e20/0x2e24` (relative to some PMU base —
which base is UNVERIFIED, likely audio PMU under `18C10000` sysreg or the
ASPM; must be pinned from the kernel driver).

DMA + I2S plumbing (`base.dts:5397-5695`): 12× `abox-rdma@18c51x00` (0x100
each), 8× `abox-wdma@18c53x00`, 4× `abox-uaif@18c505{0,1,2,3}0` (Universal
Audio Interface = I2S/PCM), 1× `abox-dsif@18c50550` (PDM/DMIC interface),
`abox-effect@18d2e000`, `abox-vss@0` (voice service?), `abox-debug@0`.
`samsung,uaif-max-div = 0x200`.

Clocks of the abox node reference `clock-controller@0x1a240000` (a CMU) with
clock-names decoded from the hex words: `pll`, `pll1`, `aud`, `ifd`, `dmic`,
`bus`, `cnt` (base.dts:5338 — the words are `0x706c6c00`= "pll\0",
`0x706c6c31`="pll1", `0x617564`= "aud", `0x69660064`="ifd"+..., `0x6d696300`="mic"?,
`0x62757300`="bus", `0x636e7400`="cnt" — byte order in the dump is odd; treat
the exact name list as UNVERIFIED until read from the vendor DT source).

### 1.2 Sound card = ASoC machine driver `samsung,beyond-madera`

`ov00.dts:1472+` — `sound { compatible = "samsung,beyond-madera"; ... }`:
- codecs on the AP side are **`snd-soc-dummy`** (`audio_codec_dummy`,
  ov00.dts:1460) — the AP kernel does NOT drive a codec chip directly;
  the real codec/amp control is inside the ABOX/Calliope firmware. **(inference
  from the dummy codec + `samsung,prefix = "ABOX"...`)**
- `samsung,codec` list ends with **`&cs35l40@41` and `&cs35l40@40`** —
  two **Cirrus Logic CS35L40 smart amplifiers** on I2C (addr 0x40/0x41),
  labelled "Left AMP SPK"/"Right AMP SPK" in `samsung,routing` (decoded ASCII:
  the routing blob contains "MIC1..MIC4", "HEADSET MIC", "ABOX UAIF3 Capture",
  "BLUEBOTH MIC", "FM IN", "Left AMP SPK", "SPEAKER", "Right AMP SPK",
  "HEADPHONE", "ABOX UAIF3 Playback", "VTS PAD DPD", "DMIC1",
  "VTS Virtual Output", "VTS Virtual Output Mux").
- `cirrus,sysclk/asyncclk/dspclk/outclk/fll1-refclk/fll2-refclk/ampclk`
  properties — Cirrus codec clock config (FLL refs at 25.000000 MHz =
  0x18CBA80, 98.304 MHz = 0x5DC0000, 147.456 MHz = 0x8CA0000).
  The "madera" name = Cirrus **CS47L9x** family (mainline `madera` driver);
  the actual codec chip on the S10e board is NOT a DT node — part number
  UNVERIFIED (teardowns say Cirrus Logic; confirm from vendor modules /
  `/proc/asound` on the phone).
- DAI links: rdma0..rdmaN/wdmaN each with `cpu { sound-dai = <0xffffffff> }`
  (ABOX phandles) and codec `audio_codec_dummy` — 25 links, prefixes
  "ABOX"×26 + "Left" "Right" "VTS".

### 1.3 Microphones

- `dmic-*` and `amic-pdm` pinctrl states exist (`base.dts:2421-2485`,
  pinctrl@15580000 group): `dmic-bus-clk`, `dmic-3rd-bus-clk`, `dmic-pdm`,
  `amic-pdm` — DMIC clock/data pins + analog-mic PDM pins.
- The capture path in routing: `MIC1..MIC4 -> ABOX UAIF3 Capture` (DMICs
  feed ABOX directly via DSIF/UAIF). **(fact from routing blob)**

### 1.4 Hardware buttons — plain GPIOs, `gpio-keys`

`ov00.dts:1298-1322` — a `gpio-keys` node with four buttons:
- `gpio-keys: KEY_WINK` (Bixby/wink key)
- `gpio-keys: KEY_VOLUMEDOWN`
- `gpio-keys: KEY_VOLUMEUP`
- `gpio-keys: KEY_POWER`
pinctrl states `key-wink/key-voldown/key-volup/key-power` at
`ov00.dts:3245+` (fragment@29). GPIO bank + pin numbers: read next (§2).

So on the S10e **all four buttons are AP-visible GPIO interrupts** — no PMIC
key ADC involved for these. (The `attn-input` and `cable_number` references
belong to Samsung's "cable detect"/MUIC subsystem, not the keys.)

### 1.5 Button GPIOs + EINT interrupt numbers (FACTS)

`ov00.dts:3245-3280` (fragment@29) — pinctrl states for the keys:

| key | pin | function | notes |
|---|---|---|---|
| KEY_VOLUMEUP (115) | **gpa0-3** | 0xF (EINT) | debounce 10 ms |
| KEY_VOLUMEDOWN (114) | **gpa0-4** | 0xF (EINT) | debounce 10 ms |
| KEY_WINK / Bixby (703) | **gpa0-6** | 0xF (EINT) | wakeup, debounce 10 ms |
| KEY_POWER (116) | **gpa2-4** | 0xF (EINT) | wakeup, debounce 10 ms |

(The `linux,code` cells render as phandles in our decompile — `jig-gpio`
= phandle 0x72 = 114 = KEY_VOLUMEDOWN, 0x73 = 115 UP, 0x74 = 116 POWER;
0x2bf = 703 = KEY_WINK. Cross-checked against `input-event-codes.h`.)

GPA banks live in `pinctrl@15850000` (`compatible =
"samsung,exynos9820-pinctrl"`, reg `0x15850000 +0x1000`) — already
`EXYNOS_ALIVE_GPIO_BASE` VERIFIED in `Exynos9820.h:477`. Each bank is also an
`interrupt-controller` (parent = main GIC `10100000`).

**EINT SPI numbers (FACT, decoded from the mangled `interrupts` cells of the
bank nodes in `base.dts:13-60` — the decompiler rendered the raw SPI ints as
phandle comments, which preserves the values):**
- gpa0 = SPI **5..12** (one per pin 0..7) → GSIV = SPI+32 = **37..44**
- gpa1 = SPI **13..20** → GSIV 45..52 (matches the DSDT's derived touch line:
  `gpa1-0` = SPI 13 = GSIV 45, `Dsdt.asl:291`)
- gpa2 = SPI **21..28** → GSIV 53..60
- gpa3 = SPI 29..36, gpa4 = SPI 37..38 (only two entries)

So the four keys are: VOLUP = SPI 8 / **GSIV 40**, VOLDN = SPI 9 / **GSIV 41**,
WINK = SPI 11 / **GSIV 43**, POWER = SPI 25 / **GSIV 57**.
**(DERIVED — same arithmetic the DSDT already uses for the touch EINT;
mark UNVERIFIED until confirmed against the vendor `exynos9820.dtsi`
interrupt lists.)**

### 1.6 Audio firmware present in vendor (FACTS)

`debugfs ls /firmware` on `dumps/hhxj6/vendor/vendor.raw` (all present):
- **`calliope_sram.bin` / `calliope_dram.bin` / `calliope_sram_2.bin` /
  `calliope_dram_2.bin` / `calliope2.dt`** — the ABOX DSP firmware named by
  the DT `abox-firmware-*` nodes.
- **`cs35l40-bot-dsp1-spk-prot{,-calib}.bin/.wmfw`** and
  **`cs35l40-rcv-dsp1-spk-prot{,-calib}.bin/.wmfw`** — Cirrus CS35L40
  SmartAmp firmware: **bot** = bottom speaker, **rcv** = receiver ("earpiece").
  Two amps → stereo speaker path (S10e has main speaker + earpiece stereo pair).
- **`cs47l93-dsp1-trace.wmfw`** — names the codec: **Cirrus CS47L93**
  (Madera family). This closes the "codec part number UNVERIFIED" question:
  the S10e codec is CS47L93. (FACT from the filename; the chip itself is not
  a DT node because the ABOX/Calliope firmware owns its control — §1.2.)
- `AP_AUDIO_SLSI.bin`, `APBargeIn/APBiBF/APDV/APSV/APTV_AUDIO_SLSI.bin` —
  Samsung **SLSI-ADSP** framework images (the SLSI audio framework is
  Samsung's ADSP loader/middleware; these are the DSP-side audio graphs).
- `bidirmic.bin`, `vts.bin`, `usbin.bin`, `usbout.bin`, `dnn.bin`, `epx.bin`,
  `rxse/txse1-3.bin`, `SoundBoosterParam.bin`, `dax_param.bin` — audio
  processing models (voice effects, noise suppression, sound booster).

### 1.7 Kernel evidence (FACT)

`strings` on `dumps/hhxj6/kernel` (the shipped Image): the **abox driver is
built in** — `abox_core_download_firmware`, `abox_core_power/enable/standby`,
`abox_cmpnt_register_if/rdma/wdma`, `abox_component_control_get/put_ipc`,
`abox iommu mapping ...` strings. `arizona-ldo1`/`arizona-micsupp` regulator
strings present (Cirrus family glue). No `madera`/`cs35l40` strings in the
kernel Image — those are loadable modules (no .ko in vendor.raw; they live in
the vendor_boot/dtbo ramdisk path we have not unpacked). Not needed: the
driver sources are on GitHub (see §3 research).

## 2. Consequences for the Windows port (inference, to be validated by research)

- **Hardware buttons are the tractable win**: 4 plain EINT GPIOs on the
  already-VERIFIED ALIVE GPIO window `0x15850000`. Windows design: one ACPI
  device + one small KMDF driver that claims the 4 EINTs and injects HID
  reports via **VHF** (Virtual Hardware Framework, inbox `vhid.sys`):
  Consumer page Volume Increment/Decrement (0xE9/0xEA) → inbox HID stack does
  the volume; System Power Down (0x81) → Windows power-button semantics.
  Bixby → AC Search (0x0223) or unmapped.
- **Audio is a driver project with a ladder** (like D5b for GPU):
  rung 0 (buy-it-now): USB audio dongle behind D1 — inbox `usbaudio2.sys`
  on ARM64 **(to verify)**;
  rung 1: Windows portcls miniport driving ABOX UAIF+DMA as plain I2S
  (bypass Calliope graph, control CS47L93 + CS35L40 directly over I2C —
  Cirrus Linux `madera`/`cs35l40` drivers are the register reference);
  rung 2: full ABOX/Calliope IPC parity (Samsung-proprietary firmware).
- Mic path: DMICs → ABOX DSIF/UAIF3 Capture (routing blob §1.2); codec ADC
  inside CS47L93 also feeds "HEADSET MIC"/"HEADSET MIC" routes.

## 3. Verified ALIVE GPIO / EINT register map (FACT — vendor kernel, 2026-10-11)

Source: `drivers/pinctrl/samsung/pinctrl-exynos-arm64.c` + `pinctrl-exynos.c` +
`pinctrl-exynos.h` @ LineageOS/android_kernel_samsung_exynos9820 lineage-22.2
(fetched to /tmp/opencode), plus `include/dt-bindings/interrupt-controller/exynos9820.h`.

Bank type `exynos9810_bank_type_alive`: fld_width {4,1,4,4}, reg_offset
{0x00,0x04,0x08,0x0C} → per bank: **CON +0x00 (4 bits/pin), DAT +0x04,
PUD +0x08 (4 bits/pin), PDN +0x0C**.

9820 ALIVE bank offsets (`exynos9820_pin_banks0`, base 0x15850000):
gpa0=**0x000**, gpa1=**0x020**, gpa2=**0x040**, gpa3=**0x060**, gpa4=**0x080**
(8,8,8,8,2 pins). NOTE the 9810 table (`exynos9810_pin_banks0`, same file):
etc1=0x000, gpa0=0x020, gpa1=0x040, gpa2=0x060 — **the 9820 dropped etc1 and
shifted every bank down by 0x20**.

Wakeup EINT regs (`samsung,exynos7-wakeup-eint` child in the vendor SoC dts,
chip struct `exynos7_wkup_irq_chip`): per-pin 4-bit **EINT_CON** word at
**0x700 + eint_offset**, eint_offset gpa0=0x00, gpa1=0x04, gpa2=0x08,
gpa3=0x0C, gpa4=0x10; trigger encoding LOW=0 HIGH=1 FALLING=2 RISING=3
**BOTH=4**. **FLTCON** at **0x800 + fltcon_offset** (gpa0=0x00, gpa1=0x08,
gpa2=0x10, gpa3=0x18, gpa4=0x20), 8 bits/pin, FLTCON0 covers pins 0-3 and
FLTCON1 = +0x4 covers pins 4-7; EN=(1<<7) SEL=(1<<6) WIDTH=0x3F; the vendor
sets gpa banks to EN|SEL = 0xC0 width 0 (same value the 9810 touch driver
writes). **MASK** at **0x900 + eint_offset** (1 bit/pin, 1=masked),
**PENDING** at **0xA00 + eint_offset** (1 bit/pin, write-1-clears).
These match the 9810 touch driver's EINT8 constants (0x704/0x808/0x904/0xA04)
exactly — the EINT scheme is identical between 9810 and 9820.

Interrupts (FACT, `ex9820-irq.h`): `INTREQ__ALIVE_EINTn = 5 + n` (SPI), so
gpa0-3 VOLUP = SPI 8 = **GSIV 40**, gpa0-4 VOLDN = SPI 9 = **GSIV 41**,
gpa0-6 WINK = SPI 11 = **GSIV 43**, gpa2-4 POWER = SPI 25 = **GSIV 57**;
codec CS47L93 IRQ gpa2-2 = SPI 23 = GSIV 55 (for later D8 audio work).
This **closes the DERIVED/UNVERIFIED marker in §1.5** — confirmed from the
vendor dt-bindings header, not just the mangled DTB.

Key polarity (FACT, `exynos9820-beyondlte_input_common.dtsi`): all four keys
`gpios = <&gpaX n 0xf>` (function F = EINT), `pin-pud = <0>` (no internal
pull → external pull-up, pressed = low), debounce 10 ms; wakeup flag on
POWER + WINK only.

## 4. Bug found in the Beyond0 touch port (FACT, fix = own commit)

`drivers/S6SY761TouchBeyond0/Device.h:39-43` still carries the **9810** values:
- `STAR2_ALIVE_GPIO_PHYSICAL_ADDRESS 0x14050000` — the Beyond0 DSDT provides
  `Memory32Fixed(0x15850000)` (`Dsdt.asl:292`), and `Device.c:1433` *matches
  the resource against the constant* → on the S10e the match fails,
  `fAliveResourceFound` stays FALSE, probe returns
  STATUS_DEVICE_CONFIGURATION_ERROR. Touch would never start.
- `STAR2_GPA1_CON 0x040` / `STAR2_GPA1_PUD 0x048` — on the 9820 those are
  **gpa2's** registers (gpa1 moved to 0x020/0x028, §3). Even with the base
  fixed, the driver would configure the wrong bank's CON/PUD.
- The EINT constants (0x704/0x808/0x904/0xA04) are CORRECT for the 9820
  (gpa1 eint/fltcon offsets unchanged, §3) — only base + CON/PUD are wrong.

Fix: base → 0x15850000, GPA1_CON → 0x020, GPA1_PUD → 0x028 in the Beyond0
package only (9810 package stays byte-unchanged).

## 5. Windows buttons architecture — decision (from notes/d8-audio-windows.md Q3)

Native in-box path, no custom HID code:
1. **`Exynos9820Gpio`** KMDF driver = GpioClx provider for the ALIVE block
   (skeleton: official `gpio/samples/SimGpio`, memory-mapped controller).
   Pin space = EINT index (gpa0-0..7 = 0..7, gpa1 = 8..15, gpa2 = 16..23,
   gpa3 = 24..31, gpa4 = 32..33); controller `_CRS` carries the 34 EINT
   GSIVs 37..70 in pin order.
2. **DSDT `Device (BTNS)` `_HID "ACPI0011"`** + 4 `GpioInt(Edge, ActiveBoth,
   …)` + `_DSD` UUID **FA6BD625-9CE8-470D-A2C7-B3CA36C4282E** (exact format
   fetched from learn.microsoft.com/acpi-button-device, desktop-edition
   sample): Power = 0x01/0x81 System Power Down, VolUp = 0x0C/0xE9,
   VolDn = 0x0C/0xEA, Bixby = 0x0C/0x221 AC Search. Top-level collection
   0x01/0x0D "Portable Device Control". Inbox `Hidinterrupt.sys` binds it.
3. ActiveBoth: GpioClx emulates via level+dynamic polarity (documented
   requirement, Q3.2); Exynos EINT supports level-low/high natively.

## 6. Progress log

- 2026-10-11: §1 written from `dumps/hhxj6/{base,ov00}.dts`; §1.5 keys + EINT
  SPIs decoded; §1.6 vendor firmware inventory (codec = CS47L93, amps = 2×
  CS35L40, ABOX = Calliope 2×CA32); §1.7 kernel strings. Next: two research
  passes (Linux stack; Windows prior art) → `notes/d8-audio-linux.md`,
  `notes/d8-audio-windows.md`; then constants + DSDT + keys driver.
- 2026-10-11: both research passes done (Linux: `notes/d8-audio-linux.md`,
  Windows: `notes/d8-audio-windows.md`). Spot-checked the load-bearing claims
  against raw sources: board dtsi (CS47L93 on spi_2, amps on hsi2c_38, FLL
  ref 26 MHz — the subagent's correction of my 25 MHz guess is right), input
  dtsi (key pins/flags), abox-gic regs (local DTB 5312-5314), vendor pinctrl
  (bank/EINT layout §3), dt-bindings irq header (GSIVs §3), ACPI0011 docs
  (_DSD format §5). §3/§4/§5 added. Touch-port bug found (§4) — own commit.
