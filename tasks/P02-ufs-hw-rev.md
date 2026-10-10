# P02 — UFS hw-rev class-6 HCI encoding

Status: **open — parked behind the boot attempt**

## Goal

The S10e's UFS PHY reports `hw-rev = 0x00000006` — now confirmed **on the device**
(`/proc/device-tree/ufs@0x13D60000/hw-rev` = `00 00 00 06`, `notes/s10e-dtb-mmio.md`
S02), matching Samsung's DTS `UFS_VER_0006`. What is NOT known is which HCI
encoding that revision class implies, and `notes/windows-m7-toolchain.md:283`
records that as "not knowable from any source consulted".

## Known

- Live DTB identity props for the PHY: `hw-rev = 6`, `evt-ver = 1`,
  `brd-for-cal = 0x10` (the DTS said `1` for SMDK — the live S10e differs),
  `fmp-id = 0`, `smu-id = 0`, `freq-table-hz` = 16 zero bytes,
  `dma-coherent` and `ranges` present as empty flag properties,
  `compatible = "samsung,exynos-ufs"`.
- The driver already carries the vendor-specific HCI offsets at HCI+`0x1100`
  (`UFSHCI_VS_1US_TO_CNT_VAL 0x110C`, `UFSHCI_VS_UFSHCI_V2P1_CTRL 0x118C`) as an
  **inference** (`notes/exynos9820-research.md:145`).

## Next actions

1. Stop guessing: read the HCI's own version registers at runtime —
   `0x000 DCMD`, `0x004 TCMD`, `0x008 CSUM`, `0x00C PSUM`, `0x010 VERSION`,
   `0x014 HVERSION` — and print them. That is the same read the JEDEC spec
   defines, so it closes the class question without a source hunt.
2. Keep the diagnostic path able to print what the hardware says
   (`MILESTONES.md:159`) — that row is the mechanism for step 1.

## Done when

The printed `VERSION`/`HVERSION` pair is recorded in `notes/m8-ufs-hc.md` and the
driver's behaviour branches on it instead of on `hw-rev`.
