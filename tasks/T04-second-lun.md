# T04 — Handle the second LUN

Status: **open**

## Goal

The UFS host controller and the storage driver were ported against **one**
`/dev/sda`-style device. This phone's partition table proves a second one exists:
`cpefs` lives on **`sdd1`** (`notes/s10e-partitions.md:10`, the live GPT read).
Everything named `sda*` is the first block device; `sdd` is a different LUN of the
same UFS PHY.

## Known

- Live GPT (root read, `notes/s10e-partitions.md`): `cpefs sdd1`, and the rest
  `sdaNN`. The entry-name offset was corrected in `notes/s10e-root-reads.md` §2
  (name at **+56**).
- The DTB's `ufs@0x13D60000` node has one `ranges`/`reg` list and a
  `pinctrl-0`; nothing read so far says how many LUNs the PHY drives.
- `drivers/Exynos9820Ufs/` was ported with one device in mind; the host-controller
  side (`Beyond0LteUfsHcDxe`) reports whatever the PHY says, so the driver half is
  the part that may be wrong.

## Next actions

1. Read the PHY's own LUN count from the HCI (`UFSH2O_DEVICE` / `NUM_LUNS`
   behaviour register) — that is a runtime read, not a guess, and the driver
   already has the plumbing to print it (`notes/m8-ufs-hc.md`).
2. Decide whether Windows on ARM needs the `cpefs` LUN at all (it is Android's
   content-storage filesystem, not a Windows volume) — likely "must be visible,
   must not be mounted".
3. Keep the answer in the driver's device enumeration, not in a hardcoded count.

## Done when

The LUN count is a read from the hardware (printed, not asserted) and the
storage map either includes or explicitly excludes `cpefs` with a reason.
