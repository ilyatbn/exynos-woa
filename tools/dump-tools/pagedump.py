#!/usr/bin/env python3
"""Recursive ARMv8 4K-granule translation table dumper for firmware images.
usage: pagedump.py <image> <table_vaddr> <max_va_bits>
Maps VA->file offset as: off = va - 0x87000000 + 0x1000  (uh.bin GREENTEA+ELF layout).
"""
import sys

BASE_VA, BASE_OFF = 0x87000000, 0x1000

def main():
    data = open(sys.argv[1], 'rb').read()
    base = int(sys.argv[2], 0)
    # level 0 covers remaining bits: T0SZ from max_va: entries = 2**(48-t0sz?) simpler:
    # start at level determined by caller's max_va: 4K map: L0 if va range >2^39? Use standard:
    # L0 idx bits = VA_BITS-39, L1 = 9, L2 = 9, L3 = 9 (4K pages)
    va_bits = int(sys.argv[3])
    lvl0_bits = max(0, va_bits - 39)
    lvl0_shift = va_bits - lvl0_bits
    def rd64(va):
        off = va - BASE_VA + BASE_OFF
        if off < 0 or off + 8 > len(data):
            return None
        return int.from_bytes(data[off:off+8], 'little')
    def walk(tbl_va, level, va_start):
        if level == 0:
            bits, shift = lvl0_bits, lvl0_shift
        elif level == 1:
            bits, shift = 9, 30
        elif level == 2:
            bits, shift = 9, 21
        else:
            bits, shift = 9, 12
        for i in range(1 << bits):
            e = rd64(tbl_va + i * 8)
            if e is None or e == 0:
                continue
            va = va_start + (i << shift)
            typ = e & 3
            if typ == 3:
                print(f"L{level} [{i:3d}] -> table {e & ~0xFFF:#x} (for VA {va:#x})")
                if level < 3:
                    walk(e & ~0xFFF, level + 1, va)
                else:
                    print("  (L3 points to table? invalid)")
            elif typ == 1:
                pa = e & ((1 << 48) - 1) & ~((1 << shift) - 1)
                memattr = (e >> 6) & 3
                sh = (e >> 8) & 3
                print(f"L{level} BLK {va:#012x}..{va + (1<<shift):#012x} -> PA {pa:#012x} "
                      f"sz={1<<shift:#x} MemAttr={['Device-nRnE','Normal-NC','Normal-OWB-ORWB?','Normal-WB'][memattr]} "
                      f"SH={['Non','ISV(res0)','OS','IS'][sh]} AP={(e>>7)&1}XN PXN={(e>>53)&1} UXN={(e>>54)&1} AF={(e>>10)&1} contiguous={(e>>52)&1} raw={e:#018x}")
            else:
                print(f"L{level} [{i:3d}] raw={e:#018x} @VA {va:#x}")
    start_level = 0 if lvl0_bits else 1
    print(f"== table @ {base:#x}, start L{start_level}, VA bits {va_bits}")
    walk(base, start_level, 0)

if __name__ == '__main__':
    main()
