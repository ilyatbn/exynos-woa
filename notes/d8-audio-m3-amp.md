# D8 audio M3 — CS35L40 amp bring-up without the Halo DSP (research)

Scope: two CS35L40 on hsi2c_38 (@0x40 cs35l40-bot loudspeaker, @0x41 cs35l40-rcv earpiece).
Research only, no device access, no build. `K` = scratchpad\k (vendor lineage-22.2 copies),
`A` = scratchpad\amp (fetched files), `DTS` = scratchpad\dt\out_0_0.dts.

## Progress log

- 2026-10-12 start: notes file created; reading vendor cs35l41.c.
- K/cs35l41.c == K/sound_soc_codecs_cs35l41.c (cmp identical). Read whole driver (2247 lines).
  Vendor raw findings (all `K/cs35l41.c`):
  - probe order: DEVID/REVID check :2125-2147; IRQ1_MASK1 <- CS35L41_INT1_MASK_DEFAULT :2170;
    dsp_init writes DSP1_RX5..8_SRC :2087-2090; REVID A0 -> regmap_register_patch(reva0_errata) :2177-2185;
    poll IRQ1_STATUS4 & OTP_BOOT_DONE 1 ms x100 :2187-2197; cs35l41_otp_unpack :2199;
    saves OTP_TRIM_30/31 as spk_3/4_trim :2201-2202; pcm_vol default, amp_mute=1 :2204-2205.
  - reva0_errata_patch :2040-2052 (11 writes, unlock 0x40=5555/AAAA ... lock CCCC/3333).
  - otp_unpack :544-632: test-key unlock (TEST_KEY_CTL 5555/AAAA), read OTPID, read 32 words
    OTP_MEM0.., header check, find_otp_map(otp_id), bit-unpack into regs via update_bits, relock.
  - pup/pdn patch :777-791 (reg 0x2084 = 0x002F1AA0 before GLOBAL_EN, 0x002F1AA3 after disable).
  - main_amp_event :961-1021: POST_PMU = pup_patch, GLOBAL_EN=1, (no DSP) DAC_PCM1_SRC <- ASPRX1,
    usleep 1 ms; POST_PMD = GLOBAL_EN=0, usleep 1 ms, pdn_patch. No PUP_DONE wait in vendor PMU path.
  - cap_trim :879-902: when DAC_PCM1_SRC==ASPRX1 (DSP bypass, "rcv_mode") clears INT1/INT2 CAP_TRIM
    bits in OTP_TRIM_30/31 (test-key), else restores saved trims. Called from pcm_source_event :904-945.
  - set_dai_fmt :1158-1233; fs table :1240-1254; pcm_mute :1256-1338; hw_params :1340-1419
    (RX1 slot = right_channel?1:0, RX2 slot = right_channel?0:1 :1385-1394);
    PLL ref table :94-159 (3072000 -> 0x21 :128); codec_set_sysclk :1452-1521.
  - codec_probe :1551-1706 applies DT pdata (bst_vctrl, bst_ipk, hiz, NG, classh...).
  - handle_of_data :1828-2015. boost-ctl-millivolt -> bst_vctrl=((mV-2550)/50)+1 :1856-1863;
    boost-peak-milliamp -> bst_ipk=((mA-1600)/50)+0x10 :1865-1873.
  - **The vendor driver has NO boost inductor/capacitor config at all**: no bst-ind/bst-cap
    properties, no BST_K1/K2/SLOPE/LBST writes (grep pending to confirm).
- Line-number correction after grep: hw_params RX slot writes are K/cs35l41.c:1398-1407
  (RX1 :1401, RX2 :1406); IRQ1_MASK1 default write :2172; remove() masks all :2227.
- Header K/include_linux_mfd_cs35l41_registers.h: addresses :20-140 (PWR_CTRL1 0x2014 ... IRQ1_DB3 0x10318),
  IRQ2/GPIO/NGATE/MBOX :141-200, OTP_TRIM_1..36 :500-536, field masks :538-760. CS35L41_REVID_A0=0xA0,
  CHIP_ID 0x35a40. PLLSRC: SCLK 0, LRCLK 1, SELF 3, PDMCLK 4, MCLK 5, SWIRE 7. No tone/noise/test-signal
  source defined anywhere in the header (grep tone|noise|sine|test|diag|osc only hits TEST_KEY_CTL,
  DSP1_INTPCTL_TESTBITS, BST_TEST_DUTY, PLLSRC_SELF).
- MFD core K/cs35l41-core.c:39-108: VA+VP regulators, reset gpio low->high after 1 ms, then 2 ms wait :87-92.
- DTS amp nodes DTS:5283-5357 (fragment@80, hsi2c clock-frequency 0x61a80 = 400 kHz, pinctrl incl amp-int gpm24-0).
  Neither node has boost-ctl-millivolt, classh-bst-overide, classh-bst-max-limit, fixed-hw-params,
  use-fsync-errata, amp-gain-zc, invert-pcm. boost-peak-milliamp 0x1004 = 4100 mA both.
- DTS sound node :1469-1480: cirrus,ampclk = <0x0 0x0 0x5dc000> = (id 0 -> PLLSRC_SCLK, src 0, 6 144 000 Hz).
  prefixes: 0x1e (@41 rcv) = "Left", 0x1f (@40 bot) = "Right"; routing RECEIVER<-"Left AMP SPK",
  SPEAKER<-"Right AMP SPK" (decoded from samsung,routing hex).
- DTS uaif1 :1737-1746: format dsp_a + bitclock-inversion, no bitclock-master/frame-master -> codec is slave (CBS_CFS).
- Fetched A/ files (raw lineage-22.2): drivers_mfd_cs35l41-tables.c, -pwr.c, -cal.c, cs35l41.h,
  include_sound_cs35l41.h, include_linux_mfd_cs35l41_power.h, sound_soc_samsung_beyond_madera.c,
  exynos9820_madera.c, Makefile, Documentation cs35l41.txt.
- defconfig K/beyond0_defconfig:3734-3740: SND_SOC_SAMSUNG_BEYOND_MADERA=y, EXYNOS9820_CS35L41_I2C=y.
- Machine driver A/sound_soc_samsung_beyond_madera.c: beyond_uaif1_init :678-717 calls
  snd_soc_codec_set_sysclk(ampclk id/source/rate) per amp once at init; read_clk_conf :1808 order (id,source,rate);
  ampclk parsed :1966. prepare :197-231 untristates UAIF1 BCLK; shutdown tristates :277-278.
  => stock SCLK is 6.144 MHz (PLL ref code 0x28, K/cs35l41.c:135) = 128 SCLK/frame at 48 kHz (frame layout = inference).
- A/drivers_mfd_cs35l41-tables.c: reg defaults table :17-~125 (vendor regmap cache defaults; treat as POR
  values = inference): PWR_CTRL2 0x3321, PWR_CTRL3 0x01000011, PLL_CLK_CTRL 0x10, GLOBAL_CLK_CTRL 0x3,
  BSTCVRT_VCTRL1 0x0, VCTRL2 0x1, PEAK_CUR 0x4A, COEFF 0x2424, SLOPE_LBST 0x7500, SP_RATE_CTRL 0x01000028,
  SP_FORMAT 0x18180200, SP_HIZ 0x2, SP_FRAME_RX_SLOT 0x100, SP_RX_WL 0x18, DAC_PCM1_SRC 0x08 (ASPRX1),
  AMP_DIG_VOL_CTRL 0x8000, CLASSH_CFG 0x000B0405, WKFET 0x111, NG_CFG 0x33, AMP_GAIN_CTRL 0x273,
  DTEMP_WARN_THLD 0x2, GPIO1/2_CTRL1 0xE1000001, MIXER_NGATE_CH1/2 0x303.
  OTP maps: otp_map_1 :668-769, otp_map_2 :771-872, otp_map_map :874-890 (OTPIDs 1,2,3 only).
- Upstream (A/upstream, torvalds master): cs35l41-lib.c reg defaults :21-69 agree on COEFF 0x2424,
  SLOPE_LBST 0x7500, PEAK_CUR 0x4A, SP_*; differ on PWR_CTRL2 (0), AMP_GAIN_CTRL (0) because the upstream
  A0 errata :635-652 writes PWR_CTRL2=0 and AMP_GAIN_CTRL=0. Upstream A0 errata also writes
  VIMON_SPKMON_RESYNC(0x4100)=0, 0x4310=0, VPVBST_FS_SEL(0x4400)=0, DSP1_YM/XM_ACCEL_PL0_PRI=0,
  ASP_TX3/4_SRC=0, then CCM_CORE_CTRL=0 (:900-936). Upstream probe order (codecs/cs35l41.c:1244-1300):
  reset high, 2 ms, poll OTP_BOOT_DONE, check IRQ1_STATUS3 bit31 OTP_BOOT_ERR, test-key unlock 0x55/0xAA,
  errata, otp_unpack, lock 0xCC/0x33, gpio_config, IRQ1_MASK1=0x7FFCFE3F, set_pdata (boost config REQUIRED).
- OTP maps vendor vs upstream: element-for-element identical (99 entries each, scripted diff). Upstream adds
  OTPIDs 0x06 (map2) and 0x08 (map1) :699-735; vendor map table lacks them.
- Upstream boost_config lib :970-1073: K1/K2 tables [LBST idx][CBST range], slope table {75,6B,3B,28};
  POR COEFF 0x2424 / SLOPE 0x7500 == table entry (1.0 uH [idx0], C<20 uF [range0]) ->
  with no DT override, stock vendor runs boost with POR coefficients unless the DSP .bin tuning rewrites them.
- Upstream global_enable INT_BOOST :1218-1355: set GLOBAL_EN, poll IRQ1_STATUS1 PUP_DONE (bit24) 1 ms/100 ms,
  W1C. Upstream main_amp_event :508-535 PRE_PMU = pup_patch (key 0x55/AA, 0x2084=0x002F1AA0, 0xCC/0x33) +
  global_enable; POST_PMD = global_enable(0) (poll PDN_DONE bit23) + pdn_patch (0x2084=0x002F1AA3).
- Gain encodings: upstream "Analog PCM Volume" AMP_GAIN_CTRL[9:5], 0..20, TLV 0.5 dB + 1 dB/step
  (upstream codecs/cs35l41.c:171,332). "Digital PCM Volume" AMP_DIG_VOL_CTRL[13:3] signed 11-bit,
  0x4CF(-817)=mute .. 0x391(+913)? no: TLV min -102 dB max +12 dB over 913 steps -> 0.125 dB/step
  (:168-170,330). Vendor TLVs (K/cs35l41.c:356-359) disagree (0.25 dB step, gain TLV 0/0.01 dB) - treat
  upstream as authoritative (inference). Vendor exposes "AMP PCM Gain" (K:506-507) -> value comes from
  Samsung userspace mixer paths, not from the kernel (UNVERIFIED value).

- Facts section written; every K line number re-verified with `grep -n` first. Device OTPID = 3
  (notes/d8-audio-m1.md:582) -> otp_map_2 (T:771-872, map-map T:874-890).
- Self-review of Facts: spot-checked cited lines with sed -n; fixed 10 off-by-N refs (T table lines, UL:649, M:1179, KH:722).
- Sequence / Safety / Status / Open-questions sections written; citations spot-checked with sed/grep (fixed KH:122-125, DTS:5272).

## Facts (with citations)

Abbreviations (paths under the session scratchpad): **K** = `k/cs35l41.c` (vendor codec, identical to
`k/sound_soc_codecs_cs35l41.c`), **KH** = `k/include_linux_mfd_cs35l41_registers.h`, **KC** = `k/cs35l41-core.c`,
**T** = `amp/drivers_mfd_cs35l41-tables.c`, **M** = `amp/sound_soc_samsung_beyond_madera.c`,
**BD** = `amp/Documentation_devicetree_bindings_sound_cs35l41.txt`, **UL / UC / UH** = upstream
`amp/upstream/sound_soc_codecs_cs35l41-lib.c` / `sound_soc_codecs_cs35l41.c` / `include_sound_cs35l41.h`,
**DTS** = `dt/out_0_0.dts`. Registers: 32-bit address, 32-bit data, stride 4.

### F1. Identity / bring-up already done
- DEVID 0x35A40, REVID 0xA0 -> the rev A0 errata applies (K:2177-2185, KH:716). Device OTPID = 3 (d8-audio-m1.md:582).
- Reset: VA+VP on, reset low->high, then 2 ms (KC:55-92). OTP_BOOT_DONE = IRQ1_STATUS4 (0x1001C) bit1 (KH:689),
  polled every 1 ms, 100 tries (K:2187-2197). Upstream also fails on IRQ1_STATUS3 (0x10018) bit31 OTP_BOOT_ERR (UC:1262-1263).

### F2. Errata (A0): vendor K:2040-2052, applied by regmap_register_patch K:2177-2185
`0x40=0x5555, 0x40=0xAAAA, 0x3854=0x05180240, 0x7418=0x9091A1C8, 0x3014=0x0200EE0E, 0x381C=0x00000051,
0x54=0x00000004, 0x10318=0, 0x10B18=0, 0x40=0xCCCC, 0x40=0x3333`.
The upstream A0 patch (UL:635-652, applied with the key unlocked UC:1287-1289) has the same core writes **plus**:
0x4100=0, 0x4310=0, 0x4400=0 (in the vendor driver these three are the "fsync errata" K:1540-1549, applied only with
`cirrus,use-fsync-errata`, which the stock DT does NOT have, DTS:5292-5357), DSP1 XM/YM_ACCEL_PL0_PRI=0,
PWR_CTRL2=0, AMP_GAIN_CTRL=0, ASP_TX3/4_SRC=0, and then DSP1_CCM_CORE_CTRL (0x02BC1000)=0 (UL:930).
The vendor applies the errata *before* the OTP_BOOT_DONE poll (K:2177 vs K:2187); upstream applies it after (UC:1250-1289).

### F3. OTP unpack: the driver must do it itself (vendor and upstream both do it unconditionally)
- Vendor K:544-632: test key 0x5555/0xAAAA (K:564-565); read OTPID at 0x10; read the 32 words OTP_MEM0..31
  (0x400..0x47C) (K:567-572); check the header: word1 & 0xFFFFFFFE == 0x1500 and word2 & 0xFFFF == 0x0300
  (K:574-578, KH:744-747); pick the map by OTPID (K:532-542, T:874-890: ID 1 -> map_1, IDs 2 and 3 -> map_2);
  bit-unpack from word 2 bit 16 (K:550-551). For each of the 99 elements, extract `size` bits (which can span two
  words) and do `update_bits(reg, GENMASK(shift+size-1, shift), val<<shift)` when reg != 0 (K:590-624); relock with
  0xCCCC/0x3333 (K:626-627).
- Upstream uses the same algorithm (UL:815-897, bit_offset 16 / word_offset 2 UL:699-735) but skips the header check.
  The vendor and upstream maps are identical element by element (scripted diff, 99/99).
- What it writes (map_2, T:771-872): oscillator trim 0x2030, reference trims 0x208C/0x2090, PLL LDO 0x300C,
  **boost current-ADC / peak-comparator trims 0x394C-0x395C**, VMON/IMON gain and offset 0x4160-0x4170, temperature
  sensor 0x4360, VP/VBST SAR ADC 0x4448/0x444C, gain-error/offset tables 0x6E30-0x6E64, class-D cap trims
  0x7418/0x741C (T:857-860), 0x7434, MODIX 0x7068, VIMON delay 0x410C/0x400C, VMON_POL 0x4000, die ID 0x17040/44.
  => Without it the boost peak-current limit and the references run untrimmed. Treat it as mandatory.
- After the unpack the vendor saves 0x7418 and 0x741C (K:2201-2202). In DSP bypass (DAC_PCM1_SRC == ASPRX1) it clears
  INT1_CAP_TRIM 0x7418[11:7] and INT2_CAP_TRIM 0x741C[4:0] under the test key (K:879-902, called at K:920,
  masks KH:538-539). It restores them when the signal is routed to the DSP. The purpose is undocumented ("rcv_mode").

### F4. Boost
- The vendor driver has **no** inductor or capacitor handling: no bst-ind/bst-cap properties and no writes to
  BSTCVRT_COEFF 0x3810 or SLOPE_LBST 0x3814 anywhere in K (grep for BST_K/SLOPE/LBST/COEFF: 0 hits).
  It only handles: `boost-ctl-millivolt` -> VCTRL1 0x3800[7:0] = (mV-2550)/50+1 (K:1854-1863, written K:1575-1577),
  which is **absent from the DT**; `boost-peak-milliamp` -> PEAK_CUR 0x3808[6:0] = (mA-1600)/50+0x10 (K:1864-1873,
  K:1579-1581); and the class-H boost override/max-limit -> VCTRL2 (K:1659-1667), also **absent from the DT**.
- DT: boost-peak-milliamp 0x1004 = 4100 mA on both amps (DTS:5308, 5342) -> PEAK_CUR = **0x42**.
- Register defaults (T:53-61; upstream UL:28-30 agrees): VCTRL1 0x0, VCTRL2 0x1 (BST_CTL_SEL = class-H, KH:564-566),
  PEAK_CUR 0x4A (= 4500 mA), COEFF 0x2424, SLOPE_LBST 0x7500. In the upstream tables (UL:970-986), 0x24/0x24/0x75
  with LBST 0 is exactly the (1.0 uH, C < 20 uF) entry. So stock runs the boost loop coefficients at POR, unless the
  DSP tuning .bin rewrites them (UNVERIFIED). Upstream refuses to run the internal boost without ind/cap values
  (binding yaml :84-85, :172-175; UL:988-1073).
- BD:28-31: boost-ctl-millivolt "(Default) VP". Without it the regulated target is VP, and class-H moves VBST with the signal.
- BST_EN = PWR_CTRL2[5:4]; 2 = enabled (KH:670-673), 0 = disabled with the FET off (UH:681). No source defines value 1.

### F5. Clocking (PLL)
- Stock: the machine driver calls the codec's set_sysclk(ampclk) once per amp at card init (M:678-701). The DT has
  `cirrus,ampclk = <0 0 6144000>` (DTS:1480; parse order id/source/rate M:1808-1831): clk_id 0 = PLLSRC_SCLK
  (K:1462-1464), 6.144 MHz -> code **0x28** (K:135; the upstream table UC:79 is the same). 3.072 MHz -> **0x21** (K:128, UC:72).
- set_sysclk body K:1491-1514 (upstream UC:841-857 is identical except that it does not write SP_RATE_CTRL):
  SP_RATE_CTRL 0x4804[5:0] = code (vendor only, K:1494-1497); PLL_CLK_CTRL 0x2C04: OPENLOOP(b11)=1, REFCLK_FREQ[10:5]=code,
  PLL_CLK_EN(b4)=0, PLL_CLK_SEL[2:0]=0, OPENLOOP=0, PLL_CLK_EN=1 (masks KH:650-663). This runs at init, *before* any BCLK
  exists: BCLK is un-tristated only in prepare (M:209-211) and tristated again at shutdown (M:277-278).
- GLOBAL_CLK_CTRL 0x2C0C[4:0] = 0x03 for 48 kHz (table K:1240-1254, written in hw_params K:1379); POR is already 0x3 (T:40).
- Upstream also writes TST_FS_MON0 0x2D10 (fs1 | fs2<<12; 3.072 MHz: 28/44, 6.144 MHz: 16/24; UC:145, 152, 879-891).
  The vendor driver never writes it.
- PLL_LOCK = IRQ1_STATUS3 bit1 (UH:707, UC:468-469). An upstream comment says the lock arrives only after the host
  starts the stream (UL:1203-1216), while the INT_BOOST GLOBAL_EN + PUP_DONE poll happens earlier (UC:508-522).
  So PUP_DONE does not need the PLL lock (inference from that ordering).

### F6. ASP (serial port)
- Stock link: UAIF1 `format = "dsp_a"; bitclock-inversion;` with no master properties, so the codec is the consumer (DTS:1737-1746).
- set_dai_fmt K:1158-1233: CBS_CFS -> SCLK_MSTR(b4)=0, LRCLK_MSTR(b0)=0; DSP_A -> ASP_FMT[10:8]=0 (I2S = 2);
  IB_NF -> SCLK_INV(b6)=1, LRCLK_INV(b2)=0 (KH:546-557, 617). Upstream is identical (UC:684-733).
- hw_params K:1340-1419: SP_FORMAT RX width[31:24] = physical width, TX width[23:16] = the same; SP_RX_WL 0x4840[5:0] = sample
  width (K:1387); SP_TX_WL 0x4830 the same (K:1394). **Slots** (I2S or TDM): RX1_SLOT 0x4820[5:0] = right_channel ? 1 : 0,
  RX2_SLOT [13:8] = right_channel ? 0 : 1 (K:1398-1407). Only @40 has `cirrus,right-channel-amp` (DTS:5299).
- Which amp is which (DTS:1483 prefixes, plus the routing hex in the DTS sound node): 0x1e = @41 rcv = "Left" -> RECEIVER;
  0x1f = @40 bot = "Right" -> SPEAKER. => **@40 (loudspeaker) plays slot 1 and @41 (earpiece) plays slot 0.**
- Stock frame: SCLK 6.144 MHz / 48 kHz = 128 SCLK per frame (DSP_A TDM). The channel count and slot width that ABOX
  uses on UAIF1 are UNVERIFIED (abox_hw_params_fixup_helper, M:1179). Supported RX formats are S16_LE and S24_LE (KH:722).
- SP_ENABLES 0x4800: ASPRX1 = b16, ASPRX2 = b17, ASPTX1..4 = b0..3 (K:1035-1040, UH:612-623).
- SP_HIZ_CTRL 0x480C[1:0] = 3 from `asp-sdout-hiz` (DTS:5301, K:1588-1592; meaning BD:44-50).
- Register defaults: SP_FORMAT 0x18180200 (I2S, 24/24), SP_FRAME_RX_SLOT 0x100, SP_RX_WL 0x18, SP_RATE_CTRL 0x01000028 (T:68-74).

### F7. Data path / DSP bypass
- DAC_PCM1_SRC 0x4C00: 0x08 = ASPRX1 ("ASP"), 0x32 = DSP TX1 ("DSP") (K:430-431, KH:638-649). POR = 0x08 (T:75, UL:39).
  When no DSP is routed, the vendor PMU writes DAC_PCM1_SRC = ASPRX1 (K:985-991).
- DAPM path: ASPRX1 (SP_ENABLES b16) -> PCM Source -> CLASS H (PWR_CTRL3 0x201C b4) -> Main AMP (PWR_CTRL2 b0)
  (widgets K:1035, 1047, 1049; routes K:1140, 1144, 1146).

### F8. Gain / volume
- AMP_GAIN_CTRL 0x6C04: PCM gain [9:5], 0..0x14; upstream TLV is 0.5 dB + 1 dB/step (UC:171, 332-333), so 0x14 = 20.5 dB.
  ZC is bit 10 (KH:560). Defaults: the vendor table has 0x273 (field 19 = 19.5 dB, T:101); the upstream errata writes 0 (UL:649).
  The vendor sets the gain only through the ALSA control "AMP PCM Gain" (K:506-507), so the stock value lives in
  Samsung's userspace mixer paths: **UNVERIFIED**.
- AMP_DIG_VOL_CTRL 0x6000: PCM volume [13:3], signed 11-bit, 0.125 dB/step: 0 = 0 dB, 0x4CF = mute (-102 dB),
  max 0x060 = +12 dB (UC:168-170, 330-331; KH:604-608). Soft ramp [2:0] = {off, .5, 1, 2, 4, 8, 15, 30 ms} (UC:176-180).
  Invert is bit 14 (KH:609). POR is 0x8000 (T:90, UL:54); neither header defines bit 15 (keep it via read-modify-write).
  The vendor's default control value 817 maps to register 0 = 0 dB (K:2204, K:1316-1325). The vendor TLV (K:356-359)
  claims 0.25 dB/step; upstream's 913 steps from -102 to +12 dB gives 0.125 dB. Upstream is taken as correct (inference).
- AMP_OUT_MUTE 0x2024 bit4 = mute (K:1288-1306, KH:601-602). Vendor mute: volume -> 0x4CF, wait the ramp, AMP_OUT_MUTE=1.
  Vendor unmute: AMP_OUT_MUTE=0, volume -> the stored value (K:1256-1338).

### F9. Power up / down
- Vendor Main AMP POST_PMU (K:969-998): pup_patch {0x40=0x55, 0x40=0xAA, 0x2084=0x002F1AA0, 0x40=0xCC, 0x40=0x33}
  (K:777-783), then PWR_CTRL1 0x2014 bit0 GLOBAL_EN=1 (K:974-976), DAC_PCM1_SRC=ASPRX1, sleep 1 ms (K:992).
  **There is no PUP_DONE wait.** POST_PMD (K:999-1016): GLOBAL_EN=0, sleep 1 ms, pdn_patch {.., 0x2084=0x002F1AA3, ..} (K:785-791).
- Upstream (UC:491-535 + UL:1278-1293) uses the same patches but polls IRQ1_STATUS1 for PUP_DONE (b24) / PDN_DONE (b23)
  every 1 ms for up to 100 ms, then clears the bit by writing 1. PUP/PDN masks: KH:675-678.
- IRQ1_MASK1 0x10110 = 0x7FFCFE3F at probe (K:2172, KH:699); everything is masked on remove (K:2227).

### F10. DT noise gate / class-H / GPIO (vendor codec_probe K:1551-1706, values DTS:5292-5326)
- NG_CFG 0x6808: sel[13:8]=0x3F, dly[6:4]=2, thld[2:0]=6 -> **0x3F26** (K:1638-1657, KH:587-592). POR is 0x33 (T:100).
- MIXER_NGATE_CH1/CH2 0x12004/0x12008: enable b16, thld[2:0]=6, delay[11:8]=2 -> **0x00010206** (K:1603-1636, KH:594-599).
- CLASSH_CFG 0x6800 = 0x000B0405 and WKFET_CFG 0x6804 = 0x111 from the DT (K:1659-1700); both are identical to POR (T:98-99).
- GPIO2 IRQ: GPIO_PAD_CONTROL 0x242C[26:24] = 4 (active-low IRQ), GPIO2_CTRL1 0x1100C bit31 = 0 (output) (K:1733-1760, DTS:5318-5321).
- `cirrus,pwr-params` (DTS:5322-5325) feeds cirrus-pwr (amp/drivers_mfd_cs35l41-pwr.c:59-80) only when
  `cirrus,pwr-global-enable` is present. It is absent, so stock DSP power management is off anyway.

### F11. Internal signal sources (question 2)
- Every DAC/ASP/DSP input source that is defined: Zero 0x00, ASPRX1 0x08, ASPRX2 0x09, VMON 0x18, IMON 0x19, CLASSH 0x21,
  VPMON 0x28, VBSTMON 0x29, DSP_TX1 0x32, DSP_TX2 0x33, TEMPMON 0x3A, RSVD 0x3B (KH:638-649; UH:645-655).
  Neither driver nor either header has a tone, sine, noise or test-pattern generator (grep tone|sine|noise|test|diag).
- PLL sources: SCLK 0, LRCLK 1, SELF 3, PDMCLK 4, MCLK 5, SWIRE 7 (KH:655-661). PLLSRC_SELF can be selected through
  vendor set_sysclk clk_id 3 (K:1472-1473), but no driver uses it and nothing documents it. It would give only a
  clock, not a signal.
- The monitor sources are ADC readings (supply, temperature, class-H level, i.e. close to DC). Routing them to the DAC
  would put DC or low-frequency content on the speaker: never do this.
- The only other generator is Halo DSP firmware, which needs a firmware download and still needs a clock.
- => **Audible output without loading DSP firmware requires the host to supply SCLK + LRCLK (FSYNC) + SDIN data.**
  The CS47L93 tone generator cannot help: the amps sit on ABOX UAIF1, not on a codec AIF (DTS:1737-1746).

## Proposed sequence

Scope: DSP bypass, internal boost, codec = clock consumer, DSP_A + SCLK inverted (stock link), 48 kHz.
Frame choice: **stock clock, 6.144 MHz SCLK = 128 bits/frame, i.e. 4 x 32-bit slots** (PLL code 0x28). The
alternative is 2 x 32-bit = 3.072 MHz (code 0x21); then only rows 4.x change. Host data: 32-bit samples MSB-first,
amp WL = 24 (it takes the top 24 bits of its slot; inference). Slot 0 = @41 earpiece, slot 1 = @40 loudspeaker.
"RMW" = read-modify-write of the named field; other bits keep their read value. Run the sequence per amp
(@40 and @41). The only per-amp difference is row 5.2.

| # | reg (name) | write / op | source |
|---|---|---|---|
| 0.1 | GPIO reset gph1-1 / gph1-2 | already high, >= 2 ms | KC:87-92 (done in M1b) |
| 0.2 | 0x1001C IRQ1_STATUS4 | poll bit1 OTP_BOOT_DONE, 1 ms x 100 | K:2187-2197 |
| 0.3 | 0x10018 IRQ1_STATUS3 | read; bit31 OTP_BOOT_ERR must be 0 | UC:1262-1263 |
| 1.1 | 0x40 TEST_KEY_CTL | 0x5555, then 0xAAAA | K:2041-2042 |
| 1.2 | 0x3854 | 0x05180240 | K:2043 |
| 1.3 | 0x7418 OTP_TRIM_30 | 0x9091A1C8 | K:2044 |
| 1.4 | 0x3014 | 0x0200EE0E | K:2045 |
| 1.5 | 0x381C BSTCVRT_DCM_CTRL | 0x00000051 | K:2046 |
| 1.6 | 0x54 | 0x00000004 | K:2047 |
| 1.7 | 0x10318 IRQ1_DB3 / 0x10B18 IRQ2_DB3 | 0 / 0 | K:2048-2049 |
| 1.8 | 0x40 | 0xCCCC, then 0x3333 | K:2050-2051 |
| 2.1 | 0x40 | 0x5555, 0xAAAA | K:564-565 |
| 2.2 | 0x10 OTPID | read (expect 3 -> otp_map_2) | K:567, T:883-887 |
| 2.3 | 0x400..0x47C OTP_MEM0..31 | read 32 words; check w1&0xFFFFFFFE==0x1500, w2&0xFFFF==0x0300 | K:569-578 |
| 2.4 | 99 trim fields | unpack from w2 bit16, RMW each (reg,shift,size) of T:771-872 | K:590-624 |
| 2.5 | 0x40 | 0xCCCC, 0x3333 | K:626-627 |
| 2.6 | 0x7418 / 0x741C | read and log (vendor spk_3/4_trim) | K:2201-2202 |
| 3.1 | 0x40 | 0x5555, 0xAAAA | K:883-884 |
| 3.2 | 0x7418 | RMW clear [11:7] (INT1_CAP_TRIM) | K:886-887, KH:538 |
| 3.3 | 0x741C | RMW clear [4:0] (INT2_CAP_TRIM) | K:888-889, KH:539 |
| 3.4 | 0x40 | 0xCCCC, 0x3333 | K:898-899 |
| 3.5 | 0x10110 IRQ1_MASK1 | 0x7FFCFE3F | K:2172 |
| 3.6 | 0x3808 BSTCVRT_PEAK_CUR | RMW [6:0] = 0x42 (4100 mA) | DTS:5308, K:1871, K:1579-1581 |
| 3.7 | 0x480C SP_HIZ_CTRL | RMW [1:0] = 3 | DTS:5301, K:1588-1592 |
| 3.8 | 0x6808 NG_CFG | 0x00003F26 (optional, stock parity) | DTS:5305-5307, K:1638-1657 |
| 3.9 | 0x12004 / 0x12008 MIXER_NGATE_CH1/2 | 0x00010206 (optional, stock parity) | DTS:5302-5304, K:1603-1636 |
| 3.10 | 0x6800 / 0x6804 | leave (DT value == POR 0x000B0405 / 0x111) | T:98-99, K:1659-1700 |
| 4.1 | 0x4804 SP_RATE_CTRL | RMW [5:0] = 0x28 (0x21 for 3.072 MHz) | K:1494-1497 |
| 4.2 | 0x2C04 PLL_CLK_CTRL | 0x810 -> 0xD10 -> 0xD00 -> (SEL=0) 0xD00 -> 0x500 -> 0x510 (3.072 MHz: 0x810, 0xC30, 0xC20, 0xC20, 0x420, 0x430) | K:1499-1514, KH:650-663 |
| 4.3 | 0x2C0C GLOBAL_CLK_CTRL | RMW [4:0] = 0x03 (48 kHz) | K:1243, K:1379 |
| 5.1 | 0x4808 SP_FORMAT | 0x20200040 (RX/TX width 32, DSP_A, SCLK_INV, consumer) | K:1158-1233, K:1384-1397 |
| 5.2 | 0x4820 SP_FRAME_RX_SLOT | **@40: 0x00000001**, **@41: 0x00000100** | K:1398-1407, DTS:5299 |
| 5.3 | 0x4840 SP_RX_WL | 0x18 | K:1387 |
| 5.4 | 0x4C00 DAC_PCM1_SRC | 0x08 (ASPRX1) | K:985-991, KH:638 |
| 5.5 | 0x6C04 AMP_GAIN_CTRL | RMW [9:5] = test value (see Safety; first test 0x0A -> 0x00000140) | UC:171,332; K:506 |
| 5.6 | 0x6000 AMP_DIG_VOL_CTRL | RMW [13:3] = 0x4CF (mute) -> 0x0000A678 from POR | K:1265-1270, KH:604-607 |
| 5.7 | 0x2024 AMP_OUT_MUTE | 0x10 (muted) | K:1286-1294 |
| 6.1 | (host) ABOX UAIF1 | start BCLK + LRCLK, send zeros | M:209-211 (stock order: clocks before DAPM power-up) |
| 6.2 | 0x10018 IRQ1_STATUS3 | poll bit1 PLL_LOCK (log; up to ~10 ms, inference), W1C 0x2 | UH:707, UC:468-469 |
| 6.3 | 0x4800 SP_ENABLES | 0x00010000 (ASPRX1) | K:1035 |
| 6.4 | 0x201C PWR_CTRL3 | RMW set bit4 (CLASS H) | K:1047, K:1140-1146 |
| 6.5 | 0x2018 PWR_CTRL2 | 0x00000021 (AMP_EN b0 + BST_EN=2 [5:4]; monitors off) | K:1049, KH:670-673, UL:1069-1070 |
| 6.6 | 0x40 / 0x2084 / 0x40 | 0x55, 0xAA, 0x2084=0x002F1AA0, 0xCC, 0x33 (pup_patch) | K:777-783, UC:491-497 |
| 6.7 | 0x2014 PWR_CTRL1 | RMW set bit0 GLOBAL_EN | K:974-976 |
| 6.8 | 0x10010 IRQ1_STATUS1 | poll bit24 PUP_DONE, 1 ms x 100; then W1C 0x01000000 | UL:1278-1293 (vendor only sleeps 1 ms, K:992) |
| 6.9 | 0x10010 | check bits 31/17/15/8/7/6 == 0 (else abort -> 8.x) | KH:683-688 |
| 7.1 | 0x2024 AMP_OUT_MUTE | 0x00 | K:1303-1307 |
| 7.2 | 0x6000 AMP_DIG_VOL_CTRL | RMW [13:3] = test volume (Safety: -20 dB = 0x760 -> 0x0000BB00) | K:1314-1331 |
| 7.3 | (host) | play the tone for 2 s, re-read IRQ1_STATUS1 + RAW_STATUS1 mid-tone | - |
| 8.1 | 0x6000 | RMW [13:3] = 0x4CF (mute); wait >= ramp (ramp 0 -> 1 ms) | K:1265-1285 |
| 8.2 | 0x2024 | 0x10 | K:1288-1294 |
| 8.3 | 0x2014 PWR_CTRL1 | clear bit0 | K:1002-1005 |
| 8.4 | 0x10010 | poll bit23 PDN_DONE 1 ms x 100, W1C 0x00800000 | UL:1278-1293 (vendor: 1 ms sleep K:1007) |
| 8.5 | 0x40 / 0x2084 / 0x40 | 0x55, 0xAA, 0x2084=0x002F1AA3, 0xCC, 0x33 (pdn_patch) | K:785-791 |
| 8.6 | 0x4800 / 0x2018 | 0 / 0 (ASP and boost off) | inference (upstream errata baseline PWR_CTRL2=0 UL:648) |
| 8.7 | (host) | stop ABOX clocks (after PDN_DONE) | M:277-278 (stock tristates at shutdown, after DAPM down) |
| 8.8 | reset GPIO | optionally drive low to leave the amps in reset | KC:72-90 |

Notes on the table:
- Row 1 (errata) is done after OTP_BOOT_DONE in upstream order. The vendor does it before (F2); both precede the unpack.
- Rows 3.1-3.4 copy the vendor bypass behaviour (F3). Upstream does not do it. If in doubt, log both trims and keep it.
- Row 6.2 is informational: per F5, PUP_DONE should not depend on the lock. If PUP_DONE times out, the log of
  PLL_LOCK / PLL_CLK_CTRL tells the two cases apart.
- Upstream sets AMP_EN *after* GLOBAL_EN (DAPM PRE_PMU, UC:508-522). The vendor sets it before (POST_PMU, K:969). We follow the vendor.
- Upstream-only extras, not needed for bypass and not done by stock: TST_FS_MON0 (F5), DSP1_CCM_CORE_CTRL=0 (F2).

## Safety

What protects what, without the DSP (CSPL speaker protection):
- **Nothing models the voice coil** (temperature or excursion). That is the job of the Halo firmware, and cirrus-pwr is
  off even in stock (F10). The chip's own protections cover only the die and the boost stage: TEMP_WARN at the
  DTEMP_WARN_THLD 0x4220[1:0] threshold (POR 2 = 125 C), which "applies the over-temperature warning attenuation"
  (BD:53-61, T:64); TEMP_ERR; AMP_SHORT; BST_OVP/DCM_UVP/SHORT (KH:683-688). All of these latch the amp into a
  safe mode that needs the PROTECT_REL_ERR_IGN 0x2034 release pulse (write 0, set RLS bit, clear) (K:676-760, KH:692-697).
  => Speaker safety in bypass comes **only from the signal level we allow** (analog gain x digital volume x
  sample amplitude) and from the boost being limited.
- Stock settings: the vendor kernel never sets AMP_GAIN, only exposes "AMP PCM Gain" (K:506-507); the stock value is
  in userspace (UNVERIFIED). The kernel default is 0 dB digital (K:2204). Boost: class-H tracking (VCTRL2 POR 0x1),
  target VP when nothing else asks (BD:28-31), peak current 4100 mA (DTS:5308). No boost-voltage cap is configured
  in stock (no boost-ctl-millivolt, no classh-bst-max-limit in DTS:5292-5357).

Recommended values (**all inference**: the datasheet full-scale voltage vs gain is not in hand):
- ~2 s test tone, both amps:
  - AMP_GAIN_CTRL [9:5] = **0x0A** (10.5 dB, 10 dB under the 20.5 dB max; register 0x00000140).
    If inaudible, step to 0x0E; never above 0x13 (the 19.5 dB POR default in T:101) without the stock mixer value.
  - AMP_DIG_VOL_CTRL [13:3] = **-20 dB** (field 0x760, register 0x0000BB00 from POR 0x8000).
  - Host sine 1 kHz at **-6 dBFS**, so -26 dBFS at the DAC. At 10 dB below max gain this is ~36 dB under the
    amp's full scale. Even if full scale were ~10 Vpk (assumption), that is about 0.16 Vpk, ~1.6 mW into 8 ohm:
    audible at arm's length and orders of magnitude under a microspeaker's ~0.5-1 W rating. Duration 2 s is irrelevant thermally.
  - Earpiece @41: the same, or 6 dB lower digital volume (it is a smaller receiver driver; stock calls it "rcv").
  - Boost: leave class-H and POR coefficients (stock behaviour). PEAK_CUR 0x42 as in stock. Low-level signals
    keep VBST near VP in class-H mode (BD:28-31 + class-H description; inference).
  - Mute -> unmute only after PUP_DONE; mute before GLOBAL_EN=0 (vendor order, K:1256-1338, K:999-1016) to avoid pops.
- Normal Windows playback (until the DSP firmware path exists):
  - Clamp in the driver, not in the UI: analog gain fixed at **<= 0x0A** (loudspeaker), **<= 0x06** (earpiece);
    digital volume user range **-102 .. 0 dB, never positive** (the register allows +12 dB, F8).
  - Keep the hardware noise gates as stock (rows 3.8/3.9), so idle hiss and DC are gated.
  - Keep AMP_DIG_VOL_CTRL bit15 as POR (meaning undefined; possibly an HPF, UNVERIFIED). Never route a monitor
    source to the DAC (F11).
  - On any TEMP_WARN / TEMP_ERR / *_SHORT / BST_* status: mute, GLOBAL_EN=0, log; release only on the next stream.
  - Revisit once the stock "AMP PCM Gain" value is known (open question 1). Stock plays loud only because CSPL
    limits excursion and temperature; without it, stay well under the stock level.
- **Never**: write BSTCVRT_COEFF/SLOPE_LBST to guessed inductor values, set boost-ctl-millivolt higher than needed,
  or run GLOBAL_EN without the OTP unpack (untrimmed boost current sense, F3).

## Status/IRQ readback for an unattended log

Read and log everything as hex. "W1C" = write-1-to-clear (K:658-662, UL:1292).

| when | reg | what proves what | source |
|---|---|---|---|
| after reset | 0x1001C IRQ1_STATUS4 | bit1 OTP_BOOT_DONE = 1 | KH:689 |
| after reset | 0x10018 IRQ1_STATUS3 | bit31 OTP_BOOT_ERR = 0 | UC:1262-1263 |
| unpack | 0x10 OTPID, 0x404/0x408 | OTPID = 3; header 0x1500 / 0x0300 matched | K:567-578 |
| unpack | 0x7418, 0x741C (before/after row 3) | trims landed; cap-trim bits cleared | K:2201-2202, K:879-902 |
| config | 0x2C04 PLL_CLK_CTRL | 0x510 (or 0x430) | row 4.2 |
| config | 0x4804, 0x4808, 0x4820, 0x4840, 0x4C00, 0x2C0C | ASP config is as intended (per-amp slot) | rows 4-5 |
| config | 0x3800, 0x3804, 0x3808, 0x3810, 0x3814 | boost state (expect 0, 1, 0x42, 0x2424, 0x7500) | T:53-58 |
| config | 0x6C04, 0x6000, 0x2024 | gain / volume / mute | vendor log_status K:161-194 |
| clocks on | 0x10018 bit1 | PLL_LOCK (W1C 0x2) | UH:707, UC:468-469 |
| GLOBAL_EN | 0x2014, 0x2018, 0x201C | PWR_CTRL1 b0 = 1, PWR_CTRL2 = 0x21, PWR_CTRL3 b4 = 1 | K:161-194 |
| GLOBAL_EN | 0x10010 IRQ1_STATUS1 | **bit24 PUP_DONE = 1 within 100 ms** = power-up proven | KH:677, UL:1278-1293 |
| GLOBAL_EN + mid-tone | 0x10010..0x1001C, RAW 0x10090..0x1009C | errors: b31 AMP_SHORT, b17 TEMP_ERR, b15 TEMP_WARN, b8 BST_SHORT, b7 BST_DCM_UVP, b6 BST_OVP must be 0 | KH:683-688, KH:122-125 |
| mid-tone | 0x640C VPBR_STATUS, 0x6410 VBBR_STATUS | brownout activity (field layout UNVERIFIED, log raw) | KH:106-107 |
| GLOBAL_EN=0 | 0x10010 | bit23 PDN_DONE = 1 within 100 ms | KH:675, UL:1278-1293 |

- An unattended pass = OTP_BOOT_DONE, PLL_LOCK, PUP_DONE, no error bits during the tone, PDN_DONE. That proves the amp
  powered up and stayed healthy. It does **not** prove sound.
- Stronger unattended proof (design idea, later task): use the amp's own V/I sense. Set PWR_CTRL2 VMON b12 + IMON b13,
  ASP_TX1_SRC 0x4C20 = 0x18 (VMON) and ASP_TX2_SRC 0x4C24 = 0x19 (IMON) (POR already 0x18/0x19, T:76-77;
  sources K:442-446), enable ASPTX1/2 (SP_ENABLES b0/b1), and pick free TX slots (SP_FRAME_TX_SLOT 0x4810). Then
  capture UAIF1 on the ABOX side: a 1 kHz component in IMON means current flowed through the speaker. Needs the ABOX
  capture path.
- IRQ1 status pin: GPIO2 configured active-low (F10) on gpm24-0 "amp-int" (DTS:5272-5281). Polling is enough for the test.

## Open questions

1. **Stock "AMP PCM Gain"** for speaker and receiver: in `/vendor/etc/mixer_paths*.xml` (adb read, no root needed
   usually). This gives the upper bound for Windows. UNVERIFIED until read.
2. **Boost inductor/capacitor**: the vendor kernel never programs them (F4). Does the stock tuning
   `/vendor/firmware/cs35l40-bot*.bin` / `-rcv*.bin` write 0x3810/0x3814? Pull and parse (read-only). Until then, keep POR.
3. **Stock UAIF1 frame** (channels x slot width at 6.144 MHz): ABOX DT `abox_uaif_1` node / abox fixup. Only the
   SCLK frequency (6.144 MHz) is verified (DTS:1480).
4. Datasheet-only semantics, all UNVERIFIED: BST_EN = 1, VCTRL1 = 0, VCTRL2 bit2 BST_LIM, AMP_DIG_VOL_CTRL bit15,
   which register holds PLL_UNLOCK (0x10, KH:690), VPBR/VBBR_STATUS layout, the absolute full-scale output vs AMP_GAIN.
5. Does PUP_DONE assert without SCLK? Upstream implies yes (F5). Our sequence starts the clocks first anyway. Log both
   PLL_LOCK and PUP_DONE to settle it.
6. What the vendor cap-trim clearing in bypass is for (F3, "rcv_mode"). We copy it. A/B test: tone with and without.
7. Test-key values: the vendor uses both 0x5555/0xAAAA (errata, OTP) and 0x55/0xAA (pup/pdn). Upstream uses 0x55/0xAA
   everywhere (UL:782-812). Copy each sequence verbatim.
