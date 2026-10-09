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

## Sources

* `installer/src/S9Woa.Installer.Core/Stages/StageCatalog.cs`
* `installer/src/S9Woa.Installer.Core/Toolset/{ToolsetManager,ToolDefinitions,Validation}.cs`
* `installer/src/S9Woa.Installer.Core/Twrp/{WinReTwrpBuilder,PowerOffRoutePatch}.cs`
* `firmware/tools/pack-uefi.ps1` (header)
* `notes/m9-installer.md` §2/§3
* `dotnet build` / `dotnet test` runs above
