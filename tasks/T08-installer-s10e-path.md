# T08 — Make the installer app actually able to drive the S10e

Status: **open**

## Goal

The user's next session is "build and run the software so I can try it on the
S10e directly". The app recognises the phone but deliberately refuses to run the
stages on it, so "run it" currently means "it says no". This task is the honest
gate: decide what the app must do, and change only that.

## Where it stands (committed)

- `GalaxyS10e` is in the catalog: `("SM-G970F", "G970F", "beyond0lte",
  "exynos9820", "G970FXXSGHWC1", Validated: false)` (`notes/m9-installer.md:9`).
- `PerDevice` for `beyond0lte`: `image` → `Experimental`, everything else
  (`unlock`, `twrp`, `backup`, `partition`, `transfer`, `uefi`, `firstboot`) →
  `NotImplemented` (`notes/m9-installer.md:18`).
- `Runnable("transfer", "beyond0lte")` is **false** (true for `star2lte`), and the
  stages have **no `beyond0lte` implementation at all** — the gating only stops
  the app from claiming support (`notes/m9-installer.md:39`, `:44`).
- Tests: `dotnet build` clean, `dotnet test` 188 passed including
  `RecognisesTheS10eButKeepsItUnvalidated`.

## Next actions

1. Decide the smallest honest change: the **`uefi`** and **`firstboot`** stages
   are the ones the boot attempt actually needs; implement those two for
   `beyond0lte` as `Experimental` (never `Ready`), leaving the rest
   `NotImplemented`.
2. Keep the 9810 catalog byte-unchanged; the S10e entry stays `Validated: false`
   until a device validation exists.
3. Re-run `dotnet test` and add a test that names the new state explicitly
   (the repo's pattern: the test asserts the *unvalidated* label, not the
   feature).

## Done when

The app can be pointed at `SM-G970F` and asked to write the UEFI image, and the
stage it runs is labelled `Experimental` with the validation state still false.
