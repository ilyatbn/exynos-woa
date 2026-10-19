# G32: Unity games on the Mali - Dave the Diver, Schedule I (2026-10-19)

User: "im gonna try another game too ... dave the diver", then goal "run dave the diver without crashing".
Both are Unity (D3D11 on Windows), x64 -> maliumd_ec.dll (ARM64EC UMD behind the maliumd forwarder), not the GL ICD.

## Dave the Diver (Steam 1868140, Unity 6000.0.52 URP, IL2CPP, v1.0.6.2113.steam)
- Player.log (C:\Users\S10e\AppData\LocalLow\nexon\DAVE THE DIVER\Player.log, copy traces/games/dave-player.log):
  "Direct3D 11.0 [level 10.1]", Renderer "Arm Mali-G76 MP12 (Galaxy S10e, G10/G11)", VRAM 2955 MB;
  kGfxThreadingModeSplitJobs not supported on D3D11 -> ClientWorkerJobs; shaders not supported on this GPU:
  Hidden/CoreSRP/CoreCopy, Hidden/VoxelizeShader, Hidden/Universal/HDRDebugView; "RenderTexture.Create failed: format
  unsupported for random writes - R16 SFloat" (LensFlareCommonSRP, UAV = FL 11 feature). Reached LogoManager, then
  "Crash!!!".
- Crash 1 (12:08 phone time, launched by the user through Steam): Application Error 1000, maliumd_ec.DLL c0000005
  offset 0x80416c. Unity crash report (tools/phone/games/unitycrash.ps1 -> traces/games/dave-crash.dmp), cdb with the
  0148 EC PDB (s10e-artifacts\mali-g12-ec at the time, maliumd_ec a11ad698):
    READ AV at 000001bd`b9ba9000 (page end) in memcpy_unaligned, ldp q0,q1,[x1,#0x50] - x1 = the copy SOURCE
    util_copy_rect <- panfrost_ptr_unmap+0x5cc <- u_transfer_helper_transfer_unmap <- util_resource_copy_region
    <- tc_batch_execute (driver thread). Rows 0x17c0 B (1520 px x 4 B), 0x2d0 = 720 rows.
  = a CopySubresourceRegion of a 1520x720 4-byte depth/stencil texture (URP's depth copy) on the CPU path
  (panfrost_resource_copy_region keeps depth/stencil on util_resource_copy_region, H4); the destination's write-map
  goes through u_transfer_helper, and panfrost_ptr_unmap's u-interleaved -> linear conversion (after
  LAYOUT_CONVERT_THRESHOLD whole overwrites) copies from trans->map past its end. Root cause of the over-read inside
  the transfer-helper path not pinned down (the GPU path below avoids it entirely).
- Fix Mesa 0160: same-format, same-sample-count depth/stencil copies go through pctx->blit with the format's Z/S mask
  (panfrost's blitter does ZS), both resources DEPTH_STENCIL-capable. Also removes ~36 ms of uncached CPU copy per
  full-screen depth copy. Build ec: maliumd_ec 889d474a (s10e-artifacts\mali-umd-0160-ec, PDB kept there).
  Live: System32\maliumd_ec.dll = 0160; rollback = System32\maliumd_ec.dll.0148 (a11ad698). Not yet in the ARM64 /
  x86 UMDs (port after validation, memory rule).
- steam_appid.txt (1868140) next to DaveTheDiver.exe since the shortcut work (notes/games-steam.md).

## Progress log
- 20:25 0160 deployed live (rename trick: the old DLL renamed, new copied); waiting for the user's launch.
