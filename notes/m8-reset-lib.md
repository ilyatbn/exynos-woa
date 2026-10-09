# M8 — port the reset library 9810 → 9820 (`Star2LteResetSystemLib` → `Beyond0LteResetSystemLib`)

Task: give `Beyond0LtePkg` a `ResetSystemLib` instance so
`MdeModulePkg/Universal/ResetSystemRuntimeDxe` (currently DROPPED in the DSC) can
publish the ResetSystem services on the S10e. Source of truth for the port:
`firmware/Platform/Star2LtePkg/Library/Star2LteResetSystemLib/` (440 lines, read
in full). Nothing under `Star2LtePkg` or `Silicon/Exynos9810Pkg` is touched.

## Progress log

### Step 1 — what the 9810 library actually does

Three channels, in order of trust:

1. **PSCI (architected, no SoC address)** — `ARM_SMC_ID_PSCI_SYSTEM_RESET`,
   `PSCI_SYSTEM_RESET2_AARCH64` (gated by a `PSCI_FEATURES` probe),
   `PSCI_SYSTEM_OFF`, via `ArmMonitorCall`. Carried **unchanged**.
2. **PMU fallback** — `EXYNOS_PMU_BASE + 0x400` (SWRESET, write BIT0) and
   `EXYNOS_PMU_BASE + 0x330C` (PS_HOLD_CONTROL, clear BIT8).
3. **PRAM breadcrumb** — ramoops-style writes at `0xFED14000`
   (magic `0x43474244`, header 12, capacity `0x3F00`).

### Step 2 — the two Exynos channels and why neither is carried

* **PMU offsets are UNVERIFIED on 9820.** `Exynos9820.h:185` gives
  `EXYNOS_PMU_BASE 0x15860000` as a VERIFIED read
  (`system-controller@15860000`), but the interior of that block was never read
  here. The same fact is already recorded for the FADT in
  `notes/m8-acpi.md` ("the +0x400 SWRESET offset is NOT a read on the 9820 →
  UNVERIFIED"). So the fallback is compiled out behind
  `#define BEYOND0LTE_PMU_RESET 0`, with the 9810 offsets kept **inside** the
  guard as reference — the repo's established shape
  (`Beyond0LteAcpiPlatformDxe.c` `BEYOND0LTE_PRAM_DIAG 0`).
* **PRAM base is unusable, not merely unverified.** `0xFED14000` is inside bank0
  (0x80000000 + 0x80000000) and inside the vendor's
  `debug_snapshot.log_bcm` (0xfeb10000 + 0x400000) —
  `notes/s10e-live-reads.md` §7.3. Compiled out behind
  `#define BEYOND0LTE_PRAM_DIAG 0`.

### Step 3 — consequences of compiling both channels out (found by hand, will be
confirmed by the compiler)

With both switches 0, these become dead and had to be gated too, otherwise
`-Werror` (clang, `-Wall`) rejects the module:

| Item | Why it goes dead | How it is handled |
|---|---|---|
| `mSwReset`, `mPsHoldControl`, `mSwResetVirtualReady`, `mPsHoldVirtualReady` | only the PMU channel uses them | gated on `BEYOND0LTE_PMU_RESET` |
| `mPram`, `mPramVirtualReady` | only the PRAM channel | gated on `BEYOND0LTE_PRAM_DIAG` |
| `mVirtualAddressChangeEvent`, `MarkRangeRuntime`, `VirtualAddressChange` | only exist to relocate the channel pointers | gated on `(PMU \|\| PRAM)`; `CreateEvent` likewise |
| `RuntimePramByte/String/Hex64` | only the breadcrumb writer | `#else` provides a no-op `RuntimePramMarker` so every call site stays valid |

`ExynosPmuReset` / `ExynosPmuPowerOff` keep their `WaitForReset()` tail with the
channels off — that is the 9810 flow's "the reset call did not return, stop
touching memory" guard, which is channel-independent.

### Step 4 — naming / identity

* `STAR2LTE_*` → `BEYOND0LTE_*`, `Star2Lte` → `Beyond0Lte` (constructor
  `Beyond0LteResetSystemLibConstructor`).
* `FILE_GUID` `11111111-2222-3333-4444-555555555546`. Checked free by grepping
  `5555555555[0-9a-f]{2}` over `firmware/`: in use are 5500,5501,5502,5510,5520,
  5521,5522,5530,5531,5540,5541,5542,5543,5544,5545,5550 → 5546 is free. It is a
  build identity, not a read (the 9810 library's GUID
  `9631ACB0-8E87-4D3B-87A7-3AC1EF042B28` is deliberately not reused).
* `.inf` `INF_VERSION = 0x0001001B` (the 9810 file says `1.29`; the rest of this
  tree uses the hex form — matched to the sibling 9820 modules).
* `[Packages]` `Silicon/Exynos9820Pkg/Exynos9820Pkg.dec`.

## Constants left UNVERIFIED

* PMU intra-block offsets `+0x400` (SWRESET) and `+0x330C` (PS_HOLD_CONTROL) —
  compiled out, not guessed.
* The PRAM breadcrumb address — no legal 9820 address chosen yet.
* PSCI support itself on this SoC's monitor (`ArmBootArch PSCI_COMPLIANT` is a
  9810 value per `Exynos9820.h`) — the library is honest about it in the header.

## Sources (local evidence actually consulted)

* `firmware/Platform/Star2LtePkg/Library/Star2LteResetSystemLib/{.c,.inf}` — the 9810 original.
* `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` — PMU base (VERIFIED), PSCI flag (UNVERIFIED).
* `firmware/Platform/Beyond0LtePkg/Beyond0LtePkg.dsc` — the DROPPED `ResetSystemLib`
  instance (lines 217-220) and the DROPPED `ResetSystemRuntimeDxe` (lines 457-460).
* `notes/m8-acpi.md` — the same PMU +0x400 offset recorded UNVERIFIED for the FADT.
* `notes/s10e-live-reads.md` §7.3 — 0xFED1xxxx inside bank0 / debug_snapshot.
