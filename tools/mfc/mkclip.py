#!/usr/bin/env python3
"""mkclip.py OUT.h264 W H FRAMES [profile] [bframes] - encode a moving test pattern with libx264 (PyAV) to an
Annex-B H.264 file and print libavcodec's decode of it as per-frame FNV-1a 32 hashes of the cropped luma and NV12
CbCr planes - the same lines tools/mfc/mfcdec prints on the phone (D10 V3, notes/d10-mfc-decode-flow.md).

Needs PyAV (pip install av numpy; kept in a scratch venv, not a repo dependency).
"""
import sys
from fractions import Fraction

import av
import numpy as np


def fnv_np(arr):
    # FNV-1a 32 over the plane bytes in row order (= mfcdec / Exynos9820Mfc MfcFnv)
    h = 0x811C9DC5
    for b in arr.tobytes():
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def nv12_planes(frame):
    f = frame.reformat(format="nv12")
    w, h = f.width, f.height
    y = np.frombuffer(bytes(f.planes[0]), np.uint8).reshape(-1, f.planes[0].line_size)[:h, :w]
    uv = np.frombuffer(bytes(f.planes[1]), np.uint8).reshape(-1, f.planes[1].line_size)[:h // 2, :w]
    return y, uv


def main():
    out, w, h, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    profile = sys.argv[5] if len(sys.argv) > 5 else "high"
    bframes = sys.argv[6] if len(sys.argv) > 6 else "2"
    enc = av.CodecContext.create("libx264", "w")
    enc.width, enc.height, enc.pix_fmt = w, h, "yuv420p"
    enc.time_base = Fraction(1, 30)
    enc.framerate = 30
    enc.options = {"profile": profile, "bframes": bframes, "preset": "fast", "crf": "23", "g": "30",
                   "x264-params": "annexb=1:repeat-headers=1"}
    yy, xx = np.mgrid[0:h, 0:w]
    data = bytearray()
    packets = []
    for i in range(n):
        y = ((xx * 2 + yy + i * 8) % 220 + 16).astype(np.uint8)
        y[(yy // 64 + xx // 64 + i // 4) % 2 == 0] //= 2
        u = ((xx[::2, ::2] + i * 3) % 200 + 28).astype(np.uint8)
        v = ((yy[::2, ::2] * 2 + i * 5) % 200 + 28).astype(np.uint8)
        fr = av.VideoFrame(w, h, "yuv420p")
        fr.planes[0].update(np.ascontiguousarray(np.pad(y, ((0, 0), (0, fr.planes[0].line_size - w)))).tobytes())
        fr.planes[1].update(np.ascontiguousarray(np.pad(u, ((0, 0), (0, fr.planes[1].line_size - w // 2)))).tobytes())
        fr.planes[2].update(np.ascontiguousarray(np.pad(v, ((0, 0), (0, fr.planes[2].line_size - w // 2)))).tobytes())
        fr.pts = i
        for p in enc.encode(fr):
            data += bytes(p)
            packets.append(p)
    for p in enc.encode(None):
        data += bytes(p)
        packets.append(p)
    with open(out, "wb") as f:
        f.write(data)
    # the same packets (x264 pts/dts) muxed into an .mp4 next to it, for Media Foundation's MPEG-4 source
    mp4 = av.open(out.rsplit(".", 1)[0] + ".mp4", "w")
    st = mp4.add_stream("h264", rate=30)
    st.width, st.height, st.pix_fmt = w, h, "yuv420p"
    st.time_base = Fraction(1, 30)
    first = bytes(packets[0])
    idr = first.find(bytes([0, 0, 0, 1, 0x65]))       # SPS + PPS (x264 repeat-headers) = everything before the IDR
    st.codec_context.extradata = first[:idr] if idr > 0 else first
    for p in packets:
        p.stream = st
        mp4.mux(p)
    mp4.close()
    print(f"{out}: {len(data)} bytes, {w}x{h}, {n} frames, {profile}, bframes {bframes}")
    dec = av.CodecContext.create("h264", "r")
    k = 0
    for pkt in dec.parse(bytes(data)) + dec.parse(None):
        for fr in dec.decode(pkt):
            y, uv = nv12_planes(fr)
            print(f"frame {k}: luma fnv 0x{fnv_np(y):08x} chroma fnv 0x{fnv_np(uv):08x}")
            k += 1
    for fr in dec.decode(None):
        y, uv = nv12_planes(fr)
        print(f"frame {k}: luma fnv 0x{fnv_np(y):08x} chroma fnv 0x{fnv_np(uv):08x}")
        k += 1


if __name__ == "__main__":
    main()
