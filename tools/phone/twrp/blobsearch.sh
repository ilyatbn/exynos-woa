#!/sbin/sh
# blobsearch.sh PATTERN - (TWRP, adb push + sh) read-only search for a firmware blob: file names containing PATTERN on
# the mounted filesystems, then the raw string (case-insensitive) in the UFS LUNs' small partitions and boot LUNs
# (sdb/sdc/sdd...; userdata/system/vendor/product/cache skipped - too big). Prints device + number of matching
# "lines" (TWRP's BusyBox grep has no -b: pull a hit device and search it on the PC for offsets).
P=${1:?pattern}
echo "--- file names (*$P*)"
find / -xdev -iname "*$P*" 2>/dev/null | head -20
for m in /system_root /vendor /system /product; do [ -d "$m" ] && find "$m" -iname "*$P*" 2>/dev/null | head -10; done
echo "--- raw string '$P' in block devices"
for dev in /dev/block/sd[a-z] /dev/block/sd[a-z][0-9]*; do
  n=${dev##*/}
  case "$n" in sda) continue;; esac          # whole LUN 0 = the big partitions below
  sz=$(cat /sys/class/block/$n/size 2>/dev/null); [ -z "$sz" ] && continue
  [ "$sz" -gt 409600 ] && continue           # > 200 MB: system/userdata/... skipped
  hits=$(grep -a -i -c "$P" "$dev" 2>/dev/null)   # BusyBox grep: no -b; pull a hit device for offsets
  [ "${hits:-0}" -gt 0 ] && echo "$dev ($((sz / 2)) KB): $hits matching lines"
done
echo "--- done"
