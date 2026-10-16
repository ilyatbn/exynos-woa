#!/bin/bash
# wddm2dbg.sh CMD [args] - PC-side driver for the MaliG76Wddm / WDDM 2.0 debugging loop over SSH (host alias s10e).
# Every phone-side script lives next to this file and is pushed to C:\S10eTmp by `push`. See README.md here.
#
#   push                 copy all phone-side scripts to C:\S10eTmp (run once per fresh Windows install / after edits)
#   ready [MAXS]         wait until SSH answers and explorer.exe runs
#   check                adapter status, Wddm2 value, KMD trace failures, DWM/explorer, recent crashes, dumps
#   hb [N]               the KMD's PRAM heartbeat line N times, 4 s apart (isr, dpc, vsync, submits, presents, fence)
#   fb [NAME]            dump the scan-out (base from DPU DMA G0 +0x40) to scratch NAME.png (pitch 4352)
#   idma                 DPU DMA G0 registers 0x19071000..0x1907105F (base at +0x40)
#   winpram [NAME]       copy S:\WINPRAM.BIN (firmware dump after an unclean reset): KMD trace + S10eDiag ring
#   kmdswap SYS          swap a built MaliG76Wddm.sys into the DriverStore (SYSTEM task), print the new hash
#   umdswap DIR          stage maliumd.dll + libgallium_d3d10.dll from DIR into System32 (old ones kept as .prev)
#   probe ARGS           probe.ps1 ARGS (volatile Parameters\Probe + pnputil /restart-device), e.g. probe -Wddm2 3
#   unprobe              delete the probe key and restart the device (back to the persistent setting)
#   set N [reboot]       persistent Parameters\Wddm2 = N (0 = WDDM 1.3, 3 = WDDM 2.0), optionally reboot
#   umdtrace [-Off]      machine-wide UMD logging on (off) - DWM picks it up at the next boot
#   umdlog [EXE] [N]     newest malikm log of EXE (default dwm)
#   wttest [-Wait N]     start Windows Terminal in the user session, alive after N s? crashes?
#   bc                   Kernel-Power 41 / 1001 / 6008 events and dump files
#   crashes              Application Error 1000 events of the last 20 min (module + offset)
#   dwmdump NAME         fetch C:\S10eTmp\dumps\NAME and print .ecxr + stack with cdb and public symbols
set -u
H=$(cd "$(dirname "$0")" && pwd)
OUT=${WDDM2DBG_OUT:-$HOME/wddm2dbg}
mkdir -p "$OUT"
REPO=$(cd "$H/../../.." && pwd)
PS='powershell -NoProfile -ExecutionPolicy Bypass -File'
r() { timeout "${T:-120}" ssh s10e "$@" < /dev/null; }
task() {  # task NAME CMDFILE - run a C:\S10eTmp cmd file once as SYSTEM
  r "schtasks /create /tn $1 /tr C:\\S10eTmp\\$2 /sc once /st 23:59 /ru SYSTEM /f >nul & schtasks /run /tn $1 >nul & ping -n ${3:-8} 127.0.0.1 >nul"
}
cmd=${1:-help}; shift || true
case $cmd in
push)
  for f in "$H"/*.ps1 "$H"/*.cmd; do
    case $f in *.cmd) sed 's/\r*$/\r/' "$f" > "$OUT/$(basename "$f")"; f="$OUT/$(basename "$f")";; esac
    scp -q -O "$f" s10e:C:/S10eTmp/ < /dev/null || exit 1
  done
  scp -q -O "$REPO/tools/phone/runuser2.ps1" s10e:C:/S10eTmp/ < /dev/null; echo pushed;;
check) r "$PS C:\\S10eTmp\\w2check.ps1";;
ready) t0=$(date +%s)   # wait (max ${1:-600} s) until SSH answers and explorer.exe runs
    until T=10 r 'tasklist /fi "imagename eq explorer.exe" /nh' 2>/dev/null | grep -qi explorer; do
      [ $(( $(date +%s) - t0 )) -ge "${1:-600}" ] && { echo "not ready after ${1:-600} s"; exit 1; }
      sleep 10; done; echo "ready after $(( $(date +%s) - t0 )) s";;
hb) for i in $(seq 1 "${1:-1}"); do
      r 'C:\S10eTmp\s10ediagx.exe dumpmmio 0x8FE04000 128 C:\S10eTmp\hb.bin >nul & type C:\S10eTmp\hb.bin' | tr -c '[:print:]' '\n' | grep HB
      [ "$i" -lt "${1:-1}" ] && sleep 4; done;;
idma) r 'C:\S10eTmp\s10ediagx.exe dumpmmio 0x19071000 0x60 C:\S10eTmp\idma.bin >nul'
      scp -q -O s10e:C:/S10eTmp/idma.bin "$OUT/" < /dev/null && od -A x -t x4 "$OUT/idma.bin";;
fb) n=${1:-fb}
    r 'C:\S10eTmp\s10ediagx.exe dumpmmio 0x19071040 4 C:\S10eTmp\base.bin >nul'
    scp -q -O s10e:C:/S10eTmp/base.bin "$OUT/" < /dev/null
    base=$(od -A n -t x4 "$OUT/base.bin" | tr -d ' ')
    echo "scan-out base 0x$base"
    T=200 r "C:\\S10eTmp\\s10ediagx.exe dumpmmio 0x$base 9922560 C:\\S10eTmp\\$n.bin" > /dev/null
    scp -q -O "s10e:C:/S10eTmp/$n.bin" "$OUT/" < /dev/null && (cd "$OUT" && python "$REPO/tools/phone/fb2png.py" "$n.bin" 4352 >/dev/null \
      && python -c "from PIL import Image; im=Image.open('$n.png'); print('$OUT/$n.png extrema', im.getextrema())");;
winpram) n=${1:-winpram}; task espget espget.cmd 6
    scp -q -O s10e:C:/S10eTmp/WINPRAM.BIN "$OUT/$n.bin" < /dev/null || exit 1
    echo "--- S10eDiag ring (bugcheck lines: BC ...)"; dd if="$OUT/$n.bin" bs=4096 skip=10 count=4 2>/dev/null | tr -c '[:print:]' '\n' | grep -v '^$' | sort | tail -12
    echo "--- KMD trace, newest rolling lines"
    python -I - "$OUT/$n.bin" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()[0x4000:0x8000]
lines = [d[i*64:(i+1)*64].split(b'\0')[0].decode('latin1').strip() for i in range(112, 256)]
ent = []
for l in lines:
    p = l.split()
    if len(p) >= 2:
        try: ent.append((int(p[1], 16), l))
        except ValueError: pass
for s, l in sorted(ent)[-40:]: print(l[:90])
EOF
    ;;
kmdswap) sys=${1:?kmdswap SYS}; scp -q -O "$sys" s10e:C:/S10eTmp/MaliG76Wddm-new.sys < /dev/null || exit 1
    sed 's/\r*$/\r/' "$H/kmdswap.cmd" > "$OUT/kmdswap.cmd"; scp -q -O "$OUT/kmdswap.cmd" s10e:C:/S10eTmp/ < /dev/null
    task kmdswap kmdswap.cmd 6; r 'type C:\S10eTmp\kmdswap.log' | grep -E '^[0-9a-f]{64}'; sha256sum "$sys" | cut -c1-64;;
umdswap) d=${1:?umdswap DIR}; r 'mkdir C:\S10eTmp\umd 2>nul' >/dev/null
    scp -q -O "$d/maliumd.dll" "$d/libgallium_d3d10.dll" s10e:C:/S10eTmp/umd/ < /dev/null || exit 1
    task umdswap umdswap.cmd 8; r 'type C:\S10eTmp\umdswap.log' | grep -E '^[0-9a-f]{64}|denied|cannot'; sha256sum "$d/maliumd.dll" | cut -c1-64;;
probe) r "$PS C:\\S10eTmp\\probe.ps1 $*";;
unprobe) r "$PS C:\\S10eTmp\\unprobe.ps1";;
set) if [ "${2:-}" = reboot ]; then r "$PS C:\\S10eTmp\\w2set.ps1 -Wddm2 ${1:?set N} -Reboot"; else r "$PS C:\\S10eTmp\\w2set.ps1 -Wddm2 ${1:?set N}"; fi;;
umdtrace) r "$PS C:\S10eTmp\umdtrace.ps1 $*";;
umdlog) r "$PS C:\\S10eTmp\\umdlog.ps1 -Exe ${1:-dwm} -Tail ${2:-40}";;
bc) r "$PS C:\\S10eTmp\\bc.ps1";;
wttest) T=150 r "$PS C:\S10eTmp\wttest.ps1 $*";;
crashes) r "$PS C:\\S10eTmp\\crashq.ps1";;
dwmdump) n=${1:?dwmdump NAME}; scp -q -O "s10e:C:/S10eTmp/dumps/$n" "$OUT/" < /dev/null || exit 1
    "/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe" -z "$(cygpath -w "$OUT/$n")" \
      -y 'srv*C:\symbols*https://msdl.microsoft.com/download/symbols' -c '.ecxr; kc 30; q' 2>&1 | sed -n '/Reading initial command/,/^quit:/p';;
*) sed -n '2,25p' "$0";;
esac
