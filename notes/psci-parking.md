# PSCI / core bring-up research (for Exynos 9820 WoA)

## A. WoA projects: parking & core bring-up (agent A)

Status: in progress. Research questions: how open-source WoA projects bring up
secondary cores under Windows; whether Windows onlines cores via PSCI SMC;
what "CPU parking protocol" refers to; minimum PSCI version for Windows ARM64.

### Progress log

- (started 2026-10-11) Created this file; beginning web research on
  WOA-Project (Lumia), Renegade Project (SD845/850), SurfaceDuoProject (SD855),
  Windows-on-ARM-Experiments (8cx), and Windows ARM64 PSCI/ACPI discovery.

- Cloned shallow copies to /tmp/opencode/woa-repos/: WOA-Project/Lumia950XLPkg,
  Project-Aloha/mu_aloha_platforms, WOA-Project/mu_andromeda_platforms,
  edk2-porting/edk2-sdm845, edk2-porting/edk2-msm, edk2-porting/edk2-msm8998.
  (Repos found via web search; all clone OK.)

## A1. Lumia 950 / 950 XL — WOA-Project/Lumia950XLPkg  (THE "parking protocol" hit)

Source: https://github.com/WOA-Project/Lumia950XLPkg (local clone /tmp/opencode/woa-repos/Lumia950XLPkg)

**FACT — the term "parking protocol" in WoA comes from the ACPI MADT GICC subtable
fields "Parking Protocol Version" + "Parked Address".** Lumia950XLPkg ships MADT
variants that set them:

- `AcpiTables/8992/src/MADT.MpPark.asl` (MSM8992 = SD 808) and
  `AcpiTables/8994/src/MADT.MpPark.asl` (MSM8994 = SD 810), plus
  `AcpiTables/Hapanero/src/MADT.MpPark.asl` (950 XL codename).
- Each GICC entry (Subtable Type 0x0B) carries:
  `Parking Protocol Version : 00000001` and `Parked Address : 0000000000301000`
  (core 0), `0x311000` (core 1), `0x321000`, `0x331000`, `0x341000`, `0x351000`
  — one 0x1000-aligned park mailbox per core inside a region at 0x00300000.
- The *plain* `AcpiTables/8992/src/MADT.asl` ALSO has Parking Protocol Version 1
  (parked addrs 0x301000/0x302000/0x303000... — slightly different layout), so the
  MpPark variant is a per-core-page layout vs a shared-page layout. (fact from diff)
- OEM ID of these MADTs: "QCOM", OEM table ID "QCOMEDK2" — i.e. these are the
  **stock Qualcomm EDK2 ACPI tables** (from WP10 / Qualcomm's own UEFI), not
  hand-written by the community. (fact: bytes; inference: "stock" attribution)

**FACT — UEFI-side PCDs for the park region** (`Lumia950XLPkg.dec` lines 66-70):

```
gLumia950XLPkgTokenSpaceGuid.SecondaryCpuStackSize|0x4000|UINT32|0x0000f001
gLumia950XLPkgTokenSpaceGuid.SecondaryCpuMpParkRegionBase|0x00300000|UINT64|0x0000f002
gLumia950XLPkgTokenSpaceGuid.SecondaryCpuIgnoreCpuIdCheck|FALSE|BOOLEAN|0x0000f003
gLumia950XLPkgTokenSpaceGuid.PsciCpuSuspendAddress|0x06c03aa8|UINT64|0x0000f004
```
`Shared.dsc.inc` overrides: SecondaryCpuStackSize 0x2000, MpParkRegionBase 0x00300000,
SecondaryCpuIgnoreCpuIdCheck TRUE.

**FACT — memory map reserves the park region** (`Include/Configuration/DeviceMemoryMap.h` line 68):
`{"MPPark Code", 0x00300000, 0x00080000, AddMem, MEM_RES, UNCACHEABLE, RtCode, UNCACHED_UNBUFFERED}`

**FACT — PrePi entry refuses secondaries** (`PrePi/ModuleEntryPoint.S`): reads
MPIDR, computes CPU index, and `cmp x19, #0x0 / bne dead` — "Secondary startup is
not expected", any non-primary core that enters UEFI spins in `dead: b dead`.
The file still exports `_MpParkBase` = FixedPcdGet64(SecondaryCpuMpParkRegionBase)
(line 33-34) for consumers. PrePi.inf comments the ArmHvcLib/ArmSmcLib libs as
"# MpPark" (lines 47, 75).

**FACT — FADT variants**: `AcpiTables/common/builtin/` contains `FADT.acpi`,
`FADT.NoPsci.acpi`, `FADT.PsciSmc.acpi`. Byte diff (cmp -l): they differ only at
byte 130 (1-based, = FADT offset 0x81): NoPsci has 0x02, PsciSmc has 0x01.
(Interpretation of that FADT field: pending — check ACPI 6.0 FADT ARM flags.)

**FACT — UEFI uses stock EDK2 `ArmPkg/Drivers/CpuDxe/CpuDxe.inf`** (Lumia950XL.fdf
lines 39/49, Shared.dsc.inc line 542). No custom CpuDxe in the package.

**FACT — PsciCpuSuspendAddress = 0x06c03aa8** appears only in .dec/.dsc (no C
consumer found in this repo with grep for PsciCpuSuspendAddress in *.c/*.S/*.h).

### A1.1 Which tables the shipped builds use (fact, from .fdf)

- `Lumia950XL.fdf` lines 147/152: embeds `common/builtin/FADT.PsciSmc.acpi` +
  `8994/builtin/MADT.aml`.
- `Lumia950.fdf` lines 147/152: `FADT.PsciSmc.acpi` + `8992/builtin/MADT.aml`.
- `Hapanero.fdf` (950 XL proto): `FADT.PsciSmc.acpi` + `Hapanero/builtin/MADT.aml`.
- Decoded GICC fields of the builtin .aml binaries (python struct parse):
  - `8994/builtin/MADT.aml`: **PPV=0**, ParkedAddress 0x301000..0x308000 (stale but ignored when PPV=0)
  - `8994/builtin/MADT.MpPark.aml`: **PPV=1**, ParkedAddress 0x301000,0x311000,...,0x371000
  - `8992/builtin/MADT.aml`: **PPV=1**, ParkedAddress 0x301000..0x306000
  - `8992/builtin/MADT.MpPark.aml`: **PPV=1**, per-core pages 0x301000,0x311000,...
- FADT variants decoded at offset 0x81 (`ArmBootArchFlags`, UINT16; EDK2 defines
  `EFI_ACPI_6_0_ARM_PSCI_COMPLIANT`=BIT0, `EFI_ACPI_6_0_ARM_PSCI_USE_HVC`=BIT1,
  source: https://raw.githubusercontent.com/tianocore/edk2/master/MdePkg/Include/IndustryStandard/Acpi60.h):
  - `FADT.acpi` (stock WP10): 0x0003 = PSCI compliant + **HVC** conduit
  - `FADT.NoPsci.acpi`: 0x0002 (compliant bit cleared → PSCI off)
  - `FADT.PsciSmc.acpi`: 0x0001 = PSCI compliant, **SMC** conduit
- `AcpiTables/FADT.aslc` (the source-generated FADT) also sets
  `EFI_ACPI_6_0_ARM_PSCI_COMPLIANT` with comment:
  `// We now traps SMC calls so no need to declare HVC / Thanks @winocm!`

### A1.2 THE MECHANISM: `PrePi/HvcPatch.h` — EL2 trap of SMC-based PSCI (fact)

Commit https://github.com/WOA-Project/Lumia950XLPkg/commit/bec3e37abd807787d203c7f0f3c2207f545c72f8
(2020-09-08, imbushuo + Gustave Monce + Sarah Purohit/winocm, PR #75):
**"PSCI: Trap SMC-based PSCI calls in EL2 to unblock Windows Mn/Fe+ boot"**.
Header comment of `PrePi/HvcPatch.h` (verbatim):

> "Some background story: it seems that Windows build higher than 19041 (not
> including 19041) Messed up the HVC-based PSCI call, although it boots fine on
> Qemu/KVM platform. There are undisclosed platforms also reporting issues with
> HVC-based PSCI call. But on 8992/8994 devices, it completely hangs the boot
> process (ACPI.sys refused to enumerate devices)"
> "The patch assumes BSP version 1078, which is the last production release..."

What the patch does (binary patches written into the **stock Qualcomm WP10 EL2
hypervisor** that is already resident when UEFI starts at EL2):

- `WAKE_FROM_POWERGATE_PATCH_ADDR 0x6C001F8` → code that sets `HCR_EL2.TSC`
  (trap SMC from EL1 to EL2) on powergate wake.
- `LOWER_EL_SYNC_EXC_64B_PATCH_ADDR 0x6C08C24` / `..._32B_ 0x6C08E24` → patched
  "lower-EL synchronous exception" vector dispatch:
  - ESR.EC 0x16 (HVC64) → stock `HvcHandlerEntry` (the stock hyp's PSCI handler)
  - ESR.EC 0x17 (SMC64 trap) → new stub: `ELR_EL2 += 4` (skip the SMC insn),
    then `(w0 & ~0x1F) | 0x40000000 == 0xC4000000` test: any PSCI-0.2 64-bit
    function in the 0xC4000000-0xC400001F window (CPU_SUSPEND/CPU_ON/...) is
    **redirected into the stock HVC PSCI handler**; everything else falls
    through to a real `smc #0` (forwarded to EL3 TrustZone).
- `PrePi/Pi.c: PsciFixupInit()` CopyMem's the three handlers to those fixed
  addresses, invalidates caches, then probes: `ArmCallHvc(PSCI_VERSION)` and
  `ArmCallSmc(PSCI_VERSION)` ("Call into the handler to make HCR_EL2.TSC sticky").

Interpretation (inference): on MSM8992/8994 the *usable* PSCI implementation
lives in the stock WP10 **EL2 hypervisor** (HVC conduit), not EL3; TZ PSCI via
SMC is a limited subset (README: "8992/8994 uses HVC call for PSCI commands
instead of SMC call"; "PSCI_SYSTEM_OFF and PSCI_SYSTEM_RESET are not
implemented. Hence we use PMIC to shutdown"). Windows >19041 broke HVC-based
PSCI (or at least the combo hung these devices, ACPI.sys failing to enumerate),
so the fix was: advertise SMC conduit in FADT, set HCR_EL2.TSC, and have the
EL2 vectors translate SMC PSCI calls into the existing HVC PSCI handler.

### A1.3 Parking protocol (ACPI) — what it is (fact + inference)

- The "CPU parking protocol" is a standard ACPI-for-ARM concept: MADT GICC
  subtable (type 0x0B) fields `ParkingProtocolVersion` (UINT32) and
  `ParkedAddress` (UINT64) — field layout per EDK2
  `EFI_ACPI_6_0_GIC_STRUCTURE` (Acpi60.h, URL above). PPV=0 → not parked.
- Lumia stock tables park secondaries in a spin region at 0x00300000
  ("MPPark Code" in the UEFI memory map, 0x80000 bytes), one mailbox page per
  core (0x301000, 0x311000, ... in the MpPark variant).
- README Linux notes (fact, quote): "If you are using EL2 startup, use
  `spin-table` with `per_cpu_mailbox_addr + 0x8` as the release address." —
  i.e. the park mailbox is the DT spin-table equivalent (mailbox+8 = release
  address). (inference) the ACPI parking protocol v1 is the ACPI spelling of
  the same spin-table mailbox: OS writes the entry point into the mailbox at
  Parked Address and `sev`s the parked core out of its spin loop.
- Verdict for Lumia (fact from .fdf): shipped builds do NOT use parking
  (8994 MADT PPV=0 + FADT PsciSmc); the MpPark MADT variants exist as an
  alternative for when PSCI is not used. (inference) they are kept because the
  stock WP10 boot chain (LK) leaves cores parked at 0x300000 regardless.

### A1.4 History note

- Earlier commit (2018-05-03) "Fix MPCore bringup on 950XL."
  https://github.com/WOA-Project/Lumia950XLPkg/commit/aa01ee264ff79fe92824114c747ed5b767033fa1
  Added a minimal `AcpiTables/DSDT.asl` with 8 `ACPI0007` processor devices
  (Windows enumerates CPUs from DSDT/MADT GICC UIDs). The stock 950XL MADT
  variant in that commit (`MADT.Uni.asl`, disassembled from
  `G:/Images/RM-1085/PLAT/ACPI/madt.acp`, i.e. the **stock WP10 image**) has:
  PPV=0, ParkedAddress 0x301000..0x308000 present, and **only CPU0 has
  "Processor Enabled" flag set** (CPU1-7 flags=0). So the stock 950XL "Uni"
  table described a single-core machine; the community enabled all cores in
  their MADT and let PSCI (HVC at the time) bring them up.

- Stock-vs-community table matrix (fact, decoded from .aml at HEAD and at
  parent commit fa0b66c):
  | table | stock (pre-2020) | HEAD (shipped) |
  |---|---|---|
  | 8992 (Lumia 950) MADT | PPV=1, parked 0x301000+0x1000*N | PPV=1 (unchanged) |
  | 8994 (950XL) MADT | PPV=0 | PPV=0 |
  | FADT builtin stock | ArmBootArch=0x0003 (PSCI+HVC) | PsciSmc variant=0x0001 used in .fdf |
  → **Microsoft's own WP10 firmware for Lumia 950 (MSM8992) used the ACPI CPU
  parking protocol (PPV=1)** for secondary cores; 950 XL used PSCI-over-HVC.
  Desktop Windows ARM64 boots fine with PPV=1 on 8992 (Lumia950.fdf ships it),
  so Windows ARM64 supports the parking protocol (fact by device validation in
  the project; inference: Windows implements ACPI parking protocol v1).

## A2. Renegade Project — edk2-porting/edk2-sdm845 (SD 845/850, also sm8250)

Source: https://github.com/edk2-porting/edk2-sdm845 (local clone)

**FACT — FADT**: `Silicon/Qualcomm/sdm845/AcpiTables/Facp.aslc` lines 65-68:
```
  // Bit 0 is for PSCI support
  // Bit 1 is to tell the OS to use HVC instead of SMC.
  // 0x0 to disable PSCI
  0x1,                                           // ARM_BOOT_ARCH;
```
→ Windows onlines cores via **PSCI over SMC**.

**FACT — MADT**: `Silicon/Qualcomm/sdm845/AcpiTables/Madt.aslc`: every GICC
entry sets `ParkingProtocolVersion = 0x00` (8 cores, MPIDR 0x0,0x100,...,0x700).
→ **No parking**; cores are started by the OS through PSCI SMC, which is served
by the **stock signed TrustZone/AppsBL EL3 firmware** (xbl/TZ cannot be
modified on retail devices — see Renegade docs "Fastboot to EDK2":
https://renegade-doc.readthedocs.io/en/latest/edk2/Overview.html — "xbl and abl
are both signed on retail devices. We can't make any modifications to them").

**FACT — no parking code anywhere in the repo** (grep 'park' over .c/.h/.dsc/
.inf/.fdf found nothing relevant; only Lumia950XLPkg has MpPark).

## A3. Surface Duo — WOA-Project/mu_andromeda_platforms (SD 855 / SD 888)

Source: https://github.com/WOA-Project/mu_andromeda_platforms (local clone)

**FACT**: `Platforms/SurfaceDuo1Pkg/AcpiTables/FACP.aslc` and
`SurfaceDuo2Pkg/AcpiTables/FACP.aslc` line 66:
`EFI_ACPI_6_0_ARM_PSCI_COMPLIANT, // UINT16 ArmBootArchFlags` → PSCI via SMC.
No `ParkingProtocolVersion`/`ParkedAddress` set anywhere (grep empty) → PPV=0,
no parking. Same model as Renegade: stock EL3 PSCI does CPU_ON/CPU_OFF.

## A4. Windows-on-ARM-Experiments (SD 8cx / Surface Pro X)

**FACT**: the GitHub org https://github.com/Windows-on-ARM-Experiments hosts
toolchain repos (gcc-woarm64, mingw-woarm64, binutils-woarm64, msys2, etc.) —
repo listing via GitHub API shows **no UEFI/EDK2 repo** in the org.
(inference) their 8cx work used the *stock Microsoft/Qualcomm UEFI* of the
Windows Dev Kit / Surface Pro X, so core bring-up was never their problem;
PSCI is provided by stock firmware. (needs confirmation — search pending)

## A5. Cross-cutting facts from the clones

### A5.1 UEFI itself never starts APs in these ports (fact)

- EDK2 `ArmPkg/Drivers/CpuDxe/CpuDxe.inf` (used by Lumia950XLPkg and
  edk2-sdm845 fdf's) installs only `gEfiCpuArchProtocolGuid`,
  `gEfiMemoryAttributeProtocolGuid`, `gHardwareInterruptProtocolGuid` —
  **no EFI_MP_SERVICES_PROTOCOL** (checked CpuDxe.c/.inf on edk2 master:
  https://raw.githubusercontent.com/tianocore/edk2/master/ArmPkg/Drivers/CpuDxe/CpuDxe.c).
- Lumia PrePi `ModuleEntryPoint.S`: secondary cores entering UEFI → `b dead`.
- So in every port examined, **the OS (Windows) is the entity that onlines
  secondary cores** — via PSCI SMC (SD845/855/888, Lumia post-2020) or via the
  ACPI parking protocol (stock WP10 Lumia 950).

### A5.2 Stock Qualcomm EL3 TZ implements PSCI as standard SMCs (fact)

`edk2-sdm845/Silicon/Qualcomm/QcomPkg/Include/Protocol/scm_sip_interface.h`
lines ~283-308: "PSCI Command IDs: Do not reuse" —
`PSCI_VERSION_ID / CPU_SUSPEND_ID / CPU_OFF_ID / CPU_ON_ID / AFFINITY_INFO_ID /
MIGRATE_ID / MIGRATE_INFO_TYPE_ID / MIGRATE_INFO_UP_CPU_ID / SYSTEM_OFF_ID /
SYSTEM_RESET_ID` built with `TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_STD, 0x0, N)`
(= standard PSCI IDs), plus Qualcomm vendor extensions `CPU_FREEZE (0xB)`,
`CPU_DEFAULT_SUSPEND (0xC)`. Confirms SD845-era TrustZone ("AppsBL") serves
full PSCI-0.2+ via SMC at EL3; Renegade's UEFI just consumes it (its own
reset/poweroff drivers call these).

### A5.3 Windows kernel itself calls PSCI functions (fact, from DuoWoA)

`mu_andromeda_platforms/Platforms/AndromedaPkg/Library/KernelErrataPatcherLib/`
(also in mu_aloha_platforms; based on https://github.com/SamuelTulach/rainbow):
patches NTOSKRNL/OS loader at boot for four platform errata, one of them:
"Patches NTOSKRNL to not cause a bugcheck when attempting to use
**PSCI_MEMPROTECT** Due to an issue in QHEE" (`OsLoaderIntercepter.c` line 51
injects `PSCI_MEMPROTECT_FAILING_PATCH` shellcode).
→ Windows ARM64 probes/uses PSCI 1.0 `PSCI_MEM_PROTECT` (id 0x84000028 /
0xC4000028) when the firmware reports a PSCI version that has it; a broken
implementation (Qualcomm QHEE hypervisor) bugchecks the kernel, so the WoA
project patches the kernel to skip it. (inference) Windows decides which PSCI
functions to use by calling PSCI_VERSION + PSCI_FEATURES — consistent with
the HvcPatch comment that ACPI.sys hangs when the PSCI conduit is broken.

## A6. SD 835 — edk2-porting/edk2-msm8998 (OnePlus 5/5T WoA)

Source: https://github.com/edk2-porting/edk2-msm8998 (local clone)

**FACT**: `Platforms/Msm8998ACPI/Common/src/FACP.dsl` decoded: "PSCI Compliant : 1,
Must use HVC for PSCI : 0"; `build/FACP.aml` offset 0x81 = 0x01 → PSCI via SMC.
`Common/src/Madt.dsl`: all 8 GICC entries `Parking Protocol Version : 00000000`,
`Parked Address : 0` → pure PSCI, no parking. Same as sdm845/sm8150: stock EL3
PSCI does the work.

## A7. Spec-level facts (the "parking protocol" defined)

- ACPI spec (MADT GICC subtable, type 0x0B): fields "Parking Protocol Version"
  (offset 16 in entry) = "Version of the ARM-Processor Parking Protocol
  implemented", and "Parked Address" (offset 24) = "The 64-bit physical address
  of the processor's [mailbox]...". Source: ACPI 6.5 §5.2.12
  https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model.html
  (fetch returned 403; text confirmed via search-engine snippet of that page;
  also ACPI 5.1 PDF https://uefi.org/sites/default/files/resources/ACPI_5_1release.pdf
  which points the PPV field at an infocenter.arm.com parking-protocol doc).
  EDK2 struct `EFI_ACPI_6_0_GIC_STRUCTURE` (ParkingProtocolVersion, ParkedAddress):
  https://raw.githubusercontent.com/tianocore/edk2/master/MdePkg/Include/IndustryStandard/Acpi60.h
- Arm SBBR (DEN0044): "MADT.GICC.PPV is the parking protocol version number.
  A value of 0 identifies a PSCI exclusive system." and "For systems without
  PSCI, this table provides the Parked Address for secondary CPU initialization."
  https://documentation-service.arm.com/static/5fb7e559d77dd807b9a80c8d?token=
- Linux kernel arm64 ACPI docs: "Booting of secondary CPUs using the ACPI
  parking protocol is possible, but discouraged, since only PSCI is supported
  for ARM servers."
  https://android.googlesource.com/kernel/common/+/d2a6fd45c5c4a5c5fdfe6c57f74f630e61d8d9a0%5E2..d2a6fd45c5c4a5c5fdfe6c57f74f630e61d8d9a0/
- FADT ARM Boot Architecture Flags (ACPI 6.0): bit0 PSCI compliant, bit1 use HVC
  (EDK2 `EFI_ACPI_6_0_ARM_PSCI_COMPLIANT` / `..._ARM_PSCI_USE_HVC`, Acpi60.h URL
  above). This is the ONLY way Windows ARM64 discovers the PSCI conduit on ACPI
  systems — there is no ARMH0011-style DT match; the WoA ports all set it.
- Parking mailbox protocol = ACPI spelling of DT spin-table: Lumia README tells
  Linux to use "spin-table with per_cpu_mailbox_addr + 0x8 as the release
  address" (fact, quote above); the ACPI parking protocol uses the same
  mailbox at Parked Address (inference, corroborated by SBBR wording).

## A8. qcpu.sys / winocm — what was found (and dead ends)

- **Dead end**: no public source or MS documentation for `qcpu.sys` found.
  GitHub repo search for "qcpu" → unrelated projects. GitHub API code search
  requires auth. Community driver mirrors (WOA-Project/Qualcomm-Reference-Drivers
  incl. Lenovo/YogaC630 SD850 packages, edk2-porting/sdm845-drivers,
  WOA-Project/MSM8994-8992-NT-ARM64-Drivers) contain **no qcpu.inf/qcpu.sys** —
  the Qualcomm processor device driver ships via Windows Update, not in these
  packages. (fact: tree listings checked via GitHub contents API)
- What those packages do show (fact): `qcpep.wd850.cab` (Qualcomm PEP — Power
  Engine Plug-in, SoC idle/LPI states), `HalExtQCWdogTimer850.cab` (HAL
  extension), `qcscm850.cab` (Qualcomm SCM driver — the Windows-side client of
  the TZ SMC interface in scm_sip_interface.h). No per-core "parking protocol"
  driver in any public WoA repo.
- (inference, marked uncertain) Windows "core parking" is a kernel PPM/scheduler
  feature (same as x86): the kernel decides which cores to park; on ARM64 the
  parked/offlined cores are powered down through the PSCI functions
  (CPU_OFF/CPU_SUSPEND) that the kernel already owns — qcpu.sys is the
  processor device driver for performance states (per-core/capacity/CPPC-like
  QoS), not a firmware parking protocol. No public source found to confirm the
  exact qcpu.sys per-core behavior.
- **Dead end**: winocm (Sarah Purohit) credited in Lumia FADT comment and PR #75
  for the SMC-trap idea; https://github.com/winocm public repos (efi_samples,
  p0sixspwn_lite, winocm.github.io "Not much to see anymore") contain no 835
  PSCI/parking code. The 835 "compute rocket" work appears unpublished.

## A9. boot-shim (Lumia kickstarter) — no parking there

Source: https://github.com/WOA-Project/boot-shim (clone, branch msm8994-pcie).
It is an ELF loader EFI app run from LK; uses `smc #0` (ArmCallSmcHardCoded in
src/ARM/ProcessorSupport.asm) for the secure-monitor transition; no secondary
core / park / release code at all. Secondary cores are simply left wherever the
stock WP10 chain (LK + EL2 hypervisor) parked them — which is why the
"MPPark Code" region at 0x00300000 must be reserved in the UEFI memory map.

## A10. VERDICT (agent A)

1. **Which projects rely on stock EL3 PSCI vs emulate/park:**
   - Renegade (SD845/850/835/855/865: edk2-sdm845, edk2-msm, edk2-msm8998):
     100% stock signed TrustZone PSCI via **SMC**; FADT=0x0001, MADT PPV=0.
     No parking, no PSCI emulation. UEFI never starts APs.
   - Surface Duo (WOA-Project/mu_andromeda_platforms, SD855/888): same —
     FADT PSCI-compliant SMC, PPV=0. Plus kernel-patching for PSCI 1.0
     MEM_PROTECT failing in QHEE.
   - Windows-on-ARM-Experiments (8cx/Surface Pro X): no UEFI repo at all —
     stock MS/QCOM firmware, PSCI provided by it.
   - Lumia 950/950XL (MSM8992/8994): the odd ones. Stock WP10: PSCI lives in
     the **EL2 hypervisor via HVC** (FADT 0x0003), and stock Lumia 950 ACPI
     used the **ACPI CPU parking protocol (PPV=1, spin mailboxes at
     0x301000+)** for secondary cores. Community desktop-Windows builds
     (2020+) switched to: FADT PSCI-SMC (0x0001) + binary-patch of the stock
     EL2 hypervisor vectors to trap SMC (HCR_EL2.TSC) and route PSCI SMCs
     into the stock HVC PSCI handler (PrePi/HvcPatch.h). Reason: Windows
     builds > 19041 broke HVC-based PSCI on these devices (ACPI.sys fails to
     enumerate). 8992 MADT still ships PPV=1 → desktop Windows ARM64 handles
     the parking protocol fine.
2. **Does Windows online cores via PSCI SMC?** Yes — on every modern WoA
   device/port, the Windows kernel (ACPI.sys/ACPI processor devices, ACPI0007 +
   MADT GICC with "Processor Enabled" + MPIDR) brings secondaries online via
   PSCI CPU_ON over the conduit declared in FADT. Firmware's only jobs: boot
   with one core, set FADT ArmBootArch, emit MADT GICC entries (PPV=0),
   reserve/annotate any parked region it leaves behind.
3. **For Exynos 9820 (EL3 monitor exposing PSCI-0.2 IDs via SMC):** this is
   exactly the Qualcomm SD845+ model — the best-supported configuration.
   - Set FADT ArmBootArchFlags = 0x0001 (PSCI compliant, SMC).
   - MADT GICC: PPV=0, ParkedAddress=0, Processor Enabled=1, correct MPIDR per
     core; DSDT/XSDT ACPI0007 processor devices (UIDs matching GICC).
   - Windows will probe PSCI_VERSION; report **0.2** (or ensure PSCI_FEATURES
     reports NOT_SUPPORTED for 1.0 functions) so it never tries MEM_PROTECT —
     the Duo QHEE bugcheck shows a half-working PSCI 1.0 is worse than a
     clean 0.2.
   - Must actually work: CPU_ON, CPU_OFF, CPU_SUSPEND (idle), SYSTEM_OFF,
     SYSTEM_RESET — the Lumia precedent shows missing SYSTEM_OFF/RESET is
     survivable (they reset via PMIC/SCSS register + FADT ResetReg GAS
     instead), but CPU_ON/CPU_OFF are non-negotiable for Windows.
   - The ACPI parking protocol is a viable *fallback* (Windows supports it —
     proven by stock Lumia 950 tables and the still-shipped 8992 MADT), but
     unnecessary if SMC PSCI-0.2 works; and note SBBR's guidance: PPV=0 =
     "PSCI exclusive system" is the normal, recommended shape.
   - Do NOT advertise HVC (bit1) unless an EL2 relay exists; and if the
     monitor's PSCI is only reachable via SMC from EL1 with no hypervisor,
     no trap patch of the Lumia kind is needed at all.

## Sources (agent A)

- https://github.com/WOA-Project/Lumia950XLPkg (clone; README, PrePi/HvcPatch.h,
  PrePi/Pi.c, PrePi/ModuleEntryPoint.S, Lumia950XLPkg.dec, Shared.dsc.inc,
  *.fdf, AcpiTables/*)
- https://github.com/WOA-Project/Lumia950XLPkg/commit/bec3e37abd807787d203c7f0f3c2207f545c72f8
- https://github.com/WOA-Project/Lumia950XLPkg/commit/aa01ee264ff79fe92824114c747ed5b767033fa1
- https://renegade-doc.readthedocs.io/en/latest/edk2/Overview.html
- https://github.com/edk2-porting/edk2-sdm845 ; https://github.com/edk2-porting/edk2-msm ;
  https://github.com/edk2-porting/edk2-msm8998 (clones)
- https://github.com/WOA-Project/mu_andromeda_platforms ;
  https://github.com/Project-Aloha/mu_aloha_platforms (clones)
- https://github.com/WOA-Project/boot-shim (clone)
- https://raw.githubusercontent.com/tianocore/edk2/master/MdePkg/Include/IndustryStandard/Acpi60.h
- https://raw.githubusercontent.com/tianocore/edk2/master/ArmPkg/Drivers/CpuDxe/CpuDxe.c
- https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model.html (403; snippet via search)
- https://uefi.org/sites/default/files/resources/ACPI_5_1release.pdf (snippet via search)
- https://documentation-service.arm.com/static/5fb7e559d77dd807b9a80c8d?token= (SBBR, snippet via search)
- https://android.googlesource.com/kernel/common/+/d2a6fd45c5c4a5c5fdfe6c57f74f630e61d8d9a0%5E2..d2a6fd45c5c4a5c5fdfe6c57f74f630e61d8d9a0/
- https://www.kernel.org/doc/html/next/arm64/arm-acpi.html
- https://github.com/Windows-on-ARM-Experiments (org listing via GitHub API)
- https://github.com/SamuelTulach/rainbow (referenced by KernelErrataPatcher)
- https://chipsandcheese.com/p/state-of-windows-on-arm64-a-high-level-perspective
- GitHub API: orgs/WOA-Project/repos, orgs/edk2-porting/repos, users/winocm/repos,
  repos/WOA-Project/Qualcomm-Reference-Drivers/contents/...
- Dead ends: grep.app (429/Vercel checkpoint), GitHub code search (auth),
  "qcpu.sys" web search (no results), Microsoft public docs on WoA PSCI
  minimum version (none found), WOA-Project/Microsoft-Phones-ACPI (empty repo).

## B. PSCI on Samsung/mainline (agent B)

Status: COMPLETE (2026-10-11) — all five research questions answered below;
one UNVERIFIED constant remains (9820 monitor's PSCI_VERSION answer), with
the on-device read that closes it logged at the end. Research questions:
mainline Exynos4/5/7 secondary-core bring-up (samsung,exynos4210-smp
enable-method vs PSCI), PSCI 0.1 vs 0.2/1.0 function-ID differences, how
Linux picks PSCI version on ACPI, and what the Exynos 9810/9820 Android
kernels' psci DT nodes actually say.

### Progress log

- (started 2026-10-11) Section created; beginning with mainline dtsi/binding
  reads on elixir.bootlin.com.

#### Q1 — mainline Exynos bring-up methods

**FACT — Exynos7 (arm64) uses PSCI, method=smc, with 0.2-style IDs under a
bare `arm,psci` compatible.** Current mainline
`arch/arm64/boot/dts/exynos/exynos7.dtsi` (fetched from raw.githubusercontent,
torvalds/linux master):

```dts
psci {
        compatible = "arm,psci";
        method = "smc";
        cpu_off = <0x84000002>;
        cpu_on = <0xc4000003>;
};
```

All four Cortex-A57 cores have `enable-method = "psci"`. Source:
https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/exynos/exynos7.dtsi
(same content viewable at
https://elixir.bootlin.com/linux/latest/source/arch/arm64/boot/dts/exynos/exynos7.dtsi).

Note (inference): `0x84000002` = PSCI 0.2 **CPU_OFF**, `0xc4000003` = PSCI 0.2
**CPU_ON (64-bit)** — i.e. the DT declares the legacy bare compatible but
carries *0.2 function IDs*, exactly the pattern we saw on Exynos 9820. No
`cpu_suspend`/`migrate` cells are given in mainline exynos7.dtsi.

**FACT — binding doc `Documentation/devicetree/bindings/arm/psci.yaml`**
(https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/arm/psci.yaml):
- `compatible = "arm,psci"` → "For implementations complying to PSCI versions
  prior to 0.2"; when bare `arm,psci` is present, `cpu_off` and `cpu_on` are
  **required** cells (the allOf/if rule at the bottom of the yaml).
- `"arm,psci-0.2"` → "Function IDs are not required and should be ignored by
  an OS with PSCI 0.2 support, but are permitted to be present for
  compatibility with existing software when `arm,psci` is later in the
  compatible list."
- `"arm,psci-1.0"` → backward compatible with 0.2.
- `method` enum: `smc` or `hvc`; optional cells `cpu_suspend`, `cpu_off`,
  `cpu_on`, `migrate`, `arm,psci-suspend-param`; `power-domain-*` subnodes for
  the hierarchical idle-state model (PSCI 1.0 OSI mode mentioned).
- Example "Case 1: PSCI v0.1 only" shows vendor-style IDs like
  `cpu_suspend = <0x95c10000>` — the 0.1 convention where the *firmware
  supplies* the function IDs (0.1 IDs are implementation-defined, unlike 0.2's
  fixed IDs).

**FACT — Exynos4/5 (ARM32) mainline uses a holding-pen + PMU-register boot,
not PSCI.** `arch/arm/mach-exynos/platsmp.c` (torvalds/linux master,
https://raw.githubusercontent.com/torvalds/linux/master/arch/arm/mach-exynos/platsmp.c):
- `exynos_boot_secondary()` writes `__pa_symbol(exynos4_secondary_startup)`
  into a boot register: `cpu_boot_reg_base()` returns `sysram_base_addr`
  (Exynos4412: `+ 4*cpu`; Exynos5420/5800: `+ 4`), except Exynos4210 rev1.1
  which uses `pmu_base_addr + S5P_INFORM5`.
- Power is enabled by poking PMU registers directly:
  `exynos_cpu_power_up()` writes `S5P_CORE_LOCAL_PWR_EN` into
  `EXYNOS_ARM_CORE_CONFIGURATION(cpu)`; `exynos_cluster_power_up()` writes to
  `EXYNOS_COMMON_CONFIGURATION(cluster)`; hotplug `cpu_die` does
  `exynos_cpu_power_down()` + `wfi()` loop.
- Wakeup is an IPI (`arch_send_wakeup_ipi_mask`) after `call_firmware_op(cpu_boot, core_id)`
  — the firmware op path exists for Exynos SoCs behind Samsung SEC (TrustZone)
  firmware (set_cpu_boot_addr via firmware first, fall back to the boot reg).
- The file now carries `/* XXX exynos_pen_release is cargo culted code - DO
  NOT COPY XXX */` comments — mainline treats this path as legacy.
- SoC power-domain/PMU syscon node in `arch/arm/boot/dts/samsung/exynos4.dtsi`:
  `pmu_system_controller: system-controller@10020000 { compatible =
  "samsung,exynos4210-pmu", "simple-mfd", "syscon"; reg = <0x10020000 0x4000>; }`
  (https://raw.githubusercontent.com/torvalds/linux/master/arch/arm/boot/dts/samsung/exynos4.dtsi).
  The `cpus` node is per-SoC (exynos4210.dtsi / exynos4412.dtsi) — checked next.

**FACT — Exynos5433 (arm64, 2016) also uses PSCI with the same bare-compatible
pattern.** `arch/arm64/boot/dts/exynos/exynos5433.dtsi` (master): all 8 CPUs
`enable-method = "psci"`; node:

```dts
psci {
        compatible = "arm,psci";
        method = "smc";
        cpu_off = <0x84000002>;
        cpu_on = <0xc4000003>;
};
```

(https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/exynos/exynos5433.dtsi;
identical node already in v5.4:
https://raw.githubusercontent.com/torvalds/linux/v5.4/arch/arm64/boot/dts/exynos/exynos5433.dtsi).

**FACT — "samsung,exynos4210-smp" as an enable-method does NOT exist in
mainline.** Checked: master `Documentation/devicetree/bindings/arm/cpus.yaml`
enable-method enum lists `psci`/`spin-table` (required on arm64) plus a set of
ARM32 vendor methods — no samsung entry (only `samsung,mongoose-m*` as CPU
*compatible* strings).
https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/arm/cpus.yaml
Also no `enable-method` in v4.9/v5.15/master exynos4210/4412/5250/5420 dtsi
cpu nodes — Exynos4/5 ARM32 boot is driven by `exynos_smp_ops` registered by
mach-exynos (machine descriptor), not by a DT enable-method. The related real
DT binding is the SYSRAM one: `Documentation/devicetree/bindings/sram/samsung-sram.txt`
with compatibles `samsung,exynos4210-sysram` / `samsung,exynos4210-sysram-ns`
(secondary bring-up code lives in SYSRAM;
https://zircon-guest.googlesource.com/third_party/linux/+/4697abe3837e4cc6dfb2c50f6c46b2d426f2ae90/Documentation/devicetree/bindings/sram/samsung-sram.txt).
(Inference: the "samsung,exynos4210-smp" string in our task brief is likely a
mis-remembering of `samsung,exynos4210-sysram` / the SEC-firmware `cpu_boot`
firmware op; I found no mainline use of an `exynos4210-smp` enable-method.)

#### Q5 — Exynos 9810 Android kernel DT psci node (LineageOS mirror)

**FACT — Exynos 9810 uses bare `arm,psci` + smc + PSCI-0.2 IDs, same as 9820.**
From `LineageOS/android_kernel_samsung_exynos9810` (branch `lineage-20`,
decompiled board DT `arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_16.dts`,
https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9810/lineage-20/arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_16.dts):

```dts
psci {
        compatible = "arm,psci";
        method = "smc";
        cpu_suspend = <0xc4000001>;
        cpu_off = <0x84000002>;
        cpu_on = <0xc4000003>;
};
```

- All 8 cores `enable-method = "psci"`; 4x `compatible = "arm,ananke"` at
  reg 0x0–0x3 and 4x `"arm,meerkat"` at reg 0x100–0x103. **Correction
  (verified later):** "Meerkat" is the Exynos **M3** big core (Hot Chips 2018,
  per https://en.wikipedia.org/wiki/Exynos_(system_on_chip) and
  https://chipswiki.org/Samsung/exynos/9810), so the big M3 cluster sits at
  MPIDR 0x100–0x103 (Aff1=1) and `arm,ananke` at 0x0–0x3 is the Cortex-A55
  LITTLE cluster (inference from core counts; gs101's mainline dtsi also
  labels its A55 cluster "ananke"). MPIDR layout: LITTLE in Aff1=0, big in
  Aff1=1.
- idle-states: `entry-method = "arm,psci"`, `arm,psci-suspend-param = <0x10000>`
  (= bit16 Type=1 memory power-down, Level=0 core, per PSCI power_state layout)
  — i.e. CPU_SUSPEND is used for core idle on 9810.
- `exynos-pm` node has Samsung vendor hooks: `suspend_psci_idx = <0x85>`,
  `cp_call_psci_idx = <0x85>`, `suspend_mode_idx = <0x8>` — vendor SMC indices
  used by the downstream exynos-pm suspend driver (check driver next).
- (Inference) `0xc4000001` = PSCI 0.2 CPU_SUSPEND under the 64-bit SMC
  convention (bit31=1), `0x84000002` = CPU_OFF, `0xc4000003` = CPU_ON(64-bit):
  all three are **PSCI 0.2 function IDs**, despite the bare `arm,psci`
  compatible that the binding reserves for pre-0.2. Confirms the 9820 pattern
  one generation earlier.

#### Q3 — PSCI function IDs 0.1 vs 0.2/1.0 (confirmed from Linux uapi header)

**FACT — `include/uapi/linux/psci.h`** (https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/psci.h):

```c
#define PSCI_0_2_FN_BASE    0x84000000
#define PSCI_0_2_FN(n)      (PSCI_0_2_FN_BASE + (n))
#define PSCI_0_2_64BIT      0x40000000
#define PSCI_0_2_FN64_BASE  (PSCI_0_2_FN_BASE + PSCI_0_2_64BIT)
PSCI_0_2_FN_PSCI_VERSION    = FN(0) = 0x84000000
PSCI_0_2_FN_CPU_SUSPEND     = FN(1) = 0x84000001
PSCI_0_2_FN_CPU_OFF         = FN(2) = 0x84000002
PSCI_0_2_FN_CPU_ON          = FN(3) = 0x84000003
PSCI_0_2_FN_AFFINITY_INFO   = FN(4) = 0x84000004
PSCI_0_2_FN_MIGRATE         = FN(5) = 0x84000005
PSCI_0_2_FN_MIGRATE_INFO_TYPE   = FN(6)
PSCI_0_2_FN_MIGRATE_INFO_UP_CPU = FN(7)
PSCI_0_2_FN_SYSTEM_OFF      = FN(8) = 0x84000008
PSCI_0_2_FN_SYSTEM_RESET    = FN(9) = 0x84000009
PSCI_0_2_FN64_CPU_SUSPEND   = 0xC4000001
PSCI_0_2_FN64_CPU_ON        = 0xC4000003
PSCI_1_0_FN_PSCI_FEATURES   = FN(10); CPU_FREEZE=FN(11); CPU_DEFAULT_SUSPEND=FN(12);
   NODE_HW_STATE=FN(13); SYSTEM_SUSPEND=FN(14); SET_SUSPEND_MODE=FN(15);
   STAT_RESIDENCY=FN(16); STAT_COUNT=FN(17)
PSCI_1_1_FN_SYSTEM_RESET2   = FN(18); MEM_PROTECT=FN(19); MEM_PROTECT_CHECK_RANGE=FN(20)
PSCI_1_3_FN_SYSTEM_OFF2     = FN(21)
power_state: ID mask 0xffff bits0-15, TYPE bit16 (0x1<<16), AFFL bits24-25
```

So the task brief's 0.2 numbering is **confirmed** (PSCI_VERSION 0x84000000,
CPU_SUSPEND 0x84000001, CPU_OFF 0x84000002, CPU_ON 0x84000003 / FN64
0xC4000003).

**FACT — PSCI 0.1 function IDs are implementation-defined, not fixed.** The
binding example uses vendor values (`0x95c10000`-style), and Linux's 0.1 path
reads them from DT: `drivers/firmware/psci/psci.c` `psci_0_1_init()` —
"Using PSCI v0.1 Function IDs from DT", reads `cpu_suspend`/`cpu_off`/`cpu_on`/
`migrate` cells into `psci_0_1_function_ids` (https://raw.githubusercontent.com/torvalds/linux/master/drivers/firmware/psci/psci.c).
**Inference:** the "0.1 IDs" `CPU_SUSPEND 0x84000000 / CPU_OFF 0x84000001 /
CPU_ON 0x84000002` quoted in the task brief are the *ARM reference v0.1
implementation's* numbering (DEN0022A); 0.2 shifted everything by inserting
PSCI_VERSION at 0. This shift is exactly why a 0.1-style monitor and a
0.2-style kernel mismatch is fatal — CPU_ON/CPU_OFF get swapped semantics.
(Still to verify against DEN0022A text if obtainable.)

**FACT — DT match table in `psci.c`:**

```c
static const struct of_device_id psci_of_match[] __initconst = {
    { .compatible = "arm,psci",     .data = psci_0_1_init},
    { .compatible = "arm,psci-0.2", .data = psci_0_2_init},
    { .compatible = "arm,psci-1.0", .data = psci_1_0_init},
};
```

i.e. bare `arm,psci` → Linux takes the **0.1 path** and uses the DT cells
verbatim (no PSCI_VERSION probe, `get_version` hardcodes `PSCI_VERSION(0,1)`).
Samsung's DTs put 0.2 IDs in those cells, so the driver ends up calling 0.2
IDs anyway. `arm,psci-0.2` → `psci_0_2_init` → `psci_probe()` calls
`PSCI_0_2_FN_PSCI_VERSION` and prints "PSCIv%d.%d detected in firmware",
rejects versions < 0.2 ("Conflicting PSCI version detected"), then
`psci_0_2_set_functions()` installs the fixed 0.2 IDs + registers
`pm_power_off = psci_sys_poweroff` and a restart handler (SYSTEM_OFF /
SYSTEM_RESET). For >= 1.0 it additionally probes PSCI_FEATURES for
CPU_SUSPEND/OSI, SYSTEM_SUSPEND, SYSTEM_RESET2, SYSTEM_OFF2.

#### Q4 — PSCI on ACPI ARM64 (what Windows-like firmwares see)

**FACT — `psci_acpi_init()` in `drivers/firmware/psci/psci.c`:**

```c
/*
 * We use PSCI 0.2+ when ACPI is deployed on ARM64 and it's
 * explicitly clarified in SBBR
 */
int __init psci_acpi_init(void)
{
    if (!acpi_psci_present()) { ... return -EOPNOTSUPP; }
    pr_info("probing for conduit method from ACPI.\n");
    if (acpi_psci_use_hvc())
        set_conduit(SMCCC_CONDUIT_HVC);
    else
        set_conduit(SMCCC_CONDUIT_SMC);
    return psci_probe();
}
```

(https://raw.githubusercontent.com/torvalds/linux/master/drivers/firmware/psci/psci.c).
So on ACPI systems Linux **assumes PSCI 0.2+** (per SBBR requirement), takes
the conduit from the FADT "PSCI Profile" flags (HVC bit), and then *probes*
the real version with the PSCI_VERSION call (0x84000000) via `psci_probe()`;
a firmware answering < 0.2 (or NOT_SUPPORTED → `PSCI_RET_NOT_SUPPORTED` =
-1, which prints as a bogus version and trips the <0.2 check) is rejected.
- (Inference) An ACPI/SBBR-conformant Exynos 9820 firmware must therefore
implement at least PSCI 0.2 with the standard IDs and answer PSCI_VERSION —
this is the same contract Windows on ARM64 assumes.

#### Q3 (cont.) — PSCI spec itself (DEN0022E fetched, mirrors DEN0022C tables)

**FACT — ARM PSCI spec, DEN0022E (2023, "Arm Power State Coordination
Interface, Platform Design Document", copy at
https://www.scs.stanford.edu/~zyedidia/docs/arm/psci.pdf, text extracted to
`/tmp/opencode/psci/psci.txt`):**
- Release history table: Issue **A = 13 Aug 2012** "First release" (this is
  PSCI 0.1); Issue **B = 24 Jun 2013** "Second release, PSCI version 0.2",
  first bullet of changes: **"Provided values for Function ID parameter of
  all functions."** → in 0.1 the function IDs were NOT fixed by the spec.
- §5.1.1 PSCI_VERSION: Function ID **0x84000000**, return = major[31:16] /
  minor[15:0]. §5.1.2 CPU_SUSPEND: **0x84000001 (SMC32) / 0xC4000001 (SMC64)**.
  (Matches the Linux uapi header above; CPU_OFF 0x84000002, CPU_ON
  0x84000003/0xC4000003 follow the same FN(n) table.)
- Table 17 "Mandatory and optional functions": for PSCI 0.2, 1.0 and 1.1/1.2
  **PSCI_VERSION, CPU_SUSPEND, CPU_OFF, CPU_ON, AFFINITY_INFO, SYSTEM_OFF,
  SYSTEM_RESET are Mandatory**; MIGRATE / MIGRATE_INFO_TYPE /
  MIGRATE_INFO_UP_CPU are Optional. → A "PSCI 0.2 compliant" monitor MUST
  have SYSTEM_OFF/SYSTEM_RESET — which Samsung's 7420 monitor demonstrably
  lacks (see Q2), so Samsung's monitor is a 0.2-*flavored* subset, not
  spec-compliant.
- Table 6 maps spec issues to PSCI major.minor version numbers (0.2, 1.0, …).

**FACT — Linux's own comment on 0.1** (`include/uapi/linux/psci.h`,
https://github.com/torvalds/linux/blob/master/include/uapi/linux/psci.h):
"PSCI v0.1 interface — The PSCI v0.1 function numbers are implementation
defined. Only PSCI return values … are applicable to PSCI v0.1."
U-Boot's original v0.1 header used base `0x95c1ba5e` for Layerscape
(https://lists.u-boot-project.org/pipermail/u-boot/2014-August/187364.html) —
proof by example that 0.1 IDs were arbitrary per vendor.
**Status of the brief's "0.1 IDs" (CPU_SUSPEND 0x84000000, CPU_OFF
0x84000001, CPU_ON 0x84000002):** could NOT verify against DEN0022A itself
(not publicly hosted); they are consistent with ARM's *reference* v0.1
numbering that 0.2 renumbered by inserting PSCI_VERSION. Treat as
**UNVERIFIED** for spec text, but the key operative fact is verified: 0.1 IDs
are implementation-defined, 0.2+ IDs are fixed, and the two sets differ by a
one-slot shift for SUSPEND/OFF/ON.

#### Q2 — Samsung monitor quirks: PSCI 0.1 vs 0.2, SYSTEM_OFF/RESET

**FACT — mainline Exynos7 (7420) was downgraded from `arm,psci-0.2` to bare
`arm,psci` in Nov 2020 because Samsung's EL3 monitor does NOT implement
SYSTEM_OFF/SYSTEM_RESET.** Commit `e1e47fbca668`
"arm64: dts: exynos: Correct psci compatible used on Exynos7"
(Paweł Chmiel, merged for v5.11, marked Fixes: b9024cbc937d/fb026cb65247):

> "It's not possible to reboot or poweroff Exynos7420 using PSCI. Instead we
> need to use syscon reboot/poweroff drivers, like it's done for other Exynos
> SoCs. This was confirmed by checking vendor source and testing it on Samsung
> Galaxy S6 device based on this SoC.
> To be able to use custom restart/poweroff handlers instead of PSCI
> functions, we need to correct psci compatible. This also requires us to
> provide function ids for CPU_ON and CPU_OFF."

Diff: `compatible = "arm,psci-0.2"` → `compatible = "arm,psci"` plus
`cpu_off = <0x84000002>; cpu_on = <0xC4000003>;`.
Companion patch in the same series: `73bc7510ea0d` "arm64: dts: exynos:
Include common syscon restart/poweroff for Exynos7" (reboot/poweroff via PMU
syscon registers instead).
Sources: https://api.github.com/repos/torvalds/linux/commits/e1e47fbca668 ,
https://api.github.com/repos/torvalds/linux/commits/73bc7510ea0d ,
lore thread id 20201107133926.37187 (lore.kernel.org itself is bot-blocked).

**FACT — version timeline of the Exynos7 psci node:**
- v4.0 (initial, Dec 2014): `compatible = "arm,psci-0.2"; method = "smc";`
  (https://raw.githubusercontent.com/torvalds/linux/v4.0/arch/arm64/boot/dts/exynos/exynos7.dtsi)
- v5.9: unchanged.
- v5.11+ (today master): bare `arm,psci` + 0.2-style cpu_off/cpu_on cells
  (see Q1 above).
- Exynos5433 dtsi as first added (v4.10, commit 5f04c4cfcc92 "arm64: dts:
  exynos: Add dtsi files for Samsung Exynos5433 64bit SoC"): already bare
  `arm,psci` + `cpu_off = <0x84000002>` + `cpu_on = <0xC4000003>`
  (https://raw.githubusercontent.com/torvalds/linux/v4.10/arch/arm64/boot/dts/exynos/exynos5433.dtsi).

**Inference from the above facts:** Samsung's arm64 monitors (7420/5433/9810/
9820 lineage) implement a *subset* of PSCI 0.2 — CPU_ON/CPU_OFF/CPU_SUSPEND
with standard 0.2 IDs over SMC — but not SYSTEM_OFF/SYSTEM_RESET (those are
done by writing PMU registers, e.g. via the syscon poweroff/restart drivers),
and the DT deliberately declares bare `arm,psci` so Linux's 0.1 code path
installs no PSCI poweroff/restart handlers. The 2020 fix also shows the
0.2-style IDs themselves are what the real Samsung monitor accepts (they were
kept verbatim when switching to the 0.1 path).

#### Q5 (cont.) — 9820 DT + Samsung vendor PSCI extensions in the Android kernels

**FACT — Exynos 9820 psci node** (`LineageOS/android_kernel_samsung_exynos9820`,
branch `lineage-23.2`, `arch/arm64/boot/dts/exynos/exynos9820.dts` line ~595,
https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/arch/arm64/boot/dts/exynos/exynos9820.dts):

```dts
psci {
        compatible = "arm,psci";
        method = "smc";
        cpu_suspend = <0xC4000001>;
        cpu_off = <0x84000002>;
        cpu_on = <0xC4000003>;
};
```

- 8 CPUs, all `enable-method = "psci"`: cpu0–3 `"arm,ananke"` reg 0x0–0x3,
  cpu4–5 `"arm,prometheus"` reg 0x4–0x5, cpu6–7 `"arm,cheetah"` reg 0x100–0x101.
  Mapping (verified): 9820 is 2x Exynos M4 + 2x Cortex-A75 + 4x Cortex-A55
  (https://semiconductor.samsung.com/processor/mobile-processor/exynos-9-series-9820),
  and **M4 = "Cheetah"** (https://en.wikipedia.org/wiki/Exynos_(system_on_chip),
  https://chipswiki.org/Exynos_9820), so the 2 cheetah cores at MPIDR
  0x100/0x101 are the M4 big cluster (Aff1=1); ananke (4) = Cortex-A55 and
  prometheus (2) = Cortex-A75 by elimination (inference from counts).
  Matches the cpu-driver-9820 notes' MPIDR set {0x0–0x5, 0x100–0x101}.
- idle-states `entry-method = "arm,psci"`, `arm,psci-suspend-param = <0x0010000>`
  (Type bit16=1 → core power-down).
- `cpupm` nodes: `cpd_cl0`/`cpd_cl1` `psci-index = <128>` (cluster power-down),
  `sicd` `psci-index = <256>` (system idle).
- `exynos-pm`: `suspend_psci_idx = <1024> /* PSCI_SYSTEM_SLEEP */`,
  `cp_call_psci_idx = <1024>`, `suspend_mode_idx = <8>`.

**FACT — Samsung's downstream psci driver extends CPU_SUSPEND with vendor
power states.** `include/linux/psci.h` (9820 mirror):

```c
#define PSCI_CUSTOMIZED_INDEX   (1 << 7)   /* 128 */
#define PSCI_CLUSTER_SLEEP      (PSCI_CUSTOMIZED_INDEX) /* 128 */
#define PSCI_SYSTEM_IDLE        (1 << 8)   /* 256 */
#define PSCI_CP_CALL            (1 << 9)   /* 512 */
#define PSCI_SYSTEM_SLEEP       (1 << 10)  /* 1024 */
```

(https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/include/linux/psci.h;
9810 uses a slightly different numbering — `PSCI_UNUSED_INDEX=128`,
CLUSTER_SLEEP=128, SYSTEM_IDLE=129, SYSTEM_IDLE_CLUSTER_SLEEP=130,
SYSTEM_IDLE_AUDIO=131, CP_CALL=132, SYSTEM_SLEEP=133 — in
https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9810/lineage-20/include/linux/psci.h,
which is why its DT says `suspend_psci_idx = <0x85>` = 133 = PSCI_SYSTEM_SLEEP).
`drivers/firmware/psci.c` (both kernels) has
`psci_suspend_customized_finisher()` — "This function is only for Exynos" —
which maps those indices to a packed PSCI power_state
(`psci_power_state_pack(id, type, affinity_level)`) and calls
`psci_ops.cpu_suspend(state, __pa_symbol(cpu_resume))`:
CLUSTER_SLEEP → affinity_level=1, CP_CALL → 2, SYSTEM_SLEEP → 3,
SYSTEM_IDLE → id=1. So **system suspend on 9810/9820 = PSCI 0.2 CPU_SUSPEND
(0xC4000001) with power_state.AffLevel=3** — a vendor extension (standard PSCI
0.2 CPU_SUSPEND takes affinity levels ≤ caller's own; level 3 "system" is not
a standard CPU_SUSPEND target; PSCI 1.0 introduced SYSTEM_SUSPEND 0x8400000E
for that instead). `drivers/soc/samsung/exynos-cpupm.c` drives cluster
power-down (cpd/sicd) through the same `psci-index` mechanism.
Sources:
https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/firmware/psci.c
https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/soc/samsung/exynos-cpupm.c
https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9810/lineage-20/drivers/firmware/psci.c
https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9810/lineage-20/drivers/soc/samsung/exynos-pm.c
(9810 `exynos-pm.c` reads `suspend_psci_idx` from DT and passes it to
`psci_cpu_suspend_enter`.)

**FACT — no cpu-boot-flow / `exynos_cpu_power_up` remnants in the 9810/9820
arm64 kernels' PSCI path**: secondary bring-up is entirely `enable-method =
"psci"`; the old PMU-register boot code (`exynos_cpu_power_up` etc.) exists
only in the ARM32 `arch/arm/mach-exynos` tree (mainline platsmp.c, see Q1),
which these arm64 kernels don't use for the app cores.

**FACT — Exynos4/5 behind TrustZone used Samsung's own SMC command set, not
PSCI.** `arch/arm/mach-exynos/firmware.c` (master,
https://raw.githubusercontent.com/torvalds/linux/master/arch/arm/mach-exynos/firmware.c):
`exynos_cpu_boot()` sends `exynos_smc(SMC_CMD_CPU1BOOT, cpu, 0, 0)` (4210/4212/4412/manta),
idle uses `SMC_CMD_CPU0AFTR` / `SMC_CMD_SLEEP` / `SMC_CMD_SAVE`+`SMC_CMD_SHUTDOWN`
(AFTR = "Arm Fault Tolerant Return" style deep idle), boot addr via SYSRAM
`EXYNOS_BOOT_ADDR 0x8`/`EXYNOS_BOOT_FLAG 0xc`. DT binding:
`Documentation/devicetree/bindings/arm/samsung/samsung-secure-firmware.yaml`
(`compatible = "samsung,secure-firmware"`, reg = non-secure SYSRAM mailbox;
https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/arm/samsung/samsung-secure-firmware.yaml).
So pre-arm64 Exynos had a *vendor* SMC ABI; PSCI (0.2-ID subset) starts with
the arm64 parts (Exynos7/5433) and continues through 9810/9820.

**FACT — how Linux decides PSCI on ACPI (Q4 detail):** `psci_acpi_init()`'s
helpers live in `arch/arm64/kernel/acpi.c`:

```c
bool __init acpi_psci_present(void)
{
    return acpi_gbl_FADT.arm_boot_flags & ACPI_FADT_PSCI_COMPLIANT;
}
/* Whether HVC must be used instead of SMC as the PSCI conduit */
bool acpi_psci_use_hvc(void)
{
    return acpi_gbl_FADT.arm_boot_flags & ACPI_FADT_PSCI_USE_HVC;
}
```

(https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/kernel/acpi.c).
i.e. ACPI firmware advertises PSCI via the **FADT ARM Boot Flags** bits
("PSCI Compliant", "PSCI Use HVC"); Linux then assumes 0.2+ (SBBR) and
confirms/probes the version with the PSCI_VERSION call. There is no ACPI
mechanism to negotiate 0.1-style DT function-ID cells — 0.1 is a
DT-only world. (Fact from code; the SBBR requirement itself is asserted in
the psci.c comment quoted above.)

**FACT — driver refactor context:** linux-next log for
`drivers/firmware/psci` shows David Brazdil's 2020-12-04 series
"psci: Split functions to v0.1 and v0.2+ variants", "psci: Support
psci_ops.get_version for v0.1", "psci: Add accessor for
psci_0_1_function_ids" (https://git.kerneltesting.org/next/linux-next/log/drivers/firmware/psci)
— the 0.1 vs 0.2 split in the driver is deliberate and recent.

**FACT — the newest Samsung-derived SoC declares full PSCI 1.0:** Google
Tensor (gs101, Exynos-based) mainline `gs101.dtsi` has
`psci { compatible = "arm,psci-1.0"; method = "smc"; }`, all CPUs
`enable-method = "psci"`, idle-states `arm,psci-suspend-param = <0x0010000>`
(same C2 encoding as 9810/9820) — see
https://cgit.freebsd.org/src/tree/src/arm64/exynos/google/gs101.dtsi .
Related LAKML thread (2025-07) "soc: samsung: exynos-pmu: Enable CPU Idle for
gs101" discusses working "around firmware limitations in the PSCI
implementation" on gs101 (extra ACPM hints via CPU_INFORM registers needed
beside PSCI calls):
https://lists.infradead.org/pipermail/linux-arm-kernel/2025-July/1043629.html

**Dead ends (so we don't re-search):** lore.kernel.org and
elixir.bootlin.com are Anubis bot-blocked for fetching; marc.info GET search
does not execute; grep.app API 429s; GitHub commit-search found no
"samsung,exynos4210-smp" enable-method anywhere; the 2015-era "Cho Hasoo"
Exynos7 suspend LAKML thread was not locatable via web search (the operative
Exynos7 PSCI facts above came from git history instead: commits b9024cbc937d,
fb026cb65247, 73bc7510ea0d, e1e47fbca668).

#### What this means for Windows on the Exynos 9820 (inference)

All of this is **inference** from the sourced facts above, flagged as such:

1. **The 9820 EL3 monitor speaks PSCI-0.2 IDs over SMC #0.** Every Samsung
   arm64 part from Exynos7 to 9820 ships DT cells with exactly
   CPU_SUSPEND 0xC4000001, CPU_OFF 0x84000002, CPU_ON 0xC4000003, and the
   2020 Exynos7 fix kept those IDs while downgrading the compatible — i.e.
   those IDs are what the silicon's bootloader-era monitor actually accepts.
   Windows on ARM64 onlines parked cores via PSCI CPU_ON (SMC64 0xC4000003,
   x0=ID, x1=MPIDR, x2=entry, x3=context-id); that matches the 9820 monitor's
   vocabulary. The MPIDRs to use are the DT ones (0x0–0x5, 0x100–0x101).
2. **Do not trust PSCI_VERSION / PSCI_FEATURES / SYSTEM_OFF / SYSTEM_RESET on
   Samsung monitors.** 7420's monitor lacks SYSTEM_OFF/RESET (proven, Q2);
   the bare `arm,psci` compatible means Samsung never claimed 0.2 compliance,
   and PSCI_VERSION support on 9810/9820 monitors is unverified. An ACPI
   image that sets FADT "PSCI Compliant" and whose kernel/OS then probes
   PSCI_VERSION (Linux does; Windows' PSCI usage is similar per SBBR) could
   fail at init if the monitor returns SMC_UNK (-1) for 0x84000000. Before
   declaring PSCI in ACPI on our port, probe from U-Boot/Linux first: call
   0x84000000 and check the return; if it's -1, Windows/Linux 0.2-style
   probing breaks and we'd need to know exactly what the monitor answers.
   (This is the single most important read to close — mark UNVERIFIED until
   done on device.)
3. **CPU_OFF/CPU_ON are the safe subset; suspend is vendor-land.** Windows
   parking/offline uses CPU_OFF (0x84000002) — same ID Samsung's own kernel
   uses. CPU_SUSPEND with standard power_state (0x10000 = core retention/off,
   as in the 9820 idle-states) is what Samsung's cpuidle uses, so core idle
   via PSCI works; but Samsung's *system* suspend abuses CPU_SUSPEND with
   AffLevel=3 vendor encodings — Windows will never issue those, so system
   suspend on WoA must be done via the vendor SMC path (exynos-smc/ACPM) or
   not at all; don't expect SYSTEM_SUSPEND (1.0) to exist.
4. **Version floor:** for our ACPI/MAT work, assume "PSCI 0.2 function IDs,
   conduit SMC, no guaranteed PSCI_VERSION/SYSTEM_OFF/RESET/PSCI_FEATURES" —
   not 1.0. If the device probe (point 2) shows PSCI_VERSION works and
   reports >= 1.0, we can upgrade that assumption; gs101 shows Samsung's
   newer monitors do full 1.0, but the 9820's BL monitor predates that
   generation.

### Sources (agent B)

- https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/exynos/exynos7.dtsi
- https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/arm/psci.yaml
- https://raw.githubusercontent.com/torvalds/linux/master/arch/arm/mach-exynos/platsmp.c
- https://raw.githubusercontent.com/torvalds/linux/master/arch/arm/boot/dts/samsung/exynos4.dtsi
- https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/arm/cpus.yaml
- https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/exynos/exynos5433.dtsi
- https://raw.githubusercontent.com/torvalds/linux/v4.0/arch/arm64/boot/dts/exynos/exynos7.dtsi
- https://raw.githubusercontent.com/torvalds/linux/v4.10/arch/arm64/boot/dts/exynos/exynos5433.dtsi
- https://api.github.com/repos/torvalds/linux/commits/e1e47fbca668
- https://api.github.com/repos/torvalds/linux/commits/73bc7510ea0d
- https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/psci.h
- https://raw.githubusercontent.com/torvalds/linux/master/drivers/firmware/psci/psci.c
- https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/kernel/acpi.c
- https://raw.githubusercontent.com/torvalds/linux/master/arch/arm/mach-exynos/firmware.c
- https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/arm/samsung/samsung-secure-firmware.yaml
- https://www.scs.stanford.edu/~zyedidia/docs/arm/psci.pdf (DEN0022E, PSCI spec)
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9810/lineage-20/arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_16.dts
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9810/lineage-20/drivers/firmware/psci.c
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9810/lineage-20/include/linux/psci.h
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9810/lineage-20/drivers/soc/samsung/exynos-pm.c
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/arch/arm64/boot/dts/exynos/exynos9820.dts
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/include/linux/psci.h
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/firmware/psci.c
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/soc/samsung/exynos-cpupm.c
- https://cgit.freebsd.org/src/tree/src/arm64/exynos/google/gs101.dtsi
- https://lists.infradead.org/pipermail/linux-arm-kernel/2025-July/1043629.html
- https://git.kerneltesting.org/next/linux-next/log/drivers/firmware/psci
- https://lists.u-boot-project.org/pipermail/u-boot/2014-August/187364.html
- https://zircon-guest.googlesource.com/third_party/linux/+/4697abe3837e4cc6dfb2c50f6c46b2d426f2ae90/Documentation/devicetree/bindings/sram/samsung-sram.txt
- https://en.wikipedia.org/wiki/Exynos_(system_on_chip) (M3="Meerkat" Hot Chips 2018, M4="Cheetah")
- https://chipswiki.org/Samsung/exynos/9810 , https://chipswiki.org/Exynos_9820 (core names/clusters)
- https://semiconductor.samsung.com/processor/mobile-processor/exynos-9-series-9820 (2x custom + 2x A75 + 4x A55)

### Progress log (agent B) — closing

- (2026-10-11) Q1 done: Exynos4/5 = holding-pen + PMU regs + SEC SMC (no
  enable-method in DT; "samsung,exynos4210-smp" does not exist in mainline);
  Exynos7/5433 = bare arm,psci + 0.2 IDs, smc.
- (2026-10-11) Q2 done: e1e47fbca668 proves 7420 monitor lacks PSCI
  SYSTEM_OFF/RESET; timeline v4.0 arm,psci-0.2 → v5.11 bare arm,psci.
  Cho Hasoo 2015 thread not found (dead ends logged).
- (2026-10-11) Q3 done: 0.2/1.0 IDs confirmed from uapi header + DEN0022E;
  0.1 IDs implementation-defined (DEN0022E rev-history bullet + uapi comment);
  brief's specific 0.1 numbering left UNVERIFIED vs DEN0022A.
- (2026-10-11) Q4 done: psci_acpi_init assumes 0.2+ (SBBR), FADT
  arm_boot_flags give conduit, psci_probe() calls PSCI_VERSION.
- (2026-10-11) Q5 done: 9810 + 9820 DT psci nodes captured (bare arm,psci,
  smc, 0.2 IDs incl. cpu_suspend 0xC4000001); vendor suspend indices
  (suspend_psci_idx 0x85/1024) and psci_suspend_customized_finisher
  (AffLevel 1/2/3) documented.
- Open follow-up for the mainline session: on-device probe of SMC
  0x84000000 (PSCI_VERSION) and 0x84000008/9 (SYSTEM_OFF/RESET — do NOT call
  OFF/RESET on the S10e; only FEATURES/VERSION style reads) to close the
  UNVERIFIED monitor-capability constants.

## C. Parent-session verification + seed facts (merged 2026-10-11)

### C1. VERIFIED anomaly in A1.1: the Lumia PSCI flag is at FADT 0x81, not the ARM field

Re-decoded the three builtin FADTs myself (`cmp -l` + struct parse of
`/tmp/opencode/woa-repos/Lumia950XLPkg/AcpiTables/common/builtin/FADT*.acpi`):
all are `FACP`, length **276** (ACPI 6.0 layout). In that layout
offset **0x81 = IaPcBootArch** (IA-PC Boot Architecture Flags) and
**0x98 = ArmBootArch** (edk2 `Acpi60.h` GAS-aware recount; agent A's
"0x81 = ArmBootArchFlags" label was wrong). Measured values:

| file | @0x81 (IaPc) | @0x98 (ArmBootArch) |
|---|---|---|
| FADT.acpi (stock WP10) | **0x0003** | 0x0000 |
| FADT.NoPsci.acpi | **0x0002** | 0x0000 |
| FADT.PsciSmc.acpi | **0x0001** | 0x0000 |

So Microsoft's WP10 firmware (and the community's patches) carry the
PSCI-compliance/conduit bits (bit0 compliant, bit1 HVC) in the **IA-PC**
field, leaving the spec ARM field zero. FACT of placement; *(inference)*
Windows on those devices therefore reads 0x81 — a WP10-era quirk, not the
modern contract: Renegade's spec-conformant FADT (ARM_BOOT_ARCH=0x1 at the
struct's ArmBootArch member → 0x98) runs current Windows ARM64 on SDM845.
Our Fadt.aslc sets the spec field via the edk2 struct — correct for modern
Windows; keep the Lumia quirk in mind only if PSCI detection fails.

### C2. P0 — the shipped 9820 monitor, from our own dump (anchor fact)

FACT (`dumps/hhxj6/base.dts`, `/psci` node of the G970FXXUHHXJ6 base DTB):

```
psci {
    compatible = "arm,psci";        /* the bare 0.1 binding */
    method = "smc";
    cpu_suspend = <0xc4000001>;     /* PSCI 0.2 FN64 id */
    cpu_off    = <0x84000002>;      /* PSCI 0.2 id  (0.1 used 0x84000001) */
    cpu_on     = <0xc4000003>;      /* PSCI 0.2 FN64 id (0.1 used 0x84000002) */
};
```

All 8 cpu nodes `enable-method = "psci"`; idle `entry-method = "arm,psci"`,
suspend-param `0x10000` (Standby). Exactly the Exynos7/5433/9810 pattern
agent B documented — now confirmed for the 9820 from the ODIN dump itself.

### C3. P1 — where each protocol lives in OUR tables

FACT (`firmware/edk2/MdePkg/Include/IndustryStandard/Acpi63.h` + our ASL):
- PSCI is advertised via **FADT `ArmBootArch`** (`EFI_ACPI_6_3_ARM_PSCI_
  COMPLIANT` BIT0 / `ARM_PSCI_USE_HVC` BIT1, Acpi63.h:227-228);
  `Beyond0LtePkg/Fadt.aslc:95` sets PSCI_COMPLIANT, marked UNVERIFIED.
- Parking = MADT GICC `ParkingProtocolVersion`/`ParkedAddress`
  (Acpi63.h:519-539); our `BEYOND0LTE_GICC_INIT` zeroes both → "PSCI
  exclusive system" (SBBR wording per agent A).
- `Madt.aslc:60-110`: all 8 GICC ENABLED by default with
  uniprocessor/LITTLE-only/MID-only isolation knobs — the 9810 flow
  already anticipated the PSCI bring-up fault-isolation experiment.
- `notes/m8-reset-lib.md`: our reset lib already probes `PSCI_FEATURES`
  before `SYSTEM_RESET2` — a 0.2+ style caller; `ArmMonitorLib` in DSC:170.

### C4. MIDR conflict on prometheus (flagged, not resolved)

Live `/proc/cpuinfo` read (`notes/dock-desktop.md` §15/§16) says
implementer 0x41 **part 0xd0a variant 0x2** (= A76 r2p2 exactly); the
shipped Android 12 kernel's MIDR-shaped constants carry **0xd05/0xd09**
(A55/A75) only, Samsung's product page says 2×custom+2×**A75**+4×A55, and
the codename map (M4=cheetah, M3=meerkat) makes prometheus the A75 by
elimination. Weight of evidence: the live transcription was mistyped
(0↔a). Closing read: re-dump `/proc/cpuinfo` (cheap, safe).

### C5. The monitor-capability probe — how WE close the UNVERIFIEDs

Agent B's open items (does the 9820 monitor answer PSCI_VERSION
0x84000000 / PSCI_FEATURES 0x8400000A / AFFINITY_INFO) need an SMC call —
and we control the firmware: add a **read-only diagnostic to our UEFI**
(behind a `BEYOND0LTE_PSCI_DIAG` knob like the existing PRAM/PMU ones)
that calls PSCI_VERSION, PSCI_FEATURES(CPU_SUSPEND), AFFINITY_INFO for
each DT MPIDR and prints the returns. Never call SYSTEM_OFF/RESET or
CPU_OFF/CPU_SUSPEND in the probe. That converts "FADT PSCI_COMPLIANT =
UNVERIFIED" into a device read on the next boot pass, and directly tells
us whether Windows' 0.2 probing (agent B Q4: ACPI path assumes 0.2+,
probes PSCI_VERSION, rejects <0.2/NOT_SUPPORTED) will succeed on this
monitor.

### C6. Verdict for the 9820 port (synthesis of A + B + C)

1. Model to copy: **Renegade/SDM845** — FADT ArmBootArch=0x1 (PSCI, SMC),
   MADT GICC PPV=0 + ENABLED + DT MPIDRs (0x0-0x5, 0x100-0x101), ACPI0007
   per CPU (our DSDT already has the processor devices). Samsung's monitor
   is the SD845 class of firmware: PSCI-0.2 IDs over SMC in EL3.
2. Do NOT assume: PSCI_VERSION/PSCI_FEATURES/SYSTEM_OFF/SYSTEM_RESET/
   SYSTEM_SUSPEND/MEM_PROTECT — the 7420 precedent (e1e47fbca668) proves
   Samsung monitors lack SYSTEM_*; Duo's KernelErrataPatcher proves
   Windows calls PSCI 1.0 functions when the reported version offers them.
   Close with C5 before trusting any of it.
3. If PSCI_VERSION turns out unsupported (returns -1): Windows' behaviour
   is the open question — the **fallback with device precedent on Windows
   ARM64 is the ACPI parking protocol** (Lumia 950/8992: PPV=1, park
   mailboxes; Windows onlines cores by writing entry points to the
   spin-table-style mailbox). Implementing parking in our UEFI (park APs
   in a reserved region, publish PPV=1 + ParkedAddress) is the proven
   Plan B; it needs no EL3 cooperation at all.
4. System off/reset on our port: keep the vendor path (PMIC/ACPM), not
   PSCI SYSTEM_* — same conclusion Samsung upstream reached for 7420.
