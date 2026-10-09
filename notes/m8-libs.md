# M8 — porting two small EDK2 components to `beyond0lte` (Exynos 9820)

Task: port the 9810's SerialPortLib and SimpleFramebufferDxe (+ its fb-text helper)
into the 9820 packages. Started 2026-10-09. Findings are appended as work proceeds.

## 0. Inputs read before writing anything

| File | What it gave me |
|---|---|
| `firmware/Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib/{.c,.inf}` | the source component (201 L + 26 L) |
| `firmware/Silicon/Exynos9810Pkg/Include/Platform/Exynos9810.h` | 9810 UART base `0x10440000`, fb base `0xCC000000`, fb 1440x2960 |
| `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` | 9820 `EXYNOS_UART_BASE 0x10440000` / `EXYNOS_UART_SIZE 0x100`; `EXYNOS_FB_BASE 0` (deliberate); fb 1440x3040; `EXYNOS_DECON_F_BASE 0x19030000` |
| `firmware/Silicon/Exynos9820Pkg/Exynos9820Pkg.dec` | already declares `gExynos9820TokenSpaceGuid.PcdSerialRegisterBase\|0x10440000` (VERIFIED) |
| `firmware/Platform/Star2LtePkg/Drivers/SimpleFramebufferDxe/{.c,.inf}` | the source driver (425 L + 35 L) |
| `firmware/Platform/Star2LtePkg/Library/PlatformMemoryMapLib/Star2LteFbText.{c,h}` | the fb-text helper (183 L + 24 L) |
| `firmware/Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/{.inf,ArmPlatformLib.c,PlatformMemoryMapLib.c}` | the 9820 conventions already established by the port |
| `notes/s10e-live-reads.md` §7.2/§7.3 | why `0xCC000000` is unusable on the S10e |
| `docs/exynos9820-port/{HANDOFF.md,MILESTONES.md}` | ground rules: `UNVERIFIED` never guessed; nothing `Ready` until validated on an S10e |

## 1. Facts that decide the port (all reads, not inference)

* UART: `uart@10440000` on the live S10e and in the shipping vendor DT — **same base as
  the 9810**, but the vendor DT declares the window as **0x100**, not the 9810's 0x1000
  (`Exynos9820.h:120-121`).
* Framebuffer base: **not a read on this SoC**. `notes/s10e-live-reads.md` §7.3 states
  `0xCC000000` sits inside the `abox-rmem` **alloc range** (`0xc0000000..0xe0000000`) in
  the shipping DT, and inside `HLOS 1` (`0xc1200000+0x3ee00000`) in edk2-exynos' working
  9820 memory map. The header therefore keeps `EXYNOS_FB_BASE = 0` (`Exynos9820.h:420`).
* Framebuffer geometry: 1440x3040 (`Exynos9820.h:421-422`) — the S9+ was 1440x2960.
* DECON moves and splits: `decon_f@0x19030000` (+0x10000), `decon_t@0x19050000`
  (`Exynos9820.h:386-389`; live platform devices confirmed in `notes/s10e-live-reads.md` §2).
* The 9810's PRAM breadcrumb channel is compiled out on the 9820
  (`BEYOND0LTE_PRAM_DIAG 0` in both 9820 lib files) because `0xFED14000` is inside
  bank0 — so the debug UART is the sign-of-life channel here.

## 2. Decision: the .dec library section

Checked first, as instructed: **neither** `Exynos9810Pkg.dec` **nor** `Exynos9820Pkg.dec`
has a `[LibraryClasses]` section. In this repo the library class is bound only in the
platform DSC (`Star2LtePkg.dsc:76`
`SerialPortLib|Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib/...inf`), and the
9810 `.dec` carries nothing for it. Following the 9810's style means **no `.dec` edit** —
so `Exynos9820Pkg.dec` is left byte-unchanged. Recorded so it is not re-litigated.

## 3. A — SerialPortLib (done)

Created:

* `firmware/Silicon/Exynos9820Pkg/Library/Exynos9820SerialPortLib/Exynos9820SerialPortLib.c`
* `firmware/Silicon/Exynos9820Pkg/Library/Exynos9820SerialPortLib/Exynos9820SerialPortLib.inf`

Deltas from the 9810 original, each one deliberate:

| Change | Why |
|---|---|
| `#include <Platform/Exynos9820.h>`; `GetUartBase()` falls back to `EXYNOS_UART_BASE` when the PCD is 0 | the header is the repo's single source of MMIO addresses; no address literal appears in the .c |
| `SerialPortInitialize()` DEBUG-warns when `PcdSerialRegisterBase != EXYNOS_UART_BASE` | the PCD default and the header are two copies of one read; a half-edited set is otherwise silent (no UART output is indistinguishable from a dead phone) |
| `STATIC_ASSERT (EXYNOS_UART_URXH < EXYNOS_UART_SIZE)` | the 9820's declared window is **0x100**, the 9810's was 0x1000. Highest offset used is URXH +0x24, so the shrink is safe — asserted, not eyeballed. `STATIC_ASSERT` comes from MdePkg `Base.h:816-820` |
| `DebugLib` added to `[LibraryClasses]` | needed by that warning; the DSC binds it for every module type (`Star2LtePkg.dsc:71`) |
| `FILE_GUID ...555555555522` | free in this tree (9820 GUIDs seen so far: `...5520` .dec, `...5521` AcpiTables, `...5531` PlatformMemoryMapLib) |

`Exynos9820Pkg.dec` left byte-unchanged — see §2 for why.

Not buildable yet: there is **no `Beyond0LtePkg.dsc`/`.fdf`** in the tree, so the DSC
line that would bind `SerialPortLib|Silicon/Exynos9820Pkg/Library/...inf`
(the 9810 equivalent is `Star2LtePkg.dsc:76`) has nowhere to go. Follow-up, not silently
skipped.

**Still UNVERIFIED in this component:** the baud sboot left on the 9820's UART (never
read; `SerialPortSetAttributes` still returns without programming it), and the meaning
of the vendor DT's `fifo-size = 0x100` for a future UFCON/UBRDIV pass.

## 4. Correction to §3 — the DSC turned up after all

A concurrent M8 task (`notes/m8-dscfdf.md`, files written 03:40–03:47 while this one
was running) created `firmware/Platform/Beyond0LtePkg/Beyond0LtePkg.dsc` and
`Beyond0LtePkg.fdf`. So the SerialPortLib **is** already wired:

* `Beyond0LtePkg.dsc:144` — `SerialPortLib|Silicon/Exynos9820Pkg/Library/Exynos9820SerialPortLib/Exynos9820SerialPortLib.inf`
* `Beyond0LtePkg.dsc:398` — the same `.inf` listed in `[Components.common]`
* `Beyond0LtePkg.dsc:310` — `gExynos9820TokenSpaceGuid.PcdSerialRegisterBase|0x0000000010440000`

That DSC's GUID note (line 13) already cites `Exynos9820SerialPortLib …5522`, i.e. it
saw the `.inf` I created — the two tasks agree on the serial-port half.

**They do NOT agree on the framebuffer half.** Both `Beyond0LtePkg.dsc:24-29` and
`Beyond0LtePkg.fdf:21-24` list `SimpleFramebufferDxe` as **DROPPED** ("no 9820
equivalent", "no `Drivers/` directory at all"), and `Beyond0LtePkg.dsc:445` /
`Beyond0LtePkg.fdf:169` keep the console stack without a graphics producer. That was
true when that task ran its `Test-Path`; it is no longer true — the ported driver now
exists at `firmware/Platform/Beyond0LtePkg/Drivers/SimpleFramebufferDxe/`.

Follow-up, deliberately not done here (the DSC/FDF belong to the other task and were
being edited while I worked — editing them from two sides is a clobber risk):

* add `Platform/Beyond0LtePkg/Drivers/SimpleFramebufferDxe/SimpleFramebufferDxe.inf`
  to `[Components.common]` near `Beyond0LtePkg.dsc:445` and to the FDF near
  `Beyond0LtePkg.fdf:169`;
* delete it from both DROPPED blocks so the two files stop contradicting the tree.

Note that adding it changes nothing observable today: the entry point returns
`EFI_NOT_READY` while `EXYNOS_FB_BASE == 0`.

## 5. B — framebuffer driver + fb-text helper (done)

Created:

* `firmware/Platform/Beyond0LtePkg/Drivers/SimpleFramebufferDxe/SimpleFramebufferDxe.c`
* `firmware/Platform/Beyond0LtePkg/Drivers/SimpleFramebufferDxe/SimpleFramebufferDxe.inf`
* `firmware/Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/Beyond0LteFbText.h`
* `firmware/Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/Beyond0LteFbText.c`
* edited (one line + a comment block): `.../PlatformMemoryMapLib/PlatformMemoryMapLib.inf`
  now lists `Beyond0LteFbText.c` in `[Sources]`, mirroring the 9810 `.inf:18`

Deltas from the 9810 driver, each deliberate:

| Change | Why |
|---|---|
| `#include <Platform/Exynos9820.h>`; **no** `0xCC000000` literal anywhere in the driver or the helper | the 9820's single source of addresses; the 9810 base is proven unusable here (§1) |
| `#define DECON_BASE EXYNOS_DECON_F_BASE` (was a hardcoded `0x16030000`) | DECON moves and splits on this SoC: `decon_f@0x19030000` (`Exynos9820.h:386`) |
| `DECON_SHADOW_UPDATE_REQ 0x60` / `DECON_HW_SW_TRIG 0x70` kept but marked **UNVERIFIED on 9820** | they are device-proven only against the 9810's `dpu_9810 regs-decon.h`; the 9820 kernel drives the display through Samsung's `dpu20/cal_9820` tree (`notes/exynos9820-research.md` §1a), a different DPU generation |
| `SHADOW_UPDATE_REQ_WINS 0x3F` marked as the **9810's** 6-window count | the 9820 has two DECON halves (`decon_f`, `decon_t`); its window count was never read |
| Entry refuses with `EFI_NOT_READY` when `EXYNOS_FB_BASE == 0`, comment says the carveout **has to be measured** | keeps the honesty: a wrong fb base is a blank/faulting screen, not recoverable |
| Geometry read from the header (1440x3040), and the scale math re-derived for this panel | width unchanged from the 9810 (1440) so the `GraphicsConsole` 80-column limit gives the same verdict — scale 1 or 2 only; rows go 155 → 160 at scale 1 |
| `FB_GOP_VENDOR_GUID` last byte-pair `0x21` → `0x22` | a namespace identifier, chosen so the two platforms' console handles cannot be confused; **not** a device read |
| `FILE_GUID …555555555544`, `BASE_NAME Beyond0LteSimpleFramebufferDxe` | `…5544` is free in the tree; the prefixed BASE_NAME follows the 9820 convention already used by `Beyond0LtePlatformMemoryMapLib` |

In the fb-text helper:

| Change | Why |
|---|---|
| `FB_BASE`/`FB_WIDTH`/`FB_HEIGHT` are `EXYNOS_FB_*` macros, no literals | same reason as above |
| `if (FB_BASE == 0) return;` before any store | the placeholder must never become a write to physical address 0 |
| `if (PixelRow + GLYPH_H*FB_SCALE > FB_HEIGHT) return;` | the 9810's callers pass rows (1240/1300) chosen against a 2960-tall panel; the taller 3040 panel makes row choice a per-model decision, and past the buffer the extent is unknown |
| `STATIC_ASSERT (EXYNOS_FB_BYTES_PER_PIXEL == 4, …)` | the renderer stores `UINT32`; a non-4-byte format needs a different store |
| font table, `FB_MARGIN_X 160`, `FB_SCALE 2`, FG/BG colours unchanged | a font and a layout choice, not device constants — `FB_MARGIN_X` is **not** a read |

## 6. Everything left UNVERIFIED by this port

| Constant | Where | What would close it |
|---|---|---|
| `EXYNOS_FB_BASE` (fb scanout carveout) | `Exynos9820.h:420`, consumed by both new components | measure the sboot-lit carveout on the phone (the 9810 flow already has a tool) |
| fb pixel format (BGRA8888 assumed) and stride = width*4 | `SimpleFramebufferDxe.c`, `Beyond0LteFbText.c` | read the carveout's format with the base; pmOS forcing `bits_per_pixel=16` on the S10's `decon_f` is a warning sign (`notes/exynos9820-linux-boot.md` §Round 2) |
| fb geometry 1440x3040 itself | `Exynos9820.h:421-422` — the header does **not** tag these VERIFIED, unlike the 9810's | read the panel's own geometry on the device |
| DECON register offsets `0x60`/`0x70` and the 6-window mask | `SimpleFramebufferDxe.c` | re-derive from Samsung's `dpu20/cal_9820` register map |
| UART baud left by sboot | `Exynos9820SerialPortLib.c` (`SetAttributes` still a stub) | read the UART node's clock/baud, or program ULCON/UCON/UFCON/UBRDIV once sign-of-life exists |
| `FB_MARGIN_X 160`, glyph rows (1240/1300 equivalents) | `Beyond0LteFbText.c` | layout decision; the S10e's punch-hole safe area is itself unread |
| `FB_GOP_VENDOR_GUID` | `SimpleFramebufferDxe.c` | not a device value — a chosen namespace |

## Sources

* `firmware/Silicon/Exynos9810Pkg/Include/Platform/Exynos9810.h` (9810 constants, comment style)
* `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` (9820 constants + their provenance)
* `firmware/Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib/*`, `firmware/Platform/Star2LtePkg/Drivers/SimpleFramebufferDxe/*`, `firmware/Platform/Star2LtePkg/Library/PlatformMemoryMapLib/Star2LteFbText.*` (the sources being ported)
* `notes/s10e-live-reads.md` §7.2, §7.3 — fb base inside the `abox-rmem` alloc range; live `decon_f@0x19030000`
* `notes/dock-desktop.md` §4, §5, §11, §16, §17 — the 9810→9820 constants table
* `notes/exynos9820-research.md` §1a — `drivers/video/fbdev/exynos/dpu20/cal_9810` vs `cal_9820`
* `notes/exynos9820-linux-boot.md` §Round 2 — pmOS `initfs-hook.sh` forcing `19030000.decon_f/graphics/fb0/bits_per_pixel` to 16
* `docs/exynos9820-port/{HANDOFF.md,MILESTONES.md}` — ground rules (UNVERIFIED never guessed; nothing Ready until validated)
* `notes/m8-dscfdf.md`, `firmware/Platform/Beyond0LtePkg/Beyond0LtePkg.{dsc,fdf}` — the concurrent M8 task's wiring decisions
* `firmware/edk2/MdePkg/Include/Base.h:816-820` — `STATIC_ASSERT` is available in this toolchain

## 7. Checks run after writing (all passed)

* **9810 packages untouched:** every file under `firmware/Silicon/Exynos9810Pkg` and the
  four `Star2LtePkg` fb files still carry the baseline mtime `2026-10-09 01:33:55` —
  nothing in the validated package was edited.
* **No `0xCC000000` in code:** `rg 0xCC000000` over the 9820 tree hits only comments and
  one DEBUG string (the refusal message that names the 9810 value as a warning, not a
  constant). No `#define`, no address arithmetic uses it.
* **Brace balance** in the three new `.c` files: 23/23, 46/46, 108/108.
* **Font table identical to the 9810's:** 95 rows compared whitespace-normalized,
  0 mismatches (script at `C:\Users\ilya\AppData\Local\Temp\opencode\cmpfont.ps1`,
  run through `Invoke-Expression` because this box's execution policy rejects unsigned
  script files).
* **Not compiled:** no C toolchain on this host (`clang`/`gcc`/`cl` all absent) and the
  EDK2 build needs `build-uefi.ps1`'s BaseTools. The two `STATIC_ASSERT`s are the
  compile-time checks that will fire if a constant set is half-edited; they are
  unexercised so far.
