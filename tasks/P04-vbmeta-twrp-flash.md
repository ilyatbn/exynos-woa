# P04 — Guided vbmeta→TWRP flash for a stock S10e (parking lot)

Status: **open — parking lot (off the boot path; the first boot was done with TWRP flashed manually)**

## Goal

Give the `beyond0lte` `twrp` stage a real, user-facing procedure (and ideally an
automated native flash) so a **stock / reverted** S10e can get TWRP back and stay
bootable. Today the stage is `Guided` and only prints text
(`StageCatalog.cs:73`, `:110`, `:137-139`).

## Why it is needed

- A stock S10e is protected by **Android Verified Boot**. Team Win's own warning
  (quoted in `StageCatalog.cs:138-139`): flashing TWRP **without first flashing a
  verity-disabled `vbmeta`** leaves the device **unable to boot**.
- The repo's native flasher path exists but is wired for the 9810 flow:
  `AppServices.cs:94-106` prefers `OdinTwrpFlasher` (the installer's own Odin-protocol
  writer over the **official Samsung USB Driver for Mobile Phones**, `HostPreflight.cs:103`),
  with `HeimdallTwrpFlasher` only as a fallback. The Samsung-driver route needs **no zadig/WinUSB**.

## Order (the guide must state this)

1. Flash the **verity-disabled `vbmeta`** first (this disk's `vbmeta` is LBA 83 584..83 599,
   `notes/s10e-root-reads.md` §2).
2. Flash **TWRP to RECOVERY** (sda15) from Download mode.
3. Boot TWRP once to confirm it runs.

## Reference artefacts on this machine (for the guide / test)

- `C:\Users\ilya\code\twrp-3.7.0_9-2-beyond0lte.img.tar` — 56 514 560 B, the official
  beyond0lte TWRP (matches `TwrpProfiles.cs:37` `twrp-3.7.0_9-2-beyond0lte.img`).
- `C:\Users\ilya\code\vbmeta_disabled.tar` — 2048 B, verity-disabled vbmeta.

## Done when

Either the `beyond0lte` `twrp` stage runs the native `OdinTwrpFlasher` end to end on a
stock device and the phone boots TWRP, **or** the app ships a written step-by-step guide
(vbmeta first, then TWRP, Samsung driver, no zadig) in the Guided text. Keep the stage
`Guided` until it is validated on hardware (ground rule 3).
