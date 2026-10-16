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
- 15:4x **balance measurement** (user: bottom louder than top). scratchpad srv/bal3.py + lr3.wav: 12 x alternating
  0.25 s band-limited noise bursts (500-6000 Hz, -20 dBFS RMS) L/R, mpv --volume=100, endpoint 100 %. Steady tones
  are unusable with the PC "Microphone Array (Realtek)": its capture processing suppresses stationary signals within
  ~0.5 s (first run: tones decayed 45 -> 21 dB); the WDM-KS pin recorded digital zeros. Result (11 clean pairs, first
  pair dropped as onset): top/earpiece median 47.4 dB over floor, bottom 54.3 dB -> **bottom - top = +7.3 dB**.
- Stock gains found (Dumps/extracted/vendor/etc/mixer_gains.xml, closes amp-notes open question 1):
  gain-media-dual-speaker: Left (earpiece @41) AND Right (bottom @40) "AMP PCM Gain" 17, "Digital PCM Volume" 817
  (= 0 dB); gain-media-speaker: Right 17 / 817; gain-media-handset (call earpiece): Left 4 / 761. So stock stereo
  music drives both amps identically, at analog gain 17 (we run 0x0A = 10, 7 steps lower, without the DSP
  protection firmware stock has). => the -6 dB earpiece trim (AMP_RCV_TRIM_STEPS -48) is not stock; drop it.
- 1.0.1.15: AMP_RCV_TRIM_STEPS (-6 dB earpiece) removed; new Parameters\RcvAtten / SpkAtten (DWORD, 1/8 dB amp steps,
  attenuation under the fixed 0 dB, clamped to 816 = -102 dB; default 0 = both amps 0 dB as stock dual-speaker).
  Measurements (bal3.py, same mic position, 100 %): trim 0/0 -> bottom - top = +1.5 dB; SpkAtten 12 (-1.5 dB) ->
  -1.2 dB (run-to-run spread ~+-1 dB). Phone left at **SpkAtten 6 (-0.75 dB)**, RcvAtten 0. Driver log confirms the
  registers: bot VOL 0xBFA0 (field 0x7F4 = -12 steps with SpkAtten 12), rcv VOL 0x8000 (0 dB); GAIN 0x153 both.
- Headphone jack (user: "doesn't work or doesn't detect"): **not implemented at all** - the jack is the CS47L93
  codec's HPOUT2 (notes/d8-audio.md §7), on spi_2; the driver only drives ABOX -> UAIF1 -> the two CS35L40 amps.
  Needs: Exynos USI SPI master driver (none in repo), CS47L93 bring-up (reset gpg1-0, BOOT_DONE, FLL1 26 -> 98.304
  MHz, AIF1), ABOX UAIF0 route, HPDET/MICDET jack detection on the codec IRQ, a second (headphone) endpoint with
  KSJACK_DESCRIPTION2. Plan in notes/d8-audio-design.md §1a.
- Review of e37f6c5: checked AmpTargetSteps clamp (min(Atten, 816), result >= -816 = lowest non-mute; mute stays
  the separate 0x4CF field), registry table + defaults, version in Driver.c/INF, the driver log VOL values above.
  Found: (a) the trim also applies in HwVolume 1 mode and to the mono (Stereo 0) loudspeaker - harmless (fixed
  per-phone offset) but the header comment said "under the fixed digital volume"; accepted, documented here.
  (b) the amp notes' "<= 0x06 earpiece" clamp was inference; stock evidence recorded in d8-audio-m3-amp.md.
- 16:0x user: "make the top a bit louder" (top already at 0 dB digital, the ceiling). 1.0.1.16: per-amp analog gain
  AMP_GAIN_CTRL 0x6C04 [9:5] (= ALSA "AMP PCM Gain", amp notes :182, 0.5 dB + 1 dB/step) from Parameters\RcvGain /
  SpkGain (default 0x0A both, clamped to 17 = stock gain-media-dual-speaker). Phone: RcvGain 12 (+2 dB on the
  earpiece), SpkAtten 6. Installed 16:08 (log: 1.0.1.16 loaded).
- User next: "make the speaker gain dynamic and controllable via an app" -> plan: control device interface + IOCTL in
  the driver (apply to powered amps at once, persist to Parameters), small tray app with top/bottom sliders.
- 1.0.1.17/18 + tools/S10eSpeakerTrim (user: "make the speaker gain dynamic and controllable via an app"):
  private KS property AUDPROPSETID_SpeakerTrim {6FEF068C-7388-4619-86D2-91FCA6A5E4AC} id 0 = AUD_TRIM {RcvAtten,
  SpkAtten, RcvGain, SpkGain} (AudControl.h), handled in AudEvtCircuitRequestPreprocess before ACX (Control.c): GET
  returns Cfg; SET clamps (atten <= 816, gain <= 17), writes Cfg under BusLock, persists to Parameters, AmpApplyTrim
  (gain RMW + digital volume on powered amps, register dump "after trim"). 1.0.1.18 = 1.0.1.17 + that dump.
  Verified on the phone: trimcli get/set from ssh AND from the unelevated desktop user (runuser2, RunLevel Limited)
  -> no admin needed; live during playback: rcv GAIN 0x1D3 (14) then 0x193 (12), bot VOL 0xBFD0 (-6 steps), STATUS1
  0x01400000 (no error bits); Parameters updated. Mic check of the live change inconclusive (room floor 7 dB higher,
  phone moved: absolute levels 15 dB lower than the earlier runs).
  App: C:\Program Files\S10eSpeakerTrim\S10eSpeakerTrim.exe (+ Start menu "S10e Speakers"), WinForms, two sliders
  (0.5 dB, -20..+7 dB; > 0 = analog gain, < 0 = digital attenuation), tray icon, "Equal" + Exit menu. Built with the
  .NET Framework csc (anycpu). Launched in session 1 at ~16:35.
- Process note: the 1.0.1.16 per-amp gain parameter (installed 16:08, verified only now via the "after trim" dumps)
  and the app control land in one commit - 1.0.1.16 was never committed separately.
- 16:14 also seen: the reset after freeze #16 came ~8 s after the last sample (raw counter of the next boot puts the
  SoC reset at ~16:14:02), not 82 s - see notes/thermal-clocks.md.
- Review of f4cd8d1: checked Split/Join round trip (+0.5 dB -> gain 11 atten 4 -> +1 half-dB; +7 dB -> gain 17 atten 0;
  registry gain 12/atten 0 -> slider +2.0 dB), AMP_OP_GAIN runs inside the BusLock-held sequences, Control.c clamps
  before touching Cfg, interface filter "sams9827" matched on the phone, register dumps match the requested values.
  Accepted risks: any desktop user can raise both amps to gain 17 (= the stock music value, the cap); a failed
  WdfRegistryAssignULong is not logged (only a failed key open); the property is answered for any item type, not
  only the circuit. No follow-up needed.
- 16:3x user set the balance by ear in the app: RcvGain 12 / RcvAtten 0 (top +2 dB), SpkGain 10 / SpkAtten 40 (bottom
  -5 dB); "yeah make it the default". 1.0.1.20: these are the driver defaults (Driver.c AudReadConfig); app sliders now
  show dB relative to them (TopBase Join(12,0) = +4, BottomBase Join(10,40) = -10 half-dB; menu "Default (0 / 0 dB)").
  Installed with the app; trimcli get = 0/40/12/10. The phone's Parameters hold the same four values, so the new
  compiled defaults only matter on a fresh install (not exercised by a test - would need the values deleted).
