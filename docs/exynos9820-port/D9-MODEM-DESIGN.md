# D9 — WWAN: mobile data via the 9820's integrated CP — design

Plan-of-record: [`DRIVERS-MILESTONES.md`](DRIVERS-MILESTONES.md) §D9.
Evidence for every hardware claim: [`../../../notes/modem-wwan.md`](../../notes/modem-wwan.md)
(cited "T§" below). Constants: `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h`
§WWAN (D9). Nothing here is validated on the device; the rungs in §1 say what observation
closes each. This design was written from the vendor's own GPL source and the shipped DT —
where the vendor's userspace protocol is closed, that is stated, not papered over.

## 0. The honest headline

The S10e's modem is the Exynos 9820's **integrated CP** (Samsung project S500AP; T7).
Three planes, three different openness verdicts:

| Plane | Openness | Evidence |
|---|---|---|
| CP **boot** (power, reset, image download) | **open** — SMC id, PMU regs, AP2CP_CFG, STD_UDL boot-channel framing all in GPL source | T7, T8 (`sipc5.h:261-297`) |
| **link** (SBD rings, mailbox doorbell, IRQs, rmnet raw-IP frames) | **open** — full struct layouts pinned (T6) | `sbd.h`, `mcu_ipc`, `link_device_*` |
| **control** (SIM files, network registration, PDN activate, RFS NV-serving) | **CLOSED** — lives in `libsec-ril.so` ↔ CP firmware; kernel only frames/routes (proven at `misc_write`, T6); zero AT strings anywhere (T8) | U10 |

Windows requires **MBIM** for cellular (T10, verbatim MS requirement); the sanctioned
non-USB shape is an IHV lower-edge client driver translating MBIM over the vendor bus.
Prior art for **this chip family** is **empty**: no Shannon/Exynos cellular has ever run
under Windows (T9). The shape itself is proven elsewhere — Mu's Status.md lists
`Mobile Data ✅` under **Windows** on at least five Snapdragon phones (Mi 9/cepheus,
lisa, surya, miatoll, POCO X3 Pro/vayu), each via a vendor IHV driver package (second
review, T9 correction). What Exynos lacks is the translator, not the Windows-side
architecture.
So the deliverable ladder below ends at "CP alive + rings pumping under Windows" (R6);
the last step to *user-visible mobile data* (R7) is gated on the closed control plane and
a policy decision (§6, P5) — this design does not pretend otherwise.

## 1. Definition of done + rung ladder

Done for this arc: Windows owns the CP — the modem boots, the SBD link is established,
and raw IP frames demonstrably move on an rmnet ring pair, with a staged MBBCx skeleton
in the tree. **Out of scope until U10 resolves:** registration/PDN activation, i.e.
actually carrying traffic to the network.

| Rung | Observation | Where | Proves |
|---|---|---|---|
| R0 | live DT node names + `/proc/interrupts` lines for mif/mailbox (task **R01**) | Android (needs-phone) | the dump's contract is what the live device runs |
| R1 | `[CP] STATUS=1 IN=1` on debug serial after `AP2CP_CFG=1` (+ SMC 0x82000700 if needed) | UEFI serial | monitor SMC path + PMU CP regs live under our boot; closes P1/P2 |
| R2 | `[MBOX] issr3 cp_status=0x…` changes; an AP→CP `INTGR1` doorbell is answered | UEFI | mailbox `0x15920000` + doorbell contract (T6) |
| R3 | `[SBD] g_desc version=%d channels=%d` read back from `0xFB200000` after we create it | UEFI | shmem IPC base + SBD struct layouts + CP alias (P3) |
| R4 | `[BOOT] modem.bin 52428800 B downloaded, CP_START seen` — STD_UDL frames from UFS LBA 57856 | UEFI | boot plane end-to-end, using our own UFS driver |
| R5 | post a deliberately-unknown FMT message on `umts_ipc0` UL ring → CP consumes it (wp/rp move, error/ack frame appears in DL ring) | UEFI | ring data path WITHOUT any closed protocol |
| R6 | Windows: `ACPI\SAMS9827` device enumerates, driver maps windows, re-attaches the latched link | Windows | ACPI description + role-latch handoff |
| R7 | cellular carries IP (Settings shows the modem, traffic flows) | Windows | **gated on U10 — see §6 P5** |

Nothing above a rung is claimed until that rung is observed on the device.

## 2. Hardware contract (all VERIFIED unless marked)

| Item | Value | Source |
|---|---|---|
| CP shmem window (`modem_if`) | `0xF0000000` + `0xB900000`, partitioned cp/vss/l2b/**ipc**/vparam | T1, T6 |
| SBD IPC region | `0xFB200000` + `0x500000` | T6 (`link_device_shmem.c:3963`) |
| pktproc gdesc | at `modem_spare` `0xD0000000`; CP-side addrs via alias `0x50000000` (**UNVERIFIED** — P3) | T6/T8 |
| DIT | `0x1A2A0000` + `0x10000`; reg map `link_rx_dit.h:74-102` | T1/T6 |
| SYSREG_BUSC | `0x1A220000` + `0x2000` (bit meanings unread) | T1 |
| MCU_IPC mailbox | `0x15920000` + `0x180`; ISSR0..3 slots, INTGR1 doorbell | T6 |
| AP2CP_CFG | `0x15020000` + `0x40`, write 1 | T7 |
| PMU CP regs | `0x15860000` + `0x3000/0x3004/0x3014/0x3020/0x3024` | T7 |
| SMC | `0x82000700` (READ_CTRL 3 / WRITE_CTRL 4) | T7 |
| IRQs | MBOX **73**, CP2AP_WAKEUP **407** (=cp_wdt), DIT err/rx/tx **100/99/98** | T1/T7 |
| channels | `umts_ipc0/1` FMT ch 0xeb/0xec, `rmnet0..7` ch 0xa..0x11, `umts_boot0` 0xd7, `umts_rfs0` 0xf5 | T1/T6 |
| CP firmware | RADIO partition, LBA 57856 ×4096, 12800 LBAs; TOC at `+0xa0` | T4/T8 |
| hw-rev sense bits | gpg3-4..1 CP_REV0..3, gpg1-5..2 HW_REV0..3 | T7 (U7) |
| MCU_IPC clocking | **no clock/power-domain handling exists in `mcu_ipc.c`** and the `mailbox_cp` node has no `clocks`/`power-domain` property — the vendor accesses the block raw, so no enable step is needed (fact: absence, grep 2026-10-12); "already clocked at DXE entry" is inference, confirmed by the R2 print | second review |

## 3. Two halves — who owns what

**Firmware half (UEFI):** `Beyond0LteModemDxe` (new) runs the vendor's own order
(`s5000ap_on → boot_on → download → handshake`, T7): PMU CP status read → AP2CP_CFG=1
→ mailbox slot zeroing + ISSR2 `sbi_pda_active`/`sbi_ap_status` → map IPC shmem, create
SBD g_desc/rings (transliterate `create_sbd_link_device`) → read modem.bin from UFS
(our UFS driver already does 4096-B LBAs) and run STD_UDL on `umts_boot0` → complete
`INIT_START→PIF_INIT_DONE→INIT_END`. It **leaves latched at ExitBootServices**: CP on,
mailbox configured, SBD rings built, link online. Module:
`firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteModemDxe/`. **First slice
landed (2026-10-12, notes T13): observation only** — PMU status pre-read (P2) +
mailbox dump; the `AP2CP_CFG` write and everything after it are staged behind
`BEYOND0LTE_MODEM_INIT` / later slices.

**Role-latch contract:** the CP link is NOT a standard class device — the Windows driver
is ours end to end, so `_CRS` may carry everything it needs. But PMU + SYSREG_BUSC stay
**out** of `_CRS` (firmware-owned, like the D3 XHC0 pattern); DIT stays out until the
zerocopy path is actually built (R5 uses plain rings, not DIT).

**Windows half:** DSDT `Device (WWAN)` `_HID "SAMS9827"` (next free after SAMS9825
WLAN / SAMS9826 BT); `_CRS` order = driver resource contract: MBOX_CP window,
AP2CP_CFG window, CP_IPC_SHMEM window, `Interrupt(Edge? Level?, …){73}` + `{407}`
(polarity per GIC SPI type cell = 0 → the repo's level-high convention, confirm at R6),
`_CCA 1`. No inbox driver exists for this (T10(c): inbox MBIM binds USB only) — the
bind is our `ACPI\SAMS9827` NDIS/MBBCx client.

## 4. Windows driver layering — `drivers/Exynos9820Wwan/`

Per T10 the Windows-correct shape is a **NetAdapterCx + MBBCx client** (MBBCx converts
NDIS WWAN control OIDs to MBIM messages for the IHV driver; data path = raw-IP NBLs,
`NdisMediumWirelessWan`, per-connection CID ↔ our rmnet channel 1:1).

Layer by layer:

1. **bind** `ACPI\SAMS9827`, map the three windows — skeleton implements.
2. **link attach**: re-find our own `g_desc` at `0xFB200000` (magic/version check),
   adopt ring pairs, take over mailbox — skeleton stages (NOT-IMPLEMENTED marker →
   `link_device_memory_sbd.c` + rung R3/R6).
3. **MBIM responder**: answer MBIM commands (device caps, subscriber info, pin,
   registration, activation) — **NOT-IMPLEMENTED, gated on U10**: each maps to a
   SEC-RIL request whose encoding is closed (§6 P5). Markers name the reference
   (`MBIM 1.0 spec` for the Windows side; nothing adoptable for the CP side).
4. **data path**: raw-IP NBL ↔ rmnet ring post/poll — skeleton stages the ring I/O
   (open contract, T6) but refuses traffic until 3 exists.
5. **DIT/pktproc zerocopy**: deferred entirely (optimization; P4 says plain rings first).

## 5. License posture

- **Adoptable upstream: none** for the modem stack — no mainline driver is known for
  `sec_modem,modem_pdata` (vendor-only family per T6; web query `"sec_modem,modem_pdata"
  OR "exynos-shm_ipc" OR "exynos-shd-ipc-mailbox" compatible` → zero hits, 2026-10-12;
  elixir.bootlin.com bot-walled, dead end); prior-art rung empty (T9). The Windows-side
  shape follows MS docs + MBBCx samples (MS-PL).
- **Reference only (GPL-2.0):** `drivers/misc/modem_v1/*`, `drivers/misc/mcu_ipc/*` —
  transliterate sequences, cite lines, never paste (repo rule).
- **Closed, do not RE into committed code:** `libsec-ril.so` wire encoding, CP firmware
  internals. The repo hard stop forbids committing reverse-engineered vendor internals —
  this binds R7 (P5).
- **Firmware:** `modem.bin` stays on the device's RADIO partition; the driver reads it
  via the disk stack at runtime (or UEFI reads LBAs directly) — never committed, never
  copied into the tree. cpefs NV files likewise device-side only.

## 6. Risks / open questions (each names its closing read)

| # | Risk or unknown | Closing read | Cost if wrong |
|---|---|---|---|
| P1 | Is the Samsung EL3 monitor (SMC 0x82000700 handler) still resident when our UEFI runs? | R1: SMC from UEFI, print return | CP can't be powered from our stack → boot-plane work moves into the shim/monitor carve-out |
| P2 | Does sboot pre-boot the CP before `load_kernel` (Android's CP boot is kernel+cbd-driven — T4)? | R1: read PMU CP_STATUS/CP_IN **before** writing anything | sequencing change only; observation first, then write |
| P3 | CP alias `0x50000000` ↔ AP `0xD0000000` is a source-comment read, not observed | R3/R4: does the CP acknowledge g_desc/rings addressed through the alias | ring setup must move into the CP's own view; contract unchanged |
| P4 | Is plain SBD ring polling (signaling=0, legal per `sbd.h:99-102`) enough for RX, or does the CP only push via DIT/pktproc? **The vendor AP always writes `signaling = 1` (`setup_sbd_rb_desc`, `link_device_memory_sbd.c:110`) — the polling path is our construction, never exercised by the vendor (second review)** | R5: DL ring activity without DIT programmed, and with signaling=0 vs 1 both tried | must build the DIT ring path in UEFI first (reg map already pinned) |
| P5 | **The wall:** SIM/registration/PDN/RFS control encoding is closed (U10); repo policy forbids committing RE'd vendor internals | options, cheapest first: (a) R5 error-frame probe — does the CP expose any open control surface (bootloader-style cmds)? (b) TWRP capture of framed `umts_ipc0` to size the dialect (observation, not committed RE); (c) an explicit policy decision on shipping a device-local translator; (d) Samsung publishes the SEC-RIL dialect (no evidence); **(e) static RE of `libsec-ril.so`/`cbd` in the gitignored `re/` workspace (T14); findings stay out of the tree, shipping a translator built from them is an open policy decision** | R7 unreachable → D9 ships as "CP alive, no user data" — must be stated in MILESTONES, not hidden |
| P6 | ss vs dsds on this unit (U8) | R01 `getprop ro.vendor.multisim.simslotcount` | only whether `umts_ipc1` matters |
| P7 | CP NV/calibration plausibly comes from cpefs served by AP userspace over the RFS channel (`umts_rfs0` exists, T1; cpefs is mounted by Android init, T4 — the *link* between them is my inference, not a read) | R5 + TWRP listing of cpefs (U9) + mirror grep of the RFS client/server split | a booted CP may never register without an AP-side RFS server — widens P5 |
| P8 | Is the CP shmem writable from non-secure EL1 under our boot (Android maps it normally; monitor stage-2 unknown)? | R1/R3: write g_desc, read it back | link must be built by the monitor side; same relocation as P1 |

## 7. What the tester does first

R0 = task **R01** (`tasks/R01-modem-live-reads.md`) — read-only adb, no device risk.
R1 lives in the UEFI PoC once the boot stream lands (this arc is behind M8/M9 in the
queue — same "device rungs ride the boot stream" caveat as D4b/D4c). Rollback for every
device step: it is a read or a register write Android itself performs; rebooting to
Android restores vendor state; nothing writes media.

## 8. Explicit stop line

Per HANDOFF §2 rule 7 ("if something can only be decided on a real phone, say so and
stop") and the hard stop on RE'd internals: **R7 is not scheduled.** The arc commits
R1–R6 machinery + this design; P5 stays open — whether the closed-control
question is worth a capture experiment, a policy decision, or parking WWAN as a
documented dead end for the donation build (LAN/WiFi already cover the network need —
M12 decision, 2026-10-10).
