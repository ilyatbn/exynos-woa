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

Harsh review of `7d17971`, each item verified mechanically rather than by re-reading:

* **`UFS_FULL_WINDOWS_MODE 0` does not compile.** Flipping the switch and
  rebuilding gives `Exynos9820Ufs.h(1219,1): error C2118: negative subscript` at
  `C_ASSERT(UFS_PROTECTED_PARTITION_FIRST_LBA > UFS_WRITE_FENCE_FIRST_LBA)` —
  the non-full-windows run starts at VENDOR (1 543 168), which is BELOW the
  fence's lower bound (1 983 488), so "the protected block sits strictly inside
  the fence" is false by construction here, unlike the 9810 where it was true.
  The assert must be branch-specific. **Fixed in its own commit.**
* **Comment defects**: the "Turning it off…" paragraph lost its ` * ` alignment
  (it now reads `  * `); the new `C_ASSERT(... > 41343ULL)` is commented as
  "must not reach EFS (sda3 starts at 4096) or BOOT" but 41343 is BOOT's **end**
  and nothing tests EFS; the sda26..sda31 table is now printed twice (narrative
  comment + the comment next to the constants); `UFS_GPT_PRIMARY_LAST_LBA` lost
  its provenance marker. **Fixed in its own commit.**
* `Exynos9820Ufs.c:6927` still says "Require the known 4096-byte block size and
  last LBA **15615999**" — a 9810 number presented as the current requirement,
  now contradicted by `UFS_EXPECTED_LAST_LOGICAL_BLOCK = 31236095`. Only stale
  LBA mention left in the .c (`rg` over the file). **Fixed in its own commit.**
* **`UfsDiag.c:1621` `UFS_SCRATCH_LBA` is still 15 614 336**, a 9810 LBA that on
  THIS disk falls inside USERDATA (2 151 168..31 234 815) — the diag write
  ladder would stamp the middle of the Windows volume. Its own `C_ASSERT`s
  (`UfsDiag.c:1656-1657`) are self-consistent with 9810 literals, so the build
  cannot see it. **Separate task, separate commit.**
* `UfsDiag.c:1491-1620` narrative still speaks of sda18/sda25 and the archived
  `backups\TAIL-lba15614336-...bin`; the S10e tail measurement is §9's.

## 6. Fix commits (one per finding)

### A. `UFS_FULL_WINDOWS_MODE 0` did not compile → branch-specific asserts

* Before: `C_ASSERT(UFS_PROTECTED_PARTITION_FIRST_LBA > UFS_WRITE_FENCE_FIRST_LBA)`
  unconditional → `Exynos9820Ufs.h(1219,1): error C2118` with the switch at 0.
* After: the fence-consistency asserts are split, `> FENCE_FIRST` in full-windows
  and `< FENCE_FIRST` in the refusing build, with the reason stated in the comment.
* Verified by building **both** modes: `-WideUncached -DmaWindow`, outdirs
  `ufs-9820-fence-2` (mode 1, `EXACT_BUILD_OK`, 49 152 B, sha256
  `3a19a6bb…caf0d0b` — identical to the pre-fix binary, as expected since the
  asserts emit no code) and `ufs-9820-fence-nw2` (mode 0, `EXACT_BUILD_OK`,
  49 664 B). The switch was left at 1.

### B. Comment/assert defects from the same review

* The "Turning it off…" paragraph lost its ` * ` alignment → restored.
* The HISTORY bracket now says "every paragraph below, up to the last one, is
  the 9810's", because the last paragraph speaks of this device's fence.
* The sda26..sda31 table was printed twice (narrative comment + the comment next
  to the constants) → the narrative one now points at the table below and keeps
  only the contiguity fact.
* `C_ASSERT(UFS_WRITE_FENCE_FIRST_LBA > 41343ULL)` was commented as "must not
  reach EFS (sda3 starts at 4096) or BOOT" — 41343 is BOOT's **end** and nothing
  tested EFS → now three separate asserts, one fact each: BOOT 41343, RECOVERY
  57855, EFS 9215.
* `UFS_GPT_PRIMARY_LAST_LBA` lost its provenance → marked VERIFIED with the
  read that gives it (128 entries × 128 B at LBA 2..5).
* Rebuild after B: `EXACT_BUILD_OK`, 49 152 B, sha256 `3a19a6bb…caf0d0b` —
  byte-identical to the pre-fix binary, which is what a comment/assert-only
  change should produce.

### C. The .c side of the same guard still narrated the 9810's layout

`rg "sda1[0-9]|sda2[0-9]|sda3[0-9]"` over `Exynos9820Ufs.c` found four places
where the comment now contradicts the constants:

* `Exynos9820Ufs.c:5538` "the fence spans from sda18 … sda19 VENDOR through
  sda25 USERDATA is carved back out" → rewritten to sda28 ESP / sda29+sda30
  refused / sda31 Windows volume. The sentence "a write that clips one block of
  USERDATA" was wrong twice over (USERDATA is writable here) → CP_DEBUG.
* `Exynos9820Ufs.c:5554` "sda21 the ESP sits between the two groups" → the split
  is kept for the assert-halves, not because the ESP is between them.
* `Exynos9820Ufs.c:6927` "last LBA 15615999" → names
  `UFS_EXPECTED_LAST_LOGICAL_BLOCK` and states 31 236 095.
* `Exynos9820Ufs.c:7491-7517` (inside `#if UFS_READOUT_BOOT`, compiled out) →
  marked HISTORY rather than rewritten, since the numbers in it are the record
  of the 9810's readout boot.
* Rebuild after C: `EXACT_BUILD_OK`, 49 152 B, sha256 `3a19a6bb…caf0d0b` —
  comments only, byte-identical as expected.

## Sources

* `notes/s10e-root-reads.md` §2 (live GPT), §8 (header geometry fields),
  §9 (tail measurement) — all read on `RF8M21HYDSZ` in TWRP as root.
* `notes/pit-geometry.md` — the PIT table the GPT equals.
* `notes/windows-m7-toolchain.md` §4.5 — the working `build-ufs.ps1` command line.
