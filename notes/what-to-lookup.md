# What to look up — five items, in the order they matter

Everything else is already in this repo. These are the only numbers still missing,
and each one says who can get it.

## 1. The phone's live partition table — **you, one command, in TWRP**

```
sgdisk -l /dev/block/by-name/sda        # or: sfdisk -d /dev/block/by-name/sda
```

Save the whole output to a file and drop it in `C:\Users\ilya\code\Dumps\`.

Why it matters: it is the only source for the **real start offsets** and the
**unique GUIDs** of `USERDATA`, `CACHE`, `SYSTEM`, `BOOT`, `RECOVERY`. The PIT in
`Dumps` gives the stock layout and its sizes already match the phone exactly, but
the live GPT can differ from the PIT, and the Windows BCD addresses partitions by
the unique GUID, not by name.

Not obtainable from here: `/sys/block/sda` and every `reserved-memory/*/reg` are
`Permission denied` from an unprivileged `adb` shell (`notes/s10e-live-reads.md` §9.1).

## 2. The device's own PIT (optional cross-check) — **you, Download mode**

```
heimdall print-pit --no-reboot
```

Heimdall is not installed on this PC; the installer gets it through winget
(`BenjaminDobell.Heimdall`). Only needed if you want a second confirmation of (1) —
(1) alone is enough.

## 3. A TWRP build for the S10e — **me / any download**

Official file name must contain `beyond0lte` (e.g. `twrp-…-beyond0lte.img`), not
`star2lte`. `C:\Users\ilya\code\Dumps\twrp_device_tree` is the **device tree** for
beyond0lte (its `recovery/` folder is an unpacked recovery payload), which proves a
beyond0lte recovery exists — but the installer flashes a bootable image, so the
built `.img` is what's needed.

## 4. Samsung's SM-G970F kernel DTS — **me (public source), only if we want two more constants**

File to find: `exynos9820-beyond0lte*.dts` (and `exynos9820-rmem_s5100_dual.dtsi`)
in Samsung's released kernel source for SM-G970F.

Why: it is the only source for the **framebuffer base** (`EXYNOS_FB_BASE`, still
`0`) and the exact **`reserved-memory` sizes** behind the PRAM breadcrumb choice.
Neither blocks booting today — the fb entry is simply not published, and the
breadcrumb channel is compiled out.

## 5. Nothing else

Already known and in the tree: the Windows volume geometry (`notes/pit-geometry.md`
— offset 8 811 184 128, size 119 126 622 208, disk 127 943 049 216), the UFS
fence/protected LBAs in the driver's own units, the partition name→index map, the
GIC/UFS/MMIO windows, the ACPI tables.

## Order to work in

1 → then the code change that uses it (the driver's fence constants + an offset
check in the installer's `partition` stage) → then 3 → then a first boot attempt.
4 is optional and can wait.
