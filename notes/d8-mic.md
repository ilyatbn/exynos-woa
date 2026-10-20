# D8 microphones under Windows (user 2026-10-20: "Microphone sounds like a quick-ish win. Make it. Add gain control
# to the sound app. Measure it by playing a sound from the pc and checking if the mic works.")

Sources (fetched 2026-10-20 as data, scratchpad, LineageOS android_kernel_samsung_exynos9820 lineage-22.2):
`AB` = sound/soc/samsung/abox/{abox_soc.h, abox_soc.c, abox_wdma.c, abox_cmpnt_v20.c, abox_if.c, abox.h, abox_rdma.c},
`MD` = include/linux/mfd/madera/registers.h, sound/soc/codecs/{madera.c, cs47l92.c}, drivers/mfd/cs47l92-tables.c.
Prior research: notes/d8-audio-linux.md Q6 (DMIC1/2 -> CS47L93 IN3L/IN4L, MICBIAS1A/1B; headset mic IN1BL),
notes/d8-audio-design.md (capture path), notes/d8-jack-windows.md (UAIF0 <-> AIF1 link works for headphones,
codec = clock master, HpMaster 1).

## Path

```
DMIC1 (MICBIAS1A) -> CS47L93 IN3L ---\                          ABOX
DMIC2 (MICBIAS1B) -> CS47L93 IN4L ---> AIF1TX1/2 mixers -> AIF1 TX (DSP-A 4 x 32, slots 0/1, codec master)
  -> UAIF0 MIC (CTRL0 MIC_ENABLE) -> ROUTE_CTRL1 NSRC0 = UAIF0 -> SPUM NSRC0 chain (no ASRC, OUT = WDMA)
  -> SIFM0 -> WDMA0 -> DRAM ring (physical address, AUD SysMMU bypassed like RDMA0)
```

## Facts

### Codec (CS47L92 family = CS47L93)
- IN3 / IN4 are digital-mic-only on CS47L92: cs47l92.c has "IN3 OSR" (DMIC oversample) and no IN3 mode control;
  madera_configure_input_mode (madera.c:1170-1268) writes only IN_DMIC_SUP for inputs >= max_analogue_inputs (2 on
  this family: the `default` case, max_dmic_sup 4).
- DT `cirrus,dmic-ref = <0 0 1 1>` (d8-audio-linux.md:252) -> IN3/IN4 DMIC_SUP = 1 = MICBIAS1:
  IN3L_CONTROL 0x320 field IN3_DMIC_SUP 12:11 (MD registers.h 3998) = 0x0800; IN4L_CONTROL 0x328 the same.
  IN3L_HPF bit15 (0x8000) = input high-pass filter (DC removal).
- ADC_DIGITAL_VOLUME_3L 0x321 / _4L 0x329: IN_VU 0x0200, IN3L_MUTE 0x0100, DIG_VOL 7:0; reset 0x0180 (muted,
  0x80); TLV madera_digital_tlv, max 0xBF (cs47l92.c:312-317) -> 0x80 = 0 dB, 0.5 dB steps, 0xBF = +31.5 dB.
  This is the gain control.
- DMIC3L_CONTROL 0x322 / DMIC4L 0x32A: IN3_OSR 10:8, reset 0x0500 = OSR 5 = 3.072 MHz DMIC clock (madera.c:2312-2318).
- INPUT_ENABLES 0x300: IN3L_ENA 0x0020, IN4L_ENA 0x0080 (registers.h 3746-3756). IN3L_RATE_CONTROL 0x323 rate 15:11,
  reset 0 = SAMPLE_RATE_1 (48 kHz, set by CodecClockOn).
- MIC_BIAS_CTRL_1 0x218 (reset 0x00E6): EXT_CAP 0x8000, LVL 8:5, RATE 0x8, DISCH 0x4, BYPASS 0x2, ENA 0x1.
  MIC_BIAS_CTRL_5 0x21C (reset 0x2222): MICB1A_ENA 0x0001, MICB1B_ENA 0x0010 (discharge bits 0x2 / 0x20).
  DT: MICBIAS1 1.8 V with external cap (d8-audio-linux.md:310). LVL encoding 1.5 V + 0.1 V per step (Linux
  arizona/madera micbias regulators: min 1.5 V, step 100 mV) -> 1.8 V = 3 -> 0x8000 | 3<<5 | DISCH | ENA = 0x8065.
  (LVL encoding = INFERENCE from the regulator driver convention; 2.2 V reset level would also bias a DMIC.)
- AIF1 TX: FRAME_CTRL_3 0x509 (TX1 slot, reset 0), FRAME_CTRL_4 0x50A (TX2 slot, reset 1), AIF1_TX_ENABLES 0x519
  (TX1 0x1, TX2 0x2). AIF1TX1MIX_INPUT_1_SOURCE 0x700 / VOLUME 0x701, TX2 0x708 / 0x709. Mixer source codes
  (madera_mixer_values, madera.c:1574+): IN3L 0x14, IN4L 0x16. Mixer volume reset 0x0080 = 0 dB
  (SOC_SINGLE_RANGE_TLV 0x20..0x50 at shift 1, madera_mixer_tlv -32 dB + 1 dB/step: 0x40 << 1).

### ABOX
- UAIF CTRL0 MIC_ENABLE bit1; capture startup also sets DATA_MODE=1 / IRQ_MODE=0 (AB abox_if.c:51-55); reset
  CTRL0 0x01000010 already has DATA_MODE 1.
- ROUTE_CTRL1 0x44: NSRC(x) = 4-bit field at 4x (AB abox_soc.h:84-88); texts {RESERVED, SIFS0..4, RES, RES, UAIF0..3}
  (abox_cmpnt_v20.c:3427-3431) -> NSRC0 [3:0] = 8 = UAIF0. ROUTE_CTRL2 NSRC_CONNECTION_TYPE(0) bit20 ("NSRC0 Bridge")
  left 0.
- SPUM_CTRL0 0x300: RSRC_RECP bit1 (patch_9820), FUNC_CHAIN_NSRC(x) at 4+4x: bit 4x+4 ASRC, bit 4x+7 OUT
  (0 WDMA, 1 SIFMS; sifmx_texts abox_cmpnt_v20.c:3473-3474) -> NSRC0 chain = 0 (no ASRC, out to WDMA).
- SPUM_SBANK_NSRC(0) 0x328 = (512 << 16) | 1024 = 0x02000400 (patch_9820, abox_soc.c:1856).
- DAPM: SIFM0 <- SPUM ASRC1 <- NSRC0; "WDMA%d Capture" <- SIFM0..6 with control "WDMA" (route_base_wdma,
  abox_cmpnt_v20.c:5007-5017). No AP register selects the SIFM for a WDMA (abox_wdma.c has no regmap writes besides
  the generic ones) -> WDMA n <- SIFM n assumed. UNVERIFIED until the device test.
- WDMA(x) base 0x3000 + 0x100x: CTRL +0 (ENABLE bit0; reset 0x00400000 = BURST_LEN 8), BUF_STR +8, BUF_END +0xC,
  BUF_OFFSET +0x10, STR_POINT +0x14, VOL_FACTOR +0x18, VOL_CHANGE +0x1C, SBANK_LIMIT +0x20, STATUS +0x30
  (PROGRESS 31, RBUF_OFFSET 27:12, RBUF_CNT 11:0) (AB abox_soc.h:466-504). FORMAT field position = RDMA's
  (12:8, proven for RDMA0 on boot #99) assumed for WDMA - UNVERIFIED.

## Progress log
- 2026-10-20 research above (codec + ABOX capture registers). Next: a driver test mode (Parameters MicTest) that
  records ~3 s from the DMICs into a DMA ring at driver start and writes RMS + raw samples to LogFiles, to prove the
  hardware path before the ACX capture endpoint.
- 2026-10-20 device runs (audio 1.0.2.0-1.0.2.5, self test Parameters\MicTest one-shot, mictest.sh / micsweep.sh):
  - codec path came up as written (BIAS1 8065 BIAS5 2233 IN3L/IN4L 8800, vol 0098, OSR 0500, MIX 8014/8016, INEN 00A0,
    TXEN 0003); ABOX route registers as written (ROUTE_CTRL1 8, SPUM_CTRL0 2, SBANK_NSRC0 02000400, UAIF0 CTRL0 01000012).
  - WDMA0 never moved (STATUS 0) in any variant (START_FIFO_DIFF_MIC, NSRC0 bridge, SPUM ASRC, SIFM flush).
  - TX-only: gpb0 DAT bit1 toggled (BCLK) only; AIF1 RX enabled too (1.0.2.2): bits 1 and 4 toggle (codec data out).
  - Loopback variant (NSRC0 <- SIFS1, silent RDMA0 -> UAIF1 running) + WDMA index sweep (1.0.2.5): **WDMA1 runs**
    (STATUS 83F40000 / 84FC0220), WDMA0 and WDMA2-7 never start. => SIFM0 feeds WDMA1 (WDMA0 = the RECP / SPUM
    ASRC0 path: route_base_wdma's unconditional "WDMA%d Capture" <- "SPUM ASRC0"). The position fields advance
    ~4x faster than 16-byte frames at 48 kHz would - unit of RBUF_OFFSET for WDMA to be measured.
  - **Real mics on WDMA1 (1.0.2.5, MicWdma 1)**: 144000 frames in 3 s (real time), all 4 slots saved from 1.0.2.6:
    slots 0/1 carry data, 2/3 zero (layout = AIF1TX1/2 in slots 0/1 confirmed). But the inputs were constant: HPF decay
    from full scale over 0-150 ms, then -63 dBFS DC, no tone.
  - Cause: MICVDD off. MIC_CHARGE_PUMP_1 0x200 resets to 0x0006 (BYPASS | DISCH, CPMIC_ENA clear); Linux enables it as
    the "MICVDD" supply of the MICBIAS regulators (arizona-micsupp.c madera_micsupp: enable_reg 0x200 mask CPMIC_ENA,
    vsel LDO2_CONTROL_1 0x213 [10:5], ext ranges 0.9 V + 25 mV (<= 0x14), 1.5 V + 100 mV (0x15..0x27), enable_time
    3000 us). LDO2 reset 0x03E4 -> VSEL 0x1F = 2.5 V. 1.0.2.7 writes 0x200 = 0x0005 (ENA | DISCH, regulated) + 3 ms
    before MICBIAS1: ambient -63 dBFS rms with 50/100 Hz room hum = live mics.
  - PC tone: the PC's default speaker endpoint is muted (tools/phone/audio/pcvolume.ps1, Core Audio
    IAudioEndpointVolume: mute 1 level 0.72); mictest.sh unmutes at 0.30 for the tone only and restores it.
  - **RESULT (1.0.2.7, gain +12 dB, PC tone 1000 Hz amp 0.15 at endpoint level 0.30):** ch0 rms -52.0 dBFS, 1000 Hz
    band -50.0 dB, SNR 55.0 dB vs the surrounding 200 Hz; ch1 -48.3 dB, SNR 53.3 dB; harmonic 2000 Hz -75 dB. The tone
    lands exactly on 1000 Hz -> 48 kHz rate right. Both DMICs work. traces/audio/mic-*.raw.
- Windows endpoint (audio 1.0.3.0): Capture.c - ACX capture circuit "Microphone0" (sample CaptureCircuit.cpp shape:
  host pin Source / CommunicationSink / KSCATEGORY_AUDIO, bridge pin Sink / KSNODETYPE_MICROPHONE + integrated jack,
  software volume (<= 0 dB) + mute elements = Windows' mic level), RT stream: WDMA1 ring of 8 periods, a packet timer
  on the WDMA position copies each completed period into the client packet (slot 0 -> L, slot 1 -> R, 16 / 32-bit),
  GetCapturePacket = last completed packet + its start QPC. Ep[2] in the endpoint array (Capture flag keeps the
  HwVolume amp branches off it). INF: Audio_Device.I.Microphone (KSCATEGORY_AUDIO / CAPTURE / REALTIME, "Microphone0",
  48 kHz 16-bit OEM format), Parameters MicGain default 24 (+12 dB).
  Device: "Microphone (Galaxy S10e Audio (ABOX + CS35L40))" endpoint OK; the engine's format probe ran (16/32-bit
  streams created + released). tools/phone/audio/wascap.c (ARM64, WASAPI shared-mode capture, works from SSH):
  mix format float32 2 ch 48 kHz; with the PC tone: 192000 frames / 4 s, 0 silent packets, 2 discontinuities
  (start), ch0 1000 Hz -50.9 dB SNR 54.7 dB, ch1 -49.0 dB SNR 55.2 dB (traces/audio/wascap-1.raw) = same as the raw
  hardware test -> the endpoint works end to end.
- Gain control (audio 1.0.3.1): AUD_PROPERTY_MICGAIN (id 1 in AUDPROPSETID_SpeakerTrim, ULONG 0.5 dB steps 0..63,
  Control.c AudMicGainRequest -> MicSetGain + Parameters\MicGain). App tools/S10eSpeakerTrim: "Microphone boost" slider
  0..+31.5 dB (disabled "n/a" on older drivers), CLI `S10eSpeakerTrim.exe mic [N]`. Installed to C:\Program Files\
  S10eSpeakerTrim. tools/phone/audio/micgaintest.sh (PC tone 1 kHz amp 0.15, PC level 0.30, wascap 3 s per gain):
  gain 0 -> ch0 1 kHz -61.6 dB / ch1 -59.9; 24 -> -49.4 / -47.8; 48 -> -37.4 / -35.7 = +12.2 / +12.0 dB per 24 steps
  (expected 12.0). Registry follows (0x30 after "mic 48", back to 0x18). Live change during a 6 s capture (mic 48 at
  ~3.5 s): per-second 1 kHz -49.4 -49.3 -49.5 -49.6 | -42.3 (straddles) | -37.4, no extra discontinuity. Default left
  at 24 (+12 dB).
