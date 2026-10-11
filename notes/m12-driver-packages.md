# M12 - ARM64 test-signed driver packages: Exynos9820Hsi2c + S6SY761TouchBeyond0 (2026-10-11)

Task (sub-agent of the M12 touch session): build installable ARM64 test-signed packages (.sys/.inf/.cat) for offline
injection into the Win11 22631 image. Output: C:\Users\ilya\code\s10e-artifacts\touch-t1\{Exynos9820Hsi2c,S6SY761TouchBeyond0}.
No device access, no commits (caller's instruction). No driver-logic / constant changes.

## Progress log
- Toolchain: VS 18 Community has WDK MSBuild toolsets (WindowsKernelModeDriver10.0 under VC\*\Platforms\ARM64);
  WDK build dir 10.0.26100.0 present. pwsh 7 not on PATH.
- Build-Touch.ps1 is NOT usable here: it needs drivers\S6SY761TouchGpio\RetainedV24 (+ .sha256.json), tools\wdk-mirror
  and windows-drivers\Exynos9810Hsi2c\Arm64BuildEvidence.ps1 - none exist in this repo, and it #requires PS 7.
  => build S6SY761TouchBeyond0.vcxproj directly with MSBuild.
- drivers/Exynos9820Hsi2c/ARM64/Release already holds a stale Oct 9 build (gitignored); rebuilding into a fresh IntDir.
- Hsi2c MSBuild: VS18 MSBuild wants Microsoft.DriverKit.Build.Tasks.18.0.dll; WDK 26100 ships only *.17.0.dll
  -> /p:VisualStudioVersion=17.0. IntDir outside the tree breaks the vcxproj's `CopyOutput .\$(IntDir)` (".\C:\...")
  -> keep default in-tree ARM64\Release (gitignored). InfVerif task can't load x86\InfVerif.dll and ApiValidator's
  aitstatic exits 193 (tool breakage, not findings) -> /p:SkipPackageVerification=true /p:ApiValidator_Enable=false,
  infverif run by hand instead. Result: exit 0, 0 warnings (/W4 /WX), KMDF 1.15 (WdfLdr/WdfDriverEntry 1.15,
  SpbCxStubs spb\1.0), stampinf -a ARM64 -k 1.15. No source changes.
- Touch MSBuild (direct vcxproj, same 4 workaround properties + OutDir/IntDir under s10e-artifacts\touch-t1\build\touch):
  exit 0, 0 warnings (/W3 /WX), KMDF 1.15 libs, /arch:armv8.0. INF is a hand-written .inf (no stampinf in the project):
  already NTarm64.10.0...16299, KmdfLibraryVersion=1.15, DriverVer 09/16/2026,6.0.5058.3. No source changes.
- Offline-install finding: the touch INF sets UpperFilters=mshidkmdf via AddReg but has no Include=mshidkmdf.inf /
  Needs=MsHidKmdf_UpperFilter, so it never creates the mshidkmdf service. On this desktop the key exists only because
  hidvhf/msgpiowin32/buttonconverter/mshidkmdf INFs installed it (Owners). On the target image check
  SYSTEM\ControlSet001\Services\mshidkmdf offline; if absent the HID stack will not start. Left unchanged (driver INF
  design, not build plumbing) - reported to caller.
- Packaging (both): copy .sys + .inf to s10e-artifacts\touch-t1\<name>\, infverif /w /v (x64 Tools) = VALID,
  signtool sign /sha1 5DD5DEE1...E48 /fd SHA256 <sys>, Inf2Cat /driver:<dir> /os:10_RS3_ARM64,10_NI_ARM64,10_GE_ARM64
  (no errors/warnings), sign the .cat. signtool verify /pa /c: both .sys "signed in catalog", signer
  "WDKTestCert ilya,134359747320405644"; host verify fails only on root trust (test cert not in host Root) - expected.
  SHA256: Exynos9820Hsi2c.sys 11CD6C7E...DDA1, .inf F8361EC1...CBDC, .cat B72E99C7...9810;
  S6SY761TouchBeyond0.sys 50B836DF...14BB, .inf 4A334C9E...EA64, .cat 0D748C01...1BB6. Logs next to each package.
- Review: git status shows no tracked-file changes from this task (only this notes file + gitignored ARM64\Release).
