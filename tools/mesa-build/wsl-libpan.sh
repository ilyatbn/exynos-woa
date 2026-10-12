#!/usr/bin/env bash
# M16 G12 - libpan SPIR-V for the Windows Mesa build, generated on Linux (WSL), the way
# wddm-mali-g52 .github/workflows/mesa.yml job `libpan-spirv` does it.
# Usage: wsl-libpan.sh <patched-mesa-tree-as-/mnt/c-path> <out-dir-as-/mnt/c-path>
# Tooling lives in user space only: micromamba + conda-forge env under ~/mali-g12 (no sudo).
# Outputs libpan_v{N}.{spv,cpp,h} (used by the Windows build via the reference's clc stubs) and
# libpan_shaders_v{N}.{h,c} (Linux/GCC ABI - for comparison only, NOT usable on Windows, see
# notes/gpu-m16-g12-mesa.md step 9).
set -euo pipefail
SRC=${1:?patched mesa tree}
OUT=${2:?output dir}
W=~/mali-g12
MM=$W/dl-micromamba/bin/micromamba
ENV=$W/env
export MAMBA_ROOT_PREFIX=$W/mamba-root

if [ ! -x "$MM" ]; then
  mkdir -p $W/dl-micromamba
  curl -sSL https://micro.mamba.pm/api/micromamba/linux-64/latest -o $W/dl-micromamba/mm.tar.bz2
  tar -C $W/dl-micromamba -xjf $W/dl-micromamba/mm.tar.bz2 bin/micromamba
fi
if [ ! -x "$ENV/bin/meson" ]; then
  "$MM" create -y -p "$ENV" -c conda-forge \
    python=3.12 meson ninja mako pyyaml packaging pkg-config bison flex \
    'llvmdev=18.1' 'clangdev=18.1' 'libllvmspirv=18.1' 'spirv-tools>=2024.1' \
    libdrm expat zlib c-compiler cxx-compiler
fi

export PATH=$ENV/bin:$PATH
export PKG_CONFIG_PATH=$ENV/lib/pkgconfig:$ENV/share/pkgconfig
export LD_LIBRARY_PATH=$ENV/lib
export CC=gcc CXX=g++

cd $W
rm -rf mesa build libpan
git clone -q "$SRC" mesa
echo "mesa: $(git -C mesa log --oneline -1)"

meson setup build mesa -Dbuildtype=release \
  -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled \
  -Dvideo-codecs= -Dbuild-tests=false > setup.log 2>&1 \
  || { tail -60 setup.log; tail -60 build/meson-logs/meson-log.txt; exit 1; }

targets=""
for v in 4 5 6 7 9 10 12 13 14; do
  for f in libpan_v$v.spv libpan_v$v.cpp libpan_v$v.h libpan_shaders_v$v.h libpan_shaders_v$v.c; do
    targets="$targets src/panfrost/libpan/$f"
  done
done
ninja -C build $targets > ninja.log 2>&1 || { tail -60 ninja.log; exit 1; }
echo "ninja: $(tail -1 ninja.log); warnings: $(grep -ci warning ninja.log || true)"

mkdir libpan
cp build/src/panfrost/libpan/libpan_v*.{spv,cpp,h} libpan/
mkdir libpan/linux-abi-only
cp build/src/panfrost/libpan/libpan_shaders_v*.{h,c} libpan/linux-abi-only/
rm -rf "$OUT" && mkdir -p "$OUT"
cp -r libpan/* "$OUT/"
(cd libpan && sha256sum libpan_v* linux-abi-only/*) > "$OUT/SHA256SUMS"
echo "libpan -> $OUT"
