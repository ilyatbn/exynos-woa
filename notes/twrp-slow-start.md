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
