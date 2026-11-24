#!/usr/bin/env python3
import pathlib
import struct
import sys

if len(sys.argv) != 4:
    raise SystemExit("usage: verify.py BOOTSHIM.bin UEFI_BASE UEFI_SIZE")

path = pathlib.Path(sys.argv[1])
data = path.read_bytes()
base = int(sys.argv[2], 0)
size = int(sys.argv[3], 0)

if len(data) < 0x60:
    raise SystemExit("bootshim is too small to contain the Linux Image header")
if len(data) % 16:
    raise SystemExit("bootshim payload offset is not 16-byte aligned")
if data[0x38:0x3C] != b"ARMd":
    raise SystemExit("missing ARM64 Linux Image magic at offset 0x38")
if data[0x40:0x48] != b"EDK2-MSM":
    raise SystemExit("missing EDK2-MSM tag at offset 0x40")
if struct.unpack_from("<Q", data, 0x48)[0] != base:
    raise SystemExit("embedded UEFI_BASE does not match build argument")
if struct.unpack_from("<Q", data, 0x50)[0] != size:
    raise SystemExit("embedded UEFI_SIZE does not match build argument")

print(f"Verified {path}: {len(data)} bytes; payload starts at aligned offset {len(data)}")
