# M5/M8 — re-aim the UFS write fence at the S10e's own partition table

Task: `docs/exynos9820-port/MILESTONES.md:160` "Re-aim the write fence at the
S10e's partition list" (was `todo`). The numbers were already read
(`notes/s10e-root-reads.md` §2/§8/§9); what remained was the ~200-line
interlocking block in `drivers/Exynos9820Ufs/Exynos9820Ufs.h`.

## Progress log

### 1. Resume state (what the crash left behind)

* `git status` showed `drivers/Exynos9820Ufs/Exynos9820Ufs.h` modified and
  uncommitted: the fence half of the block had been rewritten, the rest had not.
  That half-edit is exactly what the `C_ASSERT`s exist to catch — with
  `UFS_WRITE_FENCE_LAST_LBA = 31236090` against the still-9810
  `UFS_GPT_BACKUP_FIRST_LBA = 15615995`, the assert at `Exynos9820Ufs.h:1157`
  fails. Nothing was committed for it, so the block was redone in one step.
* Newest commit at resume: `9cb991d` (notes only, the retarget table).
* `info/DEN0101_00bet1_ADAC.pdf` is untracked and unrelated (its detour was
  already closed in `5195f0e` / `notes/info-den0101-adac.md`).

### 2. The numbers, all read rather than derived

From the live GPT captured in TWRP (`notes/s10e-root-reads.md` §2, §8) and the
tail measurement (§9), in the driver's own 4096-byte LBAs:

| constant | 9810 | S10e | source |
|---|---|---|---|
| `UFS_WRITE_FENCE_FIRST_LBA` | 68 736 (sda18 SYSTEM) | **1 983 488** (sda28 CACHE = the ESP) | live GPT + PIT |
| `UFS_WRITE_FENCE_LAST_LBA` | 15 615 994 | **31 236 090** (this disk's LastUsableLBA, header +0x30) | header field |
| `UFS_PROTECTED_PARTITION_FIRST_LBA` | 1 195 136 (sda19 VENDOR) | **2 137 088** (sda29 OMR) | live GPT |
| `UFS_PROTECTED_PARTITION_LAST_LBA` | 1 526 911 (sda20 ODM end) | **2 149 887** (sda29 OMR end) | live GPT |
| `UFS_PROTECTED_PARTITION2_FIRST_LBA` | 1 680 512 (sda22 HIDDEN) | **2 149 888** (sda30 CP_DEBUG start) | live GPT |
| `UFS_PROTECTED_PARTITION2_LAST_LBA` | 1 697 151 (sda24 CP_DEBUG end) | **2 151 167** (sda30 CP_DEBUG end) | live GPT |
| `UFS_GPT_BACKUP_FIRST_LBA` | 15 615 995 | **31 236 091** (= AltLBA − 4) | header +0x20 |
| `UFS_EXPECTED_LAST_LOGICAL_BLOCK` | 15 615 999 | **31 236 095** (media 127 943 049 216 / 4096 − 1) | `blockdev --getsize64` + header |
| `UFS_READOUT_WINDOW/SCRATCH_*` | 15 614 336..15 614 591 | **31 234 816..31 235 071** (one LBA past USERDATA, 256 LBAs) | §9 read them back zero |
| non-full-windows protected run | 1 195 136..15 614 335 | **1 543 168..2 151 167** (VENDOR..CP_DEBUG) | live GPT |

Role change that forced comment rewrites, not renumbering: on the 9810 the
Windows volume was the SYSTEM slot (sda18) and USERDATA (sda25) was protected;
here USERDATA (sda31) **is** the Windows volume and the ESP is CACHE (sda28),
with only OMR and CP_DEBUG real between them.

### 3. What the block now sanctions

Writable = sda28 (ESP) ∪ sda31 (Windows volume) ∪ the verified-zero tail
31 234 816..31 236 090. Refused = OMR, CP_DEBUG (the two protected ranges),
everything below the fence (the whole Android run, EFS, BOOT, RECOVERY), LBA
0..5, and the five LBAs the header reserves for the backup GPT — which the
media does not actually carry (§9 read them back zero), but they are refused
regardless so a reflashed mirror stays protected.

### 4. Build verification (fact)

```
powershell -NoProfile -ExecutionPolicy Unrestricted -File drivers\Exynos9820Ufs\build-ufs.ps1 `
  -SourceDir drivers\Exynos9820Ufs -OutDir %TEMP%\opencode\ufs-9820-fence-1 -WideUncached -DmaWindow
```

* `EXACT_BUILD_OK`, `BUILD_OK`. `Exynos9820Ufs.sys` **49 152 B** sha256
  `3a19a6bb5b88cac04aaccbe292fb998c60bad9be0a6c783dbb1fafd22caf0d0b`;
  `UfsDiag.exe` **213 504 B** sha256 `5dfb2a7abfdab210026f8317c88534bb235fc951f61893a141cf4851458f0b9b`.
  ABI unchanged: `{"signature":960910165,"version":1376256,"bytes":992,...}`.
* Byte size identical to the pre-edit build (49 152 B) — the fence constants are
  immediates, so a size change is not expected; the FD/driver is not
  byte-deterministic anyway (`notes/m8-build.md` §8), so compare sizes/ABI only.
* **Negative test run**: setting `UFS_WRITE_FENCE_LAST_LBA` to 31 236 091 makes
  the build fail with `Exynos9820Ufs.h(1206,1): error C2118: negative subscript`
  — i.e. the `C_ASSERT` set really does gate these constants, and a half-edit
  cannot slip through. Reverted immediately after.
* Log: `%TEMP%\m8-ufs-fence-build-1.log`.

### 5. Flagged by the review — follow-ups

* `UfsDiag.c:1621` `UFS_SCRATCH_LBA` is still **15 614 336**, a 9810 LBA. On the
  S10e that lands **inside USERDATA** (2 151 168..31 234 815), so the diag tool's
  write ladder (`--ufs-write-live`) would stamp the middle of the Windows volume.
  Its own `C_ASSERT`s (`UfsDiag.c:1656-1657`) are self-consistent with the 9810
  literals so they still compile — the hazard is invisible to the build.
  **Follow-up task, separate commit.**
* `UfsDiag.c:1491-1620` narrative still speaks of sda18/sda25 and the archived
  `backups\TAIL-lba15614336-...bin`; the S10e tail measurement is §9's.

## Sources

* `notes/s10e-root-reads.md` §2 (live GPT), §8 (header geometry fields),
  §9 (tail measurement) — all read on `RF8M21HYDSZ` in TWRP as root.
* `notes/pit-geometry.md` — the PIT table the GPT equals.
* `notes/windows-m7-toolchain.md` §4.5 — the working `build-ufs.ps1` command line.
