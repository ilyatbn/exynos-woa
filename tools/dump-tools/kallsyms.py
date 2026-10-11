# SPDX-License-Identifier: BSD-2-Clause-Patent
"""kallsyms.py — recover the symbol table from a raw ARM64 Linux 4.14 Image.

The shipped 9820 kernel keeps CONFIG_KALLSYMS on. In 4.14 the generated
tables sit in one translation unit in source order:

    kallsyms_addresses[]  (num * u64, strictly increasing)
    kallsyms_num_syms     (u32)
    kallsyms_names[]      (u8 len + text; bytes >= 0x80 index token_table)
    kallsyms_markers[]    (u32 offsets, one per 256 names)
    kallsyms_token_index[] (256 * u16)
    kallsyms_token_table[] (NUL-terminated substrings)

Detection is mechanical: the longest run of strictly increasing u64s in
kernel-VA range is the addresses array; everything else is found relative
to it. VA -> file offset uses the `_text` symbol as the base (ARM64 Image
text starts at file offset 0).

Usage:  python3 kallsyms.py kernel > symbols.tsv   (addr \t size \t name)
"""

from __future__ import annotations

import struct
import sys


def find_addresses(blob: bytes) -> tuple[int, int]:
    """Return (offset, count) of the longest non-decreasing u64 run whose
    values look like kernel VAs (PAGE_OFFSET 0xffffff8000000000 for
    VA_BITS=39, or 0xffff800000000000 for 48). Aliased symbols repeat an
    address, so the run is non-decreasing, not strictly increasing."""
    n = len(blob)
    best = (0, 0)

    def va_ok(v: int) -> bool:
        # VA_BITS=39 -> PAGE_OFFSET 0xffffff8000000000; VA_BITS=48 -> 0xffff000000000000
        return (v >> 32) in (0xFFFFFF80, 0xFFFF0000)

    off = 0
    while off + 16 <= n:
        a, b = struct.unpack_from("<2Q", blob, off)
        if not (va_ok(a) and va_ok(b) and b >= a and b - a < 0x4000000):
            off += 8
            continue
        start, prev = off, a
        off += 8
        while off + 8 <= n:
            (v,) = struct.unpack_from("<Q", blob, off)
            if not va_ok(v) or v < prev or v - prev > 0x4000000:
                break
            prev, off = v, off + 8
        count = (off - start) // 8
        if count > best[1]:
            best = (start, count)
    return best


def main(path: str) -> None:
    blob = open(path, "rb").read()
    a_off, num = find_addresses(blob)
    print(f"# addresses at file {a_off:#x}, num_syms {num}", file=sys.stderr)

    # num_syms u32 should sit right after the array (source order)
    p = a_off + num * 8
    (num2,) = struct.unpack_from("<I", blob, p)
    if num2 == num:
        p += 4
    names_off = p

    # names blob: walk num entries; each is len byte + len bytes
    q = names_off
    for _ in range(num):
        if q >= len(blob):
            raise SystemExit("names blob overruns file — layout guess wrong")
        ln = blob[q]
        q += 1 + ln
    # markers: ceil(num/256) u32s, increasing
    markers = []
    m = q
    expect = 0
    while len(markers) < (num + 255) // 256:
        (v,) = struct.unpack_from("<I", blob, m)
        if v < expect or v > names_off + 0x2000000:
            break
        markers.append(v)
        expect = v
        m += 4
    tok_idx_off = m
    tok_idx = struct.unpack_from("<256H", blob, tok_idx_off)
    tok_tab_off = tok_idx_off + 512

    def token(i: int) -> str:
        e = blob.index(b"\0", tok_tab_off + tok_idx[i])
        return blob[tok_tab_off + tok_idx[i]:e].decode("latin1")

    def name_at(pos: int) -> tuple[str, int]:
        ln = blob[pos]
        raw = blob[pos + 1:pos + 1 + ln]
        out = []
        for b in raw:
            if b >= 0x80:
                out.append(token(b - 0x80))
            else:
                out.append(chr(b))
        return "".join(out), pos + 1 + ln

    # sanity: token_index[0] must be 0 and names must decode to ascii-ish
    if tok_idx[0] != 0:
        raise SystemExit(f"token_index[0] = {tok_idx[0]}, expected 0")

    names = []
    q = names_off
    for _ in range(num):
        s, q = name_at(q)
        names.append(s)

    base = None
    try:
        base = next(a for a, s in zip(
            struct.unpack_from(f"<{num}Q", blob, a_off), names) if s == "_text")
    except StopIteration:
        pass
    if base is None:
        base = struct.unpack_from("<Q", blob, a_off)[0]
    print(f"# _text VA {base:#x} -> file 0", file=sys.stderr)

    addrs = struct.unpack_from(f"<{num}Q", blob, a_off)
    rows = []
    for a, s in zip(addrs, names):
        fo = a - base
        rows.append((a, fo, s))
    rows.sort(key=lambda r: r[0])
    for i, (a, fo, s) in enumerate(rows):
        size = rows[i + 1][0] - a if i + 1 < len(rows) else 0
        print(f"{a:x}\t{fo:x}\t{size}\t{s}")


if __name__ == "__main__":
    main(sys.argv[1])
