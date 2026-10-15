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
| M8 | UEFI firmware for `beyond0lte` | **done** | M5, M6 — the platform **builds** (`BEYOND0LTE_UEFI.fd` 7 340 032 B, `FVMAIN_COMPACT` 3840 B free) with the ACPI installer, the ported UFS host controller, the reset library and the RAM manager; the two remaining 9810 subsystems (HSI40/GPIO handoff, platform `PlatformBootManagerLib`) are **not applicable** — the 9810 build never used them. **Nothing is validated on an S10e**: the boot is Guided, and the §5 hazards (fb base, PRAM choice, write-fence LBAs, UFS hw-rev class) are still open — the DSDT sub-device buffers were closed |
| M9 | Installer: add the S10e behind honest gating | doing | M7, M8 — the target and the per-device stage gating exist; the phone-touching stages have no `beyond0lte` implementation and stay `NotImplemented` |
| M10 | USB-C **host** mode: keyboard, mouse, dock attachments | todo | M8 |
| M11 | External display: the dock's HDMI over the USB-C link | todo | M10 |
| M12 | WiFi on Windows for this chip | doing | M8 |
| M13 | LAN through the dock | todo | M10 |
| M14 | Stay powered and cool while docked | todo | M8 |
| M15 | The assembled donation build (repeatable, serviceable) | todo | M9–M14 |
| M16 | GPU acceleration: Mali-G76 (Bifrost) on Windows via **Panfrost** — WDDM kernel driver + Mesa user-mode driver | todo | M8 — `notes/gpu-panfrost.md`, `notes/gpu-panfrost-repos.md`; supersedes the 2026-10-10 "not feasible" verdict; ladder G1-G13 (UEFI probe → write-value job → KMDF → full WDDM 1.3 KMD on the GOP fb → Mesa 26.2 d3d10umd+panfrost → DWM), reusing gahingwoo/wddm-mali-g52 (`notes/gpu-wddm-mali-g52.md`) |

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
| Resolve the CMU windows from Samsung's cal-if instead of guessing | **done** | The DT's five-window `cmu` node is only a subset. `cal-if/exynos9820/cmucal-sfr.c` starts with `cmucal_sfr_block_list[]` (34 `SFR_BLOCK(name, addr, size)`) and gives `CMU_PERIC0 0x10400000+0x8000` (the 9810 base was right, its length was `dbg_offset` 0x4000 not the block) and `CMU_FSYS0A 0x10a00000+0x8000`. Gate offsets taken by name: IPCLK `0x2094`, PCLK `0x2098`, RSTNSYNC `0x2054`, OSCCLK source `0x2008`, QCH `0x3038` (unchanged from 9810); `SFR_ACCESS(..._MANUAL,20,1)`/`(..._CG_VAL,21,1)` confirm the driver's masks. `notes/s10e-live-reads.md` §7.5 |
| Carry the HSI2C #10 IRQ change into the ACPI tables | **done** | SPI 404/GSIV 436 on 9810 → **SPI 388/GSIV 420** on 9820 — the DSDT line carries the verified number (`notes/dock-desktop.md` §5) |
| Re-derive the bus clock for timing | **done** | the 9820 DT declares `default-clk = 0xbebc200` = **200 MHz**, identical to the 9810 — no timing change |

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

State: **doing** — the write fence is now this device's own table (read, not
derived), the DMA window and the second LUN are not.

| Task | State | Notes |
|------|-------|-------|
| Move the four hardware bases to the 9820 values | done (planning) | HCI `0x13D60000`, VS `0x13D61100`, UNIPRO `0x13D50000`, protector `0x13D70000` — all read from the vendor DTS; HCI confirmed on the live phone |
| Confirm the helper region address on the 9820 | **done** | `ufs-phy { reg = <0x0 0x13D64000 0x800> }` — verified, no longer a guess |
| Re-derive the timeout clock from the 9820 tick rate | todo | **UNVERIFIED**: the kernel computes `mclk_rate/1000000 & 0x3FF` from the UNIPRO clock; 166 MHz is the candidate (Mu-Silicium uses it for 9810 AND 9830) |
| Port the newer-revision behaviour | **done** | `hw-rev = UFS_VER_0006` is a **class**, not a register value (`ufs.h`: 4/5/6). The exact-equality HCI check that used to clear `WarmStateValid` is now a non-fatal `UFS_DIAG_FAILURE_HCI_VERSION_UNCONFIRMED` bit (57, free) reported by the driver and named in `UfsDiag.c`'s decoder, so the first Windows run tells us the real value instead of refusing disk on an unmeasured constant — `notes/windows-m7-toolchain.md` §5 |
| Use the second template | todo | `edk2-exynos` already has `Exynos9820Pkg/Library/ExynosUfsLib` (`ufs.c`, `scsi.c`) — a UEFI 9820 UFS port exists outside Mu-Silicium |
| Keep the diagnostic path able to print what the hardware says | todo | must be readable before any destructive step |
| Read the S10e's own partition table live | **done** | `/proc/partitions` and `by-name` give 31 partitions on `sda` plus four other LUNs; EFS is **the same LBA range as the 9810** — `notes/dock-desktop.md` §13 |
| Re-aim the write fence at the S10e's partition list | **done** | the whole interlocking block in `Exynos9820Ufs.h` replaced in one step from the live GPT (`notes/s10e-root-reads.md` §2/§8/§9): fence **1 983 488..31 236 090** (ESP through this disk's LastUsableLBA), protected ranges now sda29 OMR and sda30 CP_DEBUG, GPT backup first LBA **31 236 091**, expected last LBA **31 236 095**, readout scratch **31 234 816..31 235 071**. Builds clean (`EXACT_BUILD_OK`, driver 49 152 B) and a deliberate off-by-one was proved to break the compile, so the `C_ASSERT`s really gate these constants. **Not validated on the device** — the fence is a compile-time fact, not a boot result; `UfsDiag.c`'s own `UFS_SCRATCH_LBA` is still the 9810's and is a separate task — `notes/m8-ufs-fence.md` |
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
| Port the DSDT | **done** | `firmware/Platform/Beyond0LtePkg/AcpiTables/Dsdt.asl` — device classes retargeted (`SCY0761` stays, the panel really is a y761), UFS windows and the ALIVE GPIO moved, HSI2C interrupt 436→420; the CMU/SYSREG windows, the parked Storport line and the touch EINT are **left at the 9810 values and documented as such** (the CPU sub-device buffers were later re-derived, see the row below) |
| Re-derive the DSDT CPU sub-device `_MAT` buffers | **done** | the eight buffers are 80-byte `EFI_ACPI_6_3_GIC_STRUCTURE` entries (`Acpi63.h:520-539`); the 9810's equal its own MADT GICC entries field for field, so `_MAT` = the MADT entry with the matching `_UID`. Rewritten to the 9820 values from `Madt.aslc:149-163` (MPIDRs `0x00`–`0x05`/`0x100`,`0x101`; classes 2/1/0) and verified in the built FFS sections, DSDT and MADT agreeing — `notes/m8-dsdt-mat.md` §5, `notes/claude-mat-layout.md`. Windows parsing them is still unvalidated |
| Settle the PRAM breadcrumb address | **done — decided, deliberately not enabled** | `debug_snapshot` occupies `0xFD900000`→`0x100000000` contiguously, so the 9810's `0xFED14000` sits inside a vendor log region; the unclaimed gap `0xD9801000`…`0xF0000000` (immediately after `s5100_msi`, `Exynos9820.h:102-103`) is above the published bank0 stop `0xD0000000`, so Windows never sees it as RAM. `BEYOND0LTE_PRAM_DIAG` stays `0` — diagnostics off for the first boot — `notes/vendor-dt-reserved.md` |
| Prove what `adb` can and cannot read | **done** | every `reserved-memory/*/reg` read is `Permission denied`, and `/proc/partitions` gives sizes not LBAs — so the fb base and the partition LBAs are **not** closable from the phone; the vendor DTS (or the driver's own LBA-1 read under Windows) is the only route — `notes/s10e-live-reads.md` §9 |
| Read the S10e's partition layout by name | **done — names only** | `ls -l /dev/block/by-name` gives BOOT=**sda14**, RECOVERY=**sda15**, and the Android data run sda26 vendor … sda31 userdata, so the driver's 9810 fence (sda19..sda25 protected, BOOT/RECOVERY sda10/sda11) is confirmed to cover the wrong ranges; exact starts are unread and `Exynos9820Ufs.h:975-978` forbids replacing the block from arithmetic — `notes/s10e-partitions.md` |
| Get the S10e's real geometry from Samsung's own PIT | **done** | `Dumps\BEYOND0LTE_EUR_OPEN.pit` parsed directly (PIT Magic misparses the fields): 41 entries at 0x1C stride 0x84, start/length in 4096-byte units — proven by contiguity and by matching 30/31 of the phone's sizes exactly. Gives `WindowsOffset 8 811 184 128`, `DiskBytes 127 943 049 216`, `WindowsBytes 119 126 622 208`, `CacheBytes` unchanged, BOOT size unchanged, RECOVERY 67 633 152 — `notes/pit-geometry.md` |
| Make the installer geometry per-device | **done — code, unvalidated on hardware** | `PartitionMap` now carries a `GeometryType` per phone (`For(codename)`), the VHDX build and the partition-stage check take the identified phone's geometry, `Validation.cs` limits boot images by the smaller RECOVERY; a new test pins the beyond0lte numbers and the suite is green (189 tests). Still open: the GPT GUIDs, an offset check against the live GPT, and the star2lte payload names/messages in `ToolsetManager.cs` |

| Create the 9820 DSC/FDF and actually build the platform | **done** | `firmware/Platform/Beyond0LtePkg/Beyond0LtePkg.dsc` + `.fdf`; `firmware/tools/build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG` exits 0 and produces `BEYOND0LTE_UEFI.fd` (7 340 032 B, sha256 `ef4381b7...`). Toolchain bootstrapped on this machine: edk2 clone + submodules, LLVM, GnuWin32 make, `iasl-win-20260408`, and BaseTools host binaries built with `Edk2ToolsBuild.py -t VS2026` (`BrotliCompress` fails - its brotli submodule was not pulled; nothing here needs it). Five DSC fixes were needed and each is an edk2-master drift, not a hardware question: `LzmaDecompressLib` has no provider, `GptLib` instance added, `PlatformBootManagerLib` from ArmPkg, `PcdDefaultTerminalType|4` (MdePkg token space), `ResetSystemRuntimeDxe` dropped - `notes/m8-build.md` |
| Port the ACPI installer | **done** | `firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteAcpiPlatformDxe/` — the tables now have a consumer, so MADT/DSDT are published through `EFI_ACPI_TABLE_PROTOCOL`. Its PRAM breadcrumb and the GTDT memory-mapped counter frame are **compiled out** (no safe 9820 address; the 26 MHz/MCT base are 9810 values). Rebuild is clean: `FVMAIN_COMPACT` 99% full with 2984 bytes free, `BEYOND0LTE_UEFI.fd` 7 340 032 B sha256 `821af51c...` — `notes/m8-build.md` §6 |
| Publish the two high DRAM banks in the memory map | **done** | `PlatformMemoryMapLib.c` now maps `0x880000000`+2 GiB and `0x900000000`+2 GiB as their own cached entries (vendor DT reads, `Exynos9820.h:87-90` VERIFIED, `notes/s10e-live-reads.md` §7.2); bank0 stays published only to `0xD0000000` because the 729 MB modem/CP reservation and the debug-snapshot tail to `0x100000000` are reads, not a conservative stop. The map offers 5.25 GiB, not 1.25. Rebuild clean: `FVMAIN_COMPACT` 2976 bytes free, FD 7 340 032 B sha256 `81b0fd9e...` — `notes/m8-build.md` §7 |
| Port the UEFI-side UFS host controller | **done** | `firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteUfsHcDxe/` (.c + .inf, FILE_GUID `…5545`) — every UFS window remapped to the `EXYNOS_UFS_*` macros (`Exynos9820.h:268-278`, all VERIFIED), intra-window offsets carried, the whole `0xFED1xxxx` PRAM/diag/stage-fnptr telemetry channel **compiled out** behind `BEYOND0LTE_UFS_DIAG 0` (not retargeted), DRAM bounds derived from `EXYNOS_DRAM_BASE+SIZE`. DSC `[Components.common]` + FDF storage section wired. Builds clean: `FVMAIN_COMPACT` 99% full with **2352 B free**, FD 7 340 032 B sha256 `1bce9446...`, module `.efi` 55 808 B present in the FV — `notes/m8-ufs-hc.md` |
| Port the reset library | **done** | `firmware/Platform/Beyond0LtePkg/Library/Beyond0LteResetSystemLib/` (.c + .inf, GUID `…5546`) — only the architected PSCI path is live; the PMU SWRESET/PS_HOLD fallback is **compiled out** (`BEYOND0LTE_PMU_RESET 0`, the 9820 PMU base is a read, the intra-block offsets are not) and the `0xFED14000` breadcrumb likewise (`BEYOND0LTE_PRAM_DIAG 0`). DSC `ResetSystemLib` instance + `ResetSystemRuntimeDxe` re-added in DSC and FDF. Builds clean, but `FVMAIN_COMPACT` is now **99% full with 968 B free** — `notes/m8-reset-lib.md` |
| Port the RAM manager | **done** | `firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteRamManagerDxe/` (.c + .inf, GUID `…5547`) — the decision is recorded first: the page-table map in `PlatformMemoryMapLib` and the DXE **GCD** SystemMemory map are different things, so the module is still needed. The 9810's hardcoded bank-1 ranges are replaced by `EXYNOS_DRAM_BANK3/4_*` (verified base AND size); banks with unread extents are skipped, not guessed; the `0xFED17F60` status stamp is compiled out. DSC+FDF wired. Builds clean: `FVMAIN_COMPACT` auto-grew one block to 700416 B with 3864 B free, FD unchanged at 7 340 032 B sha256 `c72b3490…` — `notes/m8-ram-manager.md` |
| The HSI40/GPIO-handoff subsystem | **not applicable** | checked, not ported: `rg Hsi40|GpioHandoff` over **both** `Star2LtePkg.dsc/.fdf` and `Beyond0LtePkg.dsc/.fdf` finds nothing, and neither `AcpiTables.inf` lists `GpioHandoffDevices.asl`/`Hsi40Devices.asl` — so the 9810's own validated build never used them either. They are orphan trial/diagnostic sources (~108 KB), and `Exynos9820.h` has no HSI40 window at all (the touch bus is HSI2C). `notes/m8-hsi40.md` |
| The 9810's own `PlatformBootManagerLib` | **not applicable** | decided, not ported: the library CLASS is already resolved to `ArmPkg/Library/PlatformBootManagerLib` (`Beyond0LtePkg.dsc:194`), which is what `BdsDxe` requires. The 9810 platform instance added the platform BDS hook and its USB-debug modes, neither of which is on the phase-one path |
| Produce a flashable UEFI boot image for the S10e | **doing — image built AND flashed to the recovery slot twice; first boot OBSERVED 2026-10-10: fell back to download mode, Windows NOT established** | The platform builds (`BEYOND0LTE_UEFI.fd`, 7 340 032 B, present at `firmware/edk2/Build/Beyond0LtePkg/DEBUG_CLANGPDB/FV/`) but nothing turns an FD into an image sboot accepts. `firmware/tools/pack-uefi.ps1` is star2lte-only and calls `tools/repack-with-ref.py`, which — like `firmware/device-data/` and `tools/bootimg-pack.sh` — **does not exist in this checkout** (`Test-Path` false for all three), so even the 9810 pipeline is not reproducible here. DONE (a): `firmware/tools/beyond0lte-bootshim/bootshim.S` — the 9810-shaped constants retargeted or compiled out (paint `0xCA000000` at 1440×3040, temp SP `0x83000000` marked UNVERIFIED, ramoops compiled out behind `BEYOND0LTE_PRAM_DIAG` 0 because `0xFED14000` sits inside `debug_snapshot.log_bcm`/`log_llc`, watchdog cl0/cl2 bases VERIFIED at `0x10050000`/`0x10060000`), and a defect the 9810 shim had (a second `ldr x5, _uefi_base` after the copy, reading a literal the copy itself overwrote) fixed; DONE (b): `firmware/tools/repack-with-ref.py` (the tool `pack-uefi.ps1:121` calls but that never existed in this checkout) wraps `[shim][FD]` in the phone's own recovery container with `page_size` 2048 read from the ref and ramdisk/second/dt/tail kept byte-identical, output 18 253 824 B; still needed: (c) the still-unknown constants named `UNVERIFIED` in the source, each with the read that would close it: `UEFI_BASE` (`Beyond0LtePkg.fdf:68` says `0x80080000`, marked UNVERIFIED; the real sboot kernel-drop address is **not** closable by a cheap read — **CORRECTED 2026-10-10: it is closable, `notes/k4b-drop-address.md` K01-K03 derives `0x80118000` from `/proc/iomem` (`Kernel code 0x80198000-81717fff`, `Kernel data …-832cffff`) plus the `Image` header's `text_offset 0x80000` / `image_size 0x3138000`, and the arithmetic is exact**), the required Linux-header `image_size`, and the 9820's write-protected (RKP-analogue) zones **(d) K10 partial closure (2026-10-10, `notes/K10-rkp.md` T15-T18): the live 9820 DTB, read in a TWRP root shell, settles the mechanism half — `el2_code` `0x87000000+0x200000` and `el2_earlymem` `0x97E000000+0x200000` (both owner `el2,uh`) are sboot-added and now VERIFIED, `kaslr` is `0x80100000+0x100000` (owner `kernel-kaslr`), and Samsung's own `rkp.h` shows the kernel-text window is runtime-derived from `_text.._erodata`, so there is no 9820 constant to copy from 9810. Two carried numbers are now known WRONG: the 9810 ramoops `0xFED14000` sits inside live `log_bcm` (`0xFEB10000+0x400000`), and the UFS DMA window `0x90000000..0xBC800000` overruns the live `memory@80000000` bank, which ends at `0xBAB00000`**. **Hazard recorded from Team Win's own device page:** a stock S10e is protected by Android Verified Boot — flashing TWRP without first flashing a verity-disabled `vbmeta` image leaves the device unable to boot; this disk's `vbmeta` is LBA 83 584..83 599 (`notes/s10e-root-reads.md` §2). Ground rules 5/6 still forbid flashing anything until Windows booting is established. Full detail: `notes/m8-uefi-image.md` §16, `notes/m9-install-attempt.md` §15 |

| Re-decide the FD/UEFI run address against the live DTB | **done (as a decision, not a validation)** | The drop IS a read: sboot loaded this boot's `Image` at **`0x80118000`** and its kernel window is `0x80198000..0x832D0000` (`notes/k4b-drop-address.md` K01-K03, `/proc/iomem` + the `Image` header, arithmetic exact), which made the old `0x80080000` provably inside the loaded-image region and overlapping `sec_debug_magic` and the `kaslr` node — the shim's self-overwrite was CONFIRMED, not hypothetical. New base **`0x83300000`** (first address above the window, below `mm_debug 0x91200000`, inside bank0 which ends `0xBAB00000`, disjoint from src so the copy cannot hit itself) moved together in `.dsc` `PcdFdBaseAddress`/`PcdFvBaseAddress`, `.fdf` `FD_BASE_ADDRESS`, the shim's `UEFI_BASE` default and `pack-uefi-beyond0lte.ps1`'s `-UefiBase` default. Verified by running: build exit 0 with `0x83300000` propagated into `FVMAIN_COMPACT.inf` and the `PeilessSec` AutoGen; shim linked at `-Ttext=0x83300000` (`_head 0x83300000`, `_payload 0x83300120` = last byte of the 288 B image); packer run end to end against the pulled recovery ref → shim 288 B, kernel 7 340 320 B, output **18 255 872 B**. Still untested: whether sboot reuses the same drop base for a 7.6 MB image, and nothing has booted |
| Stop publishing the monitor-owned RAM as Windows RAM | **done** | `PlatformMemoryMapLib.c` now emits each bank as the gaps around a sorted `mMonitorReservations` table (`sec_debug_magic`, `kaslr`, `el2_code`, `el2_earlymem` — all live DTB reads, owners `el2,uh` / `kernel-kaslr`), so live EL2 code is never handed to Windows; bank0 also clamped to the live `0x3AB00000`. The **Android-side heaps** (`camera`, `secure_camera`, `mm_debug`, `modem_*`) are deliberately still RAM because no read says what breaks if Windows uses them — that half stays an open question, not a decision dressed as a fact (`Exynos9820.h`, `notes/s10e-dtb-mmio.md` S02-S04) |
| Re-pick the UFS DMA window inside the live bank0 | **done (still UNVERIFIED)** | the carried 9810 window `0x90000000..0xBC800000` is now known ILLEGAL (live bank0 ends `0xBAB00000`); re-picked to the free band `0xA7400000..0xBAB00000` (above the camera reservation, inside bank0), kept marked UNVERIFIED, `C_ASSERT(FIRST<LAST)` added, both build modes compile clean — `notes/K10-rkp.md` T18, `notes/s10e-dtb-mmio.md` S02, `tasks/T01-ufs-dma-window.md` |

| Fix the framebuffer RAM/DEV overlap | **todo** | `EXYNOS_FB_BASE 0xCA000000` is published `MMAP_DEV` but sits inside the bank0 RAM gap `0x87200000..0xBAB00000`, so the map still has a cached-RAM and Device entry covering the same range — pre-existing, but the reservation carve made the gap explicit and this is the same class of hazard; the fix is to split the gap around the FB like the reservations — `notes/s10e-dtb-mmio.md` S04 |

| Build the UEFI container as Android boot image **header-v2 with a DTB section** | **done (device-observed 2026-10-10: on the BOOT partition sda14 the v2 image cleared the DT check — boot #4 looped instead of `DT LOAD fail`, and manual download mode shows the normal banner, `notes/m8-uefi-image.md` B19-B21; on the RECOVERY slot it did NOT, B18)** | built by `firmware/tools/build-bootimg-v2.py` (`d1422da`); the first two boot attempts failed at sboot's `dt table header check fail` → `DT LOAD fail`; the 9820's Android-12 ABL `load_kernel` wants a header-v2 image with a real DTB section (edk2-exynos `Platform/Samsung/exynos9820/s10.sh.inc` uses AOSP `mkbootimg.py --header_version 2 --dtb …`), not the Samsung header-v0 + DTBH our `bootimg.py`/`repack-with-ref.py` emit (`dt_size=1`). Fix = extend our builder to v2 (fallback: AOSP mkbootimg) — `notes/edk2-bootshim.md`, `tasks/T09-boot-payload-dtb.md` |
| Rewrite the 9820 boot shim to the device-proven edk2-exynos minimal flow | **done (built and disassembled; NOT booted)** | boot #4 cleared sboot's DT check and then boot-looped (`notes/m8-uefi-image.md` B20-B22); our shim did watchdog/SCTLR/cache/paint/SP work the proven 9820 shim never does. `firmware/tools/beyond0lte-bootshim/bootshim.S` is now instruction-for-instruction edk2-exynos `tools/BootShim/BootShim.S` (zero header, `EDK2-MSM` tag, `UEFI_BASE`/`UEFI_SIZE` quads, ldp/stp copy, `br x5`); a half-finished uncommitted rewrite (truncated copy loop, bogus `0xd420007e` 'wfe') was replaced. Verified by running: clang/ld.lld/objcopy exit 0, 144 B, `_payload 0x83300090` = last byte, objdump matches. Cost: no panel paint any more, so a failure is now silent. Open: prior art runs the FD at `0x90000000` (`configs/exynos9820.conf`), ours is the DECISION `0x83300000` (B23) |
| Move the FD/UEFI run address to the 9820 prior-art value `0x90000000` | **done (built; NOT booted)** | edk2-exynos `configs/exynos9820.conf` FD_BASE=0x90000000; ours was the DECISION `0x83300000`. Moved in all four lockstep sites; 0x90000000..0x90700000 is clear of every reservation read. Verified by running: FD build exit 0 with the PCD at 0x90000000 in PeilessSec AutoGen, packer shim checks pass, kernel blob = 144 B shim + FD byte-exact (`notes/m8-uefi-image.md` B25) |
| Make the shim's progress visible on the panel (boot #5 diagnostic) | **done (built; NOT booted)** | `BEYOND0LTE_SHIM_DIAG` (default ON): prior-art DECON enable `0x1281`→`decon_f 0x19030000` (edk2-exynos PlatformUtils.c), then WHITE band rows 200-649 on entry and rows 750-1199 before `br` into the FD. DIAG=0 is byte-identical to the plain prior-art shim. Verified by assembling both modes + objdump + packer checks (`notes/m8-uefi-image.md` B26) |
| Put PeilessSec's stack/UEFI region in mapped bank0 RAM | **done (built; NOT booted)** | boot #5 reached the FD and looped (B28). `PcdSystemMemorySize` 0x50000000 put the SEC stack + UEFI region at 0xC8000000..0xD0000000, which the MMU map does not cover (and bank1 faulted for UEFI on the 9810). Now 0x3AB00000 (= verified bank0 size) → region 0xB2B00000..0xBAB00000. Verified by build + AutoGen (`notes/m8-uefi-image.md` B29) |
| Tell a UEFI-initiated reset from an external one (boot #7 diagnostic) | **done (built; NOT booted)** | boot #6 got further (brief output) then reset. `BEYOND0LTE_RESET_HOLD=1` in Beyond0LteResetSystemLib: every reset path paints a third white band and parks instead of resetting. MUST be switched off before a real install (`notes/m8-uefi-image.md` B32) |
| Find and stop the fixed-period (~15.5 s) external reset | **done — superseded: it was never a timer (B47); the resets were firmware faults, fixed in the rows below** | Samsung's debug_history (B35) shows every loop is a SOFT reset every ~15.5 s, whatever our code does; the reset-hold build proved UEFI never calls ResetSystem (B34). Shim now samples, stops (RMW, register layout from the 9820 kernel's s3c2410_wdt.c) and re-reads both SoC watchdogs, reporting each answer as a band (`notes/m8-uefi-image.md` B36) |
| Get the FD through SEC/PEI on the device | **done (device-validated 2026-10-10, boots #12-#14)** | SEC links the C BaseMemoryLib (DC ZVA faults with the MMU off, B48/B50); every MMIO window page-aligned so ArmConfigureMmu accepts the table (UFS_IOCOH 0x13C10700, B56). Proven by the on-panel log reaching DXE |
| Get DXE through the architectural drivers on the device | **done (device-validated, boots #15-#17)** | ArmGicV2Dxe instead of the GICv3 ArmGicDxe (B59), virtual timer (B60), UEFI kept on bank0 - high banks and RamManagerDxe out (B63). Proven: END_OF_DXE + GraphicsConsole 1080x2280 on the panel |
| **UEFI reaches BDS on the S10e** | **done (device-validated 2026-10-10, boot #17)** | `notes/m8-uefi-image.md` B65, photo `notes/img-boot17-bds.jpg`: BdsWait countdown, PlatformRecovery, then storage fails |
| Make UFS answer: query requests fail with OCS=4 (response UPIU size mismatch) | **done (device-validated 2026-10-10, boot #25): GPT valid, partitions enumerated, multi-block reads clean** | UfsControllerInit finishes, every query UPIU fails OCS=4, `Device Error`, no block device, so no `\EFI\BOOT\BOOTAA64.EFI` (B65). Start from the UTRD response-UPIU length/offset fields vs the 9820 HCI (hw-rev 6, P02) |
| Give Windows more than bank0 (~0.9 GiB) | **todo (after UFS)** | high banks are off for UEFI (B63); the 9810 record (Star2LtePkg.dsc:211-250) needs multi-region RAM for winload |
| Strip the boot-bring-up diagnostics before a real install | **todo** | shim DIAG strips/bar, FD checkpoints, DiagVectors, ResetHold, FB serial console, FVMAIN_COMPACT at 416 B free |

### M9 — Installer

| Task | State | Notes |
|------|-------|-------|
| Add the S10e to the stage catalog as `Guided`, not `Ready` | **done** | Stricter than the note said, which is the honest direction: `SM-G970F` is a recognised `SupportedTarget` with `Validated = false`, so the model check warns instead of passing, and per-device availability keeps `unlock`/`twrp`/`backup`/`partition`/`transfer`/`uefi`/`firstboot` at `NotImplemented` for `beyond0lte` while `image` is `Experimental` (PC-side, but it carries drivers that have never run on hardware). The 9810 catalog is untouched. `notes/m9-installer.md` |
| Installer tests still green | **done** | `dotnet build` 0 warnings/0 errors; `dotnet test` 188 passed, 0 failed (187 before, +1 new `RecognisesTheS10eButKeepsItUnvalidated`) |
| Back up EFS/modem/partition table before any destructive step | todo | same as the 9810 flow — unreachable for the S10e until the phone stages stop being `NotImplemented`, which needs a validated boot first |

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
| Bring the drivers-branch D1 host stack (Beyond0LteUsbHostDxe + XhciDxe chain, DSDT XHC0, MAX77705 VBUS, fuel-gauge PoC) onto main | **done** | imported from `origin/drivers` 419fc03; FILE_GUID clash with BootLogDxe (…548) moved to …552; CLANGPDB build green — `notes/m16-usb.md` |
| Self-healing guard + feature flag around the USB bring-up | **done** | progress 0x67 (skip after a hang), `BEYOND0LTE_USB_HOST`, and XHC0._STA gated on a UEFI-written `USBH` word (0x8FE0F008, cleared in SEC) — `notes/m16-usb.md` |
| First device run: UEFI PHY/DWC3/CC/VBUS log, dock plugged before power-on | todo | `notes/m16-usb.md` |
| Windows binds inbox USBXHCI to ACPI\PNP0D10; hub + keyboard + mouse enumerate | todo | `notes/m16-usb.md` |
| Re-point the CMU clock/QCH windows before flipping PRTCAP | **done** | resolved from Samsung's cal-if, not guessed: `CMU_PERIC0 0x10400000+0x8000`, `CMU_FSYS0A 0x10a00000+0x8000`, gate offsets `2094/2098/2054/2008`, QCH `3038` — `notes/s10e-live-reads.md` §7.5. The four 0x1BC…–0x1BF… banks the DT lists are the **media IP** CMUs (MIF..MIF3), which is why the old note read as unresolved |

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
| Find existing Windows drivers for Samsung Exynos WLAN | **reopened 2026-10-11 — the old "dead end" was wrong about the bus** | §6 proved only the *platform glue node* has no `reg`. The chip is a **BCM4375 on PCIe root port `pcie_0@0x13ED0000`** (Gen1 x1, INTx, expected `14e4:4475`) — `notes/m12-wifi-chip.md`. No Windows driver exists for it on any arch, so one must be written |
| Milestone 1: UEFI PCIe link-up (`Beyond0LtePcieDxe`, flag + 0x68 guard + 'WLAN' word) | **done** | device-validated: endpoint 14e4:4475 (`notes/m12-wifi-m1.md`) |
| Milestone 1: DSDT `Device (WLAN)` `SAMS9825`, `_STA` gated on 'WLAN' | **done** | Windows binds `ACPI\SAMS9825\0`; driver starts, no PnP problem |
| Milestone 1: Windows probe driver reads VID/DID/BARs/chipid | **done** | `RESULT 0: BCM4375 alive` on the device |
| Milestone 2: BCM4375 firmware download + ARM start from the Windows probe (no NDIS, no DMA) | **done** | device-validated 2026-10-12: fw 18.41.117 runs, pcie_shared v7 after 180 ms (`notes/m12-wifi-m2.md`) |
| Milestone 3: msgbuf common rings + bus mastering (polled) + first firmware ioctl (`cur_etheraddr`, `ver`) | **done** | device-validated 2026-10-12 (boot #93, driver 1.0.3.0): both GET_VARs CMPLT status 0, MAC A8:DB:03:06:3C:EF, `ver` 18.41.117 FWID 01-1b4772fd (`notes/m12-wifi-m3.md`) |
| Milestone 4: CLM blob + events + UP + escan (firmware still torn down after PrepareHardware) | **done** | device-validated 2026-10-12 (driver 1.0.4.3): clmload 29232 B, escan v2, 8 BSS on 2.4 + 5 GHz in 1.7 s (`notes/m12-wifi-m4.md`) |
| Milestone 5: firmware resident for the device lifetime (DMA + rings in device context, ReleaseHardware teardown, interrupt or poll thread) | **done** | device-validated 2026-10-12 (1.0.5.2): resident fw rescans at t=30 s (SCAN 0, 10 BSS), 100 ms poll, common-buffer cache maintenance required (cached buffer, non-snooping device writes) - `notes/m12-wifi-m5.md` |
| Milestone 6: WPA2-PSK join (SET_SSID + 4-way handshake) | doing | run m6-3 with the user's network: firmware refuses `sup_wpa` (-23 BCME_UNSUPPORTED) = **no in-dongle supplicant** in this build; the 4-way handshake must run on the host over EAPOL frames, so it needs the M7 data path first. PBKDF2 PMK (self-test PASS) is reused - `notes/m12-wifi-m6.md` |
| Milestone 7: msgbuf data path (tx flowring + rx completion) + host WPA2 4-way handshake (EAPOL) | doing | associates (1.0.7.1-1.0.7.3 on device) but the fw never completes an rx buffer (rx cpl w 0, deauth 15); 1.0.7.4 (`wifi-m7c`, built, not device-tested): 511 rx buffers of rxbufpost_sz posted before the first UP, fmac-style refill, counters/mailbox/ring_mem diagnostics at join end - `notes/m12-wifi-m7.md` |
| Milestone 8: NDIS 802.3 adapter (Windows sees a wired NIC) + `wifi-connect` app that writes Ssid/Passphrase and restarts the driver (disable/enable) | todo | user's choice 2026-10-12: do this first (also gives a network for Windows debugging tools) |
| Milestone 9: native Windows WiFi (WDI / WiFiCx: adapter in the network flyout, scan list, connect from the Windows UI) | doing | user's choice 2026-10-12 (after the 1.0.7.4 join): WiFiCx 1.0 + NetAdapterCx 2.2 client `drivers/Exynos9820WlanCx`, plan W1..W5 in `notes/m12-wifi-wificx.md` |
| M12 milestone 9 T1: warm restart - D11 cores into reset before the firmware image write (fmac cr4_set_passive) | done (device-validated wifi-cx3 install over the running KMDF driver: 0 mismatches) | built (shared WlanDl.c, both drivers; Parameters\D11Reset = 0 turns it off); does it cure "DL 6 TCM read-back mismatch" after a device restart? needs a device run (UNVERIFIED) |
| M12 milestone 9 W1: WiFiCx skeleton - adapter visible in Windows, MAC from the firmware, radio/reset/config commands, no traffic | done (device-validated wifi-cx3, boot #113: interface "WiFi" in netsh) | built + test-signed (`s10e-artifacts\wifi-cx1`, driver 1.0.9.1), not device-tested |
| M12 milestone 9 W2: scan - WDI_TASK_SCAN -> escan -> BSS list in the Windows network flyout | done (device-validated wifi-cx3: 10-14 BSS, networks in the flyout) | built (CxScan.cpp), not device-tested; broadcast scan only (no hidden SSIDs) |
| M12 milestone 9 W3: data path - NetAdapter tx/rx queues over the msgbuf flowring / rx completions | doing | built (CxData.cpp + CxAdapter.cpp queues, 64-slot copy FIFOs), not device-tested; NetAdapter frame format for WiFiCx UNVERIFIED (Ethernet assumed, 802.11 tx converted, first layouts logged) |
| M12 milestone 9 W4: connect - WDI_TASK_CONNECT (firmware join, OS supplicant over the data path), cipher keys, disconnect | doing | cx3: association result TLV failed (no PHY type list) -> fixed in cx4; cx4 on device: association result OK, EAPOL msg 1 delivered as Ethernet II but the OS never answered -> cx5 delivers 802.11 data frames (`s10e-artifacts\wifi-cx5`), not device-tested |
| M12 milestone 9 W5: power, roaming, interrupt instead of polling, flowring delete on disconnect | todo | |
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
| Windows battery indicator: KMDF battc miniclass `drivers/S10eBattery` on the MAX77705 gauge + DSDT `BAT0` (`SAMS9826`) | **done** | device-validated 2026-10-12 (driver 1.0.0.3): the user saw the icon follow plug/unplug, refresh takes a few seconds (20 s poll); `notes/battery-driver-m1.md`. Charging after a late plug-in with OTG boost on stays in `parkinglot/usb-host-hotplug-and-power.md` |
| Assume battery exhaustion is normal and make it survivable | **done — design rule** | a donated phone-desktop must degrade gracefully and be restartable by a non-technical recipient |
| Measure thermals under a desktop workload (not destructive, but needs the device) | todo | **needs-phone** — Samsung's own thermal stack (`devfreq_*`, `sec_thermistor@*`) has no Windows equivalent |

### M15 — The donation build

| Task | State | Notes |
|------|-------|-------|
| Define the honest "working" bar | **done** | Windows boots on the S10e and Windows sees **disk, RAM, CPU cores** — the user's own words; that is what a donated unit may be described as |
| Keep the stretch goals out of the definition | **done** | WiFi, keyboard/mouse via dock, LAN, HDMI are stretch: "if we don't get there, we don't get there" — they never lower the bar above |
| Make the build serviceable: what to do when one part fails | todo | a donated device will fail in the field |
| Keep the 9810 and 9820 flows separate and labelled | todo | golden rule: no unvalidated `Ready` |

### Parking lot — off the boot path, do later

| Task | State | Notes |
|------|-------|-------|
| Build an automated restore/reflash path with **heimdall** for when the device breaks completely | **parking lot** | heimdall v2.1.0 (Grimler's fork) is the CLI equivalent of Odin: `detect` → `print-pit` (both read-only) → `flash --RECOVERY <file> --no-reboot`. Not installed on this machine. Two hazards already recorded in `notes/heimdall-flashing.md`: it has **no partition-size check** (PR #555 — silently overwrites neighbours; our 18 255 872 B into a 55 279 616 B slot is a 3× margin but must be checked against `print-pit`, whose `blockSizeOrOffset` field is ambiguous), and it is **untested on an S10e** (works on a 2021 device per issue #547, fails on an A14 5G per #548). The rollback file already exists locally: `ref-recovery.img` is a byte-exact snapshot of the recovery slot |
| TWRP sits ~70 s on its logo: it mounts the Windows NTFS `userdata` as `/data` (ntfs-3g, read-only after a hard reset) and scans the whole C: tree for its storage size; grows with the file count on C: | **parking lot** (user 2026-10-13: document for later) | `notes/twrp-slow-start.md` (dmesg + recovery.log evidence). Fix = a TWRP build without the `/data` auto-mount (or `/data` flagged so it is not mounted at start) - that writes the **recovery** partition, which the ground rules exclude (only `boot` is written), so it needs the user's explicit go-ahead and the `ref-recovery.img` rollback |

### M16 — GPU acceleration (Mali-G76 via Panfrost)

Opened 2026-10-11 on two inputs: the user's find of the **Panfrost** project
(`https://gitlab.freedesktop.org/panfrost` — Mesa's open Mali driver, kernel DRM driver, reverse-engineered
ISA/hardware docs), and ntdev's report of GPU-accelerated DWM on Exynos (unreleased, unverified). The old
verdict (`notes/d5-gpu-display.md`) said no Mali WDDM exists; that is still true, but it is a driver to
**write/port**, not a dead end. Architecture to verify: a WDDM kernel-mode driver (DXGK) ported from the
Panfrost/kbase kernel side (job submission, GPU MMU, power) + a user-mode driver built from Mesa's Panfrost
gallium driver behind Mesa's Windows D3D UMD frontend.

Correction 2026-10-12 (`notes/gpu-panfrost-repos.md`): current Mesa has **no D3D UMD frontend** (Windows
frontend = WGL/OpenGL only), so the Mesa path gives GL/Vulkan apps, not DWM; the G76 is **"Norr"/tNOx**
(Gondul = G52); Bifrost job-manager GPUs need **no GPU firmware**. Register offsets below come from
`drm/panfrost` `panfrost_regs.h` and are generic Mali; every **9820-specific** value (G3D PMU/CMU offsets,
GPU_ID, core masks, coherency) is **UNVERIFIED** until the named device read. GPU MMIO base `0x18500000`
is DT-only today (not yet in `Exynos9820.h`). Ladder: UEFI first (shortest debug loop, no WDDM), KMD after.

Restructure 2026-10-12 (`notes/gpu-wddm-mali-g52.md`, `notes/gpu-windows-prior-art.md`): the 2026-10-12
correction above is itself superseded for the UMD - Mesa **26.2.x still has the d3d10umd frontend** (removed
from main 2026-09-30), and **gahingwoo/wddm-mali-g52** (GPL-2.0, cloned to `C:\Users\ilya\code\mali\`) runs
d3d10umd + panfrost as the hardware UMD of a WDDM 1.3 KMD on an RK3576 Mali-G52 ("G52 r1",
`PAN_PROD_ID(7,4,2)`; our G76 is `PAN_PROD_ID(7,2,1)`, same Bifrost v7 JM). G9-G13 below reuse its components
(named per row; retarget table R1-R19 in the notes). `EXYNOS_G3D_BASE` is now in `Exynos9820.h` (SOURCE-ONLY).
Hard prerequisite for G9 onwards: UEFI must hand Windows a **powered, clocked G3D** - wddm-mali-g52 never
touches power or clocks (its firmware does), and on the 9820 that is pmucal MMIO + a DTZPC SMC + an ACPM DVFS
request (`notes/gpu-m16-g2g3.md` F2-F6), i.e. G3's `BEYOND0LTE_GPU_POWER_ON` stub made real.

| Task | State | Notes |
|------|-------|-------|
| Inventory the Panfrost repos: what supports Mali-G76 / Bifrost v7 (Mesa gallium, kernel `drm/panfrost`, docs, test tools) | **done** | `notes/gpu-panfrost-repos.md`: all 22 group projects cloned to `C:\Users\ilya\code\mali`; G76 = "Norr"/tNOx, `GPU_MODEL(g76, 0x7001)` exists in drm/panfrost but no mainline SoC or Mesa CI job uses it (G52, same arch 7.2, is the proxy) |
| Inventory gahingwoo/wddm-mali-g52 (KMD layers, DDIs, memory/submission model, Mesa patches, shim, build, licence) and list every RK3576/G52 -> Exynos9820/G76 change | **done** | `notes/gpu-wddm-mali-g52.md` (commits 3e73db6 + review fix 5a66a06): retarget table R1-R19; biggest blocker = UEFI must power+clock G3D (SMC + ACPM) before Windows; patches 0001-0019 apply to our Mesa 26.2.4 clone |
| G1. Device facts, read-only from Android/TWRP: `/proc/device-tree` mali node name + `interrupt-names` + `samsung,power-domain` target, `pd_embedded_g3d` node name; LineageOS 9820 kernel: `grep request_firmware drivers/gpu/arm/b_r10p0` | todo | no MMIO, no flashing. Validates: `mali@18500000` present → add `EXYNOS9820_G3D_BASE` to `Exynos9820.h` (rule 6) in the same commit; confirms "no G76 firmware" (UNVERIFIED for Samsung's kbase). **Stock-ROM pre-read (`notes/gpu-odin-rom.md`):** node `mali@18500000` reg 0x5000, `interrupt-names` JOB/MMU/GPU = SPI 288/287/289, power-domain -> `pd-embedded_g3d@15862100`; stock Android 12 kbase is **`bv_r32p1` (`r32p1-01bet2`), not `b_r10p0`**, JM backend, no GPU firmware in kernel/vendor/system - live read still required |
| G2. G3D power-domain + clock recipe from source: PMU G3D CONFIGURATION/STATUS offsets (`pd_embedded_g3d`, cal-if pmucal), CMU_G3D gate/mux, whether ACPM must be asked (vs direct PMU write), regulator (buck for G3D) | **done** | `notes/gpu-m16-g2g3.md` F1-F6 (source only, LineageOS lineage-22.2): status = PMU+0x2104 bit0 (kbase DT `gpu_pmu_status_reg_offset`), on-seq = PLL_G3D (CMU_CMU+0x140/0x148) + PMU 0x2380/0x2384 + 0x2120 atomic + 0x2100/0x2104, then **SMC** DTZPC restore (`need_smc 0x18410204`); clock+voltage via **ACPM** DVFS id 10 on IPC ch 5 (no direct regulator); F7: same PMU/pmucal/ACPM path in the shipped bv_r32p1 `gpexbe` glue, plus an optional `SMC_DRM_G3D_POWER_ON`. ⇒ UEFI power-on needs SMC+ACPM — G3 stays read-only; whether sboot leaves G3D on is G3's first log line; stock-ROM facts in `notes/gpu-odin-rom.md` (kbase bv_r32p1, pd-embedded_g3d PMU+0x2100/0x2104, need_smc 0x18410204) |
| G3. UEFI `Beyond0LteGpuProbeDxe` (feature-flagged, self-healing guard like USB 0x67): power G3D per G2, then read GPU_ID, L2/SHADER/TILER/AS/JS_PRESENT, MMU_FEATURES, COHERENCY_FEATURES into the PRAM log | **doing** (built; NOT booted - needs-boot) | `notes/gpu-m16-g2g3.md` "G3 design/build": READ-ONLY (power-on = `BEYOND0LTE_GPU_POWER_ON` UNVERIFIED stub, needs SMC+ACPM per G2); guard 0x6A; PMU 0x2104+0x2384 gate → CMU_G3D → GPU_ID canary → ID regs; CMU_TOP/CMU_G3D/G3D added to mMmioRegions, PMU/CMU_G3D/G3D constants SOURCE-ONLY in `Exynos9820.h`. Build exit 0, FVMAIN_COMPACT 2080 B free (after review fix). Original gate: **first GPU MMIO touch** — only after G2 proves the domain-on status bit, else bus hang. Device validation: log shows `GPU_ID[31:16]` = `0x72?1` (pan_model `PAN_PROD_ID(7,2,1)`), popcount(SHADER_PRESENT) closes MP11-vs-MP12, AS/JS counts; Windows still boots |
| G4. UEFI: soft reset (`GPU_CMD=1`, poll RAWSTAT bit 8) + G76 quirks (SHADER_CONFIG TLS_HASH, JM_CONFIG IDVS 0xF) + L2 → shader → tiler power-on with *_READY polls (`panfrost_gpu.c:62-104,117-174,452-481`) | todo | validation: logged READY bitmaps == PRESENT (first core group only if `L2_PRESENT != 1`), no GPU_FAULT, boot unaffected |
| G5. UEFI: AS0 with an AArch64-4K page table mapping one 2 MiB scratch buffer (TRANSCFG ADRMODE 6, MEMATTR from MAIR, `AS_COMMAND_UPDATE`, `panfrost_mmu.c:167-243`) | todo | validation: `AS_STATUS` not stuck ACTIVE, `MMU_INT_RAWSTAT` 0, readback of TRANSTAB; coherency mode chosen from G3's COHERENCY_FEATURES (UNVERIFIED) |
| G6. UEFI: first job — a hand-built **Write Value** job (genxml `v7.xml:1334-1399`, type 2, Immediate32 `0xC0FFEE00`) on **JS1**, chain of one, header index 1 | todo | the "GPU executes" gate, zero shaders. Validation: `JOB_INT_RAWSTAT` done bit 1, no error bit 17, value present in the scratch buffer, `JS_STATUS` = DONE 0x01 (`panfrost_device.h:260`; same code expected in the job header exception status — UNVERIFIED); chain pre-checked on the PC with pandecode |
| G7. UEFI: **Fragment** job clearing a small render target (hand-built framebuffer descriptor + RT clear colour, `v7.xml` Framebuffer/RT structs) then CPU-blit to the GOP fb | todo | first visible GPU output — validation: photo of a solid rectangle in the expected colour; tiler heap/context requirements for a fragment-only job UNVERIFIED |
| G8. UEFI: **Compute** job running a Bifrost shader compiled on the PC by Mesa's `compiler/bifrost/cmdline.c` (`G76` target) — e.g. fill a buffer with `gid*4` | todo | validation: buffer contents match; proves shader descriptors + compiler output on real G76 (first use of the Mesa compiler) |
| G9. ACPI + hand-off: `Device (GPU0)` in the DSDT - `Memory32Fixed` 0x18500000/0x5000 **first** in `_CRS`, then 3 `Interrupt` {320} job, {319} mmu, {321} gpu (any order; Level/ActiveHigh by our DSDT convention, trigger UNVERIFIED - the DT flags cell is 0) - **and** UEFI leaves G3D powered at a fixed OPP at ExitBootServices (pmucal `embedded_g3d_on` + DTZPC SMC + ACPM `cal_dfs_set_rate(0xb04000a)` at **260 MHz**, Samsung's start clock) | **doing** (UEFI half + ACPI half: code + build done, NOT booted - needs-boot) | ACPI half: `Device (GPU0)` `_HID` **SAMS9828**, `_CRS` = 0x18500000/0x5000 first, PMU page 0x15862000/0x1000, IRQ 320/319/321 Level/ActiveHigh (UNVERIFIED), `_CCA` 0; `_STA` = 0x0F only when progress word [4] 0x8FE0F010 = `GPU0` (0x30555047), written by `Beyond0LteGpuProbeDxe` when its last probe saw both PMU bits on, the GPU gate not off and GPU_ID = G76 (SEC clears it) - so with the flag 0 and G3D off the device is hidden (`notes/gpu-m16-g9b.md`); build exit 0. UEFI half: `Beyond0LteGpuProbeDxe` `GpuPower.c` (pmucal on-seq + DTZPC SMC 0x82000410(1, 0x18410204, 2), refused -> power back off) + `GpuAcpm.c` (FREQ_REQ {10, 260000 kHz} on ACPM ch **0** = the VERIFIED clock-controller channel, m15 - not ch 5 as G2 says) behind `BEYOND0LTE_GPU_POWER_ON` (committed **0**; only if PMU+0x2104 bit0 = 0 at entry, the vendor test; failure path restores the entry state; guard 0x6A) - `notes/gpu-m16-g3don.md`; builds flag 0 and 1 exit 0. Then: reuses nothing executable: wddm-mali-g52 expects the firmware to do this (`PLAN.md:70-72,117`, edk2-rk3576 is RK-only); its drivers take the first memory resource and up to 3 IRQs (`maliwddm/adapter.c:106-126`, `malikm/device.c:43-59`) - notes R1-R3, R13. New `_HID` = naming decision. No thermal path under Windows: stay at 260 MHz until one exists. Validation: UEFILOG shows PMU+0x2104 bit0 = 1 and +0x2384 bit0 = 1 at ExitBootServices; Device Manager shows the ACPI device with its resources (no driver yet); BasicDisplay still drives the panel; 3 boots |
| G9b. KMDF write-value under Windows: `m1probe` retargeted - PMU check rewritten for PMU+0x2104/+0x2384 bit0 = 1 (R4), GPU_ID gate `((id>>16) & 0xFF0F) == 0x7201` (R5), `GPU_SHADER_CONFIG` 0xF04 `SC_TLS_HASH_ENABLE` BIT(17) (R6), Mali-LPAE AS0 + one WRITE_VALUE job (R17) | **doing - device RESULT: PASS 2026-10-12 (boot #106; c0ffee42 written, no MMU fault, JOB irq on GSIV 320); "boots 3x" still to count** | **`drivers/MaliG76Probe`** (binds `ACPI\SAMS9828`; checks the `_CRS` contract; PMU page from `_CRS`; panfrost G76 quirks SHADER_CONFIG/TILER_CONFIG/JM_CONFIG; one core group only; IRQs 320/319/321 connected and recorded, completion polled; results in `C:\Windows\System32\LogFiles\MaliG76Probe-run.txt`, last line `RESULT: PASS|FAIL|SKIPPED`), package `s10e-artifacts\mali-g9b` (`notes/gpu-m16-g9b.md`, sha256 there). Validation now: `RESULT: PASS` in the run log, no MMU fault, Windows boots 3x. Original plan: reuses **`drivers/m1probe`** (KMDF 1.33, 280 lines, GPL-2.0 -> own subtree + NOTICE like `drivers/S6SY761Touch/`). Also validates on the G76 the **Mali-LPAE** format (TRANSCFG 0, MEMATTR 0x888d88) that the reused KMD uses, vs G5's AArch64-4K (R10). needs-windows to build (WDK 10.0.26100, plain vcxproj). Validation: `Result` = 1 in the device's registry key, 0xC0FFEE42 at the target, no MMU fault, Windows boots 3x |
| G10. **Full** WDDM 1.3 KMD (display + render on one adapter, not render-only) whose display half keeps the **GOP framebuffer** 0xCA000000 1080x2280 (`DxgkCbAcquirePostDisplayOwnership`, one mode, CPU-blit presents), GPU optional at start, UMD = Mesa 26.2 d3d10umd **softpipe** | **doing** (code + build + test-signed package `s10e-artifacts\mali-g10` done (sha256 in the notes), NOT booted - needs-boot; ESP procedure `drivers/MaliG76Wddm/esp/extra-gpu-g10.cmd`, INF UMD = maliumd.dll (G12). Rescue path built first: flag-0 image always hides GPU0, ESP `\S10EDIAG\nogpu.flg` hides it without a reflash. KMD = **`drivers/MaliG76Wddm`**, default mode display-only (KMDOD) because the full adapter without a real UMD went black on installed Windows (their run 6); full adapter behind `Parameters\FullAdapter`. Rotation: identity only - a regression vs Basic Display (Bixby rotate) until a follow-up; `notes/gpu-m16-g10.md`; 2026-10-12 device: display-only WORKS (3 boots), FullAdapter 1 / GpuEnable 1 black + no return; 2026-10-13 hang work (`notes/gpu-full-adapter-hang.md`): PRAM DDI trace at WINPRAM+0x4000, software vsync (ControlInterrupt promised CRTC_VSYNC and never delivered it), flips as CPU blt + presents no longer skipped with the GPU half off, EnablePreemption 0 before the first full boot - package `s10e-artifacts\mali-g13` (1.0.13.1), NOT booted) | reuses **`drivers/maliwddm`** (adapter.c, display.c, memory.c, command.c; WDDM 1.3 caps and DDI set that dxgkrnl accepted on 22621) + its registry DDI trace; edits: child type HDMI -> internal (R14), GOP format check (R15), INF `_HID`/strings (R3, R16). Render-only was refused on an ACPI GPU (`maliwddm.h:24-34`). **First** build the rescue path: their installed-Windows full adapter went black with a WARP UMD (`results/m5.2-board-run6`), so a way to disable the service offline (TWRP-mounted hive or WinRE) must exist before install (R19). Validation: adapter Started, desktop visible through it, `Presents` growing, 3 boots without bugcheck |
| G11. KMD GPU half: `maligpu` power-up/MMU/job slots behind maliwddm - one aperture segment, fixed per-allocation Mali VA mapped on MAP_APERTURE_SEGMENT, job chains via Render/SubmitCommand, DMA_COMPLETED on the job IRQ, TDR = re-bringup | **doing - device RESULT: PASS 2026-10-14 (run h1a, boot #115: m5test WRITE_VALUE through WDDM, jobs done 1, irqs 1, no fault; notes/gpu-hw-path.md H1); more boots to count** (code + build done in `drivers/MaliG76Wddm` behind `Parameters\GpuEnable` = 1 (default 0); m5test ported (`drivers/MaliG76Wddm/tools/m5test`, G76 GPU_ID gate) and packaged in `s10e-artifacts\mali-g10`, run by the ESP script when GpuEnable is live; `notes/gpu-m16-g10.md`) | reuses **`drivers/common/maligpu.{c,h}`** (with the R4-R6 edits), maliwddm `memory.c`/`command.c`, `include/maliwddm_abi.h`, and the D3DKMT acceptance test **`tools/windows/m5test.c`** (their M5.2 step b). Check `Irqs` != 0 before trusting interrupts (their 1 ms poll stays as a fallback, `command.c:406-435`). Validation: m5test PASS (write-value through WDDM), STATS 0 faults, a deliberately bad job recovers via TDR |
| G12. UMD: **pin Mesa 26.2** (local clone 26.2.4) **d3d10umd + panfrost** with wddm-mali-g52 `mesa-patches/0001-0019` + `win32-compat/` (malikm_drm.c libdrm shim, maliwddm backend) - CI-style build first (`MALIKM_FAKE`, no device), then the triangle on the S10e as a software device through the shim (their c1), then as the adapter's INF `UserModeDriverName` hardware device on the runtime callbacks (their c2) | **doing** (built, NOT run - needs-boot) | **Built 2026-10-12** (`notes/gpu-m16-g12-mesa.md`, script `tools/mesa-build/build-mesa-g76.ps1` + `wsl-libpan.sh`): working copy `mali\mesa-26.2-g76` (26.2.4 + 0001-0019 LF + win32-compat, all apply), libpan SPIR-V from WSL (user-space conda-forge LLVM 18), **`panfrost_compile` built for Windows x64** (shader blobs embed raw `pan_shader_info`: GCC layout differs, x64/ARM64 MSVC layouts proven identical), ARM64 via clang-cl 23 `--target=aarch64-pc-windows-msvc` + MSVC link; 1037/1037, 0 errors; `libgallium_d3d10.dll` = `maliumd.dll` (the reference INF's `UserModeDriverName`) AA64, imports VCRUNTIME140 only (+ `vc_redist.arm64.exe`), in `s10e-artifacts\mali-g12` with SHA256SUMS. Still G52-named: shim device `\\.\MaliG52`, `MALIKM_FAKE` table (0x7402). Plan: reuses **`mesa-patches/`** (all 19 apply to 26.2.4 after CRLF->LF, notes), **`win32-compat/`**, `ci/clc-stubs` + Linux-precompiled libpan SPIR-V, `tests/d3d11-triangle`; build = meson/ninja + clang-cl ARM64 (`.github/workflows/mesa.yml`). G76 needs **no Mesa change** (`pan_model.c:80` row, bifrost quirks 0); refresh the FAKE GET_PARAM table from the G3 log. Validation: triangle centre/corner pixels match softpipe on the S10e |
| G13. DWM on the Mali: panfrost made the default when d3d10umd is bound to the maliwddm runtime (today `GALLIUM_DRIVER=panfrost` is per process, so DWM would get softpipe), the hardware present path (their step d: patch 0019 `pfnPresentCb` -> KMD CPU blt, never validated upstream), then DWM compositing with Transparency on | **doing - DWM composes on Panfrost (runs h3g+); H4b zero-copy scan-out (firmware #116, KMD 1.0.36.1 re-stride to 4352 B, Mesa 0053 GPU blit present) validated in run h3m 2026-10-14: DWM 41 presents/s at logon, 21/s under a 21 paints/s GDI load (was ~6.5 with the CPU readback), 0 GPU faults; windowed D3D presents reach the panel through a GDI hand-off (Mesa 0061/0063: ~30 fps for a 540x1140 window), Explorer multi-device rendering fixed (0060), scan-out dumps show desktop + cube correct (run h3x), 7-minute 3D soak clean (run s2: 0 faults, 0 resets) (notes/gpu-hw-path.md)** | reuses mesa-patch **0019** + `malikm_present_d3dddi`; new work beyond the prior art: panfrost-by-default, flips/`SetVidPnSourceAddress` instead of two CPU copies per frame, WaitForIdle-per-BO-wait removal. No public code runs DWM on a Mali; NTDEV's S9+ DWM photos do not say how (`notes/gpu-windows-prior-art.md`). Validation: maliwddm STATS `Submits`/`JobsDone` grow with DWM activity, desktop correct, CPU time vs WARP measured. **Guided** only after device validation |
| G14. Panel brightness under Windows: DECON into command mode (sboot leaves video mode, DSIM then never sends commands), DSIM DCS packets, DXGK brightness interface v2 (AOR-only, sboot value = 100 %, Samsung 2-nit AOR = 0 %) in MaliG76Wddm `panel.c`; later: hardware keys, idle dim, light sensor | **done 2026-10-14** - KMD 1.0.42.1-1.0.43.1 over 5 boots: Windows restores its level at boot, WmiSetBrightness 100/30/75 reached the panel as AOR 0x0014/0x0835/0x042F; 1.0.43.1 fades in 5 % steps (0->100 20 steps 337 ms); the user drove the slider 30/100/75/80/10/90/0 and accepted it ("as good as done"). Open: idle dim never fires (parked), keys and light sensor later | `notes/brightness-research.md` D1-D10 + KMD 1.0.42.x. Validation: slider in Settings / quick settings changes the panel visibly, no display regression, 3 boots |

### M17 — Bluetooth, hardware video decode, WWAN (from the drivers branch)

Opened 2026-10-15 (goal: "get bluetooth, video accel working. Wwan is a stretch goal").

| Task | State | Notes |
|------|-------|-------|
| B1. Bluetooth (BCM4375 on uart@10840000): UEFI role latch (`Beyond0LteBtUartDxe`), DSDT BTH0 SAMS9829, `drivers/Exynos9820Bt` BthxDDI transport + patchram | **doing** - R1-R5 passed on device (radio On, HCI both ways, patch runs, LE scan finds advertisers from a Windows app). Open: pairing (needs a device in pairing mode), own BD address, RTS flow control, role latch in the default image, power management | `notes/bt-driver.md` |
| V1. MFC v13 firmware boot + SYS_INIT from a Windows KMDF driver, R2H interrupt (GSIV 370) | **done 2026-10-15** (vidc1: R2H 12 / SYS_INIT_RET 1 / ISR 1) | `notes/d10-mfc-main.md`, `drivers/Exynos9820Mfc` |
| V2. MFC plain-mode H.264 decode in the driver (OPEN, SEQ_HEADER, INIT_BUFFERS, NAL_START, LAST_FRAME, CLOSE) | **done 2026-10-15** (vidc4: generated I_PCM/P_Skip vector decoded bit-exactly, 3 frames, FNV hashes = libavcodec). Rules learned: every buffer above the RISC base (else ERR 10); never reset a running core (0x122) | `notes/d10-mfc-decode-flow.md`, `tools/mfc/mkh264pcm.py` |
| V3. User-mode interface: IOCTL decode of an arbitrary stream (`MfcIoctl.h`, `tools/mfc/mfcdec`) | **done 2026-10-15** (vidc6/vidc8: x264 640x360 baseline, 720p and 1080p High with B-frames, 120/120 frames bit-exact vs libavcodec; cached buffers + KeFlushIoBuffers: 1080p 77 fps from user mode) | `notes/d10-mfc-decode-flow.md`, `tools/mfc/mkclip.py` |
| V4. Media Foundation integration (an H.264 decoder MFT on the IOCTLs) so Windows apps use the hardware | todo | |
| W1. WWAN (Shannon modem): stretch | todo | drivers branch `D9-MODEM-DESIGN.md`, `notes/modem-wwan.md` |

## What is blocked on the user right now

| Needs | Milestones | Why |
|-------|-----------|-----|
| **needs-windows** | M7 | The Windows driver toolchain only runs on Windows; nothing here can compile these drivers |
| **needs-phone** | M10, M14 | Further live reads (USB function, thermals) — the phone is already connected, so these are cheap |
| **needs-dock** | M11, M13 | The owner has **both** a DisplayLink dock and a non-DisplayLink one; the non-DisplayLink path is preferred because it is cheaper |
| **one device only** | M10, M14 | There is **no spare unit yet**: nothing destructive may run on the S10e until Windows booting is established, and the powered-dock/host-mode risk is deliberately deferred to a later stage |
