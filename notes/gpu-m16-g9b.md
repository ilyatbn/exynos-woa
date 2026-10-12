- Reused-driver resource contract (W, read 2026-10-12): m1probe.c:231-238 and malikm/device.c:43-48 map the
  **first** CmResourceTypeMemory; maliwddm/adapter.c:106-126 MapGpu the same. Interrupts: malikm/device.c:49-59
  creates up to 3 WDFINTERRUPTs in resource order, one ISR (MkEvtIsr) for all, which reads all three
  INT_STAT blocks (maligpu.c:373-424) -> **any order works**; m1probe connects none (polled, m1probe.c:210-211).
  -> _CRS: GPU block first, then the PMU page, then job 320 / mmu 319 / gpu 321 (= DT order JOB, MMU, GPU,
  gpu-odin-rom.md:55-56,253). A second Memory32Fixed is invisible to maliwddm/malikm (they keep the first).
- Panfrost quirks for G76 (local clone C:\Users\ilya\code\mali\linux, b20a9b5f9): panfrost_gpu.c:117-175
  init_quirks: SHADER_CONFIG gets only SC_TLS_HASH_ENABLE (:141-145; hw_features_g76 has TLS_HASHING,
  panfrost_features.h:85-97; hw_issues_g76 = 0, panfrost_issues.h:259, so no issue-gated bit); TILER_CONFIG
  read back + written (:149-155); JM_CONFIG = JM_DEFAULT_IDVS_GROUP_SIZE 0xF << 16 (:166-170, regs.h:227-228;
  same as maligpu.c:95-98). Called from panfrost_gpu_power_on (:458) after soft reset (:519), before L2_PWRON.

## Task 1 - G9 ACPI half (Device GPU0)

- `Dsdt.asl` Device (GPU0): _HID SAMS9828, _UID 0, _CCA 0; _CRS = Memory32Fixed 0x18500000/0x5000 (FIRST),
  Memory32Fixed 0x15862000/0x1000 (PMU page with 0x2104/0x2384), Interrupt Level/ActiveHigh/Exclusive {320} JOB,
  {319} MMU, {321} GPU. Overlap check: other DSDT windows near it are AUD0 0x15861000 (PMU+0x1000 page), TSP0
  0x15850000, CMU_G3D 0x18400000 is in no device; nothing else claims 0x15862000 or 0x18500000-0x18504FFF
  (rg Memory32Fixed over Dsdt.asl).
- `_STA`: OperationRegion GPST SystemMemory 0x8FE0F010 len 4; 0x0F iff GPUS == 0x30555047 ('GPU0'), else 0 -
  the WLAN pattern.
- `Beyond0LteGpuProbeDxe.c`: GpuProbe() now returns BOOLEAN usable (TRUE only on the path that read GPU_ID != 0/~0
  and ((Id>>16)&0xF00F) == 0x7001, which itself is only reached with both PMU bits 1 and the GPU gate not
  manually off). Entry: word [4] = 0 first (also on the disabled and 0x6A-skip paths), after the last probe
  (the re-probe if GpuPowerOn returned TRUE) word [4] = 'GPU0' or 0, one log line
  "GpuProbe: ACPI GPU0 visible|hidden (status word [4] = ..)".
- `ArmPlatformLib.c` (SEC): word [4] = 0 next to the [2]/[3] clears.
- Cases: flag 0 + sboot leaves G3D off (expected) -> hidden. flag 0 + sboot leaves it on -> visible with sboot's
  clock (no ACPM request by us; the probe logs PLL_G3D M/P/S). flag 1 -> visible after a successful power-on;
  power-on failure path -> domain back off -> hidden; ACPM FREQ_REQ failure -> domain on, clock unknown -> visible
  (logged "on[9] ... FAILED").
- Build (2026-10-12, scratch copy `scratchpad\build-g9b.ps1` of build-g9.ps1: `$ws` = this worktree's firmware,
  edk2 = main checkout's, WORKSPACE = `subst W:` of `scratchpad\g9bbuild`, Conf copied from g9build; flag 0 as
  committed). First try exit 1: my scripted edit put a raw newline into the new DEBUG string (clang
  "missing terminating '\"'"), fixed before commit. Second: **exit 0**, `FVMAIN_COMPACT [99%Full] 774144 (0xbd000)
  total, 772192 used, 1952 (0x7a0) free` (was 2024 free after e951137 -> +72 B), FVMAIN 33 %; FD 7 340 032 B sha256
  `021ab3cdb5dfdb01...`; GpuProbeDxe.efi 14 336 B (contains "ACPI GPU0"). Dsdt.aml disassembled (iasl -d):
  GPU0 _HID SAMS9828, _CRS bytes = Memory32Fixed 0x18500000/0x5000, 0x15862000/0x1000, three Interrupt descriptors
  flags 0x01 (consumer, level, active-high, exclusive) with GSIV 0x140/0x13F/0x141 = 320/319/321, GPST at 0x8FE0F010,
  _STA compares 0x30555047. Not packed, not flashed.
- FVMAIN_COMPACT headroom is now 1952 B: the next firmware change may need the FV auto-grow seen with flag 1.

## Review of 889f781 (G9 ACPI half, adversarial)
- Checked mechanically: 9810 tree untouched (`git diff --stat e951137..HEAD -- firmware/Platform/Star2LtePkg
  firmware/Silicon/Exynos9810Pkg` empty); Dsdt.aml disassembled = intended _CRS bytes (above); no other ACPI table
  in Beyond0LtePkg/AcpiTables names GSIV 319/320/321 or the 0x15862000 page (rg); GpuPowerOn's return values
  (GpuPower.c:87-233): FALSE for "already ON" (first probe's verdict stands) and for the failure path (domain
  back off -> first probe said OFF -> hidden), TRUE after success or after an ACPM failure (domain on, re-probed).
  The status write precedes the existing `dsb sy` after the guard restore, and the page is uncached.
- Accepted, not defects (recorded): (a) ACPM FREQ_REQ failure -> GPU0 visible at sboot's/reset PLL_G3D rate (the
  re-probe already read GPU_ID through that clock, so the block answers); (b) EMBEDDED on + outer off at entry ->
  hidden and not powered on (conservative); (c) 0x6A-skip boot -> hidden (word cleared before the skip).
- Nothing to fix in a follow-up commit.
