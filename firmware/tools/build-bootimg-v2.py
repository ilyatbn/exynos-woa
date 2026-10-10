# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Wrap a [shim][FD] kernel in an Android boot image with header VERSION 2 + a real DTB section.

sboot's Android-12 ABL `load_kernel` rejects the Samsung header-v0 container our
`repack-with-ref.py` emits (`dt_size=1` -> `dt table header check fail` -> `DT LOAD fail`,
notes/m8-uefi-image.md B12/B16). The proven 9820 recipe (edk2-exynos
`Platform/Samsung/exynos9820/s10.sh.inc`) builds the container with AOSP `mkbootimg.py
--header_version 2 --dtb <FDT>`. This mirrors that header layout byte-for-byte (the
`write_header()` field order in AOSP `mkbootimg.py`, `BOOT_IMAGE_HEADER_V2_SIZE=1660`) so the
DTB section carries a valid FDT and the DT check passes.

Header-v2 field order (AOSP mkbootimg.py write_header, header padded to page_size):
  magic[8] kernel_size kernel_addr ramdisk_size ramdisk_addr second_size second_addr
  tags_addr page_size header_version os_version/patch board[16] cmdline[512] id[32]
  extra_cmdline[1024] recovery_dtbo_size[4] recovery_dtbo_offset[8] header_size[4]
  dtb_size[4] dtb_addr[8]
Sections (each padded to page_size): kernel, ramdisk, second, dtb.

The offsets/base/OS fields default to the edk2-exynos s10.sh.inc values (UNVERIFIED as a
boot-confirmed constant until a boot passes the DT check); the DTB defaults to nothing - pass
--dtb (the phone's own FDT, stock/dt.dtb, magic d00dfeed).
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

BOOT_MAGIC = b"ANDROID!"
BOOT_NAME_SIZE = 16
BOOT_ARGS_SIZE = 512
BOOT_EXTRA_ARGS_SIZE = 1024
BOOT_IMAGE_HEADER_V2_SIZE = 1660


def _pad(n: int, page: int) -> int:
    return (n + page - 1) // page * page


def _os_version(x: str) -> int:
    a, _, rest = x.partition(".")
    b = rest.partition(".")[0] if rest else "0"
    return (int(a) << 14) | (int(b or 0) << 7)


def _os_patch_level(x: str) -> int:
    y, _, m = x.partition("-")
    mm = m.partition("-")[0] if m else "0"
    return ((int(y) - 2000) << 4) | int(mm or 0)


def _asciiz(s: str, size: int) -> bytes:
    b = s.encode() + b"\x00"
    if len(b) > size:
        raise SystemExit(f"asciiz too long for {size}: {len(b)}")
    return b.ljust(size, b"\x00")


def _empty_ramdisk() -> bytes:
    """A valid empty initramfs: newc cpio with only the TRAILER, padded to 512."""
    name = b"TRAILER!!!\x00"
    fields = [0, 0o100755, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0]
    out = bytearray(b"070701")
    for v in fields:
        out += b"%08x" % (v & 0xFFFFFFFF)
    out += name
    out += b"\x00" * (_pad(len(out), 4) - len(out))
    out += b"\x00" * (_pad(len(out), 512) - len(out))
    return bytes(out)


def build(kernel: bytes, dtb: bytes, ramdisk: bytes, second: bytes, *,
          base: int, kernel_offset: int, ramdisk_offset: int, second_offset: int,
          tags_offset: int, dtb_offset: int, page_size: int,
          os_version: str, os_patch_level: str, cmdline: str, board: str) -> bytes:
    if dtb[:4] != b"\xd0\x0d\xfe\xed":
        raise SystemExit(f"dtb is not a valid FDT (magic {dtb[:4]!r}) - the DT check would fail")

    sha = hashlib.sha1()
    for part in (kernel, ramdisk, second):
        sha.update(part)
        sha.update(struct.pack("<I", len(part)))
    sha.update(struct.pack("<I", 0))          # recovery_dtbo (none)
    sha.update(dtb)
    sha.update(struct.pack("<I", len(dtb)))
    img_id = sha.digest()

    h = bytearray()
    h += struct.pack("8s", BOOT_MAGIC)
    h += struct.pack("<I", len(kernel))
    h += struct.pack("<I", base + kernel_offset)
    h += struct.pack("<I", len(ramdisk))
    h += struct.pack("<I", (base + ramdisk_offset) if ramdisk else 0)
    h += struct.pack("<I", len(second))
    h += struct.pack("<I", (base + second_offset) if second else 0)
    h += struct.pack("<I", base + tags_offset)
    h += struct.pack("<I", page_size)
    h += struct.pack("<I", 2)
    h += struct.pack("<I", (_os_version(os_version) << 11) | _os_patch_level(os_patch_level))
    h += _asciiz(board, BOOT_NAME_SIZE)
    h += _asciiz(cmdline[:BOOT_ARGS_SIZE - 1], BOOT_ARGS_SIZE)
    h += struct.pack("32s", img_id)
    h += _asciiz(cmdline[BOOT_ARGS_SIZE - 1:], BOOT_EXTRA_ARGS_SIZE)
    h += struct.pack("<I", 0)                 # recovery_dtbo_size
    h += struct.pack("<Q", 0)                 # recovery_dtbo_offset
    h += struct.pack("<I", BOOT_IMAGE_HEADER_V2_SIZE)
    h += struct.pack("<I", len(dtb))          # dtb_size
    h += struct.pack("<Q", base + dtb_offset) # dtb_addr
    assert len(h) == BOOT_IMAGE_HEADER_V2_SIZE, f"header is {len(h)}, expected {BOOT_IMAGE_HEADER_V2_SIZE}"

    out = bytearray(h)
    out += b"\x00" * (page_size - len(h))
    for part in (kernel, ramdisk, second, dtb):
        out += part
        out += b"\x00" * (_pad(len(part), page_size) - len(part))
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="build an Android boot image header-v2 with a DTB section")
    ap.add_argument("--kernel", required=True, help="[shim][FD] blob")
    ap.add_argument("--dtb", required=True, help="device tree blob (valid FDT)")
    ap.add_argument("--output", required=True)
    ap.add_argument("--base", default="0x10000000")
    ap.add_argument("--kernel-offset", default="0x8000")
    ap.add_argument("--ramdisk-offset", default="0x1000000")
    ap.add_argument("--second-offset", default="0xf0000000")
    ap.add_argument("--tags-offset", default="0x100")
    ap.add_argument("--dtb-offset", default="0x0")
    ap.add_argument("--pagesize", default="2048")
    ap.add_argument("--os-version", default="13.0.0")
    ap.add_argument("--os-patch-level", default="2023-07")
    ap.add_argument("--cmdline", default="buildvariant=userdebug")
    ap.add_argument("--board", default="")
    ap.add_argument("--ramdisk", default="", help="ramdisk file (default: empty cpio)")
    args = ap.parse_args()

    kernel = Path(args.kernel).read_bytes()
    dtb = Path(args.dtb).read_bytes()
    ramdisk = Path(args.ramdisk).read_bytes() if args.ramdisk else _empty_ramdisk()

    out = build(
        kernel, dtb, ramdisk, b"",
        base=int(args.base, 0), kernel_offset=int(args.kernel_offset, 0),
        ramdisk_offset=int(args.ramdisk_offset, 0), second_offset=int(args.second_offset, 0),
        tags_offset=int(args.tags_offset, 0), dtb_offset=int(args.dtb_offset, 0),
        page_size=int(args.pagesize, 0), os_version=args.os_version,
        os_patch_level=args.os_patch_level, cmdline=args.cmdline, board=args.board,
    )
    Path(args.output).write_bytes(out)

    print(f"kernel   {len(kernel)}  ramdisk {len(ramdisk)}  second 0  dtb {len(dtb)}")
    print(f"header   v2  page {args.pagesize}  base {args.base}  -> {len(out)} bytes -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
