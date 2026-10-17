# G26 - BC1-BC5 re-compressed to ETC2/EAC on upload (G23 step 2)

User 2026-10-18: NFSU2 got in-game (dgVoodoo windowed borderless fullscreen-size), then crashed loading the race:
32-bit address space out (SPEED2.EXE is NOT large-address-aware -> 2 GB; PE characteristics 0x010f), UMD BOs ~600 MB
in 7783 BOs (tlog malikm-SPEED2.EXE-6272: CB FAIL Allocate 0x8876017c, create-bo FAIL 413696, then Panfrost
`assert(staging)` pan_resource.c:1610). G23 step 1 decodes BC to RGBA8 (BC1 8x, BC2/3 4x). User: "nah, do B" (re-
compress to ETC2, not smaller decode formats); "make something that ... causes this crash, then do your thing and repro
the fix" -> a 32-bit BC memory stress test before/after.

## Research
- This Mali: TEXTURE_FEATURES0 0xf7fe007e (gpuinfo): bits 1-4 (ETC2 RGB8, ETC2 R11 UNORM, ETC2 RGBA8, ETC2 RG11 UNORM)
  and 17-19 (ETC2 R11 SNORM, RG11 SNORM, RGB8A1) set; BC bits 7-16 clear. (genxml v7.xml texture format values.)
- Panfrost pan_format.c has FMTC entries for all of them; pan_query_compressed_formats = TEXTURE_FEATURES0.
- Mapping (all same size as the BC source, same 4x4 block grid and row pitch):
  BC1 (8 B, 1-bit alpha possible) -> ETC2_RGB8A1 (punch-through); BC2/BC3 (16 B) -> ETC2_RGBA8 (EAC alpha + ETC2 RGB);
  BC4 U/S (8 B) -> ETC2_R11 U/S (EAC); BC5 U/S (16 B) -> ETC2_RG11 U/S.

## Progress log
- Encoder written: Mesa d3d10umd BcEtc.c/.h (ETC1 individual + differential, both flips, closed-form table/modifier
  choice; punch-through differential with index 2 transparent; EAC alpha/R11/signed R11 with per-table multiplier from
  the range). Layouts from src/mesa/main/texcompress_etc.c (etc2_rgb8_parse_block / fetch, alpha8, r11, signed r11) and
  util/format/texcompress_etc_tmp.h (etc1 tables, base colours).
- PC test tools/mesa-build/bcetc-test (reference decoder ported from Mesa's; 20000 blocks per case), first run:
  rgb solid 40.85 dB, gradient 36.44, bc1-like soft 36.36, bc1-like HARD (random far endpoints) 18.13, rgb8a1 opaque
  36.34, punch-through 35.86 (transparency exact), EAC alpha smooth 48.45 / bc4-like 41.64, R11 u 48.35 / 41.77,
  R11 s 48.34 / 41.77; T/H/planar produced 0. Floors are regression floors set from this run (hard case 17: the
  ETC1-mode limit; follow-up: ETC2 T/H modes for two-colour blocks).
- UMD integration (Mesa 0123): FormatBcBacking -> bcetc_target (sRGB twin via util_format_srgb) when the screen samples
  it, else the G23 RGBA8 backing; bc_unpack_rect transcodes straight into the mapped ETC blocks
  (util_format_is_compressed(backing)). D3D10UMD_BC_DECODE=1 = old decode backing (diagnosis / A-B only, rule 12).
  BcEtc.c added to meson. x86 build mali-umd-0123-x86 (artifact dir reused, now the BC->ETC build).
- d3dbc gained "lossy" (PSNR vs WARP over the format's channels; BC1 1-bit alpha exact; random + soft data). x86 run 1
  (traces/games/d3dbc-lossy-1.txt): 125/126 ok; 1-bit alpha matched everywhere; soft-data minimum per format: BC1
  40.33 dB, BC1 sRGB 37.89, BC2 32.33, BC3 36.48, BC3 sRGB 34.81, BC4 U 35.32 / S 38.97, BC5 U 37.90 / S 38.80.
  The one miss: random-data BC2 3x3 mip 12.10 dB (random explicit alpha = noise) -> random floor 14 -> 10 dB (it only
  guards against broken layouts).
- d3dbcmem (32-bit memory repro, drivers/MaliG76Wddm/tools/d3dbcmem): G23 decode backing (D3D10UMD_BC_DECODE=1,
  diagnosis): ~260 1024x1024 BC1/BC3 textures (~260 MB BC) then "Allocate 0x8876017c", create-bo FAIL 6148096, process
  dies - the NFSU2 signature reproduced (traces/games/d3dbcmem-decode.txt); upload ~850 ms/texture.
  ETC path v1: upload ~4000 ms/texture (too slow). PC bench (tools/mesa-build/bcetc-test/bench.c, x64 native, 87381
  blocks = 1024x1024+mips): rgb 224.5 ms, rgb8a1 104.0 ms, eac 208.8 ms -> ~9x slower under the phone's x86 emulation.
  Plan v2: palette-based fit (BC block = <=4 colours / <=8 alpha levels: fit per distinct value with counts), own BC
  decoders (no per-texel generic unpack), individual mode only when differential clamps, EAC binary search over the
  sorted decoded levels + flat shortcut.
- d3dbcmem ETC v1 run cut off by the 900 s ssh timeout at ~120 textures (4 s/texture), no failure until then (traces/games/d3dbcmem-etc-v1.txt). Encoder v2 (palettes): PC bench BC1 56 ms, BC3 129 ms, BC5 167 ms per 1024x1024+mips (x64 native); bcetc-test quality unchanged within 0.05 dB. Next: parallel transcode (thread pool, block-row bands).
- Mesa 0124-candidate (v2 palette encoder + thread-pool bands, mali-umd-0124-x86): d3dbc lossy soft minimums BC1 40.33,
  BC1 sRGB 37.89, BC2 32.30, BC3 35.92, BC3 sRGB 34.84, BC4 U 34.88 / S 36.59, BC5 U 37.66 / S 38.67 (own BC decoders
  agree with WARP); 1-bit alpha exact (traces/games/d3dbc-lossy-2.txt).
- d3dbcmem ETC v2: RESULT 600 textures fit (600 MB of BC data; the cap) vs ~260 then a crash on the decode path ->
  the address-space failure is fixed (traces/games/d3dbcmem-etc-v2.txt). Upload ~700 ms/texture after warm-up (mean
  950) - no faster than the decode path's ~850: timing where it goes next.
- Upload timing (tlog "BC upload", 1024x1024+mips): map 1-7 ms, unmap 6-16 ms, transcode = everything. Threads (thread-pool
  bands, 6 processors): BC1 1944 -> 530 ms, BC3 3772 -> 1000 ms (3.7x; D3D10UMD_BC_THREADS=1 diagnosis run). Phone
  single-thread x86-emulated is ~35x the PC's 32-bit build (bench x86: BC1 54 ms, BC3 143 ms). PC x86 vs x64: ~20%.
- v3 encoder changes: no 64-bit math (BC alpha decode, EAC packing), flip chosen by sub-block variance (one flip
  fitted; -0.15 dB in bcetc-test). EAC table wins are spread over all 16 tables (stats.c, 200000 blocks per kind) -> no
  table pruning; next: sorted merge-walk for the nearest level, shifts instead of divisions.
- v3 EAC: values sorted once per block, one merge walk per table; 16.16 reciprocal for the multiplier, shifts for the
  base (every table has tmin + tmax = -1); PC x86 bench BC1 54.3, BC3 119.6, BC5 159.7 ms; bcetc-test identical.
- Final x86 (mali-umd-0123-x86, Mesa commit 01b4102 = patch 0123): d3dbc lossy 0 of 126 differ -> PASS
  (traces/games/d3dbc-lossy-3.txt, no 1-bit alpha mismatch); decode backing (D3D10UMD_BC_DECODE=1) d3dbc exact 63/63.

## Review of 13563c6 / Mesa 0123 (AGENTS rule 4)
- Mechanical: patches 0111-0123 `git am` cleanly onto 5c8935c and reproduce the Mesa branch byte for byte.
- Thread pool: the job struct lives on the caller's stack and the caller waits (WaitForThreadpoolWorkCallbacks) before
  returning; surplus callbacks find no band and return - holds. Bands write disjoint block rows.
- Block geometry: ETC2/EAC blocks = BC block size (8 / 16 B) and 4x4 grid, so texture_map strides and the block-aligned
  boxes (bc_clamp_box) carry over; mips < 4 texels -> one block - holds (d3dbc 3x3 / 6x6 mips pass).
- OPEN (pre-existing G23 limits, not regressions): (1) CopyResource between a BC texture and a 64-bit UINT texture
  (D3D's format-reinterpreting copy) copies ETC bits, not BC bits; (2) Map(READ) of a BC texture returns the CPU shadow,
  stale if the GPU wrote the texture by a copy. No test case / no app seen doing either.
- OPEN: the commit message's "600 fit" is the v2 run; the v3 run (same allocation behaviour) is in progress.
