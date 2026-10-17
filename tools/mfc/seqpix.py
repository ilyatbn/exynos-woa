#!/usr/bin/env python3
"""seqpix.py LOG - decode DWM's TEMP "seqpix <ms> 0x<pixel> fmt <f>" lines (d3d10_gdi.c, G17 notes/g17-dxva-mfc.md)
written while tools flipseq runs: the composed centre pixel carries flipseq's frame number (R = 0x80 marker,
G = frame >> 8, B = frame & 0xff for B8G8R8A8; R/B swapped for an RGBA primary). Prints the shown-frame sequence and
counts repeats (same frame composed again), steps back (an older frame after a newer one) and skips."""
import re
import sys

rows = []
for line in open(sys.argv[1], encoding='latin-1'):
    m = re.search(r'seqpix ([0-9.]+) 0x([0-9a-f]{8}) fmt (\d+)', line)
    if m:
        rows.append((float(m.group(1)), int(m.group(2), 16)))
seq = []
for t, v in rows:
    b, g, r = v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff
    if r == 0x80:
        seq.append((t, (g << 8) | b))
    elif b == 0x80:          # RGBA primary: bytes R,G,B,A -> u32 A B G R
        seq.append((t, (g << 8) | r))
    else:
        seq.append((t, None))
shown = [f for _, f in seq if f is not None]
print(f'{len(rows)} composed frames logged, {len(shown)} carry a flipseq frame number')
rep = back = skip = 0
prev = None
out = []
for f in shown:
    if prev is not None:
        if f == prev:
            rep += 1
        elif f < prev:
            back += 1
        elif f > prev + 1:
            skip += f - prev - 1
    out.append(f)
    prev = f
print(f'repeats {rep}, steps back {back}, frames skipped {skip}')
print('sequence:', ' '.join(str(f) for f in out[:200]))
if len(seq) > 1:
    gaps = [b[0] - a[0] for a, b in zip(seq, seq[1:])]
    print('composition gaps ms: min %.1f median %.1f max %.1f' % (min(gaps), sorted(gaps)[len(gaps) // 2], max(gaps)))
