# M7 â€” Windows session: toolchain setup and driver builds

Running notes for the Windows session that started after
`docs/exynos9820-port/HANDOFF.md`. Appended to as things are learned.

## 0. Session start â€” environment verified (2026-10-09)

| Check | Result |
|---|---|
| Host OS | Windows 11, `10.0.26100` Build 26100 (24H2), x64, 64-bit â€” Lenovo ILYA-YOGA |
| adb | `C:\Users\ilya\code\platform-tools\adb.exe` â†’ `RF8M21HYDSZ  device  product:beyond0ltexx model:SM_G970F` â€” **authorized**, no `unauthorized` flag |
| git | repo usable only after `git config --global --add safe.directory C:/Users/ilya/code/exynos-woa` (tree copied from the Linux box; owner `S-1-5-32-544` != current user). HEAD = `85f1135 docs: write the Windows handoffâ€¦` |
| Visual Studio | **absent** â€” no `C:\Program Files\Microsoft Visual Studio`, no `msbuild`, no `cl`, no `infverif` on PATH |
| Windows Kits (SDK/WDK) | **absent** â€” `C:\Program Files (x86)\Windows Kits` does not exist |
| Free space on C: | ~2.9 TB free of ~3.9 TB |
| Privileges | `whoami /priv` shows no AdministratorRights/Debug â†’ **not elevated**; DISM refuses (`Elevated permissions are required`) |
| Network | reachable (`https://www.google.com` â†’ 200) |
| winget | present, `v1.29.380`; `Microsoft.WindowsWDK.10.0.26100` (10.1.26100.6584) and `Microsoft.VisualStudio.Community` (18.10.3 = VS 2026) both resolvable |

Conclusion: M7 is genuinely blocked on installing the toolchain. The build scripts
expect exactly:

- `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat`
  (`build-ufs.ps1:21`) and `...\18\Community\MSBuild\Current\Bin\MSBuild.exe`
  (`Build-Touch.ps1:15`) â†’ **VS 2026 Community** (major version 18).
- `C:\Program Files (x86)\Windows Kits\10\{Include,Lib}\10.0.26100.0`
  (`build-ufs.ps1:22,24,25`) â†’ **WDK/SDK 10.0.26100**, which matches this OS build.

## 1. Toolchain install â€” in progress
## 1. Toolchain install — DONE (2026-10-09, this session)

| Step | Command | Result |
|---|---|---|
| WDK/SDK 10.0.26100 | `winget install --id Microsoft.WindowsWDK.10.0.26100 --accept-package-agreements --accept-source-agreements --force` | "burn" installer (`wdksetup.exe`); installed `C:\Program Files (x86)\Windows Kits\10\{Include,Lib,Tools,bin,build}\10.0.26100.0`. Every import lib the UFS script needs is present: `Lib\10.0.26100.0\km\arm64\{storport,ntoskrnl,hal,wmilib,bufferoverflowfastfailk,aux_klib}.lib`, `Lib\10.0.26100.0\um\arm64\arm64rt.lib`; `Tools\10.0.26100.0\x64\infverif.exe`, `bin\10.0.26100.0\x86\Inf2Cat.exe` |
| VS 2026 Community (core only) | `winget install --id Microsoft.VisualStudio.Community --accept-package-agreements --accept-source-agreements --silent` | landed at `C:\Program Files\Microsoft Visual Studio\18\Community` (major **18** = the path the scripts hardcode), but **only the core workload**: no `VC\` dir, no `cl.exe` |
| VS components | `setup.exe modify --installPath "...\18\Community" --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add ...ARM64 --add Component.Microsoft.Windows.DriverKit --quiet` | **needs elevation**: run non-elevated it exits **5007** ("Commands with --quiet or --passive should be run elevated from the beginning"). With `Start-Process -Verb RunAs` it returns **3010** (ok, reboot pending) and installs `VC\Tools\MSVC\14.51.36231` + the WDK MSBuild toolsets |
| Windows 11 SDK component | same modify route, `--add Microsoft.VisualStudio.Component.Windows11SDK.26100` | exit 0. This is what supplies `Include\10.0.26100.0\shared\warning.h` — the WDK MSI alone does **not** ship it, and `build-ufs.ps1:104` force-includes it. Include tree went 1597 -> 6365 files |

Toolchain identity now:

- `cl.exe` = **Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260** (MSVC v145), host x64, cross targets `bin\Hostx64\arm64` and `bin\Hostx64\x64` both present
- WDK/SDK kit version **10.0.26100.0**, matching this OS build 26100
- KMDF toolset present: `MSBuild\Microsoft\VC\v180\Platforms\{ARM64,Win32,x64}\PlatformToolsets\WindowsKernelModeDriver10.0`

## 2. Two toolchain caveats found while verifying

1. **`HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots` (KitsRoot10) is absent** even after
   the elevated WDK install. MSBuild's `WindowsKernelModeDriver10.0` toolset derives
   `WDKContentRoot` from that registry value (`Toolset.props:33`), so a plain
   `msbuild ... /p:Platform=ARM64` will not find the kit. Workaround: pass
   `/p:WDKContentRoot=` or set `MicrosoftKitRoot` (`Toolset.props:27-31`).
   Source: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Microsoft\VC\v180\Platforms\ARM64\PlatformToolsets\WindowsKernelModeDriver10.0\Toolset.props`.
2. **The 9810 byte-for-byte reproduction check is inconclusive, not a failure.**
   `build-ufs.ps1 -SourceDir drivers\Exynos9810Ufs -OutDir <new> -WideUncached -DmaWindow`
   builds cleanly (`EXACT_BUILD_OK`, 49152 bytes, sha256 `e5eb688500bae492101973601607fd0d12f877105a76aa4ddee8b031bb5c43a2`)
   but the README's released hash is `a80b222939e7ec81522221740de5346b9799ff68492a875d996fdf947138eaa7`.
   The tree's 9810 sources are **not byte-identical to the published ones**:
   `release/provenance.json` records `published_sha256` for `Exynos9810Ufs.c` = `8c2adefe…`
   while the worktree file hashes `48a10631…` (git status is clean, so this tree's copy is what it is).
   Inference (mine): the mismatch is explained by the source difference and probably also by
   v145 vs the v143-era compiler the released driver used; the repo records **no compiler version**
   for the validated build (grep for msvc/v14x in the .md files: no matches). So the hash check
   cannot be closed here — it is not evidence that the toolchain is wrong.


## 3. M7.1 HSI2C build — attempt and diagnosis (2026-10-09)

Command that got furthest:

```
$env:MicrosoftKitRoot = 'C:\Program Files (x86)\'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  drivers\Exynos9820Hsi2c\Exynos9820Hsi2c.vcxproj /p:Configuration=Release /p:Platform=ARM64 /nologo /v:normal
```

Result: reaches `ClCompile`, runs `bin\HostX86\arm64\CL.exe /c /I\. /Zi /W4 /WX /O2 …`, then

```
internal.h(11,10): error C1083: Cannot open include file: 'ntddk.h': No such file or directory
```

Same failure for the **validated** `drivers\Exynos9810Hsi2c\Exynos9810Hsi2c.vcxproj` ? not a porting defect.

Facts:

* `ntddk.h` exists: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km\ntddk.h`.
* The cl line carried **no** WDK include path (`/I\.` only) and none of the WDK's kernel flags (`/kernel`, `/FI…\shared\warning.h`), so the WDK props that set them did not apply.
* `WindowsDriver.KernelMode.props:28` sets `<IncludePath>$(CRT_IncludePath);$(KM_IncludePath);$(KIT_SHARED_IncludePath);$(KIT_SHARED_INC_PATH_WDK)</IncludePath>`; `WindowsDriver.Shared.Props:105` sets `<ForcedIncludeFiles>$(KIT_SHARED_IncludePath)\warning.h</ForcedIncludeFiles>`. Those `*_IncludePath` values are empty in this invocation.
* Import chain that should have supplied them: `Microsoft.Cpp.Platform.props` ? `$(_PlatformFolder)ImportAfter\*.props` ? `MSBuild\Microsoft\VC\v180\Platforms\ARM64\ImportAfter\Microsoft.Cpp.WDK.props` ? `Windows Kits\10\build\10.0.26100.0\ARM64\ImportAfter\WDK.arm64.WindowsKernelModeDriver.Platform.props` ? `WindowsDriver.common.props` + `WindowsDriver.KernelMode.props`.
* `Microsoft.Cpp.WDK.props` derives `WDKBuildFolder` from `SDKVersionBuild`, which falls back to `$([Microsoft.Build.Utilities.ToolLocationHelper]::GetLatestSDKTargetPlatformVersion('Windows','10.0'))` — that helper needs `HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots\KitsRoot10`, which was **absent** after the WDK MSI install and had to be added with elevated `reg.exe`.
* Passing `/p:WDKContentRoot="…\Windows Kits\10"` **without** a trailing backslash breaks `Toolset.props:41` (`$(WDKContentRoot)build\…` — no separator) while `Toolset.props:45` (`$(WDKContentRoot)\build\…`) still works. That asymmetry produced the misleading `KMDF_VERSION_MINOR`/`TargetPlatformVersion_CO` empty-property errors. With the trailing backslash PowerShell eats the quote, so use `MicrosoftKitRoot` instead.

Inference (mine, unconfirmed): the remaining gap is in the `*_IncludePath` definitions, most plausibly supplied by `Microsoft.Cpp.WindowsSDK.props` / the Windows SDK component's registry wiring, which an admin session may complete. Next session: re-run the same command as admin and diff the cl command line.
