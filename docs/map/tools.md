# Map: tools/twrp-winre/ — WinRE-look TWRP re-skin

Python **reference implementation** of the WinRE-look recovery (~6.5 kLOC). End users never
run it: the C# `Core/Twrp/WinReTwrpBuilder.cs` does the same build on the user's PC from the
same committed theme/art/scripts. The two are kept in step by tests on both sides.
This directory is where you iterate on the theme and art.

Base: official `twrp-3.7.0_9-0-star2lte.img` from twrp.me. The build never rebuilds TWRP:
kernel (bar a 4-byte power-off patch), DTBH, boot header and untouched ramdisk entries
carry across byte-for-byte; only `/twres`, `/sbin` helpers, modules and one line of
`init.recovery.usb.rc` change. RECOVERY partition is 68,149,248 B; a WinRE build ≈ 42 MiB.

## Scripts

| File | Lines | What |
|------|-------|------|
| `build.py` | 638 | orchestrator → `star2lte-winre-recovery.img`, `ui.zip`, `build-report.txt`; `--gears <gif>` optional |
| `bootimg.py` | 274 | boot image split/join, newc cpio round-trip, LZMA-alone helpers (C# port: `AndroidBootImage.cs`/`CpioArchive.cs`/`Lzma/`) |
| `poweroff.py` | — | the four-byte power-off route kernel patch (hash/pattern gated) |
| `mkassets.py` | 927 | draws ALL original art: icons, gears, bars, replacements for every one of the 57 stock TWRP bitmaps (same names/sizes); TeamWin logos → empty images |
| `validate.py` | 695 | coverage gate: classifies every page; fails on any TWRP colour/TeamWin logo/stock header/scrolling log or foreign bitmap; `--release` `--pages` |
| `fonts/make_fonts.py` | — | compiles the open font set from Selawik (SIL OFL 1.1) |

## Theme XML

| File | Lines | What |
|------|-------|------|
| `theme/winre.xml` | 1,474 | the WinRE pages: home *Choose an option* (Continue / Restart to recovery / Troubleshoot / Turn off), Troubleshoot actions, install screen, `lock` (tap-anywhere), `singleaction_page`, `action_page`, `action_complete` |
| `theme/reskin.xml` | 340 | stock reskin rules: palette (black/white/grey, Windows blue `#0078D4`, accent `#60CDFF`), Segoe fonts, no-header page template, gears on operation pages, dark keyboards; renames hand `lock`/`singleaction_page`/`action_page`/`action_complete` to winre.xml. Every op has an exact match count → a different base theme stops the build |
| `theme/splash.xml` | — | gears on black splash (replaces TeamWin splash) |

## Overlay scripts (`overlay/sbin/`, baked into the ramdisk)

| Script | What |
|--------|------|
| `winre-actions.sh` (304L) | Troubleshoot actions + `prepare-boot` (MISC request, ack, startup records) — same fix as `BootRouteService` |
| `winre-statuswatch.sh` (264L) | raises/clears the "Installing Windows" screen from `/tmp/s9woa/status` via `setprop` + `twrp changepage=` (the only ORS command TWRP serves without flashing `singleaction_page`); 300 s stale = crashed installer; verified against `Set page:` lines in `/tmp/recovery.log` |
| `winre-ntfs-watchdog.sh` (129L) | ntfs-3g FUSE deadlock breaker, registered as **init service** (`init.recovery.service.rc`) — must run from init because the deadlock strikes while TWRP mounts `/data`, before any boot script |
| `rebootsystem.sh` | TWRP's reboot-to-system hook: runs `winre-actions.sh prepare-boot` before every restart into Windows |
| `postrecoveryboot.sh` | boot-time self-heal (deletes stale `TWRP/theme/ui.zip` for the standalone case) |
| `winre-gui.sh` | console output path (`function="terminalcommand"`, pads console, `echo " "`) |
| `winre-pram.sh` (223L) / `winre-debug.sh` | Check/Erase PRAM, Collect diagnostics |

## GPL kernel modules (`modules/`, baked under `/sbin/s9woa`)

| Module | Source | What |
|--------|--------|------|
| `rwd1_ack.ko` | `rwd1_ack.c` (387L) | acknowledge `RECOVERY_PENDING` watchdog record |
| `rwd1_evidence_reader.ko` | `rwd1_evidence_reader.c` (307L) | read RWD1 (64B @0xFED13D80) + P3/SMP1 (128B @0xFED13E80) whole |
| `rwd1_clear_poc.ko` | `rwd1_clear_poc.c` | zero RWD1 record — **token-gated** (fixed constant must match `BootRouteService.cs`; not a secret) |
| `pram_smp_clear_poc.ko` | + `pram_cache.h` | zero P3/SMP1 record — any non-zero P3 content halts UEFI at the Samsung logo |

## Art

`assets/images/` (81 PNGs): icons (`winre_ic_*`), gear frames (`winrecog*`), bars (`winrebar*`), install progress bar.
`assets/stock/` (57 PNGs): an original for every stock TWRP bitmap.

## Device traps encoded here

Full list in `tools/twrp-winre/README.md` — measured on device / read from TWRP 3.7 source.
Key ones: `terminalcommand` not `cmd`/ORS; `<animation>` needs `retainaspect="1"`;
`<condition var2="">` is false; conditions evaluated on page entry not per frame;
GUIText every-4th-update counter needs an **odd** number of texts on the page;
`GUIProgressBar` uninitialised slide counter → set `ui_progress_frames=0`;
animation speed `floor(30/fps)+1`; busybox `timeout -t`, whole-second `sleep`;
never `setprop ctl.stop recovery`; internal storage is `/data/media` not `/data/media/0`.
