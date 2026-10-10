# edk2 BootShim + DT handling — research notes (for the 9820 DT LOAD fail)

Started 2026-10-10. Goal: find how prior art satisfies the 9820 (Android-12 ABL) `load_kernel`
DT check, so the beyond0lte UEFI container boots past `dt table header check fail`.

## Findings

1. **This repo's parent lineage does nothing special for the DT.** The 9810 project
   (ntdevlabs/exynos9810-woa) bootshim.S is byte-identical in shape to ours; its container
   (star2lte TWRP) carries a **DTBH** section — `tools/twrp-winre/build.py:407` reports
   `dtbh {len(boot.dt)}` and keeps it verbatim. 9810 sboot accepts that DTBH.
   Source: https://deepwiki.com/ntdevlabs/exynos9810-woa/4.2-boot-shim-and-firmware-build-pipeline

2. **edk2-exynos BootShim has no DTB either.** The minimal edk2-msm BootShim.S
   (fake Linux header: text_offset/image_size/flags all 0, magic "ARM\x64", "EDK2-MSM")
   appends no device tree. So the DT is NOT handled in the shim — it's in the boot-image
   PACKAGING.
   Source: https://raw.githubusercontent.com/sonic011gamer/edk2-exynos/master/tools/BootShim/BootShim.S

3. **The packaging is a per-device hook.** edk2-exynos `build.sh` calls
   `_call_hook platform_build_bootimg` (and `platform_build_kernel`), sourced from
   `Platform/<Vendor>/<soc>/<device>.sh.inc`. That hook is where the boot image (and its
   DTBH/dtbo) is assembled for the specific SoC.
   Source: https://raw.githubusercontent.com/sonic011gamer/edk2-exynos/master/build.sh

4. **The 9820 containers have no embedded DTBH.** Probed this repo's own stock + TWRP
   beyond0lte containers: all `dt_size=1`, no DTBH/DTBO magic. Android-12 keeps the trees in
   separate `dt.img`/`dtbo.img` partitions (see notes/m8-uefi-image.md B11). So the DT LOAD
   fail is about the container/kernel DT association under the Android-12 ABL, not an appended
   FDT (that theory, B12, was disproven in B15).

## Open question (the actual fix)

How does edk2-exynos' `platform_build_bootimg` hook for the exynos9820/beyond0lte device present
the DT so sboot accepts it — a DTBH in the container, a separate dtbo, or a different boot path?
NEXT: read that hook (`Platform/Samsung/exynos9820/...sh.inc` or the device `.conf`) in edk2-exynos.

## ANSWER - the exynos9820 bootimg hook (fetched 2026-10-10)

`Platform/Samsung/exynos9820/s10.sh.inc` `platform_build_bootimg()` runs AOSP `mkbootimg.py`:
```
--header_version 2  --pagesize 2048  --base 0x10000000
--kernel_offset 0x8000  --ramdisk_offset 0x1000000  --second_offset 0xf0000000  --tags_offset 0x100
--dtb_offset 0x0  --dtb "Platform/Samsung/exynos9820/FdtBlob_compat/${DEVICE}.dtb"
--ramdisk ".../dummyramdisk"  --kernel "${WORKSPACE}/uefi-${DEVICE}-kernel"
```
KEY: the container is an **Android boot image header VERSION 2** with a real **DTB section**
(`--dtb` = a device-tree blob, `FdtBlob_compat/s10.dtb`). NOT the Samsung header-v0 + DTBH that
our `bootimg.py`/`repack-with-ref.py` produce (which leave `dt_size=1`). sboot's Android-12
`load_kernel` "dt table header check" wants the v2 DTB section; our v0 container fails it.
The kernel blob is still `[shim][FD]` (same shape as ours) - so the shim/payload is fine; the
CONTAINER FORMAT is the gap.
FIX: build the beyond0lte candidate as a **header-v2** boot image with a proper DTB section
(our `bootimg.py` is v0-only - needs a v2 builder, or use AOSP `mkbootimg.py`). DTB source:
`FdtBlob_compat/s10.dtb` (their compat tree) or our `stock/dt.dtb` (239680 B).
Source: https://raw.githubusercontent.com/sonic011gamer/edk2-exynos/master/Platform/Samsung/exynos9820/s10.sh.inc

## Sources

- https://deepwiki.com/ntdevlabs/exynos9810-woa/4.2-boot-shim-and-firmware-build-pipeline
- https://github.com/sonic011gamer/edk2-exynos
- https://raw.githubusercontent.com/sonic011gamer/edk2-exynos/master/tools/BootShim/BootShim.S
- https://raw.githubusercontent.com/sonic011gamer/edk2-exynos/master/build.sh
- repo: tools/twrp-winre/build.py:407 (dtbh), bootimg.py:40-84 (DTBH layout, 'DTBH' magic)
