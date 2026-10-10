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
| Produce a flashable UEFI boot image for the S10e | **doing — shim, repack tool and a beyond0lte packer committed and run end to end; nothing flashed or booted yet** | The platform builds (`BEYOND0LTE_UEFI.fd`, 7 340 032 B, present at `firmware/edk2/Build/Beyond0LtePkg/DEBUG_CLANGPDB/FV/`) but nothing turns an FD into an image sboot accepts. `firmware/tools/pack-uefi.ps1` is star2lte-only and calls `tools/repack-with-ref.py`, which — like `firmware/device-data/` and `tools/bootimg-pack.sh` — **does not exist in this checkout** (`Test-Path` false for all three), so even the 9810 pipeline is not reproducible here. DONE (a): `firmware/tools/beyond0lte-bootshim/bootshim.S` — the 9810-shaped constants retargeted or compiled out (paint `0xCA000000` at 1440×3040, temp SP `0x83000000` marked UNVERIFIED, ramoops compiled out behind `BEYOND0LTE_PRAM_DIAG` 0 because `0xFED14000` sits inside `debug_snapshot.log_bcm`/`log_llc`, watchdog cl0/cl2 bases VERIFIED at `0x10050000`/`0x10060000`), and a defect the 9810 shim had (a second `ldr x5, _uefi_base` after the copy, reading a literal the copy itself overwrote) fixed; DONE (b): `firmware/tools/repack-with-ref.py` (the tool `pack-uefi.ps1:121` calls but that never existed in this checkout) wraps `[shim][FD]` in the phone's own recovery container with `page_size` 2048 read from the ref and ramdisk/second/dt/tail kept byte-identical, output 18 253 824 B; still needed: (c) the still-unknown constants named `UNVERIFIED` in the source, each with the read that would close it: `UEFI_BASE` (`Beyond0LtePkg.fdf:68` says `0x80080000`, marked UNVERIFIED; the real sboot kernel-drop address is **not** closable by a cheap read — **CORRECTED 2026-10-10: it is closable, `notes/k4b-drop-address.md` K01-K03 derives `0x80118000` from `/proc/iomem` (`Kernel code 0x80198000-81717fff`, `Kernel data …-832cffff`) plus the `Image` header's `text_offset 0x80000` / `image_size 0x3138000`, and the arithmetic is exact**), the required Linux-header `image_size`, and the 9820's write-protected (RKP-analogue) zones **(d) K10 partial closure (2026-10-10, `notes/K10-rkp.md` T15-T18): the live 9820 DTB, read in a TWRP root shell, settles the mechanism half — `el2_code` `0x87000000+0x200000` and `el2_earlymem` `0x97E000000+0x200000` (both owner `el2,uh`) are sboot-added and now VERIFIED, `kaslr` is `0x80100000+0x100000` (owner `kernel-kaslr`), and Samsung's own `rkp.h` shows the kernel-text window is runtime-derived from `_text.._erodata`, so there is no 9820 constant to copy from 9810. Two carried numbers are now known WRONG: the 9810 ramoops `0xFED14000` sits inside live `log_bcm` (`0xFEB10000+0x400000`), and the UFS DMA window `0x90000000..0xBC800000` overruns the live `memory@80000000` bank, which ends at `0xBAB00000`**. **Hazard recorded from Team Win's own device page:** a stock S10e is protected by Android Verified Boot — flashing TWRP without first flashing a verity-disabled `vbmeta` image leaves the device unable to boot; this disk's `vbmeta` is LBA 83 584..83 599 (`notes/s10e-root-reads.md` §2). Ground rules 5/6 still forbid flashing anything until Windows booting is established. Full detail: `notes/m8-uefi-image.md` §16, `notes/m9-install-attempt.md` §15 |

| Re-decide the FD/UEFI run address against the live DTB | **done (as a decision, not a validation)** | The drop IS a read: sboot loaded this boot's `Image` at **`0x80118000`** and its kernel window is `0x80198000..0x832D0000` (`notes/k4b-drop-address.md` K01-K03, `/proc/iomem` + the `Image` header, arithmetic exact), which made the old `0x80080000` provably inside the loaded-image region and overlapping `sec_debug_magic` and the `kaslr` node — the shim's self-overwrite was CONFIRMED, not hypothetical. New base **`0x83300000`** (first address above the window, below `mm_debug 0x91200000`, inside bank0 which ends `0xBAB00000`, disjoint from src so the copy cannot hit itself) moved together in `.dsc` `PcdFdBaseAddress`/`PcdFvBaseAddress`, `.fdf` `FD_BASE_ADDRESS`, the shim's `UEFI_BASE` default and `pack-uefi-beyond0lte.ps1`'s `-UefiBase` default. Verified by running: build exit 0 with `0x83300000` propagated into `FVMAIN_COMPACT.inf` and the `PeilessSec` AutoGen; shim linked at `-Ttext=0x83300000` (`_head 0x83300000`, `_payload 0x83300120` = last byte of the 288 B image); packer run end to end against the pulled recovery ref → shim 288 B, kernel 7 340 320 B, output **18 255 872 B**. Still untested: whether sboot reuses the same drop base for a 7.6 MB image, and nothing has booted |
| Stop publishing sboot's reservations inside bank0 as Windows RAM | **todo** | `el2_code 0x87000000+0x200000`, `kaslr 0x80100000+0x100000`, `camera 0x99000000+0xE400000`, `secure_camera 0x96000000+0x2200000`, `mm_debug 0x91200000+0x100000` are all VERIFIED reservations now sitting inside the published bank0 entry — `notes/K10-rkp.md` T17/T20 |
| Re-pick the UFS DMA window inside the live bank0 | **todo** | the carried 9810 window `0x90000000..0xBC800000` is now known ILLEGAL (live bank0 ends `0xBAB00000`); the new window stays UNVERIFIED until a device validation and the `C_ASSERT` set must stay self-consistent — `notes/K10-rkp.md` T18 |

| Fix the framebuffer RAM/DEV overlap | **todo** | `EXYNOS_FB_BASE 0xCA000000` is published `MMAP_DEV` but sits inside the bank0 RAM gap `0x87200000..0xBAB00000`, so the map still has a cached-RAM and Device entry covering the same range — pre-existing, but the reservation carve made the gap explicit and this is the same class of hazard; the fix is to split the gap around the FB like the reservations — `notes/s10e-dtb-mmio.md` S04 |

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
