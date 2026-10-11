# PROGRESS — open tasks by ID

Start here. One line per task; the detail lives in `tasks/<ID>-*.md`.
Indicator: `[ ]` open, `[x]` done (the task file carries the same `Status:` line).
A task is not `[x]` until its verification actually ran.

## Boot/install path — what gates trying the S10e

- [x] **T01** — Re-pick the UFS DMA window inside the live bank0 → `tasks/T01-ufs-dma-window.md`
- [ ] **T02** — Split the framebuffer out of the bank0 RAM gap → `tasks/T02-framebuffer-overlap.md`
- [ ] **T03** — Resolve the UFS DTB extent disagreement (live vs vendor DT) → `tasks/T03-ufs-extent-disagreement.md`
- [ ] **T04** — Handle the second LUN (`cpefs` is `sdd1`, driver written against one `sda`) → `tasks/T04-second-lun.md`
- [ ] **T05** — Re-derive the timeout clock from the 9820 tick rate (26 MHz is the 9810 value) → `tasks/T05-timeout-clock.md`
- [ ] **T06** — Re-read the **stock** recovery container geometry before repacking (needs a root shell) → `tasks/T06-stock-container-ref.md`
- [x] **T07** — The first boot attempt itself (consent, AVB `vbmeta` ordering, then observe) → `tasks/T07-first-boot.md`  *(attempted 2026-10-10: fell back to download mode, Windows not established)*
- [ ] **T08** — Make the installer app able to drive the S10e (stages are `NotImplemented` on purpose) → `tasks/T08-installer-s10e-path.md`
- [ ] **T09** — Append a valid DTB to the boot payload so sboot's load_kernel passes the DT check (the real first-boot blocker) → `tasks/T09-boot-payload-dtb.md`

## WiFi (M12/D4b — reopened 2026-10-11: the chip is a BCM4375 on PCIe0)

- [ ] **W01** — rung R1: `[PCIe] R1: endpoint 14e4:xxxx` on serial at boot → `tasks/W01-wifi-r1-linkup.md`  *(needs-boot)*
- [ ] **W02** — live reads: PCI device ID, firmware blob names, SPI 253 → `tasks/W02-wifi-live-reads.md`  *(needs-phone)*
- done (code, unvalidated): research `notes/m12-wifi-chip.md` · design
  `docs/exynos9820-port/M12-WIFI-DESIGN.md` · `Beyond0LtePcieDxe` (in the FD) ·
  DSDT `Device (WLAN)` (in the FD) · `drivers/Exynos9820Wlan/` skeleton (needs-windows build)

## Parked — off the boot path

- [ ] **P01** — heimdall auto-restore/reflash path for a fully broken device → `tasks/P01-heimdall-restore.md`
- [ ] **P02** — UFS hw-rev class-6 HCI encoding (the DTB says `hw-rev = 6`) → `tasks/P02-ufs-hw-rev.md`
- [ ] **P03** — Which CMU bank feeds PERIC0/FSYS0A (the §5 hazard) → `tasks/P03-cmu-bank-hazard.md`
- [ ] **P04** — Guided vbmeta→TWRP flash for a stock S10e (native Odin flasher, no zadig) → `tasks/P04-vbmeta-twrp-flash.md`

## Later milestones — tracked in MILESTONES, no task file yet

M10 USB-C host mode · M11 external display · M12 WiFi · M13 LAN · M14 power/thermals ·
M15 the donation build. Their open rows and notes are in
[`docs/exynos9820-port/MILESTONES.md`](docs/exynos9820-port/MILESTONES.md); make a
`tasks/<ID>-*.md` file when one is actually started.

## Finished this session (do not redo)

K10 (write-protected zones = a mechanism, not a constant) · K4b (sboot drop =
`0x80118000`, kernel window to `0x832D0000`) · FD base moved to `0x83300000` in
lockstep and re-run end to end · DRAM/MMIO extents closed from the live DTB ·
monitor-owned RAM carved out of the memory map · three carried numbers proved
wrong and fixed. Detail: `notes/K10-rkp.md`, `notes/k4b-drop-address.md`,
`notes/s10e-dtb-mmio.md`, HANDOFF §16/§17.
