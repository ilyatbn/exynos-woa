#!/bin/bash
# build-ffmpeg.sh <work-dir> - cross-build FFmpeg 9.0.2 + h264_exynos for Windows ARM64 (shared DLLs + ffmpeg.exe)
# with llvm-mingw, from an MSYS2 bash (for make). notes/mpv-build.md. The work dir holds llvm-mingw-*/ and
# src/ffmpeg-9.0.2/; the result goes to <work-dir>/prefix.
set -e
W=${1:?usage: build-ffmpeg.sh <work-dir>}
export PATH="$(echo "$W"/llvm-mingw-*/bin):$PATH"
SRC="$W/src/ffmpeg-9.0.2"
"$(dirname "$0")/apply.sh" "$SRC"
mkdir -p "$W/build-ffmpeg"
cd "$W/build-ffmpeg"
if [ ! -f config.h ]; then
  "$SRC/configure" --prefix="$W/prefix" \
    --enable-cross-compile --arch=aarch64 --target-os=mingw32 --cross-prefix=aarch64-w64-mingw32- \
    --cc=aarch64-w64-mingw32-clang --cxx=aarch64-w64-mingw32-clang++ \
    --enable-shared --disable-static --disable-autodetect --enable-w32threads \
    --disable-ffplay --disable-ffprobe --disable-doc --disable-debug \
    --extra-libs=-lcfgmgr32
fi
grep -E "^#define CONFIG_H264_EXYNOS_DECODER" config_components.h
make -j"$(nproc)"
make install
ls "$W/prefix/bin"
