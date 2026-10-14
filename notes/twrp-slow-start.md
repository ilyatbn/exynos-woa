# TWRP takes ~70 s on the logo (2026-10-13)

## Progress log
- User: TWRP sits on its logo ~60-70 s (since Windows has been in use).
- Logs: s10e-artifacts/runs/twrp-slow (dmesg incl. /proc/uptime 112.9 s at read, /tmp/recovery.log).
- recovery.log: TWRP's fstab entry '/data' = by-name/userdata, "Setting up '/data' as data/media emulated
  storage", `/sbin/mount.ntfs .../userdata /data` -> "Metadata kept in Windows cache, refused to mount" ->
  read-only fallback (dirty journal after a hard reset), then it walks the whole Windows tree (storage size scan):
  hundreds of "Error opening: '/data/Windows/CbsTemp/...'" / "Unable to stat '/data/S10eTmp/gfxperf.txt' (I/O
  error)" lines.
- dmesg: recovery service starts at 7.7 s; mount.ntfs (pid 4878) still busy at 51.8 s; sda25 (EXT4) unmounted by
  recovery at 74.8 s = end of TWRP's startup. Kernel itself is up by ~7 s.
- Cause: TWRP's auto-mount + size scan of the Windows C: volume over ntfs-3g FUSE; grows with the number of files
  on C: (Windows Update CbsTemp). Not related to our UEFI image.
- Fix would need a TWRP build without the userdata auto-mount = writing the recovery partition, which the ground
  rules exclude (only boot is written). Not done; asked the user.

## 2026-10-14 ~20:10 - re-measured (user: "twrp is taking forever ... drop everything and figure out the cause")
- flushreset (hard reset) -> TWRP on adb: 133 s wall. Logs s10e-artifacts/runs/twrp-slow2 (dmesg at uptime 151.8 s,
  recovery.log). Kernel: initramfs unpack 0.9-6.4 s, init starts 'recovery' at 10.5 s; no kernel gap > 6 s afterwards
  (only periodic SSP/fingerprint debug prints). TWRP's startup ends with the sda25 umount at 149.0 s -> ~138 s inside
  TWRP (was ~65 s on 2026-10-13).
- recovery.log: /data = by-name/userdata = the Windows C: volume, mounted with /sbin/mount.ntfs (ntfs-3g FUSE): "unclean
  file system ... Metadata kept in Windows cache" -> read-only fallback (every flushreset/hard reset leaves it dirty),
  then "Data backup size is 41393MB" (was 30987 MB yesterday, +10 GB: KB5068865, Store apps, Brave) = TWRP walks the
  whole C: tree through FUSE to compute the backup size. Time scales with the number of files on C: -> 65 s -> 138 s.
  Also mounts a 128 GB microSD (exFAT, 51.7 GB used, "not unmounted cleanly") - statfs only.
- Not our firmware, not the kernel. Fix options:
  A. Patch TWRP's fstab (recovery ramdisk) so /data is not auto-mounted - removes the walk entirely; our deploy scripts
     mount userdata themselves (kmd-deploy.sh/umd-deploy.sh mount.ntfs /tmp/win). Needs a recovery-partition write
     (ground rules: only boot is written) -> user decision. Rollback: Odin/download mode re-flash of the current TWRP.
  B. Shrink C: (DISM /StartComponentCleanup, SoftwareDistribution\Download, CbsTemp) - partial, grows back.
  C. Avoid TWRP for routine work: flash by-name/boot from Windows over SSH (raw write of the boot partition's LBA range on
     the UFS disk) - TWRP only for emergencies. Also a ground-rule change (boot written from Windows instead of TWRP).
- **User chose A (2026-10-14 ~20:15, "do it"); ground-rule exception: the recovery partition is written once.**
  Backup: s10e-artifacts/twrp/recovery-orig.img = dd of by-name/recovery (sda15, 67 633 152 B) sha256 f101bf73...
  Tool firmware/tools/twrp-nodata/twrp_nodata.py (generic for TWRP images, header v0-v2, gzip or legacy-lzma ramdisk):
  comments out the /data line(s) of etc/recovery.fstab / etc/twrp.fstab, recompresses the ramdisk the same way
  (lzma: props 5d, dict 8 MiB, size field -1 + end marker as the original), recomputes ramdisk_size / recovery_dtbo_offset
  / SHA-1 id, keeps everything else byte for byte, pads to the partition size. Self-checks: round-trip decompress,
  re-parse. Result recovery-nodata.img sha256 c7b198c0...: kernel + recovery DTBO identical, header bytes identical
  except ramdisk size/id, ramdisk 10 905 981 -> 10 907 856 B, 3546/3546 cpio entries identical except
  etc/recovery.fstab (its /data line now "#exynos-woa: ... # /data ext4 .../by-name/userdata ...").
  Rollback: dd recovery-orig.img to by-name/recovery (from Windows-free TWRP or Odin).
- **Result (20:21):** flashed recovery-nodata.img to by-name/recovery (readback of the whole partition = c7b198c0...).
  `adb reboot recovery` -> TWRP on adb in **36 s wall** (incl. sboot + its warning; was 133 s from reset), uptime 19.5 s
  at the first adb read; init starts 'recovery' at 7.7 s. recovery.log: no /data setup, no mount.ntfs, no "Data backup
  size"; "Settings storage is '/external_sd'" (TWRP settings now on the microSD). Logs runs/twrp-fast.
- Side effects: TWRP's "Internal Storage"/MTP and Data backup/wipe of userdata are gone (they were the Windows volume
  anyway); our deploy scripts mount userdata themselves (mount.ntfs to /tmp/win) and are unaffected.
- Future phones: run twrp_nodata.py on that phone's TWRP image (dump by-name/recovery first, keep it as the rollback).
