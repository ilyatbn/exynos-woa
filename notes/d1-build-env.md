# D1 build env — Linux verification build of Beyond0LtePkg

The authoritative FD build stays the Windows one (`firmware/tools/build-uefi.ps1`,
CLANGPDB). This machine (Zorin OS 18, no sudo) got a **verification-only** build so
the USB driver work can compile-check without waiting for the Windows box.
Recipe script: `/tmp/opencode/build-uefi-linux.sh` (regenerate from this file if lost).

## What had to be solved, in order (each is a fact from this session)

1. **edk2 clone**: `git clone --depth=1 https://github.com/tianocore/edk2` into
   `firmware/edk2` (gitignored by the repo's `edk2/` rule). Submodules needed even
   when unused: `MipiSysTLib/mipisyst`, `BrotliCustomDecompressLib/brotli`,
   `CryptoPkg/.../openssl`, `RegularExpressionDxe/oniguruma`, `ArmTransferListLib/libtl`
   — the `.dec` files reference their include dirs at parse time. `git submodule
   update --init --depth=1 --recursive` covers all.
2. **BaseTools host build**: `make -C BaseTools` fails on `GenFv` without
   `uuid/uuid.h`. No sudo → extracted `uuid-dev_2.39.3-9ubuntu6_amd64.deb` from
   `archive.ubuntu.com/pool/main/u/util-linux/` into `~/toolchains/root-uuid-dev`,
   copied `uuid/uuid.h` into `BaseTools/Source/C/Include/uuid/`, built with
   `LIBRARY_PATH=~/toolchains/root-uuid-dev/usr/lib/x86_64-linux-gnu`, run with
   `LD_LIBRARY_PATH=~/toolchains/root-libuuid1/usr/lib/x86_64-linux-gnu`.
   `BrotliCompress` still fails (submodule was empty at first make) — harmless,
   same as the Windows build (notes/m8-build.md).
3. **Toolchain tag**: edk2 master tools_def 3.06 **removed GCC5** — use `-t GCC`
   (NOT `-t GCCNOLTO`: it defines no `*_AARCH64_*_PATH`, silently falls back to host
   `gcc`). Env var is `GCC_AARCH64_PREFIX`.
4. **Conf**: `build.py` needs `Conf/tools_def.txt` + `build_rule.txt` — copy from
   `BaseTools/Conf/*.template`. The template ships **CRLF**; any sed edit must strip
   `\r` first (`sed -i 's/\r//g'`), and `Conf/.cache` must be deleted after editing
   tools_def (the parse cache is otherwise reused silently).
5. **BinWrappers**: `PATH` must include `BaseTools/BinWrappers/PosixLike` (Trim,
   AmlToC, …). Those wrappers exec `python` — set `PYTHON_COMMAND=python3` or every
   `.S` module dies with exit 127.
6. **iasl**: Ubuntu package is `acpica-tools` (pool/universe/a/acpica-unix/), not
   `iasl` — extracted to `~/toolchains/iasl`.
7. **Cross gcc**: xpack `aarch64-none-elf-gcc` — **both 15.2.1 and 12.3.1** emit
   `memcpy` libcalls for `VA_LIST`-by-value struct copies when `-mstrict-align`
   is on (bisected on the exact build command line: strict-align alone flips it
   0 -> 6 references). clang (Windows CLANGPDB) inlines them, so the real build
   never sees this. Verification-build fix: strip `-mstrict-align` from the
   gitignored Conf copy (kept `-mgeneral-regs-only`). Using xpack gcc 12.3.1
   (`xpack-aarch64-none-elf-gcc-12.3.1-2.1-linux-x64.tar.gz`) anyway for
   closer-to-CI behaviour.

## Script (as of 2026-10-10)

```bash
#!/bin/bash
set -x
cd /home/ilya/exynos-woa/firmware/edk2
rm -rf Conf/.cache   # tools_def parse cache — must go when we edit tools_def
sed -i "s/\r//g" Conf/tools_def.txt   # strip ALL CRs
sed -i 's/ -flto//g' Conf/tools_def.txt
# gcc (12 and 15) emits memcpy libcalls for VA_LIST struct copies when
# -mstrict-align is on; clang (the Windows CLANGPDB build) inlines them. This
# strip is VERIFICATION-BUILD-ONLY in the gitignored Conf copy — the real FD
# keeps the flag. (memcpy=6 -> 0 proven by flag bisection, notes/d1-build-env.md)
sed -i 's/ -mstrict-align//g' Conf/tools_def.txt
# gcc-only maybe-uninitialized false positive in edk2-master PeilessSec (clang
# on Windows does not warn) — verification build only.
grep -q "Wno-error=maybe-uninitialized" Conf/tools_def.txt || sed -i '/DEFINE GCC_ALL_CC_COMMON/s/$/ -Wno-error=maybe-uninitialized/' Conf/tools_def.txt
grep -q tree-loop-distribute Conf/tools_def.txt || sed -i '/DEFINE GCC_ALL_CC_COMMON/s/$/ -fno-tree-loop-distribute-patterns/' Conf/tools_def.txt
export WS=/home/ilya/exynos-woa/firmware
export WORKSPACE=$WS/edk2
export EDK_TOOLS_PATH=$WORKSPACE/BaseTools
export BASE_TOOLS_PATH=$EDK_TOOLS_PATH
export PACKAGES_PATH=$WORKSPACE:$WS
export PYTHON_COMMAND=python3
export PYTHONPATH=$WORKSPACE/BaseTools/Source/Python
export GCC_AARCH64_PREFIX=$HOME/toolchains/xpack-aarch64-none-elf-gcc-12.3.1-2.1/bin/aarch64-none-elf-
export LD_LIBRARY_PATH=$HOME/toolchains/root-libuuid1/usr/lib/x86_64-linux-gnu
export PATH=$WORKSPACE/BaseTools/BinWrappers/PosixLike:$WORKSPACE/BaseTools/Source/C/bin:$HOME/toolchains:$PATH
python3 $WORKSPACE/BaseTools/Source/Python/build/build.py -a AARCH64 -t GCC -p ../Platform/Beyond0LtePkg/Beyond0LtePkg.dsc -b DEBUG -j 24
```

## Status log

- 2026-10-10: all modules up to the DXE/FV stage compile with gcc 15 EXCEPT the
  memcpy link failures above; gcc 12.3.1 download started. No FD produced yet on
  this machine. Windows FD state is unchanged by any of this (nothing here touches
  the repo's Conf — `firmware/edk2/` is gitignored).
- 2026-10-10 (later): **build green with gcc 12.3.1** after also (a) stripping
  `-mstrict-align` in the Conf copy — the memcpy calls come from strict-align +
  VA_LIST struct copies, proven by bisection, and it is NOT a gcc-15 quirk
  (gcc 12.3.1 does it too; clang inlines them), and (b) `-Wno-error=maybe-uninitialized`
  for edk2-master `PeilessSec.c:169` (gcc-only warning). Output:
  `Build/Beyond0LtePkg/DEBUG_GCC/FV/BEYOND0LTE_UEFI.fd` 7 340 032 B
  sha256 `83a9986e…`, FVMAIN_COMPACT 417 792 B / 648 free, FVMAIN 46% full.
  **This FD is verification-only** (different flags from CLANGPDB); do not flash it.
  The repo itself changed only by the two `Pmic.c` initializations gcc flagged.
