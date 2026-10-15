# Exynos9820Bt - BCM4375 Bluetooth-over-UART transport (S10e, D4c)

Windows half of D4c (design `docs/exynos9820-port/D4c-BT-DESIGN.md`, research and run log `notes/bt-driver.md`).
The UEFI half (`firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteBtUartDxe`) latches the UART role (clocks, pins,
USI mode) and proved rungs R1 (UART loopback) and R2 (the BCM4375 ROM answers HCI) on the device.

## Provenance

Based on Microsoft's Windows-driver-samples `bluetooth/serialhcibus` (KMDF bus driver implementing the Bluetooth
Extensibility transport DDIs, `bthxddi.h`). The sample files keep their Microsoft copyright headers; the repository
is MS-PL. Imported unchanged except:
- files flattened into this folder (`WDK/device.c|h` -> `Device.c|h`, `WDK/Debugdef.h` -> `Debugdef.h`);
- WPP removed: `DoTrace` is a `DbgPrintEx` macro in `Debugdef.h` (messages go to the KD log), `%!STATUS!` -> `0x%08X`,
  `.tmh` includes dropped.

Linux `hci_bcm.c` / `btbcm.c` (GPL-2.0) are behaviour references only (opcodes, sequences); no code copied.
Firmware patch files (`.hcd`) are copied from the phone's own `/vendor/firmware`, never committed.

## Status

- W1 (this commit): the sample builds as `Exynos9820Bt.sys` (ARM64, test-signed, catalogued), binding
  `ACPI\SAMS9829`. Its lower edge still opens a UART controller through the resource hub - it will NOT start on the
  phone yet.
- W2 (next): direct-MMIO lower edge - UART from `_CRS` memory + interrupt, UART init + patchram in the
  `DeviceInitialize` vendor hook, TX straight into the FIFO, RX ISR/DPC feeding the sample's H4 reassembler.
- W3: DSDT `Device (BTH0)`; W4: deploy + bthport binding (R4), then scan/pair (R5).

## Build

`build-package.ps1 -Out <dir>` (VS18 MSBuild, WDK 26100, ARM64 Release, test certificate).
