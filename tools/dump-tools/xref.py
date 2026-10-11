# SPDX-License-Identifier: BSD-2-Clause-Patent
"""xref.py — locate code that materialises a target VA (string or MMIO
constant) in a raw ARM64 kernel Image, and dump the surrounding function.

No symbol table in the shipped 9820 kernel, so we anchor on .rodata
strings: find ADRP (+ADD/LDR) pairs computing the string's VA, then walk
back to the function preamble (STP x29, x30 after a RET or image start)
and forward to the matching RET.

Usage:
  python3 xref.py kernel --va 0xffffff8012345678            # all xrefs
  python3 xref.py kernel --string 'ipc_timeout' --func      # xref + disasm
  python3 xref.py kernel --pair 0x15900000 --func           # MMIO base VAs
"""

from __future__ import annotations

import argparse
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM64_LITTLE

VA_BASE = 0xFFFFFF8008000000  # file offset 0 == this VA (F7 in notes)


def find_string_offsets(blob: bytes, needle: str) -> list[int]:
    pat = needle.encode() + b"\0"
    out, i = [], 0
    while True:
        j = blob.find(pat, i)
        if j < 0:
            return out
        out.append(j)
        i = j + 1


def adrp_add_xrefs(md: Cs, blob: bytes, text_lo: int, text_hi: int,
                   targets: set[int]) -> list[tuple[int, int]]:
    """Return (file_off_of_adrp, target_va) for ADRP+ADD pairs computing a
    target VA. Decodes linearly in 4-byte steps over .text."""
    code = blob[text_lo:text_hi]
    hits = []
    # first pass: record pending ADRP results per register
    pending: dict[int, tuple[int, int]] = {}  # rd -> (adrp_off, page_va)
    for off, size, mnemonic, ops in md.disasm_lite(code, text_lo + VA_BASE):
        fo = off - VA_BASE
        if mnemonic == "adrp":
            parts = ops.split(", ")
            rd = int(parts[0][1:])
            page = int(parts[1], 16)
            pending[rd] = (fo, page)
        elif mnemonic in ("add", "ldr") and "#" in ops:
            parts = ops.split(", ")
            try:
                rd = int(parts[0][1:])
            except ValueError:
                continue
            if rd not in pending:
                continue
            adrp_fo, page = pending[rd]
            last = parts[-1]
            if last.startswith("#"):
                try:
                    imm = int(last[1:], 0)
                except ValueError:
                    m = last.strip("[]")
                    if "[" not in last:
                        continue
                    imm = int(m.split("#")[1], 0) if "#" in m else 0
                va = page + imm
                if va in targets:
                    hits.append((adrp_fo, va))
    return hits


def function_bounds(blob: bytes, fo: int, text_lo: int, text_hi: int) -> tuple[int, int]:
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM64_LITTLE)
    md.skipdata = True
    # walk back for a RET, then forward for the function start
    start = text_lo
    p = fo
    while p > text_lo:
        p -= 4
        for i in md.disasm(blob[p:p + 4], p + VA_BASE):
            if i.mnemonic == "ret":
                return _fwd_start(blob, p + 4, text_hi), fo
    return start, fo


def _fwd_start(blob: bytes, p: int, text_hi: int) -> int:
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM64_LITTLE)
    md.skipdata = True
    for i in md.disasm(blob[p:min(p + 0x400, text_hi)], p + VA_BASE):
        if i.mnemonic == "stp" and i.op_str.startswith("x29, x30"):
            return i.address - VA_BASE
    return p


def disasm_range(blob: bytes, fo: int, n: int) -> None:
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM64_LITTLE)
    md.skipdata = True
    for i in md.disasm(blob[fo:fo + n], fo + VA_BASE):
        print(f"  {i.address - VA_BASE:#09x} {i.address:#x}  "
              f"{i.mnemonic:<8} {i.op_str}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kernel")
    ap.add_argument("--string")
    ap.add_argument("--va", type=lambda x: int(x, 0))
    ap.add_argument("--pair", type=lambda x: int(x, 0),
                    help="device VA base; xrefs to base..base+0x1000")
    ap.add_argument("--text-lo", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--text-hi", type=lambda x: int(x, 0), default=0x2C00000)
    ap.add_argument("--func", action="store_true")
    ap.add_argument("--back", type=int, default=0x80,
                    help="bytes of function before the xref to dump")
    a = ap.parse_args()
    blob = open(a.kernel, "rb").read()

    targets: set[int] = set()
    if a.string:
        for fo in find_string_offsets(blob, a.string):
            targets.add(fo + VA_BASE)
            print(f"string {a.string!r} at file {fo:#x} VA {fo + VA_BASE:#x}")
    if a.va:
        targets.add(a.va)
    if a.pair:
        # MMIO is ioremapped — the kernel materialises the PHYSICAL addr in
        # the ioremap call; match the raw device address range.
        targets.update(range(a.pair, a.pair + 0x1000, 4))
    if not targets:
        raise SystemExit("give --string, --va or --pair")

    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM64_LITTLE)
    md.detail = False
    hits = adrp_add_xrefs(md, blob, a.text_lo, a.text_hi, targets)
    for fo, va in hits:
        print(f"xref: adrp at file {fo:#x} -> {va:#x}")
    if a.func and hits:
        fo = hits[0][0]
        s, _ = function_bounds(blob, fo, a.text_lo, a.text_hi)
        print(f"# function near file {s:#x}..; dumping from "
              f"{max(s, fo - a.back):#x}")
        disasm_range(blob, max(s, fo - a.back), 0x400)


if __name__ == "__main__":
    main()
