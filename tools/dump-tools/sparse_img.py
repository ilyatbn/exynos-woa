# SPDX-License-Identifier: BSD-2-Clause-Patent
"""sparse_img.py — Android sparse image -> raw image (pure Python).

Format read from sparse_defs.h semantics: 28 B header, then chunks of
12 B header + payload. CAC1 raw, CAC2 fill, CAC3 hole, CAC4 crc/check.

Usage: python3 sparse_img.py in.img out.img
"""

import struct
import sys

SPARSE_MAGIC = 0xED26FF3A
CAC_HEADER, CAC_DATA, CAC_FILL, CAC_HOLE, CAC_CRC = (
    0xCAC0, 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4)


def convert(src: str, dst: str) -> None:
    with open(src, "rb") as f:
        data = f.read()
    (magic, vmaj, vmin, hsz, chsz, blk_sz, blocks, chunks,
     _csum) = struct.unpack_from("<IHHHHIIII", data, 0)
    assert magic == SPARSE_MAGIC, f"not sparse: {magic:#x}"
    # secondary magic2 only exists for minor_version >= 1
    if vmin >= 1:
        (sp_magic,) = struct.unpack_from("<I", data, 28)
        assert sp_magic == 0x61ED26, f"bad secondary magic {sp_magic:#x}"
    out = bytearray()
    p = hsz
    for _ in range(chunks):
        ctype, _res, bcount, csize = struct.unpack_from("<HHII", data, p)
        # csize counts the chunk header too
        payload_len = csize - chsz
        p += chsz
        if ctype == CAC_DATA:
            out += data[p:p + payload_len]
        elif ctype == CAC_FILL:
            val = data[p:p + 4] * 4
            out += val * (bcount * blk_sz // 4)
        elif ctype == CAC_HOLE:
            out += b"\0" * (bcount * blk_sz)
        elif ctype == CAC_CRC:
            pass
        else:
            raise SystemExit(f"unknown chunk type {ctype:#x}")
        p += payload_len
    with open(dst, "wb") as f:
        f.write(out)
    print(f"{src}: {chunks} chunks -> {len(out)} B raw "
          f"({blocks} blocks x {blk_sz})")
    print("fs magic:",
          "ext4" if out[0x438:0x43A] == b"\x53\xef" else
          "f2fs" if out[0x400:0x404] == b"\x10\x20\xf5\xf2" else
          "erofs" if out[1024:1028] == b"\xe2\xe1\xf5\xe0" else "unknown")


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
