# Exynos 9820 / Galaxy S10e ("d17") — Linux booting research notes

Started: 2026-10-08. Task: find any Linux (mainline kernel, ArchLinuxARM, buildroot, Debian) port or boot attempt on Exynos 9820 / Galaxy S10 / S10e / d17 devices; plus U-Boot/EDK2/WOA efforts, hardware diffs vs 9810, bootloader (LK4/LK5) notes.

Status: IN PROGRESS — findings appended incrementally.

Parent-project context: this repo (`exynos9810-woa`) runs Windows 11 ARM64 on the Galaxy S9+ SM-G965F (Exynos 9810, "star2lte") via EDK2 UEFI + Windows drivers (UFS, HSI2C/SpbCx, S6SY761 touch). The research here is the S10e / Exynos 9820 ("d17") analogue.

## Round 1 — web/GitHub search: "exynos 9820 linux", "galaxy s10e linux", "d17" trees

### FINDING (very promising): postmarketOS ships an Exynos 9820 kernel package
- `linux-postmarketos-exynos9820` version 7.2.0-r0, described as "Close-to-mainline kernel for Samsung Exynos 9820 devices", project URL `https://github.com/chiffathefox/exynos-9820-mainline-linux`, maintainer "chiffa", built 2026-07-31.
  Source: https://pkgs.postmarketos.org/package/main/postmarketos/aarch64/linux-postmarketos-exynos9820
- Repo `chiffathefox/exynos-9820-mainline-linux` exists on GitHub (created 2026-01-04, ~1.46M commits → looks like a kernel-history fork). Source: https://github.com/chiffathefox/exynos-9820-mainline-linux
- TODO next: find the postmarketOS *device* port (docs/`device-*` repo) and whether it actually boots on S10e hardware.

### FINDING: Samsung released S10/S10e/S10+ kernel sources (Android 4.14-based)
- XDA: "Samsung Galaxy S10, S10e, and S10+ kernel sources are available for the Exynos models" (SM-G970F/G973F/G975F) released on the Samsung Open Source Release Center (OSS). Source: https://www.xda-developers.com/exynos-samsung-galaxy-s10-s10e-s10plus-kernel-sources/ (2019-03-13)
- LineageOS wiki metadata: beyond0lte (Galaxy S10e, SM-G970F) → kernel repo `android_kernel_samsung_exynos9820`, kernel version `4.14`. Source: https://github.com/LineageOS/lineage_wiki/blob/main/_data/devices/beyond0lte.yml and https://wiki.lineageos.org/devices/beyond0lte/
- Linux4's builds list "Linux 4.14.345" etc. for beyond0lte → the S10e kernel is Linux 4.14-based (Android-patched), NOT 4.10. Source: https://lineage.linux4.de/changes/beyond0lte.html

### FINDING: Android kernel trees for exynos9820 (Samsung BSP + downstream maintainers)
- LineageOS/android_kernel_samsung_exynos9820 (53★, default branch lineage-23.2, has `build.config.universal9820`, `build.config.universal9820_5g`, `build.config.universal9825`). Source: https://github.com/LineageOS/android_kernel_samsung_exynos9820
- whatawurst/android_kernel_samsung_exynos9820 — "Linux Kernel for Samsung Galaxy S10e|S10|S10+ (Exynos)". Source: https://github.com/whatawurst/android_kernel_samsung_exynos9820
- GoRhanHee/android_kernel_samsung_exynos9820 (44★). Source: https://github.com/GoRhanHee/android_kernel_samsung_exynos9820
- gzvzp/samsung-exynos9820-9825 "Cruel Kernel Tree for Samsung S10, Note10" (default branch `cruel-HVJ5-v5.3`). Source: https://github.com/gzvzp/samsung-exynos9820-9825
- XDA thread "How to compile Samsung S10 kernel for Exynos 9820?" — build config name is `exynos9820-beyond2lte_defconfig`. Source: https://xdaforums.com/t/how-to-compile-samsung-s10-kernel-for-exynos-9820.3925740

### FINDING: Android device trees (codename mapping — S10e is "beyond0lte", not "d17" in AOSP land)
- ExtremeXT/android_device_samsung_exynos9820 (TWRP trees): S10e = SM-G970F → `beyond0lte` / `beyond0lteks`; S10 = `beyond1lte`; S10+ = `beyond2lte`; S10 5G = `beyondx`; Note10 = `d1`, Note10+ 5G = `d2x`. Source: https://github.com/ExtremeXT/android_device_samsung_exynos9820
- whatawurst/android_device_samsung_beyond0lte (LineageOS device tree for S10e). Source: https://github.com/whatawurst/android_device_samsung_beyond0lte
- BoardConfig: `TARGET_SOC := exynos9820`, `BOARD_PLATFORM := universal9820`, `TARGET_SLSI_VARIANT := bsp`, GPU `mali-g76`, `BOARD_KERNEL_SEPARATED_DTBO := true`. Source: https://github.com/whatawurst/android_device_samsung_exynos9820-common/blob/lineage-18.1/BoardConfigPlatform.mk
- NOTE (inference): the "d17 / d17ete / d17zet / d17zed" names in the task are Samsung's internal *platform/project* codenames (used in OSS release names & bootloader/SECURITY blobs), not the AOSP device-tree names. Needs verification — searching next.

### CONFIRMED: TWRP exists for S10e (beyond0lte) — so non-Android booting on S10e is at least recovery-level
- TeamWin/android_device_samsung_beyond0lte — "TWRP device tree for the Samsung Galaxy S10e (G970F)", default branch android-9.0, created 2019-04-30. Kernel source pointer: `https://github.com/corsicanu/android_kernel_samsung_universal9820`. Source: https://github.com/teamwin/android_device_samsung_beyond0lte
- e Foundation (Replicant-family) lists beyond0lte as a community release; notes "Bootloader not lockable". Source: https://doc.e.foundation/devices/beyond0lte

## Round 2 — postmarketOS port internals (read from a pmaports mirror: `sm7150-mainline/pmaports`)

Mirror used: https://github.com/sm7150-mainline/pmaports (older snapshot of pmaports; current tree lives at https://gitlab.postmarketos.org/postmarketOS/pmaports). The mirror still has the *per-device* S10 port; the current upstream has a *generic* `samsung-exynos9820` port (see wiki Category:Samsung, "testing").

### `device-samsung-beyond1lte` (Galaxy S10, testing) — deviceinfo (verbatim, from mirror)
- `deviceinfo_flash_method="heimdall-bootimg"`, `deviceinfo_flash_sparse="true"` (Samsung sparse-image flashing), `deviceinfo_generate_bootimg="true"`, `deviceinfo_bootimg_custom_args="--header_version 1 --os_version 12.0.0 --os_patch_level 2021-12"`.
- Boot-image load addresses: base `0x10000000`, kernel `0x00008000`, ramdisk `0x01000000`, second/dtb `0xf0000000`, tags `0x00000100`, pagesize `2048`.
- Heimdall partition names: `VBMETA`, `BOOT` (kernel), `SYSTEM` (rootfs). `deviceinfo_rootfs_image_sector_size="4096"`, `deviceinfo_no_framebuffer="true"`, `deviceinfo_bootimg_append_seandroidenforce="true"`, `deviceinfo_kernel_cmdline="buildvariant=userdebug"`.
- Depends on `mkbootimg`, `msm-fb-refresher`, `firmware-samsung-beyond1lte` (nonfree firmware subpackage).
  Source: https://github.com/sm7150-mainline/pmaports (path `device/testing/device-samsung-beyond1lte/deviceinfo`)

### `initfs-hook.sh` for beyond1lte — display node name (very useful for S10e display bring-up)
- `echo 16 > /sys/devices/platform/19030000.decon_f/graphics/fb0/bits_per_pixel` → the S10 display is driven by a `decon_f` device at platform path `19030000.decon_f`; framebuffer must be forced to 16 bpp.
  Source: https://github.com/sm7150-mainline/pmaports (path `device/testing/device-samsung-beyond1lte/initfs-hook.sh`)

### `linux-samsung-beyond1lte` kernel package (mirror) — how the S10 kernel is built for pmOS
- `pkgver=4.14.253`, "Samsung Galaxy S10 kernel fork", built as a **downstream/vendorkernel** from `Linux4/android_kernel_samsung_exynos9820` @ commit f7194c1d…, config based on `arch/arm64/configs/exynos9820-beyond1lte_defconfig`, `pmb:cross-native`, uses `downstreamkernel_prepare`/`downstreamkernel_package`.
- Patch needed: `selinux_include_generated_headers.patch` — arm64 needs `-I./arch/$(ARCH)/include/generated/uapi` added to `scripts/selinux/genheaders/Makefile` and `scripts/selinux/mdp/Makefile` when built out-of-tree (a real gotcha for any S10 kernel build).
  Source: https://github.com/sm7150-mainline/pmaports (path `device/testing/linux-samsung-beyond1lte/`)

### `firmware-samsung-beyond1lte` — proprietary WiFi (bcmdhd) blobs packaged for pmOS
- Pulls `bcmdhd_{mfg,mon,sta}.bin_b1`, `bcmdhd_clm.blob`, and `nvram.txt_*` from `Linux4/proprietary_vendor_samsung` (paths `exynos9820-common/proprietary/vendor/firmware/...` and `beyond1lte/...`) → S10 WLAN is a Broadcom bcmdhd wafer needing closed blobs + nvram files.
  Source: https://github.com/sm7150-mainline/pmaports (path `device/testing/firmware-samsung-beyond1lte/APKBUILD`)

## Round 3 — Mu-Silicium (Project Silicium) UEFI status for Exynos 9820

- Mu-Silicium README: "Project Silicium UEFI for ARM64-Based Devices … Making ARM-Based Devices Boot Operating Systems like Windows or Linux using Custom UEFI Firmware"; based on Project Mu + WOA-Project's `mu_andromeda_platforms` + Project-Aloha reference; credits "sonic011gamer and halal-beef for Exynos Related Stuff". 515★, BSD-2-Clause. Source: https://github.com/Project-Silicium/Mu-Silicium
- Status.md, "Exynos 9820 Devices" section: **Samsung Galaxy S10, codename `beyond1lte`, State: Inactive, requires `BIT-16`**; UEFI status: Display ✅, Internal Storage ❌, Side Buttons ✅, USB Host/Device Mode ❌, Windows Boot ❌, **Linux Boot ❌**. (S9 `starlte` is Active with Windows Boot ✅ but Linux Boot ❌; S20 5G `x1s` Active, WinPE only.)
  Source: https://github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md
- Inference: Mu-Silicium's Exynos-9820 UEFI currently gets you to a display in UEFI but has no OS boot; the "BIT-16" is the Samsung LK/bootloader-bit version requirement. Also note the S24 (`e1s`) and Note20 5G (`c1s`) entries show the same "Linux Boot ❌" pattern — nobody has booted Linux via Mu-Silicium on Exynos flagships yet.

## Round 4 — cross-reference inside THIS repo (facts from the codebase)
- `firmware/Platform/Star2LtePkg/Drivers/Star2LteUfsHcDxe/Star2LteUfsHcDxe.c:457` says: vendor HCI setup was cross-checked "against Linux `drivers/ufs/host/ufs-exynos.c` (reg_hci = vs_hci)" and "reference **Exynos9820Pkg ExynosUfsLib** (`ufs_pre_setup/ufs_vendor_setup`)" → an EDK2 package named `Exynos9820Pkg` (with an `ExynosUfsLib`) exists somewhere upstream and is the S10e-side reference for UFS bring-up. TODO: locate that repo (search "Exynos9820Pkg" on GitHub returned no hits → likely inside edk2-exynos/Mu-Silicium silicon tree, or a private/vendor drop).
- Inference: mainline Linux already has an Exynos UFS host driver (`drivers/ufs/host/ufs-exynos.c`) — that is the reason a *mainline* S10e kernel can talk to the UFS storage, and it is what pmOS's exynos9820 port leans on.

---
