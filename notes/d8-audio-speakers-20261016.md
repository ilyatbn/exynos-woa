# Speakers: volume, stereo (earpiece), BT hand-back, choppiness - 2026-10-16

## Progress log
- 11:5x user: no sound from the speakers in the BBB test -> my mpv `--volume=10` (mpv volume is cubic: 10 % ~ -60 dB);
  the driver log showed the stream running at 0 dB endpoint volume with the R/bot amp powered. Not a driver bug.
- 12:06 BT hand-back works: after the WH-1000XM4 disconnects the default render endpoint returns to
  "Speakers (Galaxy S10e Audio)" (100 %, unmuted); user confirmed sound from the bottom speaker (T8 closed).
- User: bottom speaker "pretty low for 100%". Cause: fixed amp digital volume -10 dB (AMP_SPK_FIXED_STEPS -80) with
  software endpoint volume. Tried HwVolume=1 (endpoint level on the amp, 0 dB at 100 %) -> user: "jittery and choppy",
  also when seeking in foobar. Log: 88 late timer passes / 52 late writes in one 55 s stream during post-boot CPU load
  (33-44 %); the ring had only a 1-period (10 ms) lead.
- 1.0.1.12: HwVolume back to 0 (no I2C during playback), AMP_SPK_FIXED_STEPS -80 -> 0 (0 dB = top of the amp notes'
  playback range, analog gain 0x0A unchanged, earpiece trim -6 dB unchanged), DMA ring 4 -> 8 periods with a 3-period
  lead (AUD_DMA_LEAD; reported position excludes the lead). User: seeking still stutters, but the next 36 s foobar
  session logged 0 late timer passes / 0 late writes over 3649 packets -> not the DMA timing.
- Stereo (Parameters\Stereo 1, L -> slot 0 -> earpiece amp @0x41, R -> slot 1 -> loudspeaker @0x40): user heard
  only the bottom speaker. 1.0.1.13 dumps both amps' registers after power-up and before power-down
  (notes/d8-audio-m3-amp.md verification table): both identical except RXSLOT (@40 0x1, @41 0x100) and VOL
  (@40 0x8000 = 0 dB, @41 0xBE80 = -6 dB); PWR1 1, PWR2 0x21, PLL 0x510, DACSRC 0x08 (ASPRX1), GAIN 0x153 ([9:5] 0x0A),
  IRQ1 STATUS1 0x01400000 (PUP_DONE, no AMP_SHORT/TEMP/BST error bits), unchanged at power-down.
- 1.0.1.14: Parameters\RcvSlotReg (diagnostic override of the earpiece SP_FRAME_RX_SLOT; 0xFFFFFFFF = default 0x100).
- 13:05 **microphone measurement** (PC "Microphone Array (Realtek)", was muted - unmuted for the test, muted again;
  scratchpad srv/mictest.py: record while lr.wav plays = left 440 Hz 2 s (-> earpiece only in Stereo 1), pause, right
  660 Hz 2 s (-> loudspeaker only)): 440 Hz 34-47 dB over the noise floor for ~2.25 s, 660 Hz 49-61 dB.
  Log confirms earpiece RXSLOT 0x100 / bottom 0x1 in that run => **the earpiece speaker works**; at the PC microphone
  it is ~14 dB quieter (the -6 dB trim + the small receiver driver facing the ear). Not raised: the earpiece already
  runs analog gain 0x0A, above the amp notes' <= 0x06 earpiece clamp; the -6 dB trim keeps the total near it.
- Still open: user-perceived stutter when seeking in foobar (driver shows 0 late periods); in Stereo 1 the bottom
  speaker plays only the right channel (left-panned content goes to the quiet earpiece).
