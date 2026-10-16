# Parking lot: D3D feature level 11_0 (Chrome/Edge hardware video, and more)

Status: **parked** (user 2026-10-17: "Windows first ... add a parking lot ticket explaining in detail everything you
have ... about feature level 11 for chrome"). Windows-app video (MF / msmpeg2vdec through a UMD DXVA bridge) is
being done first; HDMI (G18) is a must. This ticket is the full brief for picking FL 11_0 up later.

## Why FL 11_0 matters
- **Chrome/Edge hardware video decode** is impossible below FL 11_0. Chromium main
  `media/gpu/windows/d3d_video_decoder.cc`:
  `bool IsFeatureLevelSupported(ComD3D11Device device) { return device && device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0; }`
  checked in `GetSupportedVideoDecoderConfigs` (returns `{}`) and `AcquireDeviceResources`
  (`kUnsupportedFeatureLevel`). It is the only Windows hardware decoder
  (`media/mojo/services/gpu_mojo_media_client_win.cc` `CreatePlatformVideoDecoder`). No switch bypasses it; the
  only workaround (`disable_d3d11_video_decoder`) turns decode off. The D3D12 backend (`kD3D12VideoDecoder`) is
  FEATURE_DISABLED_BY_DEFAULT and needs a FL 11_0 D3D12 device anyway.
- MediaFoundationRenderer accepts FL 10_1 but clear playback is behind `kMediaFoundationClearPlayback`
  (DISABLED_BY_DEFAULT, "for testing purposes") -> a flag, not a native solution (AGENTS.md rule 12). Encrypted MF
  playback is x86_64 only (`media/base/win/mf_feature_checks.cc`).
- Side benefits: games/apps that need FL 11, Chromium GPU rasterization would still be blocklisted for our vendor
  id (software_rendering_list entry 122 - independent of FL), Graphite still needs FL 12 (entry 188).

## What Chrome needs from the decoder once FL 11_0 exists
- `ID3D11VideoDevice1` (device_.As) and `ID3D11VideoContext1` (CHECK_EQ - crashes the GPU process if missing).
- Profile `D3D11_DECODER_PROFILE_H264_VLD_NOFGT`; configs probed with NV12 at 1920x1080+; `ConfigBitstreamRaw == 2`
  (DXVA_Slice_H264_Short, slices with 00 00 01 start codes); `ConfigDecoderSpecific & (1 << 14)` = one texture per
  picture instead of an array.
- Output textures `DXGI_FORMAT_NV12`, `D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE`,
  `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED` (kD3D11VideoDecoderUseSharedHandle default on);
  zero-copy: viz/Skia samples the NV12 planes (no VideoProcessorBlt unless `disable_dxgi_zero_copy_video`).
- Per frame: DXVA_PicParams_H264, DXVA_Qmatrix_H264, slices. No SPS/PPS NALs.
- The UMD DXVA bridge being built now for MF apps (stateless DXVA -> stateful MFC: SPS/PPS synthesis from PicParams +
  Qmatrix + slice-header pps_id, MFC display delay 0, copy into the output view at EndFrame, POC type 1 unsupported)
  is exactly what Chrome would use - FL 11_0 is the only missing gate.

## What FL 11_0 requires (MS Learn "Direct3D feature levels" table, 11_0 vs our 10_1)
- Shader model 5.0 (vs 4.x): new bytecode (SM5 tokens: UAV declarations/ops, atomics, structured/raw buffers,
  interfaces/class linkage, bit ops, double precision optional), hull + domain shaders (tessellation), compute shader
  CS_5_0 (vs optional CS_4_x), 8 UAV slots (PS + CS only at 11_0; "UAVs at every stage" is 11_1), BC6H/BC7,
  max texture 16384 (vs 8192), max cube 16384, typed UAV loads limited (3 formats), DrawIndirect / DispatchIndirect,
  read-only depth views, ...
- WDDM side: D3D11 DDI with pipeline level 11_0 caps (D3D11DDI_3DPIPELINELEVEL_11_0), D3D11DDICAPS_SHADER etc.

## Where our stack stands (2026-10-17)
- UMD = Mesa d3d10umd frontend, extended to the D3D11.1 / WDDM1.3 / WDDM2.0 DDI tables (Mesa 0070-0081), pipeline
  level 10_1. The shader translator (`ShaderParse.c` / `ShaderTGSI.c`) knows SM 4.0/4.1 tokens only - no SM5 opcode,
  no UAV, no hull/domain/compute stage. TGSI -> NIR (`tgsi_to_nir.c`, sampler-view opcodes added by exynos-woa).
- panfrost on G76 (Bifrost v7, `pan_screen.c`): compute = true, cube_map_array = true, max_shader_images /
  buffers only where side effects are allowed (compute + fragment - matches FL 11_0's PS+CS UAVs), BPTC formats in
  `panfrost/lib/pan_format.c` (BC6H_SF16/UF16, BC7_UNORM - hardware or decode path to verify),
  max_texture_2d_size = 1 << (PAN_MAX_MIP_LEVELS - 1) (to verify >= 16384). **No geometry and no tessellation
  stages** (Mali-G76 has no tessellation hardware; panfrost exposes GLES 3.1-class without GS/tess).
- Mesa ships `src/poly/` (geometry.h, tessellator.h, prim.h, nir/ - the geometry/tessellation emulation through
  compute used by Asahi): the candidate to emulate HS/DS (and GS - see the gap below) on panfrost. INFERENCE: needs
  porting effort; asahi wires it through its own driver paths.
- **Existing FL 10_x gap**: FL 10_0/10_1 require geometry shaders and stream-out; panfrost has none. d3d10umd will
  fail any app that uses a GS today. Not seen in DWM/Edge/Terminal so far. Same `src/poly` route would fix it.
- D3D lod (SM4.1) is wrong on Bifrost (d3dsm41 7/8; panfrost advertises texture_query_lod only for v9+).
- WebGL MSAA off: ANGLE stops its sample-count loop at our 2x = 0 (panfrost refuses 2x before arch 12).

## Rough work breakdown if picked up
1. SM5 parser/translator in d3d10umd (or switch the frontend: Mesa's newer D3D work - check upstream for an SM5-
   capable translator; dxil is the other direction). Compute stage + UAVs + atomics first (CS on panfrost works).
2. Hull/domain shaders via `src/poly` tessellation emulation on panfrost (largest risk; performance unknown).
3. Geometry shaders via the same library (also closes the FL 10 gap).
4. D3D11 DDI entry points for CS/HS/DS/UAV binding (generated stubs exist: `tools/mesa-build/gen-d3d11-ddi.py`).
5. Report FL 11_0; test with D3D11 conformance-style tools (d3dsm41-style Mali-vs-WARP tests per feature).
6. Chrome: its decoder then uses the DXVA bridge; check `ID3D11VideoContext1` coverage.
Estimate: weeks to months; tessellation emulation is the uncertain part.

## Sources
- notes/gpu-browser-video-research.md (G17 research 2026-10-17 section), notes/gpu-umd-d3d11.md (FL 10_1 plan and
  log), Chromium main d3d_video_decoder.cc / d3d11_video_decoder_backend.cc / gpu_mojo_media_client_win.cc /
  media_switches.cc / mf_feature_checks.cc, MS Learn overviews-direct3d-11-devices-downlevel-intro,
  d3d11-video-playback-improvements, nc-d3d10umddi-pfnd3d10ddi_retrievesubobject.
