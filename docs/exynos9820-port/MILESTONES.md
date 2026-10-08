# Exynos 9820 (Galaxy S10e) port — milestones and tasks

Goal: make the Windows-on-Exynos work proven on **SM-G965F (Exynos 9810, star2lte)**
also run on the **Galaxy S10e (Exynos 9820, `beyond0lte`, SM-G771F)**.

This file is the plan of record. It is updated and committed **per task**, never
batched at the end. Research evidence lives in [`notes/exynos9820-research.md`](../notes/exynos9820-research.md).

## Ground rules

1. **Milestones and tasks are written down before work starts** (this file), and the
   line is moved from `todo` to `doing` to `done` in the same commit as the work.
2. **One commit per task.** Message says what the task was, in plain English.
3. **Nothing is called `Ready` until it has been validated end to end on a real S10e.**
   This is the repo's existing golden rule ([CONTRIBUTING](../../CONTRIBUTING.md)) and
   it is not relaxed for a new device: unvalidated stages stay `Guided` or
   `NotImplemented` in the installer's stage catalog.
4. **A pause is required whenever the next step needs something I do not have.**
   Two things can be blocked on the user:
   - **`needs-phone`** — the S10e plugged into this computer (adb, read-only commands).
   - **`needs-windows`** — a Windows machine, to compile the drivers and run the installer.
5. **Read-only on the phone until a milestone is explicitly marked destructive.**
   No flashing, no partition writes, no installer runs without a separate green light.
6. **Uncertain values are isolated in one header and named `UNVERIFIED`**, so a
   wrong guess is visible instead of buried in code.

## Milestones

| # | Milestone | State | Blocked on |
|---|-----------|-------|------------|
| M1 | Gather the 9820 reference material | **done** | — |
| M1b | Read the mainline-style 9820 kernel (postmarketOS) | todo | — |
| M2 | Write the plan of record (this file) | **done** | — |
| M3 | Port the HSI2C bus driver to 9820 | todo | — |
| M4 | Port the touchscreen driver to 9820 | todo | M3 |
| M5 | Port the UFS storage driver to 9820 | todo | M3 |
| M6 | Verify the 9820 constants against a live S10e | todo | **needs-phone** |
| M7 | Build and test-sign the three 9820 drivers | todo | **needs-windows** |
| M8 | UEFI firmware for `beyond0lte` | todo | M5, M6 |
| M9 | Installer: add the S10e behind honest gating | todo | M7, M8 |

## Task list

### M1 — Reference material

| Task | State | Result |
|------|-------|--------|
| Read the 9820 Android kernel: touch, HSI2C, storage layout | done | `notes/exynos9820-research.md` §1–§5 |
| Find existing UEFI work for this chip (Mu-Silicium, edk2-exynos) | done | §3, §2 |
| Find existing Linux/other-OS work for the S10e family | done | §6 |
| Diff the 9810 and 9820 versions of the shared code | done | §1a, §5 |
| Merge the two parallel research passes left in the tree | done | `notes/exynos9820-linux-boot.md`, `notes/exynos9820-drivers.md` |
| Correct the S10e model number to SM-G970F | done | notes §9 |

### M1b — Mainline-style reference (new, from the merged research)

| Task | State | Notes |
|------|-------|-------|
| Read what postmarketOS keeps of the BSP on this SoC (`chiffathefox/exynos-9820-mainline-linux`) | todo | a close-to-mainline 9820 kernel exists; tells us what a generic driver may rely on |
| Use `S5E9830Pkg` (Exynos 9830, storage works) as the second storage template | todo | alongside `S5E9810Pkg` |

### M2 — Plan of record

| Task | State | Result |
|------|-------|--------|
| Write milestones, tasks and ground rules | done | this file |

### M3 — HSI2C bus driver

| Task | State | Notes |
|------|-------|-------|
| Diff our driver's register set against the 9820 kernel's | todo | expected: identical offsets, 16 extra status constants |
| Rename the package and retarget its hardware identification | todo | keep the 9810 driver untouched |
| Mark the two device-specific constants as verified-or-not | todo | bus base is the same on both chips |

### M4 — Touchscreen driver

| Task | State | Notes |
|------|-------|-------|
| Diff our command set against `sec_ts/y771` | todo | command list is in the notes §8 |
| Handle the per-model panel firmware name and tuning values | todo | `tsp_sec/y771_*` |
| Decide what fingerprint-on-glass changes mean for us | todo | `support_fod` in the 9820 device tree |

### M5 — UFS storage driver

| Task | State | Notes |
|------|-------|-------|
| Move the four hardware bases to the 9820 values | todo | HCI `0x13D60000` **verified on device**; UNIPRO/protector from kernel DTS only |
| Re-derive the timeout clock from the 9820 tick rate | todo | **UNVERIFIED** — 9810 uses 166 MHz; the phone would not tell us |
| Confirm the helper region address on the 9820 | todo | **UNVERIFIED** — `0x13D64000` by rule only; not exposed on the live device |
| Keep the diagnostic path able to print what the hardware says | todo | must be readable before any destructive step |

### M6 — Live verification on the phone

State: **doing** (user chose the phone path)

| Task | State | Notes |
|------|-------|-------|
| Install the PC-side tooling | done | `android-sdk` (not `android-tools`); `adb` 1.0.41 / 34.0.4-debian from `/usr/lib/android-sdk/platform-tools` |
| Diagnose why `adb` could not reach the phone | done | **not** a permissions problem: the device node is group-`plugdev` + ACL `user:ilya:rw-`, and `ilya` is in `plugdev`. `adb devices -l` reports `RF8M21HYDSZ unauthorized` — the phone never accepted the "allow this PC" prompt, so `~/.android/authorized_keys` does not exist |
| Read the running device tree from the S10e | done | `notes/s10e-live-reads.md` §2–§4 |
| Confirm the storage and bus regions in the live kernel | done | UFS `13d60000.ufs` and bus `104b0000.hsi2c` both present — the address swap is confirmed on hardware |
| Record the panel's identity and firmware version | done | driver `sec_ts`, blob `tsp_sec/y761_beyond0_phole.bin`, project `BEYOND0G970`, `afe_base 0x0205` |
| Note what the phone could NOT tell us | done | DTB *contents* are SELinux-blocked; PMA base and `UFS_SCLK` stay unverified (§5) |

The exact read-only commands I will run on the phone (nothing is flashed or written):

```
adb shell "uname -a"
adb shell "cat /proc/device-tree/ufs/0x13d60000/name"        # storage regions
adb shell "ls -l /sys/devices/platform/"                     # bus + display nodes
adb shell "cat /proc/iomem"                                  # where the hardware lives
adb shell "ls -l /sys/class/touchscreen /sys/devices/*hsi2c*" # touch + bus identity
adb shell "dmesg | grep -iE 'hsi2c|sec_ts|ufs|unipro'"       # what the kernel says
```

Paths are adjusted to whatever the phone actually exposes; all of it is `cat`/`ls`/`grep`.

**M6 step 1 — phone authorization (paused, needs the user):** `adb kill-server`, then
re-plug and accept the "Allow access from this PC" dialog on the phone. Success condition:
`adb devices -l` stops saying `unauthorized`.

### M7 — Build and sign

| Task | State | Notes |
|------|-------|-------|
| Build the three drivers for ARM64 | todo | **needs-windows** (WDK toolchain, no Linux cross-build) |
| Test-sign and package as `drivers.zip` equivalent | todo | **needs-windows** |

### M8 — Firmware

| Task | State | Notes |
|------|-------|-------|
| Decide whether to build on Mu-Silicium's `S5E9820Pkg` | todo | their storage path is known-broken |
| Note the Windows-build constraint for this chip | todo | only 23H2 and lower boot there |

### M9 — Installer

| Task | State | Notes |
|------|-------|-------|
| Add the S10e to the stage catalog as `Guided`, not `Ready` | todo | golden rule |
| Back up EFS/modem/partition table before any destructive step | todo | same as the 9810 flow |

## What is blocked on the user right now

| Needs | Milestones | Why |
|-------|-----------|-----|
| **needs-windows** | M7 | The Windows driver toolchain only runs on Windows; nothing here can compile these drivers |
| **needs-phone** | M6 | The two `UNVERIFIED` storage values can only be confirmed by reading a live S10e |
