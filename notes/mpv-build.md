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
- 16:20 toolchain: llvm-mingw 20261006 ucrt x86_64 (clang 23.1.3, zip sha256 317492c4...) in mpv-arm64\; meson
  1.12.1 + ninja in mpv-arm64\venv. WSL Ubuntu exists but sudo needs a password -> no apt; shinchiro's
  mpv-winbuild-cmake not used.
- Cheaper route: MSYS2 CLANGARM64 repo ships Windows ARM64 mpv 0.41.0-8 linked to ffmpeg 9.0.2 DLLs.
  pacman -Sw (integrity-checked) mpv + ffmpeg closure -> mpv-arm64\pkgs (145 packages, 184 MB), extracted to
  mpv-arm64\sysroot\clangarm64 (not installed into MSYS2). DLLs: avcodec-63, avformat-63, avdevice-63, avfilter-12,
  avutil-61, swresample-7, swscale-10. Plan: cross-build ffmpeg 9.0.2 (same version -> same DLL names / avpriv ABI)
  with h264_exynos and replace the ffmpeg DLLs next to mpv.exe.
- 16:25 ffmpeg 9.0.2 + h264_exynos cross-built (tools/mfc/ffmpeg/{h264_exynos.c,apply.sh,build-ffmpeg.sh}; first
  configure had CONFIG_H264_EXYNOS_DECODER 0 because deps="mingw32" is not a configure feature -> dropped; missing
  libavutil/mem.h include). Shared DLLs with the same names as MSYS2's (avcodec-63, avformat-63, avdevice-63,
  avfilter-12, avutil-61, swresample-7, swscale-10) + ffmpeg.exe in mpv-arm64\prefix\bin. No warnings in
  h264_exynos.c.
- 16:22 the phone reset silently (ping/ssh gone ~2 min, back 16:24 via stay.flg): Kernel-Power 41 (unclean), KD log
  just stops (no bugcheck). MFC idle at that time (last session 15:40; mftest -enum only opened a handle). Same
  family as the earlier intermittent silent resets; not attributed to the MFC.
- 16:27 phone (ssh, C:\S10eTmp\mpv = mpv.exe + 60-file DLL closure, 62 MB, our 7 ffmpeg DLLs + ffmpeg.exe):
  ffmpeg -c:v h264_exynos c1080.mp4 -benchmark: 60 frames in 0.78 s (77 fps, 2.56x realtime, utime 0.11 s);
  framemd5 (both -pix_fmt nv12) h264_exynos vs FFmpeg's software h264: **60/60 identical incl. pts**
  (the first compare differed only because the software decoder outputs planar yuv420p).
- 16:30 **mpv plays on the MFC.** vd=lavc:h264_exynos is ignored (mpv opened software h264): mpv treats decoders
  with a wrapper_name + AV_CODEC_CAP_HARDWARE as hwdec methods "<wrapper>-copy" -> portable_config/mpv.conf
  hwdec=exynosmfc-copy (tools/mfc/ffmpeg/mpv.conf). Log: "Looking at hwdec h264_exynos-exynosmfc-copy... Using
  underlying hw-decoder 'h264_exynos' ... Using hardware decoding (exynosmfc-copy)", decoder format 1920x1080 nv12;
  VO gpu/d3d11 on the Mali-G76 WDDM adapter (feature level 10_0, shaderc). c1080.mp4 x4 (--loop-file=3, 8 s of
  video) finished at 9.0 s incl. ~0.3 s start-up -> real time, no frame drops logged. Launched in the desktop
  session with tools/phone/runuser2.ps1 (non-elevated user: opening the device worked).
