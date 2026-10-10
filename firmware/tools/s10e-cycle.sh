#!/bin/bash
# deploy $1 (signed sys) into the phone's Windows, run one Windows attempt + saving boot, decode WINPRAM into $2
export MSYS_NO_PATHCONV=1
A=/c/Users/ilya/code/platform-tools/adb.exe
SYS="$1"; OUT="$2"; mkdir -p "$OUT"
until $A devices | grep -q recovery; do sleep 5; done
if [ -n "$SYS" ]; then
  $A push "$(cygpath -w "$SYS")" /tmp/Exynos9820Ufs.sys >/dev/null
  $A shell 'mkdir -p /tmp/win; umount /data 2>/dev/null; mount.ntfs /dev/block/by-name/userdata /tmp/win >/dev/null 2>&1; F=/tmp/win/Windows/System32/DriverStore/FileRepository/exynos9820ufs.inf_arm64_4efdc4525d11d997/Exynos9820Ufs.sys; cp /tmp/Exynos9820Ufs.sys $F && sync && echo deployed $(sha256sum $F | cut -c1-16); umount /tmp/win'
fi
$A shell 'mkdir -p /tmp/esp; mount -t vfat /dev/block/by-name/cache /tmp/esp 2>/dev/null; rm -f /tmp/esp/WINRUN.FLG; umount /tmp/esp'
T0=$(date +%s); $A reboot; sleep 40
until $A devices | grep -q recovery; do sleep 5; if [ $(( $(date +%s) - T0 )) -gt 3600 ]; then echo "STAGE1 TIMEOUT (no TWRP after 3600 s)"; exit 2; fi; done
echo "stage1 TWRP after $(( $(date +%s) - T0 )) s"
$A shell 'mkdir -p /tmp/esp; mount -t vfat /dev/block/by-name/cache /tmp/esp 2>/dev/null; ls /tmp/esp | tr "\n" " "; echo; umount /tmp/esp'
T1=$(date +%s); $A reboot; sleep 30
until $A devices | grep -q recovery; do sleep 5; if [ $(( $(date +%s) - T1 )) -gt 400 ]; then echo "STAGE2 TIMEOUT"; exit 3; fi; done
echo "stage2 TWRP after $(( $(date +%s) - T1 )) s"
$A shell 'mkdir -p /tmp/esp; mount -t vfat /dev/block/by-name/cache /tmp/esp 2>/dev/null; ls -la /tmp/esp | grep -v recovery'
$A pull /tmp/esp/UEFILOG.TXT "$(cygpath -w "$OUT/UEFILOG.txt")" >/dev/null
$A pull /tmp/esp/WINPRAM.BIN "$(cygpath -w "$OUT/WINPRAM.BIN")" >/dev/null
$A shell 'umount /tmp/esp'
head -1 "$OUT/UEFILOG.txt"
PYTHONIOENCODING=utf-8 python - "$OUT/WINPRAM.BIN" <<'PY'
import sys,struct,re
d=open(sys.argv[1],'rb').read()
e=d[0x8000:0x9000]; c=struct.unpack_from('<I',e,4)[0]
clean=lambda b: re.sub(rb'[^\x20-\x7e]',b'?',b).decode()
print('early magic %08x count %d'%(struct.unpack_from('<I',e,0)[0],c))
for i in range(min(c,63)): print(clean(e[64+i*64:64+i*64+64].split(b'\n')[0]))
PY
