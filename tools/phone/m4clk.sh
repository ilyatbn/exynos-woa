#!/bin/bash
# m4clk.sh [GICC...] - real clock of parked, not-offered cores running stub flag 7 with option bit 0 (PMU, no WFI):
# two mailbox reads (s10ediagx dumpmmio) ~3 s apart, clock = d(PMCCNTR @+0x3F0) / d(CNTVCT @+0x3F8) x 26 MHz.
# notes/m4-cores.md (V/F lead). Run from the repo root.
set -u
[ $# -eq 0 ] && set -- 6 7
S=/c/Users/ilya/AppData/Local/Temp/m4clk; mkdir -p $S
for pass in 1 2; do
  cmd=""
  for n in "$@"; do cmd="$cmd C:\\S10eTmp\\s10ediagx.exe dumpmmio $(printf '0x%X' $((0xBA240000 + (n - 1) * 0x1000))) 4096 C:\\S10eTmp\\clk$n-$pass.bin >nul &"; done
  ssh s10e "${cmd% &}" < /dev/null
  [ $pass = 1 ] && sleep 3
done
for n in "$@"; do for pass in 1 2; do scp -O -q "s10e:C:/S10eTmp/clk$n-$pass.bin" "$S/" < /dev/null; done
python - "$(cygpath -m $S)/clk$n-1.bin" "$(cygpath -m $S)/clk$n-2.bin" "$n" <<'PY'
import struct, sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
q = lambda d, o: struct.unpack_from('<Q', d, o)[0]
dc = q(b, 0x3F0) - q(a, 0x3F0); dt = q(b, 0x3F8) - q(a, 0x3F8)
print("gicc%s: %d cycles in %d ticks (%.3f s) -> %.0f MHz" % (sys.argv[3], dc, dt, dt / 26e6, dc / dt * 26.0 if dt else 0))
PY
done
