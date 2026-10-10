# T07 — The first boot attempt on the S10e

Status: **open — needs explicit user consent before any write**

## Goal

Get `BEYOND0LTE_UEFI` running on the phone: write the packed image into the
**recovery slot only**, boot it, and read what the hardware says. Nothing in this
repo has booted on an S10e yet, so this task is the point where `Guided` stops
being a label and becomes a fact.

## The artifact

- Candidate: `C:\Users\ilya\AppData\Local\Temp\opencode\pack-out-833.img`,
  **18 255 872 B**, built by `firmware/tools/pack-uefi-beyond0lte.ps1` against
  the TWRP-era recovery ref (`page_size` 2048, ramdisk/second/dt/tail kept
  byte-identical). Regenerate with `-Ref <pulled recovery>`; the FD inside it
  links to run at **`0x83300000`** (the K4b decision, `notes/k4b-drop-address.md`).
- Slot size: the recovery partition is **55 279 616 B** → a 3.0× margin, which
  matters because heimdall has no size check (`notes/heimdall-flashing.md` H03).

## Rules that bind this task

- Ground rules 5/6: **nothing may be written to the media until Windows booting
  is established.** Proceeding is an explicit decision by the user, not by the
  agent.
- **AVB (Team Win's own page):** a stock S10e is protected by Android Verified
  Boot; flashing without a verity-disabled `vbmeta` first leaves the device
  unable to boot. This disk's `vbmeta` is **LBA 83 584..83 599**
  (`notes/s10e-root-reads.md` §2). Evidence that the recovery slot is not
  currently AVB-fatal: TWRP booted from it.
- The write is isolated to the recovery slot; `boot` (`sda14`) stays untouched,
  so normal Android still boots.

## Order

1. `heimdall detect` → `heimdall print-pit` (read-only) — confirm the slot name
   and its block count against the GPT reads before writing anything.
2. `vbmeta` first (verity-disabled), then the recovery slot.
3. Boot the recovery slot and **observe**: does the panel show the UEFI output,
   does it bootloop, does it sit silent? Each of those is a different fault class
   (paint address, drop address, watchdog not stopped).
4. If it fails: restore. The rollback file on disk is a **TWRP-era** snapshot, so
   after a stock restore the correct rollback is the stock firmware via Odin, not
   `ref-recovery.img` (see `tasks/T06-stock-container-ref.md`).

## Done when

Something is observed on the panel or in a bootloop, and the observation is
written to `notes/m8-uefi-image.md` — a build succeeding says nothing about a
boot hazard, and that difference has to be recorded.
