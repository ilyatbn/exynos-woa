#!/usr/bin/env python3
"""Pair adrp with the next add/ldr/str on the same register (skip <=8 instrs not touching it)
from a capstone base-0 disasm dump; emit code-site -> absolute file-offset refs.
usage: adrpindex.py <disasm.txt>  -> writes <disasm.txt>.adrpidx"""
import sys, re

def main():
    path = sys.argv[1]
    addrs, mnems, ops = [], [], []
    for line in open(path):
        m = re.match(r'^([0-9a-f]+)\s+[0-9a-f]+\s+(\S+)\s*(.*)$', line)
        if not m: continue
        addrs.append(int(m.group(1), 16)); mnems.append(m.group(2)); ops.append(m.group(3))
    n = len(addrs)
    refs = {}
    for i in range(n):
        if mnems[i] != 'adrp': continue
        parts = ops[i].split(', ')
        if len(parts) != 2: continue
        reg, pg = parts[0], int(parts[1].lstrip('#'), 0)
        for j in range(i+1, min(i+9, n)):
            mn2, op2 = mnems[j], ops[j]
            if mn2 not in ('add', 'ldr', 'str', 'ldp', 'stp'): continue
            p2 = op2.split(', ')
            if p2[0] != reg: continue
            if mn2 == 'add' and len(p2) == 3 and p2[2].startswith('#'):
                refs.setdefault(pg + int(p2[2].lstrip('#'), 0), []).append((addrs[j], 'add'))
                break
            mm = re.search(r'\[x\d+, #(0x[0-9a-f]+)\]', op2)
            if mm:
                refs.setdefault(pg + int(mm.group(1), 0), []).append((addrs[j], mn2))
                break
    with open(path + '.adrpidx', 'w') as f:
        for t, sites in sorted(refs.items()):
            for b, k in sites:
                f.write(f"{b:08x}\t{k}\t->0x{t:08x}\n")
    print(f"wrote {path}.adrpidx ({len(refs)} targets)")

if __name__ == '__main__':
    main()
