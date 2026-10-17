"""dxbcdis.py <tlog> <tag> - disassemble a shader token stream the UMD dumped into its tlog (lines "<tick> <tag> @<n>:
<hex tokens>", e.g. tag "GS 0" from Shader.cpp gs_unsupported, G25) on the PC: the tokens are wrapped in a DXBC container
with one SHDR chunk and handed to D3DDisassemble (d3dcompiler_47, documented API). The container carries the DXBC
checksum (an MD5 variant over the bytes after the checksum field, with DXBC's own padding of the last block; zeros
were refused with E_FAIL). notes/g24-compute-cs4x.md."""
import ctypes
import math
import re
import struct
import sys

ZERO = bytes(1)
PAD = bytes([0x80])


def tokens(path, tag):
    out = []
    pat = re.compile(r'^\d+ ' + re.escape(tag) + r' @(\d+):((?: [0-9a-f]{8})+)\s*$')
    for line in open(path, encoding='utf-8', errors='replace'):
        m = pat.match(line)
        if m:
            idx = int(m.group(1))
            vals = [int(x, 16) for x in m.group(2).split()]
            if idx != len(out):
                raise SystemExit('gap in token dump at %d (have %d)' % (idx, len(out)))
            out.extend(vals)
    return out


_S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4
_K = [int(abs(math.sin(i + 1)) * 2 ** 32) & 0xffffffff for i in range(64)]


def _md5_block(st, blk):
    a, b, c, d = st
    m = struct.unpack('<16I', blk)
    for i in range(64):
        if i < 16:
            f, g = (b & c) | (~b & d), i
        elif i < 32:
            f, g = (d & b) | (~d & c), (5 * i + 1) % 16
        elif i < 48:
            f, g = b ^ c ^ d, (3 * i + 5) % 16
        else:
            f, g = c ^ (b | ~d), (7 * i) % 16
        f = (f + a + _K[i] + m[g]) & 0xffffffff
        a, d, c = d, c, b
        b = (b + (((f << _S[i]) | (f >> (32 - _S[i]))) & 0xffffffff)) & 0xffffffff
    return [(x + y) & 0xffffffff for x, y in zip(st, (a, b, c, d))]


def dxbc_checksum(data):
    """DXBC container checksum of everything after byte 20."""
    d = data[20:]
    n = len(d)
    st = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
    full = n - n % 64
    for o in range(0, full, 64):
        st = _md5_block(st, d[o:o + 64])
    left = d[full:]
    bits, bits2 = (n * 8) & 0xffffffff, (((n * 8) >> 2) | 1) & 0xffffffff
    if len(left) >= 56:
        st = _md5_block(st, left + PAD + ZERO * (63 - len(left)))
        st = _md5_block(st, struct.pack('<I', bits) + ZERO * 56 + struct.pack('<I', bits2))
    else:
        blk = struct.pack('<I', bits) + left + PAD
        blk += ZERO * (60 - len(blk)) + struct.pack('<I', bits2)
        st = _md5_block(st, blk)
    return struct.pack('<4I', *st)


def container(toks):
    shdr = struct.pack('<%dI' % len(toks), *toks)
    chunk = b'SHDR' + struct.pack('<I', len(shdr)) + shdr
    header_size = 4 + 16 + 4 + 4 + 4 + 4   # magic, checksum, one, size, count, one offset
    total = header_size + len(chunk)
    c = b'DXBC' + ZERO * 16 + struct.pack('<IIII', 1, total, 1, header_size) + chunk
    return c[:4] + dxbc_checksum(c) + c[20:]


def main():
    toks = tokens(sys.argv[1], sys.argv[2])
    if not toks:
        raise SystemExit('no tokens for tag %r' % sys.argv[2])
    print('%d tokens (header says %d), version 0x%08x' % (len(toks), toks[1], toks[0]))
    data = container(toks[:toks[1]])
    d3dc = ctypes.WinDLL('d3dcompiler_47.dll')
    blob = ctypes.c_void_p()
    hr = d3dc.D3DDisassemble(ctypes.c_char_p(data), ctypes.c_size_t(len(data)), 0, None, ctypes.byref(blob))
    if hr != 0:
        raise SystemExit('D3DDisassemble hr 0x%08x' % (hr & 0xffffffff))
    vtbl = ctypes.cast(ctypes.cast(blob, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    getptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtbl[3])
    getsize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtbl[4])
    p, n = getptr(blob), getsize(blob)
    sys.stdout.write(ctypes.string_at(p, n).decode('ascii', 'replace'))


main()
