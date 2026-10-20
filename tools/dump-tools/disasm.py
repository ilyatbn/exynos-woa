#!/usr/bin/env python3
"""Full AArch64 disassembly + system-register index for firmware images.
Usage: disasm.py <elf|raw-bin> <out-prefix> [--raw BASE]
Outputs: <out>.disasm.txt (all insns), <out>.sysreg.tsv (msr/mrs sites), <out>.mmio.tsv (constructed abs addrs)
"""
import sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from capstone.arm64 import *

def load(path, raw_base):
    if raw_base is not None:
        data = open(path, 'rb').read()
        return data, raw_base, [(0, len(data))]
    # ELF: iterate exec sections
    import struct
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF'
    e_phoff = struct.unpack_from('<Q', d, 0x20)[0]
    e_phnum = struct.unpack_from('<H', d, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        o = e_phoff + i * 56
        p_type, p_flags = struct.unpack_from('<II', d, o)
        p_off, p_vaddr = struct.unpack_from('<QQ', d, o + 8)
        p_filesz, p_memsz = struct.unpack_from('<QQ', d, o + 32)
        if p_type == 1 and p_flags & 1:  # PT_LOAD X
            segs.append((p_vaddr, p_off, p_filesz))
    return d, None, segs

def main():
    path, prefix = sys.argv[1], sys.argv[2]
    raw_base = None
    if '--raw' in sys.argv:
        raw_base = int(sys.argv[sys.argv.index('--raw') + 1], 0)
    d, base, segs = load(path, raw_base)
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    md.skipdata = True
    sysregs = {}
    mmio = {}
    lines = []
    for seg in segs:
        if raw_base is None:
            vaddr, off, fsz = seg
        else:
            off, fsz = 0, len(d)
            vaddr = raw_base
        for ins in md.disasm(d[off:off+fsz], vaddr):
            if ins.id == 0:  # skipdata placeholder
                for i in range(0, len(ins.bytes), 4):
                    w = int.from_bytes(ins.bytes[i:i+4], 'little')
                    lines.append(f"{ins.address+i:08x}  {ins.bytes[i:i+4].hex():<8s}  .word 0x{w:08x}")
                continue
            txt = f"{ins.address:08x}  {ins.bytes.hex():<8s}  {ins.mnemonic} {ins.op_str}".rstrip()
            lines.append(txt)
            if ins.mnemonic in ('msr', 'mrs'):
                try:
                    op = ins.operands[1] if ins.mnemonic == 'msr' else ins.operands[1]
                    sr = ins.reg_name(op.sysreg) if op.type == ARM64_OP_SYSREG else None
                except Exception:
                    sr = None
                if sr is None:
                    # fallback: parse op_str
                    parts = ins.op_str.replace(',', ' ').split()
                    sr = parts[0] if parts else '?'
                sysregs.setdefault(sr, []).append((ins.address, ins.mnemonic))
            # adrp+add/ldr constant reconstruction is done later; record literal-ish constants
    open(prefix + '.disasm.txt', 'w').write('\n'.join(lines) + '\n')
    with open(prefix + '.sysreg.tsv', 'w') as f:
        for sr, sites in sorted(sysregs.items()):
            for addr, mn in sites:
                f.write(f"{addr:08x}\t{mn}\t{sr}\n")
    # index: which sysregs are WRITTEN (msr) at EL2-relevant space
    with open(prefix + '.msr-index.txt', 'w') as f:
        for sr, sites in sorted(sysregs.items()):
            wr = [hex(a) for a, m in sites if m == 'msr']
            rd = [hex(a) for a, m in sites if m == 'mrs']
            f.write(f"{sr}: msr={len(wr)} {','.join(wr[:12])} | mrs={len(rd)}\n")

if __name__ == '__main__':
    main()
