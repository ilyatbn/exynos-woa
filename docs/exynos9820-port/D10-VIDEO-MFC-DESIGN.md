# D10 — Video codec (MFC): design

Evidence: `notes/video-mfc.md` (read that first — this file assumes it; T1 = shipped dump,
T2 = the Exynos 9820 vendor kernel mirror, T3 = the Windows contract, T4 = prior art,
T5 = the SPI-vs-GSIV rule).
Constants: `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h`, block
"Video codec / MFC (D10)".

## 1. Definition of done + rung ladder

Done means: **the MFC answers under our own stack and Windows can enumerate a hardware
decoder for it.** Explicitly out of scope for now: the encoder side, and anything DRM/secure
(`smfc`, the `ion,secure` heaps) — the vendor's secure path is a separate subsystem.

| Rung | Observation | Where | Proves |
|---|---|---|---|
| R0 | *(done, no device)* `/mfc@188D0000` present, no `status`, not touched by any overlay | dumps | the block exists on this board and nothing disables it |
| R1 | `[MFC] R1: version=0x1300` on the debug serial | UEFI, **zero writes** | the SFR is reachable in our boot state — the vendor says it is, over the Q-CH, even with the clock off (`mfc_pm.c:48-52`) |
| R2 | `[MFC] R2: gate 422 on, version=0x1300` | UEFI | we can clock the block the way Android does and it still answers |
| R3 | `[MFC] R3: risc_on, R2H 12 seen` | UEFI | the RISC boots from a firmware image we placed in DRAM — the blob path works |
| R4 | `[MFC] R4: SYS_INIT, R2H 1 seen, fimv=D/E/N` | UEFI | the codec's firmware is alive under our stack; the control plane is genuinely open |
| R5 | Windows: `Device (VIDC)` `_HID SAMS9828` enumerates and an AVStream minidriver binds | Windows | the ACPI description and the class binding are right |
| R6 | `MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_HARDWARE …)` returns our filter; one H.264 frame decodes to NV12 | Windows | the whole chain works end to end |
| R7 | encode path | — | deliberately not scheduled |

Nothing above a rung is claimed until that rung is observed on the device.

## 2. Hardware contract (all VERIFIED unless marked)

| Item | Value | Source |
|---|---|---|
| Core window | `0x188D0000` + `0x10000` | `base.dtb /mfc@188D0000 reg` |
| Interrupt | DT cell `0x152` = **SPI 338** → **GSIV 370** | `base.dtb`; +32 rule proven in `notes/video-mfc.md` T5 |
| IP version | `ip_ver = 0x1300`; register `0xF028` (full 32-bit read) | `base.dtb` + `mfc_regs.h:51,583` |
| Clock gate | index **422** in `/clock-controller@0x1a240000` (= `EXYNOS_CMU_TOP_BASE`) | `base.dtb clocks <0x2 0x1a6>`; mirror `GATE_MFC = 420+2` |
| SYSMMUs | `0x18890000+0x9000` and `0x188B0000+0x9000`; secure aliases `0x188A0000`/`0x188C0000`; gates 423/424; GSIVs 375/376/377/378 | `base.dtb` |
| Side windows | hwfc `0x18A28000+0x100`, mmcache `0x1A300000+0x100` | `base.dtb` (MFC child nodes) |
| Power domain | `/pd-mfc@15862600`, `status = "okay"`, `cal_id = <0xb138000c>`, `need_smc = <0x18820204>` | `base.dtb` |
| Firmware | `mfc_fw.bin`, **687 912 B**, at `/vendor/firmware/mfc_fw.bin` | `debugfs stat` on the shipped `vendor.raw` |
| Bring-up order | clock on → `MFC_RESET (0x1070)` → `RISC_BASE_ADDRESS (0x0054)` → `RISC_ON (0x0000)` → wait R2H **12** → `SYS_INIT` → wait R2H **1** | `mfc_run.c:30-101` |
| Which CMU bank feeds gate 422 | **UNVERIFIED** — the MFC node names five `cmu` windows and none is labelled; closes by resolving the clock-provider node against `cmucal-*.h` in the mirror | `notes/video-mfc.md` T1 §2 |
| `mfc_freqs` unit | **UNVERIFIED** (kHz is the obvious reading, unstated in the DT) | T1 §8 |

## 3. Two halves — who owns what

**Firmware half (UEFI).** `firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteMfcDxe/` — first
slice is **observation only**, the same discipline as `Beyond0LteModemDxe`: map the core window,
read `0xF028`, print `[MFC] R1: version=%x`, write nothing. R2 (gate 422) and beyond are compiled
out behind a `BEYOND0LTE_MFC_INIT 0` macro until R1 is seen on device.
At `ExitBootServices` the module leaves the MFC exactly as it found it.

**Role-latch contract.** Nothing is latched for the MFC yet. The rule for the later rungs: the
SYSMMUs that feed the codec must be owned by **one** party. Either our UEFI programs them and the
Windows driver inherits a working translation setup (then they stay **out** of `_CRS`), or the
Windows driver owns them and they go **into** `_CRS`. Deciding that on device without having read
the SYSMMU register contract is how a silent corruption bug gets made, so the design says:
**Windows owns them** (they are in `_CRS`), and the UEFI module never touches them.

**Windows half.** `Device (VIDC)` with `_HID "SAMS9828"`, `_CRS` in this fixed order — that order
*is* the driver's binding contract:

1. `Memory32Fixed (ReadWrite, 0x188D0000, 0x10000)` — the core
2. `Memory32Fixed (ReadWrite, 0x18890000, 0x9000)` — SYSMMU port 0
3. `Memory32Fixed (ReadWrite, 0x188B0000, 0x9000)` — SYSMMU port 1
4. `Interrupt (Level, ActiveHigh, Exclusive) { 370 }` — the codec
5. `Interrupt … { 375 }, { 376 }, { 377 }, { 378 }` — the two SYSMMUs (fault reporting)

## 4. Windows driver layering

The contract (T3) offers two shapes; **shape (1), AVStream, is the one to build**, because Media
Foundation then proxies the filters as MFTs for free and no user-mode registration/COM work is
needed.

| Layer | What it does | Rung | Reference |
|---|---|---|---|
| Bind | `SAMS9828` → our AVStream minidriver under `Ks.sys` | R5 | `stream/getting-started-with-hardware-codec-support-in-avstream` |
| Map windows | `_CRS` order above; core + 2 SYSMMUs + 5 interrupts | R5 | our own D4c/D4b `_CRS`-as-contract pattern |
| Identity | read `0xF028`, require `0x1300`, refuse otherwise (the vendor does exactly this) | R4→R5 | `mfc_run.c:110-116` |
| Bring-up | reset → risc base → risc on → R2H 12 → SYS_INIT → R2H 1 | R3/R4 | `mfc_run.c:30-101` |
| IOMMU | program the two SYSMMUs (register map is in the mirror: `drivers/iommu/exynos-iommu*`) | R3 | GPL — **reference only**, transliterate |
| Codec filters | three KS filters: decoder, encoder, video processor; decoder must offer NV12 | R6 | the AVStream page above |
| INF | `include=ks.inf,kscaptur.inf`, `AddInterface=%KSCATEGORY_ENCODER%`, KsProxy CLSID | R6 | `stream/encoder-installation-and-registration` |

Everything from "Codec filters" down is a `NOT-IMPLEMENTED` marker in any skeleton; the honest
first deliverable is the UEFI observation slice (R1), not a driver.

## 5. License posture

- **Adoptable**: WDK AVStream samples (`Avshws`) — check the licence of the exact sample at the
  time of pulling; MS-PL/MIT samples have been usable elsewhere in this repo (`serialhcibus`,
  `cywbtserialbus`).
- **Reference only (GPL-2.0)**: the whole vendor kernel tree — `drivers/media/platform/exynos/mfc/*`
  and `drivers/iommu/exynos-iommu*`. Cite, transliterate the sequence, paste nothing.
- **Firmware**: `mfc_fw.bin` is copied from the device's own `/vendor/firmware` at install time.
  **Never committed** — `tools/release/scan.py` and the blob ban both apply.
- Contributed code here: BSD-2-Clause-Patent.

## 6. Risks / open questions (each names its closing read)

| # | Risk or unknown | Closing read | Cost if wrong |
|---|---|---|---|
| P1 | Is the MFC SFR really readable in our boot state with the clock off, as the vendor comment claims? | R1, zero writes | nothing above R1 is meaningful; a hang here means the block is powered down and R2 needs the power domain first |
| P2 | Does the EL3 monitor / sboot leave the MFC in a state a second owner can use? | R1 values + `need_smc = <0x18820204>` on `pd-mfc` (an SMC-mediated power domain — the same monitor question D9 has) | power-domain control may be unavailable to us |
| P3 | SYSMMU ownership: two IOMMUs sit between the codec and DRAM, and nothing in our firmware describes them | read `exynos-iommu-reg.h` + observe whether the vendor leaves them enabled | codec DMAs to the wrong physical pages → silent corruption of a *filesystem* buffer. This is the worst failure mode in D10 |
| P4 | `mfc_fw.bin` version must match `ip_ver` exactly (`mfc_run.c:111`) | R3 | the driver refuses to run; a mismatched blob from another build bricks nothing but wastes a boot cycle |
| P5 | Which CMU bank feeds gate 422 | resolve the clock provider against `cmucal-*.h` | a wrong gate write pokes an unrelated block |
| P6 | TZPC "IP protection" is in the vendor's clock-on sequence (`mfc_pm.c:52-55`) and we have not read what it protects | mirror read of the TZPC driver | a protected window may reject our accesses |
| P7 | Is any of this worth it? *(inference, not a read)* Windows is expected to decode H.264 in software on this CPU via the in-box Media Foundation decoder, so D10 buys power/performance rather than capability. **Unverified here**, and the HEVC case in particular has a licensing wrinkle on consumer Windows that has not been checked | measured: R6 frames/s and power draw vs whatever software path the box actually has | if the software path is already fine for a docked desktop, D10 is the lowest-priority driver on the board — say so in the milestone row, do not let it displace D11 |
| P8 | The AVStream model is old; is it still supported on Windows 11 ARM64? | build a `SAMS9828` device with the Avshws-shaped minidriver and see it enumerate | if AVStream is dead on WoA, fall back to shape (2): KMDF + proxy hardware MFT |

## 7. What the tester does first

One module, one line, no writes:

```
# after flashing the FD that carries Beyond0LteMfcDxe (never this repo's GCC FD):
grep -E '\[MFC\]' <serial log>
```

Expected: `[MFC] R1: version=0x1300`. Rollback: a reboot — the module writes nothing.
Task file: `tasks/V02-mfc-r1-observe.md`.
