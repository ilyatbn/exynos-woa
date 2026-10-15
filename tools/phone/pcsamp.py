#!/usr/bin/env python3
"""Decode \\PCSAMP.BIN - the m14 PC sampler ring (park stub flag 5, notes/m14-smp.md "PC sampler").

Header (slot 0): 'PCS1', slots, next slot, samples taken, CNTVCT of the last sample, period.
Record (256 B):  CNTVCT, sample number, then per core (MADT GICC order) PCSRlo, EDSCR, VIDSR, PCSRhi, PRSR, AUTH
(records before 2026-10-15 10:00 have CIDSR, 0 in those two words).
Prints the records oldest first; 'same' collapses runs where no core's PC/EL/PRSR changed.

usage: pcsamp.py PCSAMP.BIN [--all] [--last N]
"""
import struct
import sys

SLOT = 0x100
NAMES = ["a55-0", "a55-1", "a55-2", "a55-3", "a75-0", "a75-1", "m4-0", "m4-1"]


def el_of(vid):
    if vid == 0xFAFAFAFA:
        return "flt"
    if vid & (1 << 29):
        return "E3"
    if vid & (1 << 30):
        return "E2"
    return "ns" if vid & (1 << 31) else "s"


def main():
    args = sys.argv[1:]
    path = args[0]
    show_all = "--all" in args
    last = int(args[args.index("--last") + 1]) if "--last" in args else 0
    d = open(path, "rb").read()
    magic, slots, nxt, taken, lastcnt, period = struct.unpack_from("<IIQQQQ", d, 0)
    print(f"magic {magic:08x} slots {slots} next {nxt} samples {taken} last cntvct {lastcnt:x} period {period}")
    order = list(range(nxt, slots)) + list(range(1, nxt)) if taken >= slots - 1 else list(range(1, nxt))
    recs = []
    for s in order:
        cnt, seq = struct.unpack_from("<QQ", d, s * SLOT)
        cores = []
        for c in range(8):
            lo, scr, vid, hi, prsr, auth = struct.unpack_from("<6I", d, s * SLOT + 0x10 + c * 0x18)
            cores.append(((hi << 32) | lo, scr, vid, prsr, auth))
        recs.append((cnt, seq, cores))
    if last:
        recs = recs[-last:]
    t_end = lastcnt
    prev = None
    skipped = 0
    for cnt, seq, cores in recs:
        key = [(pc, el_of(vid), prsr, scr, auth) for pc, scr, vid, prsr, auth in cores]
        if prev is not None and key == prev and not show_all:
            skipped += 1
            continue
        if skipped:
            print(f"   ... {skipped} identical samples")
            skipped = 0
        prev = key
        t = (cnt - t_end) / 26e6
        line = " ".join(f"{NAMES[i]}:{pc:016x}/{el_of(vid)}/prsr {prsr:03x}/scr {scr:08x}/auth {auth:02x}"
                        for i, (pc, scr, vid, prsr, auth) in enumerate(cores))
        print(f"#{seq:6d} t{t:+8.2f}s {line}")
    if skipped:
        print(f"   ... {skipped} identical samples (to the end)")


if __name__ == "__main__":
    main()
