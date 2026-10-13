# Exynos9820Bt — BCM4375 Bluetooth-over-UART transport (S10e)

Status: **skeleton, not built, not device-validated.** Windows half of D4c
(`docs/exynos9820-port/DRIVERS-MILESTONES.md`, design
`docs/exynos9820-port/D4c-BT-DESIGN.md`, research `notes/bt-driver.md`).

What this is: a KMDF function driver binding `ACPI\SAMS9826` (DSDT
`Device (BTH0)`) that will expose the BCM4375's Bluetooth function — reached
through the dedicated BT UART (`uart@10840000`, H4 framing, Broadcom
patchram) — to the in-box stack as a **BthxDDI HCI transport**
(`bthport.sys` binds the HCI function; rung R4 splits FDO/PDO).

What is implemented already (pure logic, no hardware needed):
- UART register programming incl. the USI v2 quirks (USI_CON reset release,
  USI_OPTION CLKREQ_ON) — semantics from vendor `samsung.c`/`serial_s3c.h`.
- H4 TX framing (`BtH4SendCommand`/`BtH4SendAcl`).
- The `.hcd` record walker (`BtHcdNextRecord`) — the exact format proven
  byte-exact against the device's own `bcm4375B1_semco.hcd` (notes B2).

What is deliberately NOT-IMPLEMENTED (each marker names its rung + reference):
- `_CRS` resource mapping + interrupts (R4) — until then `BtUartInit` is
  never called on an unmapped window.
- ROM HCI probe (R2), patchram download (R3), baud switch (Q2, registry
  `Parameters\BaudRate`, default 115200 = no switch).
- BthxDDI version/capabilities/read-write (R4/R5).

## License posture

- Structure and BthxDDI shape: `microsoft/Windows-driver-samples`
  `bluetooth/serialhcibus` — **MS-PL**, adoptable with notices.
- Broadcom-specific prior art: `worproject/cywbtserialbus` — **MS-PL**.
- `hci_bcm.c` / `btbcm.c` / `bcm43xx.c` (Linux, GPL-2.0): **reference
  documents only** — opcodes/sequences are facts; no code or text copied.
- Firmware blobs: **never committed.** The installer copies them from the
  device's own `/vendor/firmware` (see `notes/bt-driver.md` B1 for the
  extraction recipe: `debugfs -R "dump /firmware/<name>.hcd"` on
  `dumps/hhxj6/vendor/vendor.raw`), placing them under `C:\S9WOA\bt\`.

## Build

Same WDK flow as the other drivers in this repo (ARM64, `WindowsKernelModeDriver10.0`,
test-signing). Not buildable on the Linux verification box — that build covers
the firmware half only.
