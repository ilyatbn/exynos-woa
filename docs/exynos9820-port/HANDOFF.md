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
