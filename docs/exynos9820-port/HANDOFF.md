# Handoff — the Windows session starts here

Read this first, then [`MILESTONES.md`](MILESTONES.md). Everything below is the
state of the tree at the time of writing; the plan of record is the milestone
file and it is updated and committed **per task**, never batched.

No branch has been pushed. All work so far is local commits.

---

## 1. What we are building, in one paragraph

The repo already runs Windows on a Galaxy S9+ (Exynos 9810, `star2lte`, SM-G965F)
and that whole flow is validated end to end. The job is to make the same thing
work on a **Galaxy S10e** (`beyond0lte`, SM-G970F, Exynos 9820). The bar for
"working" is deliberately narrow: **Windows boots, and Windows sees disk, RAM and
CPU cores.** WiFi, keyboard/mouse through a dock, LAN and an external HDMI
monitor are stretch goals on top of that, and they must never be used as an
excuse to lower the bar.

## 2. Ground rules (do not relax these)

1. Milestones and tasks are written in `MILESTONES.md` **before** the work, and
   the line moves `todo → doing → done` **in the same commit** as the work.
2. **One git commit per task.**
3. **Nothing is `Ready` unless it has been validated end to end on the new
   device.** Unvalidated stages stay `Guided` / `NotImplemented`
   (see `installer/src/S9Woa.Installer.Core/Stages/StageCatalog.cs`).
4. Uncertain constants are isolated and named **`UNVERIFIED`** — never guessed
   into a working-looking value.
5. A donated device must always be able to go back to Android. Never adopt a
   boot path that erases a partition (the `x1s`/`UH` pattern from Mu-Silicium is
   destructive — do not copy it).
6. Standardise on a **passive** dock. A powered hub plus USB host mode is the
   configuration the community warns "can irreversibly damage your device", and
   there is **only one S10e** — that test is deferred until Windows booting is
   established, and nothing destructive may run on it until then.
7. If something can only be decided on a real phone, say so and stop. If a step
   needs Windows, say so immediately rather than emulating it.

## 3. What already exists (all created in this session, none validated)

| Path | What it is |
|---|---|
| `drivers/Exynos9820Hsi2c/` | I2C bus driver (the touch bus). Copy of the validated 9810 package, names retargeted |
| `drivers/Exynos9820Ufs/` | storage driver — includes `build-ufs.ps1` and the `UfsDiag.exe` helper |
| `drivers/S6SY761TouchBeyond0/` | touchscreen driver, plus its host gesture tests |
| `firmware/Silicon/Exynos9820Pkg/` | `Exynos9820Pkg.dec` + `Include/Platform/Exynos9820.h` (the single source of every MMIO address) |
| `firmware/Platform/Beyond0LtePkg/` | `Library/PlatformMemoryMapLib/` (memory map + ArmPlatformLib) and `AcpiTables/` (`Madt.aslc`, `Dsdt.asl`, `Beyond0LteAcpiHeader.h`) |

The 9810 packages are **byte-unchanged** — verify with `git diff --stat <base> --
drivers/Exynos9810Hsi2c drivers/Exynos9810Ufs drivers/S6SY761Touch firmware/Platform/Star2LtePkg`.

Evidence lives in `notes/dock-desktop.md` (§1–§18), `notes/exynos9820-research.md`,
`notes/s10e-live-reads.md`. Every claim there carries its source; facts are
separated from inference.

## 4. The task for the Windows session — M7, build and test-sign

There is **no Linux cross-build** for these drivers. This is the first step that
genuinely requires Windows, and it needs Visual Studio with the ARM64 build tools
plus the **10.0.26100** SDK/WDK.

Build each package into a **new output directory outside the repo** (all three
scripts refuse an output directory inside the tree):

```powershell
# 1. HSI2C bus driver — plain WDK MSBuild
msbuild drivers\Exynos9820Hsi2c\Exynos9820Hsi2c.vcxproj `
        /p:Configuration=Release /p:Platform=ARM64
#    package lands in ARM64\Release\Exynos9820Hsi2c

# 2. UFS storage driver — fixed cl/link command lines, never signs or installs
powershell drivers\Exynos9820Ufs\build-ufs.ps1 `
        -SourceDir drivers\Exynos9820Ufs -OutDir "$env:TEMP\ufs-9820"
#    pass -WdkRoot if the WDK is not in the default location

# 3. Touch driver — also runs the production C through its host mocks first
powershell drivers\S6SY761TouchBeyond0\Build-Touch.ps1 `
        -OutputDirectory "$env:TEMP\touch-9820"
#    uses tools\wdk-mirror when -WdkRoot is omitted
```

What to record afterwards, in one commit per package:

* the exact compiler/linker diagnostics and any `C_ASSERT` that fires — the UFS
  header is full of compile-time checks that exist precisely to catch a
  half-edited constant set, so a fired assert is evidence about the port, not
  noise;
* whether the `.inf` passes `infverif` (the touch script already runs it and
  writes `infverif.log`);
* the **unsigned** catalog/package hashes, so a later session can tell a rebuilt
  binary from the shipped one.

Then, still on Windows: test-sign the three `.sys` files with the project's test
certificate. The installer turns test signing on in the image it builds; nothing
here should change device trust without going through it.

## 5. Known blockers that are NOT build errors — decide these before flashing

These are recorded as `UNVERIFIED` in the source on purpose. A build will
succeed with them wrong, so they are a boot-time hazard, not a compile-time one.

| Unknown | Where it is | What would close it |
|---|---|---|
| **Framebuffer base** — the 9810's `0xCC000000` was never confirmed on an S10e; the header leaves it `0` so the map skips it | `Exynos9820.h` | read the sboot-lit carveout on the phone (the 9810 flow already has a tool for this) |
| **Which CMU bank feeds PERIC0 (touch bus) and FSYS0A (USB)** — the 9820's banks are `1A200000`, `1BC00000`–`1BF00000`, none is the 9810's `11000000` | `Exynos9820.h`, `exynos9820hsi2c.h`, DSDT | derive from Samsung's `cal-if` headers (`soc/samsung/cal-if/exynos9820/cmucal-{vclk,node,qch}.h`) — QCH ids there are enum positions, not addresses |
| **PRAM breadcrumb address** — `0xFED14000` is *inside* DRAM bank0 on this SoC, so the 9810's scratch-above-RAM assumption is false | `PlatformMemoryMapLib.c`, `ArmPlatformLib.c`, `Exynos9820Ufs.h` | pick a reserved carveout from the DT's `reserved-memory` node |
| **DSDT CPU sub-device buffers** — the 9810's `_MAT` encoding cannot describe 6+2 clusters (and CPU0/CPU1 look identical in the 9810 file too) | `Dsdt.asl` | re-derive from the ACPI Processor Sub-Device spec; the MADT already carries the correct topology |
| **Write-fence / protected-partition LBAs** — still the 9810's partition table | `Exynos9820Ufs.h` | read the S10e's own GPT (needs root on the phone, or the driver's own LBA-1 read on Windows) |
| **UFS hw-rev** — the S10e reports `UFS_VER_0006`; the 9810 code path handles only 0004/0005 | `Exynos9820Ufs.c` | real behavioural gap, must be ported before storage is trusted |

## 6. What still needs the phone (it is connected; reads are cheap and safe)

Read-only, non-destructive, and already proven to work: `/proc/partitions`,
`/proc/cpuinfo`, `/proc/device-tree/<node>/` **names** and symlink targets are
readable. **Property contents are SELinux-blocked** to an unprivileged shell —
that is the whole lesson, and the workaround is that node *names* carry the
address. `/sys/**` deeper than one level is also blocked.

Still worth reading on the device:

* MPIDR per core (the cluster order is now settled from the DT — `cpu@0000`–`cpu@0005`, `cpu@0100`–`cpu@0101` — but confirming it on the running kernel is cheap);
* the DRAM bank sizes (node names give bases; sizes need the vendor DT);
* thermals under load, and whether the port can be host **and** charging at once
  (the S10e has **one** USB-C connector, so the dock occupies the charging port —
  and DisplayLink's own docs state that charging while docked is impossible).

## 7. Order of work after M7

1. Finish M8: the rest of `Beyond0LtePkg` — `Star2LtePkg.dsc`/`.fdf` equivalents,
   `PlatformBootManagerLib`, `Star2LteResetSystemLib`, `SimpleFramebufferDxe`,
   `Star2LteUfsHcDxe` (the UEFI-side storage), the serial-port library, and the
   remaining ACPI tables (`Fadt`, `Gtdt`, `Pptt`, `Hsi40Devices`,
   `GpioHandoffDevices`).
2. Close the unknowns in §5 that a read can close, before any flashing.
3. M9: installer stages for `beyond0lte` behind the same honest gating — the
   device checks must not be relaxed.
4. Only then: the stretch phase (M10 USB host mode, M11 HDMI, M12 WiFi,
   M13 LAN, M14 power, M15 the donation build).

## 8. Sources that matter, so they are not re-searched

* Shipping 9820 device tree: `CruelKernel/samsung-exynos9820`, branch
  `cruel-HVJ5-v5.3`, `arch/arm64/boot/dts/exynos/exynos9820.dts` (decompiled DT —
  numeric phandles) plus `exynos9820-universal9820_common.dtsi` and
  `exynos9820-rmem_s5100_dual.dtsi`.
* Mainline bindings that describe the shapes: `Documentation/devicetree/bindings/usb/snps,dwc3.yaml`,
  `usb-drd.yaml` (the `dr_mode` enum: `host`, `peripheral`, `otg`) and
  `samsung,exynos-dwc3.yaml`.
* Mu-Silicium `Status.md` — USB Host Mode ❌ on `starlte`/`beyond1lte`/`x1s`,
  USB Power Delivery ❌ on every Exynos target; `x1s` needs the `UH` partition
  erased (**do not copy**).
* DisplayLink's own support article on phone-as-host: the phone "needs to become
  the USB host", and "Android devices cannot currently enable USB and charge at
  the same time".
* Prior art for a 9820 UEFI UFS library: `sonic011gamer/edk2-exynos` has
  `Exynos9820Pkg/Library/ExynosUfsLib` (`ufs.c`, `scsi.c`); Mu-Silicium has
  `S5E9820Pkg` but **no** `PlatformUfsLib` for it.
* GitHub's unauthenticated API is rate-limited from this network; use
  `raw.githubusercontent.com` or the HTML pages.
