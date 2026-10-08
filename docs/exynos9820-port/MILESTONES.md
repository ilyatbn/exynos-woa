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
batched at the end. Research evidence lives in [`notes/exynos9820-research.md`](../notes/exynos9820-research.md)
and [`../notes/dock-desktop.md`](../notes/dock-desktop.md).

**If you are starting a fresh session — especially one on Windows — read
[`HANDOFF.md`](HANDOFF.md) first.** It states what exists, what does not, the
exact build commands, and the unknowns that are boot hazards rather than build
errors.

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
| M7 | Build and test-sign the three 9820 drivers | **done** | built + test-signed 2026-10-09; **nothing installed or validated on the phone yet** |
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
| Diff our driver's register set against the 9820 kernel's | **done** | the EHI2C register set is unchanged; four constants are verified identical (base, 200 MHz source clock, GPIO base, GPP1 pins 4/5) — `notes/dock-desktop.md` §12 |
| Rename the package and retarget its hardware identification | **done** | `drivers/Exynos9820Hsi2c/` created as a copy; every name, the `.inx` strings, the ACPI match (`ACPI\SAMS9820`) and the `.rc` version block retargeted; the 9810 package is byte-unchanged |
| Mark the moved windows as `UNVERIFIED` rather than guessing | **done** | CMU_PERIC0 and SYSREG_PERIC0 have **no 9820 equivalent** at the 9810 addresses; the 9820 CMU banks are `1A200000`/`1BC00000`–`1BF00000` and the QCH numbers come from Samsung's `cal-if` headers — §12 |
| Carry the HSI2C #10 IRQ change into the ACPI tables | todo | SPI 404/GSIV 436 on 9810 → **SPI 388/GSIV 420** on 9820 — `notes/dock-desktop.md` §5 |
| Re-derive the bus clock for timing | todo | the 9820 DT declares `default-clk = 0xbebc200` = **200 MHz** |

### M4 — Touchscreen driver

| Task | State | Notes |
|------|-------|-------|
| Diff our command set against `sec_ts/y771` | **done** | the opcode list is in `notes/exynos9820-research.md` §8; our Windows driver is not a command issuer — it binds `ACPI\SCSY0761` and delivers HID reports, so the commands live on the firmware side |
| Handle the per-model panel firmware name and tuning values | **done** | the S10e's blob is `tsp_sec/y761_beyond0_phole.bin` — **y761**, the family our driver is already named after; `y771` is only the kernel source family. Tuning values are per-model: `afe_base 0x0205` (S10 was `0x0206`), `tclm_level 2`, `max_coords 4096×4096` — `notes/dock-desktop.md` §14 |
| Decide what fingerprint-on-glass changes mean for us | **done — does not apply** | the S10e's own shipping touch node has **no `support_fod`**; it carries `support_dex_mode` and `support_ear_detect_mode` instead |
| Re-qualify report delivery at 120 Hz | todo | the node declares `sync-reportrate-120`; our candidate's coalescing rules are written around a 64-event FIFO, so this model is where they are most likely to need re-testing — **needs-phone** |
| Create the `S6SY761TouchBeyond0` package | **done** | copy of the 9810 candidate. The driver has **no MMIO**, so the only device-specific numbers in it are the HID descriptor's panel extents (`271`/`558` hundredths of an inch) — left at the S9+'s and annotated above `G_DefaultReportDescriptor`. The four `sec_ts` opcodes it sends are verified identical |
| Derive the touch EINT for the 9820 | **done** | same pin as the 9810 (`sec,irq_gpio = <&gpa1 0 0>`) but **renumbered**: the shipping DT lists gpa1's SPIs as 13..20, so gpa1-0 is SPI 13 → **GSIV 45**, not the 9810's 76 — `notes/dock-desktop.md` §18 |

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
| Read the S10e's own partition table live | **done** | `/proc/partitions` and `by-name` give 31 partitions on `sda` plus four other LUNs; EFS is **the same LBA range as the 9810** — `notes/dock-desktop.md` §13 |
| Re-aim the write fence at the S10e's partition list | todo | the 9810's fence protects partitions that do not exist here; the derived map in §13 is **arithmetic, not a GPT read** — it needs root on the phone or the driver's own LBA-1 read |
| Handle the second LUN | todo | `cpefs` is on `sdd1`, a separate LUN; the 9810 driver was written against a single `sda` |
| Re-derive the DMA window from the S10e's DRAM banks | todo | 9810 `90000000..BC800000` vs S10e banks `80000000`+0x80000000, `880000000`, `900000000` |
| Create the `Exynos9820Ufs` package | **done** | copy of the validated 9810 package; the four windows carry the **verified 9820 bases**, the PMU base moves to `15860000`, the watchdog stays at `10050000`; PRAM, DMA window and every fence LBA are kept at 9810 values and marked `UNVERIFIED` so the `C_ASSERT` set stays self-consistent |

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
| Second pass: `/proc/iomem`, `reserved-memory/**/reg`, CMU devices | done | all still blocked (`/proc/iomem` -> `Permission denied`; only `1a240000.cmu_ewf` is a live platform device, the five CMU banks are not) - `notes/s10e-live-reads.md` §7.1 |
| Close the vendor-DT values that a fetch can close | done | DRAM banks, the five CMU bank windows, the touch-bus node (`default-clk 200 MHz`, pins `gpp1-4/gpp1-5`), the USB node, and every reserved carveout inside bank0 - §7.2. This also proves `0xCC000000` sits inside the `abox-rmem` **alloc range** on this SoC and `0xFED14000` inside `debug_snapshot.log_bcm`, so neither the 9810 fb base nor the 9810 breadcrumb address is reusable - §7.3 |
| Try to resolve CMU bank -> power domain | done | **dead end, recorded**: `cal-if/exynos9820/cmucal-{vclk,node,qch}.h` are enum lists only and that repo ships no clock driver to bind enum -> bank. Adopted instead: map the whole `0x0..0x20000000` as one `NS_DEVICE` region (what edk2-exynos' working 9820 port does), which covers every bank and both verified windows - §7.3 |

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

State: **doing** — the Windows toolchain is installed and the ARM64 WDK invocation now
resolves; HSI2C and UFS build, the touch build's host-mock half is blocked by files this
tree does not contain.

| Task | State | Notes |
|--------|-------|-------|
| Install the Windows toolchain | **done** | VS 2026 Community + WDK/SDK 10.0.26100 installed (`notes/windows-m7-toolchain.md` §1). Needs elevation for the VS component step; `warning.h` comes from the Windows 11 SDK component, not the WDK MSI. `KitsRoot10` registry value added elevated — that is what makes the WDK include paths resolve (§4.1) |
| Build the three drivers for ARM64 | **done** | All three compile and link clean with the WDK's real kernel flags (`/kernel`, `/FI...\shared\warning.h`, KMDF 1.15): HSI2C 45112 B, UFS 49152 B, touch 31744 B unsigned (`notes/windows-m7-toolchain.md` §4.2, §4.5, §4.6). The two post-link checks that still fail (`InfVerif.dll` missing from `build\10.0.26100.0\bin\x86\`, `aitstatic` exit 193) fail **identically on the validated 9810 package** -> toolchain gaps, not port defects; `infverif.exe` from `Tools\10.0.26100.0\x64` was run standalone instead. **Blocked, not skipped**: the touch package's host gesture tests cannot build here - `tests\TouchGestureTests.vcxproj:10` hardcodes `..\..\S6SY761TouchGpio\tests` for its mocks and that directory does not exist in this tree (§4.6) |
| Test-sign and package as `drivers.zip` equivalent | **done** | The WDK's own test certificate is reusable from the keystore: `signtool sign /i "WDKTestCert ilya,134359747320405644" /fd SHA256` signs all three. Signed package + three `.cat` files at `C:\Users\ilya\AppData\Local\Temp\opencode\signed-9820\` (`Inf2Cat /os:10_VB_ARM64` -> Errors: None, Warnings: None) (§4.7). **This is a locally generated test signature, not the released one** - the released binaries' cert salt is not recoverable from this tree |

### M8 — Firmware

| Task | State | Notes |
|------|-------|-------|
| Decide whether to build on Mu-Silicium's `S5E9820Pkg` | todo | their storage path is known-broken |
| Note the Windows-build constraint for this chip | todo | only 23H2 and lower boot there |
| Build the 9810→9820 constants table | **done** | 8 blocks identical (DWC3 core, touch bus, touch addr, CMU TOP, chip ID, UART, watchdog, PERIC0 GPIO); the PHY/PMU/SPEEDY/DECON/UFS regions move — `notes/dock-desktop.md` §4 |
| Resolve the 9820 GIC windows | **done** | **identical to 9810**: `interrupt-controller@10100000`, GICD/GICC/GICH/GICV = `10101000`/`10102000`/`10104000`/`10106000`, GICv2 — `notes/dock-desktop.md` §5 |
| Resolve the 9820 USB PHY/combo-PHY/PCS windows | **done** | one node, three windows: `10B00000`+0x200, `10AE0000`+0x2800, `10AF0000`+0x800; `phy_version 0x301`/`sub_phy_version 0x400` mean the 9810 PHY init cannot be pasted |
| Resolve the framebuffer geometry (1440×2960 → 1440×3040) | todo | the S10e panel is taller; the fb carveout must be re-derived |
| Re-derive the DRAM bank list | **done** | the SoC DT declares three 2 GiB banks (`80000000`, `880000000`, `900000000`); the live phone splits the low one at `C0000000` and `E1900000`, the same split points the 9810 used. **Bank0 is not 2 GiB of usable RAM**: the board file reserves a contiguous 729 MB, `D0000000`..`FD900000`, for the modem/CP stack — so the map publishes 1.25 GiB and the rest waits for the split sizes to be confirmed — `notes/dock-desktop.md` §17 |
| Write the 9820 silicon header `Exynos9820.h` | **done** | `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` — every read recorded with its source, every unknown left at 0 or at the 9810 value and marked `UNVERIFIED`; GIC and DWC3 windows are identical to the 9810's, PMU/SPEEDY/DECON/UFS regions move, and the fb base is **0** because it was never read on this SoC |
| Read the S10e's CPU topology live | **done — and corrected** | 8 CPUs in **two MPIDR clusters sized 6 + 2** (`cpu@0000`–`cpu@0005`, `cpu@0100`–`cpu@0101`), with **three efficiency classes** inside them (`ananke`/`prometheus`/`cheetah`). The first read suggested three clusters from the power-domain names; the device tree's node names settled it — `notes/dock-desktop.md` §15 then §16 |
| Create the 9820 memory-map library | **done** | `firmware/Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/` — 9820 windows in, both zero-base **and zero-size** placeholders skipped, the USB PHY mapped as one envelope because the DT's three windows overlap, and the 9810's PRAM breadcrumb compiled out since `0xFED14000` is **inside bank0** on this SoC |
| Publish the 9820 MADT | **done** | `firmware/Platform/Beyond0LtePkg/AcpiTables/Madt.aslc` — the GIC half transfers unchanged (same GICv2 bases); the CPU half is **4+2+2 across three efficiency classes**, and the MPIDR affinity **ordering** is marked UNVERIFIED because it is an inference from the power-domain naming |
| Port the DSDT | **done** | `firmware/Platform/Beyond0LtePkg/AcpiTables/Dsdt.asl` — device classes retargeted (`SCY0761` stays, the panel really is a y761), UFS windows and the ALIVE GPIO moved, HSI2C interrupt 436→420; the CMU/SYSREG windows, the parked Storport line, the touch EINT and the CPU sub-device buffers are **left at the 9810 values and documented as such**, because the DSDT and the driver header have to be corrected in one step |

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
| Investigate the **non-DisplayLink** path (preferred: cheaper, and the owner already has such a dock) | **done** | DeX uses **DisplayPort Alt Mode**, not DisplayLink and not EEM — and the S10e is listed as DP-alt-mode capable; the 9820 DT has a **USB/DP combo PHY** (`10AE0000`+0x2800, `has_combo_phy=1`) and a `displayport@130B0000` node — §11 |
| Set up DECON → DP scanout in firmware | todo | the repo never touches DECON today (`Exynos9810.h:203`); it reuses the sboot-lit framebuffer for the phone panel only |
| Make Windows report the external monitor as a second display | todo | **the hard part** — Windows enumerates displays only through a WDDM driver; a second output is real driver work, not a config change |

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
