# D8 audio — Windows driver M1 (Exynos9820Audio: render endpoint + volume)

Goal: Windows sound on the loudspeaker with working volume (the volume keys already send Consumer
0xE9/0xEA via ACPI0011/Hidinterrupt, so a normal render endpoint with endpoint volume is enough).
Later: 3.5 mm jack. No device access in this task (agent worktree); the main session tests builds.

Hardware path (notes/d8-audio-m3-abox.md, notes/d8-audio-m3-amp.md):
ABOX RDMA0 -> SPUS OUT0 -> SIFS1 -> UAIF1 (DSP-A, ABOX master) -> CS35L40 @0x40 (bottom speaker,
slot 1) / @0x41 (earpiece, slot 0) on hsi2c_38.

Tasks: A1 driver skeleton (ACX or portcls), A2 DSDT Device (AUD0), A3 INF + build + package.

## Progress log

- 2026-10-12 start: worktree fast-forwarded a8d8a62 -> 8efa319 (main). Read d8-audio-m3-abox.md,
  d8-audio-m3-amp.md, d8-audio-design.md, d8-audio-windows.md.
- Framework evidence: WDK 10.0.26100.0 has ACX headers km\acx\km\1.0 and 1.1 and
  Lib\10.0.26100.0\km\arm64\acx\km\{1.0,1.1}\acxstub.lib (plus portcls.lib/ks.lib/stdunk.lib).
  The phone's Windows image (s10e-artifacts\win-w5\s10ewindows.vhdx, mounted read-only, ntoskrnl
  10.0.22621.2861) ships `Windows\System32\drivers\Acx01000.sys` and portcls.sys; no INF matching *acx*
  (ACX is a class extension loaded by KMDF, no INF of its own). No 1.1 runtime in 22621.
  **Decision: ACX 1.0** (ACX_VERSION_MAJOR=1, ACX_VERSION_MINOR=0, link km\1.0 acxstub.lib) - the
  only ACX version the phone's OS has; building against 1.1 would fail to bind on 22621.
- Reference fetched (data only): microsoft/Windows-driver-samples tree 2dc3fd3a (main), all 34 files under
  audio/Acx/ -> scratchpad\acx\Samples\{AudioCodec,Common,Inc,Shared}. Cited below as `ACX/<path>`.
- ACX sample read (ACX/AudioCodec/Driver/{Driver,Device}.cpp, ACX/Common/{RenderCircuit,StreamEngine,CircuitHelper}.cpp,
  ACX/AudioCodec/Driver/AudioCodec.{inf,vcxproj}): render circuit = host pin (AcxPinTypeSink, KSCATEGORY_AUDIO) + bridge
  pin (Source, KSNODETYPE_SPEAKER) with a jack; RT stream via AcxRtStreamCreate with EvtAcxStreamAllocateRtPackets
  (max 2 packets, page-rounded, packet 0 at offset alloc-PacketSize so it ends where packet 1 starts), timer-paced
  AcxRtStreamNotifyPacketComplete, QPC position. INF: Class MEDIA, KmdfService, AddInterface KSCATEGORY_AUDIO/RENDER/
  REALTIME with reference string = circuit name, EP\0 association + EventDriven. **No ACX include in the INF**
  (the class extension is bound by acxstub.lib through WDF). Sample builds ACX 1.1 / KMDF 1.31; we use ACX 1.0.
- Repo patterns: S10eBattery/build-package.ps1 (VS18 MSBuild, VisualStudioVersion=17.0, OutDir/IntDir in %TEMP%,
  signtool sha1 5DD5DEE1..., Inf2Cat 10_RS3/NI/GE_ARM64); hand-written .inf (not .inx).
- Amp bus facts in Exynos9820.h (VERIFIED): EXYNOS_AUDIO_HSI2C_BASE 0x109A0000 size 0x1000 GSIV 460; PERIC1 GPIO
  0x10830000 (+0x1000) gph0-6/7 = SCL/SDA function 3; gph1-1 = amp R reset, gph1-2 = amp L reset (low = reset);
  USI17_I2C mode reg 0x10821050 is never written (UEFI probe reads only). CMU_AUD 0x18C00000+0x8000 and
  PINCTRL_AUD 0x18C60000+0x1000 are in the header as SOURCE-ONLY (boot #96 read them live and got sane values).
- UEFI amp proof (d8-audio-m1.md:582): @40 and @41 DEVID 35A40 REVID A0 OTPID 3 OTP_BOOT_DONE 1 after the reset
  release (resets are input/low at handoff; the UEFI probe puts them back = amps in reset when Windows starts).
  So the Windows amp layer must: gph0-6/7 to fn 3, I2C init, gph1-1/2 output low 1 ms -> high, 3 ms, then the
  d8-audio-m3-amp.md table. Port source: Beyond0LteAudioProbeDxe.c I2cReset/I2cXfer/AmpRead (lines 157-306).

## Design (A1)

- **Language C**, like the repo's other drivers (ACX headers are WDF-style C). Files: Driver.c (entry, PnP/power,
  resources, registry), Render.c (circuit, pins, formats, RT stream, DMA ring, timer clock), AboxHw.c (ABOX
  layer: AboxAudit read-only + AboxInit/AboxStart/AboxStop/AboxPosition with TODO stubs for stage 1/2),
  AmpI2c.c (hsi2c_38 polled master ported from Beyond0LteAudioProbeDxe.c + CS35L40 table-driven sequences),
  RunLog.c (text log -> C:\Windows\System32\LogFiles\Exynos9820Audio-run.txt).
- **Volume decision: no hardware volume/mute element by default (Parameters\HwVolume = 0).** If a circuit exposes a
  KSNODETYPE_VOLUME element, the Windows audio engine hands endpoint volume to that node and stops applying it
  in software; a node backed by nothing (stage 2 not proven, amp sequence not proven) would make the slider and
  the volume keys do nothing audible. Without a volume node, audiodg applies endpoint volume/mute in software
  (IAudioEndpointVolume::QueryHardwareSupport = 0), so the keys (Consumer 0xE9/0xEA -> shell -> default endpoint
  volume) and the slider work from day one, independent of the hardware. HwVolume = 1 creates ACX volume +
  mute elements wired to the CS35L40 AMP_DIG_VOL_CTRL / AMP_OUT_MUTE (clamped to <= 0 dB, amp notes F8/Safety)
  for later, once the amp sequence is device-proven.
- **DMA ring**: ACX RT packets (<= 2) carved from ONE MmAllocateContiguousNodeMemory block below 4 GiB
  (BUF_STR is a 32-bit SFR), page-rounded per packet, packet 0 at offset (alloc - PacketSize) so packet 0 ends
  exactly where packet 1 starts -> the ring [phys + alloc - PacketSize, + PacketCount*PacketSize) is physically
  contiguous for the RDMA. Memory is CPU-cached (the ACX user mapping's cache type is not ours to pick, and a
  mismatched non-cached alias is an ARM64 hazard); cache maintenance = KeFlushIoBuffers(Mdl, FALSE, TRUE)
  ("clean", the m12-wifi-m5 helper) on SetRenderPacket, on Run, and on every packet tick (2 pages / 10 ms).
  Parameters\CacheMaint (default 1). Ring hardware-usable only if PacketSize % 16 == 0 (RDMA BUF_* 16-byte units,
  F11 2.1 layout, UNVERIFIED on 2.0.1); otherwise timer-only.
- **Clock/position**: the sample's QPC timer (one-shot high-resolution WDF timer per packet) is the default clock;
  AboxPosition() replaces the linear position only when Parameters\AboxEnable = 1 and the stage-2 code reports a
  valid RDMA offset (TODO until stage 2 lands).
- **Default boot behaviour (no registry values)**: map resources, read-only ABOX audit guarded by PMU AUD_STATUS,
  amp bus up + reset release + ID read (the UEFI-proven m1b steps), then nothing else. The Speaker endpoint works
  in timer mode (silent). AmpEnable = 1 runs the amp table; AboxEnable = 1 runs the ABOX stage-1/2 hooks.
- **_CRS additions beyond the task list** (A2): PMU page 0x15861000 (AUD_STATUS guard, F5 - every AUD access must be
  gated on it) and PERIC1 GPIO 0x10830000 (gph0-6/7 I2C pin mux, gph1-1/2 amp resets). Neither is claimed by
  another DSDT device (Dsdt.asl grep).
- Offline check of the phone image (s10ewindows.vhdx RO, SYSTEM hive copied to scratchpad\hive and loaded):
  Services\Acx01000 exists (ImagePath system32\drivers\Acx01000.sys, Group WdfLoadGroup, Start 3, Type 1);
  Acx01000.sys 10.0.22621.2506, Wdf01000.sys 1.33.22621.1. => ACX 1.0 binds through WDF on the phone;
  KMDF version for the project: 1.31 (<= 1.33 available; the ACX sample's value).
- ACX 1.0 headers have every API used (AcxRtStreamCreate, AcxStreamInitSetAcxRtStreamSupportsNotifications,
  AcxRtStreamNotifyPacketComplete, ACX_RT_STREAM_CALLBACKS with Allocate/Free/SetRenderPacket/GetCurrentPacket/
  GetPresentationPosition, AcxVolumeCreate/AcxMuteCreate, AcxPinGetRawDataFormatList, AcxJackCreate).
- A1 code: drivers/Exynos9820Audio/{Exynos9820Audio.h, Driver.c, Render.c, AboxHw.c, AmpI2c.c, RunLog.c,
  Exynos9820Audio.vcxproj, resource.rc, build.ps1}. First build: ks.h C2054 errors - fixed by the sample's include
  order (windef.h + mmsystem.h before ks.h). Second build: exit 0 at /W4 /WX, Exynos9820Audio.sys 49664 B, ARM64,
  imports WDFLDR WdfVersionBind + WdfVersionBindClass, map shows acxstub:stub.obj _Acx_BIND_INFO (km\1.0 stub).
  sha256 60415DD47420CEEA59A3E9BFD6170C94005CEB7A47CB9B6081E7D6F9590B2105 (unsigned work build, not a package).
- AmpI2c error-mask self-check while writing: bits 31/17/15/8/7/6 = 0x800281C0 (first draft had 0x8002A1C0, fixed
  before the build).

## Review of A1 (469ed1b)

Checked mechanically: every Exynos9820.h line cited in Exynos9820Audio.h (280, 688, 737-742, 754-760, 792-793,
798-811) holds the named define with the copied value; the UEFI line refs in AmpI2c.c (157, 177, 285, 395-424)
point at I2cReset / I2cXfer / AmpRead / the reset release; every amp-table constant (11 sampled) appears in
d8-audio-m3-amp.md; computed masks: STATUS1 errors 0x800281C0, -20 dB field 0x760, mute 0x4CF, cap trim 0xF80,
gain 0x0A<<5 = 0x140 - all match the code. Build already exit 0 at /W4 /WX.

Findings (each a follow-up commit):
- R1 (real, latent): AudHwStart runs AboxStart (DMA + SPK_ENABLE) BEFORE AmpPowerUp, whose PUP_DONE poll can take
  up to 100 ms, and Run takes StartTime only after both. Once stage 2 works, the RDMA would run up to ~100 ms ahead
  of the timer clock that tells the client which packet to fill -> it would play packets not written yet. Order must
  be AboxInit (stage-1 clocks, needed by the amp PLL) -> AmpPowerUp -> AboxStart -> StartTime, so DMA start and
  clock start are back to back.
- R2 (real, HwVolume = 1 only): Dev->VolumeLevel starts at 0 (zeroed context) = 0 dB, the loudest allowed amp level,
  until Windows assigns its persisted volume. Start at the fixed safe level (-20 dB) instead.
- Not findings (checked): clean (not invalidate) on a packet the client may be writing cannot lose data; 64-bit
  GlitchAdjust read is single-copy atomic on ARM64; the glitch loop terminates (next deadline = now + 1 packet);
  AudLog's RtlStringCbVPrintfA at DISPATCH is legal (no %ws/%wZ used anywhere).
- R1 fixed: AudHwStart = AboxInit -> AmpPowerUp -> AboxStart (amps powered down again if the DMA start fails); Run still takes StartTime right after AudHwStart. Build exit 0 /W4 /WX.
- R2 fixed: Dev->VolumeLevel starts at -20 dB (-20 * 0x10000 KS units = AmpTargetSteps -160 = the fixed level). Build exit 0.

## 2026-10-12 message from the main session: loudspeaker path DEVICE-VALIDATED (boot #99, PC mic SNR 44-51 dB)

Source of truth: main-branch Beyond0LteAudioProbeDxe/AudioSpeaker.c (+ AudioAmpOtp.h), notes/d8-audio-m3-abox.md
"boot #99 RESULT". Facts as relayed (to be checked against the code before porting):
- RDMA addresses are PHYSICAL (AUD SysMMU disabled at boot); contiguous DRAM below 4 GiB works; CPU must write back
  caches.
- ABOX: CMU_AUD DIV_UAIF1 (+0x1830, low 9 bits) = 95 with AUDIF left at sboot /2 -> 6.144 MHz BCLK, poll BUSY bit16;
  gpb1 CON (GPIO_AUD+0x20) low 16 bits = 0x2222; SYSREG MISC_CON (+0x504) = 1; IFR+0x10 SYSPOWER_CTRL = 1;
  IFR+0x38 |= bit3; UAIF1 CTRL1 = 0x1B4DF7DF; CTRL0 = 0x01000014 then |= 1; IFR+0x220 = 0x02000000;
  IFR+0x40 = 0x20; RDMA0 BUF_STR +8, BUF_END +0xC = STR+bytes, BUF_OFFSET +0x10 = period, STR_POINT +0x14 = STR,
  VOL_FACTOR +0x18 = 0x800000, CTRL0 = 0x00400000 | (0x1B<<8) | 1. STATUS +0x30: bit31 progress, bits 11:0 =
  frame counter mod 4096 at 48 kHz (position source).
- Data: 4 interleaved 32-bit slots per frame, left-justified; slot 1 = speaker @0x40, slot 0 = earpiece @0x41
  (@41 untested).
- ORDER: SPK_EN + RDMA0 running with data BEFORE amp power-up (PUP_DONE times out otherwise; arrives in ~1 ms).
  PLL_LOCK bit never reads set - ignore. Amp: errata, OTP unpack (AudioAmpOtp.h), bypass trims, PLL code 0x28,
  SP_FORMAT 0x20200040, DAC src 0x08, gain 0x0A, digital volume -10 dB.
- Teardown: amp mute + GLOBAL_EN off + PDN_DONE, then SPK_EN off, RDMA0 CTRL0 = 0x00400000.
- In Windows: AUD domain still on; amps left in reset by UEFI (gph1-1/2 low) -> driver releases them; gph0-6/7 -> fn 3.
Consequences for the driver: the stream format becomes 4x32 slots in the ring (driver must expand the 2ch client
format), R1's order (amps before DMA) is WRONG for this hardware and must be reversed, timer clock could become the
RDMA frame counter.
- Read main 24fb796 AudioSpeaker.c (the validated code) + AudioAmpOtp.h (otp_map_2, 99 entries) + abox notes boot
  #99 RESULT. Differences from my A1 tables: pup/pdn patch keys are 0x5555/0xAAAA + 0xCCCC/0x3333 (AUnlock/ALock),
  not 0x55/0xAA/0xCC/0x33 (amp notes 6.6/8.5); power-down = DIG_VOL mute, 2 ms, OUT_MUTE, GLOBAL_EN off, PDN_DONE,
  W1C, pdn patch, SP_ENABLES 0, PWR_CTRL2 0; volume -10 dB (0x7B0); PLL_LOCK poll dropped; RDMA STATUS 27:12 stays
  0 -> AboxPosition's RBUF_OFFSET decode is wrong, the position source is STATUS[11:0] frames mod 4096.
  Stage-1 writes: DIV_UAIF1 (CMU+0x1830 & 0x1FF) = 95 + BUSY(bit16) poll, gpb1 CON & 0xFFFF = 0x2222,
  MISC_CON = 1, SYSPOWER_CTRL = 1, QCH |= bit3, UAIF1 CTRL1 0x1B4DF7DF, CTRL0 0x01000014, SBANK_RDMA0 0x02000000,
  ROUTE_CTRL0 0x20. Stage 2: SPK_EN, RDMA0 BUF_STR/END/OFFSET/STR_POINT, VOL 0x800000, CTRL0 0x00401B01.
  Teardown: SPK_EN off, CTRL0 0x00400000, wait STATUS bit31 clear (<= 50 ms); UEFI restores every saved register.

### Plan B (port the validated sequence; one commit each)
- B1 AboxHw.c: stage-1 table filled with the validated rows (saved + restored on release: AboxRelease), AboxStart =
  SPK_EN + RDMA0 program/enable, AboxStop = SPK off + RDMA reset + progress poll, AboxFrameCount = STATUS[11:0].
- B2 AmpI2c.c: OTP unpack (AmpOtpMap.h = copy of AudioAmpOtp.h with NT types), pup/pdn keys 0x5555/0xAAAA, the
  validated power-down order, fixed digital volume -10 dB, earpiece amp only with Parameters\Stereo = 1.
- B3 Render.c: the hardware frame is 4 x 32-bit slots (slot 1 speaker, slot 0 earpiece) but the endpoint stays
  48 kHz stereo 16/32-bit -> a separate NON-CACHED contiguous DMA ring (4 periods x frames x 16 B, below 4 GiB;
  no CPU cache maintenance needed, no user mapping of it) filled by the packet DPC one period ahead of the DMA:
  at boundary j the DPC completes packet k0+j-1 and expands packet k0+j into slot (j+1)%4 (slot 0 = silence at
  start, so 1 period of added latency). Mono default (Stereo = 0): slot 1 = (L+R)/2, slot 0 = 0. Boundaries come
  from the RDMA frame counter when the hardware runs (no QPC/PLL drift), else from QPC as before. Order: AboxInit
  -> AboxStart (data flowing) -> timer running -> AmpPowerUp (reverses R1: the amp needs data on UAIF1 for
  PUP_DONE). Defaults AboxEnable = AmpEnable = 1 now that the path is device-validated in UEFI.
  Rejected: a 4-channel endpoint (zero-copy) - the user would see a quad-speaker device and 16-bit would go.
- B1 code: AboxHw.c = validated stage 1 (table, saved/restored), AboxStart (SPK_EN, RDMA0 phys ring, VOL 0x800000, CTRL0 0x00401B01; refuses anything but 48 kHz 4 x 32), AboxStop (+ AboxDmaStuck if progress never clears), AboxRelease, AboxFrameCount (STATUS[11:0]); AboxPosition (wrong RBUF_OFFSET decode) removed. ReleaseHardware: AmpRelease then AboxRelease. Self-check vs AudioSpeaker.c :541-604: same writes, same order. Build exit 0.
- B2 code: AmpI2c.c = validated amp sequence: errata table, OTP unpack ported line-for-line (AmpOtpMap.h = AudioAmpOtp.h, 99 rows, count checked), config rows 3-5 (same order as AudioSpeaker.c :305-333), power-up with PLL_LOCK ack only and 0x5555/0xAAAA pup keys, power-down runs every row (W1C after the PDN poll even on timeout, 2 ms mute ramp), fixed digital volume -10 dB, earpiece only with Parameters\Stereo = 1 (default 0), amp flags cleared when AmpInit pulses a reset. Build exit 0.
- B3 code: Render.c stream rewritten: client packets (cached, CPU-only) + separate non-cached DMA ring (4 periods x frames x 16 B, < 4 GiB) filled by the packet timer one period ahead (slot 0 silence, slot 1 = current packet at Run; boundary j copies packet k0+j into slot j+1); mono (L+R)/2 in slot 1 unless Stereo; clock = RDMA STATUS[11:0] accumulated (QPC fallback); Run order AboxInit -> fill -> AboxStart -> timer -> AmpPowerUp (reverses R1, per boot #98/#99); position excludes the silent first period; GetHwLatency reports one period. CacheMaint parameter removed (no cached DMA memory left); defaults AboxEnable = AmpEnable = 1; driver version 1.0.1.0. Build exit 0.
- Review of B3 (2469e03), finding R3: the milestone edit went through PowerShell Set-Content -Encoding utf8, which added a BOM and rewrote all 135 lines' endings. Fixed by rebuilding the file from the HEAD~1 blob + the one row, written as UTF-8 without BOM: git diff HEAD~1 --stat now shows 1 insertion.
- Review of B1-B3 (substance): checked the slot timeline (Run: slot 0 silence, slot 1 = k0; boundary j copies k0+j
  into slot j+1 while the RDMA reads slot j - one full period of lead, never the slot being read); frame counter
  wraps every 85 ms vs a 10 ms timer and is accumulated under a spin lock shared with position queries; the DMA ring
  is written only with aligned 32-bit volatile stores (no memset/memcpy on non-cached memory); teardown order
  amps -> SPK/RDMA -> stage-1 restore on every path (Pause, ReleaseHardware stream, FreeRtPackets, cleanup,
  D0Exit, device ReleaseHardware); stage-1/2/teardown register values diffed by eye against AudioSpeaker.c
  :541-604 and the amp rows against :289-358. No further findings.
  Accepted, noted: stage 1 (BCLK keep-alive) stays on from the first stream until ReleaseHardware (amps idle at
  GLOBAL_EN 0 meanwhile); AboxDmaStuck is sticky (every later ring is leaked after one stuck stop) - conservative.
- A2: Dsdt.asl Device (AUD0) _HID SAMS9827, _CCA 0, _STA 0x0F, 8 Memory32Fixed in AUD_RES order (IFR, SYSREG, CMU_AUD 0x8000, GPIO_AUD 0x1000, HSI2C38 0x1000, SRAM 0x59000, PMU 0x15861000 page, PERIC1 GPIO 0x10830000). iasl 20260408 (%TEMP%\acpica-iasl-20260408) on a scratch copy: Compilation successful, 0 errors, 0 warnings; disassembly shows the 8 descriptors with the right bases/sizes. Scripted overlap check of the 8 windows vs the 16 other Memory32Fixed in the DSDT: none (hsi2c_38 0x109A0000 is not claimed elsewhere). The firmware FD itself was not rebuilt here (main session merges DSDT with its UEFI work).
