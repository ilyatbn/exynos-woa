# T01 — Re-pick the UFS DMA window inside the live bank0

Status: **open**

## Goal

`UFS_DMA_WINDOW_FIRST` / `UFS_DMA_WINDOW_LAST` in
`drivers/Exynos9820Ufs/Exynos9820Ufs.h:910-911` are the 9810's FMP aperture
(`0x90000000..0xBC800000`) and are marked UNVERIFIED on 9820. The live DTB
proves that upper bound is **outside** the declared bank, so the window must be
re-picked from reads.

## Known (reads, not inference)

- bank0 = `0x80000000 + 0x3AB00000`, ends **`0xBAB00000`** — `notes/K10-rkp.md` T18
  (DTB `reg`) and confirmed by `/proc/iomem` `System RAM 80000000-baafffff`
  (`notes/k4b-drop-address.md` K01).
- The carried window overruns that end by **`0x1D00000`**.
- Reservations inside the candidate range (live DTB, `notes/s10e-dtb-mmio.md` S02):
  `sec_debug_magic 0x80000000+0x1000`, `kaslr 0x80100000+0x100000`,
  `el2_code 0x87000000+0x200000`, `mm_debug 0x91200000+0x100000`,
  `secure_camera 0x96000000+0x2200000`, `camera 0x99000000+0xE400000`
  (ends `0xA7400000`), `modem_spare 0xD0000000+0x1600000`.
- The enforce switch is compiled out: `UFS_DMA_WINDOW_ENFORCE 0`
  (`Exynos9820UfsDiag.h:72`), so a wrong window cannot reject a valid buffer
  today — it is a latent inconsistency, not a boot blocker.

## Next actions

1. Pick a window inside `0x80000000..0xBAB00000` that avoids the reservations
   above it (the obvious candidate is the band between the camera reservation end
   `0xA7400000` and the bank end `0xBAB00000`, or the gap below `mm_debug`).
2. Keep both macros marked **UNVERIFIED** until a device validation says
   otherwise — the repo rule forbids dressing a choice as a read.
3. Re-run the `C_ASSERT` set in the UFS package and `build-ufs.ps1`
   (`notes/windows-m7-toolchain.md` §4.5 has the working command line).

## Done when

The asserts compile clean with the new window AND the comment above the macros
names the read that bounds them.
