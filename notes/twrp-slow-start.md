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
