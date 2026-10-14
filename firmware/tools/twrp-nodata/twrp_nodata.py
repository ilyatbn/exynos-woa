"""twrp_nodata.py - patch a TWRP recovery image so TWRP does not auto-mount the Windows partition at start.

Why (notes/twrp-slow-start.md, 2026-10-14): TWRP mounts /data (= by-name/userdata = the Windows C: volume on this
port) through ntfs-3g and walks every file to compute a backup size - 138 s per start with a 41 GB C:, growing.

What it does: unpacks an Android boot image (header v0/v1/v2), opens the ramdisk (gzip newc cpio), and in every TWRP
fstab it finds (etc/twrp.fstab, etc/recovery.fstab, twres/... not touched) comments out the line(s) whose mount point
is /data. Everything else - kernel, second, recovery DTBO, DTB, header fields, data after the last section (Samsung's
SEANDROIDENFORCE / AVB footer) - is copied byte for byte; only ramdisk_size, the offsets after it and the SHA-1 id are
recomputed as mkbootimg does. Generic: works for any TWRP whose fstab names /data.

    python twrp_nodata.py IN.img OUT.img [--list]      (--list: print the fstab files and their /data lines only)
"""
import gzip
import hashlib
import io
import lzma
import struct
import sys

FSTABS = ('etc/twrp.fstab', 'etc/recovery.fstab', 'system/etc/twrp.fstab', 'system/etc/recovery.fstab')


def pad(n, page):
    return (n + page - 1) // page * page


def parse(img):
    if img[:8] != b'ANDROID!':
        raise SystemExit('not an Android boot image')
    (ksize, kaddr, rsize, raddr, ssize, saddr, tags, page, hver, osver) = struct.unpack_from('<10I', img, 8)
    h = dict(ksize=ksize, rsize=rsize, ssize=ssize, page=page, hver=hver)
    off = 8 + 40 + 16 + 512 + 32 + 1024           # after name, cmdline, id, extra_cmdline
    h['id_off'] = 8 + 40 + 16 + 512
    if hver >= 1:
        h['dtbo_size'], h['dtbo_off'], h['hdr_size'] = struct.unpack_from('<IQI', img, off)
        h['dtbo_field'] = off
    else:
        h['dtbo_size'] = 0
    if hver >= 2:
        h['dtb_size'], h['dtb_addr'] = struct.unpack_from('<IQ', img, off + 16)
        h['dtb_field'] = off + 16
    else:
        h['dtb_size'] = 0
    if hver > 2:
        raise SystemExit('header v%d not handled (v0-v2 only)' % hver)
    p = page
    o = p
    h['kernel'] = img[o:o + ksize]; o += pad(ksize, p)
    h['ramdisk'] = img[o:o + rsize]; o += pad(rsize, p)
    h['second'] = img[o:o + ssize]; o += pad(ssize, p)
    h['dtbo'] = img[o:o + h['dtbo_size']]
    if h['dtbo_size']:
        if hver >= 1 and h['dtbo_off'] != o:
            raise SystemExit('recovery_dtbo_offset 0x%x != computed 0x%x' % (h['dtbo_off'], o))
        o += pad(h['dtbo_size'], p)
    h['dtb'] = img[o:o + h['dtb_size']]; o += pad(h['dtb_size'], p)
    h['end'] = o
    h['tail'] = img[o:]
    return h


def cpio_entries(data):
    """newc cpio -> list of (header_fields:list[str], name:bytes, body:bytes)."""
    out = []
    i = 0
    while True:
        if data[i:i + 6] != b'070701':
            raise SystemExit('ramdisk is not newc cpio at 0x%x' % i)
        f = [data[i + 6 + 8 * k:i + 14 + 8 * k] for k in range(13)]
        namesize = int(f[11], 16)
        filesize = int(f[6], 16)
        name = data[i + 110:i + 110 + namesize]
        j = (i + 110 + namesize + 3) & ~3
        body = data[j:j + filesize]
        i = (j + filesize + 3) & ~3
        out.append([f, name, body])
        if name[:-1] == b'TRAILER!!!':
            return out, data[i:]


def cpio_pack(entries, trailer_pad):
    b = bytearray()
    for f, name, body in entries:
        f = list(f)
        f[6] = b'%08X' % len(body)
        b += b'070701' + b''.join(f) + name
        b += b'\0' * (((len(b) + 3) & ~3) - len(b))
        b += body
        b += b'\0' * (((len(b) + 3) & ~3) - len(b))
    return bytes(b) + trailer_pad


def patch_fstab(text):
    lines = text.split(b'\n')
    changed = []
    for n, l in enumerate(lines):
        s = l.strip()
        if not s or s.startswith(b'#'):
            continue
        fields = s.split()
        # twrp.fstab: "/data ext4 /dev/..." ; recovery.fstab (fs_mgr): "/dev/... /data ext4 ..."
        if fields[0] == b'/data' or (len(fields) > 1 and fields[1] == b'/data'):
            lines[n] = b'#exynos-woa: TWRP must not auto-mount the Windows partition# ' + l
            changed.append(l)
    return b'\n'.join(lines), changed


def main():
    src, dst = sys.argv[1], sys.argv[2]
    listing = '--list' in sys.argv
    img = open(src, 'rb').read()
    h = parse(img)
    raw = h['ramdisk']
    if raw[:2] == b'\x1f\x8b':
        kind = 'gzip'
        cpio = gzip.decompress(raw)
    elif raw[0] == 0x5D:
        kind = 'lzma'           # legacy .lzma (LZMA_Alone): props 0x5D (lc 3, lp 0, pb 2) + dict size + 64-bit size
        dict_size = struct.unpack_from('<I', raw, 1)[0]
        cpio = lzma.decompress(raw, format=lzma.FORMAT_ALONE)
    else:
        raise SystemExit('ramdisk is neither gzip nor lzma (magic %s) - extend the script' % raw[:6].hex())
    print('ramdisk: %s, %d -> %d bytes' % (kind, len(raw), len(cpio)))
    entries, trailer_pad = cpio_entries(cpio)
    total = 0
    for e in entries:
        name = e[1][:-1].decode('utf-8', 'replace')
        if name in FSTABS:
            new, changed = patch_fstab(e[2])
            print('%s: %d /data line(s)' % (name, len(changed)))
            for c in changed:
                print('    ' + c.decode('utf-8', 'replace'))
            if not listing:
                e[2] = new
            total += len(changed)
    if listing:
        return
    if total == 0:
        raise SystemExit('no /data line found - nothing written')
    packed = cpio_pack(entries, trailer_pad)
    if kind == 'gzip':
        buf = io.BytesIO()
        with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0) as g:
            g.write(packed)
        ramdisk = buf.getvalue()
    else:
        ramdisk = bytearray(lzma.compress(packed, format=lzma.FORMAT_ALONE,
                                          filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': dict_size,
                                                    'lc': 3, 'lp': 0, 'pb': 2}]))
        # Header size field stays "unknown" (-1) with an end marker - the same form as the original TWRP ramdisk
        # (5d 00008000 ffffffffffffffff), which this kernel already decompresses.
        if raw[5:13] != b'\xff' * 8:
            raise SystemExit('original lzma header carries an explicit size - not handled')
        ramdisk = bytes(ramdisk)
        if lzma.decompress(ramdisk, format=lzma.FORMAT_ALONE) != packed:
            raise SystemExit('lzma round trip failed')

    p = h['page']
    out = bytearray(img[:p])
    struct.pack_into('<I', out, 16, len(ramdisk))           # ramdisk_size
    body = bytearray()
    body += h['kernel'] + b'\0' * (pad(len(h['kernel']), p) - len(h['kernel']))
    body += ramdisk + b'\0' * (pad(len(ramdisk), p) - len(ramdisk))
    body += h['second'] + b'\0' * (pad(len(h['second']), p) - len(h['second']))
    if h['dtbo_size']:
        struct.pack_into('<Q', out, h['dtbo_field'] + 4, p + len(body))
        body += h['dtbo'] + b'\0' * (pad(len(h['dtbo']), p) - len(h['dtbo']))
    if h['dtb_size']:
        body += h['dtb'] + b'\0' * (pad(len(h['dtb']), p) - len(h['dtb']))
    # mkbootimg id: SHA-1 over each section followed by its size (v1 adds recovery_dtbo, v2 adds dtb)
    sha = hashlib.sha1()
    for part in (h['kernel'], ramdisk, h['second']):
        sha.update(part)
        sha.update(struct.pack('<I', len(part)))
    if h['hver'] >= 1:
        sha.update(h['dtbo'])
        sha.update(struct.pack('<I', len(h['dtbo'])))
    if h['hver'] >= 2:
        sha.update(h['dtb'])
        sha.update(struct.pack('<I', len(h['dtb'])))
    out[h['id_off']:h['id_off'] + 32] = sha.digest() + b'\0' * 12
    out += body
    if h['tail'].strip(b'\0'):
        out += h['tail']                    # a signature/footer after the sections: keep it as it was
    elif len(out) <= len(img):
        out += b'\0' * (len(img) - len(out))   # a whole-partition dump: pad back to the partition size
    else:
        raise SystemExit('patched image (%d) is larger than the input partition dump (%d)' % (len(out), len(img)))
    open(dst, 'wb').write(out)
    print('ramdisk %d -> %d bytes; image %d -> %d bytes; tail %d bytes kept' %
          (len(raw), len(ramdisk), len(img), len(out), len(h['tail'])))


if __name__ == '__main__':
    main()
