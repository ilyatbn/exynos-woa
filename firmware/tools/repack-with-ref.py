# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Wrap a [shim][FD] kernel in a container this phone's sboot already accepts.

Called by pack-uefi.ps1:121 (`python tools/repack-with-ref.py --ref R --kernel K
--output O`). It is device-agnostic: nothing here knows a page size, a partition
size or a phone name. Every layout fact is read from the reference image:

* `page_size` comes from the ref's own header word at +0x24 (bootimg.py:51).
  The S10e's containers say 2048 (notes/m8-uefi-image.md Step 16, read off
  /dev/block/by-name/recovery = sda15 and /dev/block/by-name/boot = sda14);
  hardcoding 4096 would misplace every section.
* `kernel_size`/`ramdisk_size`/`second_size`/`dt_size` are rewritten by
  BootImage.serialize (bootimg.py:105-122); the SHA-1 id at +0x240 is refreshed.
* The ref's ramdisk, second stage, DTBH blob and `tail` are carried verbatim,
  and the header page itself is carried verbatim except for the four size
  words and the refreshed SHA-1 id, which is what keeps sboot's acceptance of
  the container unchanged.

The reference container is the phone's own partition (notes/m8-uefi-image.md
Step 16: the recovery slot's kernel is byte-for-byte the TWRP kernel this
device booted), so acceptance is device-proven, not downloaded.

Still open after this tool runs (recorded, not guessed): where sboot drops the
kernel (K4b), and whether the ref's `kernel_addr` word at +0x0c matters to it.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_here = Path(__file__).resolve()
for _cand in (_here.parent.parent / "tools" / "twrp-winre",      # repo root: tools/twrp-winre
              _here.parent.parent.parent / "tools" / "twrp-winre"):  # firmware/tools/...
    if (_cand / "bootimg.py").exists():
        sys.path.insert(0, str(_cand))
        break
else:
    raise SystemExit("bootimg.py not found - expected at tools/twrp-winre/bootimg.py")

from bootimg import BOOT_MAGIC, BootImage


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ref", required=True, help="container this sboot already accepts")
    ap.add_argument("--kernel", required=True, help="cat(shim, FD) blob to put in the kernel slot")
    ap.add_argument("--output", required=True, help="image to write")
    ap.add_argument("--no-refresh-id", action="store_true",
                    help="leave the ref's SHA-1 id alone (default: refresh it)")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be written, write nothing")
    args = ap.parse_args()

    ref_blob = Path(args.ref).read_bytes()
    if ref_blob[:8] != BOOT_MAGIC:
        raise SystemExit(f"ref {args.ref} is not an Android boot image: {ref_blob[:8]!r}")
    img = BootImage.parse(ref_blob)
    page = img.page_size
    if page == 0 or page & (page - 1):
        raise SystemExit(f"ref page_size 0x{page} is not a power of two")

    kernel = Path(args.kernel).read_bytes()
    if len(kernel) > 0xFFFFFFFF:
        raise SystemExit("kernel_size does not fit the header's 32-bit word")

    before = {
        "ramdisk": img.ramdisk, "second": img.second, "dt": img.dt, "tail": img.tail,
    }
    old_kernel = len(img.kernel)
    img.kernel = kernel
    out = img.serialize(refresh_id=not args.no_refresh_id, keep_tail=True)

    # Nothing but the kernel slot and the five header words may change.
    reparsed = BootImage.parse(out)
    for name, old in before.items():
        if getattr(reparsed, name) != old:
            raise SystemExit(f"round-trip broke the {name} section - refusing to write")
    if reparsed.page_size != page:
        raise SystemExit("round-trip changed page_size")

    if not args.dry_run:
        Path(args.output).write_bytes(out)

    print(f"ref            {args.ref}  page_size={page}")
    print(f"kernel slot    {old_kernel} -> {len(kernel)} bytes")
    print(f"ramdisk        {len(reparsed.ramdisk)} (verbatim)")
    print(f"second         {len(reparsed.second)} (verbatim)")
    print(f"dt             {len(reparsed.dt)} (verbatim)")
    print(f"tail           {len(reparsed.tail)} (verbatim)")
    print(f"output         {len(out)} bytes"
          + ("  (dry run, nothing written)" if args.dry_run else f"  -> {args.output}"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
