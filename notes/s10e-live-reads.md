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
