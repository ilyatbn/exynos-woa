# D8 jack research - UAIF0 -> CS47L93 AIF1 -> HPOUT2 + jack detect (2026-10-16)

Research-only notes (no code, no phone). Every value carries a source; anything without one is UNVERIFIED.
Kernel = LineageOS/android_kernel_samsung_exynos9820 @ lineage-22.2 (raw.githubusercontent.com), abbreviated `LOS:`.
Stock DT = `C:\Users\ilya\code\Dumps\extracted\dts\dtbo.img.6.dts` (model "Samsung BEYOND0LTE EUR OPEN 25"; the
seven dtbo overlays 0..6 are revs 17..25), abbreviated `dtbo6:`.

## Progress log
- step 1: stock dtbo sound/codec nodes read (below, raw facts).

## Raw DT facts (dtbo6)
- sound node `compatible = "samsung,beyond-madera"` (dtbo6:1484), `clocks = <.. 0x385>` clock-names "xclkout" (1485-1486)
- `cirrus,sysclk = <0x1 0x4 0x5dc0000>` (1487)    -> 98 304 000 Hz
- `cirrus,asyncclk = <0x2 0x5 0x5dc0000>` (1488)  -> 98 304 000 Hz
- `cirrus,dspclk = <0x8 0x4 0x8ca0000>` (1489)    -> 147 456 000 Hz
- `cirrus,outclk = <0x9 0x1 0x0>` (1490)
- `cirrus,fll1-refclk = <0x1 0x0 0x18cba80>` (1491) -> 0x18CBA80 = 26 000 000 Hz (NOT 25 MHz as notes/d8-audio.md §1.2 says)
- `cirrus,fll2-refclk = <0x2 0x8 0x18cba80>` (1492)
- `cirrus,ampclk = <0x0 0x0 0x5dc000>` (1493)     -> 6 144 000 Hz
- dai link `uaif0 { format = "i2s"; cpu = ABOX UAIF0; codec = <cs47l93(0x21) 0x0> }` (dtbo6:1741-1749) -> codec DAI 0 = AIF1;
  no bitclock-master / frame-master property.
- `uaif1 { format = "dsp_a"; bitclock-inversion; codec = cs35l40 x2 }` (1750-1758)
- `uaif2 { format = "i2s"; codec = <cs47l93 0x2> }` (1759-1767) -> AIF3
- codec node `cs47l93@0` on the spi fragment@78 (dtbo6:5112-5240): spi-max-frequency 0x17d7840 = 25 MHz,
  `cirrus,out-mono = <0 0 0 1 0 0>`, `cirrus,inmode`, `cirrus,gpsw = <3 0>`, madera-pinctrl defaults: group aif1
  function aif1 bias-bus-hold, aif3 likewise (5143-5170).
- `cirrus,accdet { acc@1 { reg = <0x2>; ... } }` (dtbo6:5200-5226): micd-bias-start-time 1, micd-rate 7,
  micd-detect-debounce-ms 300, micd-clamp-mode 8, jd-use-jd2, hs-mic 1, init-delay-ms 30, micd-dbtime 1,
  micd-timeout-ms 1000, micd-force-micbias, hpdet-short-circuit-imp 2, hpdet-ext-res 3300,
  micd-ranges <116 226 173 582 321 115 752 114 1257 217>, micd-software-compare, micd-open-circuit-declare 1,
  jd-wake-time 5000, moisture-pin 7, moisture-imp 2000, moisture-debounce 100, micd-configs <0 1 4 0 1>,
  hpd-pins <2 5 0 0>.

- step 2: fetched LOS beyond_madera.c, madera.c/.h, cs47l92.c, cs47l92-tables.c, registers.h, extcon-madera.c,
  abox_if.c, abox_cmpnt_v20.c, abox_soc.c/.h, soc-core.c, cmucal-*.c, exynos9820-pinctrl.dtsi into the scratchpad.

## Q1 - link format / clocking (stock design)

DT clock tuples are `<id source rate>` (`LOS: sound/soc/samsung/beyond_madera.c:1808-1830` read_clk_conf: index0 = id,
index1 = source, index2 = rate). Decoding with `LOS: sound/soc/codecs/madera.h:21-66`
(FLL1_REFCLK=1, FLL2_REFCLK=2; FLL_SRC_MCLK1=0, MCLK2=1, AIF1BCLK=8, AIF1LRCLK=12; CLK_SYSCLK_1=1, ASYNCCLK_1=2,
CLK_SYSCLK_2=5, DSPCLK=8, OUTCLK=9; CLK_SRC_MCLK1=0, MCLK2=1, FLL1=4, FLL2=5, AIF1BCLK=8):
- sysclk    = <1 4 98304000>  -> SYSCLK_1 from FLL1, 98.304 MHz            VERIFIED (dtbo6:1487 + madera.h:45,57)
- asyncclk  = <2 5 98304000>  -> ASYNCCLK_1 from FLL2, 98.304 MHz          VERIFIED (dtbo6:1488 + madera.h:46,58)
- dspclk    = <8 4 147456000> -> DSPCLK from FLL1                          VERIFIED (dtbo6:1489)
- outclk    = <9 1 0>         -> OUTCLK source value 1 (meaning of 1 for OUTCLK not decoded; UNVERIFIED)
- fll1-refclk = <1 0 26000000> -> FLL1 ref = **MCLK1 at 26 MHz** (the AP `xclkout` clock, `clocks=<.. 0x385>`
  clock-names "xclkout", dtbo6:1485-1486; enabled in set_bias_level STANDBY, beyond_madera.c:348-362)  VERIFIED
- fll2-refclk = <2 8 26000000> -> FLL2 ref = **AIF1BCLK** (MADERA_FLL_SRC_AIF1BCLK = 8, madera.h:36); the DT rate is
  overwritten per stream: `drvdata->fll2_refclk.rate = rate * params_width(params) * 2` (beyond_madera.c:167-168)
  and ASYNCCLK rate = 98304000 for 48k-family rates (beyond_madera.c:170-173, :57).  VERIFIED
- prepare() for UAIF0: un-tristate the ABOX UAIF (bclk on, `snd_soc_dai_set_tristate(cpu_dai,0)`), then
  set_sysclk(ASYNCCLK_1, FLL2, 98.304M) and set_pll(FLL2_REFCLK, AIF1BCLK, BCLK, 98.304M)
  (beyond_madera.c:209-231). shutdown(): OUT2L/R_ENA cleared in OUTPUT_ENABLES_1, 1 ms, ASYNCCLK + FLL2 off,
  UAIF tristated (beyond_madera.c:254-279).  VERIFIED
- uaif0_init: sets SYSCLK/ASYNCCLK/DSPCLK/OUTCLK, then `snd_soc_dai_set_sysclk(codec_dai /*AIF1*/, ASYNCCLK_1)`
  -> **AIF1 runs in the ASYNCCLK domain** (beyond_madera.c:623-663). In madera_hw_params_rate that writes
  ASYNC_SAMPLE_RATE_1 (0x113) = sr code and AIF1_RATE_CTRL[15:11] = 8 (madera.c, case MADERA_CLK_ASYNCCLK_1).  VERIFIED
- dai_fmt: `link->dai_fmt = snd_soc_of_parse_daifmt(dai, NULL, NULL, NULL)` (beyond_madera.c:2014) with DT
  `format = "i2s"` and no bitclock-master/frame-master/bitclock-inversion (dtbo6:1741-1749) -> SND_SOC_DAIFMT_I2S |
  NB_NF | **CBS_CFS** (default branch, `LOS: sound/soc/soc-core.c:4175-4188`). The flags are from the codec's view:
  **codec = BCLK+LRCLK slave, ABOX UAIF0 = master.**  VERIFIED
- Stream params come from the ABOX BE fixup (`be_hw_params_fixup = abox_hw_params_fixup_helper`,
  beyond_madera.c:1165) fed by mixer_paths `ABOX UAIF0 Width = 24`, `Channel = 2`, `Rate = 48000`
  (`Dumps/extracted/vendor/etc/mixer_paths.xml:53-55`).  VERIFIED (see Q2 for what width 24 means on the wire)

**How stock keeps the domains synchronous:** ABOX (AP audio PLL) is master of BCLK/LRCLK; the codec's AIF1 is
clocked from ASYNCCLK, and ASYNCCLK = FLL2 locked to AIF1BCLK -> AIF1 is exactly synchronous to the ABOX clock.
SYSCLK (FLL1 from MCLK1 = AP xclkout 26 MHz) is a separate domain. The output path (OUT2 mixer/DAC) runs at
SYSCLK's rate; the codec does async<->sync crossing for AIF1RX -> OUT2 itself? -> see Q1 note below.

### Q1 cont. - the output side is ALSO in the ASYNCCLK domain (stock)
- stock `cirrus,outclk = <9 1 0>` = OUTCLK source 1 = **MADERA_OUTCLK_ASYNCCLK** (`LOS: madera.h:68-72`:
  SYSCLK 0, ASYNCCLK 1, MCLK1 4, MCLK2 5, MCLK3 6); madera_set_outclk writes OUTPUT_RATE_1 (0x408)
  OUT_CLK_SRC[2:0] = 1 (`LOS: madera.c:2943-2958`; masks `registers.h:4337-4348`: OUT_RATE [15:11],
  OUT_EXT_CLK_DIV [5:4], OUT_CLK_SRC [2:0]).  VERIFIED
- mixer_paths default `<ctl name="Output Rate 1" value="ASYNCCLK rate 1" />` (mixer_paths.xml:177) ->
  OUTPUT_RATE_1.OUT_RATE[15:11] = 8 (`madera_rate_val = {0,1,2,8,9}`, `LOS: madera.c:1791-1818`; control
  defined `LOS: cs47l92.c:503`).  VERIFIED
- => stock runs AIF1, the OUT1-6 mixers and the DACs all at ASYNC_SAMPLE_RATE_1 on ASYNCCLK (FLL2 <- AIF1BCLK).
  No ASRC in the headset playback path (mixer_paths dev-headset has no ASRC/ISRC controls, :631-635). SYSCLK
  (FLL1) still has to run: "A connection to SYSCLK is always required" (`LOS: madera.c` madera_dai_set_sysclk).
- cs47l92 reset default OUTPUT_RATE_1 = 0x0040 (`LOS: drivers/mfd/cs47l92-tables.c:263`) -> OUT_RATE 0 (SYSCLK rate 1),
  OUT_CLK_SRC 0 (SYSCLK); bit6 = CP_DAC_MODE ("DAC High Performance Mode Switch", `LOS: cs47l92.c:377`).
  Our J2 tone ran in that default SYSCLK domain.  VERIFIED (defaults)

### Q1 - wire format numbers (stock, 48 kHz)
- ABOX UAIF0 config Width 24 / Channel 2 / Rate 48000 (mixer_paths.xml:53-55) -> fixup forces
  SNDRV_PCM_FORMAT_S24 (= S24_LE, 24 valid bits in a 32-bit container) (`LOS: abox_if.c:837-845`).
- ABOX BCLK = `rate * channels * width` = 48000*2*24 = **2.304 MHz (48 fs, 24-bit slots)** (`LOS: abox_if.c:366`
  clk_set_rate(clk_bclk, rate*channels*width), width = params_width = 24).  VERIFIED
- machine FLL2 reference = `rate * params_width * 2` = **2.304 MHz** - matches the ABOX BCLK (beyond_madera.c:168). VERIFIED
- madera_hw_params on the codec side uses slotw = snd_pcm_format_physical_width = 32 -> bclk_target 3.072 MHz ->
  writes AIF1_BCLK_CTRL.BCLK_FREQ = index 13 (3072000 in madera_48k_bclk_rates) and AIF1_RX_BCLK_RATE = 64,
  FRAME_CTRL_1/2 = (24<<8)|32 = 0x1820 (`LOS: madera.c` madera_hw_params, rates table after madera_set_fmt).
  So in stock the codec registers say 64 BCLK/frame while the wire carries 48. This works only because the codec
  is BCLK/LRCLK slave (BCLK_FREQ/RX_BCPF are master-mode settings) - INFERENCE, UNVERIFIED against the CS47L93
  datasheet. **Recommendation for our port: use 32-bit slots on both sides (ABOX width 32, BCLK 3.072 MHz = 64 fs)
  so ABOX BCLK, codec BCLK_FREQ/BCPF/slot length and the FLL2 reference all agree.**

## Q2 - ABOX side (UAIF0)

### Registers (ABOX SFR base = 0x18C50000; UAIF0 node `reg = <0x18c50500 0x10>`, `LOS: exynos9820.dts:1507-1517`)
- `ABOX_UAIF_BASE 0x0500`, `ABOX_UAIF_ITV 0x0010`, CTRL0 = +0x0, CTRL1 = +0x4, STATUS = +0xC
  (`LOS: sound/soc/samsung/abox/abox_soc.h:305-309`) -> **UAIF0 CTRL0 = 0x500, CTRL1 = 0x504, STATUS = 0x50C**.  VERIFIED
- CTRL0 fields (abox_soc.h:310-330): START_FIFO_DIFF_MIC [31:28], START_FIFO_DIFF_SPK [27:24], DATA_MODE [4],
  IRQ_MODE [3], MODE [2] (1 = ABOX master for CBS_CFS, 0 = slave for CBM_CFM, `LOS: abox_if.c:296-303`),
  MIC_ENABLE [1], SPK_ENABLE [0] (set on trigger START, abox_if.c:436-441; cleared in shutdown after 600-1200 us
  FIFO flush, abox_if.c:476-482).
- CTRL1 fields (abox_soc.h:331-355): FORMAT [28:24], BCLK_POLARITY [23], WS_MODE [22], WS_POLAR [21],
  SLOT_MAX [20:18], SBIT_MAX [17:12], VALID_STR [11:6], VALID_END [5:0].
- set_fmt (abox_if.c:248-314): I2S -> WS_MODE 0 (DSP_A -> 1); NB_NF -> BCLK_POLARITY 1, WS_POLAR 0
  (IB_NF -> BCLK_POLARITY 0); CBS_CFS -> MODE 1.
- hw_params (abox_if.c:346-417): SBIT_MAX = width-1; I2S -> VALID_STR = VALID_END = 0 (else width-1);
  SLOT_MAX = channels-1; FORMAT = abox_get_format(width, channels) = (channels-1) | {16:1,24:2,32:3}<<3
  (`LOS: abox.h:419-440`).
- **Formula check against our working UAIF1 value** (DSP_A, IB_NF, 4 ch, 32 bit): FORMAT 0x1B, POL 0, WS_MODE 1,
  SLOT_MAX 3, SBIT_MAX 31, VALID 31/31 -> 0x1B000000|0x400000|0xC0000|0x1F000|0x7C0|0x1F = **0x1B4DF7DF** = the value
  in AboxHw.c:42 that plays on the speakers. Formula VERIFIED.
- **UAIF0 CTRL1, I2S NB_NF 2 ch:**
  - width 24 (stock): FORMAT 0x11, POL 1, SLOT_MAX 1, SBIT_MAX 23 -> 0x11000000|0x800000|0x40000|0x17000 = **0x11857000**
  - width 32 (recommended): FORMAT 0x19, POL 1, SLOT_MAX 1, SBIT_MAX 31 -> **0x1985F000**   (computed, formula VERIFIED)
- **UAIF0 CTRL0** = 0x01000014 (ABOX master) assumes UAIF0 has the same reset value 0x01000010 as UAIF1
  (AboxHw.c:39 comment) - UNVERIFIED for UAIF0: read 0x18C50500 before writing.
- ROUTE_CTRL0 (0x0040): "ABOX UAIF0 SPK" mux = `ABOX_ROUTE_CTRL0`, shift `ABOX_ROUTE_UAIF_SPK_L(0)` =
  BASE 0 + 0*ITV 4 -> **bits [3:0]** (abox_soc.h:73-77; `LOS: abox_cmpnt_v20.c:3360-3361`);
  values `uaif_spkx_texts = {RESERVED, SIFS0, SIFS1, SIFS2, SIFS3, SIFS4, ...12: SIFMS}` (abox_cmpnt_v20.c:3354-3359)
  -> **SIFS0 = 1, SIFS1 = 2**. UAIF1 is [7:4] (=2 for SIFS1 in AboxHw.c:33, consistent).  VERIFIED
- Stock headset routing uses SIFS0 (mixer_paths `route-sifs0-to-uaif0`, :249-250); speaker route uses SIFS1.
  For our driver the shortest path is ROUTE_CTRL0[3:0] = 2 (UAIF0 <- SIFS1, the SIFS RDMA0 already feeds) -
  INFERENCE: whether SIFS1 can feed a 2-ch UAIF0 and a 4-ch UAIF1 at the same time is UNVERIFIED (switch exclusively:
  either [3:0]=2 or [7:4]=2).
- SPUS OUTx select enum (if SIFS0 is wanted): `spus_outx_texts` differs by ABOX version (<2.0.1: SIFS1=0,SIFS0=1,...;
  >=2.0.1: SIFS1, SIFS0, SIFS2, RESERVED, SIFS3, RESERVED, SIFS4) (abox_cmpnt_v20.c:3205-3213) - our driver already
  works with SIFS1, no change needed.

### CMU_AUD (base 0x18C00000, `LOS: drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c:6`)
- MUX_CLK_AUD_UAIF0 = **0x100C**, SELECT bit0, BUSY bit16 (cmucal-sfr.c:94, :2851-2853); parents
  {0: DIV_CLK_AUD_UAIF0, 1: CLKIO_AUD_UAIF0 (external BCLK pin)} (`cmucal-node.c:182-185`).  VERIFIED
  -> keep 0 for ABOX master; 1 would be the slave option.
- DIV_CLK_AUD_UAIF0 = **0x182C**, DIVRATIO [8:0], BUSY bit16 (cmucal-sfr.c:357, :3398-3400); parent DIV_CLK_AUD_AUDIF
  (cmucal-node.c:1247).  VERIFIED
- GATE ABOX BCLK_UAIF0 = **0x2020** (CG_VAL bit21, MANUAL bit20) (cmucal-sfr.c:548, :3967-3969); RSTNSYNC_CLK_AUD_UAIF0
  = **0x20A4** (cmucal-sfr.c:560). UAIF1 equivalents 0x1010/0x1830/0x2024/0x20A8 match AboxHw.c:61,167-172.  VERIFIED
- Stock clock: uaif-max-div = 0x200 (dt.img.0.dts:5339) -> >32 branch -> AUDIF = 384000*96*2 = 73.728 MHz
  (`LOS: abox.c:700-711`), UAIF0 BCLK 2.304 MHz = AUDIF/32. Our driver keeps sboot's AUDIF 589.824 MHz
  (AboxHw.c:64) -> **DIV_UAIF0 = 191 for 3.072 MHz (32-bit slots)** or 255 for 2.304 MHz (24-bit slots)
  (computed from that AUDIF; AUDIF value itself is from the UAIF1 measurement, AboxHw.c:64).
- Q-channel: `abox_uaif_set_tristate` -> abox_disable_qchannel(ABOX_BCLK_UAIF0) (abox_if.c:316-344); UAIF1 used
  ABOX_QCHANNEL_DISABLE bit3 (AboxHw.c:31). Bit for UAIF0 = enum qchannel ABOX_BCLK_UAIF0 value - see below.

### GPIO_AUD (pinctrl@18C60000 = EXYNOS_PINCTRL_AUD_BASE, Exynos9820.h:859)
- `aud_i2s0_bus`: pins gpb0-1..gpb0-4, function 2, pud 1 (`LOS: arch/arm64/boot/dts/exynos/exynos9820-pinctrl.dtsi:181-185`);
  idle = function 0 (:187-191); used by abox_uaif_0 pinctrl-0/1 (`exynos9820.dts:1514-1516`). gpb0-0 = aud_codec_mclk
  (fn 2, :169-173) - do not touch. gpb0 is the first bank (:145) -> CON at +0x000 (gpb1 at +0x20 per Exynos9820.h:859).
  -> **gpb0 CON (0x18C60000) RMW mask 0x000FFFF0 value 0x00022220**.  VERIFIED (bank offset 0 = inference from bank
  order + gpb1 @0x20; pud register layout same as gpb1 PUD @+0x8 inside the bank, AboxHw.c:175)
- Q-channel bit: `enum qchannel {CCLK_CA7, ACLK, BCLK_UAIF0, BCLK_UAIF1, ...}` (`LOS: abox.h:157-167`) -> UAIF0 = 2;
  abox_disable_qchannel updates ABOX_QCHANNEL_DISABLE (0x0038) bit `ABOX_QCHANNEL_DISABLE_L(clk)` (abox.c:241-247).
  UAIF1 = 3 = bit3 matches AboxHw.c:31 -> **UAIF0 keep-alive = QCHANNEL_DISABLE bit2 (0x4)**.  VERIFIED

## Q3 - codec AIF1 + HP path (CS47L92/93 = madera, codec DAI "AIF1" base = MADERA_AIF1_BCLK_CTRL, `LOS: cs47l92.c:1762`)

### Addresses (`LOS: include/linux/mfd/madera/registers.h:322-347`) and reset defaults (`LOS: drivers/mfd/cs47l92-tables.c:302-327`)
| reg | addr | default | fields (registers.h) |
|---|---|---|---|
| AIF1_BCLK_CTRL | 0x500 | 0x000C | BCLK_INV 0x80, BCLK_MSTR 0x20, BCLK_FREQ [4:0] (:4999-5011) |
| AIF1_TX_PIN_CTRL | 0x501 | 0x0000 | TX_LRCLK_INV 0x4, TX_LRCLK_MSTR 0x1 (:5024-5032) |
| AIF1_RX_PIN_CTRL | 0x502 | 0x0000 | RX_LRCLK_INV 0x4, RX_LRCLK_MSTR 0x1 (:5036-5046) |
| AIF1_RATE_CTRL | 0x503 | 0x0000 | AIF1_RATE [15:11], AIF1_TRI 0x40 (:5052-5055) |
| AIF1_FORMAT | 0x504 | 0x0000 | AIF1_FMT [2:0]: DSP_A 0, DSP_B 1, I2S 2 (fmt codes `LOS: madera.c:141-143`) |
| AIF1_RX_BCLK_RATE | 0x506 | 0x0040 | RX_BCPF [12:0] (:5066) |
| AIF1_FRAME_CTRL_1 (TX) | 0x507 | 0x1818 | TX_WL [13:8], TX_SLOT_LEN [7:0] (:5071-5074) |
| AIF1_FRAME_CTRL_2 (RX) | 0x508 | 0x1818 | RX_WL [13:8], RX_SLOT_LEN [7:0] (:5079-5082) |
| AIF1_FRAME_CTRL_11 (RX1 slot) | 0x511 | 0x0000 | RX1_SLOT [5:0] (:5127) |
| AIF1_FRAME_CTRL_12 (RX2 slot) | 0x512 | 0x0001 | RX2_SLOT [5:0] (:5132) |
| AIF1_TX_ENABLES | 0x519 | 0x0000 | |
| AIF1_RX_ENABLES | 0x51A | 0x0000 | RX2_ENA 0x2, RX1_ENA 0x1 (:5225-5229) |
VERIFIED (addresses, masks, defaults).

### What stock writes (fmt I2S, CBS_CFS, NB_NF; S24_LE stereo 48 kHz; AIF1 on ASYNCCLK_1)
- set_fmt (`LOS: madera.c:3077-3165`): BCLK_CTRL INV/MSTR = 0; TX/RX_PIN_CTRL LRCLK INV/MSTR = 0; FORMAT FMT = 2 (I2S).
- hw_params (`LOS: madera.c` madera_hw_params, ~:3500-3620): bclk index = first `madera_48k_bclk_rates[i] >= slotw*ch*rate`
  -> 3 072 000 = **index 13** (table after madera_set_fmt: -1, 48k, 64k, 96k, 128k, 192k, 256k, 384k, 512k, 768k,
  1.024M, 1.536M, 2.048M, **3.072M**, ...); RX_BCPF = 3072000/48000 = **64**; FRAME_CTRL_1/2 = (dataw<<8)|slotw.
  hw_params_rate: ASYNC_SAMPLE_RATE_1 (0x113) [4:0] = sr index of 48000 in `madera_sr_vals` = **3**
  (`{0,12000,24000,48000,...}`, `LOS: madera.c:3216-3220`), AIF1_RATE_CTRL[15:11] = **8** (ASYNCCLK rate 1).
  TX/RX enables are saved, zeroed and restored around the reconfig.
- RX slots: madera_set_tdm_slot not called by the machine driver for UAIF0 -> slots stay at defaults RX1 = 0,
  RX2 = 1 (cs47l92-tables.c:318-319).  VERIFIED (no set_tdm_slot in beyond_madera.c for UAIF0)
- AIF1RX1/2_ENA: DAPM AIF_IN widgets on AIF1_RX_ENABLES bits 0/1 (`LOS: cs47l92.c:1026-1029`), powered when
  AIF1 Playback is active and the HPOUT2 mixers select them.
- **Values for our port (32-bit slots recommended, 24-bit data):** 0x500 = 0x000D (BCLK_FREQ 13, slave, NB),
  0x501 = 0x0000, 0x502 = 0x0000, 0x504 = 0x0002, 0x506 = 0x0040 (64), 0x507/0x508 = 0x1820 (WL 24, slot 32)
  [or 0x2020 for 32-bit data], 0x511 = 0, 0x512 = 1, 0x113 = 0x0003, 0x503 = 0x4000 (rate 8 << 11; TRI 0),
  0x51A = 0x0003. Computed from the code paths above, VERIFIED-by-code (not yet device-tested).
  For stock 24-bit slots (2.304 MHz BCLK) the codec code would still write BCLK_FREQ 13 / BCPF 64 (see Q1).

### Codec pins for AIF1 (must do - reset default is GPIO, not AIF)
- AIF1 = CS47L92 pins {4,5,6,7} (0-based) = GPIO5..GPIO8 (`LOS: drivers/pinctrl/cirrus/pinctrl-cs47l92.c:29,35`).
- madera pinctrl "aif1" function = 0x000 -> writes GPn_FN [9:0] = 0 in `MADERA_GPIO1_CTRL_1 + 2*pin`
  (`LOS: drivers/pinctrl/cirrus/pinctrl-madera.c:125-130, 610-660`; GPIO1_CTRL_1 = 0x1700, GP1_FN_MASK 0x03FF,
  registers.h:1359, :7292) -> **0x1708, 0x170A, 0x170C, 0x170E: FN = 0**.
- Defaults are 0x2001 (FN = 1 = GPIO) for GPIO5-8 CTRL_1 and 0xF000 for CTRL_2 (cs47l92-tables.c:1058-1065), so
  without this write AIF1 is not connected. DT asks bias-bus-hold (dtbo6 aif1 group) = GP_PU|GP_PD (0x4000|0x2000)
  in CTRL_2 (pinctrl-madera.c:835-838; registers.h:7301-7305) - already set by the 0xF000 default.
  -> write 0x1708/0x170A/0x170C/0x170E = 0x2000 (keeps bit13 of the default, FN 0).  VERIFIED (code + defaults)

### Mixer / HPOUT2
- `madera_mixer_texts[]` "AIF1RX1"/"AIF1RX2" <-> `madera_mixer_values[]` **0x20 / 0x21** (`LOS: madera.c:1422-1445,
  1574-1597`; "Tone Generator 1" = 0x04 consistent with J2).  VERIFIED
- HPOUT2L Input 1 = OUT2LMIX_INPUT_1_SOURCE **0x690**, Input 1 Volume = 0x691; HPOUT2R = **0x698** / 0x699
  (registers.h:467-475; controls cs47l92.c:430 via MADERA_MIXER_CONTROLS, volume range 0x20..0x50 at
  MADERA_MIXER_VOL_SHIFT, madera.h:194-206). Defaults 0x690/0x698 = 0x0000, 0x691/0x699 = **0x0080**
  (cs47l92-tables.c:424-433) = VOL field 0x40 if MIXER_VOL_SHIFT = 1 -> 0 dB (madera_mixer_tlv; shift UNVERIFIED
  here, but the J2 tone was audible with defaults).
- "HPOUT2 Digital Switch" = SOC_DOUBLE_R(DAC_DIGITAL_VOLUME_2L **0x419**, _2R **0x41D**, OUT2L_MUTE_SHIFT, 1, invert=1)
  (`LOS: cs47l92.c:449-450`) -> switch 1 = OUT2L_MUTE (0x0100, registers.h:4452) **cleared**. Volume = OUT2L_VOL [7:0]
  (registers.h:4456), "HPOUT2 Digital Volume" max 0xBF (cs47l92.c:459-461). Defaults 0x419/0x41D = **0x0180** =
  muted, vol 0x80 (0 dB) (cs47l92-tables.c:273, 277). Writes need OUT_VU (bit9 0x0200, standard madera VU bit -
  UNVERIFIED in this file) on the R register to latch both. -> unmute: 0x419 = 0x0080, 0x41D = 0x0280.
- Output enable: OUTPUT_ENABLES_1 0x400 OUT2L_ENA 0x0008, OUT2R_ENA 0x0004 (registers.h:4293-4297; stock shutdown
  clears exactly these, beyond_madera.c:262-263). J2 already proved our HP enable sequence (OUT_EN1 000C).
- Stock also sets "HPOUT2 DRE Switch"=1, "EDRE Switch"=1 by default (mixer_paths.xml:187-188) - optional.
- Corrections/closures for the two UNVERIFIED items above: MADERA_MIXER_VOL_SHIFT = 1 (`LOS: madera.h:75`),
  madera_mixer_tlv = DB_SCALE(-3200, 100) with control range min 0x20 (madera.c:1741, madera.h:194-206) -> default
  0x0080 = field 0x40 = **0 dB**. VU: OUT2L_VU = 0x0200 (registers.h:4448); cs47l92 sets `CS47L92_DIG_VU 0x0200`
  on all DAC_DIGITAL_VOLUME_1L..5R at probe (`LOS: cs47l92.c:2023-2034`) -> keep bit9 set in 0x419/0x41D:
  **unmuted 0 dB = 0x0280 in both**.  VERIFIED

## Q4 - jack detection (stock = codec-native, extcon-madera)

- Driver: `LOS: drivers/extcon/extcon-madera.c` (3851 lines), DT `cirrus,accdet { acc@1 { reg = <2>; ... } }`
  (dtbo6:5200-5226). `reg = <2>` = accessory output 2 = HPOUT2: hp_clamp disables
  `(OUT1L_ENA|OUT1R_ENA) << 2*(output-1)` = OUT2L/OUT2R (extcon-madera.c:870-876).  VERIFIED
- DT `cirrus,jd-use-jd2` + `cirrus,micd-clamp-mode = <8>`: clamp mode 8 = `MADERA_MICD_CLAMP_MODE_JD1L_JD2L`
  (extcon-madera.c:40-44); written to MICD_CLAMP_CONTROL **0x2C6** [3:0] (registers.h:206, :3707), plus
  INTERRUPT_DEBOUNCE_7 (0x1A06) MICD_CLAMP_DB 0x10 set (madera_extcon_set_micd_clamp_mode, extcon-madera.c:3250-3281).
  Reset default 0x2C6 = 0x0210 (cs47l92-tables.c:223) -> after the update 0x0218 (MICD_CLAMP_OVD bit4 stays as is,
  registers.h:3703).  VERIFIED
- With jd_use_jd2 the probe writes INTERRUPT_DEBOUNCE_7 |= JD1_DB|JD2_DB (0x0001|0x0004) and
  JACK_DETECT_ANALOGUE **0x2D3** |= JD1_ENA|JD2_ENA (0x0001|0x0002) (extcon-madera.c:3686-3699; registers.h:208,
  :3720-3724, :9024-9032; defaults 0x2D3 = 0, 0x1A06 = 0, cs47l92-tables.c:225, :1116), and requests the IRQs
  MICD_CLAMP_RISE / MICD_CLAMP_FALL (not JD1_RISE/FALL).  VERIFIED
- **Presence test** (madera_jack_present, extcon-madera.c:2436-2471): read IRQ1_RAW_STATUS_7 **0x1886**
  (registers.h:1511); with jd_use_jd2: `val &= MICD_CLAMP_RISE_STS1 (0x0010); present = (val == 0)`.
  -> **jack inserted <=> (RAW_STATUS_7 & 0x0010) == 0**; removed <=> bit set (clamp engaged).
  Bit map of RAW_STATUS_7 (registers.h:7952-7972): MICD_CLAMP_FALL 0x20, MICD_CLAMP_RISE 0x10, JD2_FALL 0x08,
  JD2_RISE 0x04, JD1_FALL 0x02, JD1_RISE 0x01.  VERIFIED
- Debounce handling on events: on insert the driver CLEARS MICD_CLAMP_DB|JD1_DB|JD2_DB in 0x1A06, on removal it
  SETS them again (extcon-madera.c:2718-2756) -> for polling, keep them set (debounced) - simplest.
- Prerequisite clock: madera-core always enables the 32 kHz clock from MCLK2 at probe: CLOCK_32K_1 (0x100)
  CLK_32K_ENA (0x40) | src MCLK2 (1) (`LOS: drivers/mfd/madera-core.c:917-921`; registers.h:50, :2477).
  Our CodecTone/AudioTone already does this. The debounce runs on that clock - INFERENCE (datasheet not read).
- `cirrus,gpsw = <3 0>` -> GP_SWITCH_1 (0x2C8) SW1_MODE [1:0] = 3 (extcon-madera.c:3559-3564; registers.h:207,
  :3716). Purpose = headset-mic ground switch (INFERENCE); not needed for presence.
- IRQ path (later): IRQ1_MASK_7 0x1846 default 0x0F3F (all masked, cs47l92-tables.c:1088); unmask
  MICD_CLAMP_RISE_EINT1 0x10 / FALL_EINT1 0x20 (registers.h:7368-7372); IRQ1_STATUS_7 = 0x1806 (registers.h:1445,
  write-1-to-clear = standard madera irq behaviour, UNVERIFIED here). Codec IRQ line = gpa2-2 (notes/d8-audio.md §7).
- After presence, stock does moisture check (`moisture-imp` set -> madera_hpdet_moisture state first,
  extcon-madera.c:2719-2734), then micd ADC (headset mic vs headphone, `micd-software-compare`), then HPDET
  impedance (madera_hpdet_start :1765, clamps OUT2 via HEADPHONE_DETECT_0 0x299 HPD_OVD_ENA_SEL bit15 :906-912).
  None of this is needed to answer "headphone plugged"; it is needed to tell headset vs headphones (mic) and
  to pick a gain by impedance (beyond_madera hpdet_cb). Not researched further.

### FLL2 values for AIF1BCLK reference (computed with madera_fllhj_apply, `LOS: madera.c:4737-4881`)
FLL2 register block = FLL1 layout at base `MADERA_FLL2_CONTROL_1 - 1` = 0x190 (`LOS: cs47l92.c:2150-2153`;
CONTROL_1 0x191 .. CONTROL_11 0x19B, DIGITAL_TEST_1 0x19D, defaults cs47l92-tables.c:180-191). Lock bit =
IRQ1_RAW_STATUS_2 (0x1881) `FLL1_LOCK_STS1 << (id-1)` = **0x0200** for FLL2 (madera_wait_for_fll, madera.c:4197-4198).
- Fref 3.072 MHz (32-bit slots): refdiv 0, integer (98.304/3.072 = 32), fref >= 1.152 MHz -> HIGH gains 0x21F0,
  lockdet_thr 8, fbdiv 1, hp 0, fast_clk 0; gcd -> **N = 32, theta = 0, lambda = 1**.
  In CodecTone's CODEC_FLL order {RefSrc, N, Theta, Lambda, FbDiv, RefDiv, Gains, Hp, LockThr, FastClk}:
  **{ 8, 32, 0, 1, 1, 0, 0x21F0, 0, 8, 0 }** (RefSrc 8 = MADERA_FLL_SRC_AIF1BCLK, madera.h:36).
- Fref 2.304 MHz (stock 24-bit slots): fractional -> fast_clk 3, fout*6 = 589.824 MHz, hp 3, fbdiv 2 (ratio 256 > 255),
  N = 128, theta 0, lambda 1 -> { 8, 128, 0, 1, 2, 0, 0x21F0, 3, 8, 3 }.
  Cross-check of the method: the same code path explains CodecTone's verified MCLK1 26 MHz entry (refdiv 1, gains
  0x21F0, hp 3, lockthr 8, fast_clk 3).
- Enable sequence = madera_fllhj_enable (madera.c:4883-4942): HOLD=1; apply; REFCLK_SRC; ENA; LOCKDET; CTRL_UPD;
  HOLD=0; wait lock. Identical to CodecFllEnable (CodecTone.c:167-190) with base 0x190 instead of 0x170.
- ASYNC_CLOCK_1 0x112 = FREQ 4 (98.304 MHz, `MADERA_SYSCLK_98MHZ 4` madera.c:71, shift 8) | ENA 0x40 | SRC 5 (FLL2)
  = **0x0445** (fields registers.h:2532-2539; ENA is the DAPM "ASYNCCLK" supply, cs47l92.c:747-748; FRAC not set
  since 98304000 % 6144000 == 0, madera_set_sysclk madera.c:2995-3075). Default 0x0305 (cs47l92-tables.c:149).

## Q5 - conflicts and the shortest sequence

### Conflicts / corrections
1. notes/d8-audio.md §1.2 says the FLL refs are 25 MHz; 0x18CBA80 = **26 000 000** (dtbo6:1491-1492).
2. Stock FLL1 ref is **MCLK1 = AP xclkout 26 MHz**, not MCLK2 32 kHz. Our FLL1-from-MCLK2 is fine for SYSCLK, but
   AIF1 must NOT be clocked from that SYSCLK while ABOX is BCLK master (two free-running references -> drift/slips).
   Stock avoids it by putting AIF1 **and the outputs** on ASYNCCLK = FLL2 <- AIF1BCLK (Q1). Our J2 tone ran
   everything on SYSCLK (OUTPUT_RATE_1 default 0x0040) - that must change for the data path.
3. Stock UAIF0 uses 24-bit slots (BCLK 2.304 MHz, FLL2 ref 2.304 MHz) while madera_hw_params programs the codec for
   32-bit slots (BCLK_FREQ 3.072 MHz / BCPF 64). Works only because the codec is slave - prefer 32-bit slots.
4. Stock headset playback uses SIFS0 -> UAIF0 (mixer_paths:249-250); our speaker path uses SIFS1. Either is valid in
   the ROUTE_CTRL0 enum (SIFS0 = 1, SIFS1 = 2 in [3:0]).
5. The task statement's codec facts (ID 0x6371, cs47l92 driver, HPOUT2, mixer 0x04 tone) all match the sources.
6. d8-audio.md §7 lists `MADERA_HPD_OUT_SEL (OUT1L/OUT1R)`; the accdet node's `reg = <2>` makes HPOUT2 the
   accessory output (extcon-madera.c:870-876). Minor.

### Options for the clock-domain problem (ranked)
- **A (stock, recommended):** ABOX master (CTRL0 MODE 1, MUX_UAIF0 = DIV) + codec slave; codec ASYNCCLK = FLL2 <-
  AIF1BCLK; AIF1_RATE = ASYNC rate 1; OUTPUT_RATE_1 = ASYNCCLK rate 1, OUT_CLK_SRC = ASYNCCLK; SYSCLK stays on FLL1
  <- MCLK2. Needs BCLK running before FLL2 can lock, and FLL2 loses lock when BCLK stops -> keep UAIF0 BCLK
  running (QCHANNEL_DISABLE bit2) for as long as the headphone path is up.
- B: codec master (CBM_CFM: AIF1_BCLK_MSTR 0x20 in 0x500, LRCLK_MSTR 0x1 in 0x501/0x502, AIF1 on SYSCLK) + ABOX slave
  (CTRL0 MODE 0, MUX_UAIF0 = 1 CLKIO). Fewer codec clocks but deviates from stock; ABOX DMA then follows the
  32 kHz-derived codec clock. ABOX slave mode exists in abox_if.c:296-299 but is never used on this board - UNVERIFIED.
- C: AIF1 on ASYNCCLK + codec ASRC1 into a SYSCLK output domain - more registers, no benefit over A.

### Ordered sequence (option A, 48 kHz stereo, 32-bit slots, 24-bit data)
AP / ABOX (bases: CMU_AUD 0x18C00000, ABOX SFR 0x18C50000, GPIO_AUD 0x18C60000):
1. CMU_AUD 0x182C DIV_UAIF0 DIVRATIO[8:0] = 191, poll BUSY bit16 clear - offsets VERIFIED (cmucal-sfr.c:357, :3398);
   191 assumes AUDIF = 589.824 MHz (AboxHw.c:64, measured for UAIF1) - read 0x182C/0x100C first.
2. CMU_AUD 0x100C MUX_UAIF0 SELECT = 0 (DIV) - VERIFIED (cmucal-node.c:182-185). Gates 0x2020 / 0x20A4: read only
   (state left by sboot UNVERIFIED).
3. GPIO_AUD gpb0 CON (+0x000) RMW mask 0x000FFFF0 = 0x00022220 (gpb0-1..4 fn 2; leave gpb0-0 = codec MCLK) - VERIFIED
   (pinctrl dtsi:169-185); bank offset 0 = INFERENCE (gpb0 is the first bank, gpb1 is at +0x20).
4. ABOX 0x0038 QCHANNEL_DISABLE |= 0x4 (UAIF0 BCLK keep-alive) - VERIFIED (abox.h:157-167, abox_soc.h:58-63).
5. ABOX 0x0504 UAIF0_CTRL1 = 0x1985F000 (I2S, NB_NF, 2 x 32) - computed; formula VERIFIED against UAIF1 0x1B4DF7DF.
6. ABOX 0x0500 UAIF0_CTRL0 = 0x01000014 (ABOX master) - reset value UNVERIFIED for UAIF0; read it, then set MODE bit2.
7. ABOX 0x0040 ROUTE_CTRL0 [3:0] = 2 (UAIF0 <- SIFS1) - enum VERIFIED; sharing SIFS1 with UAIF1 UNVERIFIED (use exclusively).
8. RDMA0 / SPUS as in the speaker path (unchanged), then UAIF0_CTRL0 |= SPK_ENABLE (bit0) - VERIFIED (abox_if.c:436-441).
   Whether BCLK toggles before SPK_ENABLE is UNVERIFIED -> if FLL2 does not lock in step 13, enable SPK first with a
   silent ring.
Codec (after the proven J1/J2 bring-up: reset, ID 0x6371, patch, 0x100 = 0x0041 32k <- MCLK2, FLL1 <- MCLK2,
SYSCLK 0x101 = 0x0444):
9.  0x1708, 0x170A, 0x170C, 0x170E = 0x2000 (GPIO5-8 -> AIF1 function) - VERIFIED (pinctrl-cs47l92.c:29; pinctrl-madera.c:610-660).
10. 0x500 = 0x000D, 0x501 = 0x0000, 0x502 = 0x0000, 0x504 = 0x0002, 0x506 = 0x0040, 0x507 = 0x1820, 0x508 = 0x1820,
    0x511 = 0x0000, 0x512 = 0x0001 - VERIFIED-by-code (madera_set_fmt/hw_params), not device-tested.
11. 0x113 ASYNC_SAMPLE_RATE_1 = 0x0003 (48 kHz) - VERIFIED (madera.c:3216-3220).
12. 0x503 AIF1_RATE_CTRL = 0x4000 (rate 8 = ASYNCCLK rate 1, TRI 0) - VERIFIED (madera_hw_params_rate; registers.h:5052-5055).
13. FLL2 at 0x191.. with { RefSrc 8, N 32, Theta 0, Lambda 1, FbDiv 1, RefDiv 0, Gains 0x21F0, Hp 0, LockThr 8,
    FastClk 0 }, CodecFllEnable order; poll 0x1881 & 0x0200 - VERIFIED-by-code (madera.c:4737-4942); needs BCLK.
14. 0x112 ASYNC_CLOCK_1 = 0x0445 - VERIFIED (above).
15. With OUT2 disabled: 0x408 OUTPUT_RATE_1 = 0x4041 (OUT_RATE 8, CP_DAC_MODE bit6 kept, OUT_CLK_SRC 1 = ASYNCCLK) -
    VERIFIED (dtbo6:1490 outclk, mixer_paths:177, registers.h:4337-4348).
16. 0x690 = 0x0020 (HPOUT2L Input 1 = AIF1RX1), 0x698 = 0x0021 (HPOUT2R = AIF1RX2); 0x691/0x699 stay 0x0080 (0 dB) - VERIFIED.
17. 0x51A AIF1_RX_ENABLES = 0x0003 - VERIFIED.
18. 0x400 OUTPUT_ENABLES_1 |= 0x000C (OUT2L/R), same HP power-up as the J2 tone - VERIFIED (J2 log OUT_EN1 000C).
19. 0x419 = 0x0280, 0x41D = 0x0280 (unmute, 0 dB, VU) - VERIFIED.
Teardown (stock shutdown order, beyond_madera.c:254-279): 0x400 &= ~0x000C, 1 ms, ASYNC_CLOCK_1 ENA off, FLL2 off,
then stop UAIF0 (SPK_ENABLE 0 after a 600-1200 us flush, abox_if.c:476-482).
Jack-detect polling (independent of the data path; needs only the 32k clock):
J1. 0x1A06 INTERRUPT_DEBOUNCE_7 |= 0x0015 (MICD_CLAMP_DB | JD2_DB | JD1_DB) - VERIFIED.
J2. 0x2C6 MICD_CLAMP_CONTROL [3:0] = 8 (JD1L_JD2L; 0x0210 -> 0x0218) - VERIFIED (DT micd-clamp-mode 8).
J3. 0x2D3 JACK_DETECT_ANALOGUE |= 0x0003 (JD1_ENA | JD2_ENA) - VERIFIED.
J4. poll 0x1886 IRQ1_RAW_STATUS_7: (val & 0x0010) == 0 -> headphones present; != 0 -> absent - VERIFIED
    (madera_jack_present). Moisture/mic/impedance steps skipped (consequence: no headset-vs-headphone distinction,
    no moisture guard).

## Sources
- Stock DT: `C:\Users\ilya\code\Dumps\extracted\dts\dtbo.img.6.dts` (sound node 1482-1500, uaif links 1741-1767,
  cs47l93@0 5112-5240); `...\dts\dt.img.0.dts:5339` (uaif-max-div)
- Stock mixer paths: `C:\Users\ilya\code\Dumps\extracted\vendor\etc\mixer_paths.xml` (53-55, 177, 187-188, 249-250, 631-635)
- LineageOS/android_kernel_samsung_exynos9820 @ lineage-22.2, raw via
  https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-22.2/<path>:
  sound/soc/samsung/beyond_madera.c; sound/soc/codecs/madera.c, madera.h, cs47l92.c; sound/soc/soc-core.c;
  include/linux/mfd/madera/registers.h; drivers/mfd/cs47l92-tables.c; drivers/mfd/madera-core.c;
  drivers/extcon/extcon-madera.c; drivers/pinctrl/cirrus/pinctrl-madera.c, pinctrl-cs47l92.c;
  sound/soc/samsung/abox/abox_if.c, abox.c, abox.h, abox_soc.h, abox_cmpnt_v20.c;
  drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c, cmucal-node.c;
  arch/arm64/boot/dts/exynos/exynos9820.dts, exynos9820-pinctrl.dtsi
- Repo: drivers/Exynos9820Audio/AboxHw.c (UAIF1 working values), CodecTone.c (FLL1 struct/sequence),
  firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h:859 (GPIO_AUD base)
- Progress: step 3 (Q1-Q5 written) 2026-10-16. No phone access, no code changes.
