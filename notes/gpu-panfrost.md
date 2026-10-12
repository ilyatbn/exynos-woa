# M16 — GPU acceleration via Panfrost (Mali-G76, Bifrost)

## Inputs (2026-10-11)
- User find: https://gitlab.freedesktop.org/panfrost — Panfrost project: Mesa's open-source Mali driver,
  the Linux `drm/panfrost` kernel driver, reverse-engineered documentation. User: "you can 100% do it.
  add a new milestone for this."
- User relay: ntdev (original exynos-woa author) posted GPU-accelerated DWM + desktop transparency on Exynos,
  unreleased. Unverified (no link yet). If a link appears, find out: display-only miniport vs full render
  driver, and whether he ported Panfrost or kbase.
- Already known (drivers branch, notes/d5-gpu-display.md): 9820 GPU = `mali@18500000`, ARM's open Bifrost
  kbase `b_r10p0` in the LineageOS 9820 kernel, max 702 MHz. No Mali WDDM has ever shipped.

## To verify, not assume
- Panfrost support level for Mali-G76 (Bifrost v7): Mesa gallium driver and kernel driver status.
- Whether Mesa's Windows D3D user-mode frontend can host the Panfrost gallium driver, and whether it builds
  for Windows ARM64.
- G3D power domain / clock / regulator control on 9820 (likely ACPM-owned, like the CPU/INT DVFS in M15).

## Progress log
- 2026-10-11: milestone M16 opened in MILESTONES.md with the task ladder; nothing researched yet. USB (M10)
  stays the active task; M16 starts after it.
- 2026-10-12: repo inventory done -> `notes/gpu-panfrost-repos.md` (22 group projects cloned to
  `C:\Users\ilya\code\mali`, register map / power / MMU / job formats with file:line citations, reuse + risks).
  Key corrections: G76 = "Norr"/tNOx (Gondul is G52); Bifrost JM needs no GPU firmware; Mesa has no D3D UMD
  frontend today (WGL/GL only); G76 is untested upstream (no SoC in mainline DT, no Mesa CI job).
