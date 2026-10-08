# Exynos 9820 (Galaxy S10e) research notes

Goal: find Linux/driver references for Exynos 9820 to port the Exynos 9810 Windows drivers
(`drivers/Exynos9810Ufs`, `drivers/Exynos9810Hsi2c`, `drivers/S6SY761Touch`) to the S10e.

Findings are appended as discovered. Facts cite a URL; anything marked *(inference)* is mine.

## 1. V0lk3n/nethunter_kernel_samsung_exynos9820 (user-provided)

- Repo: https://github.com/V0lk3n/nethunter_kernel_samsung_exynos9820 — "Nethunter port of
  LineageOS for Samsung Galaxy S10 Exynos9820", forked from
  https://github.com/LineageOS/android_kernel_samsung_exynos9820 (fetched 2026-10-08).
- Device codename in LineageOS: `beyond1lte` (https://wiki.lineageos.org/devices/beyond1lte/) —
  covers Galaxy S10/S10e class, Exynos 9820.
- Branch `nethunter-lineage-23.2`; contains full kernel tree (drivers/, firmware/, arch/, mm/...)
  plus `kali-nethunter-kernel-builder` submodule and `nh_files/local.config`.
- Notable build configs at root: `build.config.universal9820`, `build.config.universal9820_5g`,
  `build.config.universal9825` → the same tree builds kernels for exynos9820 (S10/S10e) AND
  exynos9825 (S10 5G). *(inference)* useful because it is a complete, compilable 9820 kernel
  source we can grep for touch/HSI2C/storage drivers.
- 31 stars, 22 forks, 744,442 commits in history (Samsung kernel + LineageOS + NetHunter patches).

### 1a. What the 9820 kernel says about our three drivers (from the V0lk3n tree, branch `nethunter-lineage-23.2`)

**Touch (`drivers/input/touchscreen/sec_ts/`)**
- 9820 has NO `drivers/touchscreen/` dir; touch lives at `drivers/input/touchscreen/sec_ts/y771/`
  and `sec_ts/y771_d/` (files: `sec_ts.c`, `sec_ts.h`, `sec_ts_fn.c`, `sec_ts_fw.c`, `sec_ts_ioctl.c`,
  `sec_ts_only_vendor.c`) — same `sec_ts` core the S6SY761Touch driver was derived from.
- Defconfig (`arch/arm64/configs/exynos9820-beyond1lte_defconfig`): `CONFIG_TOUCHSCREEN_SEC_TS_Y771=y`,
  `CONFIG_INPUT_TOUCHSCREEN_TCLMV2=y`, `CONFIG_TOUCHSCREEN_PROPERTIES=y`, `CONFIG_TOUCHSCREEN_DUMP_MODE=y`.
  *(inference)* So on S10e the touch driver is the same Samsung `sec_ts` framework, just the `y771`
  project variant instead of the S9+ one.
- `sec_ts.h` sets `SEC_TS_DEFAULT_FW_NAME "tsp_sec/y771_beyond2.bin"`; DTS for beyond1lte uses
  `"tsp_sec/y771_beyond1_phole.bin"` — per-device touch firmware blobs, `sec,project_name = "BEYOND1","G973"`,
  `support_fod`, `sec,tclm_level = <2>`, `sec,afe_base = <0x0206>`.
- DTS `arch/arm64/boot/dts/samsung/exynos9820-beyond1lte_input_common.dtsi` attaches the touch device to
  `&hsi2c_10 { touchscreen@48 { ... } }` — **touch is still on HSI2C bus #10 on Exynos 9820**.

**HSI2C bus controller**
- HSI2C nodes in `arch/arm64/boot/dts/exynos/exynos9820.dts`: `hsi2c_0..hsi2c_46`, all
  `compatible = "samsung,exynos5-hsi2c"`; hsi2c_10 physical base = **0x104B0000** — identical to
  `EXYNOS_HSI2C10_PHYSICAL_BASE 0x00000000104B0000ULL` in our `drivers/Exynos9810Hsi2c/exynos9810hsi2c.h`.
- Bus driver source: `drivers/i2c/busses/i2c-exynos5.c` in BOTH kernels. Diffed 9810 (LineageOS/android_kernel_samsung_exynos9810)
  vs 9820 (V0lk3n): 95 vs 111 `#define HSI2C_*`; the 9820 file is a **superset** — the 16 extra defines are all
  `HSI2C_MASTER_ST_*` (master-status decode: IDLE/START/RESTART/STOP/MASTER_ID/ADDR0-2/ADDR_SR/READ/WRITE/
  NO_ACK/LOSE/WAIT/WAIT_CMD). Nothing exists only in the 9810 version. *(inference)* our HSI2C Windows driver
  needs at most cosmetic changes (rename + maybe richer error/status decode), same register offsets, same base.
- Note: "SpbCx" in our driver = Microsoft's SP Bus (Simple Bus) WDF framework (`SpbCxStubs.lib`), NOT a Samsung
  name — so nothing to port there.

**Other 9810↔9820 pairs found in the 9820 tree (useful as diff targets)**
- `drivers/bts/bts-exynos9810.c` + `bts-exynos9820.c` and `cal_bts9810.*` / `cal_bts9820.*`
  (BTS = Samsung "bandwidth/throughput sensor" power/MIF control, not touch).
- `drivers/video/fbdev/exynos/dpu20/cal_9810/` vs `cal_9820/` (display DPU calibration: decon/dpp/dsim regs)
  and `CONFIG_EXYNOS_DPU20=y` on 9820.
- `include/soc/samsung/bts.h` switches on `CONFIG_EXYNOS9820_BTS`; `drivers/soc/samsung/cal-if/exynos9810/`
  (cal-if has exynos8895/9810/9820 variants).
- GPU: `drivers/gpu/arm/exynos/` (Mali/"gpexbe") with `CONFIG_MALI_EXYNOS_BTS_MO=y`; DTS `exynos9820-mali_common.dtsi`,
  `exynos9820-sysmmu.dtsi`.
- Defconfigs available: `exynos9820-{beyond0lte,beyond1lte,beyond2lte,beyondx,d1,d1x,d2s,d2x,f62}_defconfig`
  plus `nethunter_*` variants → the tree also targets S20 (d1/d1x), S20 Ultra? (d2s/d2x), F62.

### 1b. Device-name mapping (matters a lot: our target is the S10e)

- LineageOS build roster: **Samsung Galaxy S10e = `beyond0lte`**, S10 = `beyond1lte`, S10+ = `beyond2lte`,
  S10 5G = `beyondx`, Note10 = `d1`, Note10+ = `d2s`, Note10+ 5G = `d2x`
  (https://www.lineageoslog.com/21.0/beyond1lte — "Promote Exynos 9820 Family to 20" list).
- So for the S10e use `exynos9820-beyond0lte_defconfig` and DTS `exynos9820-beyond0lte_*.dts(i)`;
  the V0lk3n repo has `nethunter_beyond0lte_defconfig` too.
- S10e GPU: ARM Mali-G76 MP12, 8 GB RAM (https://customrombay.org/samsung-beyond1lte lists the family;
  *(inference)* same GPU class as S10).

### 1c. Storage addresses — the big porting gotcha

- 9820 DTS `arch/arm64/boot/dts/exynos/exynos9820.dts`: `ufs: ufs@0x13D60000`,
  `compatible = "samsung,exynos-ufs"`, children `<0x0 0x13D50000 0x8000>` = **UNIPRO**,
  `<0x0 0x13D70000 0x100>` = **UFS protector**, `hw-rev = <UFS_VER_0006>`, `#include <dt-bindings/ufs/ufs.h>`,
  plus a `ufs-srpmb { compatible = "samsung,ufs-srpmb" }` node.
- Our `drivers/Exynos9810Ufs/Exynos9810Ufs.h` uses 9810 addresses: UFS HCI `0x11120000`, UNIPRO `0x11110000`,
  PMA `0x11124000`, UFSP `0x11130000`.
- **So the 9820 UFS HCI/UNIPRO/protector MMIO bases move to 0x13D60000 / 0x13D50000 / 0x13D70000.**
  *(inference)* the Windows UFS driver is mostly a constant-swap plus re-derivation of PMA/UFSP equivalents.
- HSI2C bus #10 base is unchanged (0x104B0000) between 9810 and 9820.

## 2. sonic011gamer/edk2-exynos — EDK2 UEFI for Samsung Exynos (has an Exynos9820Pkg!)

- Repo: https://github.com/sonic011gamer/edk2-exynos (11★, BSD-2-Clause + GPL drivers). "aims to provide a
  usable EDK2 UEFI environment for modern Samsung SoCs… boot manager for multi-booting mainline Linux,
  Android and optionally Windows on certain SoCs." Docs: https://wiki.renegade-project.cn/
- `Silicon/Samsung/` packages: **Exynos7420Pkg, Exynos7885Pkg, Exynos9820Pkg, Exynos990Pkg, ExynosPkg**.
- PR history: #6 "add suport for beyond1lte and exynos9820", #7 "Exynos9820: drop references to Exynos990",
  #5 "add S6/S6 edge" → 9820 support is recent and possibly rough.
- `GPLDrivers/Drivers/`: `BootSlotDxe`, `Op6tSlotDxe` (boot-slot switching, i.e. how it chains Android/TWRP).
- Layout mirrors our repo's: `Common/`, `Platform/`, `Silicon/`, `configs/`, `tools/`, `build.sh`.

## 3. Project-Silicium/Mu-Silicium — THE key reference (9810 and 9820 side by side)

- Repo: https://github.com/Project-Silicium/Mu-Silicium (510★, BSD-2-Clause + GPLv2 drivers). "Project Mu
  UEFI Ports for ARM64-Based Devices". Successor of sonic011gamer/Mu-Samsung
  (https://github.com/sonic011gamer/Mu-Samsung — README says "Development has been Moved to Mu-Silicium!").
- `Silicon/Samsung/` holds per-SoC packages: `S5E7870Pkg, S5E7885Pkg, S5E9810Pkg, **S5E9820Pkg**, S5E9830Pkg,
  S5E9945Pkg, SamsungPkg (common), SamsungNullPkg`. Samsung's own SoC codenames: S5E9810/S5E9820/S5E9830.
- `Platforms/Samsung/` holds per-device packages; for the 9820 family there is **`beyond1ltePkg`** (Galaxy S10)
  but **no `beyond0ltePkg`** (S10e) yet → our S10e port would add a sibling platform pkg on top of S5E9820Pkg.
  (Other Samsung targets: `starltePkg`, `starqltechnPkg`, `a10Pkg`, `r8sPkg`, `b5qPkg`, `dm2qPkg`, `gts9wifiPkg`, …)
- Status.md "Exynos 9820 Devices" section (https://raw.githubusercontent.com/Project-Silicium/Mu-Silicium/main/Status.md):
  device "Samsung Galaxy S10", codename `beyond1lte`, **State: Inactive**, contributor Robotix22, needs `BIT-16`
  version; feature table: Display ✅, Side Buttons ✅, **Internal Storage ❌**, USB Host/Device ❌,
  Windows Boot ❌, Linux Boot ❌ — with a note "Only 23H2 Windows Builds and Lower are able to Boot."
  *(inference)* so 9820 UEFI exists but storage was never finished there — exactly the gap our UFS driver port must fill.
- Compare `S5E9810Pkg/S5E9810Pkg.dsc.inc` vs `S5E9820Pkg/S5E9820Pkg.dsc.inc` (both fetched):
  - only in 9810: `PlatformUfsLib` (`S5E9810Pkg/Library/PlatformUfsLib/PlatformUfsLib.inf`)
  - only in 9820: `UsiLib` (`S5E9820Pkg/Library/UsiLib/UsiLib.inf`), `HwResetSystemLib`/`ResetSystemLib`
  - common: `PlatformSecLib`, `GpioBankLib`, `PmicLib`
- Release notes (https://github.com/Project-Silicium/Mu-Silicium/releases) mention directly relevant work:
  "Added UFS Support for `S5E9810` by @halal-beef", "Fixed Exynos HSI2C Driver", "Added Support for Exynos DWC3",
  "Refactored Exynos Files", "Added Custom SMC Library for Exynos Devices", "Added Memory Hole Names to Exynos Devices",
  "Added Shutdown & Reboot Implementation for `S5E9820` & `S5E9830`", "Updated `S5E9830` UFS Calibration Tables".
  *(inference)* `S5E9830` (Exynos 9830, Note20 class) is a later Exynos with **working UFS** in Mu-Silicium —
  a second diff point beyond 9820 for the storage driver.

## 4. Touch protocol on 9820 — same as 9810 (confirmed)

- `drivers/soc/samsung/usi_v2.c` in the 9820 kernel = "EXYNOS - USI v2 (Universal Serial Interface version 2)"
  i.e. a Samsung SoC IPC bus (i2c/spi/uart switchable), **not** the USI touch-digitizer protocol.
  So `UsiLib` in Mu-Silicium's 9820 pkg is unrelated to the touchscreen.
- `sec_ts/y771/sec_ts_fn.c` talks to the panel with `ts->sec_ts_i2c_read/write(ts, SEC_TS_CMD_*, …)`
  (commands like `SEC_TS_CMD_SENSITIVITY_MODE`, `SEC_TS_CMD_SET_TOUCHFUNCTION`, `SEC_TS_CMD_FACTORY_LEVEL`)
  — same command-over-HSI2C shape the S6SY761Touch Windows driver implements.
- S10e defconfig `exynos9820-beyond0lte_defconfig` exists in the V0lk3n tree (plus `nethunter_beyond0lte_defconfig`).

## 5. UFS/UNIPRO address mapping — 9810 → 9820 (the concrete porting delta)

Mu-Silicium `S5E9810Pkg/Library/PlatformUfsLib/BoardInit.c` (fetched raw) defines the same bases our
Windows UFS driver uses, and the 9820 kernel DTS gives the 9820 equivalents:

| Role | 9810 (Mu-Silicium / our `Exynos9810Ufs.h`) | 9820 (`exynos9820.dts` `ufs: ufs@0x13D60000`) |
|---|---|---|
| UFS HCI base | `EXYNOS9810_UFS_BASE 0x11120000` / `EXYNOS_UFS_HCI_PHYSICAL_BASE 0x11120000` | `0x13D60000` (`<0x0 0x13D60000 0x200> /* 0: HCI standard */`) |
| Vendor-specific area | `UFS_BASE + 0x1100` = `0x11121100` | `0x13D61100` (`/* 1: Vendor specificed */`) — **same +0x1100 delta** |
| UNIPRO base | `UFS_BASE - 0x10000` = `0x11110000` | `0x13D50000` size `0x8000` — **same −0x10000 delta** |
| UFS protector (UFSP) | `UFS_BASE + 0x10000` = `0x11130000` | `0x13D70000` size `0x100` — **same +0x10000 delta** |
| PHY PMA base | `UFS_BASE + 0x4000` = `0x11124000` | *(inference)* `0x13D64000` by the same rule — **needs verification on device** |
| 1µs→cnt divisor | `UFS_SCLK 166000000UL`, `CNT_VAL_1US_MASK 0x3FFU` | 9820 `oscclk clock-frequency = <26000000>` → must be re-derived |
| UFS HCI offsets | `UFSHCI_VS_1US_TO_CNT_VAL 0x110C`, `UFSHCI_VS_UFSHCI_V2P1_CTRL 0x118C` | same offsets expected *(inference)*; 9820 `hw-rev = <UFS_VER_0006>` from `#include <dt-bindings/ufs/ufs.h>` |
| CMU/PMU/SYSREG | `CMU_FSYS0_BASE 0x1A240000`, `PMU_BASE 0x14060000`, `SYSREG_FSYS0_BASE 0x11010000` | 9820 has `cmu_ewf { compatible = "samsung,exynos-cmuewf" }` and `exynos9820-pmucal-dbg.dtsi`; bases not yet extracted |

Other 9820 DTS facts: memory-map strings `"13d60000.ufs"`, `"13ed0000.pcie0"`, `"13d00000.dwmmc2"`;
`ufs-pm-qos-int = <400000>`; `ufs,pmd-attr-lane = 2`, `pmd-attr-gear = 3`; a separate
`ufs-srpmb { compatible = "samsung,ufs-srpmb" }` node (RPMB is what Windows would use for authenticated writes).

## 6. Booting Linux / other OSes on the S10e family — prior art

- **Ubuntu Touch / UBports community port** for the S10 family:
  https://gitlab.com/ubports/porting/community-ports/android10/samsung-galaxy-s10/kernel-samsung-exynos9820
  (729,717 commits, GPL-2.0+, branch `halium-10.0`) + guide
  https://forums.ubports.com/topic/7566/guide-how-to-install-ut-on-samsung-s10e-beyond0lte-s10-beyond1lte-s10-beyond2lte-exynos
  → they flash via a **LOS recovery + fastboot** flow, i.e. they keep the Android kernel (halium), not mainline.
- LineageOS supports S10e (`beyond0lte`) officially: https://wiki.lineageos.org/devices/beyond0lte —
  specs: Exynos 9820, 6/8 GB RAM, **Mali-G76 MP12**, 2280x1080 dynamic AMOLED, arm64.
- /e/OS and Whatawurst device trees for beyond0lte: https://github.com/whatawurst/android_device_samsung_beyond0lte
  (+ `android_kernel_samsung_exynos9820`, `android_vendor_samsung_beyond0lte`).
- Kali NetHunter kernel MR upstream: https://gitlab.com/kalilinux/nethunter/build-scripts/kali-nethunter-kernels/-/merge_requests/384
  ("Add support for Samsung Galaxy S10 Exynos9820 (beyond1lte)") — the V0lk3n repo is a mirror of this.
- Other 9820 kernel trees: https://github.com/CruelKernel/samsung-exynos9820 (unified S10+Note10 tree,
  models G970F/N = S10e), https://github.com/hiratazx/android_kernel_samsung_exynos9820 (KernelSU),
  https://github.com/grant-h/SM-G973F-Kernel (stock S10 kernel G973FXXU3ASG8),
  https://github.com/FabulaNox/nethunter-s10e-fix (S10e-specific: HID/USB-gadget fixes).
- Samsung's own kernel source for S10e is on Samsung Open Source Center (SM-G970F):
  https://www.xda-developers.com/exynos-samsung-galaxy-s10-s10e-s10plus-kernel-sources (opensource.samsung.com).
- No mainline-kernel Linux port for exynos9820 found (mobile-linux style). *(inference)* everything
  "Linux" on this SoC so far is Android-kernel-based (halium/NetHunter), so the HSI2C/sec_ts/UFS code we
  need is Samsung BSP code, not mainline drivers.

## 7. Porting checklist (what to change in this repo for S10e)

1. **HSI2C driver (`drivers/Exynos9810Hsi2c`)** — lowest risk. Bus #10 base is identical (0x104B0000),
   register set identical, 9820 kernel adds only `HSI2C_MASTER_ST_*` decode. Rename + retarget `.inx`
   hardware ID; keep the SPB framework.
2. **Touch driver (`drivers/S6SY761Touch`)** — same `sec_ts` command-over-HSI2C protocol, but the
   9820 variant is `sec_ts/y771` (`CONFIG_TOUCHSCREEN_SEC_TS_Y771=y`, FW `tsp_sec/y771_beyond1_phole.bin`
   for S10 / `y771_beyond2.bin` referenced in `sec_ts.h`), with `sec,tclm_level`, `sec,afe_base = <0x0206>`,
   `support_fod` (FOD = fingerprint-on-display) and TCLM v2 calibration. Diff `y771/sec_ts_fn.c`
   against our ported commands; the S10e DTS (`beyond0lte_eur_open_17.dts`) also uses `&hsi2c_10 { touchscreen@48 }`.
3. **UFS driver (`drivers/Exynos9810Ufs`)** — highest risk: swap the four MMIO bases (HCI 0x13D60000,
   VS +0x1100, UNIPRO −0x10000, UFSP +0x10000), re-derive `UFS_SCLK`/1µs counter from the 9820 oscclk
   (26 MHz per DTS), and re-check PMA (0x13D64000 by rule, unverified). Mu-Silicium has **no**
   `PlatformUfsLib` for S5E9820 → nobody has done this; use S5E9810Pkg + S5E9830Pkg as templates.
4. **Firmware side**: Mu-Silicium already builds `S5E9820Pkg` + `beyond1ltePkg`; an S10e target would be a
   new `beyond0ltePkg` on the same silicon package. Note their 9820 caveat: "Only 23H2 Windows Builds and
   Lower are able to Boot" and `BIT-16` version requirement.

## 8. 9820 `sec_ts` command opcodes (from `sec_ts/y771/sec_ts.h`) — direct input for the touch port

`SEC_TS_CMD_SENSE_ON 0x10`, `SENSE_OFF 0x11`, `SW_RESET 0x12`, `CALIBRATION_SEC 0x13`,
`FACTORY_PANELCALIBRATION 0x14`, `SET_TOUCHFUNCTION 0x30`, `SET_TSC_MODE 0x31`,
`TOUCH_MODE_FOR_THRESHOLD 0x35`, `TOUCH_THRESHOLD 0x36`, `SET_COVERTYPE 0x38`,
`WAKEUP_GESTURE_MODE 0x39`, `WET_MODE 0x3B`, `ERASE_FLASH 0x45`, `ENTER_FW_MODE 0x57`,
`CLEAR_EVENT_STACK 0x62`, `MUTU_RAW_TYPE 0x70`, `SELF_RAW_TYPE 0x71`;
plus `sec_ts_fw.c` uses `FLASH_ERASE`/`FLASH_WRITE`/`FLASH_PADDING`/`FLASH_READ_ADDR|SIZE|DATA`
with `sec_ts_i2c_write_burst()` — the same flash/FW-update-over-HSI2C path our S6SY761Touch port models.
No `s6sy761`/`SY761` string appears in the 9820 `sec_ts` variant: *(inference)* the panel IC is still a
Samsung "sec_ts" TSP but the S6SY761-specific naming may differ on S10e — verify against the panel ID
read back via `SEC_TS_CMD_GET_*` before renaming the Windows driver.

## Dead ends / not useful

- `drivers/touchscreen/` does not exist in the 9820 kernel (searched; it moved to `drivers/input/touchscreen/`).
- No `hsi2c`-named directory or file in the 9820 kernel (the bus driver is `drivers/i2c/busses/i2c-exynos5.c`).
- No `rwd`/`yandss`/`t3s` storage driver names in either kernel — "RWD1" in our repo is a project-local
  memory record (`drivers/include/Rwd1Constants.h`), not a Samsung kernel driver.
- `Project-Aloha/mu_aloha_platforms` "Samsung Galaxy S10 samsung-beyond1qlte" is the **Snapdragon 855**
  S10 variant, not Exynos 9820 — do not use as the 9820 reference.
- Nura wiki page (https://wiki.nura.eco/wiki/Samsung_Galaxy_S10_(samsung-beyond1lte)) blocked by Anubis
  anti-bot; not retried.

## 9. Correction + the parallel research that was already in the tree

- **Model number correction:** the Galaxy S10e is **SM-G970F** (`beyond0lte`); S10 = SM-G973F,
  S10+ = SM-G975F, S10 5G = SM-G977B (https://wiki.lineageos.org/devices/beyond0lte and
  https://github.com/ExtremeXT/android_device_samsung_exynos9820).
- Two earlier research passes were left in the tree before they were cancelled; they are kept as-is:
  [`notes/exynos9820-linux-boot.md`](exynos9820-linux-boot.md) and
  [`notes/exynos9820-drivers.md`](exynos9820-drivers.md).
- **Big new fact: a close-to-mainline Linux for this SoC does exist.** postmarketOS ships
  `linux-postmarketos-exynos9820` ("Close-to-mainline kernel for Samsung Exynos 9820 devices",
  7.2.0-r0, built 2026-07-31) from https://github.com/chiffathefox/exynos-9820-mainline-linux
  (source: https://pkgs.postmarketos.org/package/main/postmarketos/aarch64/linux-postmarketos-exynos9820).
  *(inference)* this is the best place to learn which of the Samsung BSP pieces (HSI2C bus, touch,
  display `decon_f`) survived into a mainline-style kernel — i.e. what a generic driver can rely on.
- S10e kernel is **Linux 4.14**-based (Android-patched), not 4.10
  (https://github.com/LineageOS/lineage_wiki/blob/main/_data/devices/beyond0lte.yml,
  https://lineage.linux4.de/changes/beyond0lte.html).
- postmarketOS device internals for the S10 family (from the pmaports mirror
  https://github.com/sm7150-mainline/pmaports, path `device/testing/device-samsung-beyond1lte/`):
  flash method `heimdall-bootimg`, partitions `VBMETA`/`BOOT`/`SYSTEM`, boot-image load base
  `0x10000000`, and the display is a `decon_f` node at `/sys/devices/platform/19030000.decon_f`
  forced to 16 bpp — the same DPU family the 9820 kernel calls `CONFIG_EXYNOS_DPU20`.
- Touch-driver lineage confirmed: Gustave Monce `edk2-porting/nt36xxx_win` → TheMorc
  `S6SY761Touch` (GPL-2.0) → this repo; `yatli/S6SY761Touch` retargets it to OnePlus 7 Pro/7T Pro
  (also s6sy761). TheMorc has no S10/S20 Windows driver of his own.
- Mu-Silicium template ranking for the storage work: `x1sPkg` (Galaxy S20 5G, Exynos 9830) is
  **Active with Internal Storage working**, so `S5E9830Pkg` + `S5E9810Pkg` are the two working
  examples to diff against the broken 9820 case.

## Sources

- https://github.com/V0lk3n/nethunter_kernel_samsung_exynos9820 (+ raw files under that branch)
- https://github.com/LineageOS/android_kernel_samsung_exynos9810
- https://github.com/sonic011gamer/edk2-exynos
- https://github.com/Project-Silicium/Mu-Silicium (+ raw Status.md, S5E9810Pkg/…/BoardInit.c, UfsCal.h, SamsungPkg.dsc.inc)
- https://github.com/sonic011gamer/Mu-Samsung
- https://github.com/Project-Aloha/mu_aloha_platforms , https://github.com/Project-Aloha/windows_samsung_platforms
- https://www.lineageoslog.com/21.0/beyond1lte , https://wiki.lineageos.org/devices/beyond0lte
- https://customrombay.org/samsung-beyond1lte
- https://gitlab.com/ubports/porting/community-ports/android10/samsung-galaxy-s10/kernel-samsung-exynos9820
- https://forums.ubports.com/topic/7566/guide-how-to-install-ut-on-samsung-s10e-beyond0lte-s10-beyond1lte-s10-beyond2lte-exynos
- https://gitlab.com/kalilinux/nethunter/build-scripts/kali-nethunter-kernels/-/merge_requests/384
- https://github.com/CruelKernel/samsung-exynos9820 , https://github.com/hiratazx/android_kernel_samsung_exynos9820
- https://github.com/grant-h/SM-G973F-Kernel , https://github.com/FabulaNox/nethunter-s10e-fix
- https://github.com/whatawurst/android_device_samsung_beyond0lte
- https://www.xda-developers.com/exynos-samsung-galaxy-s10-s10e-s10plus-kernel-sources
- https://github.com/TheMorc (no exynos repos; member of edk2-porting / Project-Aloha)
