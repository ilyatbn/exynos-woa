#!/bin/bash
# m4mb.sh [GICC...] - read the park mailboxes of the given GICC indices (default 6 7 = the M4s) from a running Windows
# through S10eDiag's uncached MMIO dump, and decode the flag-6/7 counters (notes/m4-cores.md). Mailbox of gicc n =
# 0xBA240000 + (n-1)*0x1000 in every run so far - check UEFILOG "PARK: N page(s) at". Run from the repo root.
set -u
[ $# -eq 0 ] && set -- 6 7
cmd=""
for n in "$@"; do
  cmd="$cmd C:\\S10eTmp\\s10ediagx.exe dumpmmio $(printf '0x%X' $((0xBA240000 + (n - 1) * 0x1000))) 4096 C:\\S10eTmp\\mb$n.bin >nul &"
done
ssh s10e "${cmd% &}" < /dev/null
d=$(mktemp -d)
for n in "$@"; do scp -O -q "s10e:C:/S10eTmp/mb$n.bin" "$d/" < /dev/null || exit 1; done
for n in "$@"; do
python - "$d/mb$n.bin" "$n" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
q = lambda o: struct.unpack_from('<Q', d, o)[0]
w = lambda o: struct.unpack_from('<I', d, o)[0]
print("gicc%s flag %x passes %d atomics %d timer %d sgi %d other %d lastiar %x spurious %d faults %d esr %x pmu stage %d pmccntr %d" % (
    sys.argv[2], w(0xFB8), q(0x340), q(0x348), q(0x350), q(0x358), q(0x360), q(0x368), q(0x370), q(0x618), q(0x608),
    w(0x3C0), q(0x3F0)), "setup stage", w(0x3F8))
PY
done
rm -rf "$d"
