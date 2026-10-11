# M12 WiFi milestone 1 — BCM4375 PCIe link-up in UEFI, ACPI WLAN device, Windows driver reads the chip ID

Goal: code-complete and BUILT, ready for a device test (no flashing in this session — another
session owns the phone). Ported from `origin/drivers` (base 419fc03) onto main by copying the
files (`git checkout origin/drivers -- <paths>`), never `git merge`. Plan:
`notes/drivers-branch-port-plan.md`. Chip research: `notes/m12-wifi-chip.md`. Design:
`docs/exynos9820-port/M12-WIFI-DESIGN.md`.

## Tasks (one commit each)

1. PcieDxe port + review fixes (Stall, FILE_GUID, BEYOND0LTE_PCIE_WLAN flag, 0x68 progress guard,
   'WLAN' status word, capped retries) + Exynos9820.h M12 constants + DSC/FDF.
2. PlatformMemoryMapLib: PCIe0 windows.
3. DSDT `Device (WLAN)` `SAMS9825`, `_STA` gated on the status word.
4. Windows driver `drivers/Exynos9820Wlan` building + signed package.
5. Notes / handoff / milestones.

## Progress log

- 2026-10-12: worktree branch `worktree-agent-afe491e33df42f9e6` was at a8d8a62 (264 behind main);
  fast-forwarded to main 302c285 (`git merge --ff-only main`, no merge commit).
- Exynos9820.h: main has none of the M12 PCIe/CMU_FSYS1/WLAN names (grep `PCIE|FSYS1|BCM|WLAN`
  hits only `EXYNOS_UFS_IOCOH_BASE 0x13C10700`). `EXYNOS_CMU_TOP_BASE`, `EXYNOS_PMU_BASE`,
  `EXYNOS_PERIC0_GPIO_BASE`, `EXYNOS_ALIVE_GPIO_BASE` already exist on main with the same values the
  drivers branch uses -> no duplicate or conflicting defines. Only the M12 block is needed (the D8
  ALIVE-EINT/ABOX block on the same drivers commit range is out of scope here).
- Progress page 0x8FE0F000: [0] prev boot last code, [1] this boot code, [2] USB status (cleared by
  SEC, `ArmPlatformLib.c:100`). Word [3] (0x8FE0F00C) is used by nothing (repo-wide grep for
  `8FE0F00C` / `[3]` on that page: no hits; bootshim only writes [0]/[1]) -> free for 'WLAN'.
- Progress codes in use: 0x51..0x56 shim, 0x60..0x63 SEC, 0x65 parking, 0x66 DVFS/touch, 0x67 USB,
  0x70..0x72 BootLogDxe, 0x80 (BootLogDxe check). 0x68 is free.
- PcieDxe as on drivers: FILE_GUID ...5549 (collides with Beyond0LteSecArmLib.inf), Stall(100*10)
  = 1 ms for a 100 ms WLAN_EN settle, no flag, no guard, worst case 10 tries x (200 ms PERST +
  200 ms poll + PHY seq ~2 ms) ~ 4 s. EP BAR0 written without sizing, BAR2 never assigned.
- Kernel re-read (raw pcie-exynos-host-v0.c lineage-23.2, 2026-10-12): establish_link waits
  usleep_range(18000,20000) after PERST# high (:1240), polls LTSSM 2000 x udelay(10) = 20 ms (:1292),
  retries up to 10 (:1300) and gates the PHY clock between tries. The drivers-branch port waited
  200 ms + 200 ms per try with a comment claiming "18-20ms" -> comment was wrong; kept the longer
  waits (tolerant first bring-up) but cut tries to 3, worst case ~1.2 s, every try logged.
- Order change vs the plan: the memory-map commit goes FIRST (with the constants), because the DXE
  touches those windows; a DXE-before-map commit would sync-abort if built alone.
- EXYNOS_FSYS1_GPIO_SIZE renamed EXYNOS_FSYS1_GPIO_SIZE_UNVERIFIED (AGENTS §6 naming); one page,
  only gpf2 (+0x20) accessed. BCM4375_EXPECTED_PCI_ID renamed ..._UNVERIFIED (not used by firmware;
  the device print closes it).
- Overlap check of the 7 new map windows against every existing 0x13xxxxxx/0x14xxxxxx window
  (UFS 0x13D5..0x13D7, UFS_IOCOH page 0x13C10000): none.
- PcieDxe fixes applied: Stall(100*1000) (100 ms); FILE_GUID ...5553 (repo grep: unused before;
  Guid.xref of the build lists `...5553 Beyond0LtePcieDxe`); `#define BEYOND0LTE_PCIE_WLAN 1`;
  0x68 guard in the UsbHostDxe shape ([0]==0x68 -> skip; [1]=0x68 while running, restored after);
  word [3] zeroed at DXE entry AND in SEC (ArmPlatformLib.c next to the [2] clear) - SEC clear is
  needed because WINPRAM survives a warm reset: a build without the DXE (or a skipped boot) would
  otherwise expose a stale 'WLAN'. 'WLAN' = 0x4E414C57 written only after link-up + VID 0x14E4.
- Behaviour additions (each logged): one canary read of CMU_FSYS1+0x2000 before any write, gate/QCH
  readback after ClocksOn, first ELBI read logged after WLAN_EN settle, per-try "[PCIe] link try n/3",
  failure line with RDLH + PM_DSTATE, link-up line with try/poll count. EP: all BARs sized with the
  standard all-ones probe (decode off), placed naturally aligned in the MEM window (0x14000000+0xFF0000),
  BAR that does not fit -> logged, left 0. COMMAND = MEM only (drivers branch also set BME; no DMA is
  needed before firmware download, so BME is left to the Windows side in milestone 2). Chipid read at
  BAR0+0x3000 (bcmdhd pcie_core.h:55-60, 16 KB BAR0 layout) when BAR0 >= 16 KB.
- Build blocker found + worked around (no repo change): firmware/edk2 is gitignored and absent in a
  worktree. A junction worktree/firmware/edk2 -> main's edk2 makes GNU make (GnuWin32 3.81) fail
  `CreateProcess(NULL, Trim ...)` (e=2): make's PATH search does not find Trim.bat through a junction
  (reproduced with a 1-line Makefile: via junction path fails, via the real path works). Fix used: a
  scratch copy of build-uefi.ps1 with $ws = worktree firmware and $edk2 = main firmware\edk2 (no
  junction); also dropped the empty and `Calibre2"` PATH entries. Build output goes to main's
  firmware/edk2/Build (shared, as instructed).
- BUILD (DEBUG, CLANGPDB, PcieDxe + memory map + SEC clear + DSDT WLAN all in the tree): exit 0,
  `- Done -`, FVMAIN_COMPACT 759104/761856 used, 2752 B free (was ~3136 B before this work);
  BEYOND0LTE_UEFI.fd sha256 4b8abc3f402d5913...0d815. Dsdt.aml contains SAMS9825 + WLST region.

## Review of commits 9e9f5dd (1/5) and 6a0dd62 (2/5)

- FOUND (1/5): the commit also contains the six verbatim origin/drivers PcieDxe files - they were
  still staged in the index from the initial `git checkout origin/drivers -- <dir>` and `git add
  <paths>` does not unstage other entries. Harm: none functional (module not in DSC/FDF until 2/5, so
  not built), but the commit message does not say it. Not rewritten (unpushed, but AGENTS §4: follow-
  ups, no folding); history now reads "verbatim port in 1/5, fixes in 2/5". Lesson: `git status` before
  every commit, not after.
- Checked mechanically: FILE_GUID ...5553 appears once in firmware/ (grep), old names
  EXYNOS_FSYS1_GPIO_SIZE / BCM4375_EXPECTED_PCI_ID have zero users in firmware/ (grep -w),
  `git diff --stat 302c285 HEAD -- Star2LtePkg Exynos9810Pkg` empty (9810 tree byte-unchanged),
  every Stall in the module re-read (only the 100 ms WLAN_EN one changed meaning).
- Checked by reading: guard sets [1]=0x68 BEFORE the first CMU_FSYS1 read (inside PcieWlanBringUp);
  [1] restored on every return path (single exit after the call); 'WLAN' only on EFI_SUCCESS, which
  needs link-up + VID 0x14E4; BAR sizing restores the original value before assigning, handles
  unimplemented (0), dead (~0), I/O, 64-bit (upper sized, BAR+1 skipped) and does-not-fit BARs;
  Cursor alignment assumes power-of-two sizes (PCI guarantees it).
- Accepted risks (recorded, not bugs): ClocksOn writes ACPM/HWACG-owned gates directly (open item
  P4); a PCIe completion error on the CFG/BAR reads could raise an SError rather than return ~0 -
  covered by the 0x68 guard (the boot after skips WLAN), not prevented.

## Review of commit cb15de2 (3/5)

- grep: SAMS9825 is used only by Dsdt.asl WLAN and the Exynos9820Wlan package (no _HID collision;
  SAMS9821 = UFN0, SAMS9820 = I2C0). GSIV 285 appears once in the ACPI tables (no other claimant).
  0x8FE0F00C / 0x4E414C57 identical in Dsdt.asl and Beyond0LtePcieDxe.c. Field width DWordAcc 32 =
  the UINT32 word the DXE writes. Nothing to fix.

## Windows driver decision + build (task 4)

- Decision: KMDF probe (Class=System), not the drivers-branch NDIS miniport. Reasons: milestone 1
  asks only "binds + reads chip ID"; an NDIS miniport adds INF keyword requirements
  (*IfType/*MediaType/*PhysicalMediaType/Characteristics), NDIS registration and OID handling - all
  failure modes unrelated to the question, and the skeleton was never compiled. The NDIS sources
  (WlanDriver.c, the Class=Net .inf) are NOT ported; they stay on origin/drivers (5746999, cb6a90f)
  for milestone 2+.
- The probe writes nothing to the chip (no BAR sizing, no COMMAND write, interrupt not connected),
  checks _CRS against the DSDT contract, reads VID/DID/COMMAND/class/subsystem/BAR0..5 via the CFG0
  window and chipid at BAR0+0x3000 only when BAR0 is a >=16 KB memory BAR inside the MEM window with MEM
  decode on. Sinks: C:\Windows\System32\LogFiles\Exynos9820Wlan.txt (NOT Windows\Temp - corrupt index on
  this unit, m12-touch run 6), Services\Exynos9820Wlan\Parameters (Report + DWORDs), DbgPrintEx.
- Build 1: C4013 ZwFlushBuffersFile (ntifs.h only) -> dropped, FILE_WRITE_THROUGH instead.
  Build 2: Inf2Cat 22.9.7 "DriverVer postdated" - stampinf uses the LOCAL date (10/12, 00:3x) and the
  msbuild Inf2Cat step compares against UTC (still 10/11) -> Inf2CatUseLocalTime=true in the vcxproj
  (build-package.ps1 already passes /uselocaltime to its own Inf2Cat).
- Build 3 (build-package.ps1 -Out C:\Users\ilya\code\s10e-artifacts\wifi-m1\Exynos9820Wlan): exit 0,
  /W4 /WX clean, KMDF 1.15, PE machine 0xAA64, infverif /w /v = VALID, signtool verify /pa /c: .sys
  "signed in catalog", signer WDKTestCert ilya,134359747320405644 (host root-trust failure expected, as
  for the touch packages). SHA256: Exynos9820Wlan.sys E1BE2850586F8A6F..., .inf 8A72830486C188EE...,
  exynos9820wlan.cat BF7B22BF42FD8DF4...
- s10e-artifacts\wifi-m1\ also holds wdktestcert.cer (copy of touch-t2's), extra-wlan.cmd and the FD of
  this build (BEYOND0LTE_UEFI-m12m1.fd, sha256 4b8abc3f...0d815) - the shared edk2 Build dir may be
  overwritten by the device session.
- drivers/S10eDiag/esp/extra-wlan.cmd: peeks 0x8FE0F00C, installs the package with pnputil (TMP =
  C:\S10eTmp), collects pnputil /enum-devices ACPI\SAMS9825, sc query, the Parameters key, the report
  file and setupapi.dev.log into out\last\wlan\.

## Review of commit 07f4bee (4/5)

- Checked: ReportLine's Used arithmetic (Used + (Left - Remaining) == sizeof - Remaining), every
  format specifier against its argument type, _CRS contract check vs Dsdt.asl values, BAR0 window
  bounds check, Parameters key exists before WdfDriverOpenParametersRegistryKey (INF AddReg
  FLG_ADDREG_KEYONLY), package layout extra-wlan.cmd expects == build-package.ps1 output.
- FOUND: the whole report went to ONE DbgPrintEx - DbgPrint truncates at 512 bytes and the report is
  ~600-900 bytes, so the debugger copy would lose the BAR/chipid tail. FIXED in the follow-up commit:
  one DbgPrintEx per report line. Rebuilt: exit 0, infverif VALID, signed in catalog; new SHA256
  Exynos9820Wlan.sys 54BFB49F21FE8475..., .inf 0D7F558132739B58..., .cat 3D5DE9A1655A66A3...
  (these supersede the hashes above; s10e-artifacts\wifi-m1\Exynos9820Wlan holds this build).
- Accepted: Result-1 (_CRS mismatch) is the only path that fails the start (Code 10); all chip-side
  failures still start the device so the report is written - by design.

## Firmware blobs for milestone 2 (FACT from the stock image, exact device copy = needs-phone)

Read offline (no phone): vendor.img.lz4 from
`C:\Users\ilya\code\Dumps\AP_G970FXXSGHWC1_..._meta_OS12.tar` (Android 12, G970FXXSGHWC1),
lz4 -> sparse -> raw, ext4 walked with a throw-away reader (scratchpad, not committed). WLAN files:

| path on `vendor` (sda26) | bytes | what |
|---|---|---|
| `/vendor/firmware/bcmdhd_sta.bin_b1` | 1 297 036 | STA firmware for chip rev **B1** (the one milestone 2 downloads) |
| `/vendor/firmware/bcmdhd_mon.bin_b1` | 1 296 195 | monitor-mode firmware |
| `/vendor/firmware/bcmdhd_mfg.bin_b1` | 1 124 726 | factory-test firmware (not needed) |
| `/vendor/firmware/bcmdhd_clm.blob` | 29 232 | CLM (regulatory/country) blob |
| `/vendor/firmware/nvram.txt_1rh_es43_b1` | 12 811 | nvram, B1, "1rh" module (*inference*: Murata - the BT .hcd files name only murata/semco; UNVERIFIED) |
| `/vendor/firmware/nvram.txt_CS01_semco_b1` | 12 247 | nvram, B1, SEMCO module |
| `/vendor/firmware/nvram.txt_{1rh_es11,1rh_es12,1rh_es13}_b0`, `nvram.txt_ES0{0,1,2,3}_semco_b0` | ~10-12 K | B0 (engineering) variants |
| `/vendor/firmware/bcm4375B{0,1}_{murata,semco,semco_sem}.hcd` | 50-177 K | Bluetooth patchram (BT, not WiFi) |
| `/vendor/etc/wifi/indoorchannel.info` | 14 979 | Samsung indoor-channel table (userspace) |

- Only `_b1` STA firmware ships -> (inference) production units are rev B1; the chipid report (`rev` field,
  bits 19:16) confirms it on the device. The defconfig names (`/etc/wifi/bcmdhd_sta.bin`,
  `nvram_net.txt`) are NOT the on-disk names; (inference) bcmdhd appends the rev/module suffixes at runtime.
- WHICH nvram (Murata `1rh_es43` vs SEMCO `CS01`) is per unit. On Android the module type comes from
  `.cid.info` under /data (path from memory of bcmdhd, UNVERIFIED) - but userdata on this phone is now
  the Windows NTFS volume, so that file is gone. UNVERIFIED for this unit; closes by (a) the chip's OTP/CIS
  read in milestone 2 (bcmdhd's module-CID check reads it from OTP), or (b) the phone's vendor copy plus
  any surviving `.cid.info` (not expected).
- Device session pull (TWRP, read-only mount; nothing destructive):
  ```
  adb shell 'mkdir -p /tmp/v; mount -o ro /dev/block/by-name/vendor /tmp/v'
  adb shell 'ls -la /tmp/v/firmware | grep -iE "bcmdhd|nvram|clm|4375"; sha256sum /tmp/v/firmware/bcmdhd_* /tmp/v/firmware/nvram.txt_*'
  for f in bcmdhd_sta.bin_b1 bcmdhd_clm.blob nvram.txt_1rh_es43_b1 nvram.txt_CS01_semco_b1; do
    adb pull /tmp/v/firmware/$f C:\Users\ilya\code\s10e-artifacts\wifi-m2\$f; done
  adb shell 'umount /tmp/v'
  ```
  (the phone's vendor build may differ from GHWC1 - the `ls` line is the authority). Never commit them.

## Device test procedure (milestone 1) - for the device session

Artifacts: `C:\Users\ilya\code\s10e-artifacts\wifi-m1\` = `BEYOND0LTE_UEFI-m12m1.fd` (this build,
sha256 4b8abc3f...0d815), `Exynos9820Wlan\` (sys/inf/cat), `wdktestcert.cer`, `extra-wlan.cmd`.

1. Image: pack the FD with the usual flow (`firmware/tools/pack-uefi-beyond0lte.ps1`, same reference
   image + shim as the last good boot) or rebuild from this branch (same content; FD hashes drift by
   a few bytes across rebuilds, notes/m8-build.md, so compare module lists, not hashes;
   BEYOND0LTE_PCIE_WLAN 1 is the default here). The image also carries main's current USB host/KD
   state (302c285) unchanged. Flash `boot` only, as always.
2. ESP (by-name/cache, vfat) from TWRP: copy `Exynos9820Wlan\` to `\S10EDIAG\drivers\Exynos9820Wlan\`,
   `wdktestcert.cer` to `\S10EDIAG\drivers\` (already there from the touch runs), and `extra-wlan.cmd`
   as `\S10EDIAG\extra.cmd` (or `call` it from the existing extra.cmd).
3. Boot. UEFILOG.TXT lines to look for, in order:
   - `[PCIe] WLAN bring-up: reading CMU_FSYS1 PCIe PHY_REFCLK gate` - module reached, guard armed.
     **If the log/screen stops here or right after: the gate read hung** (open item P4). Next boot must
     print `[PCIe] WLAN: previous boot hung in PCIe bring-up (progress 0x68) - skipping it` and reach
     Windows; `debug_history` per AGENTS §9 shows the watchdog reset.
   - `[PCIe] CMU_FSYS1+0x2000 = 0x... (before)` / `[PCIe] clocks on: PHY_REFCLK gate 0x..., QCH_PCIE_PHY 0x...`
     (bit 21|20 set after = manual clock on took).
   - `[PCIe] WLAN_EN high + 100 ms; ELBI RDLH 0x...` - first ELBI read survived.
   - `[PCIe] link try 1/3` ... either `[PCIe] link up on try N after P polls, RDLH=0x.., speed GEN1`
     or `[PCIe] link try N failed, RDLH=0x.. PM_DSTATE=0x..` x3 + `[PCIe] R1 FAIL: link never came up`.
     (RDLH low 5 bits = LTSSM state; 0x0D..0x14 = up per the kernel. *Inference*, assuming the
     DesignWare LTSSM encoding: stuck at 0x00-0x01 = no receiver detected -> WLAN_EN/power or refclk;
     0x02-0x0C = training fails -> PHY tables/OTP.)
   - `[PCIe] R1: endpoint 14e4:XXXX (expected 14e4:4475, UNVERIFIED until now)` -> closes P2: record
     XXXX, then BCM4375_EXPECTED_PCI_ID_UNVERIFIED becomes VERIFIED (Exynos9820.h + Exynos9820Wlan.h).
   - `[PCIe] EP BAR0: 64-bit size 0x... -> 0x14000000`, `EP BAR2: ...` (sizes = new facts for M2).
   - `[PCIe] R1: chipcommon chipid 0x...: id 0x4375 rev 1` (expected).
   - `[PCIe] WLAN: bring-up Success, ACPI WLAN visible to Windows`.
4. Windows (S10eAgent run with extra-wlan.cmd), then back in TWRP read the ESP `S10EDIAG\out\last\wlan\`:
   - `wlan-status-word.txt` = 0x4E414C57;
   - `dev-wlan.txt`: `ACPI\SAMS9825\0`, Status Started, driver oem*.inf Exynos9820Wlan;
   - `Exynos9820Wlan.txt` (also `C:\Windows\System32\LogFiles\Exynos9820Wlan.txt` on the NTFS volume):
     `RESULT 0: BCM4375 alive` + VID/DID/BARs/chipid; `params.txt` the same as registry values.
   - Result codes: 1 _CRS mismatch (Code 10), 2 map failed, 3 config dead, 4 not Broadcom, 5 BAR0
     unusable, 6 chipid mismatch.
5. Regression checks on the same boot: touch still works, USB/XHC0 state as before, boot time
   (+~0.1-1.3 s for WLAN), no new bugcheck. If the link fails, Windows must simply not show SAMS9825.
6. To disable without a rebuild of anything else: `BEYOND0LTE_PCIE_WLAN 0` in Beyond0LtePcieDxe.c.

## Review of commit 0691e3c (5/5) and final state

- Docs only. Checked: every log line quoted in the procedure exists verbatim in Beyond0LtePcieDxe.c /
  PcieLink.c (grep "[PCIe]"); %r prints "Success" for EFI_SUCCESS; ESP = by-name/cache vfat, Windows =
  by-name/userdata NTFS (firmware/tools/s10e-cycle.sh); unverified statements in the blob section are
  marked inference/UNVERIFIED. Firmware at HEAD = the tree that was built (no firmware file changed after
  the build; commits 1/5 and 2/5 were not built individually - only the combined state).
- Not done here (by instruction): no adb, no pack, no flash, no push.
- 2026-10-12: boot #88 FLASHED (readback 7e7e1219…); ESP staged extra-wlan.cmd as extra.cmd + drivers\Exynos9820Wlan package + wdktestcert.cer; rebooted to Windows.
- RESULT (boot #88): UEFI "[PCIe] link up on try 1 ... GEN1", endpoint 14E4:4475, BAR0 64-bit 0x10000 @0x14000000,
  BAR2 64-bit 0x400000 @0x14400000, chipid 0x10054375 (id 0x4375 rev 5); Windows probe (ACPI\SAMS9825, mem0
  0x14FFE000+0x2000, mem1 0x14000000+0xFF0000): "RESULT 0: BCM4375 alive". Milestone 1 DEVICE-VALIDATED.
