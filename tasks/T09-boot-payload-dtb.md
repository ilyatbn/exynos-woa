# T09 — Append a valid DTB to the boot payload so sboot's load_kernel passes the DT check

Status: **done** (2026-10-10, device-validated)

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

## Research reframe (2026-10-10, notes/edk2-bootshim.md)

The DT is NOT handled in the shim (edk2-exynos BootShim has no DTB; the 9810 lineage relies on a
container **DTBH**). The fix lives in the boot-image PACKAGING: edk2-exynos `build.sh` delegates to a
per-device `platform_build_bootimg` hook that assembles the container's DTBH/dtbo for the SoC.
The 9820 containers have no embedded DTBH (Android-12 separate dt.img/dtbo.img). NEXT: read edk2-exynos'
`platform_build_bootimg` hook for exynos9820/beyond0lte to learn the exact DT presentation sboot
wants, then mirror it in `pack-uefi-beyond0lte.ps1`/`repack-with-ref.py` (a DTBH section in the
container, not an appended FDT in the kernel).

## ANSWER (fetched the hook) - the fix is a header-v2 container with a DTB section

`Platform/Samsung/exynos9820/s10.sh.inc` builds the image with AOSP `mkbootimg.py --header_version 2`
and a real `--dtb` section (`FdtBlob_compat/s10.dtb`), kernel = `[shim][FD]`, ramdisk = dummy,
base 0x10000000, kernel_offset 0x8000, pagesize 2048. So sboot's Android-12 `load_kernel` DT check
wants an **Android boot image header VERSION 2 with a valid DTB section** — our v0 Samsung container
(`dt_size=1`) fails it. The shim/payload shape is fine. FIX: build the beyond0lte candidate as a
header-v2 boot image with a DTB section (our `bootimg.py` is v0-only → needs a v2 builder or AOSP
mkbootimg). Full recipe in `notes/edk2-bootshim.md`.

## Done when

The payload carries a DTB sboot accepts and the boot gets past `load_kernel` — the panel shows
UEFI output instead of the download-mode DT-error.

## Attempt 1 result (2026-10-10, boot #2)

Appended raw `dt.dtb` (FDT) at align(image_size=0x800000) + flags 0x1a (bit4). Boot #2 STILL
`dt table header check fail` -> download mode (AVB now bypassed: `VERIFICATION_DISABLED` bit set).
So the placement/flag guess is wrong. Next: mirror the stock kernel Image's actual header +
appended-DTB layout (`stock/boot.img` kernel blob) as ground truth. See `notes/m8-uefi-image.md` B14.

## Closed (2026-10-10)

The DT check is passed by an Android boot image **header v2 with a real DTB section**
(`firmware/tools/build-bootimg-v2.py`, edk2-exynos recipe) written to the **BOOT** partition
(sda14) - the recovery slot does not work (notes/m8-uefi-image.md B16-B21). The appended-DTB
approach this task started with was wrong (B15). Every boot from #4 to #17 passed the check.
