# M16 G12 - Mesa 26.2 d3d10umd + panfrost for Windows ARM64 (S10e Mali-G76)

Task: build Mesa 26.2 (gallium d3d10umd + panfrost + softpipe) for Windows ARM64 with the 19
wddm-mali-g52 `mesa-patches/` and `win32-compat/`, as their `.github/workflows/mesa.yml` does.
No device access. Rules: AGENTS.md §2 (persist), §3 (one commit per task), §7 (built != Ready).

## Inputs
- Mesa: `C:\Users\ilya\code\mali\mesa-26.2` (shallow clone, branch 26.2, HEAD 9a0169f, VERSION 26.2.4) - left untouched.
- Reference: `C:\Users\ilya\code\mali\wddm-mali-g52` (mesa-patches 0001-0019, win32-compat, ci/clc-stubs,
  `.github/workflows/mesa.yml`).

## Reference recipe (`.github/workflows/mesa.yml`, job `panfrost`)
- Mesa tag `mesa-26.2.0` from a GitHub mirror; we use the local 26.2.4 clone instead.
- Tools: python 3.12 + `meson ninja mako pyyaml packaging`; winflexbison 2.5.25 on PATH;
  `ci/clc-stubs` on PATH (python `mesa_clc` / `vtn_bindgen2` stand-ins that copy prebuilt files from
  `$LIBPAN_PREBUILT`).
- Source: copy `win32-compat` -> `mesa/include/win32-compat`, plus `include/malikm_ioctl.h` and
  `include/maliwddm_abi.h` into it; `git apply --whitespace=nowarn` each patch in name order.
- libpan SPIR-V: built on **Linux** (job `libpan-spirv`, ubuntu-24.04, LLVM 18 + libclc + LLVMSPIRVLib):
  `meson setup -Dgallium-drivers=panfrost -Dllvm=enabled -Dmesa-clc=enabled ...` then
  `ninja src/panfrost/libpan/libpan_v{4,5,6,7,9,10,12,13,14}.{spv,cpp,h}`. The reference does **not**
  ship them in the repo (only CI artifacts).
- Windows configure: CC=CXX=clang-cl (VS's ARM64 LLVM; CI runs natively on windows-11-arm),
  `-Dbuildtype=debugoptimized -Dplatforms=windows -Dgallium-drivers=softpipe,panfrost
  -Dgallium-d3d10umd=true -Dmesa-clc=system -Dvulkan-drivers= -Dllvm=disabled -Dopengl=false
  -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dvideo-codecs=
  -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false -Dzstd=disabled -Dshader-cache=disabled
  -Dbuild-tests=false -Dc_args=-DHAVE_FUNC_ATTRIBUTE_PACKED -Dcpp_args=-DHAVE_FUNC_ATTRIBUTE_PACKED`.
- Output: `libgallium_d3d10.dll` (the UMD).

## Host environment (2026-10-12)
- Host is AMD64 -> must **cross**-compile (reference builds natively on ARM64).
- Python 3.13.1 (WindowsApps), clang-cl 23.1.3 at `C:\Program Files\LLVM\bin` (x64 host; targets
  aarch64 with `--target`), MSVC 14.51.36231 (VS 18). VS has no `VC\Tools\Llvm`.
- No flex/bison/meson/ninja on PATH.
- WSL: `Ubuntu` (WSL2, stopped) present -> usable for the libpan step.

## Progress log
- 1. Read reference README, win32-compat/README, mesa.yml, clc-stubs; environment checked (above).
- 2. WSL Ubuntu 22.04 x86_64: **no passwordless sudo**, no LLVM/clang dev, no meson/ninja/bison/flex.
  jammy apt has llvm-15/libllvmspirvlib-15 but `spirv-tools` is 2022.1 and Mesa 26.2 needs
  `SPIRV-Tools >= 2024.1` when CLC is on (`meson.build:2086-2090`) -> use a user-space
  **micromamba / conda-forge** env in WSL (`~/mali-g12/`) for LLVM 18 + clang + SPIRV-LLVM-Translator +
  SPIRV-Tools + libdrm + meson/ninja/mako (same LLVM major as the reference CI).
- 3. Working copy: `git clone -c core.autocrlf=false --no-local mesa-26.2 mesa-26.2-g76` (original clone
  untouched; HEAD 9a0169f). Patches normalised CRLF->LF into `C:\Users\ilya\code\mali\g12-work\patches-lf\`
  (`tr -d '\r'`). Copied `win32-compat/` -> `include/win32-compat/` + `include/malikm_ioctl.h`,
  `include/maliwddm_abi.h` (as mesa.yml). `git apply --whitespace=nowarn` **0001..0019 all applied, no
  rejects**; 40 files changed, 527+/104-.
- 4. G76 in Mesa: `src/panfrost/model/pan_model.c:80`
  `BIFROST_MODEL(PAN_PROD_ID(7, 2, 1), "G76", "G76", MODEL_ANISO(ALL), MODEL_TB_SIZES(16384, 8192))`, no
  MODEL_QUIRKS (same as the G52 row 0x7222 below it) -> **no Mesa change needed** for the G76.
- 5. Cross-build finding: libpan also needs `panfrost_compile` (`src/panfrost/clc/meson.build`), built
  `native : not meson.can_run_host_binaries()`. The reference builds natively on ARM64 so it just runs
  it; on our x64->ARM64 cross it cannot link native against host libs -> use
  `-Dprecomp-compiler=system` and generate `libpan_shaders_v*.{h,c}` on Linux too (GCC = reference
  bitfield layout, the one 0003/0011/0016 reproduce on MSVC), with a third copy-stub
  `panfrost_compile` next to the reference's `mesa_clc` / `vtn_bindgen2` stubs.
- 6. WSL env: micromamba 2.9.0 (static binary, `~/mali-g12/dl-micromamba`), env `~/mali-g12/env` from
  conda-forge: llvmdev/clangdev **18.1.8**, libllvmspirv **18.1.5**, spirv-tools 2026.4, libdrm 2.4.129,
  meson 1.12.1, ninja 1.13.2, python 3.12.15, gcc/gxx 15.3.0 (conda sysroot 2.34). No sudo used.
  (conda-forge has no `spirv-llvm-translator` / `libclang-cpp18` names: the package is `libllvmspirv`.)
- 7. Working-copy patched state committed **locally in mesa-26.2-g76** as `a36e5bb` (so WSL can `git clone`
  it); not pushed anywhere, not in this repo.
- 8. WSL libpan build (`~/mali-g12/mesa` cloned from a36e5bb):
  - reference `tests/bifrost-pack/check.c` with gcc: `0 mismatches in 100000 rounds x 5 structs -> PASS`.
  - `meson setup -Dbuildtype=release -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms=
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=enabled -Dmesa-clc=enabled
    -Dprecomp-compiler=enabled -Dvideo-codecs= -Dbuild-tests=false` (LLVM 18.1.8 found).
  - `ninja` targets `libpan_v{4,5,6,7,9,10,12,13,14}.{spv,cpp,h}` + `libpan_shaders_v*.{h,c}`: 692 steps,
    0 warnings in ninja.log. Output copied to `C:\Users\ilya\code\mali\g12-work\libpan-prebuilt\` with
    `SHA256SUMS`. G76 = PAN_ARCH 7 (`libpan_v7.spv` 322424 B, `libpan_shaders_v7.c` 287711 B).
- 9. **ABI hazard found - Linux `libpan_shaders_v*.c` must NOT be used on Windows.** `pan_compile.c:223-235`
  memcpys a raw `struct bifrost_precompiled_kernel_info` (= `struct pan_shader_info` + 4 unsigned,
  `bifrost_compile.h:31-37`) in front of each binary; the UMD reads it back as the same struct. That
  struct has bitfields/enums, laid out differently by GCC (SysV) and clang-cl (MSVC ABI). The reference
  never hits this because CI runs `panfrost_compile` natively on windows-11-arm. Fix for our x64->ARM64
  cross: build `panfrost_compile.exe` **natively for Windows x64 with clang-cl** from the same patched
  tree (MSVC x64 and ARM64 use the same struct/bitfield/enum layout rules, LLP64, 8-byte pointers) and pass
  it with `-Dprecomp-compiler=system`. Only `libpan_v*.{spv,cpp,h}` (SPIR-V + NIR-builder C++, no struct
  blobs) are taken from Linux, exactly as the reference does. The Linux `libpan_shaders_*` are kept only to
  diff against the Windows ones.
- 10. Windows build tools: venv `C:\Users\ilya\code\mali\venv` (python 3.13.1; meson 1.12.1, ninja 1.13.2,
  Mako 1.4.3, PyYAML 6.0.3, packaging 26.3); winflexbison 2.5.25 (zip sha256
  8D324B62BE33604B2C45AD1DD34AB93D722534448F55A16CA7292DE32B6AC135; win_flex 2.6.4, bison 3.8.2) in
  `C:\Users\ilya\code\mali\g12-work\winflexbison`.
- 11. Host stage (x64 `panfrost_compile.exe`): `tools/mesa-build/build-mesa-g76.ps1 -Stage host`, vcvars64 +
  clang-cl 23.1.3 / lld-link 23.1.3 (`C:\Program Files\LLVM\bin`), `-Dbuildtype=release
  -Dprecomp-compiler=enabled` + the reference option set; meson pulled the `zlib` wrap subproject.
  677/677 steps, exit 0, 1919 `warning:` lines (clang `-Wunused-but-set-variable` on `NIR_PASS(_, ...)`
  etc., none errors). Script fixes on the way: vcvars prints `vswhere.exe` not found on stderr ->
  `2>nul`; PS 5.1 `Stop` turns native stderr fatal -> `Continue` + exit-code checks; `CC` with a space
  in the path breaks meson's shlex split -> LLVM bin on PATH, `CC=clang-cl`.
- 12. ABI proof (step 9 hazard):
  - x64 `panfrost_compile libpan_shaders 7 libpan_v7.spv` vs Linux: `.h` identical; `.c` differs
    (blob arrays 2 words = 8 bytes longer on Windows, e.g. `panlib_fill_0_default[308]` vs `[306]`) -> the
    header struct really has a different size; Linux `libpan_shaders_*` unusable on Windows, confirmed.
  - `clang-cl -Xclang -fdump-record-layouts-simple` of `bifrost_compile.h` for
    `--target=x86_64-pc-windows-msvc` vs `aarch64-pc-windows-msvc`: **all 366 common records
    identical** (only x64's `_SETJMP_FLOAT128` extra); `pan_shader_info` 8704 bits,
    `bifrost_precompiled_kernel_info` 8832 bits, same field offsets -> x64-generated blobs are valid for
    the ARM64 DLL. (Scratch: `g12-work\abi-check\`.)
- 13. libpan stage re-run through the committed `tools/mesa-build/wsl-libpan.sh` (exit 0, 692 steps, 0
  warnings); output layout: `libpan_v*.{spv,cpp,h}` at top (what `ci/clc-stubs` read via
  `LIBPAN_PREBUILT`), GCC-ABI `libpan_shaders_*` in `linux-abi-only/`, `SHA256SUMS`.
- 14. **Cross stage (ARM64) - built.** `build-mesa-g76.ps1 -Stage cross`: vcvarsamd64_arm64 (MSVC
  14.51.36231, link 14.51.36260.0 Hostx64\arm64), cross file `g12-work\arm64-windows.cross`
  (c/cpp = `clang-cl --target=aarch64-pc-windows-msvc` 23.1.3, `c_ld=link`, ar=llvm-lib, windres=llvm-rc,
  host_machine windows/aarch64), `-Dbuildtype=debugoptimized -Dprecomp-compiler=system` + reference
  options. meson summary: Gallium drivers `softpipe panfrost`, platforms `windows`, GFX/Compute YES;
  `mesa_clc`/`vtn_bindgen2` = reference stubs, `panfrost_compile` = step 11's x64 exe. **1037/1037
  steps, 0 `FAILED:`, 0 errors, 0 `warning LNK`**, 1231 compiler warnings: misleading-indentation 301,
  unused-variable 272, unused-but-set-variable 266, unused-function 248, pointer-sign 80,
  bitfield-constant-conversion 21, microsoft-enum-forward-reference 16, deprecated-declarations 6,
  sometimes-uninitialized 4 (d3d10umd `ShaderTGSI.c` 733/885/1015/1039, upstream code), unused-but-set-global 3.
  - bitfield-constant-conversion triage (MSVC ABI makes `enum x:2` signed): 20 in **Midgard** only
    (`midgard/compiler.h:635`, `midgard_compile.c:1317`, `midgard_emit.c:201`; G76 is Bifrost, unused);
    1 in `nir_lower_system_values.c:833` (`derivative_group:2 = DERIVATIVE_GROUP_LINEAR` -> -2) only under
    `shuffle_local_ids_for_quad_derivatives`, which only `panvk_vX_shader.c:489` sets - not gallium. No
    Bifrost-path hit; same as the reference's native build.
- 15. Collect stage -> `C:\Users\ilya\code\s10e-artifacts\mali-g12\` (exit 0):
  ```
  e0e5a88c7d5e9240c8c3f53ca2c10b9d5ad48458f4cf3119c9e260123c571146  libgallium_d3d10.dll   (16781312 B)
  008a63ecf518b3776ae07c443d35727735d0e23ae373f9c5777f72dc157af1be  libgallium_d3d10.pdb
  e0e5a88c7d5e9240c8c3f53ca2c10b9d5ad48458f4cf3119c9e260123c571146  maliumd.dll            (same bytes)
  b70ef586669a620a0a30a1156969c05c6a3831dc8f8bc992da75779d2a92f944  vc_redist.arm64.exe    (VS 14.51.36231 redist)
  ```
  `dumpbin /headers libgallium_d3d10.dll`: **`AA64 machine (ARM64)`**, subsystem 3, DLL characteristics 0x160
  (`dumpbin-headers.txt`). Imports (`dumpbin-deps-exports.txt`): KERNEL32, USER32, GDI32,
  api-ms-win-core-synch, api-ms-win-crt-* and **VCRUNTIME140.dll** (the only non-inbox one -> install
  `vc_redist.arm64.exe` first, as the reference's `tools/windows/m3-test.cmd` does). Exports: 47 `D3DKMT*`
  + `OpenAdapter10` + `OpenAdapter10_2` (49). The meson zlib wrap also produced `z-1.dll`; not imported,
  not shipped.
  - Strings in the DLL: `\.\MaliG52` device name, `maliwddm` backend, `MALIKM_FAKE`,
    `Panfrost found no Mali device` -> the shim is the reference's unchanged (as intended for this step).

## UMD registration (how Windows loads it)
- Reference `drivers/maliwddm/maliwddm.inf` `[MW_Settings]`:
  `HKR,,UserModeDriverName,%REG_MULTI_SZ%,maliumd.dll,maliumd.dll,maliumd.dll,maliumd.dll`
  (REG_MULTI_SZ = 0x00010000; 4 entries = the D3D runtime's slots) +
  `HKLM,...\GraphicsDrivers\Scheduler,EnablePreemption,REG_DWORD,0`. Binds `ACPI\RKCP7402`, class Display.
- The INF does **not** ship the DLL (`SourceDisksFiles` = maliwddm.sys only); its comment says
  `maliumd.dll` is `libgallium_d3d10.dll` renamed and must be in System32; their
  `tools/windows/m5-pe-test.cmd:50` does `copy libgallium_d3d10.dll %SystemRoot%\System32\maliumd.dll`.
- Without a loadable UMD a full WDDM adapter fails with code 43 (INF comment).
- Before c2 (hardware device) the reference runs the UMD as a *software* D3D10 driver
  (`D3D11CreateDevice(..., D3D_DRIVER_TYPE_SOFTWARE, LoadLibrary(libgallium_d3d10.dll))`,
  `tests/d3d11-triangle`) with `GALLIUM_DRIVER=panfrost` and the `\.\MaliG52` malikm device.
- For the S10e: our INF (future G13+) must bind `ACPI\SAMS9828` and keep the same `UserModeDriverName`;
  `maliumd.dll` from this build is ready to copy.

## G52 -> G76 in the Mesa tree
- None needed in Panfrost (step 4). Things that still say G52 and are **out of scope of this build**
  (follow-up tasks, not changed here): the shim's device name `\.\MaliG52` (matches whatever our KMD
  creates - keep the name in the retargeted malikm, or change the shim), and the `MALIKM_FAKE`
  GET_PARAM table in `win32-compat/malikm_drm.c` (GPU_PROD_ID 0x7402, SHADER_PRESENT 0x7, ...; G52
  values) - refresh from the G3 log per MILESTONES G12 before the FAKE run means anything for G76.

## Repeat the build
```
powershell -ExecutionPolicy Bypass -File tools\mesa-build\build-mesa-g76.ps1            # all stages
powershell ... -Stage host,cross,collect                                                  # after a source/libpan run
```
Stage `source` refuses to overwrite an existing `mesa-26.2-g76`. Inputs: `mesa-26.2` @ 9a0169f (26.2.4),
wddm-mali-g52 @ 16f7e7c (2026-10-03; 19 patches). Not run on hardware: **built, needs-boot** (AGENTS §7).
