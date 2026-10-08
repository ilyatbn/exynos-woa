# Exynos 9820 (Galaxy S10e) port — milestones and tasks

Goal: make the Windows-on-Exynos work proven on **SM-G965F (Exynos 9810, star2lte)**
also run on the **Galaxy S10e (Exynos 9820, `beyond0lte`, SM-G970F)**.

### The actual end goal (why this port exists)

These phones are **scrap hardware being turned into donated desktop computers**. A recipient
gets a phone docked into a **USB-C dock**, and from that dock they need:

| Need | Hardware path | Milestone |
|---|---|---|
| USB keyboard and mouse | the phone's USB-C port must act as a **host** port | M10 |
| LAN (network) | dock's USB ethernet, same host path | M13 |
| WiFi | the phone's own WLAN chip | M12 |
| HDMI output to a screen | the dock's HDMI, fed over the USB-C link | M11 |
| Stays on, charged, cool | power/battery/thermal while docked | M14 |
| Something a non-technical person can use | the assembled "donation build" | M15 |

That means the driver port (M3–M5) is **not the finish line** — it is the floor of a
larger plan. A phone that boots Windows but has no USB host mode cannot take a keyboard,
so M10 gates the whole desktop use case.

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
| M6 | Verify the 9820 constants against a live S10e | **done** | was **needs-phone** — resolved, see `notes/s10e-live-reads.md` |
| M7 | Build and test-sign the three 9820 drivers | todo | **needs-windows** |
| M8 | UEFI firmware for `beyond0lte` | todo | M5, M6 |
| M9 | Installer: add the S10e behind honest gating | todo | M7, M8 |
| M10 | USB-C **host** mode: keyboard, mouse, dock attachments | todo | M8 |
| M11 | External display: the dock's HDMI over the USB-C link | todo | M10 |
| M12 | WiFi on Windows for this chip | todo | M8 |
| M13 | LAN through the dock | todo | M10 |
| M14 | Stay powered and cool while docked | todo | M8 |
| M15 | The assembled donation build (repeatable, serviceable) | todo | M9–M14 |

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

State: **doing** — the address work is settled, one clock value is not.

| Task | State | Notes |
|------|-------|-------|
| Move the four hardware bases to the 9820 values | done (planning) | HCI `0x13D60000`, VS `0x13D61100`, UNIPRO `0x13D50000`, protector `0x13D70000` — all read from the vendor DTS; HCI confirmed on the live phone |
| Confirm the helper region address on the 9820 | **done** | `ufs-phy { reg = <0x0 0x13D64000 0x800> }` — verified, no longer a guess |
| Re-derive the timeout clock from the 9820 tick rate | todo | **UNVERIFIED**: the kernel computes `mclk_rate/1000000 & 0x3FF` from the UNIPRO clock; 166 MHz is the candidate (Mu-Silicium uses it for 9810 AND 9830) |
| Port the newer-revision behaviour | todo | S10e is `hw-rev = UFS_VER_0006`; the 9810 driver only handles 0004/0005 |
| Use the second template | todo | `edk2-exynos` already has `Exynos9820Pkg/Library/ExynosUfsLib` (`ufs.c`, `scsi.c`) — a UEFI 9820 UFS port exists outside Mu-Silicium |
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

### M10 — USB-C host mode (the gate for keyboard, mouse, LAN)

| Task | State | Notes |
|------|-------|-------|
| Find what the 9810 project does for USB host today | todo | our `firmware/` and Mu-Silicium's `UsbfnDwc3Dxe` |
| Locate Exynos DWC3/USBFN support in Mu-Silicium | todo | release note "Added Support for Exynos DWC3" |
| Read the S10e's own USB-function and DWC3 nodes on the live phone | todo | **needs-phone** — node names are readable |
| Establish what Windows needs for a phone's USB-C port to be a host | todo | the port is host-capable in UEFI; Windows must drive the attached devices |
| Check prior art: phones running Windows with a keyboard attached | todo | WOA / Renegade / Project-Aloha |

### M11 — External display through the dock

| Task | State | Notes |
|------|-------|-------|
| Determine how a USB-C dock's HDMI is fed at all | todo | EEM / SLDCOM protocol, not a real HDMI connection |
| Establish whether Windows supports EEM/SLDCOM docks | todo | **the make-or-break question for this goal** |
| Read the Linux reference implementation (EMD2) to see what the protocol requires | todo | https://github.com/emd2v2/emd2 |
| Check whether the target dock uses EEM or a real GPU in the dock | todo | **needs-dock** — depends on which dock we standardise on |
| Decide the fallback display path if EEM is a dead end on Windows | todo | phone panel only, or a different dock/output strategy |

### M12 — WiFi

| Task | State | Notes |
|------|-------|-------|
| Identify the WLAN hardware in the S10e (live) | todo | **needs-phone** — DTB node names |
| Find existing Windows drivers for Samsung Exynos WLAN | todo | `windows_samsung_platforms` components, Mu-Silicium |
| Decide whether WiFi is required at all when LAN is available | todo | donation case may prefer LAN only |

### M13 — LAN through the dock

| Task | State | Notes |
|------|-------|-------|
| Confirm Windows ARM64 has a driver for a generic USB ethernet adapter | todo | standard LAN drivers exist for ARM64 |
| Confirm the dock's ethernet does not need the phone's own network stack | todo | it should be plain USB host |

### M14 — Powered and cool while docked

| Task | State | Notes |
|------|-------|-------|
| Confirm charging works through the dock while Windows runs | todo | **needs-dock**, **needs-phone** |
| Decide the battery policy under Windows on ARM | todo | Windows may refuse to boot/install without a battery interface |
| Measure thermals under a desktop workload (not destructive, but needs the device) | todo | **needs-phone** |

### M15 — The donation build

| Task | State | Notes |
|------|-------|-------|
| Define what the recipient must be able to do without a console | todo | power on, log in, browse, keyboard/mouse |
| Make the build serviceable: what to do when one part fails | todo | a donated device will fail in the field |
| Keep the 9810 and 9820 flows separate and labelled | todo | golden rule: no unvalidated `Ready` |

## What is blocked on the user right now

| Needs | Milestones | Why |
|-------|-----------|-----|
| **needs-windows** | M7 | The Windows driver toolchain only runs on Windows; nothing here can compile these drivers |
| **needs-phone** | M10, M12, M14 | Further live reads (USB function, WLAN, thermals) — the phone is already connected, so these are cheap |
| **needs-dock** | M11, M13, M14 | Which dock we standardise on decides whether the HDMI path is EEM (likely dead on Windows) or a dock with a real GPU |
