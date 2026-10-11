# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Unpack Samsung/Android boot images and ODIN AP tarballs for analysis.

Handles what the SM-G970F Android 12 (G970FXXSGHWC1) AP tar actually
contains:
  * AP tar: plain tar; members boot.img, dtbo.img, super.img, ... —
    extracted selectively (streamed, so the 6.3 GB tar never lands whole).
  * Samsung boot.img: an opaque Samsung header, then a classic
    "ANDROID!" header v0 whose 0x28 word is repurposed as dt_size and
    whose DT blob trails the sections (same layout the repo's
    tools/twrp-winre/bootimg.py round-trips).
  * dtbo.img: Android DTBO container ("DTBO" magic + entry table).

Compression (ramdisk/dtb) is detected by magic and decoded with the venv's
lz4/gzip when present.
"""

from __future__ import annotations

import gzip
import io
import lzma
import os
import struct
import tarfile

ANDROID_MAGIC = b"ANDROID!"
SAMSUNG_MAGIC = b"SAMSUNG "
DTBO_MAGIC = b"DTBO"


def _pad(n: int, page: int) -> int:
    return (n + page - 1) // page * page


def find_android_header(blob: bytes, window: int = 0x10000) -> int:
    i = blob.find(ANDROID_MAGIC)
    if i < 0 or i > window:
        raise ValueError("no ANDROID! header in first window")
    return i


def decompress(data: bytes) -> tuple[bytes, str]:
    if data[:2] == b"\x1f\x8b":
        return gzip.decompress(data), "gzip"
    if data[:4] in (b"\x04\x22\x4d\x18", b"\x04\x22\x4d\x19"):
        import lz4.frame
        return lz4.frame.decompress(data), "lz4-frame"
    if data[:4] == b"\x28\xb5\x2f\xfd":
        return lzma.decompress(data), "lzma"
    if data[:6] == b"SZ\x00\x04\x00\x00":  # Samsung "sld" legacy lz4? rare
        pass
    return data, "raw"


class BootImage:
    def __init__(self):
        self.kernel: bytes = b""
        self.ramdisk: bytes = b""
        self.second: bytes = b""
        self.dt: bytes = b""
        self.samsung_header: bytes = b""
        self.header_version = 0
        self.name = ""

    @classmethod
    def parse(cls, blob: bytes) -> "BootImage":
        off = find_android_header(blob)
        img = cls()
        img.samsung_header = blob[:off]
        h = blob[off:]
        (k_size, _ka, rd_size, _ra, s_size, _sa, _ta, page,
         dt_size_or_hv) = struct.unpack_from("<9I", h, 8)
        hdr_version = 0
        if len(h) >= 0x40:
            hv = struct.unpack_from("<I", h, 0x36)[0]
            if hv in (1, 2, 3, 4):
                hdr_version = hv
        img.header_version = hdr_version
        dt_size = 0 if hdr_version else dt_size_or_hv
        p = page
        img.kernel = h[p:p + k_size]; p += _pad(k_size, page)
        img.ramdisk = h[p:p + rd_size]; p += _pad(rd_size, page)
        img.second = h[p:p + s_size]; p += _pad(s_size, page)
        if dt_size:
            img.dt = h[p:p + dt_size]
        elif off:
            # Samsung v0 fallback: DT blob(s) sit at the tail; scan for magic
            img.dt = h[p:]
        tags = h[0x10:0x20]
        img.name = h[0x30:0x40].split(b"\0")[0].decode("latin1") \
            if len(h) >= 0x40 else ""
        return img

    def unpack_to(self, outdir: str) -> list[str]:
        os.makedirs(outdir, exist_ok=True)
        written = []
        for field in ("kernel", "ramdisk", "second", "dt"):
            data = getattr(self, field)
            if not data:
                continue
            dec, how = decompress(data)
            path = os.path.join(outdir, f"{field}.img")
            with open(path, "wb") as f:
                f.write(dec)
            written.append(f"{field}: {len(dec)} B ({how})")
        return written


def parse_dtbo(blob: bytes) -> list[bytes]:
    """Android DTBO container -> list of overlay dtb blobs."""
    off = blob.find(DTBO_MAGIC)
    if off < 0:
        raise ValueError("no DTBO magic")
    h = blob[off:]
    (_magic, version, header_size, entry_size, entry_count,
     entries_off) = struct.unpack_from("<6I", h, 0)
    out = []
    for i in range(entry_count):
        e = entries_off + i * entry_size
        dt_size, dt_off, _id, _rev = struct.unpack_from("<4I", h, e)
        out.append(h[dt_off:dt_off + dt_size])
    return out


def extract_from_ap_tar(tar_path: str, members: list[str], outdir: str) -> list[str]:
    """Stream-selective extraction: only the named members are written."""
    os.makedirs(outdir, exist_ok=True)
    written = []
    with tarfile.open(tar_path, mode="r|") as tf:  # streaming, no index
        want = set(members)
        for m in tf:
            base = m.name.rsplit("/", 1)[-1]
            if m.isfile() and (m.name in want or base in want):
                want.discard(m.name); want.discard(base)
                src = tf.extractfile(m)
                if src is None:
                    continue
                dst = os.path.join(outdir, base)
                with open(dst, "wb") as f:
                    while True:
                        chunk = src.read(1 << 22)
                        if not chunk:
                            break
                        f.write(chunk)
                written.append(f"{m.name} -> {dst} ({m.size} B)")
                if not want:
                    break
    return written
