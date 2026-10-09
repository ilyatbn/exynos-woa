# Handoff — the Windows session starts here

Read this first, then [`MILESTONES.md`](MILESTONES.md). Everything below is the
state of the tree at the time of writing; the plan of record is the milestone
file and it is updated and committed **per task**, never batched.

No branch has been pushed. All work so far is local commits.

**If you are the *next* session, jump to §11 first** — it is the newest restart
point (the UFS HC port is done and building). §10 is the previous crash point,
kept for the port plan; §9 is the older toolchain handoff, still valid for the
driver builds. Do not re-install VS or the WDK.

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

## 9. Where the Windows session stopped (2026-10-09 — restart point for the admin session)

Full detail is in `notes/windows-m7-toolchain.md` (�0��2). Short version:

**Toolchain � installed, and it works.**

| Item | State |
|---|---|
| WDK/SDK 10.0.26100 | `winget install --id Microsoft.WindowsWDK.10.0.26100 --accept-package-agreements --accept-source-agreements --force` ? `C:\Program Files (x86)\Windows Kits\10\{Include,Lib,Tools,bin,build}\10.0.26100.0`. All import libs the UFS script needs are present |
| VS 2026 Community | `winget install --id Microsoft.VisualStudio.Community` ? `C:\Program Files\Microsoft Visual Studio\18\Community` (major **18**, the path `build-ufs.ps1:21` and `Build-Touch.ps1:15` hardcode). Core workload only � no `VC\`, no `cl.exe` |
| VS components | `setup.exe modify --installPath "...\18\Community" --add � --quiet` **must be elevated**: non-elevated it exits **5007**. Run with `Start-Process -Verb RunAs` it returns **3010** (ok, reboot pending). Added: `VC.Tools.x86.x64`, `VC.Tools.ARM64`, `Component.Microsoft.Windows.DriverKit`, then `Windows11SDK.26100` (exit 0) |
| Why the SDK component matters | `build-ufs.ps1:104` force-includes `Include\10.0.26100.0\shared\warning.h`. The WDK MSI alone does **not** ship it; the Windows 11 SDK component does (include tree went 1597 ? 6365 files) |
| Compiler identity | `cl.exe` **19.51.36260** (MSVC v145), ARM64 cross at `VC\Tools\MSVC\14.51.36231\bin\Hostx64\arm64` |
| Registry | `HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots\KitsRoot10` = `C:\Program Files (x86)\Windows Kits\10\` � had to be added with elevated `reg.exe`; MSBuild's WDK toolset reads it (`Toolset.props:33`) |
| adb | device authorized: `RF8M21HYDSZ � model:SM_G970F` |
| git | needed `git config --global --add safe.directory C:/Users/ilya/code/exynos-woa` |

**What has been built.**

* `build-ufs.ps1 -SourceDir drivers\Exynos9810Ufs -OutDir <new> -WideUncached -DmaWindow` ? **EXACT_BUILD_OK**, 49152 bytes, sha256 `e5eb688500bae492101973601607fd0d12f877105a76aa4ddee8b031bb5c43a2`. It does **not** match the README's released hash `a80b2229�`. That check is **inconclusive, not a failure**: `release/provenance.json` shows this tree's `Exynos9810Ufs.c` is not byte-identical to the published one (`published_sha256 8c2adefe�` vs worktree `48a10631�`, git status clean), and the repo records **no compiler version** for the validated build. Do not read the mismatch as "the toolchain is wrong".
* `msbuild drivers\Exynos9820Hsi2c\Exynos9820Hsi2c.vcxproj /p:Configuration=Release /p:Platform=ARM64` ? reaches `ClCompile` (so the KMDF toolset resolves) and then fails:
  `internal.h(11,10): error C1083: Cannot open include file: 'ntddk.h'`.
  **The validated 9810 project fails identically**, so this is a toolchain-invocation problem, not a 9820 porting defect. `ntddk.h` is present at `Windows Kits\10\Include\10.0.26100.0\km\ntddk.h`; the cl command line only got `/I\.`. The WDK sets the include path in `WindowsDriver.KernelMode.props:28` as `$(CRT_IncludePath);$(KM_IncludePath);$(KIT_SHARED_IncludePath);$(KIT_SHARED_INC_PATH_WDK)` and those evaluate empty here.

**Do in the restarted (admin) session, in this order.**

1. Make the HSI2C include path resolve. Find where `CRT_IncludePath`/`KM_IncludePath`/`KIT_SHARED_IncludePath` are defined (`WindowsDriver.Shared.Props`, `WindowsDriver.OS.props`, `Microsoft.Cpp.WindowsSDK.props`) and why they are empty � most likely the WDK's VS integration wants a workspace/`VCTargetsPath` context that bare `msbuild` on a loose `.vcxproj` does not give. With admin rights the registry/props wiring may simply complete itself; re-run the same command first and compare.
2. Build `drivers\Exynos9820Ufs` with `build-ufs.ps1` (the script is proven to run; use a **new** output dir outside the tree � it refuses an existing one, `build-ufs.ps1:33`).
3. `Build-Touch.ps1` **cannot run as written in this tree**: it needs `drivers\S6SY761TouchGpio\RetainedV24` (+ `RetainedV24.sha256.json`), `windows-drivers\Exynos9810Hsi2c\Arm64BuildEvidence.ps1` and `tools\wdk-mirror`; none of the three exist here (`drivers\S6SY761Touch` is the 9810 package, there is no `windows-drivers\`). Decide: port the script to this tree's layout, or build the touch driver directly with `msbuild /p:Platform=ARM64` plus `infverif`/`inf2cat` from `Windows Kits\10\Tools\10.0.26100.0\x64` and `bin\10.0.26100.0\x86`.
4. Test-signing: still needs the project's test certificate � it is not in this tree; find it before signing, do not fabricate one.

**Unchanged hazards.** The `UNVERIFIED` constants in �5 of this file are still open; a successful build says nothing about them.

---

## 10. Where the mid-day session stopped (2026-10-09 — restart point, second crash)

The session crashed while porting the UEFI-side UFS host controller. Everything
committed so far is local on `main`; nothing pushed.

**Committed (newest first):**

| Commit | What |
|---|---|
| `9339b0c` | M8: high DRAM banks published in `PlatformMemoryMapLib.c` (0x880000000/0x900000000, 2 GiB each, vendor DT reads; map now offers 5.25 GiB). Rebuild clean: `FVMAIN_COMPACT` 2976 B free, FD 7 340 032 B sha256 `81b0fd9e...` — `notes/m8-build.md` §7 |
| `336cf3d` | M8: ACPI installer ported + rebuilt (`notes/m8-build.md` §6) |
| `a03aae5` | M9: S10e target behind honest gating, suite 188/0 |

**In flight when it died (verify with `git status` before redoing):**

* `firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteUfsHcDxe/Beyond0LteUfsHcDxe.inf` —
  WRITTEN (FILE_GUID `…555555555545`, entry `Beyond0LteUfsHcEntry`, packages the 9820 .dec).
* `Beyond0LteUfsHcDxe.c` — **NOT created yet.**
* DSC/FDF — **NOT wired** (the UfsHcDxe lines are still in both DROPPED blocks).
* No rebuild of the port; no notes beyond the inventory; task not committed.

**The full port plan is already written** in `notes/m8-ufs-hc.md` §2 (constant
inventory of the 9810 .c, classified) and §3–4 (gate map + ordered substitution/wrap
list with 9810 line numbers). Read that file first; do not redo the inventory.

**Do next, in this order (one commit per step):**

1. Create `Beyond0LteUfsHcDxe.c` from the plan in `notes/m8-ufs-hc.md` §4 — a
   mechanical pass (renames, macro remaps) then the `#if BEYOND0LTE_UFS_DIAG`
   wraps at the listed line numbers, bottom-up. Do NOT touch the 9810 file.
2. Wire DSC `[Components.common]` + FDF storage section; delete the UfsHcDxe
   entries from both DROPPED blocks and fix the stale "is DROPPED" comments
   (`dsc:496`, `fdf:205`).
3. Rebuild: `powershell -ExecutionPolicy Unrestricted -File firmware\tools\build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG`
   (this box is `LocalMachine=AllSigned` — bare `&`/`Invoke-Expression` of the script
   fails; the child-process `-ExecutionPolicy Unrestricted` outranks LocalMachine).
   Tee to `%TEMP%\m8-build-16.log`. `FVMAIN_COMPACT` is `NumBlocks = 0` (auto-size) —
   it grows; the FD stays 7 340 032 B.
4. Append the result to `notes/m8-ufs-hc.md`, move the MILESTONES line
   (`Port the UEFI-side UFS HC driver | done`) in the same commit, commit.

**Facts to trust, not re-derive:** build toolchain state (`notes/m8-build.md` §1),
FV numbers (§6/§7), the 9820 UFS windows are ALL VERIFIED reads
(`Exynos9820.h:268-278`), intra-window offsets carry unchanged
(`Exynos9820.h:260-262`), `0xFED1xxxx` and `0xCC000000` are proven unusable
(`notes/s10e-live-reads.md` §7.3). **Still open after everything:** the §5 hazards
of this file (fb base, PRAM choice, DSDT sub-device buffers, write-fence LBAs,
UFS hw-rev class) plus the new UNVERIFIED list in `notes/m8-ufs-hc.md` §4.

---

## 11. Where the session stands now (2026-10-09 — restart point, UFS HC + reset lib DONE)

The UEFI-side UFS host controller AND the reset library are ported, wired and
**building**. Nothing is validated on the S10e; the FD is a build product, not a
boot result.

**Committed (newest first):**

| Commit | What |
|---|---|
| `e64a4e6` | reset lib: gate the constructor's `Status` (clang `-Wunused-variable` with both channels off) |
| `3c84773` | reset lib wired: DSC `ResetSystemLib` instance + `ResetSystemRuntimeDxe` re-added in DSC and FDF; build clean, **`FVMAIN_COMPACT` 968 B free** |
| `ecc0d2a` | `Beyond0LteResetSystemLib` .c + .inf + `notes/m8-reset-lib.md` |
| `c664896` | notes: the harsh review of the diag-fix commit |
| `70e34ca` | this §11 |
| `78d6f97` | review fix: the `BEYOND0LTE_UFS_DIAG 1` branch was broken (deleted locals) |
| `1cf61fa` | the five clang failures the UFS HC port exposed |
| `783bb5d` | UFS HC build result + MILESTONES line moved to done |
| `2764134` | UFS HC wired into DSC + FDF |
| `e411a81` | `Beyond0LteUfsHcDxe.c` created |

**Current build state:** `BEYOND0LTE_UEFI.fd` 7 340 032 B, `FVMAIN_COMPACT`
[99%Full] 695352 used, **968 B free**, `FVMAIN` [26%Full] 1108224 used.
Modules in the FV: `Beyond0LteUfsHcDxe` (GUID `…5545`, 55 808 B) and
`Beyond0LteResetSystemLib` (GUID `…5546`).

**Nothing is in flight.** `git status` is clean.

**Do next, in this order (one commit per step, chunked):**

1. **Space check.** `FVMAIN_COMPACT` has 968 B free. `NumBlocks = 0`
   (`fdf:86`) means it auto-grows in 0x1000 blocks, so the next module does not
   overflow — but the FD is fixed at 7 340 032 B, so growth must be re-verified
   against the flash/carveout before it is trusted. Record the number after every
   build, do not assume.
2. `Star2LteRamManagerDxe`: decide first whether it is needed at all — the high
   DRAM banks are now in the memory map (`9339b0c`), which is what the 9810
   RamManager existed to publish. Read `Star2LteRamManagerDxe.c` before porting.
3. The HSI40/GPIO-handoff subsystem — `notes/m8-libs.md` records the CMU bank→
   power-domain map as a **dead end** (cal-if ids are enum positions), so expect
   this to stay `NotImplemented`, not to be guessed.
4. The 9810's own `PlatformBootManagerLib`: this build already resolves
   `ArmPkg/Library/PlatformBootManagerLib`, so decide whether the 9810 platform
   one adds anything before porting.

**Facts to trust, not re-derive:** everything in §10, `notes/m8-ufs-hc.md`
§5 (exhaustive literal sweep — supersedes the §2 line lists) and §7/§8, plus
`notes/m8-reset-lib.md` §5 (the wiring and the FV numbers). **Still open:** the
§5 hazards of this file, the UNVERIFIED lists in both notes files, and the fact
that the `BEYOND0LTE_UFS_DIAG 1` and `BEYOND0LTE_PMU_RESET 1` branches have never
been compiled (argued, not built).
