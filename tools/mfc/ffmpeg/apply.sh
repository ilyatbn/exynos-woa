#!/bin/bash
# apply.sh <ffmpeg-source-dir> - add the h264_exynos decoder (tools/mfc/ffmpeg/h264_exynos.c) to an FFmpeg 9.x tree
# (notes/mpv-build.md). Idempotent: lines already present are not added twice.
set -e
F=${1:?usage: apply.sh <ffmpeg-source-dir>}
H=$(cd "$(dirname "$0")" && pwd)
cp "$H/h264_exynos.c" "$F/libavcodec/h264_exynos.c"
cp "$H/../../../drivers/Exynos9820Mfc/MfcIoctl.h" "$F/libavcodec/MfcIoctl.h"
grep -q ff_h264_exynos_decoder "$F/libavcodec/allcodecs.c" ||
  sed -i 's/^extern const FFCodec ff_h264_v4l2m2m_decoder;$/&\nextern const FFCodec ff_h264_exynos_decoder;/' "$F/libavcodec/allcodecs.c"
grep -q CONFIG_H264_EXYNOS_DECODER "$F/libavcodec/Makefile" ||
  sed -i 's/^OBJS-\$(CONFIG_H264_MEDIACODEC_DECODER) += mediacodecdec.o$/&\nOBJS-$(CONFIG_H264_EXYNOS_DECODER)     += h264_exynos.o/' "$F/libavcodec/Makefile"
sed -i "/^h264_exynos_decoder_deps=/d" "$F/configure"
grep -q h264_exynos_decoder "$F/configure" ||
  sed -i 's/^h264_v4l2m2m_decoder_select="h264_mp4toannexb_bsf"$/&\nh264_exynos_decoder_select="h264_mp4toannexb_bsf"/' "$F/configure"
grep -c "h264_exynos" "$F/libavcodec/allcodecs.c" "$F/libavcodec/Makefile" "$F/configure"
