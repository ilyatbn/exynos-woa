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
7. **Cross gcc**: xpack `aarch64-none-elf-gcc` 15.2.1 compiles everything but
   **links fail with `undefined reference to memcpy`** in any module using
   `VA_LIST`-by-value (PrintLib/DebugLib paths): gcc-15 + the standard
   `-mstrict-align -mgeneral-regs-only` XIP flags make it emit a `memcpy` libcall
   for the 32-byte va_list struct copy instead of inline moves. Proven by flag
   bisection on the exact build command line. Fix: use xpack gcc **12.4.0**
   (`xpack-aarch64-none-elf-gcc-12.4.0-1.2-linux-x64.tar.gz`) — or the Windows
   CLANGPDB build, which is unaffected (it builds today).
   Also added `-fno-tree-loop-distribute-patterns` to `GCC_ALL_CC_COMMON` and
   stripped `-flto` in the local Conf copy (LTO also invents memcpy calls);
   both edits live in the script, not in the repo.

## Script (as of 2026-10-10)

```bash
#!/bin/bash
set -x
cd /home/ilya/exynos-woa/firmware/edk2
rm -rf Conf/.cache
sed -i 's/\r//g' Conf/tools_def.txt
sed -i 's/ -flto\b//g' Conf/tools_def.txt
grep -q tree-loop-distribute Conf/tools_def.txt || sed -i '/DEFINE GCC_ALL_CC_COMMON/s/$/ -fno-tree-loop-distribute-patterns/' Conf/tools_def.txt
export WS=/home/ilya/exynos-woa/firmware
export WORKSPACE=$WS/edk2
export EDK_TOOLS_PATH=$WORKSPACE/BaseTools
export BASE_TOOLS_PATH=$EDK_TOOLS_PATH
export PACKAGES_PATH=$WORKSPACE:$WS
export PYTHON_COMMAND=python3
export PYTHONPATH=$WORKSPACE/BaseTools/Source/Python
export GCC_AARCH64_PREFIX=$HOME/toolchains/xpack-aarch64-none-elf-gcc-12.4.0-1.2/bin/aarch64-none-elf-
export LD_LIBRARY_PATH=$HOME/toolchains/root-libuuid1/usr/lib/x86_64-linux-gnu
export PATH=$WORKSPACE/BaseTools/BinWrappers/PosixLike:$WORKSPACE/BaseTools/Source/C/bin:$HOME/toolchains:$PATH
python3 $WORKSPACE/BaseTools/Source/Python/build/build.py -a AARCH64 -t GCC -p ../Platform/Beyond0LtePkg/Beyond0LtePkg.dsc -b DEBUG -j 24
```

## Status log

- 2026-10-10: all modules up to the DXE/FV stage compile with gcc 15 EXCEPT the
  memcpy link failures above; gcc 12.4 download started. No FD produced yet on
  this machine. Windows FD state is unchanged by any of this (nothing here touches
  the repo's Conf — `firmware/edk2/` is gitignored).
