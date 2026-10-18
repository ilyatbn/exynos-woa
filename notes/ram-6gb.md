# RAM: towards the full 6 GB (user 2026-10-18: "see about getting the full 6gb of ram")

Basis: notes/cpu-ram-plan-20261012.md (Findings - RAM, plan #2 run r2, plan #3), notes/m13-ram.md.
- DRAM 3 x 2 GiB = 6144 MiB; live System RAM (Android DT, /proc/iomem) 6034 MiB - 110 MiB holes are not RAM even for
  Android (0xBAB00000-0xC0000000, 0xE0000000-0xE1900000): unrecoverable.
- Windows sees 5370 MiB: our PlatformMemoryMapLib carve-outs (~660 MiB) explain the whole rest.
- r2 write probes from Windows EL1 (boot #99): sec_rdx_bootdev, modem_spare, modem_if2, cp2cp, modem_if,
  cp_ram_logging read/write/read-back OK; secure_camera read 0 / write OK.
- Plan #3 (publish those as RAM, keep debug_snapshot 0xFD900000..4 GiB) waited for the user's decision - this request
  is that decision. secure_camera (34 MiB, "likely TZ-protected") stays carved: a TZ-protected page handed to Windows
  would fault at an arbitrary later time.
- No SMBIOS -> Task Manager shows no "installed" figure. Usable can never be 6.0 GiB (holes + uH/sboot/fb carve-outs),
  but SMBIOS type 16/17/19 can report the 6 GB installed.

## Plan (one commit each)
- R1 memory map: publish modem_spare/modem_if2/cp2cp/s5100_msi, modem_if + cp_ram_logging (not debug_snapshot),
  sec_rdx_bootdev in the Beyond0Lte PlatformMemoryMapLib (9810 tree untouched). Expect ~5.9 GiB usable.
- R2 SMBIOS (type 0/1/16/17/19) so Windows reports 6 GB installed.

## Progress log
- 16:55 alpha7 tagged at 4fdb98a first (user: "Tag alpha6 before" -> alpha6 existed locally at 07f0c8f; user chose a new tag alpha7).
- R1 source: PlatformMemoryMapLib.c BEYOND0LTE_PUBLISH_CP_RDX 1 - mMonitorReservations drops the modem block
  (0xD0000000-0xD9801000), modem_if+cp_ram_logging (0xF0000000-0xFD900000) and sec_rdx_bootdev (0x880000000+0xAD00000);
  keeps cp_ram_logging end (EXYNOS_RSVD_CP_RAM_BASE+SIZE = 0xFD900000, both live reads K10-rkp T17) .. 4 GiB =
  debug_snapshot (log_bcm 0xFEB10000, log_llc, log_dbgc, log_kevents - T17). Expected +542 MiB -> ~5912 MiB.
  Checked: no other code uses these ranges (rg of the bases in drivers/ and firmware/); UEFI's UFS DMA bounce is bank0
  only (UfsHcDxe AllocateMaxAddress below the bank0 end), so publishing more of bank1/2/high changes nothing for it.
  Note: debug_snapshot.log_pstore 0xEB308000+0x800000 sits inside published bank2 already (pre-existing, K10-rkp).
- Current image boot-host-btmfc.img = 5c445be + build-only GPU_POWER_ON 1, WINDOWS_WATCHDOG 0, BT_UART_PROBE 1,
  MFC_PROBE/POWER/FWBOOT 1, USB_KD_DEVICE 0 (HANDOFF 56); no firmware commits since 5c445be. New
  firmware/tools/build-host-image.ps1 applies exactly those overrides to a build, packs (-Ref ref-recovery.img), and
  restores the sources.
- before R1: TotalVisibleMemorySize KB: 5498480  TotalPhysicalMemory B: 5630443520
- firmware/tools/build-host-image.ps1 (new; -Watchdog 0|1): s10e-artifacts\boot-host-r1.img 5e9a894c... (daily,
  watchdog 0) and boot-host-r1-wdt.img 433e4f87... (WINDOWS_WATCHDOG 1: the first unattended boot - the user is
  remote, and with 0 the firmware stops the 82 s timer at ExitBootServices, so a Windows hang from a bad page would
  need the key combo). Both 18255872 B (= boot-host-btmfc.img). Fixes on the way: PS 5.1 NativeCommandError from
  build stderr under Stop (separate processes + Continue around the native calls); sources restored byte-exact each run.
- 17:02 totwrp for the R1 flash
- 17:04 flashed boot-host-r1-wdt.img (433e4f87, readback OK; backup boot-before-20261018-170353.img = boot-host-btmfc.img). Booting.
- 17:06 R1 wdt image BOOTED (Windows up ~55 s). TotalVisibleMemorySize 6053492 KB = 5911.6 MiB (before 5498480 KB = 5369.6 MiB: +542.0 MiB, exactly the published amount), TotalPhysicalMemory 6198775808 B. Next: fill/verify stress.
- Stress (tools/phone/ram/memfill.c, new): `memfill 3600 2` -> load 38 % -> 98 % (81 MB of 5911 MB available = Windows
  on essentially every physical page incl. the new ranges), 3584 MB written, 2 read-back passes 0 mismatches, no
  fault/reset (no new Kernel-Power 41). d3dcs ARM64 + x64 0/7 differ afterwards. -> R1 verified. Swapping to the daily
  image (watchdog 0).
- 17:10 daily boot-host-r1.img (5e9a894c) flashed + booted (~55 s): TotalVisibleMemorySize 6053492 KB again. R1 DONE. Phone firmware = boot-host-r1.img (rollback: boot-host-btmfc.img).
