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
