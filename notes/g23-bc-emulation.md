# G23 - BC1-BC5 (DXT) texture emulation in the UMD

User 2026-10-17: "Task both. First uncompressed to see if it works and lands us ingame ... If we get into the game and
everything works we'll do the recompression later."
Why (notes/games-nfsu2.md 22:58): this Mali-G76 has no BCn texture decode (TEXTURE_FEATURES0 = 0xf7fe007e, bits 7-16
= BC1..BC7 clear); FL 10.x requires BC1-BC5; our CheckFormatSupport answers 0 for DXGI 70-84; dgVoodoo (and most PC
games) need them.
Step 1 (this task): accept BC1-BC5, back them with uncompressed formats, decompress on upload (Mesa util_format S3TC /
RGTC unpack). Step 2 (later): re-compress to a format the GPU decodes (ASTC/ETC2) to get the memory back.

## Progress log
- 23:10-23:30 Mesa 0107 (patches/0107-bc-emulation.patch): Format.cpp FormatBcBacking (DXT1/3/5 -> R8G8B8A8_UNORM, sRGB ->
  R8G8B8A8_SRGB, RGTC1 -> R8_UNORM/SNORM, RGTC2 -> R8G8_UNORM/SNORM, only when the screen cannot sample the BC format)
  + FormatBcDecode (sRGB DXT decoded with the UNORM variant: raw sRGB bytes, no linearise-to-8-bit banding);
  CheckFormatSupport adds SHADER_SAMPLE; CreateResource stores the backing + Resource::bc_format; initial data,
  UpdateSubresourceUP (box clamped to the mip), Map (compressed shadow per subresource) / Unmap (decode a write map),
  CopyRegion (boxes clamped) decode with util_format_translate via a padded scratch (edge blocks never overrun);
  SRVs use the backing (both creation paths). Mesa 0108: first 400 DDI calls per process in the tlog (diagnostic).
- VERIFIED: new test drivers/MaliG76Wddm/tools/d3dbc (x86 + ARM64 build): 9 formats (BC1/BC1_SRGB/BC2/BC3/BC3_SRGB/
  BC4/BC4_SNORM/BC5/BC5_SNORM) x 3 paths (init 12x12x3 mips incl. 6x6 / 3x3 edge mips, UpdateSubresource, staging Map
  + CopyResource) compared texel-by-texel with WARP (native BCn): traces/games/d3dbc-4.txt **0 of 63 differ -> PASS**
  (tolerance = one step of the 8-bit storage: 1/200 UNORM, 1/117 SNORM/sRGB; observed max 0.0039 UNORM, 0.0071 sRGB,
  0.0067 SNORM - storage quantisation, not decode errors).
  PITFALL found: a stale C:\S10eTmp\maliumd32.dll (16:20, first manual x86 install) was loaded by every 32-bit tool run
  from C:\S10eTmp (DLL search order: application dir first) -> renamed maliumd32-stale-1620.dll. Games run from their
  own folders were not affected (the SPEED2 logs show the SysWOW64 build).
- dgVoodoo/NFSU2 still fails: with BC reported, dgVoodoo's D3D11 init creates 42 pixel shaders (all fine, no
  SetError), then tears down without a single CreateVertexShader (DDI trace, traces/games/malikm-SPEED2.EXE-1668.txt);
  OutputAPI d3d11_fl10_0 the same; d3d11warp WORKS (game proceeds to EnumAdapterModes). Next call is refused above the
  UMD (runtime validation) or dgVoodoo's own check; the D3D11 debug layer (Graphics Tools) is unavailable (WU blocked).
- Review of Mesa 0107: checked - (1) UpdateSubresourceUP with a box: pSysMemUP points at the box origin (D3D rule), the
  decode starts there - right; box clamped to the mip. (2) array / cube layers: subResourceBox gives box.z = layer,
  depth 1 - the z loops decode one slice. (3) typeless BCn: FormatTranslate -> DXT*_RGBA -> RGBA8 backing; sRGB views
  of it reinterpret RGBA8 as SRGB (same layout). (4) sRGB: decode with the UNORM variant, store raw. Limitations (no
  fix now, noted): Map READ of a BCn subresource returns the shadow (zeros unless an earlier map wrote it); copies
  between a BCn and a non-BCn format (D3D10.1 reinterpret copies, e.g. BC1 <-> R32G32_UINT) are not emulated; memory 4x
  (BC2/3/5) to 8x (BC1/4) per texture - step 2 (re-compression to ASTC/ETC2) addresses that.
