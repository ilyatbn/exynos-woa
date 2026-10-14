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
- Correction: the 16:09 "dark" was a WiFi blip (back 16:09:57, no reboot - DSIM still in the post-FUNCRST state).
- DECON HW_SW_TRIG 0x3070 -> 0x3061 toggle: line counter still frozen at 0x1ff, FRM_PROCESSING, PH queue still 2.
  Re-triggering DECON does not restart the link. Recovering with `shutdown /r` (sboot re-inits DSIM + panel).

## Progress log - D8 boot snapshot (S10eDiag 956448f, 2026-10-14 16:14 boot)
- PRAM ring (dumpmmio 0x8FE0A000 0x4000; `dumpram` RAM mode fails with 59 here) at t=4.0 s, S10eDiag load:
  DECON TRIG 0x3061, DSIM MIPI 0x20000000, INTSRC 0x01400000, FIFO 0x9507, CMDST 0x08,
  BUSY 5000/5000 (FRM_PROCESSING), LINEMOVED 4997/5000.
- -> the stuck state exists before MaliG76Wddm touches anything. 0x3061 is exactly UEFI FbDeconPresent's final value
  (sboot base with MASK_DECON/SW_TRIG cleared, HW_TRIG_EN set; regs-decon.h cal_9820 bit layout = the 9810 one used
  by UEFI: SW_TRIG_EN 8, HW_TRIG_MASK_DECON 4, HW_TRIG_EN 0 - verified against LineageOS lineage-22.2 cal_9820).
  Suspects left: sboot itself, or UEFI's SW-trigger edges / switch to TE auto-refresh.
- D9 build: SimpleFramebufferDxe FbDsimSnapshot (read-only, ~50 ms each) at driver entry and around the first two
  FbDeconPresent calls -> DEBUG lines "FbDsim <tag>: TRIG MIPI INTSRC FIFO CMDST busy linemoved" in the UEFI RAM log.
  Image s10e-artifacts\boot116-gpuon-nowdt-d9.img sha256 1ed69ae7... = HEAD + build-only WINDOWS_WATCHDOG 0 and
  GPU_POWER_ON 1 (both reverted after the build; pack -SkipFreshCheck), same recipe as boot116-gpuon-nowdt.
- D9 flash: `shutdown /r` after `s10ediag recovery` came back to Windows (known, m14 run 20); `s10ed3 flushreset`
  reached TWRP (bat 46 %). boot116-gpuon-nowdt-d9.img pushed + dd to by-name/boot; readback via
  `head -c 18255872 | sha256sum` = 1ed69ae7... match (toybox dd has no iflag=count_bytes - that readback hashed 0 B).
- **D9 result** (UEFI RAM log, dumpmmio 0x8FF00000, runs\uefilog-d9.bin):
  entry (= as sboot left it): TRIG 0x3070 (HW_TRIG_EN 0, MASK_DECON 1), MIPI 0x20000000, INTSRC 0x01400000,
  FIFO 0x1507, CMDST 0x08, busy 5000/5000, linemoved 4996. pre-present1 identical; post-present1 TRIG 0x3061,
  FIFO 0x9507 (bit 15 appears after our first SW-trigger present), busy 4998/5000; present2 same.
  -> **sboot already leaves DSIM in FRM_PROCESSING + ABNORMAL cause 8**, not our UEFI or KMD. With DECON's trigger
  masked, FRM_PROCESSING stays set and LINK_STATUS1[12:0] still changes -> that field is not a frame-line counter
  driven by DECON (free-running) [INFER]; "line moved" is no evidence of frames (correction to D1/D6 reading).
  Next: what Samsung's kernel does between "already enabled in bootloader" and its first DCS read (panel ID),
  since Android does get commands through from this same sboot state.

## Progress log - D10 DECON operation mode (2026-10-14 ~16:40)
- Read-only: DSIM NUM_OF_TRANSFER 0x8e8 (2280), UNDERRUN_CTRL 0x57f, THRESHOLD 0x438 (1080), RESOL 0x08e80438 -
  consistent. DECON_F GLOBAL_CONTROL (+0x00) **0x00001011** = DECON_EN_F | RUN_STATUS | MASTER_MODE 1, and
  OPERATION_MODE (bit 8) = **0 = VIDEO mode**; INTERRUPT_PENDING 0x3000 (frame start+done); SCALED_SIZE 0x08e80438;
  DSIM_CONNECTION_CONTROL 0x40; HW_TE_CNT 0x22ed22ed; FRAME_COUNT 0x2e60 -> 0x2eba in ~1 s (counting);
  CLOCK_CONTROL_0 0x11011101; PLL_SLEEP_CONTROL 0.
- Samsung (LineageOS lineage-22.2 cal_9820 decon_reg.c): decon_reg_init() when RUN_STATUS is already set
  ("already called by BOOTLOADER") calls decon_reg_init_probe() -> decon_reg_set_operation_mode(CMD) (+ rgb order,
  data path, sizes), then for command mode decon_reg_set_trigger(DISABLE) (HW_TRIG_MASK_DECON). So Android
  re-puts DECON into command mode at probe; sboot leaves it in VIDEO mode while DSIM is in command mode.
  -> Hypothesis: video-mode DECON streams continuously, DSIM never reaches a frame boundary (FRM_PROCESSING forever,
  ABNORMAL cause 8), so SFR command packets never get a slot. Under Windows nothing ever switched the mode.
- Test plan: GLOBAL_CONTROL |= OPERATION_MODE_CMD (bit 8), SHADOW_REG_UPDATE_REQ |= GLOBAL (bit 31; GLOBAL_CONTROL is
  in the shadowed 0x0000-0x011C range), keep HW trigger 0x3061 (TE-driven frames). Then FRAME_COUNT, MIPI_STATUS
  histogram, PH FIFO, DCS read 0x0A. Recovery: ssh shutdown /r.
- **D10 result - commands flow.** poke GLOBAL_CONTROL 0x1011 -> 0x1111 (CMD mode), SHADOW_REG_UPDATE_REQ 0x8000003f:
  GLOBAL_CONTROL reads 0x1111; FRAME_COUNT 0x3940 -> 0x3949 (frames continue); MIPI_STATUS 20000 reads:
  0x00000000 x 4549, 0x20000000 x 15451 (**idle 23 %** - the link now reaches frame boundaries).
  INTSRC/CMD_STATUS unchanged at first (0x01400000 / 8; cause 8 stays latched, harmless).
- DCS read RDDPM: INTSRC W1C 0x10040000, PKTHDR 0x137 (max return 1), PKTHDR 0x01000A06 (DCS read 0x0A + BTA)
  -> FIFOCTRL 0x0507 (PH empty = sent, RX not empty), INTSRC 0x13540000 (PH_EMPTY, BTA, FRAME_DONE, RX_DATA_DONE,
  BTA_TOUT bit 20 also set). RXFIFO: 0x02009e21 = DT 0x21 (DCS short read resp, 1 B) data **0x9E**
  (booster on, sleep out, normal mode, display on); next pop 0x07690400 (unparsed; RX then empty, FIFO 0x1507).
  -> **first DSI command round trip under Windows.** The panel is the screen the user is looking at; nothing changed.
- Panel ID (DCS read 0x04, max return 3): RX 0x1600031c (DT 0x1c long read resp, WC 3) + payload 0x00150191 ->
  ID bytes **91 01 15**.
- 0xB1 without the level-2 key: DT 0x22, data 00 00 (+ a 0x02 error-report word 0xc048b002). With `F0 5A 5A` (long
  write: PAYLOAD 0x005A5AF0, PKTHDR 0x339) first: DT 0x22 data **00 14** -> sboot baseline AOR A0 = 0x0014 (near
  full brightness, consistent with the AOR table). Key relocked with `F0 A5 A5`; FIFO empty after each write.
- D10 step 3 (no-op write): INTSRC W1C all, then F0 5A 5A / B1 00 14 / F7 03 (long write, 2 B) / readback / F0 A5 A5:
  FIFO 0x5507 after the writes (PH+PL empty), INTSRC 0x31000000 (no INVALID_SFR, no ABNORMAL - bit 22 now clears
  with the frame boundaries back), B1 reads 00 14. CMD_STATUS still reads 8 (stale cause register).
- D10 step 4 (one real step, darker only): B1 04 74 (A0 + (0x08D4 - A0)/2) + F7 03 -> B1 reads back **04 74**.
  The webcam cannot see the phone (it faces the room), so the visual check is the user's; panel left at 04 74.
  Packet recipe that works: PAYLOAD = bytes LE (B1 hi lo -> 0x00<lo><hi>B1), PKTHDR 0x339 (DT 0x39, WC 3);
  F7 03 as DT 0x39 WC 2. Reads: PKTHDR 0x0n37 (max return n), then 0x0100<reg>06; RXFIFO word = DT | d0<<8 | d1<<16.
- DECON in command mode, GPU path check: `d3dtri cube 10` = **2268 frames / 10 s = 226.8 fps, 0 present failures**
  (27.7 fps in video mode an hour earlier, same KMD 41 / UMD 0066). Runs with -Wait 60 were cut by runuser2 (startup is
  slow), not hung. FRAME_COUNT keeps counting; MIPI idle ~23 %.
- Self-check of the screen (user's method: screenshot, launch, screenshot): the first two dumps were black = the blank
  screen saver (scrnsave.scr in session 1); after taskkill scrnsave.scr: dump, `start notepad` via runuser2, dump ->
  the dumps differ, fbB.png = desktop with the new Notepad window. (IDMA latched base 0xCA000000, pitch 4352.)
- Not yet known: why video mode capped presents at ~28 fps (the KMD/UMD present path waits on something that only
  completes per DECON frame in one mode or the other) [open].
- **CORRECTION (user):** the 226.8 fps cube runs were screen-saver artefacts (the blank screen saver covered the cube
  window - scrnsave.scr was found running right after them), exactly like the earlier 59.4 fps soak. A valid cube run
  is ~28-30 fps. Nothing is known about command mode vs video mode for present rate; the "open" item above is void.
  Rule for cube runs: kill scrnsave.scr (or check it is not running) before and after the run, else the number is void.

## KMD 1.0.42.1 - panel.c (built, s10e-artifacts\kmd-42, MaliG76Wddm.sys sha256 2F034F08...)
- MwPanelStart (end of MwDisplayStart, after the re-stride): maps DSIM + DECON; only if DECON RUN_STATUS and DSIM is in
  command mode: GLOBAL_CONTROL |= OPERATION_MODE_CMD + SHADOW GLOBAL request (if not already set), then requires DSIM
  idle in >= 1 of 2000 MIPI_STATUS samples (~50 ms) -> PanelOk; INTSRC W1C all. Reads sboot's AOR (key, B1, unkey)
  -> PanelAor0, BrightnessOk if < 0x08D4. Registry: Parameters\PanelCmdMode (1), PanelBrightness (1). Logs Panel*.
- MwQueryInterface -> GUID_DEVINTERFACE_BRIGHTNESS_2 / _1 (only when BrightnessOk): levels 0..100 step 5;
  Set: AOR = 0x08D4 - (0x08D4 - A0) * (p/100)^2.2 (table x1000, linear between 5 % steps), F0 5A 5A / B1 / F7 03 /
  F0 A5 A5; caps 0 (no smooth/adaptive); SetBrightnessState(smooth) NOT_SUPPORTED; backlight optimisation accepted
  (no-op), reduction 0. Any write/read timeout -> PanelOk FALSE for the boot (no reset/retry), logged PanelFail*.
- MwPanelStop (MwDisplayStop): restores A0, unmaps. Known limit: a driver restart without a reboot reads the current
  (possibly dimmed) AOR as A0.
- DSIM offsets/bits in MaliG76Wddm.h are marked D10 (exercised on the device), AOR 0x08D4 stays UNVERIFIED as luminance.
- **KMD 1.0.42.1 on the device (16:5x boot):** PanelDeconGlobal 0x1011 -> PanelDeconGlobalAfter 0x1111 (PanelShadowMs 2),
  PanelIdleOf2000 0x198 (408 = 20 % idle), BrightnessQuery 0x00580202 (v2 GUID, Version 2, Size 0x58) -> offered;
  Windows applied its stored level at once: BrightnessLast 0x4b042f00 (75 % -> AOR 0x042F, ok).
  WmiMonitorBrightness: CurrentBrightness 75, Active True (= Settings/quick-settings slider exists).
  WmiSetBrightness 100 / 30 / 75 -> CurrentBrightness 100 / 30 / 75, BrightnessLast AOR 0x0014 / 0x0835 / 0x042F, all ok.
- Review finding: PanelAor0 and PanelStep 0 are missing from the registry log - they are written while PanelLock (a
  FAST_MUTEX, APC_LEVEL) is held and MwLog only writes at PASSIVE. Follow-up: log after the release (1.0.42.2).
- KMD 1.0.42.2 (s10e-artifacts\kmd-42b, MaliG76Wddm.sys sha256 3CED0E5B...): every MwLog moved out of PanelLock
  (PanelAor0, PanelStep, PanelRxUnexpected, PanelFail* after release). Built; not deployed yet. MILESTONES G14 added.
- KMD 1.0.42.2 on the device (boot 2 with brightness): PanelShadowMs 1, GlobalAfter 0x1111, IdleOf2000 408, PanelAor0
  0x14, PanelRxUnexpected 0, PanelStep 0, BrightnessQuery v2, BrightnessLast 75 % / 0x042F; WMI CurrentBrightness 75.
- Idle dim (Windows' own, through the same interface): powercfg SUB_VIDEO "Dim display after" (17aaa29b-..., was
  hidden: -attributes -ATTRIB_HIDE) = 60 s AC/DC, "Dimmed display brightness" (f1fbfde2-...) = 30 % AC/DC, setactive.
  Blank screen saver stays at 120 s. Check pending: BrightnessLast after >60 s without input.
- Idle dim did not fire with display-off = never: 85 s idle (setting applied while already idle) and again 30/50/~80 s
  after a SendInput nudge (tools: C:\S10eTmp\nudge.ps1 via runuser2 - SendInput returns 0 while scrnsave.scr owns the
  desktop, 2 after taskkill) -> BrightnessLast stayed 75 %. MS Learn (Automatic display dimming, dn613880): the dim
  timeout "can be extended up to the display power-off time-out" - dimming is tied to a display-off timeout.
  Provisioning policies (HKLM\SOFTWARE\Policies\Microsoft\Power\PowerSettings) pin sleep 29f6c1db, display-off 3c0bc021,
  console-lock display-off 8ec4b3a5, hibernate 9d7815a6 all to 0. Changed only 3c0bc021 AC/DC to 600 s (display off
  after 10 min). KMD MwSetPowerState is a no-op returning success, so a display-off request leaves the panel lit
  (panel off via DCS 28/10 = later work). Re-test pending.
- Idle dim, after a reboot with display-off policy 600 s: nudge 17:05:38, CurrentBrightness 75 at +15..+114 s - no dim.
  Platform role 8 (PlatformRoleSlate, PowerDeterminePlatformRoleEx(2)), Win32_Battery 1, PowerOnline True: the
  documented preconditions hold. Not found yet: whether Windows ever enters the dimmed console display state here (a
  GUID_CONSOLE_DISPLAY_STATE listener would show it), or whether SendInput from a task counts as user input for it.
  **Parked** (user: idle dim is a later item). Display-off policy and scheme VIDEOIDLE put back to 0 (a display-off
  would run MwSetPowerState/monitor power paths never tested with the GPU scan-out). VIDEODIM 60 s / 30 % left set
  (inert while it does not fire).

## KMD 1.0.43.1 - brightness fade (user request 2026-10-14 ~17:25: "like a light switch you turn", 5 % per tick)
- SetBrightness only stores the target and signals RampEvent; PanelRampThread (system thread) walks RampLevel toward
  it MW_PANEL_RAMP_STEP 5 levels per MW_PANEL_RAMP_TICK_MS 16 ms through the same (p/100)^2.2 curve; a new target
  mid-fade redirects it. Key F0 5A 5A open once per fade; each step queues B1 + F7 03 and waits once (DsimQueueLong /
  PanelWriteAor). Logs per fade: BrightnessLast (level/AOR/fail), BrightnessRamp (steps << 16 | ms). Thread stopped in
  MwPanelStop before the A0 restore. Built s10e-artifacts\kmd-43 (MaliG76Wddm.sys B0CA2CF3...), not deployed yet.
- 1.0.43.1 on the device: WmiSetBrightness 0 -> 100: BrightnessRamp 0x140151 (20 steps, 337 ms); 100 -> 80: 0x4003a
  (4 steps, 58 ms). User session 17:22-17:23: slider 30/100/75/80/10/90/0, fades 7..17 steps, 110-325 ms, no failure.
  (The "level 0 at boot" seen earlier was the user's last slider position.) User: "its fine by me ... as good as done".
  MILESTONES G14 -> done.

## Boot brightness + debug cleanup (2026-10-14 ~19:30; user: bootloader screen "super bright", "remove all the prints ...
## the white lines, all the debugging modifications we dont need anymore")
- UEFI SimpleFramebufferDxe: FbPanelCommandMode before the first present (DECON GLOBAL_CONTROL |= CMD, only from
  running + video mode with DSIM in command mode; committed by that present's global shadow request), then after it
  FbPanelBootBrightness: DSIM idle check, F0 5A 5A + read B1 (sboot AOR) -> hand-off word [7] 0x8FE0F01C = 'A0'<<16|AOR,
  level from word [6] 0x8FE0F018 'BR'<<16|level (written by the KMD) else 50 %, same (p/100)^2.2 curve -> B1 + F7 03,
  F0 A5 A5. Logs "FbPanel: ..." in the RAM log only.
- KMD 1.0.44.1 (kmd-44, MaliG76Wddm.sys 4FAE4048...): maps the hand-off page; PanelAor0 = word [7] when tagged (else the
  B1 read); start level = the step whose AOR the panel shows now; the fade thread writes word [6] after every fade.
  Logs PanelHandoff / PanelAorNow / PanelStartLevel.
- Cleanup: UEFI progress bar off (BEYOND0LTE_FB_PROGRESS 0; exception text and RAM log kept); the D9 FbDsimSnapshot
  (~250 ms per boot) removed; S10eDiag boot DSIM snapshot (D8, 50 ms) reverted (S10eDiag.sys fa76c6af...);
  MALIKM_TGSI_DIR removed (UMD tracing off at the next boot). Shim diagnostics were already off (2026-10-11).
- Image s10e-artifacts\boot116-gpuon-nowdt-br1.img sha256 2572a7e1... = HEAD + build-only WINDOWS_WATCHDOG 0 /
  GPU_POWER_ON 1 (reverted after the build).
- Boot screen (user-approved mock-up, 19:40): firmware/tools/bootsplash/make_splash.py renders black + "EXYNOS WOA" +
  NTDEV logo (bootsplash/ntdev.png, the user's file) + "Powered by [Win11 logo] Windows" (Segoe UI), row-RLE coded
  into SimpleFramebufferDxe/BootSplash.h (137 952 B; FVMAIN_COMPACT 0xbe000 -> 0xc5000 after LZMA). Entry: command
  mode, FbDrawBootSplash (bounds-checked decode into the GOP fb), FbDeconPresent, FbPanelBootBrightness.
  Image s10e-artifacts\boot116-gpuon-nowdt-splash1.img (supersedes the unflashed br1 image) = HEAD + build-only
  WINDOWS_WATCHDOG 0 / GPU_POWER_ON 1.
- Clean boot screen, round 2 (user photo 19:5x: five white 100-row bands over the Samsung splash, "Press ESCAPE for
  boot options", a loading bar): bands = BEYOND0LTE_CHECKPOINT (B39 bring-up, rows 1300/1420/1540/1660/1780) -> now
  BEYOND0LTE_CHECKPOINTS 0; the bar + prompt = ArmPkg PlatformBootManagerWaitCallback/BootLogoUpdateProgress during
  PcdPlatformBootTimeOut 3 -> 0 (also 3 s faster); GraphicsConsoleDxe removed (it cleared the panel and printed BDS
  text over the boot screen; ConOut has no device now, the RAM log keeps DEBUG output).
  Image s10e-artifacts\boot116-gpuon-nowdt-clean1.img sha256 d67d5629... (FVMAIN_COMPACT 0xc4000).
  Rollback if it does not boot: flash boot116-gpuon-nowdt-splash1.img (42317618...) from TWRP.
- Earliest cover for sboot's "not running Samsung's official software" warning (user, 19:5x): the old checkpoint bands
  showed over it, i.e. sboot leaves DECON scanning 0xCA000000 in video mode, so plain stores show at once. Boot shim:
  right after PROGRESS 0x51, stp xzr loop over 0x964B00 B (1080 x 2280 x 4) = black (shim 512 -> 544 B). SEC
  ArmPlatformInitialize (primary core, MMU off): Beyond0LteEarlySplash draws the BootSplash.h literals only (black
  already there). SimpleFramebufferDxe later redraws + pushes in command mode + boot brightness as before. sboot's own
  warning time before it jumps to us is out of our hands. Image boot116-gpuon-nowdt-clean2.img sha256 8d4cf769...
  (FVMAIN_COMPACT 0xe5000: the splash now also lives uncompressed in SEC). Rollback: splash1 (42317618...).
- clean2 result (user): the warning stayed until the DXE splash (opaque) replaced it. Cause: the shim filled
  0x00000000 = alpha 0 and DECON blends per pixel, so sboot's warning (on another window) showed through; the old
  checkpoint bands were 0xFFFFFFFF (opaque). clean2 boot otherwise fine: FbPanel boot level 15 % "from Windows" (AOR
  0x08B3) - the hand-off works. clean3: shim fill 0xFF000000 per pixel. Image boot116-gpuon-nowdt-clean3.img d85b7391...
