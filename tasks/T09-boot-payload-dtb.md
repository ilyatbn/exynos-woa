# T09 — Append a valid DTB to the boot payload so sboot's load_kernel passes the DT check

Status: **doing — shim flags + packer DTB-append implemented; candidate built; second boot not yet attempted**

## Goal

The first boot (T07) died at sboot's `load_kernel`: `dt table header check fail` ->
`could not do Normal boot (DT LOAD fail)` -> download mode. The 9820 runs an Android-12 ABL
whose `load_kernel` reads the kernel's ARM64 Image header, loads `image_size` bytes, then
expects a **DTB (FDT magic 0xD00DFEED)** at the next page-aligned offset and checks its header.
Our `[shim][FD]` EFI payload has `image_size 0x800000` but only ~0x700000 of payload and zero
padding after, so the DT header check fails and sboot never jumps. Fix the payload.

## Known (reads, not inference)

- `notes/m8-uefi-image.md` B10: the TWRP ref we repacked has `dt_size=1`, no DTBH/DTBO.
- B11: the **stock** `recovery.img`/`boot.img` also have `dt_size=1`, no DTBH/DTBO — Android-12
  keeps the trees in separate `dt.img`/`dtbo.img` partitions. So the container ref is NOT the fix.
- B12: the 9810 `bootshim.S` and the 9820 `beyond0lte-bootshim/bootshim.S` have an **identical**
  Image header (text_offset 0x80000, image_size 0x800000, flags 0xa, no appended DTB), yet star2lte
  boots. So the difference is the loader generation, not the shim shape.

## Next actions

1. Get a valid DTB: the vendor `dtbo.img`/`dt.img` from `C:\Users\ilya\code\Dumps\AP_..._meta_OS12.tar`
   (LZ4 members), or the phone's live FDT (`/sys/firmware/fdt`) read in a TWRP root shell.
   DONE (vendor route): extracted + LZ4-decompressed; `dt.img` FDT at 0x40 totalsize **239680**
   (`stock/dt.dtb`), `dtbo.img` FDT at 0x100 totalsize **176648** (`stock/dtbo.dtb`). Primary tree
   = `dt.dtb` (239680 B).
2. Decide the exact contract sboot wants: DTB appended after the kernel Image at the page-aligned
   offset past `image_size`, and whether the ARM64 header `flags` bit4 (0x10 "can use device tree")
   and the DTB-alignment bits (bit8) must be set. Keep every value UNVERIFIED until a boot confirms.
3. Rebuild the candidate as `[shim][FD][DTB]` (pack-uefi-beyond0lte.ps1 + the shim header), then
   re-attempt the boot (T07 consent + vbmeta already in place).

## Done when

The payload carries a DTB sboot accepts and the boot gets past `load_kernel` — the panel shows
UEFI output instead of the download-mode DT-error.
