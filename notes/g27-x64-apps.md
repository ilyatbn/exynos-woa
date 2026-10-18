# G27 - x64 (emulated) apps on our GPU: an ARM64X UMD

Why: x64 applications under emulation on Windows on Arm (most modern games) cannot use the Mali adapter - the UMD is
a plain ARM64 DLL, which an x64/ARM64EC process cannot load (research-requests R10). User goal: games + browsers on
the GPU, natively.

## Research
- MS Learn "Build Arm64X Files" (https://learn.microsoft.com/en-us/windows/arm/arm64x-build), section "Building an
  Arm64X pure forwarder DLL": "An Arm64X pure forwarder DLL is a small Arm64X DLL that forwards APIs to separate DLLs
  depending on their type: Arm64 APIs are forwarded to an Arm64 DLL. x64 APIs are forwarded to an x64 or Arm64EC
  DLL." Built from empty arm64 + /arm64EC objects, two DEF files (EXPORTS X = foo_arm64.X / foo_x64.X), `link /lib`
  import libs, then `link /dll /noentry /machine:arm64x /defArm64Native:<arm64 def> /def:<x64 def> ...`. "When an Arm64
  process loads foo.dll, the operating system immediately loads foo_arm64.dll in its place". For merged Arm64X
  binaries "the Arm64 and Arm64EC configurations must use the same C runtime" (ours: static /MT since 23ef073).
- Prior art: osy/virtio-win-mesa Triton UMD, src/virtio/neptune/triton/tools/make-arm64x.bat (MIT) - exactly this
  forwarder (neptune_umd.dll -> neptune_umd_arm64.dll / neptune_umd_ec.dll), DEFs generated from the UMD's .def.
- Mesa docs: Visual Studio builds for ARM64EC are supported (docs.mesa3d.org/install.html, notes/d5-gpu-display.md).

## Plan (one commit each)
- G27.1 baseline: an x64 build of d3dcs on the phone (what does an emulated x64 app get today?).
- G27.2 ARM64EC Mesa build stage (build-mesa-g76.ps1 -Stage ec) -> maliumd_ec.dll.
- G27.3 ARM64X forwarder (tools/mesa-build/make-arm64x.ps1): maliumd.dll -> maliumd_arm64.dll / maliumd_ec.dll;
  deploy by hand, x64 + ARM64 tests, DWM untouched until the reboot.
- G27.4 INF: the three files to System32.

## Progress log
- G27.1 baseline: d3dcs built for x64 (build-d3dcs.cmd x64 -> d3dcs64x.exe), emulated on the phone: "Mali: D3D11CreateDevice 0x887a0004 FL 0x0" = DXGI_ERROR_UNSUPPORTED - an x64 process gets no device on the Mali adapter (the ARM64 UMD cannot load into it).
