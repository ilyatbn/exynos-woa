# Map: release pipeline — tools/release/ + release/

This public tree is **generated**, not edited at the source of truth. Flow:

```
private research repo ──import_sources.py──► this tree ──scan.py──► CI gate
        (manifest: release/import-manifest.json)      (allow-list: release/scan-allow.txt)
```

## tools/release/

| File | Lines | What |
|------|-------|------|
| `import_sources.py` | 293 | copies only manifest paths into this tree. Transforms keep public edits reproducible: **`resolve_macros`** (unifdef-like: drops dead `#if` branches — scrubs diagnostic/hook code from firmware, forces portable-kernel contract in UFS driver) and **per-entry patches** (recorded with `--make-patches`, reapplied every import; overwrite guard refuses hand-edited files without a recorded patch; `--force` discards) |
| `scan.py` | 202 | **leak gate**: rejects vendor/kernel internals, build hashes, workspace paths, personal emails, stray binaries |
| `make-payload.ps1` | 132 | assembles release assets: `firmware.json` + UEFI images (each vs catalog hash), `drivers.zip`, extras, `SHA256SUMS`; refuses a recovery whose build stamp isn't "open fonts, built-in gears" |
| `make-release-extras.ps1` | 121 | installer zips (self-contained publish + bundled runtime licenses) + prebuilt recovery (official TWRP re-skinned with open fonts + built-in gears) |
| `tests/test_import.py` / `test_scan.py` | — | pytest suite (CI runs it) |

## release/ (pipeline inputs, committed)

| File | What |
|------|------|
| `import-manifest.json` | 10 entries: `firmware-platform`, `firmware-silicon`, `firmware-bootshim`, `firmware-build`, `firmware-pack` ← `isolated/homegrown-ext209-source/…`; `driver-ufs-src`, `driver-ufs-project`, `driver-ufs-build`, `driver-ufs-include` ← `eight-core/harness/…`; `driver-touch` ← `windows-drivers/S6SY761Touch`. `global_exclude` drops logs/receipts/provenance. **Not imported** (maintained in this repo): installer, `tools/twrp-winre`, `tools/release`, `drivers/Exynos9810Hsi2c` |
| `provenance.json` | `files`, `source_commit`, `source_dirty` — what was imported and from which commit |
| `scan-allow.txt` | intentional exceptions: scanner's own tests, GPL author emails, WinPE `X:` path, app icon, `secret-assign` tokens (unattend publicKeyToken, supervised-clear token), `ms-internal` Patreon/Ko-fi handle, all art/module/font binaries |

## Private, NOT in git (back these up alongside the research repo)

- `.release-patches/` — per-entry patch files (their *removed* lines hold scrubbed private text)
- `.release-denylist.txt` — additional local scanner patterns

Lose these and you can still ship, but the automated reproducible scrub is gone.

## Maintainer loop

```powershell
python tools\release\import_sources.py --source <research-repo> --make-patches
python tools\release\import_sources.py --source <research-repo>   # verify reapply
python tools\release\scan.py                                      # must be clean
python -m pytest -q tools\release\tests
```

## Release assets (on GitHub Releases, not in git)

| Asset | What |
|-------|------|
| `S9WoaInstaller-<ver>-x64/arm64.zip` | the installer, self-contained |
| `firmware.json` + `star2lte-uefi-<build>.img` | UEFI per supported Windows build (22621/22631.2428, 22621.7582/22631.7584) |
| `drivers.zip` | UFS + HSI2C + touch packages (test-signed) |
| `star2lte-winre-recovery.img` | prebuilt recovery (open fonts, built-in gears) |

Never attach Windows media, a built image, or a PC-built recovery (carries Segoe UI / maybe Microsoft's GIF).
