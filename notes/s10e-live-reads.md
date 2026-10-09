# Live reads from the real S10e (M6)

Everything here was read off the phone over `adb`, **read-only** (`cat`/`ls`/`grep`/`od` only).
Nothing was flashed, written, rebooted or re-partitioned. Findings appended as discovered.

Phone as seen by the PC: `model:SM_G970F`, `product:beyond0ltexx`, `device:beyond0`
(`adb devices -l`, 2026-10-08) — confirms the target device is the **Galaxy S10e**
(`beyond0lte`), not the S10.

## 1. Kernel identity

```
uname -a      : Linux localhost 4.14.113-25257816 #1 SMP PREEMPT Thu Mar 2 09:32:04 KST 2023 aarch64
/proc/version : Linux version 4.14.113-25257816 (dpi@21DKG923) (Android ... clang 6.0.1 ...) #1 SMP Thu Mar 2 2023
```

- The shipping S10e kernel is **Linux 4.14.113**, vendor-built **2023-03-02**, `aarch64`.
  Matches the `beyond0lte` kernel family in the research notes (4.14-based, not 4.10).
- `dpi@21DKG923` is Samsung's internal build tag; `21DKG923` is the One Build identifier
  *(inference)* — useful only as a fingerprint of which vendor firmware we are sitting on.

## 2. Live hardware map (`/sys/devices/platform/`) — the decisive confirmation

`/proc/iomem` is blocked (`Permission denied`, needs root), but `/sys/devices/platform/`
gives the same information and answers the two open questions from the research notes:

```
104b0000.hsi2c      <- HSI2C bus #10, the one the touchscreen sits on
104f0000.hsi2c  10860000  10870000  10880000  10890000  108e0000  108f0000
10920000  10990000  109a0000  15d30000  15d50000  15d60000  15d70000
13d60000.ufs        <- UFS storage controller
19030000.decon_f    19050000.decon_t     (display DPU)
15861e00.pd-dpu     ufs-srpmb            (RPMB = authenticated-write region)
```

- **UFS HCI base on the S10e = `0x13D60000`** — confirmed on the device, matching
  `exynos9820.dts` and NOT the 9810 value (`0x11120000`). The address swap is real, not inference.
- **HSI2C bus #10 base on the S10e = `0x104B0000`** — identical to the 9810 driver's
  `EXYNOS_HSI2C10_PHYSICAL_BASE`. The HSI2C driver really is a drop-in.
- `ufs-srpmb` exists as its own platform node → the RPMB path the storage driver needs is present.
- Display is `decon_f` at `0x19030000` (the same node postmarketOS pokes).

## 3. Driver identities on the live S10e (from `driver` symlink targets)

Symlink *targets* are readable even where file *contents* are SELinux-blocked, which is
what makes this section possible:

| Hardware | Live driver class | Device node |
|---|---|---|
| HSI2C bus | **`exynos5-hsi2c`** | `104b0000.hsi2c` |
| Touchscreen | **`sec_ts`** (`/sys/bus/i2c/drivers/sec_ts`) | `104b0000.hsi2c/i2c-4/4-0048` |
| Storage (UFS) | **`exynos-ufs`** | `13d60000.ufs` |
| Storage RPMB | **`exynos-ufs-srpmb`** | `ufs-srpmb` |
| Display | **`exynos-decon`** | `19030000.decon_f` |
| Touch-sensor mux | `exynos-tsmux` | `188e0000.tsmux` |
| VTS / VTS-DMA | `samsung-vts`, `samsung-vts-dma` | `15510000.vts` |

- All three drivers we are porting exist on the S10e **under the same names** as on the S9+
  (`sec_ts`, `exynos5-hsi2c`, `exynos-ufs`) — *(inference)* so the Windows ports stay
  recognisable and only the hardware addresses and per-model tuning values change.
- The touch device is I2C unit **`4-0048`** (bus id 4, address `0x48` = 72) — matches the
  DTS `touchscreen@48`.

## 4. Touch panel values read from the live device tree

Readable because these nodes are not blocked:

```
sec,firmware_name  = tsp_sec/y761_beyond0_phole.bin
sec,project_name   = BEYOND0G970
compatible         = sec,sec_ts
reg                = 00 00 00 48            (unit 0x48)
sec,tclm_level     = 00 00 00 02            (TCLM level 2)
sec,area-size      = 00 00 00 85  00 00 01 0a  00 00 01 55   (0x85, 0x10A, 0x155)
sec,max_coords     = 00 00 10 00  00 00 10 00                (4096 x 4096)
sec,afe_base       = 00 00 02 05            (0x0205)
```

- **The S10e's panel firmware blob is `tsp_sec/y761_beyond0_phole.bin`** — note `y761`,
  while the kernel source directory is `sec_ts/y771`. So "y771" is the driver *family*
  and `y761_beyond0_phole` is this model's *blob*. The S10 DTS used `y771_beyond1_phole.bin`.
- `sec,project_name = BEYOND0G970` confirms the model pairing (beyond0 / G970).
- `sec,afe_base` is **0x0205 on the S10e** vs **0x0206 in the S10 DTS** — a real per-model
  difference, not a constant.
- `sec,max_coords = 4096x4096` is the panel's coordinate space; `sec,area-size` gives the
  physical size triple used for the touch-area thresholds.
- The node also exposes `input2`, `input3`, `input4` and `misc/tspio` (names readable,
  contents blocked) — Samsung's TSP I/O surface the `sec_ts` driver uses.

## 5. Still unverified after the live read

| Item | Status | Why |
|---|---|---|
| UFS HCI base `0x13D60000` | **verified on device** | §2 |
| HSI2C bus #10 base `0x104B0000` | **verified on device** | §2 |
| UNIPRO base `0x13D50000` | kernel DTS only | the DTB's `reg`/`ranges` contents are SELinux-blocked |
| UFS protector base `0x13D70000` | kernel DTS only | same |
| PHY/PMA base (`0x13D64000` by rule) | **still unverified** | not exposed as its own platform device; `ufs-phy/reg` unreadable |
| `UFS_SCLK` (timeout clock; 9810 uses 166 MHz) | **still unverified** | needs the UFS node's clock properties |

Two ways to close the remaining two rows, in ascending cost:
1. Read Mu-Silicium's **working** Exynos 9830 UFS code (`S5E9830Pkg`) — public repo, no phone needed.
2. Boot a rootable Linux on the phone (postmarketOS has a close-to-mainline 9820 kernel) and
   dump the DTB with `dtc`. Heavier; only if (1) leaves the value ambiguous.

## 6. SELinux lesson (so this is not repeated)

- `/proc/device-tree/**` file *contents* are labelled `u:object_r:sysfs:s0` and the
  unprivileged shell domain cannot read them, even though Unix permission bits look open
  (`-r--r--r--`). Directory *names* and symlink *targets* are fine.
- Practical consequence: get identities and addresses from node **names** and **symlinks**;
  get values from the kernel source DTS; only the vendor's own userspace can read the rest.


## 7. Second pass (2026-10-09, admin session) - what closed, what did not

All phone work stayed **read-only** (`ls`, `cat`, `grep`); nothing was written to the device even
though the operator allowed it, because HANDOFF §2 rule 5 forbids anything that could take the
donated device away from Android.

### 7.1 Still blocked (fact)

* `/proc/iomem` -> `Permission denied` (unprivileged shell domain).
* `/proc/device-tree/reserved-memory/<child>/reg` -> `Permission denied` (same lesson as §6).
* Live `reserved-memory` children (names only): `abox-rmem camera camera_contig cp_ram_logging
  debug_snapshot el2_code el2_earlymem ion_crypto ion_video_stream kaslr mm_debug modem_if
  modem_spare sec_debug_magic sec_rdx_bootdev seclog_mem secure_camera`.
* Live `/sys/devices/platform/` exposes **one** CMU-ish device: `1a240000.cmu_ewf`. The five CMU
  banks are *not* separate platform devices, so the bank -> power-domain mapping is not readable
  on the phone this way.

### 7.2 Vendor DT values (fact, from the shipping 9820 DTS)

`CruelKernel/samsung-exynos9820` @ `cruel-HVJ5-v5.3`, `arch/arm64/boot/dts/exynos/exynos9820.dts`
and `exynos9820-rmem_s5100_dual.dtsi`:

* DRAM: `memory@80000000 reg = <0x0 0x80000000 0x80000000>` (bank0 = 2 GiB),
  `memory@880000000 reg = <0x8 0x80000000 0x80000000>`, `memory@900000000 reg = <0x9 0x0 0x80000000>`.
* `cmu reg = <0x0 0x1a200000 0x8000  0x0 0x1bc00000 0x8000  0x0 0x1bd00000 0x8000
  0x0 0x1be00000 0x8000  0x0 0x1bf00000 0x8000>`; `cmu_ewf reg = <0x0 0x1a240000 0x1000>`.
* Touch bus node: `hsi2c@104b0000` (`compatible = "samsung,exynos5-hsi2c"`, `default-clk = <0xbebc200>`
  = 200 000 000, `interrupts = <0x0 0x184 0x0>`, pinctrl phandle `0x76` = `hsi2c10-bus` on `gpp1-4/gpp1-5`).
  Same pin pair as the 9810 -> the DSDT's GSIV for the touch bus stays at the same pins.
* USB: `usb@10C00000` (`compatible = "samsung,exynos-dwusb"`, `reg = <0x0 0x10c00000 0x10000>`,
  `samsung,power-domain = <0x34>`, child `dwc3` with `phy-names = "usb2-phy","usb3-phy"`).
* Reserved carveouts inside bank0 (real values): `sec_debug_magic 0x80000000+0x1000`,
  `mm_debug 0x91200000+0x1000`, `seclog_mem 0xc0000000+0x80000`,
  `abox-rmem` heap `alloc-ranges = <0x0 0xc0000000 0x20000000>` size `0x3c00000`,
  debug-snapshot logs `0xfd900000..0xff400000+c00000`, `log_pstore 0xeb308000+0x8000`,
  `modem_spare 0xd0000000+0x1600000`, `modem_if2 0xd1600000+0x4200000`,
  `cp2cp_shm 0xd5800000+0x4000000`, `s5100_msi 0xd9800000+0x1000`,
  `modem_if 0xf0000000+0xb900000`, `cp_ram_logging 0xfb900000+0x2000000`.

### 7.3 Consequences for the open constants (fact + inference)

* **Framebuffer base.** `0xCC000000` sits inside the `abox-rmem` **allocation range**
  (`0xc0000000..0xe0000000`) in the shipping DT, and inside `HLOS 1` (`0xc1200000+0x3ee00000`)
  in edk2-exynos' working 9820 memory map. Inference (mine): the 9810's scanout address is not
  reusable on the S10e; it must be measured, or discovered at runtime from DECON
  (`decon_f@0x19030000`, `decon_t@0x19050000` - verified node names).
* **PRAM breadcrumb.** `0xFED14000` falls inside `debug_snapshot.log_bcm` (`0xfeb10000+0x400000`)
  on this SoC, i.e. inside a live vendor log region - the 9810's "scratch above RAM" assumption is
  false here (already stated in HANDOFF §5). Candidate carveouts that are reserved and not part of
  the live logging: `seclog_mem 0xc0000000+0x80000` (but that is the start of the abox heap range)
  or the top of bank0's `log_kevents` tail. Still a choice, not a read - keep it `UNVERIFIED`.
* **CMU bank -> domain.** NOT resolvable from the phone, the shipping DTS, or the
  `cmucal-{vclk,node,qch}.h` headers (those are enum lists only - confirmed by reading them).
  Dead end recorded so it is not re-searched.
  Practical way forward (inference, mine): do not pick a bank at all. edk2-exynos' working 9820 port
  maps the whole `0x0..0x20000000` as one `NS_DEVICE` region ("Periphs"), which covers every CMU bank
  and both verified device windows (`hsi2c@104b0000`, `usb@10C00000`). Our memory map can do the
  same, and the only CMU-specific pokes left are the 9810's `UsbDebug.c` QCH offsets, which stay
  `UNVERIFIED` until a read exists.

### 7.5 RESOLVED the same day - the CMU question was the wrong question (fact)

The DT's five-window `cmu` node is **not** the whole CMU set. Samsung's own calibration table
`drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c` opens with `cmucal_sfr_block_list[]`,
34 `SFR_BLOCK(name, address, size)` entries, and the DT's five are just BUSC + MIF..MIF3:

```
SFR_BLOCK(CMU_PERIC0, 0x10400000, 0x8000)   SFR_BLOCK(CMU_FSYS0A, 0x10a00000, 0x8000)
SFR_BLOCK(CMU_FSYS0,  0x13000000, 0x8000)   SFR_BLOCK(CMU_PERIC1, 0x10800000, 0x8000)
SFR_BLOCK(CMU_PERIS,  0x10020000, 0x8000)   SFR_BLOCK(CMU_BUSC,   0x1a200000, 0x8000)
SFR_BLOCK(CMU_CMU,    0x1a240000, 0x8000)   SFR_BLOCK(CMU_MIF..MIF3, 0x1bc00000..0x1bf00000)
unsigned int dbg_offset = 0x4000;
```

Consequences, all name-matched from the same file's `cmucal_sfr_list[]`:

* `CMU_PERIC0` is at **0x10400000** - the 9810's base was already right; only its length was short
  (the 9810 mapped 0x4000, which is cal-if's `dbg_offset`, not the block size 0x8000).
* `CMU_FSYS0A` (USB31DRD, M10) is **0x10a00000**; the 9810's `CMU_FSYS0 0x11000000` really does not
  exist here, and `CMU_FSYS0` on 9820 is 0x13000000.
* The touch bus gates, by name:
  `CLK_CON_GAT_GOUT_BLK_PERIC0_UID_USI03_I2C_IPCLKPORT_IPCLK` 0x2094,
  `..._USI03_I2C_IPCLKPORT_PCLK` 0x2098,
  `CLK_CON_GAT_CLK_BLK_PERIC0_UID_RSTNSYNC_CLK_PERIC0_USI_I2C_IPCLKPORT_CLK` 0x2054,
  `CLK_CON_GAT_CLK_BLK_PERIC0_UID_RSTNSYNC_CLK_PERIC0_OSCCLK_IPCLKPORT_CLK` 0x2008,
  `QCH_CON_USI03_I2C_QCH` 0x3038 (identical to the 9810's value).
  Bit fields confirm the driver's masks: `SFR_ACCESS(..._MANUAL, 20, 1)`, `SFR_ACCESS(..._CG_VAL, 21, 1)`.
* The 9820's USI sysreg block is `usi@10421000` with `usi3` at 0x10421010 (DT alias list
  `usi3 = "/usi@10421010"`), so the 9810's `SYSREG_PERIC0 0x10411000` moves to **0x10421000** -
  but the 9810's `+0x1C` register slot has no counterpart, so that pair stays `UNVERIFIED`
  (DSDT and driver header must change together).

Inference (mine, marked): the four gate roles (SOURCE / RESET_SYNC / IPCLK / PCLK) are assigned to
the cal-if entries by name semantics; the IPCLK/PCLK/QCH three are unambiguous, the SOURCE one
(OSCCLK gate 0x2008) is the chain's root and is the least certain of the four.

### 7.6 Sources for §7.5

* `https://raw.githubusercontent.com/CruelKernel/samsung-exynos9820/cruel-HVJ5-v5.3/drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c`
* `https://raw.githubusercontent.com/CruelKernel/samsung-exynos9820/cruel-HVJ5-v5.3/arch/arm64/boot/dts/exynos/exynos9820.dts`

### 7.4 Sources for §7

* `https://raw.githubusercontent.com/CruelKernel/samsung-exynos9820/cruel-HVJ5-v5.3/arch/arm64/boot/dts/exynos/exynos9820.dts`
* `https://raw.githubusercontent.com/CruelKernel/samsung-exynos9820/cruel-HVJ5-v5.3/arch/arm64/boot/dts/exynos/exynos9820-rmem_s5100_dual.dtsi`
* `https://raw.githubusercontent.com/CruelKernel/samsung-exynos9820/cruel-HVJ5-v5.3/drivers/soc/samsung/cal-if/exynos9820/cmucal-{vclk,node,qch}.h` (+ the `.c` twins, `cal-if/cmucal.h`, `cmucal.c`)
* `https://raw.githubusercontent.com/sonic011gamer/edk2-exynos/HEAD/Silicon/Samsung/Exynos9820Pkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c`
* live `adb` reads on `RF8M21HYDSZ` (SM-G970F)

## 8. CORRECTED — adb IS available, it was a search failure not an absence

The §8 written earlier this session claimed there was no `adb`. That was wrong:
`adb` is at **`C:\Users\ilya\code\platform-tools\adb.exe`** and the device is
attached and authorized (`RF8M21HYDSZ   device`). The earlier check only looked
on `PATH`, in `System32`, and under `tools/` — never at `code\platform-tools`.
The location is now recorded in `AGENTS.md` §8 so it is never re-searched.

Consequence: the four phone-only §5 hazards (fb base, PRAM/breadcrumb choice,
write-fence / protected-partition LBAs, UFS hw-rev class) are **not blocked** —
they can be advanced with read-only `adb` from here.
