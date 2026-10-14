# Brightness control from Windows - research (2026-10-14)

Read-only research task. No code changes, no phone reads (phone in use under Windows).
Facts are tagged **[FACT: source]**, inferences **[INFER]**, unverified constants **UNVERIFIED**.

## Progress log

- 2026-10-14 start. Exynos9820.h already has `EXYNOS_DSIM_BASE 0x19080000` VERIFIED (dsim@0x19080000),
  SIZE 0x10000 CONSERVATIVE; live DTB declares 3 ranges for dsim: 0x19080000+0x300, 0x19161000+0x800,
  0x19160000+0x100 (notes/s10e-dtb-mmio.md S02). No dtc on PATH, no python fdt lib -> writing a tiny parser
  in the scratchpad.
- Parsed stock `dt.dtb` + `dtbo.img` (7 overlays, BEYOND0LTE EUR OPEN rev 17..25) with a scratch FDT dumper.
  **[FACT: dtbo.img, all overlays]** panel driver node fragment@88 (`samsung,panel-drv`, panel@0 in base DT,
  status disabled until overlay) has
  `panel-lookup { panel-name = "s6e3fa7_beyond0_a3_s0_default | s6e3fa7_beyond0_a3_s0_utype | s6e3fa7_beyond0_a3_s0_preliminary"; panel-ddi-info = <&s6e3fa7_beyond0>; panel-lut = <0x0 0x300000 0x2 0x0  0x0 0xf0000 0x1 0x0 ...> }`
  -> **panel IC = Samsung S6E3FA7** (not S6E3HA9; HA9 is beyond1/2/x = S10/S10+/5G). The lut keys on the
  panel ID read by sboot/kernel (ID 0x30xxxx -> index 2 "preliminary"?, mask 0x0f0000 ... decode with the
  panel_drv lookup code; [INFER]).
  **[FACT: dtbo]** `s6e3fa7_beyond0` ddi node: mode=2 (command mode), 1080x2280 (0x438x0x8e8), 60 Hz,
  data_lane 4, dsi-hs-clk 0x477 = 1143 Mbps, escape clk 20 MHz, dsc_en 0, mic_en 0, hdr_max_luma 0x3d0900.
  **[FACT: dtbo]** panel GPIOs: disp-reset (gpio bank phandle unresolved, pin 4), disp-det pin 2 (irq-pend
  reg 0x10430a18 bit 2), conn-det; regulators ddi-3p0 3.0 V, ddi-1p8 1.8 V, ddr-1p6 1.6 V (lpm 1.5 V).
  **[FACT: dt.dtb]** `dsim@0x19080000 { compatible "samsung,exynos9-dsim"; reg = 0x19080000+0x300,
  0x19161000+0x800 (D-PHY), 0x19160000+0x100; interrupts <0 0xbe 0> (SPI 190 -> GSIV 222); clocks aclk }`.
  Base matches `EXYNOS_DSIM_BASE` (VERIFIED). DSIM IRQ GSIV 222 is **UNVERIFIED** vs the header (not in it).
- Fetched LineageOS kernel (branch lineage-23.2 exists; branches 18.1..23.2). Panel driver is Samsung's
  "panel_drv" framework, **not** dpu20/panels: `https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/video/fbdev/exynos/panel/s6e3fa7/s6e3fa7_beyond0_a3_s0_panel.h`,
  `https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/video/fbdev/exynos/panel/s6e3fa7/s6e3fa7_beyond0_a3_s0_panel_dimming.h`, `https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/video/fbdev/exynos/panel/s6e3fa7/s6e3fa7.c`, `https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/video/fbdev/exynos/panel/panel_bl.{c,h}`.

### Panel brightness - FACTS from s6e3fa7_beyond0_a3_s0_panel.h (lineage-23.2)
- **No DCS 0x51.** Brightness is Samsung **AID dimming**: `beyond0_a3_s0_set_bl_cmdtbl[]` (PANEL_SET_BL_SEQ) =
  1. `F0 5A 5A` level-2 key enable (KEY2_ENABLE)
  2. `CA` + 33 bytes gamma (S6E3FA7_GAMMA_CMD_CNT 34) - from GAMMA_MAPTBL, row = luminance index
  3. `B1 hh ll` AOR (2 bytes) - AOR_MAPTBL
  4. `B5` + 23 bytes TSET/MPS/ELVSS (byte1 temperature 0x19=25C, byte2 MPS_CON 0xDC, byte3 ELVSS) - ELVSS/TSET maptbls
  5. `F4 EB 23` VGH/VINT - VINT_MAPTBL
  6. `B4` ACL control, `B4 20` ACL dim frame, `55 00` ACL on/off
  7. `B8` IRC (28 bytes) + `B8 80` IRC on
  8. `EB` POC comp2/comp3
  9. `F7 03` GAMMA_UPDATE_ENABLE (latches the new gamma/AOR)
  10. `F0 A5 A5` level-2 key disable
  Level-1 key = `9F A5 A5`/`9F 5A 5A`, level-3 = `FC 5A 5A`/`FC A5 A5` (not used by set-bl).
- Packet type for all of these: DSI_PKT_TYPE_WR (panel_drv picks short/long by length; [INFER] 1-2 byte ->
  DCS short write 0x05/0x15, else DCS long write 0x39 - confirm in panel_drv.c/dsim_drv.c).
- **User range**: UI brightness 0..255 (UI_DEF_BRIGHTNESS = 128, panel_bl.h), HBM to 486 (internal
  BRT_SCALE 100). brt_tbl maps user brightness -> 74 normal luminance steps (2..420 nit) + 16 HBM
  (444..800 nit). E.g. brt 128 -> 183 nit, 255 -> 420 nit.
- **Gamma (CA) is not a static table**: `init_gamma_table` -> either copied from the panel's own
  "dim flash" (`dim_flash_on = true` for the a3_s0 default variant, plus `dim_flash_gamma_offset`), or
  generated at probe by the dimming library (`generate_gamma_table_using_lut`, dimming.c) from the
  panel's per-unit **MTP** read + `dimming_lut[]` (lum, base-lum, gamma 2.15, rtbl/ctbl offsets).
  So the CA bytes are **per-panel calibration data** -> we cannot hard-code them. [FACT: s6e3fa7.c
  init_gamma_table / init_subdev_gamma_table_using_lut / generate_gamma_table_using_flash]
- **AOR (B1) is a static table** (`beyond0_a3_s0_dimming_aor_table`, 90 rows x 2 bytes): 2 nit 08 D4 ...
  60 nit 03 02, ~183 nit 03 00, 195 nit 02 62 ... 265-357 nit 00 E0, 420 nit 00 14, HBM 00 14.
  **dimming_lut base luminance** column: 2..60 nit all use base 107 (gamma for 107 nit, dimmed by AOR
  only + small rtbl/ctbl corrections); 64..183 nit base rises 109..277; 183..220 nit base 277 with AOR
  falling; >220 nit base rises to 420.
- [INFER] Therefore an **AOR-only dimmer** (keep whatever CA/B5/F4 sboot programmed, change only B1 +
  F7 03 latch, inside F0 key) is the smallest viable brightness control: it is how Samsung itself does
  2..60 nit (and 195..220 nit). It changes the emission duty, so luminance ~ (1 - AOR/period) with no
  gamma recompute. Colour accuracy degrades if the gamma row stays at a high base luminance, but it
  cannot overdrive the panel (AOR only reduces emission). Exact AOR period/max value: UNVERIFIED (table
  max 0x08D4 at 2 nit; LPM uses 08 98).
- **[FACT: s6e3fa7.h]** readable panel registers: ID 0x04 (3 bytes; kernel also gets it from cmdline
  `lcdtype=`, panel_drv.c early_param), MTP 0xC8 (32 B), HBM gamma 0xB3 ofs 2 (32 B), GAMMA 0xCA,
  **AOR 0xB1 (2 B)**, ELVSS 0xB5, VINT 0xF4, RDDPM 0x0A, ERR_FG 0xEE, chip id 0xD6. Reading 0xB1 and 0xCA
  tells us exactly what sboot programmed, non-destructively.
- [INFER, numerically checked against the tables] AOR unit = lines of a ~2299-line period
  (1080x2280 + v-porch 15+3+1 = 2299): L ~= L_base * (1 - AOR/2299). 2 nit: 107*(1-2260/2299)=1.8;
  420 nit: 420*(1-20/2299)=416; 265 nit: 308*(1-224/2299)=278. Period **UNVERIFIED**.

### DSIM (dpu20 cal_9820) - FACTS
Sources: `https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/video/fbdev/exynos/dpu20/cal_9820/regs-dsim.h`, `https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/video/fbdev/exynos/dpu20/cal_9820/dsim_reg.c`, `https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/video/fbdev/exynos/dpu20/dsim_drv.c`, `https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/video/fbdev/exynos/dpu20/dsim.h`,
`https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/video/fbdev/exynos/dpu20/panels/common_mipi_lcd.c` (mipi_write/mipi_read used by panel_drv).
- Base 0x19080000 (DT reg[0], size 0x300; matches Exynos9820.h EXYNOS_DSIM_BASE VERIFIED). reg[1]
  0x19161000+0x800 = D-PHY (not needed for packets). IRQ SPI 0xbe (GSIV 222, UNVERIFIED in header).
- Register offsets (Samsung source, **not read on device** -> UNVERIFIED until a peek):
  | off | name | use |
  |---|---|---|
  | 0x00 | VERSION | sanity read |
  | 0x08 / 0x0c | LINK_STATUS0/1 | STATUS1 bit26 = CMD_MODE_STATUS (1 = active) |
  | 0x1c | DPHY_STATUS | stop-state/ULPS bits |
  | 0x2c | ESCMODE | bit7 CMD_LPDT (1 = commands in LP; kernel sets **0 = HS**), bit16 FORCE_BTA |
  | 0x4c | CONFIG | bit18 VIDEO_MODE (expect 0), lanes bits[6:5], LANES_EN[4:0] |
  | 0x50 | INTSRC (W1C) | b29 SFR_PL_FIFO_EMPTY, b28 SFR_PH_FIFO_EMPTY, b27 PH_FIFO_OVERFLOW, b24 FRAME_DONE, b23 INVALID_SFR_VALUE, b22 ABNORMAL_CMD_ST, b21 LPRX_TOUT, b20 BTA_TOUT, b18 RX_DATA_DONE, b17 RX_TE, b16 RX_ACK, b15 ERR_RX_ECC, b14 RX_CRC |
  | 0x54 | INTMSK | (1 = masked) |
  | 0x58 | PKTHDR (write = push header FIFO) | [31:24] BTA_TYPE, [23:16] DATA1, [15:8] DATA0, [7:0] DI (VC<<6 \| type); written with mask 0x1ffffff |
  | 0x5c | PAYLOAD (write = push 4 payload bytes, little-endian) | |
  | 0x60 | RXFIFO (read = pop) | |
  | 0x64 | SFR_CTRL | b1 SHADOW_REG_READ_EN, b0 SHADOW_EN |
  | 0x68 | FIFOCTRL | [21:16] NUMBER_OF_PH_SFR, b12 EMPTY_RX, b11 FULL_PH_SFR, b10 EMPTY_PH_SFR, b9 FULL_PL_SFR, b8 EMPTY_PL_SFR, b2/1/0 INIT_RX/PL/PH |
  | 0x80 | CMD_CONFIG | b17 PKT_GO_RDY, b16 PKT_GO_EN (kernel: 0), b8 MULTI_CMD_PKT_EN |
  | 0x84/0x88 | CMD_TE_CTRL0/1 | TIME_STABLE_VFP, TE_PROTECT_ON (15.8 ms), TE_TOUT (18 ms) |
  | 0x8c | CMD_STATUS | [7:0] abnormal cause |
- **Write sequence** (dsim_write_data + mipi_write):
  - payload length 1 -> **DCS short write, DI 0x05**: PKTHDR = 0x05 \| cmd<<8 (DATA1 0).
    (0x15 short-write-param exists but panel_drv never uses it except `B0 ofs` global-param.)
  - length >= 2 (even 2 bytes, e.g. `F7 03`, `B1 hh ll`) -> **DCS long write, DI 0x39**: push payload
    words to 0x5c first (bytes LE, last word partial), **then** PKTHDR = 0x39 \| (len&0xff)<<8 \| (len>>8)<<16.
  - Before: clear INTSRC.SFR_PH_FIFO_EMPTY (W1C). After: wait for FIFOCTRL.EMPTY_PH_SFR (or INTSRC b28),
    kernel timeout 50 ms (MIPI_WR_TIMEOUT); it only waits when NUMBER_OF_PH_SFR >= threshold 1 or for
    reads. On timeout kernel does dsim_function_reset and retries 3x - **we must not**; we abort.
  - Read: write SET_MAX_RETURN_PACKET_SIZE (DI 0x37, DATA0 = n) then DCS READ (DI 0x06, DATA0 = reg,
    PKTHDR.BTA_TYPE = 1), wait RX_DATA_DONE (100 ms), pop RXFIFO: low byte = response DT
    (0x21/0x22 short 1/2 B, 0x1c long: size in [23:8], then ceil(n/4) words), 0x02 = ack+error report.
    Offset reads use `B0 ofs` (DCS short-param 0x15) first.
- **Timing / mode constraints**: commands go in **HS** (ESCMODE.CMD_LPDT = 0, set by dsim_reg_init);
  in command mode the DSIM hardware arbitrates SFR packets against the DECON frame transfer
  (TE_PROTECT_ON window ~15.8 ms of a 16.7 ms frame [INFER: commands are held until the frame is out]);
  Linux simply pushes into the FIFO from any context with no TE wait (dsim_write_data). The only
  host-side precondition the kernel checks is IS_DSIM_ON_STATE + DECON not in hibernation
  (decon_hiber_block_exit) - sboot has no hibernation, so DSIM clocks stay on [INFER; CMU_DPU QCH
  state UNVERIFIED].
- Panel-side latching: gamma/AOR/ELVSS take effect on `F7 03` (GAMMA_UPDATE_ENABLE) at the next frame;
  all set-bl writes must be inside `F0 5A 5A` ... `F0 A5 A5`.

### sboot hand-off state
- [FACT] Linux decides "already initialised by bootloader" from the disp-reset GPIO level (panel_drv.c
  ~l.2387: rst_val 1 = PANEL_INIT_BOOT) and gets the panel ID from cmdline `lcdtype=` (early_param,
  panel_drv.c l.3405). UI default brightness after probe = 128 (-> 183 nit row) - but that is what the
  **kernel** programs, not sboot.
- **UNKNOWN** what luminance row sboot programs. Nothing in the kernel tree records it. Closing reads (in
  this order, all read-only on the panel side): DSI DCS-read 0xB1 (2 B AOR) and 0xCA (33 B gamma),
  0x0A (RDDPM, expect 0x9C = booster/sleep-out/normal/display-on), 0x04 (ID, compare with `lcdtype=` in
  /proc/cmdline under TWRP/Android). Also `cat /proc/cmdline | tr ' ' '\n' | grep lcdtype` on the phone.
  [INFER] The sboot-lit screen looks "full bright" in webcam shots (saturates the camera,
  notes/gpu-hw-path.md 446) - consistent with a high row, but unmeasured.

### Windows side - FACTS (learn.microsoft.com)
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-brightness-controls-on-integrated-display-panels :
  Monitor.sys owns brightness (WMI = the Settings slider, DPPE power policy, hotkeys). "For the first video
  target that is marked as ... D3DKMDT_VOT_INTERNAL, the monitor driver calls the KMD's DxgkDdiQueryInterface"
  for GUID_DEVINTERFACE_BRIGHTNESS (v1), _2 (v2), _3 (v3). "If the KMD doesn't support at least the
  Brightness Control Interface, the monitor driver uses ACPI to query for the _BCL, _BCM, and _BQC methods
  on the child device." Levels are 0..100, max must be 100, up to 101 levels, larger = brighter.
  If DxgkDdiGetPossibleBrightness fails, no brightness support.
- v1 DXGK_BRIGHTNESS_INTERFACE (Vista+): GetPossibleBrightness / SetBrightness / GetBrightness.
  v2 https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/ns-dispmprt-dxgk_brightness_interface_2
  : WDDM 1.2+, adds GetBrightnessCaps / SetBrightnessState / SetBacklightOptimization / GetBacklightReduction.
  v3 https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/ns-dispmprt-dxgk_brightness_interface_3
  : "used by ... WDDM 2.4 and later KMDs" (nits-based) -> **not for us** (full adapter = WDDM 1.3,
  display-only = WIN8 interface).
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgk_brightness_set :
  IRQL PASSIVE_LEVEL, "should be made pageable" (so we may block/poll with KeDelayExecutionThread).
  GetPossibleBrightness (same IRQL): buffer = [AC default, DC default, then supported levels].
- KMDDOD_INITIALIZATION_DATA also carries DxgkDdiQueryInterface (search result:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nf-dispmprt-dxgkinitializedisplayonlydriver)
  -> a display-only driver can expose v1/v2 too. Our tree: both init paths already wire
  `DxgkDdiQueryInterface = T_QueryInterface` -> `MwQueryInterface` (adapter.c:319) which returns
  STATUS_NOT_SUPPORTED today; display.c:301 already reports the child as D3DKMDT_VOT_INTERNAL (R14). So
  the hook point exists in both modes.
- [INFER] The Settings > Display brightness slider and the quick-settings slider appear when
  WmiMonitorBrightness (root\wmi) has an instance, i.e. when Monitor.sys accepted the interface. Check
  with `Get-CimInstance -Namespace root/wmi WmiMonitorBrightness` after the driver change.
- ACPI alternative (_BCL/_BCM on the display child in our DSDT) would need an ACPI-side handler that
  sends DSI packets - no AML can do that sanely; rejected.

## Recommended plan

**Owner: MaliG76Wddm KMD** (new `brightness.c`), not a separate driver. Reasons: Monitor.sys only asks
the KMD of the adapter whose child is VOT_INTERNAL (ACPI _BCL fallback cannot send DSI packets); the KMD
already maps DECON/DPU_DMA and owns scan-out, so it is the one place that can serialise DSIM SFR traffic
with its own DECON shadow updates (display.c MwRestride). Works in display-only mode (WIN8 KMDOD) and
full mode (WDDM 1.3) - implement **DXGK_BRIGHTNESS_INTERFACE_2** (v1 subset is enough to get the
slider; v2 adds GetBrightnessCaps/SetBrightnessState which can return STATUS_NOT_SUPPORTED-equivalent caps
= no adaptive/smooth). v3 is WDDM 2.4+ only.

Brightness model (MVP, AOR-only): read sboot's B1 once (baseline AOR `A0`), keep sboot's CA/B5/F4. Map
Windows level p in {0,5,...,100} to AOR = clamp(A0 + (AMAX - A0) * (100 - p)/100) with AMAX = 0x08D4
(Samsung's own 2-nit AOR, never exceeded) and never below A0 (we never go brighter than sboot, so no
HBM/ELVSS/thermal risk). Send: `F0 5A 5A` (long 0x39, 3 B) - `B1 hi lo` (long, 3 B) - `F7 03` (long, 2 B)
- `F0 A5 A5` (long, 3 B). Level 100 = restore A0 exactly. Full-range/colour-correct dimming (CA gamma
rows) is a later stage: needs MTP 0xC8 + dimming.c port or the panel's dim-flash, both per-unit data.

### Minimal DSIM send routine (polling, PASSIVE_LEVEL, no IRQ)
```
MapDsim = MmMapIoSpaceEx(0x19080000, 0x1000, RW|NOCACHE)      // 0x300 used
precheck: CONFIG(0x4c) bit18 == 0 (command mode); CMD_CONFIG(0x80) bit16 PKT_GO_EN == 0;
          FIFOCTRL(0x68) bit10 EMPTY_PH_SFR == 1 and bit8 EMPTY_PL_SFR == 1; else refuse.
send_long(buf,len):            // len <= 33 here
  INTSRC(0x50) = 1<<28          // W1C SFR_PH_FIFO_EMPTY
  for each 4 bytes: PAYLOAD(0x5c) = b0 | b1<<8 | b2<<16 | b3<<24   (tail zero-padded)
  PKTHDR(0x58) = 0x39 | (len & 0xff)<<8 | (len>>8)<<16
  poll FIFOCTRL bit10 (EMPTY_PH_SFR) up to 50 ms (KeDelayExecutionThread 1 ms steps); timeout -> log
  INTSRC + CMD_STATUS(0x8c), disable the feature for this boot, NO reset/retry.
send_short(cmd): PKTHDR = 0x05 | cmd<<8 ; same poll.
read(reg,n): INTSRC = (1<<18); PKTHDR = 0x37 | n<<8 (max return size); PKTHDR = 0x06 | reg<<8 | 1<<24 (BTA);
  poll INTSRC bit18 RX_DATA_DONE <= 100 ms; pop RXFIFO(0x60): DT 0x21/0x22/0x1c (len [23:8]) / 0x02 error.
```
Serialise all senders with one FAST_MUTEX/KEVENT in the adapter (brightness DDI vs anything else).

### Safe validation ladder (each step its own boot, results to this file; stop at the first surprise)
0. (TWRP/Android, read-only) `cat /proc/cmdline | tr ' ' '\n' | grep -i lcdtype` -> panel ID.
1. Windows, **read-only**: `s10ediag peek 0x19080000 0x100` (S10eDiag already has MMIO peek) twice, 1 s
   apart. Expect VERSION non-zero, CONFIG bit18 = 0, lanes = 4 (field 3), FIFOCTRL EMPTY bits set,
   CMD_CONFIG PKT_GO_EN 0, ESCMODE CMD_LPDT 0. Closes the offsets as VERIFIED (or kills the plan).
2. Windows, **DSI read only** (panel registers untouched): DCS read 0x0A (RDDPM, 1 B) then 0x04 (ID, 3 B),
   then 0xB1 (2 B) and 0xCA (33 B; Samsung reads some regs only after `F0 5A 5A` - try without first).
   Worst case is a BTA timeout -> INTSRC BTA_TOUT; the frame stream is DECON's, not ours. Record A0.
3. Windows, **one write that changes nothing**: `F0 5A 5A`, `B1 <A0 hi> <A0 lo>`, `F7 03`, `F0 A5 A5` -
   re-writing the value just read. Screen must stay identical; INTSRC must show no INVALID_SFR/ABNORMAL.
4. One real step: AOR = A0 + (0x08D4 - A0)/2 (darker only). Webcam/user eye; then write A0 back.
   Recovery if the panel goes dark/garbled: reboot (sboot re-inits the panel on every boot; we never write
   OTP/flash - no 0xC8/0xD6/POC commands are ever sent).
5. Only then: brightness.c + QueryInterface wiring, GetPossibleBrightness = {100,100,5..100 step 5}.

### Constants
| constant | value | status | source / read that closes it |
|---|---|---|---|
| DSIM base | 0x19080000 | VERIFIED | Exynos9820.h EXYNOS_DSIM_BASE (dsim@0x19080000) |
| DSIM IRQ | SPI 190 / GSIV 222 | UNVERIFIED (not needed, polling) | stock dt.dtb interrupts <0 0xbe 0> |
| DSIM reg offsets 0x00..0x8c (table above) | - | UNVERIFIED | regs-dsim.h cal_9820; step 1 peek |
| INTSRC/FIFOCTRL bit positions | - | UNVERIFIED | regs-dsim.h; step 1/2 |
| DSI DT 0x05/0x15/0x06/0x37/0x39, resp 0x02/0x1c/0x21/0x22 | - | VERIFIED (MIPI DSI spec, generic) | not SoC-specific |
| level-2 key F0 5A 5A / F0 A5 A5 | - | UNVERIFIED on device | s6e3fa7_beyond0_a3_s0_panel.h |
| AOR reg 0xB1, 2 B | - | UNVERIFIED on device | s6e3fa7.h S6E3FA7_AOR_REG; step 2 read |
| gamma update F7 03 | - | UNVERIFIED on device | panel.h GAMMA_UPDATE_ENABLE; step 3 |
| AOR max used 0x08D4 (2 nit) | - | UNVERIFIED on device | dimming_aor_table row 0 |
| AOR period ~2299 lines | - | INFERENCE | table arithmetic; step 4 |
| sboot baseline AOR A0 | ? | UNKNOWN | step 2 read |
| write/read timeouts 50/100 ms | - | SOURCE-ONLY | dsim.h MIPI_WR/RD_TIMEOUT |

### Open questions needing a phone read
- lcdtype (panel ID) and whether it selects a3_s0 default (dim-flash) or preliminary (LUT) variant.
- DSIM register dump at Windows runtime (offsets/bits, PKT_GO, LP/HS, FIFO state).
- sboot's B1/CA/B5 values (A0 and its luminance row).
- Whether CMU_DPU clock gating ever stops DSIM while Windows idles (our scan-out keeps it running [INFER]).
- Whether DSI reads need the F0 key on this panel.

## Progress log (cont.)
- 2026-10-14 done: panel = S6E3FA7 (beyond0_a3_s0), brightness = AID (gamma CA + AOR B1 + ELVSS B5 ...,
  latched by F7 03 inside F0 key) - no 0x51; DSIM packet path documented from cal_9820; Windows interface =
  DXGK_BRIGHTNESS_INTERFACE(_2) via MwQueryInterface; plan = AOR-only MVP in MaliG76Wddm after a 5-step
  read-first ladder. Note: kernel writes PKTHDR via dsim_write_mask (read-modify-write of a FIFO port);
  a plain 32-bit write is the intended equivalent [INFER]. S10eDiag's `peek`/`poke` CLI can run steps 1-4
  from a script without any KMD change. Scratch DT dumps: %TEMP%\claude\...\scratchpad\br\*.dts (not kept).
  No code changes, no commits, no phone access in this task.
- Review (main session): EXYNOS_DSIM_BASE 0x19080000 VERIFIED in Exynos9820.h:873 (size 0x10000 conservative, three DT ranges); MwQueryInterface adapter.c:319 returns STATUS_NOT_SUPPORTED; display.c:301 reports D3DKMDT_VOT_INTERNAL - all three claims match the tree. DSIM register offsets remain UNVERIFIED (cal_9820 source only).

## Progress log - device work (2026-10-14, over SSH, Windows running, KMD 38)
- D1 read-only DSIM dump 0x19080000.. (skipping RXFIFO +0x60): VERSION 0x02030000; LINK_STATUS1 (+0x0C) 0x060008c8
  (CMD_MODE_STATUS 1, line counter moving - 12 samples spread 0x10..0x816: frames stream continuously);
  MIPI_STATUS (+0x18) 0x20000000 in 15/15 samples (FRM_PROCESSING, never FRM_DONE); DPHY_STATUS 0x0007fc00;
  CLK_CTRL 0x06131f04; ESCMODE (+0x2C) 0x01400000; RESOL 0x08e80438; CONFIG (+0x4C) 0x04807cff; INTSRC (+0x50)
  0x01400000 (FRAME_DONE + ABNRMAL_CMD_ST); INTMSK 0x8e7b4447; SFR_CTRL (+0x64) 0x2 = SHADOW_REG_READ_EN;
  FIFOCTRL (+0x68) 0x00001507 (all empty); CMD_CONFIG (+0x80) 0 (PKT_GO off); CMD_TE_CTRL0/1 0x40 / 0x2c17323a;
  CMD_STATUS (+0x8C) 0x08 (ABNORMAL_CAUSE 8 - not decoded anywhere in Samsung's code); +0x100 0x00781919,
  +0x104 0x0f010000. Register map = Samsung cal_9820 regs-dsim.h (fetched; offsets consistent with every value).
- D2 first TX test (harmless): clear INTSRC PH_EMPTY|RX_DONE, PKTHDR 0x00000137 (max return size 1), PKTHDR 0x01000A06
  (DCS read 0x0A + BTA). Result: FIFOCTRL 0x00021107 = 2 headers QUEUED, not sent; RX empty; minutes later unchanged.
  Writing INTSRC bit 22 (W1C) did not clear it (reads back 0x01400000). Open: are reads shadowed (SFR_CTRL bit 1)?
  is the queue held by the abnormal state, by the continuous HW-triggered frames, or by a TE-protect window?
  The 2 queued headers are a max-return-size and a power-mode read - harmless whenever they go out.
- D3 SFR_CTRL 0 (shadow read off): FIFOCTRL 0x00029107 (bit 15 undocumented), still 2 queued; restored to 0x2.
- D4 CMD_TE_CTRL1 TE_PROTECT_ON 0x2c17 -> 0x0001 (TOUT kept): no change; restored 0x2c17323a. Samsung derives both
  from hs_clk 1143 MHz: 15.8 ms protect (1143*158/16 = 0x2c17), 18 ms timeout (0x323a) - values match exactly.
- D5 DECON HW_SW_TRIG_CONTROL 0x3061 -> 0x3070 (HW_TRIG_EN 0, HW_TRIG_MASK_DECON 1 = Samsung's DECON_TRIG_DISABLE):
  no TX, MIPI_STATUS still FRM_PROCESSING; restored 0x3061 (frames continue: line counter moving). Android masks the
  trigger whenever the screen is idle and sends commands then; under Windows DECON triggers every TE forever.
- ABNORMAL_CAUSE 8: no definition found (Samsung cal_9820, LineageOS, web search). Leading hypothesis: the command
  engine latched an abnormal state at the sboot -> UEFI -> Windows hand-off (INTSRC bit 22 is not W1C-clearable while
  set) and holds the PH FIFO until a DSIM function reset (SWRST FUNCRST bit 8) or FIFO init (FIFOCTRL INIT_PH/PL/RX)
  - both can blank the panel until the link is re-initialised -> do them only with someone watching (or after the
  KMD learns the full DSIM/panel init sequence). The 2 queued headers are harmless and cleared by the next reboot.
- Screen-off meanwhile: blank screen saver after 120 s (HKCU Control Panel\Desktop + SPI_SETSCREENSAVETIMEOUT/ACTIVE
  in the session); powercfg monitor timeout stays 0 until the KMD can really switch the panel off (DCS 0x28/0x10).

## Progress log - D6 (2026-10-14, fresh boot after rotation recovery, KMD 41, nothing written to DSIM this boot)
- Fresh-boot state, before any DSIM write: MIPI_STATUS 0x20000000, INTSRC 0x01400000, FIFOCTRL 0x00001507,
  CMD_STATUS 0x08 -> the ABNORMAL_CMD_ST / cause 8 state exists at every Windows boot (not caused by D2-D5).
- New `s10ediag sample <phys> <count> [mask]` (one handle, histogram, no PRAM mark), deployed as C:\S10eTmp\s10ed2.exe:
  - MIPI_STATUS (+0x18): 20000 reads in 93 ms (~6 frames at 60 Hz) -> 0x20000000 x 20000. FRM_PROCESSING never
    drops, not even for one sample between frames -> DSIM's command-mode frame state machine never reaches idle,
    so SFR packets (which go between frames) never get a slot. This is consistent with the queued PH entries in D2.
  - LINK_STATUS1 & 0xfc000000: 0x04000000 x 20000 (CMD_MODE_STATUS active, always).
- INCIDENT: the same SSH command line had a typo `sample 0x19080000c` (9 hex digits = PA 0x1_9080_000C, unmapped) ->
  the read hung the phone (SSH and ping dead). No watchdog in boot116-gpuon-nowdt -> needs a manual force restart
  (Vol Down + Power). Lesson: S10eDiag maps any PA it is given; the tool must refuse addresses outside a
  verified-MMIO whitelist (follow-up task).
- Analysis while the phone was down: Android's kernel (dsim_reg_init: "already enabled in bootloader" -> no re-init)
  sends DCS commands fine from sboot's DSIM state, so the stuck FRM_PROCESSING/abnormal state most likely appears
  after sboot: UEFI SimpleFramebufferDxe FbDeconPresent drives DECON HW_SW_TRIG with SW-trigger edges using the
  9810 offsets/bits (UNVERIFIED on 9820), then the KMD sets 0x3061. Transfer math says the link should idle
  ~3.7 ms per frame (1143 Mbps x 4 lanes -> 7.39 MB in ~12.9 ms of 16.7 ms), so 20000/20000 FRM_PROCESSING is
  a stuck FSM, not back-to-back frames.
- Next (needs the user watching the screen): DSIM FUNCRST (SWRST +0x04 bit 8, self-clearing; Samsung
  dsim_reg_function_reset; keeps SFRs, resets the link FSMs), then re-read MIPI_STATUS/CMD_STATUS/INTSRC and,
  if FRM_PROCESSING drops, repeat the harmless DCS read 0x0A. Recovery if the panel blanks: `ssh s10e shutdown /r`.
- CORRECTION (user, same hour): the phone was not hung - the screen showed a Windows Update install ("24%").
  So SSH/ping loss was Windows restarting into an update, not (necessarily) the bad-PA read; whether the
  0x1_9080_000C read faulted is unknown. The guard stays (an unmapped read is still a real hazard).
  Follow-up: after the update finishes, check the build/driver state and pause Windows Update (it can replace
  or roll back test-signed drivers and restart unattended).

## Progress log - D7 DSIM FUNCRST (2026-10-14 ~16:10, build 22631.6199, KMD 41, user watching)
- Before: SWRST 0, MIPI_STATUS 0x20000000, INTSRC 0x01400000, FIFOCTRL 0x00009507 (bit 15 set again), CMD_STATUS 0x08.
- poke SWRST (+0x04) = 0x100 (FUNCRST); readback 0 (self-cleared).
- After: MIPI_STATUS 0x20000000 x 20000 (still FRM_PROCESSING); INTSRC 0x40480000 = SW_RST_RELEASE | ABNRMAL_CMD_ST |
  UNDER_RUN (FRAME_DONE gone - no frame completed since the reset); FIFOCTRL 0x1507; CMD_STATUS 0x04 (cause 8 -> 4);
  LINK_STATUS1 line counter frozen at 0x1ff (3000/3000 reads; before the reset it moved). DECON trig still 0x3061.
  -> the reset cleared cause 8 but the link stopped streaming: DSIM now waits mid-frame (underrun) for pixel data.
- After FUNCRST, a DCS read 0x0A (PKTHDR 0x137 + 0x01000A06) still queued (FIFOCTRL 0x00021107), CMD_STATUS 4.
- User: screen frozen (the panel holds its last GRAM frame). A minute later the phone dropped off WiFi/SSH
  (16:09 dark) before the planned DECON trigger toggle ran - likely the system stalls once scan-out stops
  (KMD/DWM waiting on a frame that never completes) [INFER]. Needs a force restart.
- Conclusion D7: FUNCRST alone is not a usable recovery - it stops the link. Whatever holds the command queue,
  the link must be re-armed afterwards (Samsung never uses FUNCRST without a full dsim/decon re-enable).
