# M9 — installer: the S10e behind the same honest gating (2026-10-09)

Code change plus the verification that it did not break the validated 9810 flow.

## 1. What was changed (facts)

* `installer/src/S9Woa.Installer.Core/Device/DeviceEligibility.cs`
  - `SupportedTarget` gains `bool Validated = true` (default keeps the 9810 line unchanged).
  - `GalaxyS10e` added: `("SM-G970F", "G970F", "beyond0lte", "exynos9820", "G970FXXSGHWC1", Validated: false)`.
    The bootloader string is a **read**: `getprop ro.boot.bootloader` on `RF8M21HYDSZ` returned
    `G970FXXSGHWC1` (2026-10-09), and `ro.product.model` returned `SM-G970F`.
  - `All` is now `[GalaxyS9Plus, GalaxyS10e]`.
  - The model check emits `Warning` (not `Pass`) for an unvalidated target, with the text
    "ported, not validated end to end" and the instruction that the installer will stop
    before any stage that writes to the phone.
  - The chipset check no longer hardcodes "Exynos 9810"; it reports `target.HardwareToken`.
* `installer/src/S9Woa.Installer.Core/Stages/StageCatalog.cs`
  - `PerDevice` map: for `beyond0lte`, `image` -> `Experimental`, and
    `unlock`/`twrp`/`backup`/`partition`/`transfer`/`uefi`/`firstboot` -> `NotImplemented`.
    `host`/`identify`/`media` keep their catalog `Ready` (they are PC-side and device-independent).
  - `Resolve(id, codename)` and `Runnable(id, codename)` added; the catalog's own list is
    untouched, so the 9810 flow keeps exactly the states it had.

Inference (mine): `image` is `Experimental` rather than `Ready` because it is PC-side but
carries the three 9820 `.sys` files, which are built and test-signed yet have never run on
hardware. That is the enum's own definition ("automated and unit-tested, not yet validated
end to end on the reference device").

## 2. Verification (facts)

```
dotnet build S9Woa.Installer.sln -c Debug -p:Platform=x64   -> Build succeeded, 0 warnings, 0 errors
dotnet test tests\S9Woa.Installer.Core.Tests\...csproj -p:Platform=x64
    -> Passed! Failed: 0, Passed: 188 (was 187 before this change)
```

New test `DeviceTests.cs :: RecognisesTheS10eButKeepsItUnvalidated` asserts: no blocker for
the S10e, `model` is a Warning, `bootloader` passes against the read bootloader string,
`Runnable("transfer", "beyond0lte")` is false while it stays true for `star2lte`, and
`uefi`/`image`/`identify` resolve to NotImplemented/Experimental/Ready.

## 3. What is NOT done (fact)

The stages themselves have no `beyond0lte` implementation — the gating only stops the
installer before them. Nothing about the S10e has been observed end to end, so no stage
for it is `Ready`, and the app's UI still hardcodes the S9+ title
(`S9Woa.Installer.App/MainWindow.xaml.cs:199` keys on "G965"); that display string was
left alone in this task.

## 4. Sources

* live `adb` reads on `RF8M21HYDSZ` (SM-G970F): `getprop ro.boot.bootloader`, `ro.product.model`
* local: `installer/src/...`, `installer/tests/...`, build/test logs above
