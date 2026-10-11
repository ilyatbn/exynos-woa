# W01 — WiFi rung R1: PCIe link-up observed on the device

Status: open — **needs-boot** (rides the M8 boot stream)

## What

`Beyond0LtePcieDxe` is in the main-based FD (FFS `…5553`, CLANGPDB DEBUG build 2026-10-12, M12
milestone 1 - the full log-line ladder and the Windows half are in `notes/m12-wifi-m1.md` "Device
test procedure"). The first device observation for M12/D4b is:

```
[PCIe] link up on try 1 after N polls, RDLH=0x.., speed GEN1
[PCIe] R1: endpoint 14e4:4475 (expected 14e4:4475, UNVERIFIED until now)
```

## What it closes

- **P2**: the live PCI device ID → move `BCM4375_EXPECTED_PCI_ID` from
  UNVERIFIED to VERIFIED (in `Exynos9820.h`, `Exynos9820Wlan.h`, and the INF
  if it turns out to be 4476/4477).
- **P4**: whether direct CMU_FSYS1 gate writes work while ACPM/HWACG owns
  them (link up = yes).
- The whole UEFI bring-up chain (PHY tables, I/A sequencer, PERST/WLAN_EN
  GPIOs) — a link is proof of all of it.

## If it fails

The failure modes are separable by the debug prints:
- no `[PCIe]` line at all → module not reached (FDF/DSC or earlier hang)
- `link try N failed, RDLH=…` → LTSSM never trains: suspect P4 (clocks),
  the power rail (WLAN_EN), or PERST timing
- link up but vendor != 14e4 → CFG0 window / iATU problem

Read `/proc/debug_history` per repo rule 9 if the boot loops after this
module runs.

## Do NOT

Nothing destructive; the module only writes PCIe/CMU/PMU/GPIO registers that
Android's own driver also programs. It does not touch the modem port
(pcie_1 stays disabled).
