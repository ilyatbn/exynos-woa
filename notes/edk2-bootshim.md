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

## Sources

- https://deepwiki.com/ntdevlabs/exynos9810-woa/4.2-boot-shim-and-firmware-build-pipeline
- https://github.com/sonic011gamer/edk2-exynos
- https://raw.githubusercontent.com/sonic011gamer/edk2-exynos/master/tools/BootShim/BootShim.S
- https://raw.githubusercontent.com/sonic011gamer/edk2-exynos/master/build.sh
- repo: tools/twrp-winre/build.py:407 (dtbh), bootimg.py:40-84 (DTBH layout, 'DTBH' magic)
