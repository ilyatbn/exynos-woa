# Trying the install run — what the app can and cannot do for the S10e today

Started 2026-10-09 after the fence work (`44aac64` was the last commit).

## 1. Build + suite (facts)

```
dotnet build installer\S9Woa.Installer.sln -c Debug -p:Platform=x64
  -> Build succeeded, 0 warnings, 0 errors (Core.dll, Tests.dll, App dll)
dotnet test installer\tests\S9Woa.Installer.Core.Tests\S9Woa.Installer.Core.Tests.csproj -p:Platform=x64
  -> Passed! Failed: 0, Passed: 189, Skipped: 0
```

Runnable app: `installer\src\S9Woa.Installer.App\bin\x64\Debug\net8.0-windows10.0.26100.0\win-x64\S9WoaInstaller.exe`
(Debug build, unsigned — not the release artefact).

## 2. What stops the install flow (facts, read from the source)

`installer/src/S9Woa.Installer.Core/Stages/StageCatalog.cs:66-76` — for
`beyond0lte` every phone stage is `NotImplemented`:

| stage | beyond0lte |
|---|---|
| image | **Experimental** |
| unlock / twrp / backup / partition / transfer / uefi / firstboot | **NotImplemented** |

`Runnable(id, "beyond0lte")` is false for all of the phone stages
(`StageCatalog.cs:94-95`), so the app shows them as "coming soon" and refuses to
start them. The gating was verified by the test
`DeviceTests.cs :: RecognisesTheS10eButKeepsItUnvalidated`
(`notes/m9-installer.md` §2).

So running the app now cannot drive an S10e install: only the PC-side image
stage is reachable.

## 2b. CORRECTION — the per-device gating is DEAD in the app (fact)

The paragraph above says the app refuses the phone stages for `beyond0lte`. It
does not. Verified mechanically:

* `StageCatalog.Resolve(id, codename)` and `Runnable(id, codename)`
  (`StageCatalog.cs:79-95`) are called from **`installer\tests\...\DeviceTests.cs`
  only** — `rg "Resolve\(|Runnable\("` over `installer/src` finds no app call site.
* The app tests the **catalog-declared** value instead:
  `InstallPage.xaml.cs:138` `var avail = item.Definition.Availability;`
  `:139` `if (avail == StageAvailability.NotImplemented)`, and
  `InstallPage.xaml.cs:288` the same in the stage dispatcher's default arm.
  `StageItem.cs:24` renders `NotImplemented` as "Coming soon" from the same
  declared value.

Consequence, and it is the opposite of safe: for an S10e the app shows and runs
the **9810** stage code (`twrp`, `partition`, `transfer`, `uefi` are `Ready` in
the catalog). The only thing that stops it is incidental — the toolset's
star2lte SHA-256 gate and the missing payload file — not the stage gating.
Per-device geometry is wired in only four places (`InstallPage.xaml.cs:581,647,648,657`
call `PartitionMap.For(...)`); `VhdxImageBuilder.cs:36/48` still defaults to
`Star2Lte`.

**This is the first thing to fix**, before any install attempt: make the app
consult `Resolve(id, codename)`.

## 3. The star2lte text the guided flow would show (facts)

* `ToolsetManager.cs:84` `TwrpPayload` = `twrp-star2lte.img`;
  `:89` `TwrpWinrePayload` = `twrp-winre-star2lte.img`.
* `ToolsetManager.cs:231,244,306,345` messages tell the user to download
  `twrp-3.7.0_9-0-star2lte.img` — the wrong recovery for SM-G970F.
* `ToolDefinitions.cs:88-90` the tool's display name/description say star2lte.
* `WinReTwrpBuilder.cs:63,195,196` and `PowerOffRoutePatch.cs:55` gate on the
  **star2lte TWRP kernel's SHA-256** — an S10e TWRP is refused outright, not
  just mislabelled.
* `S9Woa.Installer.App/MainWindow.xaml.cs:199` the UI title keys on "G965".

## 4. The artefact that must exist before any flash (fact)

`firmware/tools/pack-uefi.ps1` packages `STAR2LTE_UEFI.fd` into a flashable
Android boot image: shim + FD + wrapped in a **star2lte** reference TWRP boot
image, with the shim copying the FD to `0x90000000`. Nothing equivalent exists
for `BEYOND0LTE_UEFI.fd` (built this session, 7 340 032 B): the reference image
and the copy-to base are device-specific, and the 9820's DRAM layout differs
(`notes/vendor-dt-reserved.md`, `notes/s10e-root-reads.md` §4).

## 5. Order I propose (pending the user's choice)

1. Package the S10e UEFI boot image (the thing that has to exist before any
   flash) — needs the 9820 shim base + an S10e reference boot image.
2. Retarget the installer's toolset names/messages/SHA gates to beyond0lte.
3. Implement the beyond0lte stage paths as `Guided` (never `Ready`) so the app
   can walk the phone steps.
4. Only then run the app end to end, and flash once with an agreed
   first-boot observation plan (ground rules 5/6 forbid destructive steps
   before Windows booting is established).

## 6. Plan agreed with the user (2026-10-09)

The user wants the install attempt AND the app made generic (detect the device,
pick the right payloads/geometry/gates). Chunked, one commit per step:

1. **Wire the per-device gating into the app** (`Resolve(id, codename)`) — the
   safety fix; without it the app runs 9810 stage code against an S10e.
2. Device-generic toolset: payload names, user-facing messages, and the SHA-256
   /version gates keyed on the detected device.
3. Device-generic geometry: `PartitionMap.For(codename)` everywhere,
   `VhdxImageBuilder` no longer defaults to `Star2Lte`.
4. `beyond0lte` stage paths implemented as **Guided** (never `Ready`).
5. UI strings per device (title keyed on "G965", the "Galaxy S9+" experimental
   warning text, the "S9" username chain, the nine key-combo strings).
6. The S10e UEFI boot image artefact — **blocked**: `repack-with-ref.py`,
   `firmware/device-data/` and `tools/bootimg-pack.sh` do not exist in this
   checkout (`Test-Path` false for all three), so the star2lte pipeline is not
   reproducible here either. See `notes/m8-uefi-image.md`.

## 7. Device state

`adb devices` returned **empty** at 2026-10-09 after the fence work
(`adb.exe: no devices/emulators found`), although it showed
`RF8M21HYDSZ  recovery` earlier in the day. So the phone is unplugged or in a
mode adb does not see (Download mode is not an adb transport). Any device read
below is written as the command to run, not executed.

## 8. Step 1 done — per-device gating wired into the app

Changed (all in `installer/src/S9Woa.Installer.App/`):

* `StageItem.cs:20-35` — `StageItem` now carries `DeviceCodename` and renders its
  badge from `StageCatalog.Resolve(definition.Id, DeviceCodename)` instead of the
  catalog-declared value.
* `InstallPage.xaml.cs:25-34` — the items are built with
  `DeviceCodename = AppServices.CurrentDevice?.Codename`, and the "experimental
  opt-in" row is shown only when some stage is `Experimental` **for this phone**.
* `InstallPage.xaml.cs:144` — `RunAllAsync` gates on `Resolve(...)`.
* `InstallPage.xaml.cs:210-220` — a guard at the top of `RunStageAsync` itself, so
  **no** entry point can reach a stage that is not implemented for the connected
  phone (the run-all loop was not the only one: `:254` re-runs `identify`
  directly).
* `InstallPage.xaml.cs:294` — the dispatcher's default arm resolved too.

Verification: `dotnet build` 0 warnings / 0 errors. The Core suite is unchanged
(189) because this change is app-side and the app has no unit tests — recorded as
a gap, not as a passing check.

Follow-ups found while reviewing this change:

* `StageItem.DeviceCodename` is captured when the page is constructed, so the
  **badge** can go stale if the user swaps phones mid-session; the runtime guard
  reads `AppServices.CurrentDevice` live, so the gate cannot be bypassed — only
  the label can lie. Refresh the items on `CurrentDeviceChanged`.
* `PartitionMap.cs:86-91` — `For(null)` and `For("anything-unknown")` silently
  return the **star2lte** geometry. An unidentified phone is therefore treated as
  an S9+ by the partition stage. Must become an explicit refusal (step 3).

## 9. Step 2 done — geometry can no longer be guessed

* `PartitionMap.cs:86-91` — `For(codename)` now names **both** phones and throws
  `ArgumentOutOfRangeException` for anything else; `IsKnown(codename)` added so
  callers can refuse with a message instead of unwinding.
* `VhdxImageBuilder.cs:36,48` — the `Star2Lte` default is gone; a geometry must be
  supplied or the ctor throws.
* `InstallPage.xaml.cs:593` (image stage) and `:656` (partition stage) now check
  `IsKnown` and refuse with a message.
* Tests: `ImageBuilderTests.cs:127,190` had to name the phone explicitly (they
  relied on the silent default) — that is the change making the hidden assumption
  visible in the tests. New test `AnUnidentifiedPhoneGetsNoGeometryAtAll` asserts
  `IsKnown(null)`/`IsKnown("hax")` are false, `For(null)`/`For("star2lteX")` throw,
  and a `VhdxImageBuilder` without a geometry throws.

Verification: `dotnet build` 0/0; `dotnet test` **190 passed** (189 → +1).

Still open after this step (found while reviewing):
`PartitionMap.SecondaryEfiSystemPartition = "SYSTEM"` is used unconditionally by
`BootFilesService.cs:40`, `InstallPage.xaml.cs:660` and `:706`
(`GptTypeService.EnsureEspTypeAsync`). On the S9+ SYSTEM is a 55 MiB FAT copy; on
the S10e it is a **5.98 GiB ext4 Android system**, so that path would reformat
Android's system partition. It must become per-device (and absent for
beyond0lte) — step 3.

## 10. Step 3 done — the secondary ESP is per device, not a constant

The most dangerous thing the inventory found: `PartitionMap.SecondaryEfiSystemPartition`
was a **constant** `"SYSTEM"` used unconditionally by `BootFilesService.cs:40`
(reformat as FAT32), `InstallPage.xaml.cs:660` (required-partition list) and
`:706` (`GptTypeService.EnsureEspTypeAsync`). On the S9+ SYSTEM is a 55 MiB FAT
volume; on the S10e it is a **5.98 GiB ext4 Android system** — so the transfer
stage would have reformatted Android's system partition on an S10e.

* `GeometryType` gains `string? SecondaryEspPartition`; `Star2Lte` carries
  `"SYSTEM"`, `Beyond0Lte` carries **null** (this port does not reformat the
  S10e's SYSTEM; whether a second copy is needed there is unvalidated either way).
* `BootFilesService` now takes the geometry in its constructor and formats only
  the slots the phone actually has.
* The partition stage resolves the geometry **once** (it had called
  `PartitionMap.For(...)` three times) and adds the secondary slot to the
  required list only when it exists.
* The transfer stage refuses an unknown phone and only runs
  `EnsureEspTypeAsync` when there is a secondary slot.
* Tests: new `OnlyTheReferencePhoneHasASecondaryEsp` asserts the two values.

Verification: `dotnet build` 0 warnings / 0 errors; `dotnet test` **191 passed**
(189 → 190 → 191 across the last three steps).

Found while reviewing, still open:
* `GptTypeService.cs:45-49` requires a **backup GPT**, which this disk does not
  have (`notes/s10e-root-reads.md` §9 read the region back zero) — the S10e path
  would fail there.
* `Gpt.cs:16-17` reads the partition name at header+56; the S10e's live GPT keeps
  the name at entry+80 (`notes/s10e-root-reads.md` §2) — the parser needs the
  Samsung entry layout, not the spec layout.
* `PartitionMap.cs` still exposes `UserdataGuid`/`CacheGuid`/`SystemGuid` as
  star2lte constants; the per-device ones are inside `GeometryType`. Any consumer
  of the bare constants is device-wrong.

## 11. Step 4 done — payload identity follows the identified phone

* New `Toolset/TwrpProfiles.cs`: a per-phone record (official TWRP image name, the
  two payload stems, the UFS `.inf`, and whether the WinRE skin can be built at
  all). `beyond0lte` carries **null** for the official image name — it has not been
  read from the phone or the release page in this session, and naming a guessed
  file offers the user a download that will not boot this phone. `WinReSkinSupported`
  is false there because `PowerOffRoutePatch.cs` and `WinReTwrpBuilder.cs` gate on
  the **star2lte** kernel's SHA-256 and absolute offsets, nothing equivalent exists
  for this phone.
* `ToolsetConfig` gains `DeviceCodename` (schema 1 → 2) and `AppServices.CurrentDevice`'s
  setter writes it and saves, so the toolset knows which phone it is configured for.
  Null means "no phone", never "the reference phone".
* `ToolsetManager`: `TwrpPayload`/`TwrpWinrePayload` come from the profile (with no
  phone they resolve to `twrp-unidentified`, a name that matches nothing on disk);
  `UfsInf` is now `string?` from the profile; `DetectTwrp` and `BuildWinReRecovery`
  refuse with a message when there is no profile or the skin is unsupported.
* Tests: the `Manager(...)` helper now names the phone (`deviceCodename = "star2lte"`
  default) — six toolset tests were passing only because of the silent default.
  New test `PayloadsFollowTheIdentifiedPhoneAndNothingIsAssumed` asserts the beyond0lte
  stems and `.inf`, that an unidentified phone gets `twrp-unidentified` and a null
  `.inf`, and that TWRP is `Missing` in both non-reference cases.

Verification: `dotnet build` 0 warnings / 0 errors; `dotnet test` **192 passed**
(191 → +1, and the six previously-failing toolset tests now pass because the
helper states the phone).

Still open after this step (found while reviewing, not folded in):
* `ToolsetManager.cs:393` — the folder scan skips files whose **name contains
  "star2lte"** to avoid picking a UEFI image; the heuristic is star2lte-shaped.
* `BootImage.RecoveryPartitionBytes = 68_149_248` (`AndroidBootImage.cs:20`) is a
  star2lte constant used to validate the repacked recovery, and it **disagrees**
  with `PartitionMap.Star2Lte.RecoveryBytes = 68_148_992` (not even a whole number
  of 4096-byte LBAs) while `Validation.cs:9` says star2lte RECOVERY is 16,638 × 4096
  = 68,149,248. One of the two is wrong; it must come from the geometry, not a
  constant.
* `ToolDefinitions.cs:88-90` display name/description still say star2lte.

## 12. Step 5 — a wrong geometry constant, caught by arithmetic

`PartitionMap.Star2Lte.RecoveryBytes` was **68_148_992**, which is not a whole
number of 4096-byte logical blocks (16 637.9375 LBAs). A partition size cannot be
that, so it was a transcription slip, not a different partition. The star2lte
RECOVERY is 16,638 LBAs = **68_149_248**, which is the value `AndroidBootImage.cs:20`
already carries and the value `Validation.cs:9-10` documents.

Verified mechanically (`python`): 68_149_248 / 4096 = 16_638 exactly; 67_633_152 / 4096
= 16_512; 57_671_680 / 4096 = 14_080; 629_145_600 / 4096 = 153_600; 68_148_992 / 4096
= 16_637.9375.

Also corrected in `notes/installer-device-specifics.md`: that file claimed
`68_149_248 = 16_656 x 4096` and that neither value matched a PIT — the arithmetic
there was wrong, and the odd constant is the PartitionMap one, not the
`AndroidBootImage` one.

New test `GeometrySizesAreWholeLogicalBlocks` asserts every size in both geometries
is a whole number of LBAs and pins the two RECOVERY values; that is the check that
would have caught the slip without reading a PIT again.

Verification: `dotnet build` 0/0; `dotnet test` **193 passed**.

## Sources

* `installer/src/S9Woa.Installer.Core/Stages/StageCatalog.cs`
* `installer/src/S9Woa.Installer.Core/Toolset/{ToolsetManager,ToolDefinitions,Validation}.cs`
* `installer/src/S9Woa.Installer.Core/Twrp/{WinReTwrpBuilder,PowerOffRoutePatch}.cs`
* `firmware/tools/pack-uefi.ps1` (header)
* `notes/m9-installer.md` §2/§3
* `dotnet build` / `dotnet test` runs above
