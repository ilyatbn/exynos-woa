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

### Priority order — the user's, recorded 2026-10-09

The bar for "working" is **not** the dock. In order:

1. **Boots Windows on the S10e, and Windows sees disk, RAM and CPU cores.** Same bar this
   repo already meets on the S9+. **This is the finish line of phase one (M3–M9).**
2. Stretch goals, in the order wanted: **WiFi**, **keyboard/mouse via the dock**, **LAN**,
   **HDMI**. "If we don't get there, we don't get there" — they are explicitly *not* a licence
   to lower the first bar.

So M10–M15 are a **second phase** that starts only once phase one is validated on the device.
The dock research already in `notes/dock-desktop.md` is the map for that phase, not a
reordering of this one.

Two decisions from the user that change the second phase's shape:

- **The dock:** they own **both** a DisplayLink dock and a non-DisplayLink one, and prefer the
  **non-DisplayLink** path because it is cheaper. So the plan must investigate how Samsung's
  own **DeX** drives an external monitor — the Exynos side already hints at the mechanism:
  the 9820 PHY declares `has_combo_phy = 1` and there is a `displayport@0x130B0000` node
  (`notes/dock-desktop.md` §5). DisplayLink stays as the fallback, not the first choice.
- **Risk:** there is **only one device**. The powered-dock-plus-host-mode test is therefore
  **deferred to a later stage**, and until then nothing on the S10e may be destructive.
  The first thing to establish is simply that Windows boots at all.

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
7. **Hardware-safety rules learned from prior art, applied to every milestone:**
   - A **powered** USB hub/dock plus USB host mode "can irreversibly damage your device"
     (https://github.com/n00b69/woa-mh2lm5g/blob/main/guide/materials.md) → the donation build
     standardises on a **passive** dock unless host-mode-plus-power is validated on a spare unit.
   - Never adopt a boot path that **erases a partition** to make Windows boot (the `x1s` pattern
     in Mu-Silicium Status.md) — a donated device must always be able to go back to Android.

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
| Carry the HSI2C #10 IRQ change into the ACPI tables | todo | SPI 404/GSIV 436 on 9810 → **SPI 388/GSIV 420** on 9820 — `notes/dock-desktop.md` §5 |
| Re-derive the bus clock for timing | todo | the 9820 DT declares `default-clk = 0xbebc200` = **200 MHz** |

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
| Build the 9810→9820 constants table | **done** | 8 blocks identical (DWC3 core, touch bus, touch addr, CMU TOP, chip ID, UART, watchdog, PERIC0 GPIO); the PHY/PMU/SPEEDY/DECON/UFS regions move — `notes/dock-desktop.md` §4 |
| Resolve the 9820 GIC windows | **done** | **identical to 9810**: `interrupt-controller@10100000`, GICD/GICC/GICH/GICV = `10101000`/`10102000`/`10104000`/`10106000`, GICv2 — `notes/dock-desktop.md` §5 |
| Resolve the 9820 USB PHY/combo-PHY/PCS windows | **done** | one node, three windows: `10B00000`+0x200, `10AE0000`+0x2800, `10AF0000`+0x800; `phy_version 0x301`/`sub_phy_version 0x400` mean the 9810 PHY init cannot be pasted |
| Resolve the framebuffer geometry (1440×2960 → 1440×3040) | todo | the S10e panel is taller; the fb carveout must be re-derived |
| Re-derive the DRAM bank list | todo | vendor DT shows `memory@80000000` = 0x80000000+**0x80000000**, plus `880000000`, `900000000` — not the 9810 layout |

### M9 — Installer

| Task | State | Notes |
|------|-------|-------|
| Add the S10e to the stage catalog as `Guided`, not `Ready` | todo | golden rule |
| Back up EFS/modem/partition table before any destructive step | todo | same as the 9810 flow |

### M10 — USB-C host mode (the gate for keyboard, mouse, LAN)

| Task | State | Notes |
|------|-------|-------|
| Find what the 9810 project does for USB host today | **done** | our own `tools/twrp-winre/build.py` anchors `sys.usb.controller = 10c00000.dwc3` — `notes/dock-desktop.md` §3 |
| Locate Exynos DWC3/USBFN support in Mu-Silicium | **done** | `UsbfnDwc3Dxe` exists, but Status.md says USB Host Mode is ❌ on `starlte`, `beyond1lte` and `x1s` — nobody has finished it on Exynos |
| Read the S10e's own USB-function and DWC3 nodes on the live phone | **done** | USBFN at `0x10C00000`, declares a `dwc3` child; the host device is **not instantiated** under Android — `notes/dock-desktop.md` §1 |
| Establish what Windows needs for a phone's USB-C port to be a host | **done** | it is a **dual-role (DRD)** controller: the shipping DT says `dr_mode = "otg"`, a dock needs **`host`** — a firmware/device-tree decision, not a Windows one; `notes/dock-desktop.md` §8 |
| Check prior art: phones running Windows with a keyboard attached | **done** | Snapdragon targets do it (Renegade OOBE assumes a USB keyboard; a Windows-side "USB Host Mode Control" toggle exists) — Exynos targets do not |
| Decide passive vs powered dock | todo | **safety**: the community warns a powered hub plus host mode "can irreversibly damage your device" |
| Confirm the port can be host **and** receive power at once | todo | the S10e has **one** USB-C connector — the dock occupies the charging port; Mu-Silicium shows USB Power Delivery ❌ on every Exynos target |
| Inventory what our firmware already has | **done** | `UsbDebugHid.c` is a complete DWC3 register vocabulary, but it programs **peripheral** mode (`PRTCAP_DEVICE`, EP0 + HID IN) and is compiled **off** by default — §9 |
| Re-point the CMU clock/QCH windows before flipping PRTCAP | todo | 9820 CMU banks are `1A200000`/`1BC00000`/`1BD00000`/`1BE00000`/`1BF00000`, not the 9810's `11000000` — §9 |

### M11 — External display through the dock

| Task | State | Notes |
|------|-------|-------|
| Determine how a USB-C dock's HDMI is fed at all | **done** | two mechanisms: EEM/SLDCOM mirroring (no Windows driver) or a **DisplayLink GPU in the dock** (WDDM driver) — `notes/dock-desktop.md` §2 |
| Establish whether Windows supports EEM/SLDCOM docks | **open — uncited** | still inference, not fact; four search routes already failed (§7) — the DisplayLink decision does not depend on it |
| Read the Linux reference implementation (EMD2) to see what the protocol requires | todo | https://github.com/emd2v2/emd2 |
| Confirm DisplayLink on Windows-on-ARM | **done** | Synaptics 11.3+ lists Windows 10/11 **ARM64**; Targus validated 11.5M3 for ARM64; DisplayLink markets "any ARM SoC", "smartphones" |
| Confirm a phone driving a DisplayLink dock is a supported configuration | **done** | DisplayLink's own docs say the phone "needs to become the USB host" and that a phone-as-desktop workstation is the intended use — §10 |
| Choose the dock by DisplayLink chip, not by "USB-C dock" | **done** | the vendor warns many USB-C docks use USB "Alternate mode" instead; look for the **Plug and Display** logo — §10 |
| Check whether the target dock uses EEM or a real GPU in the dock | todo | **needs-dock** — we now know which kind we must buy |
| Decide the fallback display path if EEM is a dead end on Windows | **done** | DisplayLink is **the fallback**, not the first choice — see below |
| Investigate the **non-DisplayLink** path (preferred: cheaper, and the owner already has such a dock) | todo | how Samsung's own **DeX** drives an external monitor; the 9820 hints at the mechanism with `has_combo_phy = 1` and a `displayport@130B0000` node — §5 |

### M12 — WiFi

| Task | State | Notes |
|------|-------|-------|
| Identify the WLAN hardware in the S10e (live) | **done** | Broadcom **DHD** over Samsung reserved memory + blobs (`bcmdhd_sta.bin`, `nvram_net.txt`) — not a PC wifi card; `notes/dock-desktop.md` §1 |
| Find existing Windows drivers for Samsung Exynos WLAN | **done — dead end** | there is nothing to bind to: the vendor DT has **no WLAN device node** (only GPIO pins), and the live `wlan` platform node has no driver and an empty `modalias` — §6 |
| Decide whether WiFi is required at all when LAN is available | **done** | **LAN-first**; if WiFi is genuinely needed, put a **USB WiFi adapter in the dock** (an ordinary Windows device once M10 works), not the phone's WLAN |

### M13 — LAN through the dock

| Task | State | Notes |
|------|-------|-------|
| Confirm Windows ARM64 has a driver for a generic USB ethernet adapter | todo | the 9820 kernel already lists ASIX AX88179/178A, SMSC, RTL8150/2 — so the chip class is known; the Windows driver half is still open |
| Confirm the dock's ethernet does not need the phone's own network stack | **done** | it is a plain USB-net device; only host mode (M10) is required |

### M14 — Powered and cool while docked

| Task | State | Notes |
|------|-------|-------|
| Confirm charging works through the dock while Windows runs | **done — dead end** | the vendor states it for the same hardware: "Android devices cannot currently enable USB and charge at the same time… not possible to charge while docked" — §10; design the build to run **on battery while docked** |
| Decide the battery policy under Windows on ARM | **partly done** | our repo has **no battery/UPS driver at all** today, so Windows already runs with an invisible, unmanaged battery — that is the honest starting point, not a solved state (`notes/dock-desktop.md` §8) |
| Assume battery exhaustion is normal and make it survivable | **done — design rule** | a donated phone-desktop must degrade gracefully and be restartable by a non-technical recipient |
| Measure thermals under a desktop workload (not destructive, but needs the device) | todo | **needs-phone** — Samsung's own thermal stack (`devfreq_*`, `sec_thermistor@*`) has no Windows equivalent |

### M15 — The donation build

| Task | State | Notes |
|------|-------|-------|
| Define the honest "working" bar | **done** | Windows boots on the S10e and Windows sees **disk, RAM, CPU cores** — the user's own words; that is what a donated unit may be described as |
| Keep the stretch goals out of the definition | **done** | WiFi, keyboard/mouse via dock, LAN, HDMI are stretch: "if we don't get there, we don't get there" — they never lower the bar above |
| Make the build serviceable: what to do when one part fails | todo | a donated device will fail in the field |
| Keep the 9810 and 9820 flows separate and labelled | todo | golden rule: no unvalidated `Ready` |

## What is blocked on the user right now

| Needs | Milestones | Why |
|-------|-----------|-----|
| **needs-windows** | M7 | The Windows driver toolchain only runs on Windows; nothing here can compile these drivers |
| **needs-phone** | M10, M14 | Further live reads (USB function, thermals) — the phone is already connected, so these are cheap |
| **needs-dock** | M11, M13 | The owner has **both** a DisplayLink dock and a non-DisplayLink one; the non-DisplayLink path is preferred because it is cheaper |
| **one device only** | M10, M14 | There is **no spare unit yet**: nothing destructive may run on the S10e until Windows booting is established, and the powered-dock/host-mode risk is deliberately deferred to a later stage |
