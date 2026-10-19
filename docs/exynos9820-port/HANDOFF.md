# Handoff — the Windows session starts here

Read this first, then [`MILESTONES.md`](MILESTONES.md). Everything below is the
state of the tree at the time of writing; the plan of record is the milestone
file and it is updated and committed **per task**, never batched.

No branch has been pushed. All work so far is local commits.

**If you are the *next* session, jump to [`/progress.md`](../../progress.md) first**
— it is the live task list by ID, one file per task in `tasks/`, with a done
indicator per task. The newest restart point in this file is **§22** (§18-§21 are history); §16 and §17
are history (the work they describe is committed and verified, do not redo it).
Do not re-install VS or the WDK.

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
| ~~**Framebuffer base**~~ — **CLOSED on-device**: `EXYNOS_FB_BASE = 0xCA000000`, read from the phone's own kernel command line (`/proc/device-tree/chosen/bootargs` → `s3cfb.bootloaderfb=0xca000000`). The 9810's `0xCC000000` was never right here | `Exynos9820.h`, `PlatformMemoryMapLib.c` | done (`54095ad`); no DT node reserves it, so the map publishes it as device memory; the confirmation is the panel drawing on first boot |
| **Which CMU bank feeds PERIC0 (touch bus) and FSYS0A (USB)** — the 9820's banks are `1A200000`, `1BC00000`–`1BF00000`, none is the 9810's `11000000` | `Exynos9820.h`, `exynos9820hsi2c.h`, DSDT | derive from Samsung's `cal-if` headers (`soc/samsung/cal-if/exynos9820/cmucal-{vclk,node,qch}.h`) — QCH ids there are enum positions, not addresses |
| **PRAM breadcrumb address** — `0xFED14000` is *inside* DRAM bank0 **and inside `debug_snapshot`**, so the 9810's scratch-above-RAM assumption is false. **Confirmed on-device as root** (`notes/s10e-root-reads.md` §4 lists every `debug_snapshot/*` reg) | `PlatformMemoryMapLib.c`, `ArmPlatformLib.c`, `Exynos9820Ufs.h` | **DECIDED, deliberately not enabled**: the unclaimed gap is `0xD9801000`…`0xF0000000` (after `s5100_msi`) and it is above the published bank0 stop `0xD0000000`. `BEYOND0LTE_PRAM_DIAG` stays `0` — diagnostics off for the first boot (`notes/vendor-dt-reserved.md`) |
| ~~**DSDT CPU sub-device buffers**~~ — **CLOSED**: `_MAT` is the MADT GICC entry with the matching `_UID`; buffers re-derived from `Madt.aslc:149-163` and verified in the built FFS (`notes/m8-dsdt-mat.md` §5) | `Dsdt.asl`, `Acpi63.h` | done; only Windows parsing them is unvalidated |
| ~~**Write-fence / protected-partition LBAs**~~ — **CLOSED and replaced**. The live GPT was captured in TWRP as root (`notes/s10e-root-reads.md` §2) and equals Samsung's `BEYOND0LTE_EUR_OPEN.pit`; the header's own geometry fields were decoded in `notes/s10e-root-reads.md` §8 (AltLBA **31 236 095**, LastUsableLBA **31 236 090** at +0x30) and the tail was measured in §9 (LBAs 31 234 812..31 236 095 all zero → **no backup GPT on the media**). In the driver's own 4096-byte LBAs: ESP (sda28 CACHE) = **1 983 488 .. 2 137 087**, Windows volume (sda31 USERDATA) = **2 151 168 .. 31 234 815**, the refused run inside the fence = sda29 OMR **2 137 088 .. 2 149 887** and sda30 CP_DEBUG **2 149 888 .. 2 151 167** | `Exynos9820Ufs.h` | **done** — the whole interlocking block replaced in one step, rebuilt `EXACT_BUILD_OK` (driver 49 152 B), and a deliberate off-by-one was shown to fail the compile, so the `C_ASSERT`s gate the set. **Also closed, in the follow-up commit**: `UfsDiag.c`'s ladder target — it was the 9810's LBA 15 614 336, which on this disk is inside USERDATA — is now **31 234 816 .. 31 235 071**, the tail §9 read back zero, with its own asserts re-derived. What remains is a device step, not a constant: the scratch blocks are zero and **not stamped**, so `--write-live` refuses until the stamping pass runs, and nothing may be written to the media until Windows booting is established |
| **UFS hw-rev** — the S10e reports `UFS_VER_0006`; the 9810 path only knew 0004/0005. **Already handled**: `drivers/Exynos9820Ufs/Exynos9820Ufs.c:5085` sets the non-fatal `UFS_DIAG_FAILURE_HCI_VERSION_UNCONFIRMED` and lets the remaining warm-up checks decide, instead of refusing on an unmeasured constant | `Exynos9820Ufs.c`, `Exynos9820Ufs.h:57-61` | what stays unmeasured is the HCI encoding class 6 implies — read `UFS_REG_VERSION` on the device once Windows runs |

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

## 11. Where the session stands now (2026-10-09 — restart point: UFS HC + reset lib + RAM manager DONE)

Three firmware pieces are ported, wired and **building**: the UEFI-side UFS host
controller, the reset library, and the RAM manager. Nothing is validated on the
S10e; the FD is a build product, not a boot result.

**Committed (newest first):**

| Commit | What |
|---|---|
| `d1a20ae` | review fix: restore `UefiCpuPkg/UefiCpuPkg.dec` (ArmMmuLib.h lives there) |
| `cd23516` | RAM manager wired in DSC+FDF, rebuilt: `FVMAIN_COMPACT` auto-grew to 700416 B, **3864 B free**, FD unchanged 7 340 032 B sha256 `c72b3490…` |
| `2c12231` | `Beyond0LteRamManagerDxe` .c + .inf + `notes/m8-ram-manager.md` (incl. the page-table vs GCD decision) |
| `e64a4e6` | reset lib: gate the constructor's `Status` |
| `3c84773` | reset lib wired + `ResetSystemRuntimeDxe` re-enabled |
| `ecc0d2a` | `Beyond0LteResetSystemLib` .c + .inf + notes |
| `c664896` | notes: harsh review of the diag-fix commit |
| `70e34ca` | this §11 (earlier revision) |
| `78d6f97` | review fix: the `BEYOND0LTE_UFS_DIAG 1` branch was broken |
| `1cf61fa` | the five clang failures the UFS HC port exposed |
| `783bb5d` | UFS HC build result + MILESTONES line moved to done |
| `2764134` | UFS HC wired into DSC + FDF |
| `e411a81` | `Beyond0LteUfsHcDxe.c` created |

**Current build state:** `BEYOND0LTE_UEFI.fd` 7 340 032 B, `FVMAIN_COMPACT`
[99%Full] 700416 total / 696576 used / **3840 B free**, `FVMAIN` [27%Full]
1159016 used. Modules added this session: `…5545 Beyond0LteUfsHcDxe`,
`…5546 Beyond0LteResetSystemLib`, `…5547 Beyond0LteRamManagerDxe`.

**The FD is not byte-deterministic** — two identical rebuilds gave the same
used-bytes with different sha256 (`notes/m8-build.md` §8). Use sizes and FV
numbers to compare builds, never the FD hash.

**Nothing is in flight.** `git status` is clean.

**Do next, in this order (one commit per step, chunked):**

1. M8 is **done for the build**; the two remaining 9810 subsystems are
   **not applicable** (the 9810 build never used them — `notes/m8-hsi40.md`,
   and the `PlatformBootManagerLib` class already resolves to ArmPkg's).
2. **§5 hazard status, as of the last pass:**
   * **DSDT CPU sub-device buffers** — CLOSED (as above).
   * **PRAM breadcrumb** — **DECIDED, deliberately not enabled.** `debug_snapshot`
     occupies `0xFD900000`→`0x100000000` contiguously, so `0xFED14000` is inside a
     vendor log region. The unclaimed gap is `0xD9801000`…`0xF0000000` (right after
     `s5100_msi`, `Exynos9820.h:102-103`) and it is above the published bank0 stop
     `0xD0000000`, so Windows never sees it as RAM. `BEYOND0LTE_PRAM_DIAG` stays
     `0` because it is diagnostics, not boot-critical, and the first boot wants the
     fewest moving parts (`notes/vendor-dt-reserved.md`).
   * **fb base** — **not closable from the phone**: every `reg` read is
     `Permission denied` (`notes/s10e-live-reads.md` §9.1) and the browser model
     found no framebuffer / sboot / simple-framebuffer node in anything reachable.
     `EXYNOS_FB_BASE` stays `0`, which is safe — the map just omits the entry.
     Closing it needs Samsung's `exynos9820-beyond0lte*.dts` opened as a primary
     source.
   * **write-fence / protected-partition LBAs** — **partly closed by names**
     (`notes/s10e-partitions.md`): BOOT is sda14 and RECOVERY sda15 on this device,
     and the protected run is sda26…sda31, so the driver's sda19..sda25 protection
     covers the wrong partitions. Exact starts remain unread (`/sys/block/sda` is
     `Permission denied`), and the header forbids patching the block from arithmetic
     — it needs a real GPT read. **This is the most dangerous remaining constant**:
     a wrong fence admits writes into real partitions.
   * **UFS hw-rev** — already handled: `Exynos9820Ufs.c:5085` records the
     non-fatal `UFS_DIAG_FAILURE_HCI_VERSION_UNCONFIRMED` (verified:
     `UfsSetDiagnosticFailure` only ORs into `FailureMask`, `Exynos9820Ufs.c:631-637`)
     instead of refusing on an unmeasured constant.
3. **The installer's reversibility path (new, 2026-10-09):** the install is not
   one-way — it overwrites BOOT/CACHE/USERDATA only, and `BackupService.cs` is
   already device-agnostic (by-name links; every identity partition the S10e has
   was confirmed present). What blocks the S10e flow is **hard-coded 9810
   geometry**: `PartitionMap.cs:67-70` (`DiskBytes`, `WindowsOffset`,
   `WindowsBytes`, `CacheBytes`), `Validation.cs:9-10` (RECOVERY/BOOT sizes),
   `ToolsetManager.cs:84` (star2lte TWRP payload) and the UFS fence LBAs.
   and what blocks it is hard-coded 9810 geometry. **Now partly fixed**
     (`cf8fc3c` + `de7b9f7`): `PartitionMap` carries a per-device `GeometryType`
     (beyond0lte = `DiskBytes 127 943 049 216`, `WindowsOffset 8 811 184 128`,
     `WindowsBytes 119 126 622 208`, `CacheBytes 629 145 600`, from Samsung's
     `BEYOND0LTE_EUR_OPEN.pit` — its start/length fields are in 4096-byte units,
     proven by contiguity and by matching 30/31 of the phone's own sizes), the
     VHDX build and the partition-stage check take the identified phone's geometry,
     and `Validation.cs` now limits boot images by the smaller RECOVERY.
     **Still open:** the four GPT GUIDs (name-derived, unconfirmed on this device),
     whether the phone's current GPT still equals stock (sizes match, starts
     unconfirmed — the partition stage should verify offsets too), the
     `ToolsetManager.cs:84-89` payload names/messages (still say star2lte), and
     the UFS driver's fence/protected LBAs (`notes/pit-geometry.md` has the
     numbers: Windows volume LBA 2 151 168..31 234 810, protected run from
     VENDOR LBA 1 543 168, ESP CACHE LBA 1 983 488..2 137 087, all in 4096-byte
     units).
4. **The next step is a decision, not a read.** Flashing is forbidden until Windows
   booting is established (ground rules 5 and 6), and nothing here can prove that
   without the phone. So the next safe action is to agree the first-boot
   observation plan: what the screen, the disk and the CPU must show, and what a
   failure looks like — then flash once, deliberately.
4. Only after that: the stretch phase M10–M15.

**Facts to trust, not re-derive:** everything in §10, `notes/m8-ufs-hc.md`
§5/§7/§8, `notes/m8-reset-lib.md` §5, `notes/m8-ram-manager.md` §4,
`notes/m8-hsi40.md`, `notes/m8-build.md` §8, `notes/m8-dsdt-mat.md` §5,
`notes/claude-mat-layout.md` (the `_MAT` lead and its verification),
`notes/vendor-dt-reserved.md` (AI-sourced DT table — corroboration marked, not
primary), `notes/s10e-live-reads.md` §9 (what `adb` can and cannot give).
**Still open:** the fb base, the partition LBAs, the UNVERIFIED lists in the
notes files, and the fact that the
`BEYOND0LTE_UFS_DIAG 1`, `BEYOND0LTE_PMU_RESET 1` and
`BEYOND0LTE_RAM_STATUS_DIAG 1` branches have never been compiled (argued, not
built — and re-enabling them writes to known-bad addresses, so do not test them
on the phone).

---

## 12. Where the session stands now (2026-10-09 — restart point: the write fence is retargeted)

The session resumed from a crash that left `Exynos9820Ufs.h` **half-edited and
uncommitted** — exactly the state the block's own `C_ASSERT`s exist to catch. It
was finished in one step and reviewed; six commits landed, `git status` is clean
apart from `info/DEN0101_00bet1_ADAC.pdf`, which is a reference document and is
deliberately not committed.

**Committed (newest first):**

| Commit | What |
|---|---|
| `1e3e8bb` | review fix: comment alignment in `UfsDiag.c`'s `--fs-write` block |
| `3d6b04b` | M5: `UfsDiag.c` scratch region retargeted 15 614 336 → **31 234 816..31 235 071** (the 9810's LBA sat inside USERDATA), asserts re-derived, restore source is now zero-fill |
| `07d866b` | review fix: the .c side of the write guard narrated the 9810's layout |
| `fb63784` | review fix: the fence block's own documentation (duplicate table, an assert commented as EFS that tests BOOT's end, provenance marker) |
| `9f0dfef` | review fix: the protected-block/fence asserts are branch-specific (mode 0 did not compile) |
| `7d17971` | M5: the whole interlocking fence block replaced in one step |
| `ec76bce` | notes: tail measured on-device — LBAs 31 234 812..31 236 095 all zero, no backup GPT |
| `fd4e60e` | notes: the S10e GPT header's own geometry fields decoded (AltLBA 31 236 095, LastUsableLBA 31 236 090) |

**Build state:** `Exynos9820Ufs.sys` 49 152 B sha256 `3a19a6bb…caf0d0b`,
`UfsDiag.exe` 213 504 B sha256 `7f4f7436…42965bf`, `EXACT_BUILD_OK` in
`UFS_FULL_WINDOWS_MODE` 1 (and, after `9f0dfef`, in mode 0 too: 49 664 B).
Firmware FD unchanged this segment.

**Nothing is in flight.** Every claim in the fence block is a read, not
arithmetic: `notes/s10e-root-reads.md` §2 (live GPT), §8 (header fields),
§9 (tail measurement). The `C_ASSERT`s were proved to gate the constants by
deliberate off-by-ones that failed the compile.

**Do next, in this order (one commit per step, chunked):**

1. **Nothing may be written to the media.** The scratch blocks on the S10e are
   zero and **not stamped**, so `--write-live` refuses until they are stamped —
   that is a device write and ground rules 5/6 forbid it until Windows booting
   is established. Record it as the first post-boot device step, do not attempt
   it now.
2. The remaining §5 hazards that a **read** can still close: which CMU bank feeds
   PERIC0/FSYS0A (needs Samsung's `cal-if` headers as a primary source), and the
   UFS hw-rev class 6 encoding (needs `UFS_REG_VERSION` read once Windows runs).
3. The installer's last star2lte leftovers: `ToolsetManager.cs:84-89` payload
   names/messages still say star2lte.
4. MILESTONES M5 rows still `todo` that this pass made load-bearing: **handle the
   second LUN** (`cpefs` is `sdd1`, the driver was written against one `sda`) and
   **re-derive the DMA window** from the S10e's DRAM banks (the window constants
   are still the 9810's `90000000..BC800000` and marked UNVERIFIED).
5. Only after Windows boots once: the flash/stretch phase M10–M15.

**Facts to trust, not re-derive:** everything in §11, `notes/m8-ufs-fence.md`
(all of it, including the review findings), `notes/s10e-root-reads.md` §2/§8/§9,
`notes/pit-geometry.md`, `notes/windows-m7-toolchain.md` §4.5 (the working
`build-ufs.ps1` line) and §8 (the FD is not byte-deterministic — compare sizes
and FV numbers, never hashes of the FD).
**Still open:** the §5 hazards above, the second LUN, the DMA window, the
UNVERIFIED lists in the notes files, and the fact that **no** part of this has
been validated by booting.

---

## 13. Where the session stands now (2026-10-09 — restart point: installer made device-honest)

Same session as §12, continued. Two arcs landed: the UFS write fence (§12) and
six installer steps that remove the silent star2lte assumptions.

**Committed this segment (newest first):**

| Commit | What |
|---|---|
| `a42b8a0` | M9: the build-folder comment described a star2lte filter that is actually on "uefi" |
| `b829512` | M9: the UI names the connected phone (`PhoneDisplayName` keyed on codename; the backup manifest no longer fabricates SM-G965F; the recovery-ticket text says the protocol is 9810-only) |
| `7c2d0dd` | docs: §13 restart point (this table) |
| `5d0b819` | M9: ESP typing no longer demands a backup GPT (`ReadHeaderOrNull`); two wrong claims in the notes corrected (S10e entry name is at +56, not +80) |
| `5068d0b` | M9: star2lte RECOVERY geometry constant fixed (68 148 992 was not a whole number of LBAs) + a test that every geometry size is LBA-integral |
| `72aee5f` | M9: `TwrpProfiles` per-phone payload identity; `ToolsetConfig.DeviceCodename`; TWRP/WinRE payload names and the UFS `.inf` resolved from it |
| `46ce8d5` | M9: the secondary ESP is per device (the bare `SYSTEM` constant would reformat a 5.98 GiB Android system on an S10e) |
| `edec075` | M9: geometry cannot be guessed — `For` throws, `IsKnown` refuses, `VhdxImageBuilder` has no default |
| `377b4d1` | M9: per-device stage gating wired into the app (`Resolve(id, codename)` at the top of `RunStageAsync`) |
| `f223276` | notes: the install-attempt inventory (two research passes) |
| `387ec49` | notes: what an install attempt hits today |
| `44aac64` | docs: §12 restart point |
| `1e3e8bb`…`7d17971` | the UFS fence block + its five review fixes |

**Build state:** installer `dotnet build` 0 warnings / 0 errors, suite **194 passed**
(189 before this segment). UFS driver `Exynos9820Ufs.sys` 49 152 B sha256
`3a19a6bb…`, `UfsDiag.exe` 213 504 B sha256 `7f4f7436…`. Firmware FD unchanged.

**Nothing is in flight** — `git status` is clean except `info/DEN0101_00bet1_ADAC.pdf`,
which is a reference document and stays uncommitted.

**The phone is NOT attached** (`adb devices` empty). Every device read below is
written as the command to run, not run.

**Do next, in this order (one commit per step):**

1. `AndroidBootImage.cs:20` `RecoveryPartitionBytes = 68_149_248` is a star2lte
   constant used as the "does the built recovery fit" gate (`WinReTwrpBuilder.cs:358`,
   `ToolsetManager.cs:266`). It is only reached from star2lte paths now (the profile
   refuses the skin elsewhere), but it should come from the geometry.
2. The `beyond0lte` stage paths as **Guided** (never `Ready`): `twrp`, `backup`,
   `partition`, `transfer`, `uefi`, `firstboot` have no S10e implementation and are
   stopped by the gating added in `377b4d1`. The S10e's official TWRP file name is
   still UNVERIFIED (`TwrpProfiles.cs` carries null on purpose) — the read that
   closes it is `getprop ro.twrp.version` on the phone plus the release page.
3. The S10e UEFI boot image artefact — **blocked**: `firmware/tools/repack-with-ref.py`,
   `firmware/device-data/` and `tools/bootimg-pack.sh` do not exist in this checkout
   (`Test-Path` false for all three), so even the star2lte pipeline is not
   reproducible here. `notes/m8-uefi-image.md` has the blocker list and the read
   that closes each constant.
4. Only after Windows boots once: stamping the S10e scratch region (the ladder
   refuses until then) and the rest of the stretch phase.

**Facts to trust, not re-derive:** `notes/m9-install-attempt.md` (all of it,
including the six steps and the review findings), `notes/installer-device-specifics.md`
(the inventory — but read the CORRECTED lines, two of its arithmetic claims were
wrong and are fixed in place), `notes/m8-ufs-fence.md`, `notes/s10e-root-reads.md`
§2 (entry name at **+56**, corrected)/§8 (header geometry)/§9 (tail is zero),
`notes/windows-m7-toolchain.md` §4.5 (the working `build-ufs.ps1` line).
**Still open:** everything in the list above, plus the §5 hazards and the fact
that nothing has booted.

---

## 14. Restart point for a fresh session (2026-10-09 — the UEFI image is the last blocker)

A session ended here. Nothing is in flight: `git status` is clean except
`info/DEN0101_00bet1_ADAC.pdf`, a reference document that stays uncommitted on
purpose. The phone was attached and in TWRP (`RF8M21HYDSZ   recovery`, root) for
the last pass.

### What is true now (all of it committed, all of it read rather than derived)

* **The UFS write fence is this device's own table** (`Exynos9820Ufs.h`): fence
  **1 983 488 .. 31 236 090**, protected ranges sda29 OMR and sda30 CP_DEBUG,
  GPT backup first LBA 31 236 091, expected last LBA 31 236 095, scratch
  31 234 816 .. 31 235 071. `UfsDiag.c`'s ladder target moved there too (it was
  the 9810's LBA, which sits inside USERDATA). Verified by building both
  `UFS_FULL_WINDOWS_MODE` values and by deliberate off-by-ones that failed the
  compile. `notes/m8-ufs-fence.md`.
* **The installer no longer assumes the 9810.** Per-device stage gating is wired
  into the app; geometry refuses an unidentified phone; the secondary ESP is
  per-device (the bare `SYSTEM` constant would have reformatted a 5.98 GiB
  Android system); payload names/pages/the UFS `.inf` come from `TwrpProfiles`;
  the ESP typing no longer demands a backup GPT this disk does not have; the UI
  names the connected phone. Suite **194 passed**, `dotnet build` 0/0.
  `notes/m9-install-attempt.md` §8..§17.
* **The S10e's TWRP identity is closed by reads:** official image
  `twrp-3.7.0_9-2-beyond0lte.img`, page
  `https://twrp.me/samsung/samsunggalaxys10e.html`, and the phone already runs
  that build (`ro.twrp.version = 3.7.0_9-2`, `ro.twrp.sar = true`, model
  `Galaxy S10e`, BOOT = sda14, RECOVERY = sda15). `notes/m9-install-attempt.md` §15.
* **Beyond0lte stage states now:** `image`/`backup`/`partition` = Experimental,
  `unlock`/`twrp`/`transfer`/`firstboot` = Guided, `uefi` = NotImplemented.
  `StageCatalog.AutomatedFor` is a second axis: the 9810 stage code does not run
  against this phone at all; the stage returns a manual procedure instead.

### The single remaining blocker, and the exact next order

No flashable UEFI image exists for the S10e. Do this, in this order, one commit
per step:

1. Read the container this phone's sboot already accepts: as root in TWRP,
   `dd if=/dev/block/by-name/recovery bs=4096 count=256` (and the same for
   `boot`), pull, and parse the Android boot header (page size, header layout,
   the DTBH device-tree blob). Reads only.
2. Write a 9820 variant of `firmware/tools/bootshim/bootshim.S`. Carry the
   VERIFIED constants (watchdog `0x10050000`/`0x10060000` from
   `Exynos9820.h:426,431`; FD size `0x700000`, confirmed against the built
   `BEYOND0LTE_UEFI.fd`; the FD's first word is a real ARM64 `B`, checked
   mechanically) and **retarget or delete** the star2lte-shaped ones: the
   ramoops breadcrumb at `0xFED14000` is inside `debug_snapshot.log_bcm`/`log_llc`
   on this SoC, the DIAG paint is the 9810's `0xCC000000`/1440×2960 (this phone:
   `0xCA000000`, 1440×3040), and the temp SP `0xA0000000` is inside the verified
   `camera` reservation `0x99000000+0xE400000`.
3. Write the repack tool (`repack-with-ref.py` does not exist here — `pack-uefi.ps1`
   calls it and has never been reproducible in this checkout).
4. Name every still-unknown constant `UNVERIFIED` in the source, each with the
   read that would close it: `UEFI_BASE` (`Beyond0LtePkg.fdf:68` says
   `0x80080000`, unverified; the real sboot kernel-drop address is **not** closable
   by a cheap read), the required Linux-header `image_size`, and the 9820's
   write-protected (RKP-analogue) zones.

### Rules that still bind this work

* **Nothing may be written to the media** — no flashing, no partition erasing, no
  scratch stamping — until Windows booting is established (ground rules 5/6).
* **AVB hazard, from Team Win's own page:** a stock S10e is protected by Android
  Verified Boot; flashing TWRP without first flashing a verity-disabled `vbmeta`
  image leaves the device unable to boot. This disk's `vbmeta` is LBA 83 584..83 599.
* The 9810 packages stay **byte-unchanged**; port by copying into the
  `Beyond0Lte*` paths.
* No stage is `Ready` without device validation; unvalidated stays
  `Guided`/`Experimental`/`NotImplemented`.

### Facts to trust, not re-derive

`notes/m8-ufs-fence.md` (the fence and its review), `notes/m9-install-attempt.md`
(all seven installer steps plus the TWRP identity), `notes/installer-device-specifics.md`
(the inventory — read the CORRECTED lines, two of its arithmetic claims were wrong
and are fixed in place), `notes/m8-uefi-image.md` (§16 is this pass's shim table),
`notes/s10e-root-reads.md` §2 (live GPT; entry name at **+56**, corrected) / §8
(header geometry) / §9 (the tail is zero, no backup GPT), `notes/pit-geometry.md`,
`notes/windows-m7-toolchain.md` §4.5 (the working `build-ufs.ps1` line) and §8
(the FD is not byte-deterministic — compare sizes and FV numbers, never hashes).

### Still open after all of the above

The §5 hazards (which CMU bank feeds PERIC0/FSYS0A; the UFS hw-rev class-6
encoding), the second LUN (`cpefs` is `sdd1`, the driver was written against one
`sda`), the DMA window (still the 9810's `90000000..BC800000`, marked UNVERIFIED),
the timeout clock, and the fact that **nothing has booted on this phone yet**.

## 15. Restart point for a fresh session (2026-10-10 — the shim and the repack tool are committed)

A session ended here (twice). Nothing is in flight: `git status` is clean except
`info/DEN0101_00bet1_ADAC.pdf`, which stays uncommitted on purpose. The phone was
attached and in TWRP (`RF8M21HYDSZ   recovery`) for the whole pass, and the only
device writes were to `/tmp` (ramdisk), never to a partition.

### What is true now (all committed, all verified by running, not by reading)

* **HANDOFF §14 step 1** (the container read) was already committed as `078b27f`.
* **§14 step 2 — the 9820 boot shim: DONE** at
  `firmware/tools/beyond0lte-bootshim/bootshim.S` (`d54d805` + review fix
  `bf4df1b`). The 9810 `firmware/tools/bootshim/bootshim.S` is byte-unchanged
  (`git diff` on that path is empty). Both `-DBEYOND0LTE_PRAM_DIAG=0|1` builds
  assemble/link/objcopy with exit 0; `llvm-nm` puts `_payload` at the last byte
  of the image in both; no live 9810 constant survives (`rg` hits are comments
  that name the old value as WRONG here).
* **§14 step 3 — the repack tool: DONE** at `firmware/tools/repack-with-ref.py`
  (`4215fe8` + review fix `ce96084`): the tool `pack-uefi.ps1:121` calls but that
  never existed in this checkout. Device-agnostic — page size, section order and
  `tail` come from the reference image through `tools/twrp-winre/bootimg.py`.
* **§14 step 4 — the unknown constants are named `UNVERIFIED` in the source**,
  each with the read that would close it (`rg UNVERIFIED` in the shim: 11 sites —
  `UEFI_BASE`, `image_size`, the WTCON offset, `BEYOND0LTE_PRAM_BASE`, the temp
  SP, K4b the drop address, K9 the MMU state, K10 the write-protected zones).
* **The pipeline was run end to end against a real container** pulled from the
  phone this session: ref = `/dev/block/by-name/recovery` (sda15), 55 277 568 B,
  page_size 2048; kernel = `cat(shim.bin 288 B, BEYOND0LTE_UEFI.fd 7 340 032 B)`
  = 7 340 320 B; output 18 253 824 B, header differs from the ref in 24 bytes
  only, `ramdisk`/`second`/`dt`/`tail` byte-identical after re-parse. Full detail:
  `notes/m8-uefi-image.md` §17-§19 and correction passes 3-4.

### The exact next order, one commit per step

1. **A beyond0lte packer — DONE** at `firmware/tools/pack-uefi-beyond0lte.ps1`
   (`5f50e27`): the 9810 `pack-uefi.ps1` stays byte-unchanged and cannot be
   retargeted by parameters (it hardcodes `tools\bootshim`, the Star2Lte FV dir
   and `STAR2LTE_UEFI.fd`). Running it with the pulled ref reproduces the manual
   run exactly: shim 288 B, kernel 7 340 320 B, output 18 253 824 B.
2. **Record the reference container's identity** (size + SHA-1 + the exact `dd`
   line) in `notes/m8-uefi-image.md` so the 55 MB ref can be re-pulled; the image
   itself stays out of the repo (`*.img` is gitignored).
3. **K10 — the 9820's write-protected zones**: primary source is Samsung's LK4/RKP
   configuration; the alternative is observing a reset on the first boot. Until
   then the temp SP and `UEFI_BASE` stay UNVERIFIED.
4. **Only then a first boot** — and that needs explicit user consent: it means
   writing to the media, which ground rules 5/6 forbid until Windows booting is
   established, plus Team Win's AVB hazard (a verity-disabled `vbmeta` first;
   this disk's `vbmeta` is LBA 83 584..83 599).

### Rules that still bind this work

Unchanged from §14: nothing written to the media; the 9810 packages byte-unchanged;
no stage `Ready` without device validation; never guess a constant — name it
`UNVERIFIED` with the read that closes it.

## 16. Restart point for a fresh session (2026-10-10 — K10 research landed; the source is now corrected against live reads)

A session ran out of context mid-K10 and was resumed here. Nothing is in flight:
`git status` is clean except `info/DEN0101_00bet1_ADAC.pdf`, which stays uncommitted
on purpose. The phone was attached in **TWRP** (`RF8M21HYDSZ   recovery`) the whole
pass; every device action was a read (`ls`, `od`, `cat`, `adb pull` of
`/proc/device-tree/**`), nothing was written to the phone at all this session.

### What is true now (all committed, all verified by running)

* **K10 research notes** `notes/K10-rkp.md` T15-T21 (three commits): the 9820
  `reserved-memory` list from Samsung's own `exynos9820-rmem.dtsi`, the phone's own
  live DTB values, the dead ends, and two review records.
* **The live DTB closes things the vendor DTS could not.** In a TWRP **root** shell
  the property contents are **not** SELinux-blocked, so `reg` values that earlier
  sessions could only name as node names are now reads:
  `memory@80000000` = `0x80000000 + 0x3AB00000` (**ends `0xBAB00000`**, not 2 GiB),
  `memory@C0000000` = `+0x20000000`, `memory@E1900000` = `+0x1E700000`,
  `el2_code` = `0x87000000 + 0x200000` (owner `el2,uh`), `el2_earlymem` =
  `0x97E000000 + 0x200000`, `kaslr` = `0x80100000 + 0x100000` (owner
  `kernel-kaslr`), `log_bcm` = `0xFEB10000 + 0x400000`, `camera` =
  `0x99000000 + 0xE400000`, and the live `chosen` initrd =
  `0xD5000000..0xD5A67000`.
* **Three carried numbers are now provably wrong**, and the source says so:
  (1) `EXYNOS_DRAM_BANK0_SIZE` was the DTS's 2 GiB — fixed to `0x3AB00000`;
  (2) the UFS DMA window `0x90000000..0xBC800000` **overruns** the live bank0 end
  by `0x1D00000`; (3) `PlatformMemoryMapLib.c` published bank0 to `0xD0000000`,
  i.e. `0x15500000` of RAM the DTB never declared — now clamped to
  `min(EXYNOS_DRAM_SIZE, EXYNOS_DRAM_BANK0_SIZE)`.
* **The `0x84000000` initrd bound every FD_BASE comment cited is a kernel default**,
  not an sboot observation; sboot put this boot's initrd at `0xD5000000`. So
  `UEFI_BASE` / `FD_BASE_ADDRESS` `0x80080000` is now an unobserved guess in all
  three places (`Exynos9820.h`, `Beyond0LtePkg.dsc`, `Beyond0LtePkg.fdf`), and the
  first on-device evidence points at the **`0x80100000`** side of the K4b
  disagreement instead.
* **K10 is closed as a mechanism question, not an address question**: Samsung's own
  `rkp.h` (SM-G973F_QQ_Opensource) hands RKP `_text.._etext` / `_srodata.._erodata`,
  so the 9810's `0x80094000-0x828DBFFF` has **no 9820 constant equivalent** and must
  not be copied. Builds re-run clean after every edit
  (`build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG`, exit 0; shim
  assembled for both `BEYOND0LTE_PRAM_DIAG` values, 288 B / 392 B).

### The exact next order, one commit per step

1. **Decide the FD/UEFI run address — K4b is now a read, not a guess.** `/proc/iomem`
   in a TWRP root shell plus the local `Image` header of the kernel sboot just booted
   give the drop exactly: `Kernel code` starts `0x80198000`, `text_offset` is
   `0x80000`, so sboot loaded the `Image` at **`0x80118000`**, and
   `0x80198000 + image_size (0x3138000) = 0x832D0000` equals the `Kernel data` end
   (`0x832CFFFF`) — arithmetic exact. `0x80080000` is therefore **inside** the region
   sboot uses for the loaded image, and it overlaps two sboot-owned nodes
   (`sec_debug_magic 0x80000000+0x1000`, `kaslr 0x80100000+0x100000`). Full detail:
   `notes/k4b-drop-address.md` K01-K04. The candidate `>= 0x832D0000` (above the
   kernel window, below `mm_debug 0x91200000`, inside bank0 which ends
   `0xBAB00000`) is a decision to test on device, not a value to adopt; if it moves,
   `Beyond0LtePkg.dsc:415/417`, `.fdf:68` and the shim's `-DUEFI_BASE`
   in `pack-uefi-beyond0lte.ps1` must move together.
2. **The reserved regions inside bank0 are still handed to Windows as RAM**
   (`el2_code`, `kaslr`, `camera`, `secure_camera`, `mm_debug`) — the split needs a
   decision (publish them as `MMAP_UNMAP`/reserved, or stop bank0 at `0x80000000+`
   the first reservation), and it is not closable by a read we already have.
3. **The UFS DMA window**: pick a window inside the live bank0
   (`0x80000000..0xBAB00000`) minus the reservations above it, keep it marked
   UNVERIFIED until a device validation, and re-run the `C_ASSERT` set.
4. **Only then a first boot**, and that needs explicit user consent (media writes,
   ground rules 5/6, plus Team Win's AVB hazard: verity-disabled `vbmeta` first,
   this disk's `vbmeta` at LBA 83 584..83 599).

### Rules that still bind this work

Unchanged from §14/§15: nothing written to the media; the 9810 packages byte-unchanged
(`git diff` on `Star2LtePkg`, `Exynos9810Pkg`, `tools/bootshim` is empty); no stage
`Ready` without device validation; never guess a constant — name it `UNVERIFIED`
with the read that closes it. **New:** a TWRP root shell makes DTB property
contents readable — the "contents are SELinux-blocked" limit only applies to an
unprivileged shell on a booted Android, and every value read this way is a
**TWRP-boot** value, so a stock Android boot may add nodes over it.

## 17. Restart point (2026-10-10 — K10 closed, K4b closed as a read, the base moved)

Nothing is in flight; `git status` is clean. The phone stayed in TWRP
(`RF8M21HYDSZ   recovery`) and **every** device action this session was a read
(`ls`, `cat`, `od`, `adb pull` under `/proc/device-tree/**` and `/proc/iomem`).
No write to the phone, no flash, no dock test.

### What is committed and verified by running

* **K10** (`notes/K10-rkp.md` T15-T21): the 9820 write-protected set is a
  **mechanism**, not a constant — Samsung's `rkp.h` protects `_text.._erodata`
  at runtime, so the 9810's `0x80094000-0x828DBFFF` must not be copied. The
  EL2 region IS a read: `el2_code 0x87000000+0x200000`, `el2_earlymem
  0x97E000000+0x200000`, both owner `el2,uh`.
* **K4b** (`notes/k4b-drop-address.md` K01-K04): the drop is **`0x80118000`**
  and the kernel window is `0x80198000..0x832D0000`, derived from
  `/proc/iomem` + the local `Image` header (`text_offset 0x80000`,
  `image_size 0x3138000`, `flags 0xa`), cross-checked exact.
* **The base moved to `0x83300000`** in `.dsc`, `.fdf`, the shim default and the
  packer default, in lockstep; build exit 0, shim linked at `-Ttext=0x83300000`
  (`_payload` = last byte of the 288 B image), packer run end to end against the
  pulled recovery ref → 18 255 872 B, **not flashed**.
* **DRAM and MMIO extents are now reads** (`notes/s10e-dtb-mmio.md` S01-S03):
  bank0 `0x80000000+0x3AB00000` (confirmed twice — DTB `reg` and
  `/proc/iomem` `System RAM 80000000-baafffff`), bank1 `+0x20000000`,
  bank2 `+0x1E700000`; `SPEEDY`/`SPEEDY1` `+0x2000` each (they used to be
  skipped by the map), `PMU`/`DECON_T` `+0x10000`, `WDT` `+0x1000`,
  `dsim` three ranges, `clock-controller 0x1A240000+0x8000`.
* **Three carried numbers were proved wrong and fixed**: the 2 GiB bank0, the
  memory map publishing bank0 to `0xD0000000` (0x15500000 of undeclared RAM),
  and the UFS DMA window `0x90000000..0xBC800000` (overruns the live bank end).

### The exact next order, one commit per step

1. **The reserved regions inside bank0 are still published to Windows as RAM**
   — `sec_debug_magic 0x80000000+0x1000`, `kaslr 0x80100000+0x100000`,
   `el2_code 0x87000000+0x200000`, `mm_debug 0x91200000+0x100000`,
   `secure_camera 0x96000000+0x2200000`, `camera 0x99000000+0xE400000`. Decide
   the split in `PlatformMemoryMapLib.c` (reserve them, or stop bank0 at
   `0x80000000`), then rebuild and re-check the `C_ASSERT` set.
2. **The UFS DMA window**: the live bank0 ends `0xBAB00000`, so the carried
   `0x90000000..0xBC800000` is illegal; pick inside `0x80000000..0xBAB00000`
   minus the reservations, keep it UNVERIFIED, re-run the asserts.
3. **The UFS DTB extents disagree with the vendor DT** (`notes/s10e-dtb-mmio.md`
   S02): live `reg` sizes are `+0x200`/`+0x200`/`+0x800000`/`+0x1000` where
   `Exynos9820.h:319-327` calls `+0x2000`/`+0x8000`/`+0x1000` VERIFIED from the
   vendor DT. Bases agree, sizes do not. Resolve before trusting the windows.
4. **Only then a first boot** — needs explicit user consent (media writes,
   ground rules 5/6) and Team Win's AVB order: verity-disabled `vbmeta` first
   (this disk's `vbmeta` is LBA 83 584..83 599), then the image.
   The candidate image is already built at
   `C:\Users\ilya\AppData\Local\Temp\opencode\pack-out-833.img` (18 255 872 B)
   — regenerable with `pack-uefi-beyond0lte.ps1 -Ref <pulled recovery>`.

### Rules that still bind this work

Unchanged: nothing written to the media until Windows booting is established;
the 9810 packages byte-unchanged (`git diff` on `Star2LtePkg`,
`Exynos9810Pkg`, `tools/bootshim` verified empty this session); no stage `Ready`
without device validation; never guess a constant — name it `UNVERIFIED` or
mark it a **DECISION** with the reads it came from.

## 18. Restart point — the live one (2026-10-10, next session starts at `progress.md`)

The task list, by ID, is [`/progress.md`](../../progress.md) with one file per
task in [`/tasks/`](../../tasks/). This section exists so a fresh session can
start from `git status` alone; §14-§17 above are history and their work is
committed and verified — **do not redo it**.

### What is true now (committed, verified by running)

The 9820 platform builds (`build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG`,
exit 0, `FVMAIN_COMPACT` 3096 B free, FD `7 340 032 B` at
`firmware/edk2/Build/Beyond0LtePkg/DEBUG_CLANGPDB/FV/BEYOND0LTE_UEFI.fd`); the
shim assembles for both `BEYOND0LTE_PRAM_DIAG` values (288 B / 392 B); the packer
runs end to end (`pack-out-833.img`, 18 255 872 B). The FD now links to run at
**`0x83300000`** in all four places that must agree. The live DTB closed the DRAM
and MMIO extents, closed K4b as a read (sboot drop `0x80118000`, kernel window
`0x80198000..0x832D0000`), and closed K10 as a mechanism (RKP protects
`_text.._erodata` at runtime — the 9810's `0x80094000-0x828DBFFF` has no 9820
equivalent). The memory map now carves the monitor-owned RAM out of the banks.
Nothing has booted; nothing was flashed.

### Open, by ID

`T01` UFS DMA window · `T02` framebuffer RAM/DEV overlap · `T03` UFS extent
disagreement · `T04` second LUN · `T05` timeout clock · `T06` stock container
ref (blocked on root) · `T07` **the first boot attempt** · `T08` the installer's
S10e path · `P01` heimdall restore (parked) · `P02` UFS hw-rev 6 encoding ·
`P03` CMU parentage for PERIC0/FSYS0A.

### If the next session is "build it and run it on the phone"

1. Rebuild the three artifacts in that order — `build-uefi.ps1` (platform),
   `drivers/Exynos9820Ufs/build-ufs.ps1` (storage driver,
   `notes/windows-m7-toolchain.md` §4.5 has the working line), then the
   installer (`dotnet build` + `dotnet test`, `notes/m9-installer.md`; the
   catalog keeps `beyond0lte` at `NotImplemented` on purpose — see `T08`).
2. Regenerate the candidate image with `pack-uefi-beyond0lte.ps1 -Ref …`.
3. Read `tasks/T07-first-boot.md` **before** any write: it names the consent
   requirement, the AVB `vbmeta` ordering (this disk's `vbmeta` is LBA 83 584..83
   599), the slot isolation (recovery only, `boot` untouched) and the rollback
   caveat (the on-disk `ref-recovery.img` is a TWRP-era snapshot).
4. `T01`/`T05` are the two constants that can make a boot hang (timeouts) rather
   than crash — worth closing before the attempt, both are cheap code changes.

### Rules that still bind this work

Unchanged from §17: nothing written to the media without the user's explicit
decision; the 9810 packages byte-unchanged; no stage `Ready` without device
validation; every uncertain constant named `UNVERIFIED` or marked a **DECISION**
with the reads it came from; one commit per task, and the MILESTONES line moves
in the same commit as the work it describes.

## 19. Restart point — the live one (2026-10-10, next session starts at `progress.md`)

Two boot attempts were made on the real S10e and both failed at the same place; the
cause is now **identified with prior art**. Nothing is in flight; `git status` is clean
except `info/DEN0101_00bet1_ADAC.pdf` (reference doc, stays uncommitted). The phone is in
**download mode**; the recovery slot (sda15) holds the last failed container. `boot` (sda14)
is untouched, so **normal Android still boots**. Windows is **NOT** established.

### What is committed (newest first)

| Commit | What |
|---|---|
| `6916ed1` | T09 ANSWER: the 9820 boot image is AOSP `mkbootimg.py --header_version 2` + a real `--dtb` section (edk2-exynos `Platform/Samsung/exynos9820/s10.sh.inc`); our v0 container fails sboot's DT check |
| `16c6127` | research notes `notes/edk2-bootshim.md` (DT is a packaging concern, not shim/append) |
| `6689119` | boot #2 OBSERVED: AVB bypassed (`VERIFICATION_DISABLED` bit set) but STILL `dt table header check fail` |
| `6966bed` | CORRECTION: appended-FDT theory wrong (stock kernel has no appended FDT) |
| `2d7b168` | T09 attempt: bootshim flags 0xa→0x1a + packer `-Dtb`/`-ImageSize` (kernel=[shim][FD][pad][DTB]); candidate `pack-out-dtb.img` built (this attempt did NOT fix it) |
| `8ac1919` | diagnosis corrected (stock ref disproven) |
| `d4246b8` | diagnosis: container DT LOAD fail (from sboot's download-mode text) |
| `394523d` | T07 first boot OBSERVED: wrote UEFI to sda15, rebooted → straight to download mode |
| `e285a53` | P04 parking-lot task (vbmeta→TWRP guide) |
| `07cef00` | T01: UFS DMA window re-picked to 0xA7400000..0xBAB00000 |

### The exact next order (one commit per step)

The fix is a **container format change**: build the beyond0lte candidate as an **Android boot
image header VERSION 2 with a valid DTB section**, not the Samsung header-v0 + DTBH our
`tools/twrp-winre/bootimg.py` + `firmware/tools/repack-with-ref.py` emit (they leave `dt_size=1`).

1. **Extend our own builder to header-v2** (preferred — keeps the donation build in-tree):
   `bootimg.py` needs a v2 path (header_version 2 fields: `dtb_size`/`dtb_addr`, the standard
   AOSP v0/v1/v2 layout), and `repack-with-ref.py`/`pack-uefi-beyond0lte.ps1` must emit a v2
   image with `--dtb` = a device tree. Recipe to mirror (edk2-exynos `s10.sh.inc`):
   `--header_version 2 --pagesize 2048 --base 0x10000000 --kernel_offset 0x8000
   --ramdisk_offset 0x1000000 --second_offset 0xf0000000 --tags_offset 0x100 --dtb_offset 0x0
   --dtb <FDT> --ramdisk <dummy> --kernel <[shim][FD]>`. Keep the offsets marked UNVERIFIED
   until a boot confirms.
2. **If our v2 builder fails**, fall back to vendor **AOSP `mkbootimg.py`** with the same flags
   (the edk2-exynos route) to produce the v2 container.
3. DTB source: `C:\Users\ilya\AppData\Local\Temp\opencode\stock\dt.dtb` (239680 B, the phone's
   own primary FDT) — or edk2-exynos' `FdtBlob_compat/s10.dtb`. Prefer the phone's own tree.
4. **Then boot #3**: re-flash TWRP (restore sda15), `adb reboot recovery`, push + `dd` the v2
   candidate to sda15, `adb reboot recovery`, OBSERVE the panel. Expect it to get PAST the DT
   check (UEFI paint / Windows) or fail at the NEXT stage (the never-booted DECISION constants:
   UEFI_BASE 0x83300000, temp SP, K10 zones, fb base 0xCA000000).

### Facts to trust, not re-derive

`notes/edk2-bootshim.md` (the whole v2 recipe + sources), `notes/m8-uefi-image.md` B7–B16
(the two boot attempts, the byte-exact writes, the diagnosis chain and its corrections),
`tasks/T09-boot-payload-dtb.md` (the answer), `tasks/T06-stock-container-ref.md` (superseded
note). The stock images were LZ4-decompressed with python `lz4` 4.4.5 from
`C:\Users\ilya\code\Dumps\AP_G970FXXSGHWC1_..._meta_OS12.tar` (members are `*.img.lz4`).

### Still open after all of the above

The header-v2 container has never been built or booted; and the downstream DECISION constants
(UEFI_BASE 0x83300000, shim temp SP, K10 write-protected zones, framebuffer base 0xCA000000)
remain unvalidated — they are only reached once the DT check passes.

### Rules that still bind this work

Unchanged: nothing written to the media without the user's explicit decision (the two writes so
far were consented first-boot experiments); the 9810 packages byte-unchanged; no stage `Ready`
without device validation; every uncertain constant named `UNVERIFIED` or marked a **DECISION**;
one commit per task with the MILESTONES line moved in the same commit.

## 20. Restart point — the live one (2026-10-10, after boot #4)

**State.** Boot #4 put the header-v2 image (`pack-out-v2.img`) on the **BOOT** partition (sda14):
sboot's DT check now passes, but the phone **boot-loops** (B19-B21). sda14 still holds that image,
so Android does **not** boot until stock `boot.img` is restored. sda15 (recovery) holds the
failed boot #3 image, so TWRP must be reflashed (Odin) to get a shell. Windows is NOT established.

**Committed this session.** `b17d8f6` shim = the proven edk2-exynos minimal flow (144 B, built,
not booted). `992b7d2` review fix. `500abfc` stale MILESTONES row. Notes: `notes/m8-uefi-image.md` B23.
Uncommitted on purpose: `.gitignore` `info/*` and the T05 read-attempt note (left from the previous
session for the user to review).

**Update (later the same session).** Done and committed: `95357f4` build-uefi.ps1 sets
`CLANGPDB_DLL` (unset -> empty PATH entry -> "'make' is not recognized"); `6272939`/B25 FD base moved
to the prior-art **0x90000000** in all four sites, rebuilt; `62437aa`/B26 shim diagnostic (prior-art
DECON enable + two WHITE bands). Boot #5 image built, NOT yet written:
`%TEMP%\b25\boot5-v2.img`, 7 587 840 B, sha256 `1379b50a...0b3765` (full hash in notes B26).
Phone: in TWRP, root adb, user consented to boot #5. The agent's write to sda14 was refused by the
permission layer, so the user runs the flash.

**Next order:**
1. Flash `boot5-v2.img` to by-name/boot (sda14), read back the first 3705 x 2048 B and compare sha256,
   `adb reboot`, observe. Decode per notes B26: no bands / band 1 only / both bands then loop / output.
2. Record the observation as B27, then choose the next fix from it.
3. Rollback whenever wanted: stock `boot.img` (`%TEMP%\opencode\stock\boot.img`) to sda14 from TWRP.

## 21. Restart point — the live one (2026-10-10 evening): **UEFI reaches BDS on the S10e**

**State.** Boot #17 (`%TEMP%\b63\boot17-v2.img`, sha256 `1625b5e4…682d`) is on BOOT (sda14). It
runs shim → SEC → PEI → DXE → BDS on the real phone (photo `notes/img-boot17-bds.jpg`). BDS then
fails to find storage and retries forever. TWRP is on recovery (sda15). Android does not boot
until the stock `boot.img` goes back to sda14 (`%TEMP%\opencode\stock\boot.img`). Windows is NOT
established. Escape a loop with the key combos in AGENTS.md §10.

**How we debug now (read first).**
- The DEBUG log is ON THE PANEL (Beyond0LteFbSerialPortLib, B54). Ask the user for a short video
  of the end of a cycle; extract frames with `imageio-ffmpeg` (pip), take the last non-black frame,
  crop and read. The newest line is the one above the blanked line (the console wraps).
- Samsung's reset log: AGENTS.md §9 (`/proc/debug_history` from TWRP).
- Flash loop (adb allowed by permission rules): build-uefi.ps1 → pack-uefi-beyond0lte.ps1 (any -Ref;
  only the kernel blob `FV/uefi-beyond0lte-kernel.bin` is used) → build-bootimg-v2.py --kernel …
  --dtb %TEMP%\opencode\stock\dt.dtb → push + dd to by-name/boot + readback sha → adb reboot.

**What it took (each fix device-validated; notes/m8-uefi-image.md B23-B65).** FD at 0x90000000;
minimal prior-art shim; panel is 1080x2280 (not 1440x3040); UEFI region in bank0
(PcdSystemMemorySize 0x3AB00000); SEC uses C BaseMemoryLib (DC ZVA faults MMU-off); every MMIO
window page-aligned (UFS_IOCOH); ArmGicV2Dxe (ArmGicDxe.inf is GICv3 in this edk2); virtual timer;
high banks + RamManagerDxe off; build-uefi.ps1 sets CLANGPDB_DLL. Timer = 26 MHz on-device (T05).

**Next order (one commit each).**
1. UFS: every query UPIU fails `OCS = 4` (UFSHCI: mismatch response UPIU size) after
   `UfsControllerInit Finished`. Compare the UTRD response-UPIU length/offset and PRDT encoding the
   driver writes against the 9820 HCI (hw-rev 6, P02); the 9810's A46/A48 notes in
   Beyond0LteUfsHcDxe.c are the closest prior experience. Goal: a block device, then BOOTAA64.EFI.
2. RAM for Windows: high banks are off for UEFI (B63); follow the 9810 record (Star2LtePkg.dsc:211-250).
3. Before any real install: strip the diagnostics (shim DIAG, checkpoints, DiagVectors, ResetHold,
   FB console back to UART) - FVMAIN_COMPACT has 416 B free.

## 22. Restart point — the live one (2026-10-10 night): UEFI reads the UFS disk; Windows prep under way

**Phone state.** BOOT (sda14) holds **boot #25** (`%TEMP%\b83\boot25-v2.img`, sha256 `82045f34…af8b`):
shim → SEC → DXE → BDS, UFS block I/O works, the GPT is valid, every partition gets a BlockIo
(notes/m8-uefi-image.md B85). TWRP on recovery (sda15). Android does not boot until the stock
`boot.img` (`%TEMP%\opencode\stock\boot.img`) goes back to sda14. Windows NOT installed yet.
**The session ended with adb hanging** (push of boot #26 stalled; the queued `dd` never ran - nothing
was written; stuck adb.exe killed). Check `adb devices` responds before any write; do push and `dd`
as separate, verified steps (never queue a `dd` behind an unfinished push).

**Built but NOT flashed: boot #26** `%TEMP%\w2\boot26-v2.img` sha256 `200bd7e1…4fd7` = boot #25 +
bank1/bank2 published as RAM (W2) + UFS DMA pinned to bank0. Its first boot is the test that EL1 may use
bank1/bank2 (DXE allocates top-down; a bad range resets like banks 3/4 did, B62). If it resets: drop
bank2 or bank1 (PlatformMemoryMapLib BankBase[3]/[4]) and retry.

**Done this session (all committed; details in notes/m8-uefi-image.md B66-B85, notes/m10-windows-deploy-plan.md).**
UFS in UEFI: byte-granular UTRD fields; 1 KB response region for NOP/query; OCS 0x7 + Response 0x01 on
CHECK CONDITION treated as the 9820 kernel does; PRDT entries <= 4 KB at the controller's 128 B FMP
stride, PrdtL = n*128 (Samsung LK ufs.c prior art), AllocateBuffer slack; log = ERROR|WARN|INFO,
per-command `UfsHc: ring/done` lines (capped at 40) and CHECK COND sense. Windows UFS driver
(drivers/Exynos9820Ufs): PRAM breadcrumbs off (UFS_PRAM_ENABLE 0), builds in both modes (W1). RAM: W2.

**Decisions from the user.** Consent to wipe USERDATA + CACHE for Windows (confirm again right before the
write). Media: `C:\Users\ilya\code\Windows11_22631.2861_Pro_arm64.iso` (install.wim index 3 = Pro).
Build/deploy Windows **the installer's way** (S9Woa.Installer.Core: VHDX + DISM Apply/Add-Driver +
unattend + bcdboot/BCD repoint + GPT CACHE->ESP + TWRP raw transfer) - drive Core from a small harness
since the WinUI app gates the S10e stages. The shell is elevated (DISM works).

**Next order (one commit each).**
1. Get adb healthy; flash boot #26; confirm UEFI still reaches the GPT/partition scan with bank1/bank2 RAM.
2. Harness over S9Woa.Installer.Core with PartitionMap.For("beyond0lte"); driver set = the rebuilt
   Exynos9820Ufs (`%TEMP%\ufs-b86-dmawin` if bank1 is RAM, i.e. build with -DmaWindow); build the VHDX.
3. With the user's go: CACHE->ESP, USERDATA <- Windows, BCD repoint; boot.
4. Expect uH (EL2) hangs after ExitBootServices - the 9810 needed Star2LtePkg's PlatformBootManagerLib
   winload/ntoskrnl patching; Beyond0LtePkg still uses ArmPkg's generic one (audit in m10 notes).
Building tips: build-uefi.ps1 runs from bash (`powershell.exe -File`); build-ufs.ps1 needs vswhere on
PATH ("C:\Program Files (x86)\Microsoft Visual Studio\Installer"). Video frames: `pip install
imageio-ffmpeg`, extract, use the last non-black frame. Recovery key combos: AGENTS.md §10.

**Artifacts copied out of %TEMP% (survive a restart):** `C:\Users\ilya\code\s10e-artifacts\` -
boot25-v2.img (on the phone now), boot26-v2.img (next to flash), boot.img (STOCK, for rollback),
dt.dtb / dtbo.dtb (stock trees; dt.dtb is the --dtb for build-bootimg-v2.py), ref-recovery.img (any
-Ref for pack-uefi-beyond0lte.ps1), ufs-b86-default\ and ufs-b86-dmawin\ (Windows UFS driver builds).

## 23. Restart point — the live one (2026-10-10 late night): boot #26 OK, harness ready, needs an elevated session

**Phone state.** BOOT (sda14) holds **boot #26** (`s10e-artifacts\boot26-v2.img`, sha256 `200bd7e1…4fd7`):
bank1/bank2 published as RAM and UEFI still runs through the GPT/BlockIo scan with no reset (W3) - EL1
may use those ranges. TWRP on recovery. Windows NOT installed; USERDATA/CACHE untouched.

**Ready (committed):** `installer/tools/S10eHarness` (verbs `image`, `check`, `transfer`) over
Installer.Core with PartitionMap.For("beyond0lte"). Driver package (signed UFS, dmawin build):
`C:\Users\ilya\code\s10e-artifacts\drivers-w4` (notes/m10-windows-deploy-plan.md W4).

**Why the session ended:** the image build needs admin (DISM / Mount-VHD / bcdboot) and the Hyper-V
PowerShell module (New-VHD, used by VhdxImageBuilder); the session was not elevated. The user chose to
restart Claude Code elevated.

**Next order (one commit each).**
1. In the elevated session: `Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-Management-PowerShell`
   (add `-All` only if it refuses for a missing parent; reboot if asked). Confirm `Get-Command New-VHD`.
2. Build: `dotnet run -c Release --project installer\tools\S10eHarness -- image
   C:\Users\ilya\code\Windows11_22631.2861_Pro_arm64.iso 3 C:\Users\ilya\code\s10e-artifacts\drivers-w4
   C:\Users\ilya\code\s10e-artifacts\win-w5` (writes a ~119 GB windows.img; C: has 2.6 TB free). Record the
   winload/ntoskrnl sha256 it prints.
3. Phone in TWRP: `S10eHarness check <adb> RF8M21HYDSZ`; then, after re-confirming the wipe with the user,
   `transfer` (USERDATA + CACHE as FAT32 ESP). BCD numproc 4 by default - consider `numproc 1` for the first boot.
4. Boot; expect uH (EL2) hangs after ExitBootServices (see the W5 notes for the 9810 patch-set research).

**§23 update (same night):** the session was elevated; step 1 is done except the PC reboot - Hyper-V (full set,
hypervisor included, notes W6) is enabled pending restart; the user chose to keep it and reboot the PC. After the
reboot, in an elevated Claude session: confirm `Get-Command New-VHD`, then continue at step 2. W5 (9810 patch
inventory) is in notes/m10-windows-deploy-plan.md.

## 24. Restart point - the live one (2026-10-11): WINDOWS KERNEL RUNS; storage driver fixed; unattended loop

**State.** BOOT = boot #38 (s10e-artifacts\boot38-v2.img). Windows 11 Pro 22631.2861 is on USERDATA, ESP on CACHE
(BCD numproc 1, testsigning on). Windows UFS driver in the image: s10e-artifacts\ufs-w23\Exynos9820Ufs-w26.sys
(swapped in place with TWRP: mount.ntfs /dev/block/by-name/userdata; DriverStore\FileRepository\
exynos9820ufs.inf_arm64_4efdc4525d11d997\Exynos9820Ufs.sys). Full story: notes/m10-windows-deploy-plan.md W13-W26.

**What got Windows to its kernel:** UEFI UFS DMA into non-page-aligned buffers is corrupt on this controller ->
1 MiB bounce + 1 MiB SCSI split (W18, W22). No winload/ntoskrnl patching was needed to pass uH.
**Windows storage:** the driver loads and finds ACPI\EXYN9820; it rejected the PMA range (0x1000 vs DT 0x800) ->
fixed in w26 (W26). Boot #38 + w26 did not come back to TWRP within 10 min = the driver reached OPERATIONAL (it
stops the firmware watchdog there) - presumably Windows first-boot setup is running. UNVERIFIED.

**Unattended loop (how to iterate without a human):**
- Shim + SEC: dead man's switch = INFORM3 recovery + watchdog cl0 82 s; BootLogDxe: reason normal + re-arm at
  ReadyToBoot and at ExitBootServices; firmware crash -> dump + UEFILOG.TXT + SWRESET to TWRP.
- ReadyToBoot creates \WINRUN.FLG; the next UEFI boot that finds it saves 64 KB of 0x8FE00000 as \WINPRAM.BIN
  (driver early marks at +0x8000: stages, failures, =BUGCHECK code/params), zeroes it, goes to TWRP.
- KNOWN: the boot right after a firmware-triggered reset (watchdog, PMIC keys) hangs in the shim (user: always so).
  The dead man's switch turns that into TWRP after 82 s; then 'adb reboot' gives a clean first boot that saves WINPRAM.
- Read results: adb shell mount -t vfat /dev/block/by-name/cache /tmp/esp -> UEFILOG.TXT, WINPRAM.BIN.

**Next order.** (1) Read Windows' own logs from NTFS in TWRP (Panther, evtx, Minidump). (2) Fix the second-boot shim
hang (suspects: missing exynos_acpm_reboot before our resets; stale state). (3) Strip bring-up diagnostics once
Windows boots. (4) More RAM (bank1/2 never reached the UEFI map - W18), then numproc > 1 (PSCI 0.1, W8).

### §24 update (2026-10-11, unattended session)
- Windows Setup's specialize pass COMPLETED on device (W32). Fixed on the way: Windows UFS driver PMA length (W26),
  doorbell retire via UTRLCLR (W27), CACHE typed as ESP (W30), FADT reset register 0x3A00=2 (W32), handoff RAM uncached (W32).
- Current: BOOT = boot #41; driver w32 (s10e-artifacts\ufs-w23\Exynos9820Ufs-w32.sys). A Setup run that is interrupted
  cannot resume: re-transfer the image first (firmware/tools/s10e-full-attempt.sh <boot.img> <driver.sys> <outdir>).
- Unattended loop: firmware/tools/s10e-cycle.sh; driver feeds the 82 s watchdog only while Windows writes (5 min idle,
  45 min cap); bugcheck -> SWRESET; clean shutdown -> progress 0x80 -> next boot continues into Windows.
- Open: the 'second boot after our own reset hangs in SEC MMU bring-up' (progress 0x60/0x61) is intermittent;
  Beyond0LteSecArmLib (MMU-on TLB flush) is prepared but not wired into PeilessSec.

### §24 update 2 (2026-10-11): WINDOWS DESKTOP REACHED
- Boot #44 + driver w32 (fresh image, attempt c44): Setup finished, the unattend user S10e auto-logged on and Explorer
  was active >= ~11 min (ExplorerStartupLog.etl / iconcache / jump lists, Winlogon + User Profile events). Evidence: notes W36.
- Fixed since the last update: second-boot hang = stale TPIDRRO_EL0 read by PeilessSec (shim zeroes it, W34);
  bank1/bank2 published as RAM (~1.5 GB, W33); firmware-reserved UFS DMA pool 0xA7400000 'DMP1' (W35).
- Current: BOOT = boot #44; driver w32. Windows still resets to TWRP after the bring-up watchdog cap (by design).
- Next order: (1) relax the watchdog cap / stop arming the dead man's switch once Windows is the target, so the
  desktop stays up; (2) display/input confirmation on the device; (3) SMP via PSCI 0.1 (numproc > 1, W8);
  (4) strip bring-up diagnostics (shim bands, early marks).

## 25. Restart point - the live one (2026-10-11 afternoon): desktop, touch, 5.2 GB, unattended loop

**State.** BOOT = boot #48 (s10e-artifacts\boot48-v2.img: watchdog flag off at EBS (W37), touch power DXE (ACPM),
64-bit DRAM bank published). Windows on USERDATA, driver Exynos9820Ufs w32; S10eDiag (driver + agent + CLI) installed
offline; Exynos9820Hsi2c (oem2) + S6SY761TouchBeyond0 (oem4) installed by pnputil inside Windows.
**Works on device (user-confirmed):** desktop, touch. Windows sees 5.24 GiB. 1 CPU (BCD numproc=1).
**Unattended loop (notes/m11-s10ediag.md):** from TWRP: `bash firmware/tools/s10e-install-diag.sh $TEMP/s10ediag
[extra.cmd] [drivers dir]` (installs driver/agent/hive keys, pushes ESP scripts, clears WINRUN.FLG), `adb reboot`.
The agent waits for explorer up 30 s, runs ESP \S10EDIAG\collect.cmd (+extra.cmd) and restarts into TWRP; results in
ESP \S10EDIAG\out\last. Crashes: S10eDiag's bugcheck callback -> INFORM3 recovery -> TWRP; stop code + step marks in
the PRAM ring (WINPRAM +0xA000, saved as \WINPRAM.BIN by one `adb reboot` bounce while WINRUN.FLG exists).
stay.flg on the ESP = interactive (no restart); desktop icons "Reboot to TWRP" / "Collect logs" (agent requests).
**Known:** Windows restart = PSCI SYSTEM_RESET -> bugcheck 0xA0 (Samsung EL3 is PSCI 0.1) - harmless in the loop
(lands in TWRP), real fix with SMP. Never read a whole page of 0x10421000 (unimplemented words abort; m12).
**Next order:** (1) SMP via the ACPI parking protocol for the 4 A55 cores + hide PSCI from Windows (notes/m14-smp.md,
in progress); then remove numproc. (2) Power button (ALIVE key-power under pinctrl@15850000). (3) Strip bring-up
diagnostics. (4) USB device mode (KDNET over USB EEM) as its own milestone.

### §25 update (2026-10-11 16:05): 6 cores, clocks raised, storage 2x
- BOOT = boot #63; UFS driver w34 (s10e-artifacts\ufs-w23\Exynos9820Ufs-w34.sys, swap with
  firmware/tools/s10e-swap-ufs.sh). 6 CPUs (A55 x4 + A75 x2) via the ACPI parking protocol, cores started one at a
  time (notes/m14-smp.md); M4 big cores park but hang Windows' boot (mask 0x3E). FADT without PSCI: Windows
  restarts cleanly through EFI ResetSystem (runtime DebugLib null); a clean restart does NOT honour INFORM3
  recovery, so the agent returns to TWRP with `s10ediag flushreset`.
- Clocks: ACPM DVFS from UEFI (channel 0 = clock-controller@0x1a240000, verified): LITTLE 1.456 GHz, MID 1.794 GHz,
  INT 400 MHz, behind a 0x66 self-healing guard (notes/m15-perf.md). Ladders from the TWRP kernel sysfs.
- Storage: w33/w34 (8-byte copies, no pre-zero, cached read copy-back with DC CIVAC) ~2x; remaining limit is
  ~4 ms per 64 KiB inside the controller at HS-G3x2 Rate B - cause open.
- Boot screen: no text (BEYOND0LTE_FB_CONSOLE 0) + progress bar; shim bands off.
- Tools in Windows: s10ediag (peek/poke/mark/flushreset), S10eAgent (ESP scripts, desktop icons incl. Reboot to
  TWRP / Collect logs / Task Manager / Rotate *), s10erotate, s10eperf.
- Next: storage controller clocks (FSYS0/UniPro), big cores, rotation test (needs the user), power button.

## 26. Restart point (2026-10-11 evening): fresh Windows reinstalled; M17 USB kernel debugging built, not flashed

**State.** 0x14C investigation dropped (notes/m16-usb.md tail): USERDATA re-transferred from s10e-artifacts\win-w5
(pristine image) + UFS w34 + SetupComplete.cmd provisioning (firmware/tools/s10e-provision-fresh.sh: numproc
removed for the 6 parked cores, W37 power keys, S10eDiag services, HSI2C + touch packages; log
C:\S10eProv\setupcomplete.log). Windows Setup was running on BOOT = boot #71 (XHC0 hidden) at 19:08.
**M17 (notes/m17-usb-kd.md):** KDNET over USB EEM with the inbox kd_8003_5143.dll. Committed: T1 firmware KD
device mode (BEYOND0LTE_USB_KD_DEVICE 1), T2 DSDT UFN0, T3 Dbg2.aslc, T4 extra-kd.cmd + s10e-stage-kd.sh.
boot #72 (s10e-artifacts\boot72-v2.img, sha 2d9609eb…) = T1-T3, NOT flashed.
**Next order.** (1) Setup finished -> TWRP: read C:\S10eProv\setupcomplete.log, confirm 6 CPUs / touch.
(2) Flash boot #72 (dd to by-name/boot, readback sha) + `bash firmware/tools/s10e-stage-kd.sh`; boot Windows
once (agent applies BCD, writes ESP S10EDIAG\out\last\kd\kdnet-list.txt). (3) Phone on the PC's USB 3 Type-A
port, host firewall off, `windbg -k net:port=50005,key=1.2.3.4`, reboot the phone. Suspects if no link:
notes/m17-usb-kd.md T1/T3 reviews (SUSPHY, GFLADJ, DBG2 revision, OEM controller type 5 vs 3/4).

## 27. Restart point (2026-10-12, worktree branch `worktree-agent-af7371b6f9c487478`): D8a audio milestone 1 built, not flashed

**State.** Branch is main 302c285 + the D8a commits (notes/d8-audio-m1.md has the log and reviews).
New `Beyond0LteAudioProbeDxe` (flags `BEYOND0LTE_AUDIO_PROBE` 1, `..._RELEASE_RESETS` 1, guard 0x69) reads
the CS47L93 ID over spi_2 and both CS35L40 DEVIDs over hsi2c_38 into UEFILOG and restores every register;
TouchPowerDxe additionally logs S2MPS19 L17CTRL/L18CTRL (codec rails, read only). Platform build exit 0 in
a private workspace (the worktree has no edk2 checkout; main's Build dir was deliberately not used).
Nothing packed or flashed; no adb used.
**Next order.** (1) Merge to main, rebuild with `build-uefi.ps1`, pack + flash as usual. (2) Run notes
§3: grep UEFILOG for `AudioProbe` / `L1[78]CTRL`; paste the lines into the notes and move the device-test
row. (3) If the codec is silent with L18CTRL off -> milestone 2 task 2 (ACPM DCVDD enable, flag 0). Then
notes §4.3 (tone-generator beep to the 3.5 mm jack).

## 28. Restart point (2026-10-12, sub-agent worktree): M12 WiFi milestone 1 code-complete + built, not flashed

**Committed** (branch `worktree-agent-afe491e33df42f9e6`, on top of main 302c285; ported from origin/drivers by
copying files, no merge): (1/5) Exynos9820.h M12 PCIe0/WLAN constants + 7 PCIe0 memory-map windows; (2/5)
Beyond0LtePcieDxe with the review fixes (100 ms WLAN_EN, FILE_GUID ...5553, BEYOND0LTE_PCIE_WLAN 1, 0x68 progress
guard, status word 0x8FE0F00C = 'WLAN' cleared in SEC, 3 logged link tries, BAR sizing, chipid print); (3/5) DSDT
`Device (WLAN)` SAMS9825 with `_STA` on the status word; (4/5) `drivers/Exynos9820Wlan` KMDF probe (not NDIS) +
build-package.ps1 + `S10eDiag/esp/extra-wlan.cmd`, review fix (per-line DbgPrint); (5/5) notes/handoff.
Firmware DEBUG build exit 0 (FVMAIN_COMPACT 2752 B free); driver package built + test-signed.
**Artifacts:** `C:\Users\ilya\code\s10e-artifacts\wifi-m1\` (FD sha 4b8abc3f...0d815, package, cert, extra-wlan.cmd).
**Next order.** (1) Device session: follow `notes/m12-wifi-m1.md` "Device test procedure" (pack + flash boot,
stage the ESP, read UEFILOG `[PCIe]` lines, collect `out\last\wlan\`). (2) Record the endpoint ID (closes P2) and
BAR sizes. (3) Pull the firmware blobs per "Firmware blobs for milestone 2". (4) Milestone 2 = firmware download
(NDIS skeleton still on origin/drivers 5746999/cb6a90f). Build note: a worktree has no firmware/edk2; a junction
breaks GNU make's Trim lookup - build with $edk2 pointed at main's edk2 (notes/m12-wifi-m1.md).

## 29. Restart point (2026-10-12 night, unattended session - user away, phone in TWRP on the PC cable)

**Phone state.** BOOT = boot #91 (s10e-artifacts\boot91-v2.img, sha dee82084…): host-mode USB (combo PHY parked,
COM switch, fallback boost 1.2 A), charger mode (FDT lpcharge -> battery bar, WDT kick), stay.flg boot pair,
audio ID probe (0x69 guard), WiFi PCIe M1 (0x68 guard), BAT0, **BEYOND0LTE_WINDOWS_WATCHDOG 1** (unattended).
ESP: **no stay.flg** (unattended: every Windows boot collects and returns to TWRP), extra.cmd = extra-battstat.
Windows: S10eBattery (watchdog-kick build, DriverStore swap), Exynos9820Wlan m1 probe, buttons agent, s10ekeys.
**Validated today:** buttons (power screen/menu, Bixby taskmgr/rotate), USB host dock/keyboard/mouse/USB3 sticks at
HS (before boot only - parkinglot), USB KD (boot #78 image + BCD debug), charger mode, battery icon + charging in
Windows (charger WDT kick), BCM4375 alive in UEFI + Windows.
**In flight:** WiFi M2 (firmware download in the probe driver) and audio M1b (LDO18M + resets + IDs) on subagent
worktree branches; merge, build, test unattended.
**Before handing back to the user:** BEYOND0LTE_WINDOWS_WATCHDOG back to 0, put S10EDIAG\stay.flg back on the ESP,
remove extra.cmd. Battery floor: < 15 % -> park in TWRP 30 min (memory: unattended-battery-floor).
**Parking lot:** parkinglot/usb-host-hotplug-and-power.md, parkinglot/touch-double-tap.md. KD auto-role (device
when VBUS in, host otherwise; DBG2 only in device role) is the next debugging improvement.
**§29 update (2026-10-12 ~01:45).** BOOT = boot #92 (0f2cd533…: #91 + audio 1b). Validated unattended:
battery charging under Windows (charger WDT kick), audio 1b (both CS35L40 + CS47L93 IDs, LDO18M on), WiFi
milestone 2 (BCM4375 firmware 18.41.117 running, SEMCO module, driver 1.0.2.0 swapped into DriverStore, blobs in
C:\Windows\System32\drivers\bcm4375\). In flight (subagents): WiFi M3 (msgbuf + first ioctl), audio M2 (tone to jack).
**§29 update (2026-10-12 ~02:40).** BOOT = boot #93 (s10e-artifacts\boot93-v2.img, 77bfd211…: main c20e6c0 =
#92 + audio M2 tone, watchdog 1). Flash recipe: build-uefi.ps1 -Platform Beyond0LtePkg → pack-uefi-beyond0lte.ps1
-Ref s10e-artifacts\ref-recovery.img (untrimmed -> 18 255 872 B) → dd to by-name/boot + readback. DriverStore swaps
from TWRP: `umount /data` first (TWRP auto-mounts the Windows NTFS ro there), fsck.ntfs if mount is ro.
- Validated: WiFi M3 (msgbuf, MAC A8:DB:03:06:3C:EF, fw ver string), audio M2 digital path (FLL1 locked on
  MCLK2, OUT2 enabled, tone 2 s; HP2 EN_DONE 0 -> **user ear test pending**: wired headphones in before power-on,
  ~2 s beep just before Windows Boot Manager), battery path live (Windows re-queries, values track).
- In test: WLAN 1.0.4.0 (M4: events + escan, logs `[scan]` BSS lines and `SCAN n`), S10eBattery 1.0.0.3 (off line
  while OTG boost is on - the likely cause of "icon does not change on unplug"; needs the user: boot without
  cable, then plug/unplug, then read Hist00-15 in Enum\ACPI\SAMS9826\0\Device Parameters).
- Next: WiFi M5 = firmware resident for the device lifetime (DMA in device context, ReleaseHardware teardown,
  poll thread), then the NDIS/WDI question. Audio: wait for the ear test.
**§29 hand-back (2026-10-12 ~03:25) — unattended session over, interactive state restored.**
- BOOT = **boot #94** (s10e-artifacts\boot94-v2.img, 1a42cb25…, readback OK). It is main with
  BEYOND0LTE_WINDOWS_WATCHDOG **0**; audio M2 tone still on (BEYOND0LTE_AUDIO_TONE 1, for the ear test).
  ESP: **stay.flg back**, no extra.cmd, no WINRUN.FLG. Phone left in TWRP, battery 67 %.
- Windows drivers (DriverStore swaps): Exynos9820Wlan **1.0.6.1** (M3-M6 code, resident firmware) and
  S10eBattery **1.0.0.3** (off line while OTG boost).
- Validated this session:
  - WiFi M3: msgbuf + MAC A8:DB:03:06:3C:EF.
  - WiFi M4: the CLM blob was what escan needed; SCAN 0 finds 8-14 BSS on 2.4 + 5 GHz.
  - WiFi M5: resident firmware, rescan at t = 30 s. Needed common-buffer cache maintenance and a single
    poller.
  - M6 PBKDF2 self-test PASS.
  - Audio M2 digital path: FLL1 locked, OUT2 on, tone played.
  - Battery: driver → Windows path is live.
- **Needs the user:**
  1. Audio ear test: wired headphones in before power-on, ~2 s beep just before Windows Boot Manager. Then
     set BEYOND0LTE_AUDIO_TONE 0.
  2. Battery: boot without a cable, plug and unplug, then read Hist00-15 under
     Enum\ACPI\SAMS9826\0\Device Parameters.
  3. WiFi M6: Parameters\Ssid + Passphrase (REG_SZ, Services\Exynos9820Wlan\Parameters), reboot, read
     C:\Windows\System32\LogFiles\Exynos9820Wlan-run.txt for `JOIN 0`.
  4. WiFi M7 architecture: NDIS 802.3 adapter with a registry-configured network (fast) vs WDI/native WiFi.
- Unattended mode again = WATCHDOG 1 build + remove stay.flg (+ `rm WINRUN.FLG` before every reboot).

## 30. Restart point (2026-10-12 ~12:30, unattended session, user away)

**Phone state.** BOOT = **boot #101** (s10e-artifacts\boot101-v2.img, 6d7baccc…): main + AUD0 DSDT (SAMS9827),
UEFI tones OFF, BEYOND0LTE_WINDOWS_WATCHDOG 1, **BEYOND0LTE_CHARGER_BOOTS_ON 1** (a charger power-on boots on, so a
phone that powered off unattended comes back; validated: power-off from TWRP -> Windows -> TWRP). ESP: no stay.flg
(unattended loop), extra.cmd = extra-audio.cmd with audio-stage.cmd (staged audio bring-up). Battery 100 %.
**Validated today:** loudspeaker path (UEFI m3, mic SNR 44-67 dB), headphone tone (user + mic), battery icon (user),
charger boots on, RAM probe (all carved regions R/W from Windows; publishing them waits for the user - the edit was
refused by the session's safety classifier).
**In flight.**
- Windows audio (notes/d8-audio-win-m1.md): driver Exynos9820Audio (ACX). Run a1 (all stages at once) powered the
  phone off; stage A (no hardware) OK = endpoint "Speakers (Galaxy S10e Audio)" Started; restart-device crashed
  (D1, circuit re-add after remove) -> fixed 1.0.1.1; WASAPI open failed 0x80070032 -> 1.0.1.2 + PreserveSig
  diagnostics in audio-test.ps1 (stage B4 running). Next: stages C (amp init) and D (amps on) with the mic.
- WiFi (worktree branch worktree-agent-ae72ee1a2b1f2e987, notes/m12-wifi-m7.md): M7a data path + M7b host 4-way
  handshake (1.0.7.2) - associates with HomeX-5, but the firmware never completes rx (rx cpl w 0) -> rx-path audit
  running (notes/m12-wifi-m7-rx-audit.md). wifi-connect app built (s10e-artifacts\wifi-connect, tools/wifi-connect).
- CPU (notes/cpu-ram-plan-20261012.md): M4 cores differ in ID_AA64ISAR1 (no LRCPC) and MMFR1 (8-bit VMID) per
  TWRP dmesg; next = KD run with the M4s offered.
**Tooling.** scratchpad cycle.sh (flash optional, reboot, mic listen, wait TWRP, pull UEFILOG/last/WINPRAM marks),
deploy.sh (DriverStore .sys swap), tools/audio-listen/listen.py (narrowband mic detector, raw WDM-KS pin).
**§30 update (2026-10-12 ~14:05).** Since run audB5 every Windows boot hangs ~2 min after power-on (debug_history:
NORMAL -> ~126 s -> "DP INFORM3(12345670)" -> RECOVERY); nothing logged (System.evtx stops at Ntfs event 55
"corruption" from the B6 boot). Likely cause: TWRP-side DriverStore swaps ran fsck.ntfs on the dirty NTFS.
WLAN hidden (boot #103) did not help. KD (boot #104 + ESP BCD debug on, recoveryenabled No, firewall rule for
kd.exe UDP 50005) never connected - no KDNET adapter on the host. After the last KD try the phone did not return
to TWRP (unreachable since ~13:35). **User authorized a Windows reflash** ("just reflash it"). Plan when the phone is
in TWRP: S10eHarness transfer s10e-artifacts\win-w5 -> firmware/tools/s10e-provision-fresh.sh (UFS w34,
s10ediag-m16, touch-t2\Exynos9820Hsi2c, touch-hold, touch-t2\wdktestcert.cer) -> flash boot #105
(boot105-setup-nowdt-v2.img = main, WINDOWS_WATCHDOG 0) for Setup -> afterwards boot #101 (watchdog 1) + no stay.flg,
drivers re-added one at a time via ESP \S10EDIAG\install (no NTFS writes from TWRP). ESP BCD currently = debug on +
recoveryenabled No (s10e-artifacts? scratchpad bcdkd\BCD; original in bcdkd\BCD.before-kd).
**§30 GPU addendum (2026-10-12 ~17:00, phone still unreachable since ~13:35).** GPU work merged on main
(notes/gpu-panfrost-repos.md, gpu-odin-rom.md, gpu-windows-prior-art.md, gpu-wddm-mali-g52.md, gpu-m16-g2g3.md,
gpu-m16-g3don.md, gpu-m16-g9b.md; clones in C:\Users\ilya\code\mali incl. wddm-mali-g52 + mesa-26.2). Prebuilt images:
- boot105-setup-nowdt-v2.img: main@~14:30, WINDOWS_WATCHDOG 0, for Windows Setup after the reflash.
- boot107-loop-v2.img (main HEAD, watchdog 1, read-only GPU probe G3 runs every boot, GPU0 hidden unless G3D on).
- boot106-gpuon-v2.img (same + BEYOND0LTE_GPU_POWER_ON 1: pmucal G3D on + SMC 0x82000410 + ACPM 260 MHz, sha fc9f7c02…).
Order after the phone is back + Windows reinstalled: #107 loop boot (read GPU probe lines) -> #106 (power-on lines,
"ACPI GPU0 visible") -> install s10e-artifacts\mali-g9b via ESP \S10EDIAG\install (MaliG76Probe run log: RESULT PASS).
Rescue for a GPU-driver boot hang: flash #107 (GPU0 hidden -> driver not loaded); no NTFS writes from TWRP.

## 31. Restart point (2026-10-12, sub-agent worktree `worktree-agent-aaa55ccbfa329cc37`): M16 G10/G11 code + build, not booted

**Committed on the worktree branch (fast-forward of main d9b8572):** (1) GPU rescue path in firmware - a
`BEYOND0LTE_GPU_POWER_ON 0` image now ALWAYS hides DSDT GPU0, and ESP `\S10EDIAG\nogpu.flg` hides it on any image
(BootLogDxe writes 'NOGP' to progress word [4] at ReadyToBoot); so boot #107-style images are guaranteed rescue
images, and a GPU driver can be taken out of the boot from TWRP without NTFS writes. Firmware builds flag 0/1 exit 0,
nothing packed or flashed (scratchpad\BEYOND0LTE_UEFI-g10-flag{0,1}.fd). Note: boot106/107 images predate this - #107
hides GPU0 only if sboot leaves G3D off. (2) `drivers/MaliG76Wddm` (GPL-2.0, from wddm-mali-g52 maliwddm + malidod +
maligpu): default display-only KMDOD on the GOP fb; `Parameters\FullAdapter` 1 = full WDDM 1.3 adapter; `GpuEnable` 1
= + GPU half; INF UMD maliumd.dll. Package + m5test + ESP script in `s10e-artifacts\mali-g10` (sha256 in
notes/gpu-m16-g10.md). **Next:** after G9/G9b on the phone, the 4-step validation procedure in notes/gpu-m16-g10.md
(display-only first). Rotation is identity-only in this driver (follow-up).
- (~19:00) G10/G11 merged (drivers/MaliG76Wddm, display-only by default; HANDOFF §31 by the G10 agent) and G12 (Mesa
  26.2 maliumd.dll, s10e-artifacts\mali-g12). **boot106/107 REBUILT** from main 54c4f6e with the rescue path
  (\S10EDIAG\nogpu.flg on the ESP hides GPU0 on any image; flag-0 images always hide it): boot107-loop-v2.img
  sha 6f6396e2…, boot106-gpuon-v2.img sha b0a7629d…. Test order unchanged: reinstall -> #107 -> #106 -> mali-g9b
  (G9b) -> remove MaliG76Probe -> mali-g10 display-only -> full.flg -> gpu.flg (m5test).

## 32. Restart point (2026-10-12 ~19:25, unattended, user remote)

**What happened to the phone.** debug_history entry 717 (power-on 16:13 by the charger, LPM) records
"(S) OCP_B1,OCP_B5": after the KD boot (#104, ~13:20 local) the phone powered OFF on a sub-PMIC buck over-current
trip; the dtbo names those rails only s_vdd_buck1.. (consumer unknown). Logs saved to scratchpad
runs/pre-reflash-1612 (WINPRAM from 09:52 is decayed, nothing from the KD run).
**Reflash done (user-authorized).** S10eHarness transfer win-w5 EXIT 0 (19:21, s10e-artifacts\win-w5\transfer-20261012.log);
s10e-provision-fresh.sh with UFS w34 (8ea93ce8…), **s10ediag-m18** (s10eperf sweep/loop), touch-t2 Hsi2c, touch-hold,
wdktestcert. ESP: BCD pristine (e5d4a24e…, debug off), no stay.flg, no WINRUN.FLG, no extra.cmd, \S10EDIAG\install removed.
BOOT = #105 (c06b623d…, watchdog 0). Windows Setup started 19:22; background wait-for-recovery 2.5 h.
**Next order.** Setup done -> boot #109 loop (4f738af3…, #107 + UEFI UfsSpeed test) with extra.cmd = extra-ufsprobe2.cmd (m15 u2 read-only probe)
-> boot #108 (7a138c7e…, #109 + BEYOND0LTE_UFS_IOCOH_9820 1) with extra-ufsprobe2 again (A/B on iocoh) -> GPU
order of §30 (#106, mali-g9b, ...) -> drivers back one at a time via ESP install (battery-m4, WLAN 1.0.7.3, audio).
**§32 update (2026-10-12 ~22:30).** UFS: Windows driver now **w37** (live in DriverStore, sha 7f4f1bcd…; `.prev` = w36).
Large reads 20 -> 117 MB/s (notes/m15-ufs-throughput.md: device VPD 0xB0 optimal transfer 8 KiB made classpnp split
reads -> w36 clears it; per-read byte-wise evidence walk on the Device-memory bounce -> w37 limits it to boot).
Use Exynos9820Ufs-w37.sys for future reinstalls (provision script arg). Driver swaps go through
esp/extra-ufsswap.cmd (TWRP ntfs-3g refuses rw on the flushreset-dirty volume; never fsck). BOOT = #107. ESP extra.cmd
cleared. BT Q1 closed (parkinglot/bluetooth-d4c.md). Next: GPU order (#106, mali-g9b...), then drivers back one at
a time (battery-m4, WLAN 1.0.7.3, audio); UFS follow-ups (copy-back, >64 KiB transfers, writes) are lower priority.
**§32 audio addendum (2026-10-12 ~23:10, agent, no phone access).** Exynos9820Audio **1.0.1.4** packaged in s10e-artifacts\audio-a5 (sys sha 0763c194…; notes/d8-audio-win-a5.md). Suspected cause of GetMixFormat 0x80070032: the default (HwVolume 0) circuit had zero elements, unlike every ACX sample; now host -> volume -> mute -> bridge always, endpoint volume/mute applied in software in the DMA expansion (volume keys land on the element), 48k/16 default format + INF PKEY_AudioEngine_OEMFormat. Not device-validated. Test: notes/d8-audio-win-a5.md "Device test" (stage A first).

## 33. Restart point (2026-10-12 ~23:40, sub-agent, no phone access): M12 M9 WiFiCx client built

**Committed (main, ab86145..HEAD):** notes/m12-wifi-wificx.md (research, plan W1..W5, reviews, ESP test);
T1 shared WlanDl.c holds the D11 cores in reset before the firmware image write (warm-restart DL 6 candidate fix,
Parameters\D11Reset); T2 WlanCommon.c (report/settings/chip probe shared); T3 WlanMsg.c WiFiCx API (hooks, connect,
disconnect, keys); **drivers/Exynos9820WlanCx** (WiFiCx 1.0 + NetAdapterCx 2.2 + KMDF 1.33): W1 skeleton, W2 scan,
W3 data path, W4 connect/keys/disconnect; drivers/S10eDiag/esp/extra-wifi-cx.cmd. KMDF Exynos9820Wlan still builds.
**Package:** s10e-artifacts\wifi-cx3 (sys sha DD9A5148…, inf 8C081CEE…, cat B214EFB1…) - nothing device-tested.
**Next order:** notes/m12-wifi-wificx.md "ESP test" (install via extra-wifi-cx.cmd; reboot once if DL 6), user
connects from the flyout, collect; then fix the "M3 ... NOT_SUPPORTED" ids and the UNVERIFIED items the log
resolves (tx layout line, WDI version), then W5 (power/D3, roaming, interrupt, flowring delete).
**§32 update (2026-10-12 ~20:45 device time).** (1) Clean return to TWRP VALIDATED on boot #113 (fb7a0ed3…):
collect.cmd writes \S10EDIAG\torecovery.flg + `shutdown /r`; BootLog's OS-restart branch sees it -> recovery
(UEFILOG "clean Windows restart with torecovery.flg"). No more hard reset at the end of unattended runs, so registry
changes (driver installs, MaliG76Wddm Parameters) are flushed. ESP files pass both ways (token test). (2) Audio 1.0.1.4
(s10e-artifacts\audio-a5) CRASHES Windows ~50 s into a boot when loaded at boot (S10eDiag's bugcheck path resets
straight to recovery: debug_history REBOOTMODE4 64 s after NORMAL, so UEFI never saves that PRAM). Live install on the
install boot worked. Rescue: \S10EDIAG\noaudio.flg (boot #112+, AUD0._STA 0) - currently SET. (3) GPU: display-only
MaliG76Wddm works (Started, FbMatchesGop, Presents); FullAdapter 1 (GPU half off) and GpuEnable 1 both hang Windows
with a black screen and the 82 s watchdog does not fire; \S10EDIAG\nogpu.flg currently SET. (4) WiFi: 1.0.7.4 joined
the user's WPA2 network; WiFiCx driver (drivers/Exynos9820WlanCx, s10e-artifacts\wifi-cx3, HANDOFF §33) next.
**§32 audio addendum 2 (2026-10-13, agent, no phone access).** a5 refuted (same 0x80070032, endpoint volume too). Boot crash = 0xD1 from the run-log flush (64 KiB PAGED snapshot filled under a spin lock; audA5b PRAM BC D1, P1 page aligned) - fixed in 1.0.1.5; the same bug is in drivers/MaliG76Probe (MaliG76Probe.c:217-225), not fixed. 1.0.1.5 binds ACX 1.1 like the sample (1.0 = pre-release DDI) and logs every KS request + callback status. Packages s10e-artifacts\audio-a6 (ACX 1.1) and audio-a6-acx10 (fallback); test order in notes/d8-audio-win-a5.md "A6 device test".

## 34. Restart point (2026-10-13, sub-agent, no phone access): MaliG76Wddm full-adapter hang - trace + fixes built

**Committed (main, 7eee385..HEAD, notes/gpu-full-adapter-hang.md):** PRAM DDI trace in MaliG76Wddm (every DDI
entry/exit, registry trace values, 1 Hz heartbeat into WINPRAM+0x4000, 16 KiB, a slice nothing else uses - layout
inventory in the notes; saved by the firmware as part of \WINPRAM.BIN); static ranking of the hang: #1 ControlInterrupt
promised CRTC_VSYNC and nothing ever reported one (every vblank wait in dxgkrnl/DWM blocks forever: a live system,
WDT still petted, black screen - same shape as the original's run 6); #2 nothing could reach the panel (flips dropped,
and with the GPU half off MAP_APERTURE_SEGMENT never recorded the MDL, so every present was skipped); #3 the agent's
default 1800 s desktop wait makes a DWM failure look like a dead system. Fixes, each behind a Parameters switch
(default 1): SoftVsync (16 ms timer -> DxgkCbNotifyInterrupt via SynchronizeExecution), FlipBlt (flip = CPU blt into
the GOP fb), PramTrace; EnablePreemption 0 now set with the mode (extra-gpu-mode.cmd pass 1); INF 1.0.13.1;
ESP extra-gpu-update.cmd (live update in display-only mode). **Package:** s10e-artifacts\mali-g13 (sys 453C653A…).
**Next order:** the notes' "Device test steps": wait.txt 300 -> update live in display-only (must show 1.0.13.1)
-> full.flg via extra-gpu-mode.cmd -> read the PRAM slice (last DDI without exit, heartbeat, vsync count) whether
or not it is black. Open/UNVERIFIED: DMA-flip retirement vs the vsync's reported address; re-ranking of a hidden
installed device (hence the live update step).
**§32 audio addendum 3 (2026-10-13, agent).** a6: 0xD1 fixed (boots with the driver), endpoint still rejected; the a6 request log shows each endpoint-builder retry round ending in a host-pin DATAINTERSECTION size query with no follow-up (the log's statuses are dispatch returns, not completion). a7 (1.0.1.6, s10e-artifacts\audio-a7, sys 4ebb8d0c…) answers host-pin DATAINTERSECTION + PROPOSEDATAFORMAT in the driver (Parameters\KsOverride 1; 0 = ACX as in a6) and de-duplicates the request log. Test: notes/d8-audio-win-a5.md "A7 device test".
**§32 audio addendum 4 (2026-10-13, agent).** a7: intersection fixed, error now AUDCLNT_E_UNSUPPORTED_FORMAT (endpoint-builder rounds keep repeating). a8 = s10e-artifacts\audio-a8 (1.0.1.7, sys 35b16cdc...): hardware stages default OFF in INF + driver (user safety), quiet audio-test (0.03, ~10/6/16 %), user-mode diagnostics (endpoint property store formats, IsFormatSupported, one exclusive-mode tone, MMDevices registry + Audio event channels), ACX default-format and distinct-proposal logging. Test: notes/d8-audio-win-a5.md "A8 device test".
**§32 audio addendum 5 (2026-10-13, agent).** a8: the endpoint store lacks the engine DeviceFormat {F19F064D},0 and PhysicalSpeakers/FullRangeSpeakers that a working endpoint has; every AudioSrv call 0x88890008 without a pin create. a9 = s10e-artifacts\audio-a9 (1.0.1.8, sys bcedf29e...): KSPROPERTY_AUDIO_CHANNEL_CONFIG (stereo) on the volume element + INF PhysicalSpeakers/FullRangeSpeakers = 3; extra-audio-swap pass 2 now dumps MMDevices + audio event logs. Test: notes/d8-audio-win-a5.md "A9 device test".
**§32 audio addendum 6 (2026-10-13, agent).** a9: speaker keys reached the store, DeviceFormat still absent; the endpoint's format cache {74E4357A},2 is EMPTY (0 rates) on the phone vs filled on every working PC endpoint. a10 = s10e-artifacts\audio-a10 (1.0.1.9, sys 22f6196e...): driver answers the header-only PCM family probe (PROPOSEDATAFORMAT, 64 B) and offers DEFAULT mode besides RAW (Parameters\DefaultMode). Test: notes/d8-audio-win-a5.md "A10 device test".

## 35. Restart point (2026-10-13 ~02:30 local, unattended; device clock 23:30 GMT)
**Phone:** BOOT = #114 (boot114-gpuon-cleanret-v2.img, 85e4e0c5…: main + GPU_POWER_ON 1 + clean return + noaudio/nogpu
switches). ESP flags: none set except as noted per run (nogpu.flg / noaudio.flg hide GPU0 / AUD0). Windows drivers:
UFS w37, WiFiCx cx5 (**WiFi works**, user-confirmed on the laptop hotspot S10eTest, phone 192.168.137.90, TCP 135/445
answer, ping ~58 ms), battery (322ca95…), audio a10 1.0.1.9 (loads at boot, no crash; still 0x88890008 - endpoint
format cache partly filled, agent on a11), MaliG76Wddm 1.0.13.1 (display-only: desktop visible on webcam; FullAdapter
1: no hang any more (software vsync), screen dark because DWM drops its maliumd.dll device - agent on G13/UMD).
**Tooling learned today:** unattended runs end with a clean restart (torecovery.flg, #113+); driver swaps for
boot-loaded drivers go through hide-flag + pnputil stage + unhide (extra-audio-swap.cmd, extra-gpu-mode.cmd with
gpu\stage.flg); clean-restart runs save PRAM to \WINPRAM-RESTART.BIN (not WINPRAM.BIN); marker files in C:\S10eTmp
(audswap.flg, gpumode2.flg) survive a crashed pass and make the next run skip pass 1 - check marks first.
**In flight:** audio a11, GPU G13 (mali-g15), remote-control channel (S10eAgent outbound HTTP poll to the laptop).
**§32 audio addendum 7 (2026-10-13, agent).** a10: family probe accepted, DEFAULT mode added, cache {74E4357A},2 still empty; the four per-mode pin probes (create, RT buffer, latency, close) are the cache builder and fail on an ACX-answered RT property. a11 = s10e-artifacts\audio-a11 (1.0.1.10, sys 09ecf93a...): RT packets allocated like the ACX sample, GetHwLatency = sample values; new esp\ks-probe.ps1 (run by extra-audio-swap pass 2 as SYSTEM) prints ACX's answers. Test: notes/d8-audio-win-a5.md "A11 device test".
**§32 audio addendum 8 (2026-10-13, agent).** a11 stage A = Windows audio works (coordinator, run audA11c). Stage D stalled shared streams: RDMA0 STATUS[11:0] is the frame offset inside the current period and [27:12] the period start (16-byte units), not a free mod-4096 counter. a12 = s10e-artifacts\audio-a12 (1.0.1.11, sys 34557cfd...): ring position = [27:12] + [11:0] modulo the ring, torn-read guard; audio-test amplitude 0.1. Volume buttons: s10e-artifacts\s10ediag-m20 (S10eAgent + s10ekeys VK_VOLUME_*). Test: notes/d8-audio-win-a5.md "A12 device test".

## 36. Restart point (2026-10-13 ~09:30 local; context compaction requested by the user)
**Phone now:** TWRP, battery 75 %. BOOT = #114 (boot114-gpuon-cleanret-v2.img 85e4e0c5…: main + GPU_POWER_ON 1 +
clean return + nogpu/noaudio switches). ESP: \S10EDIAG\nogpu.flg SET (GPU0 hidden), no extra.cmd, no stay.flg,
wait.txt = 300. MaliG76Wddm Parameters 0/0 (display-only). Unattended loop = scratchpad cycle.sh (TWRP -> Windows ->
agent collect -> clean restart -> TWRP).
**Windows drivers installed:** UFS w37 (reads 20 -> 117 MB/s), WiFiCx cx5 (**WiFi works**, user-confirmed, hotspot
S10eTest / Home-X), battery 322ca95…, **audio a12 1.0.1.11 (speaker plays in Windows, endpoint volume/mute work, PC mic
heard the 2 s tone; stage D params AmpInit/AmpEnable/AboxEnable 1)**, MaliG76Wddm 1.0.26.1 (display-only desktop works,
seen on webcam), UMD maliumd.dll g29 (228d8332…) in System32, S10eAgent/s10ekeys m20 (Vol+/Vol- -> volume keys;
physical-button test by the user still pending).
**GPU full adapter (FullAdapter 1, GPU half off) status - notes/gpu-full-adapter-hang.md has everything:** no hang
(software vsync), mode set works (EDID synth, CPU-visible segment, CommitVidPn 0), D3D11 FL10_0 device creation OK,
DWM loads maliumd.dll, boot-console Blt presents reach the panel. DWM's own frames never do: flips (DMA, MMIO, shim- or
resource-owned primaries) all refused with 0xC01E0005 PRESENT_MODE_CHANGED (g22-g26); with NO_SCANOUT (patch 0023)
4 Blt presents accepted then stall (g27-g29). Every full-mode clean restart hangs (20-min fallback). **Next: mali-g30**
(fixed ETW capture: DxgKrnl + Dwm-Core + DXGI providers; logman update needs -o) -> read dwm-msgs.txt for DWM's own
reason. Run with: ESP gpu\{maliumd,libgallium_d3d10}.dll from mali-g30, extra.cmd = mali-g30\extra-gpu-mode.cmd,
touch gpu\full.flg + nogpu.flg + noaudio.flg; afterwards restore with the same script and no full.flg.
**Audio follow-ups:** amp pops at stream start/stop; headphone path in Windows; RDMA0 STATUS = [27:12] period start
+ [11:0] offset (fixed in a12). Install flow for boot-loaded drivers: extra-audio-swap.cmd (hide, delete old oemN,
stage, unhide) then extra-audio-stage.cmd for Parameters.
**WiFi follow-ups:** throughput ("slow", ping ~58 ms - poll-based driver), sleep/D3 resume, roaming.
**Other:** remote-control channel attempt stopped by the safety filter (not pursued; user may enable OpenSSH
themselves). Bluetooth D4c research + skeleton imported (parkinglot/bluetooth-d4c.md; bt_en = gpm8-0).
**Lessons:** marker files in C:\S10eTmp (audswap/gpumode2/audstage.flg) survive a crashed pass - check marks; a
killed ping returned to collect.cmd -> holdrestart loops; installs reset INF Parameters (stage after add); clean
restarts save PRAM to WINPRAM-RESTART.BIN; never fsck NTFS from TWRP (read-only mounts are fine).

**§33 update (2026-10-13 ~23:35).** BOOT = **#115** (s10e-artifactsoot115-wlanfix-v2.img, 358e52bd…: #114 recipe
+ 9ffee79 - the stay.flg WINPRAM save no longer zeroes the progress page; that zeroing hid XHC0/WLAN/GPU0 from
Windows after every hard reset). ESP: extra.cmd = extra-wifi-gfx.cmd (extra-wifi-state snapshot, then gfxperf),
stay.flg, gpuull.flg + bench.flg, noaudio.flg; BCD = original (KD off). Next: validate WiFi on #115
(notes/m12-wifi-wificx.md), then the power-menu "reboot to TWRP" failure and slow TWRP start, then GpuEnable 1.

## 37. Restart point (2026-10-14 ~03:50 local, unattended; goal: GPU smooth in Windows)
**Phone:** BOOT = **#116** (s10e-artifacts/boot116-fb10m-v2.img 740d97fe…: #115 + 10 MiB framebuffer reservation,
a1dd2e2). KMD **MaliG76Wddm 1.0.36.1** swapped into the DriverStore (tools/phone/kmd-deploy.sh; previous .sys kept as
C:\S10eTmp\kmd-prev). UMD in System32 = mali-umd-0052. Parameters GpuEnable 1 / FullAdapter 1, DWM on Panfrost.
ESP: extra.cmd = drivers/S10eDiag/esp/extra-gpu-h3.cmd, gpu\restride.flg set (arms Parameters\ScanoutStride 1088 for
the NEXT boot), gpu\{gpu,full,bench}.flg, noaudio.flg. Run h3l (03:35, baseline #116 + KMD 36) in flight.
**H4b plan (notes/gpu-hw-path.md has every step):** h3m = first boot with ScanoutStride 1088 + UMD 0053 (put in ESP
gpu\umd-new\): the KMD widens DPU G0's source line to 4352 B (RestrideStep 0 in trace.reg = done), the UMD blits DWM's
frame into the scan-out on the GPU (tlog "scanout:" lines). Check: RestrideStep, tlog, DWM fps in summary, webcam.
Back out: delete gpu\restride.flg (next-but-one boot is GOP layout again), MALIKM_NO_SCANOUT=1, or the old UMD.
**Then:** windowed D3D presents never reach DWM (d3dtri cube: 4 DWM frames per 10 s), tex test crash (WER dump armed),
DVFS/clock + thermal, stability (H5).

## 38. Restart point (2026-10-14 ~05:30 local, unattended; goal: GPU smooth in Windows)
**Phone:** BOOT #116 (10 MiB fb reservation). KMD **MaliG76Wddm 1.0.37.1** in the DriverStore (CddDwmInterop cap;
C:\S10eTmp\kmd-prev = 1.0.36.1). UMD **mali-umd-0061** in System32 (Mesa patches 0053-0061). Parameters: GpuEnable 1,
FullAdapter 1, ScanoutStride 1088 (re-armed every boot by extra-gpu-h3 while ESP gpu\restride.flg exists).
ESP extra.cmd = drivers/S10eDiag/esp/extra-gpu-h3.cmd (md5 293fe153..., cubes in blt + flip model, ETW cube trace,
gdi phase), gpu\d3dtri.exe = d3dtri-5.
**State (notes/gpu-hw-path.md, runs h3l-h3v):** zero-copy scan-out works (KMD re-stride + GPU blit present): DWM ~41
fps at logon, keeps up with a 21/s GDI load; windowed D3D apps reach the panel through the GDI hand-off (0061) at ~30
fps for a 540x1140 window; Explorer's multi-device Render failures fixed (0060, binding + screen per device); tex
sampling test PASS (0058); 0 GPU faults in every run. Tools: tools/phone/kmd-deploy.sh, umd-deploy.sh, pull-run.sh.
**Next:** visual confirmation of the cube on the webcam; readback cost of the GDI hand-off for big windows (CPU-cached
staging); DVFS/clock + thermal; long soak (H5); the open question why DXGI windowed presents never reach DWM natively.

## 39. Restart point (2026-10-14 ~12:15 local, unattended after an interactive look by the user; goal: GPU smooth)
**Phone (interactive config, user's request "no debugging modes or auto reboots or your special flags"):** BOOT =
s10e-artifacts/boot116-gpuon-nowdt.img (0c728578…) = HEAD firmware built with the two build-only defines
BEYOND0LTE_WINDOWS_WATCHDOG **0** and BEYOND0LTE_GPU_POWER_ON **1** (both reverted in the tree after the build; the
committed defaults are 1 and 0 - **any rebuild for GPU work must set GPU_POWER_ON 1**, the first nowdt image forgot it
and Windows ran on Basic Render). ESP: stay.flg, extra.cmd = extra-ssh-install.cmd (idempotent: sshd + GPU Check copy),
noaudio.flg removed (audio + HW GPU coexist), WINRUN.FLG left by the firmware. MALIKM_TGSI_DIR removed, BCD debug off.
KMD **1.0.38.1** (DriverStore; .37 kept beside it and in C:\S10eTmp\kmd-prev), UMD **mali-umd-0064** (System32;
*-0063.dll kept beside). Parameters GpuEnable 1, FullAdapter 1, ScanoutStride 1088 (persists in the registry).
**Remote access:** `ssh s10e` (PC key ~/.ssh/s10e_ed25519, user S10e = admin, hotspot only; notes/phone-ssh.md).
Big files: `sftp` (`get /C:/path`), not scp (stops at 200 KB). Writes into the DriverStore need SYSTEM: one-shot
scheduled task (C:\S10eTmp\kmdswap.cmd pattern). UMD/KMD can be swapped live by RENAMING the in-use file first.
Desktop-session programs: run-as-user.ps1 (C:\S10eTmp). Reboot: `ssh s10e shutdown /r /t 5` (stay.flg -> back to
Windows). Screen: s10ediagx.exe dumpmmio 0xCA000000 9922560 + pitch 4352.
**Fixed today:** panel shear (H4b never latched: KMD 38 commits GLOBAL|WIN0-5, verifies the latched DPU_DMA copy at
+0x810, LatchedBy 1); Direct2D text/icons/bitmaps (Mesa 0064: TGSI texel buffers + untyped VS inputs); Settings and
the taskbar render fully (scan-out dumps). User-facing: GPU Check.exe on the desktop.
**Open, in order:** memory (dwm 334 MB / explorer 346 MB private 2 min after boot - leak or overhead? memwatch
running), a soak with 0064/38, display rotation (KMD identity-only), GDI hand-off cost, thermal (notes/thermal-research.md
stage 0), brightness (notes/brightness-research.md), stereo speaker (Parameters\Stereo 1, untested), slow TWRP start.
To go back to unattended reboot loops: flash boot116-fb10m-v2.img (watchdog 1), rm stay.flg, extra.cmd = a run script.

## 40. Restart point (2026-10-14 ~17:10 local, unattended; user priorities: brightness, then landscape, then thermals)
- **Phone state:** firmware `s10e-artifacts\boot116-gpuon-nowdt-d9.img` (1ed69ae7..., = HEAD 3d32793 + build-only
  WINDOWS_WATCHDOG 0 / GPU_POWER_ON 1; adds read-only UEFI DSIM snapshots) on by-name/boot (readback verified);
  Windows 22631.6199 (KB5068865 installed itself 15:45; **Windows Update now disabled**, `tools/phone/wuoff.cmd`,
  notes/phone-ssh.md); KMD **1.0.42.2** (sha256 3CED0E5B..., 41 kept as C:\S10eTmp\kmd41.sys, also DriverStore
  MaliG76Wddm.prev = 42.1); UMD 0066; S10eDiag with boot DSIM snapshot (956448f); s10ediag CLI with `sample` + PA guard
  at C:\S10eTmp\s10ed3.exe (System32 copy is old). OfferRotation 0. Screen saver blank 120 s.
- **Brightness works** (MILESTONES G14 doing): root cause = sboot leaves DECON_F in VIDEO operation mode while DSIM is in
  command mode -> DSIM never reaches a frame boundary, DCS commands never leave. KMD panel.c switches DECON to command
  mode (as Samsung's decon_reg_init_probe does), then the brightness interface v2 maps 0-100 % to AOR (B1) between
  sboot's 0x0014 and 0x08D4. Windows sees it (WmiMonitorBrightness Active, levels set 100/30/75 reach the panel).
  notes/brightness-research.md D6-D10 + KMD sections.
- In flight: idle dim. powercfg VIDEODIM 60 s, dimmed 30 % (relative), display-off policy 3c0bc021 changed 0 -> 600 s
  (sleep/hibernate policies untouched); dim had not fired before the policy change; post-reboot retest running.
- Next order: (1) user visual check of the slider + 3rd boot -> G14 PASS; (2) idle dim result; (3) panel off/on for
  display-off (DCS 28/10, 11/29) in MwSetPowerState - then the screen saver can go; (4) hardware brightness keys
  (later), light sensor (much later); (5) landscape (notes/gpu-hw-path.md rotation attempts); (6) thermals.
- Rules learned today: cube fps is void if scrnsave.scr ran (real ~28-30); check frozen screens yourself (dump, launch,
  dump + DSIM status); cmd chains over SSH: run steps as separate ssh calls.

## 41. Restart point (2026-10-14 ~20:25 local, session end; user: "good enough, mark this as done")
- **Done this session (after §40):** alpha1 + alpha2 tags (local, not pushed); brightness fade (KMD 1.0.43) and boot
  brightness hand-off (UEFI sets the last Windows level, 50 % cold; KMD 1.0.44.1 reads/writes WINPRAM progress words
  [6] 'BR' / [7] 'A0'); landscape/rotation (Mesa 0067 stale GPU-presented flag; 0068 holds the scan-out 700 ms after an
  orientation change; KMD Parameters\OfferRotation = 1 on the phone, committed default still 0); touch driver
  6.0.5058.6 (event-count mask, no all-up on SPB errors, precise scan time; user: "feels perfect"); tablet posture
  (PriorityControl\ConvertibilityEnabled 1) + touch keyboard (TouchKeyboardTapInvoke 2, IE UseSWRender 1 = EdgeHTML
  software render works around the composition frame-gate hang); s10etap synthetic touch + s10etouchlog test UI;
  Windows Update disabled; boot screen (shim opaque-black fill, SEC + DXE draw the EXYNOS WOA / NTDEV / "Powered by
  Windows" splash; checkpoint bands, BDS countdown + GraphicsConsole removed); **TWRP patched** (one-time recovery
  write approved by the user: /data not auto-mounted, start 133 s -> 36 s; original in s10e-artifacts/twrp).
- **Phone state:** boot = s10e-artifacts/boot116-gpuon-nowdt-clean3.img (d85b7391..., HEAD 3883ebf + build-only
  WINDOWS_WATCHDOG 0 / GPU_POWER_ON 1); recovery = twrp/recovery-nodata.img (c7b198c0...); KMD 1.0.44.1 (4fae4048...,
  .prev = 1.0.43.1, C:\S10eTmp\kmd41.sys); UMD 0068 (System32, older *-00NN.dll kept); touch 6.0.5058.6 (touch5.sys =
  previous); S10eDiag fa76c6af...; MALIKM_TGSI_DIR removed; idle dim VIDEODIM 60 s / 30 % set but never fires (parked);
  display-off policy 0; screen saver blank 120 s.
- **Open / next candidates:** thermals (user's next big item; no clock increases before it), composition frame-gate
  hang (EdgeHTML CanPaintNow / Settings / Terminal; notes/touch-keyboard.md), ShellExperienceHost gets softpipe in its
  AppContainer, ~1 s UFS read stall under load (notes/m8-ufs-hc.md), touch SPB >250 ms stalls, Defender real-time
  protection (user to switch off via Windows Security - Tamper Protection), stereo/earpiece speaker, Android-like shell
  (parkinglot/android-like-shell.md), make OfferRotation default 1 in the INF/driver.

## 42. Restart point (2026-10-15 ~01:40 local, unattended; goal: all 8 cores in Windows; READ FIRST: notes/m14-m4-summary.md)
- **State:** the M4 big cores are NOT working yet. Windows starts all 8 (KeStartAllProcessors returns, all answer IPIs),
  then any boot where Windows RUNS ON an M4 dies within ~1 min (two M4s: silent all-CPU hang, "HOST cannot
  communicate"; one M4 (PARK 0x7E): fast reset loop every ~69 s, no bugcheck). Powered-but-unused M4s are fine
  (the "not offered" runs were clean harness restarts - see the CORRECTION entry in notes/m14-smp.md).
- **Ruled out (with data):** GPU KMD, audio (inconclusive), BIG 520 MHz DVFS, WFE wake (Windows spins with YIELD),
  CNTFRQ (26 MHz on all), GIC CPU interface numbering (M4 = 6/7), LLC (off by default; enabling it like Linux hangs
  Windows at KD init), cache geometry (CTR/DCZID identical), cached snoop coherency (100k ping-pongs, 0 misses), DVM
  broadcasts on the M4 (UEFI), CPU_ON history (CPU_OFF works). Remaining ISA gaps on the M4: LRCPC, HPDS2.
- **Tools built this session (committed):** kdrun fixed and extended (Kits dbgeng by path, INFINITE wait + timer thread,
  shared log, initial-break commands file, 'every' mode, per-break commands file, exit after TWRP reset);
  tools/phone/watchall.sh (state-change monitor; address in gitignored local/phone.env); park-stub diagnostics behind
  flags: DVM self-test, CNTFRQ, poll counter, GIC if mask, ID registers, WFI / CPU_OFF sleep modes, cached coherency
  ping-pong; BEYOND0LTE_SMP_OFFER_MASK; TouchPower BIG 520 MHz FREQ_REQ (default on), SCI LLC setup (default off),
  UEFI soak (default off). All test flags default to the old behaviour; defaults: PARK 0x3E (6 cores).
- **Phone:** boot = s10e-artifacts/boot-m4one1.img or later test image (KD image family: WATCHDOG 0, GPU_POWER_ON 1,
  USB_KD_DEVICE 1); BCD debug ON (KDNET USB, port 50005, key 1.2.3.4). ESP: stay.flg restored; nogpu.flg and
  noaudio.flg ADDED this session (remove for normal use). Daily image before this work: boot116-gpuon-nowdt-clean3
  (d85b7391) - restoring it needs BCD debug off (or a KD-capable image).
- **Next candidates:** what resets the SoC quickly after an M4 fails (Samsung uH at EL2 / EL3 - read
  reset_reason_extra_info right after a one-M4 death, without an intermediate reset); M4 under Windows with fewer
  features (e.g. TCR/SCTLR bits Windows sets from boot-CPU ID registers: HPD/HWU from HPDS2, hardware DBM/AF);
  KD bp on processor 6 exception paths (KiSystemServiceHandler / KiSErrorException / KiUndefinedInstructionException).
- **§42 update (01:52):** ONE M4 offered (PARK 0x7E) also dies: a fast reset loop (~69 s period, KD "restarted
  without notifying the debugger"), with stopping breakpoints on every Windows fatal path (SError/FIQ/fatal
  exception/HAL reboot/KeBugCheck2/WHEA) never hit => the reset is external to Windows (EL3/uH). 2 MiB guard
  reservations around every carve-out and DRAM edge do not help (prefetch theory out). M4 ID registers: CTR/DCZID
  equal to the A55; missing LRCPC and HPDS2. Phone restored: boot-kd6.img (6 cores), ESP stay.flg only.
  EL2 trap timing done (m4trap1): uH traps none of TTBR0/TCR/SCTLR/MAIR/CONTEXTIDR on any core. Next: read
  Samsung debug-snapshot memory (0xFD900000.., carved out) from UEFI after an M4 death for EL3's register dump;
  find which Windows-programmed EL1 setting differs on the M4 (TCR HPD/HWU from HPDS2, hardware DBM/AF).

## 43. Restart point (2026-10-15 ~12:20 local; M4 work PARKED by the user; goal "all 8 cores" not reached)

- **Read first:** notes/m14-m4-summary.md (updated), then notes/m14-smp.md from "## PC sampler" to the end.
  A ready-made brief for another model: notes/m14-m4-brief-other-model.md.
- **Established today (all with runs in m14-smp.md):**
  - CoreSight/EL3 view closed: DBGAUTHSTATUS 0xAA on all cores; EDPCSR reads 0; s2d/arraydump empty. The reset-surviving
    EDPRSR timeline shows every core powered, none reset or halted, at the death.
  - Windows uses hardware Access flag (TCR.HA 1); Samsung's Linux never does (CONFIG_ARM64_HW_AFDBM off). With HA forced
    off on every core from boot (BEYOND0LTE_NT_NO_HWAF) Windows runs fine on 6 cores, but 8 cores STILL die -> not the cause.
  - A busy cached M4 (stub flag 6: copy loop + DC CIVAC / IC IALLUIS / TLBI VMALLE1IS) runs 16+ min next to a healthy
    6-core Windows -> the trigger needs Windows running ON the M4. Untested: interrupts/WFI with IRQs unmasked on the M4,
    EL0 traffic, Windows' MMU attributes.
  - Timer PPIs verified from the stock DT (GSIV 29/30/27/26, 26 MHz, Linux uses the physical timer).
- **Committed today (switches default off unless noted):** PC sampler (stub flag 5, BEYOND0LTE_PARK_PCSAMPLE_MASK,
  \PCSAMP.BIN, tools/phone/pcsamp.py); CoreSight debug bases in Exynos9820.h; BEYOND0LTE_NT_NO_HWAF (BootLogDxe);
  cached stress (stub flag 6, BEYOND0LTE_PARK_STRESS_MASK); **safeguard ON by default: BEYOND0LTE_STAY_MAX_DEATHS 2**
  (with stay.flg, the 2nd unexplained Windows death in a row goes to TWRP; verified on device). The RAM-dump idea was
  dropped at the user's request (prep script reverted).
- **Phone (parked):** boot = boot-kd6.img (7810a0bd, 6 cores, KD image, WATCHDOG 0), ESP stay.flg. BCD bootdebug was
  turned ON today for a winload test; it is switched back OFF as part of parking (see the update line below).
- **Open decisions for the user:** the phone sshd firewall rule only allows 192.168.137.0/24 (old hotspot) - SSH from the
  home Wi-Fi is refused; widening it was blocked and needs the user. Test images built from now on include the safeguard;
  boot-kd6.img predates it and has the watchdog off (a hang there stays dark until a force restart).
- **Next order when resumed:** stub flag 7 = flag-6 stress + EL1 physical timer IRQ (GSIV 30) through the M4's own GIC CPU
  interface + WFI with IRQs unmasked (details in the brief); soak with Windows on the 6 small cores. Dies -> bisect timer /
  WFI / SGIs. Survives -> EL0 round trips, then Windows' MMU attributes.
- **§43 update (12:24):** parked state reached: boot-kd6.img (7810a0bd) flashed, BCD bootdebug back OFF (bcdedit output
  "bootdebug No", kernel debug still Yes as the KD image needs), extra.cmd back to the ssh-install original, ESP
  stay.flg on, rebooted into Windows.

## 44. Restart point (2026-10-16 ~11:55 local; Wi-Fi throughput DONE per user; next session: WDDM2 rewrite)
**Committed (tree clean):** Exynos9820WlanCx 1.0.10.26 installed on the phone (c446aa1 = the fix: firmware "mfp" set from
our RSN IE before the join - the PMF mismatch had blocked Block Ack with PMF-capable APs; d098f68 = last). Measured:
2.4 GHz Home-X 34-41 Mbit/s (was 4-8), 5 GHz 221-282 Mbit/s single stream at ~23 % CPU. Diagnostics: Parameters\DbgDump
(section | var: | set: | pm: | stainfo | cnt | bw40 | country:XX), counters/macstat80 every ~1 min. Details:
notes/wifi-throughput.md. BT 1.0.0.16 unchanged. alpha4 tag = baa0caf.
**Open, not in flight:**
- **Silent freeze every boot at ~2832 s (47.2 min) after the SoC reset** (13 so far, predicted to the second; tracks the
  raw counter, not Windows boot time; NOT WlanCx (froze with it removed), NOT BT, NOT a memory leak, NOT the 2^36 counter
  wrap; KD break-in fails - the hang takes the USB/KDNET path too). notes/thermal-clocks.md. Next: boot image with the m14
  EL3 PC sampler or the DSS/scan2dram dump to see the hung cores. Until then every session dies after ~47 min.
- 40 MHz in 2.4 GHz: this fw keeps HT cap 20 MHz (bw_cap, mimo_bw_cap, country tried); RSDB wlc1 untested.
- Phone leftovers: firewall rule S10eUdpTest (UDP 9998), schtasks band5/band24/wifiexp/wlanswap (one-shot, harmless).

## 45. Restart point (2026-10-16 ~19:30 local; audio session: speakers, trim app, 3.5 mm jack DONE)

Committed and device-verified this session (notes/d8-audio-speakers-20261016.md, notes/d8-jack-windows.md):
- Exynos9820Audio **1.0.1.36** installed. Speakers: both amps at 0 dB digital, per-amp analog gain; balance set by ear
  is the default (earpiece gain 12, loudspeaker atten 40 = -5 dB). Parameters RcvAtten/SpkAtten/RcvGain/SpkGain.
- tools/S10eSpeakerTrim (C:\Program Files\S10eSpeakerTrim + Start menu "S10e Speakers"): live trim over the private KS
  property AUDPROPSETID_SpeakerTrim (AudControl.h, Control.c), works unelevated.
- 3.5 mm jack: CS47L93 over spi_2 from Windows (Codec.c, pages mapped without _CRS), cs47l92 patch + 32 kHz + FLL1
  (CodecTone.c), jack detection polled (CodecJack.c), headphone path (CodecHp.c) with the **codec as AIF1 clock
  master** and ABOX UAIF0 slave (the stock ABOX-master + FLL2 path never locks: AP BCLK not seen by the codec - open,
  kept behind Parameters HpMaster 0). Separate "Headphones" endpoint (Headphone0) with jack presence; speakers stay
  listed; per-endpoint volume; Windows restores Headphones as default on replug once chosen. Phone registry:
  CodecEnable 1 (needed; INF default 0), HpRoute 1, HpMaster 1 (default), Stereo 1, AmpInit/AmpEnable/AboxEnable 1.
- Driver log keeps the previous load as C:\Windows\System32\LogFiles\Exynos9820Audio-prev.txt.
- tools/phone/debloat.ps1 run on the phone (OneDrive + Copilot appx + Copilot desktop removed, svchost grouping,
  27 services disabled) - re-run after any clean reflash. Desktop .cmd launchers removed (desktop.cmd on the ESP).
- Parking lot: Galaxy Controller (one C# UI for all our drivers; MILESTONES).

Freeze (notes/thermal-clocks.md #16-#19): still every ~47 min; **KDNET debug is OFF in the phone BCD** (left off per
user) and it still freezes, now at raw ~2791-2802 s instead of 2832 s; the "freeze" is a **fast reset ~7 s after the
last sample**, not the 82 s watchdog. No scheduled task near 47 min. Next: Samsung reset reason for that path from TWRP;
EL3 PC sampler / DSS image needs a boot flash (user's call).

Open / next: USB OTG hot-plug (parkinglot/usb-host-hotplug-and-power.md, not started); mic capture endpoint (jack mic
+ DMICs) not started; why the ABOX-master UAIF0 BCLK does not reach the codec.

## 46. Restart point (2026-10-16 ~20:50 local; user goal: WDDM2 + GPU-accelerated UI/browsers/video + HDMI; unattended)
**Committed:** alpha5 tag (local, 1e5df58). 47-minute reset FIXED: Exynos9820Ufs w38 (25b6e23; w37 minus the
W31 45-min watchdog-feed ceiling; installed on the phone, validated past raw 2966 s) - notes/thermal-clocks.md.
**WDDM 2.0 (G15):** physical-mode 2.0 adapter starts (dxdiag "WDDM 2.0"). Load gates: DRIVERCAPS.WDDMVersion 0x2000,
CalibrateGpuClock + SetStablePowerState (Parameters\Wddm2 = 3), Offset0 on the primary path; handle lookups via
DxgkCbAcquireHandleData. KMD 1.0.58.1 in the phone's DriverStore (sys 453a6236); WDDMVersion defaults to 0x2000 when
Wddm2 is on. UMD = Mesa 0069 (residency: paging queue + MakeResident) staged in System32 (maliumd.dll f8821c81,
previous kept as .prev). In flight: first persistent boot with Parameters\Wddm2 = 3 (set 20:47).
Rescue: ssh `powershell -File C:\S10eTmp\w2set.ps1 -Wddm2 0 -Reboot`; no ssh -> TWRP -> ESP \S10EDIAG\nogpu.flg.
Probe tooling (scratchpad): probe7.ps1 (volatile Probe key + restart-device), unprobe.ps1, w2check.ps1, kmdswap.cmd
(SYSTEM task "kmdswap"), umdswap.cmd, espget.cmd (ESP is mounted at S:), WINPRAM decode = dd + strings (KMD trace
+0x4000, S10eDiag ring +0xA000 has "BC <code>" lines). References cloned to C:\Users\ilya\code\wddm-ref (virtio-rs,
amdgpu-wddm read-only licence, MS docs + DDI docs).
**Next:** make DWM compose on the 2.0 adapter (S3/S4), then Terminal tracked fence; G16 chrome://gpu on FL10_0;
G17 needs D3D11.1 video DDI + FL11_0 in the UMD (notes/gpu-browser-video-research.md); G18 HDMI.

## 47. Restart point (2026-10-16 ~23:10 local; goal WDDM2 + GPU UI/browsers/video + HDMI, unattended)
**Committed:** KMD 1.0.60.1 (phone: Parameters\Wddm2 = 5 = WDDM 2.0 mixed GpuMmu, DirectFlip = 1) - desktop on the Mali
via DWM, D3D11 + monitored fences work, HWND flip-model swap chains work. UMD = Mesa 0069-0073 (residency, D3D11_0 ->
WDDM2_0 UMD DDI at FL 10_0, DXGI 1.4, redirect switch, callback logging); phone has mali-umd-0073 in System32.
Debug kit: tools/phone/wddm2 (wddm2dbg.sh + README; traces land in traces/, gitignored). Freeze: fixed (UFS w38).
**Open blocker (S4):** composition swap chains (Windows Terminal, DirectComposition/browsers) need DXGI redirection;
d3d10umd returned DXGI_STATUS_NO_REDIRECTION - D3D10UMD_REDIRECT=1 fixes the tracked fence, but DWM then loses its
devices repeatedly while redirected surfaces are live (no failing call of ours) -> redirect OFF by default.
**Next:** understand DWM's device loss with redirected surfaces from our side (KMD trace of Present/OpenAllocation
per device, shared-allocation flags, the DXGI Blt/Present DDI path the app takes when redirected); then browsers
(chrome://gpu, G16), video DDI (G17), HDMI (G18). Notes: notes/gpu-umd-d3d11.md, notes/gpu-wddm2-design.md.

## 48. Restart point (2026-10-16 ~23:55 local; goal WDDM2 + GPU UI/browsers/video + HDMI, unattended)
**Committed:** G15 S4 reached - Windows Terminal renders natively on the Mali. Mesa 0074 (MSAA, FL 10_1 task F1),
0075 (shim lists opened = other-process buffers only when the batch uses them, as READS: the DWM device-loss root
cause), 0076 (DXGI redirection default; D3D10UMD_NO_REDIRECT=1 diagnosis only). Phone: KMD 1.0.60.1 (Wddm2 = 5),
mali-umd-0076 (maliumd 107bdb1e) in System32, machine env MALIKM_TGSI_DIR on (UMD logs in C:\S10eTmp\tlog;
`wddm2dbg.sh umdtrace -Off` to stop). New AGENTS.md rules 11 (Windows internals: document in
notes/skipped-windows-internals.md, don't look), 12 (native only, no per-process flags as solutions), 13 (stuck ->
MS docs first).
**G16 state:** Edge's GPU process needs GLES3 from ANGLE = D3D FL 10_1 (FL 10_0 -> ES2 -> GPU process exits). Plan
F1-F5 in notes/gpu-umd-d3d11.md; F1 done (d3dmsaa passes). Diagnostic run with ANGLE allowES3OnFL10_0 (not a solution)
got past Skia after F1; its later crash was the tracked fence, now fixed by 0076.
**Next:** F2 SM 4.1 opcodes (LOD, GATHER4, SAMPLE_POS, SAMPLE_INFO) in ShaderParse/ShaderTGSI, F3 cube arrays, F4
per-RT blend check, F5 report pipeline level 10_1, then Edge with no flags (tools/phone/wddm2/browsertest.ps1). Then
G17 video DDI, G18 HDMI. Follow-ups: d3dmsaa over all RT formats x 4/8/16 (R32/R16 families answered from depth).

## 49. Restart point (2026-10-17 ~02:00 local; goal WDDM2 + GPU UI/browsers/video + HDMI, unattended)
**Committed:** G16 state - Edge runs natively on the Mali: ANGLE D3D11, GLES 3.0, vs/ps 4.1, Canvas/Compositing/
WebGL/WebGPU hardware accelerated (edge://gpu + DevTools SystemInfo, tools/phone/wddm2/cdp_gpuinfo.py). Mesa 0074
MSAA, 0076 redirection default, 0077 SM4.1 opcodes, 0078 pipeline level 10_1, 0079 no-stderr fail-fast fix, 0080 SM4.1
fixes (+ panfrost TEXTURE_SAMPLES sysval), 0081 allocation list grows on demand. Phone: mali-umd-0081c (maliumd
fa2109d9) in System32; machine env MALIKM_TGSI_DIR still on (umdtrace -Off to stop); WER LocalDumps for msedge.exe on
(werdump.ps1 -Exe msedge.exe -Off). Known limits: Chromium GPU rasterization blocklisted for our vendor id (entry 122),
Graphite needs FL12, D3D lod on Bifrost wrong (d3dsm41 7/8), WebGL MSAA off (ANGLE stops at our 2x = 0).
**Next:** G17 video (MFC decode integrated with D3D11 / Media Foundation), G18 HDMI. Optional: user check of real
browsing smoothness.

## 50. Restart point (2026-10-17 ~04:40 local)
**Committed since 49:** Mesa 0082 (AppContainer shell processes - Start menu / Search / Quick Settings - could not open
"NUL": softpipe fallback + fail-fast on a dead stderr; now Panfrost; user confirmed Start + Quick Settings open),
machine env MALIKM_TGSI_DIR switched OFF (never leave it on), G18 H1 (S10eBattery 1.0.0.5 reads the max77705 USB-C
block, read-only; usbc.ps1), parking lot FL 11_0 (parkinglot/fl11-chrome-video.md), G17 = UMD DXVA bridge for MF
apps (user decision; Chrome needs FL 11_0), panelon rescue tool, KMD 1.0.61.1 panel-off owner watch (BUILT, NOT
INSTALLED: C:\Users\ilya\code\s10e-artifacts\maliwddm-1.0.61.1, install with wddm2dbg.sh kmdswap ...sys + reboot).
**In flight:** the phone is finishing a Windows servicing restart ("Updates are underway", TiWorker); the panel was
dark because s10ekeys' screen-off was killed by the shutdown - relit with panelon.exe. Do NOT hard-reset while
TiWorker runs. User said "forget HDMI for now" (G18 H1 second read with the hub waits).
**Next:** after the restart: confirm desktop + Start/Quick Settings on the Mali (sandboxlogs.ps1 shows no softpipe
lines), kmdswap 1.0.61.1 + test (s10ekeys screen off, kill it -> panel relights), then G17 DXVA bridge.

## 51. alpha6 tagged (2026-10-17 ~05:35 local, local tag only like alpha5 - not pushed)
After the Windows update restart: KMD 1.0.61.1 installed + validated (panel-owner watch), 20-min soak clean (DWM alive,
0 crashes), d3dmsaa PASS, d3dsm41 7/8 (lod gap), Terminal, composition chain, Edge YouTube, Start menu. Tag message =
feature list + known limits. Next (user directive, memory clocks-directive-20261017): S10ePower C1 (AllowSend +
AllowTmu -> temperatures), C2 frequency requests, touch driver 6.0.5058.7 + governor, C5 max-clock tests. S10ePower
service is created (start= demand, read-only so far). HDMI waits for the user.

## 52. Restart point (2026-10-17 ~08:00 local)
**Committed since 51:** G19 dynamic clocks (MILESTONES line): S10ePower (C:\S10eTmp\S10ePower.sys, service auto,
AllowSend=1 + AllowTmu=1 in its service key) - temperatures + FREQ_REQ + thermal guard; touch driver 6.0.5058.7
(installed via pnputil); S10eGovernor service (C:\S10eTmp\s10egovernor.exe, auto, depends on S10ePower; Parameters =
ceilings 1950000/2314000/702000, boost 1742000/2106000/433000, EffLittle 1456000, GpuFloor 260000). Validated:
temperatures, down/up requests measured, screen-off minimum, C5 max clocks (LITTLE 1.95 GHz, MID 2.314 GHz, GPU
702 MHz measured by cycle counter), services survive a reboot. Open: touch boost needs a real finger (user), sustained
(minutes) thermals under full load not measured, MIF untouched (2314 MHz MID / 650+ MHz GPU want MIF >= 2.093 GHz
for performance per DT tables). Rollback: `sc config S10eGovernor start= demand` + `sc stop S10eGovernor` (clocks
return to the boot rates), `reg add ...S10ePower /v AllowSend /d 0` to make S10ePower read-only.
**In flight:** battery 16 % -> phone parked in TWRP to charge. **Next:** G17 V6a (UMD video DDI skeleton) offline;
HDMI with the user.

## 53. Restart point (2026-10-17 ~09:10 local)
**Committed since 52:** G17 hardware video for Windows apps, V6a-V6d (notes/g17-dxva-mfc.md has every measurement):
- Mesa 0083-0091 (tools/mesa-build/patches; working copy commits as ilyatbn): D3D11 video DDI (RetrieveSubObject
  2/3, generated stubs), H.264 VLD_NOFGT decode on the MFC (VideoH264.c rebuilds SPS/PPS from DXVA picture
  parameters; decode-order MFC session per decoder; NV12 into the output view), QueryVideoCapabilities, GPU video
  processor (VideoProc.cpp + VideoCsc.c NIR compute NV12->RGBA, then pipe->blit), profile exposed when the MFC device
  is present (no env gate). Phone UMD = maliumd 1cb628cb (0091), loaded by DWM after the next reboot (new processes
  already use it).
- Exynos9820Mfc 1.0.0.7 sys on the phone (MFC_OPEN_DECODE_ORDER); INF 1.0.0.9 carries the device Security
  (SY/BA all, Everyone/AC/LPAC/RC read); the phone's devnode got the same SDDL via tools/mfc/mfcacl.ps1 -Set
  (verified effective with mfcsd.ps1 after reboot).
- Validated on the phone: MF Source Reader decode bit-exact (720p, 1080p B-frames, visible rows); Media Engine 1080p24
  real time (mfplay); inside an AppContainer (acrun); Photos (packaged) plays test.mp4 on the MFC + GPU VP (its UMD
  log + MFC busy probe). Not yet: visual confirmation (screenshot/fb/webcam paths all failed), Photos ~15-20
  blt/s vs 24 fps (drops?), interlaced/seek/PPS switching/scaling-matrix streams, Edge (needs FL 11_0: parking lot).
- tools: d3dvdec (debug mode), mftest -d3d11/-rows, mfplay(-noread), mfd3d/mfplayq/vdecmatrix/actest(u)/photostest/
  photoslog/ordertest/mfcacl/mfcsd/appx/drvhash/totwrp (clean restart into TWRP).
**Blocked on the user:** D3D11 debug layer (Graphics Tools optional feature) - Add-WindowsCapability over SSH is
0x80070005 and a SYSTEM task was declined; webcam returns empty images.
**Photos frame rate (09:30):** decode ~5 fps after the first 40 frames, presents ~16-20/s; ruled out GPU clock (702
MHz same), present CPU cost (< 5 ms), display-off (same with display awake), vsync (62 Hz measured, tools vblank).
DWM composes ~0.2 frames/s while Photos presents (tools dwmrate) -> composition of composition/XAML swap chains on
this stack is the suspect (H5 family). Phone UMD maliumd 9d9ca53a (0092). Display: wake.ps1 before tests (it had gone
off with timeout "never" - cause unknown).
**Also validated (09:45):** 10-min soak OK (DWM alive, 0 crashes) on the new UMD; robustness clips bit-exact through
DXVA/MFC (CQM scaling matrices, Baseline, Main, B-pyramid + 4 refs + weightp, 854x480 cropping) - tools/mfc/cliptest.ps1.
**Next:** user: look at Photos playing test.mp4 (is video visible/smooth?), HDMI; me: composition swap chains (DWM
consumption of presents) as its own milestone.

## 54. Restart point (2026-10-17 ~15:30 local)

Committed (exynos-woa main, last ae51419; Mesa working copy last 1337e37 = patch 0096, clean tree):
- mpv renders on the Mali: Mesa 0095 (TGSI VS outputs sorted by location - Bifrost took varying slot 0 as
  gl_Position). Flip-model buffers: runtime present only (0094 as committed) - DWM composes them; the GDI-draw interim
  was wrong (concluded while mpv still rendered black) and is gone.
- Judder cause = clocks: S10ePower v2 (MIF control, verified 2093000 / 421000 only), S10eGovernor (GPU load = SUM of
  per-process 3D instances, GPU ceiling above 50 % load, MIF 2093000 screen on / 421000 off). mpv default config:
  DWM 35 -> 47.5 presents/s, on-time compositions 35-40 % -> ~80 %. Mesa 0096 = DWM pacing log ("pacing N" in DWM's
  tlog). Tools: tools/mfc/mpvrate.sh, mpvshot.ps1, mpvstats.lua, umdlogtail.ps1; kmdstats busy.
- On the phone: UMD 71ef0e38 (0096, DWM still runs the 0096-equivalent fafaa8c5 until it restarts), S10ePower.sys v2,
  s10egovernor.exe (old copies *.pre-mif in C:\S10eTmp). umdtrace still machine-wide ON.

Next order of work:
1. User eyes on mpv (sound on) - remaining ~20 % late compositions; audio stutter (user-reported, open).
2. Read the full devfreq_mif ladder in TWRP (sysfs available_frequencies) -> per-G3D-step MIF (Samsung table,
   gpu-odin-rom.md F2) to cut the screen-on MIF battery cost.
3. Remaining pacing: DWM's composition queues behind app jobs (one in-order queue, no preemption).
4. Blank screensaver (120 s) voids measurements - mpvrate.sh dismisses it.
5. Photos ~5 fps (open), DWM CPU-copy trigger (open), Graphics Tools WU decision (user), HDMI (G18) with the user.

## 55. Restart point (2026-10-17 ~19:30 local)

Committed (exynos-woa main; Mesa working copy last a2f5c0c = patch 0102, clean except nothing):
- Video smooth (user-confirmed): Mesa 0097 (cached BOs not listed: VidMm thrash), 0099 (RotateResourceIdentities
  rotates identities - DWM had shown flip chains out of order), 0101 (closed BOs destroyed promptly), 0100/0098/0096
  diagnostics (Render timing, app/DWM pacing logs). Tools: flipseq + seqpix (frame order), vatest/vaprobe (address
  space), gpumem/gpuengine/coretop/memcommit/lastboot/appcrash/dwmevents, kmdstats busy/paging.
- 32-bit games: x86 UMD (build-mesa-g76.ps1 stage x86 -> s10e-artifacts\mali-g12-x86\maliumd32.dll), installed BY HAND:
  C:\Windows\SysWOW64\maliumd32.dll + HKLM\...\Class\{4d36e968-...}\0000 UserModeDriverNameWow = maliumd32.dll x4
  (dxgkrnl reads it at boot). Mesa 0102: 32-bit build sub-allocates small BOs from 32 MB slabs (each allocation's
  CPU view costs 32 MB of address space; CpuVisibleOnDemand is refused in 32-bit processes and wrecks DWM in 64-bit).
  The Unity game (Game Dev Story) runs on the Mali (user: "working now").
- KMD 1.0.66.1 deployed: segment 2.5 GiB; Parameters\CpuVisibleOnDemand (0 off = default and current registry
  value; modes 2/3 broke logon / hung boot - never set them without nogpu.flg recovery ready).
- Governor: GPU load = sum of 3D instances, MIF 2093000 screen on, GpuMaxBusyPct 30, MidMaxBusyPct 50 (game
  17.6 -> 22.8 presents/s).
- Recovery learned: a forced reset leaves NTFS dirty (TWRP read-only) -> ESP S10EDIAG\nogpu.flg from TWRP boots
  Windows on Basic Display; fix the registry there; remove the flag; clean restart.

Next order of work:
1. Package the x86 UMD in the INF (UserModeDriverNameWow + SysWOW64 CopyFiles) - native install.
2. 0102 follow-up: release closed slab pages by submission sequence (busy processes keep them).
3. DWM per-session leak (~19 MB: one opened flip buffer of each app window never DestroyResource'd).
4. DWM teardown hang after taskkill (and the unkillable-process class).
5. x64-emulated apps (ARM64EC/ARM64X UMD), HDMI (G18) with the user, Photos ~5 fps, Graphics Tools WU decision.

## 56. Restart point (2026-10-17 ~23:15 local)

**Phone state changed:** firmware = **s10e-artifacts/boot-host-btmfc.img** (6db74a58: HEAD 5c445be + build-only
GPU_POWER_ON 1, WINDOWS_WATCHDOG 0, BT_UART_PROBE 1, MFC_PROBE/POWER/FWBOOT 1; USB_KD_DEVICE 0 = USB-C HOST, no KDNET).
Previous = boot-vidc2.img (KD image), backup in s10e-artifacts/boot-backups. Flash tool: tools/phone/bootflash.sh.
KMD 1.0.69.1 (ScaledModes + RgbaModes default on), UMD ARM64 0105 / x86 0106 live (System32 / SysWOW64).

Done this session (notes): G20 720p scaled mode (done); G21 game profile -> Mesa 0104 profile + 0105 per-BO WAIT_BO;
G22 RGBA source modes (KMD 69); NFSU2 (notes/games-nfsu2.md): no D3D9 DDI -> dgVoodoo2 (CHPE) in C:\Games\src ->
Mesa 0106 (tgsi_to_nir tex src sizes) -> blocked: this Mali has NO BCn decode (TEXTURE_FEATURES0 bits 7-16 clear).
Research docs for the owner's researcher: docs/exynos9820-port/research-requests.md, mongoose-research-guide.md.

Next order of work:
1. BC1-BC7 emulation in the UMD (decompress to RGBA8/R8/RG8 on upload) - unblocks dgVoodoo/NFSU2 and DXT PC games.
2. Present wait (R1): ~10.5 ms CPU<->GPU serialisation per frame in DxgiFns present.
3. Screen slow-wake outage (notes/g21-submissions.md) - open.
4. Older queue (section 55): x86 UMD in the INF, slab page release, DWM leak / teardown hang, x64 apps, HDMI.

## 57. Restart point (2026-10-18 ~02:45 local)

**Phone state:** firmware boot-host-btmfc.img (unchanged; USB-C host works, the user's keyboard dongle enumerates).
KMD 1.0.69.1. **UMD x86 = mali-umd-0123-x86** (SysWOW64; Mesa 0111-0123). **UMD ARM64 = mali-umd-0119** (System32;
Mesa 0111-0119, CS 4.x advertised, loaded by DWM since the 01:23 reboot - desktop fine); previous ARM64 kept as
System32\maliumd.dll.old0110. UPDATE ~03:05: ARM64 = mali-umd-0123 now in System32 (d3dcs 7/7, d3dbc lossy 126/126; previous kept as
maliumd.dll.old0119); browser check NOT done - battery 13% and not charging (the keyboard dongle holds the USB-C port);
testing stopped, user notified.
NFSU2: C:\Games\src, dgVoodoo.conf = windowed borderless fullscreen-size (backups .bak-exclusive / .bak-fake),
desktop shortcut SPEED2.lnk (launch it in the foreground; the game crashes on focus loss / its own resolution change).

Done this session (notes/g24-compute-cs4x.md, notes/g26-bc-etc2.md): G24 DirectCompute 4.x (0111-0115, 0117-0119),
G25 GS guard (0116; Panfrost has no GS - emulation via src/poly open), rotated DXGI exclusive fullscreen (0120-0122,
user-verified upright), G26 BC->ETC2/EAC transcode (0123; d3dbc lossy 126/126, d3dbcmem 600 vs ~260 textures).
NFSU2 reached the game; the race load ran out of 32-bit address space -> G26.

Next order of work:
1. (charge first) ARM64 0123 is deployed and tested (d3dcs, d3dbc); still to do: browsers (Edge/Brave GPU page + WebGL) with
   CS 4.x advertised (G24 review follow-up) before the next reboot puts it into DWM. User wants all fixes on 64-bit.
2. User test: NFSU2 race load with UMD x86 0123 (cannot be reproduced unattended; d3dbcmem is the proxy).
3. G26 speed: ~0.5 s (BC1) / ~0.8-1 s (BC3) per 1024x1024 upload under x86 emulation; T/H ETC2 modes for 2-colour
   blocks (quality, follow-up).
4. G25 geometry shaders via compute (src/poly) - dgVoodoo's 2 GS are skipped today (particles likely missing).
5. Older queue (sections 55/56): present wait, screen slow-wake, x86 UMD in the INF, slab release, DWM leak, HDMI.

## 58. Restart point (2026-10-18 ~12:30 local)

**Phone state:** firmware boot-host-btmfc.img; KMD 1.0.69.1; **UMD x86 = mali-umd-0127-x86 (SysWOW64), UMD ARM64 =
mali-umd-0127 (System32; DWM still runs the 0123 it loaded at boot until the next reboot)** - Mesa 0111-0127. Older
copies: System32\maliumd.dll.old0110/.old0119/.old0123/.tmp0123, SysWOW64\maliumd32.dll.old*. Battery charged (99 %).
The phone's clock reset after the 03:23 power-off (file times read 2026-10-17 ~20:00-21:00).

Done since 57: G26 BC->ETC2 (0123) verified on both UMDs; G25 geometry shaders emulated: Mesa 0124 (Panfrost builds
nir->xfb_info from TGSI stream output - stream output was dead for every TGSI VS), 0125 (GS -> compute + three-pass
draw), 0126 (per-vertex capture: indexed + strips), 0127 (barrier). d3dgs 6/6 pixel-exact vs WARP on x86 + ARM64;
d3dcs, d3dbc, Edge WebGL (cdp_webgl.py) unchanged. NFSU2: both dgVoodoo GS compile; particles need a race (user).

Next order of work:
1. User tests: NFSU2 race load (G26 memory) + particles (G25); the keyboard dongle works.
2. x86 UMD packaged in the INF (UserModeDriverNameWow + SysWOW64 CopyFiles) - today it is copied by hand.
3. D3D10 stream output without a GS (CreateGeometryShaderWithStreamOutput, empty GS): attach the SO to the VS now that
   Panfrost XFB works for TGSI.
4. Present wait (R1), screen slow-wake, G26 speed / ETC2 T-H modes, slab release, DWM leak, HDMI.

## 59. Restart point (2026-10-18 ~13:30 local)

**Phone state:** firmware boot-host-btmfc.img; KMD 1.0.69.1; **UMD x86 = mali-umd-0128-x86 (SysWOW64). System32 =
the G27 ARM64X set from s10e-artifacts\mali-umd-0129-x: maliumd.dll (pure forwarder) -> maliumd_arm64.dll (ARM64,
Mesa 0001-0129) / maliumd_ec.dll (ARM64EC, same source)**, copied live with `wddm2dbg.sh umdxswap`; DWM, explorer and
the shell load maliumd.dll + maliumd_arm64.DLL since the 13:12 reboot (desktop + taskbar OK). Rollback:
System32\maliumd.dll.pre-g27 (= 0127s plain ARM64) via umdswap. Battery 100 % on the charger. Phone clock reset again
(reads 2026-10-17 ~20:xx).

Done since 58: G25.6 SO without a GS (0128); G27 x64 apps: G27.1 baseline (x64 d3dcs got DXGI_ERROR_UNSUPPORTED),
G27.2 ARM64EC Mesa build (build-mesa-g76 -Stage ec, Mesa 0129, venv-ec with meson's arm64ec machine mapping),
G27.3 forwarder (tools/mesa-build/make-arm64x.ps1): x64 d3dcs gets Mali FL 10_1, 7/7 PASS; ARM64 d3dcs 7/7, Edge WebGL
exact; G27.4 INF 1.0.71.1 + package s10e-artifacts\mwddm-1.0.71.1 (InfVerif valid, NOT installed - attended).
shot.ps1 is DPI-aware now (older shots at 150 % were top-left 2/3 crops - no taskbar).

Next order of work:
1. G27.5: real x64 apps on the GPU (an x64 D3D11 game / x64 browser build), x64 builds of d3dgs / d3dso / d3dbc.
2. User tests: NFSU2 race load (G26) + particles (G25); install mwddm-1.0.71.1 when attended.
3. Present wait (R1), screen slow-wake, G26 speed / ETC2 T-H modes, slab release, DWM leak, HDMI.

## 60. Restart point (2026-10-18 ~15:35 local)

**Phone state:** as 59 (ARM64X UMD set live, x86 0128, KMD 1.0.69.1), Windows up after two bugcheck resets (phone clock
reset again). Game DVR settings restored to default. Battery 100 %.

Done since 59: on-phone screen recording for the remote user (tools/phone/rec: screenrec.exe GDI or -dup DXGI desktop
duplication, recfetch.sh -> GIF + contact sheet, SendUserFile); the "display asleep" readings were the blank
screensaver. x64 apps verified with recordings: mpv x86_64 D3D11 FL 10_1, ungoogled-chromium x64 WebGL Aquarium
24-27 fps. NFSU2: dies (+0x6562b) whenever it loses the foreground - the "ms-gamingoverlay" popup (no Xbox Game Bar on
this image; winget Store install fails 0x80004002) and the screensaver; Settings > Gaming capture off avoids the popup
(tested as a diagnosis switch, restored) - USER DECIDES the native fix.
NEW BUG: fullscreen x64 mpv + -dup capture across its whole run bugchecks the system 2/2 (S10eDiag ring "BUGCHECK
callback", no code; Kernel-Power 41 code 0); subsets 0/4 (notes/g27-x64-apps.md).

Next order of work:
1. Bugcheck code for the fullscreen+duplication crash (S10eDiag AuxKlib BC lines missing, or KD), then fix.
2. NFSU2 race test once the user picks the popup fix (gamedrive.ps1 key script + -dup / scan-out frames).
3. G27 leftovers: install mwddm-1.0.71.1 (attended); x64 game.

## 61. Restart point (2026-10-18 ~17:25 local)

**Phone state:** firmware **s10e-artifacts/boot-host-r2.img** (9e15a5bc; = HEAD + the btmfc build overrides via the
new firmware/tools/build-host-image.ps1; rollback boot-host-r1.img / boot-host-btmfc.img, backups in boot-backups).
Windows sees 5912 MiB usable (was 5370) and 6 GB installed (SMBIOS). KMD 1.0.69.1, UMD ARM64X set (0129), x86 0128.
New S10eDiag (AuxKlib/BC status marks) live. Game capture off (user's choice). Battery 100 %.

Done since 60: alpha7 tagged (local, 4fdb98a). R1 RAM: CP reservations + sec_rdx_bootdev published (memfill to 98 %
load, 0 mismatches). R2: SMBIOS 0/1/3/16/17/19. NFSU2 reached title + menu (black screens were the screensaver);
fullscreen dumps show stripes that move between dumps (likely capture artefact - user to confirm on the panel).

Next order of work:
1. User: stripes on the panel in NFSU2 fullscreen? Install mwddm-1.0.71.1 when attended.
2. Fullscreen + desktop-duplication bugcheck (code via the new S10eDiag marks on the next occurrence).
3. NFSU2 race (gamedrive.ps1: click at the title, then menus), GPU backlog (notes/g27, HANDOFF 60).

## 62. Restart point (2026-10-18 ~18:55 local)

**Phone state:** as 61 (boot-host-r2.img, 5912 MiB / 6 GB installed) + S10eBattery 1.0.0.6 (read-only USB-C IOCTL;
1.0.0.7 H2 write source is in the tree, NOT built - the auto-mode classifier refused the build, user decides) +
IddSampleDriver package (drivers/S10eIdd poc4) in the driver store, device only while iddstream.cmd runs.

G18 external/remote display (user: "Can you do something about type-c to hdmi and remote monitors while im away?"):
- Research: notes/g18-h2-altmode-research.md (max77705 DP alt-mode entry: AP enables alt mode 0x55/0x03, FW
  discovers, AP sends Enter Mode + DP Configure, HPD in DP Status VDOs; Android's shutdown resets the block, so under
  Windows nothing happens on plug until 0x55 is sent), notes/g18-remote-monitors-research.md (RDP works today; spacedesk
  ARM64; own IddCx driver; Miracast blocked by HW H.264 + Wi-Fi Direct; avoid Sunshine/desktop duplication).
- H1b: usbcwatch.exe logger ready for the user's hub plug test (tools/phone/usbc/README.md).
- Remote monitor PoC: IddCx virtual monitor 2560x1440 appears and DWM composes it on the Mali (proved by a GDI shot);
  frame sink + S10eIddView HTTP MJPEG pipeline runs end to end, but frames read back black - open.

Next order of work:
1. User: run usbcwatch with the hub + HDMI monitor plugged; decide on building the H2 write IOCTL (S10eBattery 1.0.0.7);
   try RDP / the virtual monitor; NFSU2 stripes check; install mwddm-1.0.71.1.
2. IddCx black frames: log render adapter + pixel probe in ShareFrame; IddCx frame-processing docs.
3. Fullscreen + duplication bugcheck (code via S10eDiag marks on the next occurrence).

## 63. Restart point (2026-10-18 ~23:45 local)

**Phone state:** firmware boot-host-r2.img (unchanged since 17:15). SysWOW64\maliumd32.dll = x86 **threaded context +
constant-buffer renaming** build x86-tc6f (e1fae195; s10e-artifacts/x86-tc6f with PDB) - Mesa TC T1-T6
(notes/perf-threaded-context.md). The ARM64 / ARM64EC UMDs (DWM, browsers, x64 apps) are NOT changed (still pre-TC).
Rollback for 32-bit apps: `bash tools/phone/wddm2/wddm2dbg.sh umd32swap /c/Users/ilya/code/s10e-artifacts/x86-0130e`
(no reboot). Diagnosis switch only: MALIUMD_NO_TC=1.

**Done this session (G29, notes/perf-threaded-context.md):** MS Learn: no runtime callbacks from a driver worker on
D3D10+ -> winsys marshals them to the DDI thread (T1); Panfrost TC subclassing + TC-direct thread safety (T2/T2b);
TC map rules + replace_buffer_storage (T3); TC enabled (T4) + no user buffers (T4b); re-entry fix (T5a), assert fix
(T5b), Mesa Windows futex timed-wait bug (T5c, the deadlock); DYNAMIC CB renaming (T6). NFSU2 race 84 -> ~30 ms.
keyplay retries the foreground (automation no longer dies at the first Enter). New tests: d3dcbdyn, x86tests build.

**Next, in order:** (1) user play test of NFSU2 on the TC build; (2) build ARM64 + ARM64EC UMDs with TC + T6, run the
ARM64 tests + DWM/browser checks (AGENTS port-x86-fixes-to-64bit); (3) driver-thread costs: new-batch init 1.5 ms
x7/frame (get_batch_slow), launch_draw, varyings; (4) export Mesa patches 0132+ to tools/mesa-build/patches.

## 64. Restart point (2026-10-19 ~00:35 local) - alpha8

**Phone state:** firmware boot-host-r2.img. UMDs: System32 ARM64X set **mali-umd-0148-x** (forwarder e7fce848 ->
maliumd_arm64 / maliumd_ec, Mesa 0001-0149) and SysWOW64 **x86-0148** (2f738e24) - threaded context + DYNAMIC
constant-buffer renaming in all three. DWM and the desktop run on it (rebooted). No page heap / LocalDumps left set.
dgVoodoo NFSU2: CenterAppWindow=true (screen-sized borderless window at 0,0). Rollback: umdxswap
s10e-artifacts/mali-umd-0129-x (pre-TC 64-bit set), umd32swap s10e-artifacts/x86-0130e. Diagnosis switch only:
MALIUMD_NO_TC=1.

**Next:** user NFSU2 play test; driver-thread batch init cost (get_batch_slow 1.5 ms x7/frame); remove the per-draw
diagnostic timers from release builds once perf work settles.

## 65. 2026-10-19 night - M4 big cores (G30), unattended

- Full log + summary: notes/m4-cores.md ("Summary 2026-10-19"). Commits 4e3081d, 5d6021a, b50d004 + this one.
- Bare-metal M4 tests from the park stub (flag 7 + options) all pass: interrupts/WFI, PMU, atomics run, DVM, timers.
- 8 cores with today's firmware: kernel heap-corruption bugchecks every boot (KD log s10e-artifacts/runs/m4kd8) ->
  next: verify atomicity/ordering between an M4 and an A55/A75 in the stub (counters checked, not just counted).
- Phone: boot-host-r2.img (6 cores) restored; BCD debug turned off again (03:35, verified).
- Never peek CPUCL2 SYSREG 0x1D100104 or PMU 0x15861200-0x1650 from Windows (SoC freeze).


## 66. 2026-10-19 - UFS speed-up (app load times)

- notes/app-launch-slowness.md + notes/ufs-async-plan.md (full log, every build with hash + test results).
- Phone: firmware boot-host-r4.img (IO coherency BIT22|23 + real UFS interrupt GSIV 281, both now source defaults)
  + Exynos9820Ufs.sys w47 (DriverStore). PAIRING RULES: IOCOH firmware needs driver >= w40; GSIV 281 needs >= w43.
- Driver features (UfsIoMode, auto-off after a failed boot via progress word [12]): fast per-command zeroing,
  full-duplex StartIo, coherent RAM bounce, 256 KiB commands, interrupt observe, hybrid async (> 64 KiB reads),
  4 slots for async reads, NEXUS cache / fine poll.
- Results: 1 MiB reads 50-120 -> ~500-560 MiB/s, 4 KiB 300 -> ~150 us, Brave first start 6.6 -> 2.35 s, Steam warm
  49.8 -> ~30 s. Integrity checks: tools/phone/perf/hashcheck.ps1, parhash.ps1, writetest.ps1 (baseline hash-w38).

## 67. 2026-10-19 - native OpenGL (G31)

- notes/gl-icd.md (design, docs, every build hash, measurements, reviews).
- Phone (live, by hand - not from a package): System32\maligl.dll (ARM64X forwarder) + maligl_arm64.dll +
  maligl_ec.dll = Mesa 0150-0156 (s10e-artifacts\mali-gl-x, deploy tools/phone/gl/gldeploy.sh); class key 0000
  OpenGLDriverName maligl.dll / OpenGLVersion 1 / OpenGLFlags 1 set by tools/phone/gl/glicd.ps1 (dxgkrnl reads
  them at adapter start: a change needs a reboot). Rollback: glicd.ps1 -Remove + reboot.
- Build: tools/mesa-build/build-mesa-g76.ps1 -Stage gl,glec then make-arm64x.ps1 -Name maligl (-Arm64 mali-g12-gl
  -Ec mali-g12-gl-ec -Out mali-gl-x). Test: drivers/MaliG76Wddm/tools/gltest (gltest.exe ARM64, gltest64.exe x64).
- Stardew Valley: starts in ~3-4 min (emulated .NET), ~41 fps at 1520x720; timing lines via tools/phone/gl/gltlog.ps1.
  Kill scrnsave.scr before screenshots (blank screensaver = black scan-out, CopyFromScreen "handle is invalid").
- Package s10e-artifacts\mwddm-1.0.72.1 (Umd 0148-x, Umd32 mali-g12-x86, Gl mali-gl-x, InfVerif valid) NOT installed.
- Next: GPU-only present (no CPU read of the frame; the copy is ~19 ms/frame from write-combined memory).
