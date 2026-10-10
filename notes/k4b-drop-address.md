# K4b — where does sboot drop the kernel on the S10e? (the FD run address)

Started: 2026-10-10. Device-read task. Phone `RF8M21HYDSZ` in TWRP (root).
Reads only; nothing written to the phone.

## Why this file exists

`UEFI_BASE` / `FD_BASE_ADDRESS` = `0x80080000` has been marked UNVERIFIED
everywhere because "the real sboot kernel-drop address is **not** closable by a
cheap read" (HANDOFF §14/§15/§16). It IS closable by a cheap read after all:
`/proc/iomem` in a TWRP root shell names the physical extents the **kernel that
sboot just booted** occupies, and the local `Dumps/twrp_device_tree/kernel`
artefact is that same kernel's `Image` header.

## Progress log

### K01 — `/proc/iomem` on the phone (TWRP root)

```
80000000-baafffff : System RAM
c0000000-dfffffff : System RAM
e1900000-ffffffff : System RAM
880000000-97fffffff : System RAM
...
80198000-81717fff : Kernel code
825c8000-832cffff : Kernel data
```

* **FACT: bank0 is `0x80000000..0xBAAFFFFF`** — exactly `0x3AB00000` long, which
  independently confirms the `memory@80000000` decode in `notes/K10-rkp.md` T18
  and the clamp added to `PlatformMemoryMapLib.c`.
* **FACT: this boot's kernel occupies `0x80198000..0x832CFFFF`** (`_text` start
  to data end).

### K02 — the local `Image` header of that same kernel

`C:\Users\ilya\code\Dumps\twrp_device_tree\kernel`, 44 363 788 B, first 64 bytes:

```
00 00 56 14 | 00 00 00 00 | 00 00 08 00 00 00 00 00 | 00 80 13 03 00 00 00 00 | 0a 00 00 00 00 00 00 00 | ... | 41 52 4d 64
```

Decoded little-endian (ARM64 Image header): `code0 = 0x14560000` (a `B`),
**`text_offset = 0x80000`**, **`image_size = 0x3138000`**, **`flags = 0xa`**,
magic `ARM\x64` at +56.

### K03 — the arithmetic that closes K4b

`_text` (`0x80198000`) − `text_offset` (`0x80000`) = **`0x80118000`** = the
physical address sboot loaded this boot's `Image` at.

Cross-check: `0x80198000 + image_size (0x3138000) = 0x832D0000`, and `Kernel data`
ends at `0x832CFFFF` — **exact**. So the header's `image_size` is measured from
`_text`, and the whole `Image` spans `0x80118000 .. 0x80118000 + 0x3138000 =
0x83250000` (header-relative) with the kernel's sections reaching `0x832D0000`.

**INFERENCE (mine), and the part that is NOT a read:** sboot picked `0x80118000`
for *this* boot. It is neither `0x80080000` nor `0x80100000`; it is `0x80100000`
(the live `kaslr` node base, `notes/K10-rkp.md` T16) plus `0x18000`. Two
explanations fit and neither is yet tested: (a) sboot loads at the `kaslr` node
base and the kernel's `_text` sits `0x18000` further in than `text_offset`
implies; (b) KASLR shifted the drop by `0x18000` from `0x80100000`. The read that
separates them is a second boot of a *different* kernel (the stock Android one)
and comparing its `_text` — or reading the `kaslr` node again on that boot.

### K04 — what this changes for the port

* The repo's `UEFI_BASE = 0x80080000` (FD `0x80080000..0x80780000`) sits **below**
  the observed drop `0x80118000`, and overlaps the head of the loaded image
  (`0x80118000..0x80780000` of the shim+FD copy is inside the source region).
  With `dst < src` a forward copy stays behind the read cursor, so the copy is
  not the hazard — but the claim "the FD lands in clean RAM" is now false as
  written: `0x80080000..0x80780000` overlaps where sboot put the `Image`.
* `sec_debug_magic` (`0x80000000 + 0x1000`) and `kaslr` (`0x80100000 + 0x100000`)
  are both inside `0x80080000..0x80780000` — two sboot-owned regions the FD would
  overwrite.
* A candidate that is outside everything read so far: above the kernel window,
  i.e. `>= 0x832D0000` (still inside bank0, which ends `0xBAAFFFFF`), and below
  the `mm_debug`/`camera` reservations (`0x91200000`, `0x96000000`, `0x99000000`).
  That is a **decision to test on device**, not a value to adopt now.

## Sources

- Device: `adb shell` on `RF8M21HYDSZ   recovery` — `cat /proc/iomem`,
  `grep -i code /proc/iomem`.
- Local: `C:\Users\ilya\code\Dumps\twrp_device_tree\kernel` first 64 bytes
  (`Get-Content -Encoding Byte -TotalCount 64`).
- Cross-reference: `notes/K10-rkp.md` T16/T18 (live `kaslr`, `sec_debug_magic`,
  `memory@80000000`).
