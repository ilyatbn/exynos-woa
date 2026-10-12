# Parked: Bluetooth (D4c), BCM4375 BT over UART1

Status: research, design and a Windows driver skeleton exist. Nothing is built or validated on the phone.

## Copied to main (2026-10-12, from `origin/drivers`)
- `notes/bt-driver.md`: the full research record (B1..B5) with its review log and sources.
- `docs/exynos9820-port/D4c-BT-DESIGN.md`: the design. A UEFI role-latch (CMU/pinctrl/USI mode) plus a Windows
  direct-MMIO BthxDDI transport over H4, with a 6-rung ladder R1..R6.
- `drivers/Exynos9820Bt/`: the KMDF skeleton (binds `ACPI\SAMS9826`, programs UART/USI, frames H4 TX, walks `.hcd`).
  It has never been compiled.
- The D4c rows in `docs/exynos9820-port/DRIVERS-MILESTONES.md`.

## NOT copied: firmware commits on `origin/drivers` to cherry-pick when BT work starts
These change the UEFI image, so they wait until the m15 UFS / GPU test images are finished.
- `0ca47c2`: `Exynos9820.h` "Bluetooth" constants block (UART_BT 0x10840000, USI_BT 0x10821000, GSIV 408, CMU
  ids, BT GPIOs, PCAL6524 on hsi2c25) plus 3 `PlatformMemoryMapLib.c` entries. Expect conflicts in both files:
  main has since added the AUD/G3D blocks and their map entries.
- `28e85fd` + `9ec78e5`: DSDT `Device (BTH0)` `_HID SAMS9826` (UART window, GSIV 408, hostwake GpioInt 19 with
  debounce 0xFFFF). Expect conflicts in `Dsdt.asl`, which now has AUD0/GPU0.
- Reference only (already copied): `80e8aa6`, `5f6931a` (research), `a17089b` (design), `6f445d4` (skeleton),
  `6cdf8de` (drivers-branch HANDOFF §25, the next-order list below).

## Next order (from drivers-branch HANDOFF §25)
1. Phone read from TWRP root: `od -t x4 /proc/device-tree/bluetooth/gpios` and the live dtbo overlay. This closes
   Q1: is the bt_en line PCAL6524 pin 7, gpm8, or null? Also run `ls /vendor/firmware/*4375*` for the blob names.
2. Firmware half: `Beyond0LteBtUartDxe` (CMU_PERIC1 QCH + ungates 0x22a/0x23c, gpq0 fn2 mux, USI mode=1), then
   R1 UART loopback and R2 `HCI_READ_LOCAL_VERSION` in UEFILOG. This also closes `BT_UART_SRC_CLK_HZ` (UNVERIFIED).
3. Build `drivers\Exynos9820Bt` (ARM64 Release), then patchram (R3), then the Windows R4 layers.
