# M8 — UEFI-side UFS host controller (Star2LteUfsHcDxe -> Beyond0LteUfsHcDxe)

Task: port the 9810's UEFI UFS host-controller driver so the storage chain
(UfsPassThruDxe -> ScsiBus/ScsiDisk -> PartitionDxe) gets its protocol producer on the 9820.

Ground rules (docs/exynos9820-port/HANDOFF.md): never guess hardware constants — leave UNVERIFIED;
9810 packages byte-unchanged; one task = one coherent change.

## Progress log (append after every meaningful step)

### Step 0 — orientation
- Repo root layout: `firmware/`, `drivers/`, `docs/`, `notes/`, `tools/`.
- `firmware/Platform/` has `Beyond0LtePkg` (9820) and `Star2LtePkg` (9810).
- `Star2LtePkg/Drivers/Star2LteUfsHcDxe/` holds `Star2LteUfsHcDxe.c` + `.inf`.
- Notes dir already has `m8-acpi.md`, `m8-build.md`, `m8-dscfdf.md`, `m8-libs.md`.
- `Beyond0LtePkg/Drivers/` now EXISTS (Beyond0LteAcpiPlatformDxe, SimpleFramebufferDxe) — the
  m8-dscfdf note that said "no Drivers/ dir" is superseded; those two drivers were ported in a
  later task. So the UFS HC driver is the remaining storage-chain gap.

### Step 1 — style references read
- `Beyond0LteAcpiPlatformDxe.c` (417 lines) + `.inf`: the pattern for a 9820 port of a 9810 driver.
  Key idioms: `#include <Platform/Exynos9820.h>`; verified addresses come ONLY from `EXYNOS_*`
  macros; 9810-only values are either (a) compiled out behind a `#define BEYOND0LTE_*_DIAG 0`
  switch (PRAM breadcrumb 0xFED14000 — `Beyond0LteAcpiPlatformDxe.c:57`), or (b) kept as a local
  placeholder explicitly tagged `// UNVERIFIED on 9820` (`Beyond0LteAcpiPlatformDxe.c:151`
  `BEYOND0LTE_MCT_CNTCV_PA`). The `.inf` header block states FILE_GUID is a BUILD IDENTITY not a
  hardware read, and that nothing is validated on an S10e (Guided, not Ready).
- `PlatformMemoryMapLib.c` (238 lines): every MMIO base/size pulled from Exynos9820.h; windows
  with Base==0 or Size==0 are SKIPPED (line 190). UFS windows listed at lines 102-107.
- `Exynos9820.h:268-278` UFS macros (ALL VERIFIED): HCI 0x13D60000+0x2000, VS 0x13D61100,
  UNIPRO 0x13D50000+0x8000, PMA 0x13D64000+0x800, UFSP 0x13D70000+0x1000, CPORT 0x13D68000,
  IOCOH 0x13C10700. Header note (line 260-262): "the intra-window vendor-specific offset is the
  SAME (HCI + 0x1100)" — so register OFFSETS carry unchanged; only the window BASES move.

### Step 2 — inventory of every 9810 address/constant in Star2LteUfsHcDxe.c (4190 lines)
Read in full. Classification (file:line in the 9810 .c):

A. UFS WINDOW BASES — remap to EXYNOS_UFS_* macros (VERIFIED on 9820):
   - `EXYNOS_UFS_HCI_BASE` used as a macro already (from Exynos9810.h=0x11120000) → 9820 macro 0x13D60000.
   - `UFS_VS_BASE 0x11121100` (local #define, line 166) → EXYNOS_UFS_VS_BASE (0x13D61100).
   - `UFSP_BASE 0x11130000` (local #define, line 156) → EXYNOS_UFS_UFSP_BASE (0x13D70000).
   - `UFS_PMA_BASE 0x11124000` (local #define, line 932) → EXYNOS_UFS_PMA_BASE (0x13D64000).
   - `UFS_UNIPRO_BASE 0x11110000` (local #define, line 941) → EXYNOS_UFS_UNIPRO_BASE (0x13D50000).
   - io-coherency sysreg literal `0x11010700` (lines 404,436,438,622,623) → EXYNOS_UFS_IOCOH_BASE (0x13C10700).
   - HCI-base raw literals `0x11120034`(HCE) `0x11120030`(HCS) `0x11120050`(UTRLBA) `0x11120060`(UTRLRSR)
     `0x11120018`(AHIT) (lines 1388-1391,1802,1803) → EXYNOS_UFS_HCI_BASE + offset.
   - UFSP raw literals `0x11130010`(RSECURITY) `0x1113020C`(SCTRL0) `0x11130204`(SEND0) (lines 365,367,368,1392)
     → EXYNOS_UFS_UFSP_BASE + offset.

B. INTRA-WINDOW OFFSETS / DME MIBs / PMA reg offsets — CARRY UNCHANGED (header says offsets are the
   same; these are protocol/vendor offsets, not SoC addresses): HCI_* offsets (0x00..0x114), UFSP_*
   offsets (0x000..0x27C), UFS_HC_* offsets (0x18..0x140), PMA offsets (0x004..0x1FC), UNIPRO offsets
   (0x00D0..0x78EC), DME attribute MIBs (0x9514,0x12,0xAA,0x2041..0x2043,0x15B0..0x15B2,0x1560..0x1584).

C. 9810-ONLY VALUES WITH NO 9820 READ → keep compiled-out or mark UNVERIFIED:
   - `EXYNOS_UFS_PHY_ISO 0x14060724` (line 133): PMU phy-isolation reg. 9820 PMU base is 0x15860000
     (Exynos9820.h:185) but the phy-iso OFFSET within it was never read → UNVERIFIED address. Only read
     feeds a compiled-out diag → compile out with the diag guard.
   - `HCI_1US_CNT_9810 0x000000A6` (line 526): mclk ~166 MHz ticks/us. Clock rate, no 9820 read → UNVERIFIED.
   - `UFS_MCLK_PRD 10u` (line 883): mclk-period fallback (proven sboot value on 9810). Code reads the
     device's own period first; only the fallback is 9810 → UNVERIFIED fallback.
   - `EXYNOS9810_OPT_SUITE 0x2E820183` (line 892): DME PA_DBG_OPTION_SUITE value. No 9820 read → UNVERIFIED.
   - `EdkiiUfsCardRefClkFreq26Mhz` (line 1876): UFSHCI RefClk *category* enum (edk2 protocol header, not
     an address). 9810's category; UNVERIFIED on 9820 but a legal enum → keep + comment.
   - PRAM breadcrumb / diag block / stage-fnptr addresses: `UFSHC_DIAG_BASE 0xFED13000` (line 1897),
     ramoops console `0xFED14000` (lines 1044,1045,1048), stage fnptr `0xFED13100` (line 1955), and the
     scattered direct writes `0xFED134xx`/`0xFED135xx` (lines 1563,1638,1672,1758,3731-3737). On the 9820
     0xFED1xxxx is INSIDE bank0 DRAM (0x80000000..0x100000000) — same hazard the AcpiPlatform + MemoryMap
     libs already flag → COMPILE OUT the whole PRAM/diag/telemetry channel (BEYOND0LTE_UFS_DIAG 0),
     mirroring BEYOND0LTE_PRAM_DIAG 0.
   - Framebuffer HUD: `HUD_FB_BASE 0xCC000000` (line 1961) + `HUD_DECON_BASE 0x16030000` (line 1979).
     9820 fb base is UNVERIFIED (EXYNOS_FB_BASE=0) and DECON moved to 0x19030000. HUD already gated by
     `STAR2LTE_UFS_SCREEN_HUD 0` → keep as BEYOND0LTE_UFS_SCREEN_HUD 0 (compiled out).
   - Bounce-buffer alloc cap `0x0000000098000000` (line 2576) and DRAM-range guards `0x80000000`/`0xE0000000`
     (lines 3820,3821,3924,3987...): 9810 DRAM-shape facts. Re-derive from EXYNOS_DRAM_BASE/SIZE macros
     (bank0 published 0x80000000..0xD0000000) — derived, not guessed.
   - SMC command IDs `SMC_CMD_* 0xC20018xx` (lines 177-183): Exynos EL3-monitor SMC codes from kernel smc.h.
     Not MMIO; family constant, not re-read on 9820 → keep + mark "family constant, not a 9820 read".
   - Vendor device-path GUID `STAR2LTE_UFSHC_VENDOR_GUID {3d2e9a14-...}` (line 58): build identity, not
     hardware → carry unchanged (UfsPassThru binds on it).

D. GUID: tree uses low-pair 5500,5501,5502,5510,5520,5521,5522,5530,5531,5540,5541,5542,5543,5544,5550.
   5545 is FREE → new FILE_GUID = 11111111-2222-3333-4444-555555555545.

### Step 3 — preprocessor gate map (fact, from rg of ^#if/#endif over the 9810 .c)

The 9810 driver already gates its experiment channels; the port leans on these gates:

* `UFS_DO_PMC 1u` (.c:983) and `UFS_DO_H8_SETTLE 1u` (.c:991) — ON in the 9810 build;
  they gate the PRAM helper block (1009–1082), PMADUMP (1696–1734), bounce-console
  dumps (2653–2675, 2734–2753) and PMC/H8 experiment code (1441–…, 3301–3441, 3755–3842).
* `STAR2LTE_UFS_SCREEN_HUD 0` (.c:1952, #ifndef-default) — OFF. The whole HUD/CRUMB
  implementation (1960–2404: Crumb, Stage, HudFbLine, 0xCC000000/0x16030000 refs) is
  already compiled out; #else (2405–2425) provides no-op stubs. So Crumb/Stage call
  sites need NO wrapping.
* `STAR2LTE_SKIP_PREHCE_SW_RST 0`, `STAR2LTE_CONVERT_UTRD_BYTE_GRAN 0` — keep at 0.
* `UFSHC_DIAG_BASE 0xFED13000` (.c:1897) — used by live panel writes AND by the
  HUD-gated Crumb/Stage; the live uses need wrapping.

### Step 4 — the port plan (mechanical; line numbers = the 9810 .c, 4190 lines)

State when this session died: `Beyond0LteUfsHcDxe.inf` WRITTEN (GUID …5545, packages
Exynos9820Pkg.dec); the .c NOT created; DSC/FDF NOT wired; no rebuild; task NOT committed.

Substitutions, in order, on a copy of the 9810 .c:
1. `Star2Lte` -> `Beyond0Lte`; `STAR2LTE_` -> `BEYOND0LTE_`.
2. include -> `<Platform/Exynos9820.h>`.
3. `EXYNOS9810_OPT_SUITE` -> `BEYOND0LTE_UFS_OPT_SUITE` (define .c:892, use .c:1275; mark UNVERIFIED).
4. `HCI_1US_CNT_9810` -> `HCI_1US_CNT` (define .c:526; UNVERIFIED — 9820 mclk unread).
5. `EXYNOS_UFS_PHY_ISO` -> `BEYOND0LTE_UFS_PHY_ISO`, define (.c:133) value -> `0ULL`
   UNVERIFIED (9820 PMU base 0x15860000, intra-block offset unread); guard the read
   at .c:609–610 with `if (BEYOND0LTE_UFS_PHY_ISO != 0)`.
6. Local base defines -> 9820 macros: UFSP_BASE(.c:156)->EXYNOS_UFS_UFSP_BASE,
   UFS_VS_BASE(.c:166)->EXYNOS_UFS_VS_BASE, UFS_PMA_BASE(.c:932)->EXYNOS_UFS_PMA_BASE,
   UFS_UNIPRO_BASE(.c:941)->EXYNOS_UFS_UNIPRO_BASE.
7. Raw literals -> macro+offset: 0x0000000011010700ULL->EXYNOS_UFS_IOCOH_BASE (.c:438,622);
   0x11130010u/0x1113020Cu/0x11130204u -> (UINT32)(EXYNOS_UFS_UFSP_BASE+0x10/0x20C/0x204) (.c:365,367,368);
   0x0000000011120034/30/50/60ULL -> (EXYNOS_UFS_HCI_BASE+0x34/30/50/60ULL) (.c:1388–1391);
   0x0000000011130010ULL -> (EXYNOS_UFS_UFSP_BASE+0x10ULL) (.c:1392);
   0x0000000011120018ULL -> (EXYNOS_UFS_HCI_BASE+0x18ULL) (.c:1802,1803 — AHIT disable, LIVE code).
8. HUD defines (.c:1961,1979): 0xCC000000ULL->EXYNOS_FB_BASE, 0x16030000ULL->EXYNOS_DECON_F_BASE.
9. Bounce cap (.c:2576): `Address = 0x0000000098000000ULL` -> `EXYNOS_DRAM_BASE + EXYNOS_DRAM_SIZE`
   (published bank0 end 0xD0000000; derived from the map, replaces a 9810-shape value).
10. DRAM guards (.c:3820–3821): 0x80000000u->EXYNOS_DRAM_BASE, 0xE0000000u->(EXYNOS_DRAM_BASE+EXYNOS_DRAM_SIZE);
    (.c:3924): 0x80000000ULL->EXYNOS_DRAM_BASE, 0xC0000000ULL->(EXYNOS_DRAM_BASE+EXYNOS_DRAM_SIZE).
    Do NOT touch 0x80000000u at .c:508/511 (bitmasks HCI_PRDT_PREFECT_EN / HCI_WLU_EN).
11. Insert `#define BEYOND0LTE_UFS_DIAG 0` after the include (~.c:44) with the reason
    (0xFED1xxxx inside bank0 / debug_snapshot — m8-build §6, s10e-live-reads §7.3).
12. Wrap `#if BEYOND0LTE_UFS_DIAG`/`#endif` at (insert bottom-up so numbers stay valid):
    4139 (stage fnptr store); 3731–3737; 1758; 1696–1734 (PMADUMP block); 1672; 1638;
    1563–1564; 1386–1393 (A116 snapshot); 1044–1057 (PramByte BODY -> no-op, covers
    PramStr/Hex8 call sites); 352–369 (panel writes + SFR backdoor).
13. Rewrite the file-header comment block (.c:1–10) for the 9820 port.

Then: DSC [Components.common] + FDF storage section (~208); delete the UfsHcDxe entries
from both DROPPED blocks (dsc:46, fdf:35) and fix the stale "is DROPPED" comments
(dsc:496, fdf:205). Rebuild: `powershell -ExecutionPolicy Unrestricted -File
firmware\tools\build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG` (box is AllSigned).
FVMAIN_COMPACT has NumBlocks=0 (auto-size, fdf:86) — it grows by 0x1000 blocks; the
"2976 free" is last-block slack, NOT a hard cap. FD stays 7 340 032 B (NumBlocks=0x700).

### Step 5 — exhaustive literal sweep (2026-10-09, restarted session)

Ran `lit_scan.py` over the 9810 .c: every hex literal >= 0x01000000 with its line
numbers. This SUPERSEDES the §2 line lists where they disagree — the §2 inventory
was incomplete. Findings, code vs comment:

* **Code uses of 9810 MMIO literals** (all must become macros):
  `0x0000000011010700ULL` at 404,436,438,622; `0x11130010u` 365, `0x1113020Cu` 367,
  `0x11130204u` 368; `0x0000000011120034/30/50/60ULL` 1388-1391;
  `0x0000000011130010ULL` 1392; `0x0000000011120018ULL` 1802,1803 (LIVE AHIT code);
  local defines 156 (UFSP_BASE), 166 (UFS_VS_BASE), 932 (UFS_PMA_BASE), 941
  (UFS_UNIPRO_BASE), 133 (EXYNOS_UFS_PHY_ISO, used live at 609).
* **§2 said "352–369 (panel writes)" — WRONG.** The FED13000 panel writes start at
  **348** (348-355), then the SFR block 363-369 writes at 365-368. Wrap 348–369.
* **§2 missed these code sites entirely:** 1702 (`Dpanel` = FED13000 inside the
  PMADUMP block), 1897 (`UFSHC_DIAG_BASE` define) + 1905/1917-1919/1929-1931 (the
  three diag functions' bodies), 1955 (`STAR2LTE_STAGE_FNPTR` define), 4139-4140
  (the stage-fnptr store + its dsb), 1044-1046 (the PRAM pointer declarations —
  wrapping only 1044-1057 would leave `Pw`/`Pb` undeclared at 1048).
* **Comment-only literals — do NOT touch:** 138,153,263,265,357,360,392,415,417,
  614,615,619,925,937,1218,1222,1283,1331,1384,1510,1883,1938,1942,1943,2061,
  2235,2566,2568,2569,3105,3720,3721,3722. (0x10830000, 0x1406080C/0x12345674,
  0x11120140, 0x6952f000, 0xbc800000, 0xe1900000, 0x880000000 all appear ONLY in
  comments.)
* **Not addresses, keep verbatim:** 0xC0000000 at 238 = SMC_REG_CLASS_SFR_R
  (3<<30); 0x80000010/0x8000000c/0x80000007/0x80000004/0x80000017/0x80002020/
  0x88000001/0x98000003/0x80000001 = UEC PA/PHY status codes; 0xA1170001,
  0xA1180000, 0xB5000000 = diag tags; 0xA2700000/10000/20000 = STAGE_* tags;
  0x20494645/0x54524150 = "EFI "/"PART" GPT data compares (2829); 0xDEAD0002
  sentinel; 0xFFFFFF80/0xFFFFFFFF masks; 0x43474244 ramoops sig (inside the
  PRAM block); 0xEFE26492/0xEFFA6492 RSECURITY values (comments); HUD colours
  0xFF33FF66/0xFF000000/0xFFFF0000 (compiled out by BEYOND0LTE_UFS_SCREEN_HUD 0).
* **Macro-name check (macro_scan.py):** the .c uses exactly TWO header macros in
  code — `EXYNOS_UFS_HCI_BASE` (present in Exynos9820.h, 40 sites) and
  `EXYNOS_UFS_PHY_ISO` (a LOCAL define at 133, not a header macro). So swapping
  the include to Exynos9820.h breaks nothing else.
* **9810 comment/code contradiction found (not fixed in the 9810 tree, which is
  byte-unchanged):** .c:2574 says "Reverted to MAX_ADDRESS" but .c:2576 still
  passes `AllocateMaxAddress` with 0x98000000. The port replaces that 9810-shape
  cap with `(EXYNOS_DRAM_BASE + EXYNOS_DRAM_SIZE)` = 0xD0000000, i.e. the
  published bank0 end — a DERIVED bound, not the 9810's low-cap experiment.

### Step 6 — the port script (mechanical, line-anchored)

Still UNVERIFIED after the port (keep marked): mclk ticks/us (HCI_1US_CNT), mclk-period
fallback (UFS_MCLK_PRD 10u), opt-suite DME value, RefClk-26MHz category enum, SMC_CMD_*
family codes (EL3 monitor, not MMIO), PHY_ISO (zeroed), iocoh mask (kernel-faithful).
