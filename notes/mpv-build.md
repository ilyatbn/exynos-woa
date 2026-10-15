# Custom ARM64 mpv with the MFC hardware decoder (D10 V5)

User (2026-10-15 ~16:10): "Forget native windows players" -> a free third-party ffmpeg-based player; answered
"do a custom build in a separate folder outside the repo somewhere".

## Layout
- Build tree (outside the repo): C:\Users\ilya\code\mpv-arm64 - toolchain (llvm-mingw), sources (ffmpeg, mpv,
  libplacebo...), prefix, venv (meson/ninja). Nothing from there is committed.
- In the repo: tools/mfc/ffmpeg/ (the h264_exynos decoder source + patch), tools/mfc/mpv-build.sh (the build steps).

## Plan
1. llvm-mingw (mstorsjo, x86_64 host) for aarch64-w64-mingw32; MSYS2 bash/make/pkgconf; meson+ninja in a venv.
2. ffmpeg (static, minimal: h264 parser/demuxers, mp4/mkv, swscale/swresample) + libavcodec/h264_exynos.c:
   receive_frame decoder over the Exynos9820Mfc IOCTLs (MfcIoctl.h), bsf h264_mp4toannexb, one AU per
   IOCTL_MFC_DECODE, pts = smallest queued packet pts (display-order output), flush = reopen session.
3. mpv (meson, minimal: d3d11 gpu output, no lua/libass at first) linked against that ffmpeg; run on the phone with
   --hwdec=no --vd=h264_exynos (or as the preferred h264 decoder).
4. Validate: frame hashes via ffmpeg -c:v h264_exynos -f framemd5 vs libavcodec; mpv plays c1080.mp4 at full rate.

## Progress log
