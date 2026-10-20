# G35 - hardware video in browsers (Edge, then Brave) on the MFC through the FL 11_0 UMD

User 2026-10-20: "Hardware video is a go. Use edge and then test on brave as well. When thats done do as much prep
work as possible on hdmi (via type c adapter and usb hub, i have both)."

## Base
- FL 11_0 shipped (G34, Mesa 0218). Chromium's only Windows HW decoder (D3D11VideoDecoder) requires FL >= 11_0
  (docs/exynos9820-port/parkinglot/fl11-chrome-video.md) - that gate is now open.
- G17 DXVA bridge (notes/g17-dxva-mfc.md): D3D11 video DDI in the UMD -> MFC IOCTLs, H.264 VLD_NOFGT, NV12 output,
  GPU video processor; MF apps (mpv, Photos) decode on the MFC.
- What Chromium additionally needs (parking-lot brief): ID3D11VideoDevice1 / ID3D11VideoContext1 (CHECK - GPU
  process crash if missing), ConfigBitstreamRaw 2, ConfigDecoderSpecific bit 14 (texture per picture), NV12 output
  textures BIND_DECODER|BIND_SHADER_RESOURCE + MISC_SHARED_NTHANDLE|MISC_SHARED, zero-copy NV12 plane sampling.
- Test clips on the phone: C:\S10eTmp\mfc\test.mp4 (1080p24 H.264, 17.5 s), c720.mp4, c1080.mp4, clips\*.mp4.

## Progress log
- 15:34 Edge (default profile, sandboxed) plays C:\S10eTmp\g35\play.html (test.mp4 1080p24, tools/phone/g35/
  browservideo.ps1 + videorun.sh): 14.1 fps, MFC probe (mfcdec2, one MFC session device-wide) still succeeds -> Edge
  does not hold the MFC = software decode (traces/g35-edge-1.png). GPU process args carry --gpu-recent-crash-count=1.
- The sandboxed GPU process writes no UMD log (C:\S10eTmp\tlog not writable from the sandbox). Diagnosis instance
  (tools/phone/wddm2/edgecdp.ps1 -Extra, comma-separated switches; separate profile; tracing only, rule 12):
  - CDP SystemInfo videoDecoding = 0 profiles both sandboxed and --no-sandbox (traces/g35-cdp-*.txt) - that list is
    not what playback uses (see below), ignore it.
  - Chromium source (googlesource main, media/gpu/windows): FormatSupportChecker needs ID3D11VideoDevice1 +
    CreateVideoProcessorEnumerator + CheckFormatSupport(NV12) succeeding; TextureSelector picks NV12 "binding
    textures" (zero copy) without format bits unless a copy is forced.
  - tools/phone/g35/cdp_nav.py (new: DevTools Media domain = media-internals) on the clip, --no-sandbox instance
    (traces/g35-media-1.txt): "Use D3DVideoDecoder ... Selected NV12 ... binding textures ... using array texture ...
    Selected D3D11VideoDecoder", config change to coded 1920x1088, then "CreateVideoDecoder failed: Unspecified error
    (0x80004005)" -> fallback FFmpegVideoDecoder.
  - UMD log of that GPU process (traces/g35-edge-gpu-1088.txt): decoder 1920x1080 created + MFC session open, then a
    second decoder 1920x1088 while the first still exists -> "IOCTL_MFC_OPEN failed 170" (ERROR_BUSY: one MFC
    session) -> E_FAIL. ROOT CAUSE 1: the session is opened in CreateVideoDecoder.
  - Then two NV12 textures (R8 1920x1620, one AFBC, one u-interleaved) and the GPU process aborts:
    "Assertion failed: util_format_get_blocksize(view_format) == util_format_get_blocksize(img_format)"
    (panfrost pan_image.h:262) = an R8G8 view on our single-R8 NV12 surface. ROOT CAUSE 2: D3D NV12 plane views
    (R8 = luma plane, R8G8 = chroma plane) are not implemented; this also explains the crash counts.
- Plan: T1 Mesa 0219 - open the MFC session at the first DecoderBeginFrame (Chromium replaces decoders without using
  the first). T2 Mesa 0220 - NV12 plane views (SRV/RTV R8 -> luma rows, R8G8 -> chroma rows at W/2 x H/2) + never
  assert on an unsupported view format (fail the view instead).
- T1 Mesa 0219 (ARM64 d424e351, umdswap arm64 tag 0219a; no reboot: a new Edge GPU process loads the new file, DWM
  keeps its copy - only Video.cpp changed): MFC session opened at the first DecoderBeginFrame. cdpplay.sh t1a (new;
  --no-sandbox instance): media log keeps "D3D11VideoDecoder" (no CreateVideoDecoder error); UMD: "decoder
  1920x1088 created", "MFC session open", frames 0-4 decoded on the MFC (~8 ms each, err 0). Then "SRV fmt 61" +
  "SRV fmt 49" (R8 / R8G8) on the NV12 array texture (r8 1920x1632 x19) -> the pan_image.h:262 assert -> GPU process
  dies -> "video decode error" / PIPELINE_ERROR_DISCONNECTED (traces/g35-media-t1a.txt, g35-gpu-t1a-8028.txt).
  => T1 verified; T2 (plane views) is the blocker now.
- Review of 0219: FINDING - CreateVideoDecoder no longer failed when the MFC is missing or denied to the process
  (e.g. a sandbox without the device ACE): MF / msmpeg2vdec chose DXVA by that result and would now fail per frame
  instead of keeping software. The driver allows any number of handles (Driver.c SessionOwner: only IOCTL_MFC_OPEN
  claims the session). Fix Mesa 0220 (ARM64 e07cbaca, tag 0219b): device handle opened at CreateVideoDecoder (E_FAIL
  as before when it cannot be opened), the session claimed at the first BeginFrame (HasSession; cleared when a
  restart loses it). Checks: mfd3d c1080 -Rows 1080 -> 60/60 MFC decodes, hashes identical to the system-memory
  decode; Edge (cdpplay t1b): decoders 1920x1080 + 1920x1088 both created, the 1088 one claims the session and
  decodes (frame 2 err 0), then the known plane-view assert (T2).
- T2 Mesa 0221 (NV12 plane views; ARM64 df192602, tag 0221a): MS Learn DXGI_FORMAT
  (https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format), NV12: "Valid luminance
  data view formats for this video resource format are DXGI_FORMAT_R8_UNORM and DXGI_FORMAT_R8_UINT. Valid chrominance
  data view formats (width and height are each 1/2 of luminance view) ... are DXGI_FORMAT_R8G8_UNORM and
  DXGI_FORMAT_R8G8_UINT. Supported view types are SRV, RTV, and UAV."
  - Design: NV12 stays one R8 surface (decoder copy / video processor / Map unchanged) but is created LINEAR through
    resource_create_with_modifiers (modifier constant); SRV/RTV/UAV with R8* / R8G8* on it use a per-(slice, plane)
    alias = panfrost_resource_create_alias (new, pan_resource.c): a linear resource with an explicit layout at
    (layer stride * slice + first_row * row pitch) of the base BO, BO shared by reference, alias_of = base. Writer
    tracking keyed by the base (pan_track_rsrc in batch_update_access / flush_writer / any_batch_writes_rsrc; batch
    users are BO-based already) so base/alias accesses order. Other view formats on NV12 fail E_INVALIDARG (no
    assert). Rejected alternatives: Gallium multi-plane NV12 (every nv12_height consumer + Map changes); a frontend-
    only alias without tracking (reads through a plane would not wait for the decoder's copy).
  - mfd3d c1080 on 0221a: 60/60 identical (linear decoder output). Edge t2a: aliases created ("NV12 1920x1088 slice 2
    plane 0/1 alias: ok"), SRVs on them, Chromium then uses our video processor (1920x1080 -> 834x470) and the GPU
    process aborts: assert count != 1 (u_inlines.h:114). Dump (WER, traces/dumps/msedge.exe.7228.dmp, cdb + the
    build's PDB, raw stack): tc_batch_execute -> panfrost_set_sampler_views -> pipe_sampler_view_reference on a freed
    view. Cause: VideoProc.cpp released its per-blit sampler view with pipe_sampler_view_reference (pre-TC code) while
    the threaded context's queued set_sampler_views still named it; everything else uses pipe->sampler_view_release.
- T2b Mesa 0222 (ARM64 bf2be4ed, tag 0222a): VideoProc releases the view through pipe->sampler_view_release.
  - cdpplay t2b (--no-sandbox): no crash, MFC probe BUSY (Edge holds the session), frames 100/200 decoded err 0,
    VP blts every frame (~15-35 us after the first), page 449 frames 22.7 fps dropped 4.
  - NATIVE Edge (default profile, sandbox, no switches; videorun.sh edge-hw1): MFC probe BUSY, page 568 frames
    23.6 fps dropped 2 (clip 24 fps; software decode was 14.1), picture correct (traces/g35-edge-hw1.png).
