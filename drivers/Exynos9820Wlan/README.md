# Exynos9820Wlan — the S10e's own WiFi (BCM4375) on Windows ARM64

**Status: skeleton, rung R3. Not built, not installed, not booted.**
Plan-of-record: [`docs/exynos9820-port/DRIVERS-MILESTONES.md`](../../docs/exynos9820-port/DRIVERS-MILESTONES.md) §D4b;
architecture: [`docs/exynos9820-port/M12-WIFI-DESIGN.md`](../../docs/exynos9820-port/M12-WIFI-DESIGN.md);
evidence: [`notes/m12-wifi-chip.md`](../../notes/m12-wifi-chip.md).

## What this is

An NDIS miniport for the **Broadcom BCM4375** PCIe endpoint inside the Galaxy
S10e. It does **not** enumerate PCI: the UEFI module `Beyond0LtePcieDxe`
trains the link, programs the iATU windows, assigns the chip's BAR0 and
latches all of it across `ExitBootServices`. This driver binds the
ACPI-described device (`ACPI\SAMS9825`, published by our DSDT) and maps the
two windows `_CRS` hands it.

## What works in this skeleton (on paper; each needs its device rung)

| Rung | Piece | State |
|---|---|---|
| R3 | bind, map windows, read EP VID/DID via CFG0 window, read ChipCommon `chipid` via BAR0+0x3000 (expect `14e4:4475`, `0x4375`) | code here |
| R4 | firmware download to RAM base `0x170000`, shared-area handshake, common ring 0, msgbuf, flow rings, interrupt (GSIV 285) | NOT-IMPLEMENTED markers in `WlanDriver.c` |
| R5 | Native 802.11 (WDI) OID surface, connect | NOT-IMPLEMENTED |

Sends complete as `NDIS_STATUS_NOT_ACCEPTED` until R4 — an honest failure
beats a fake success.

## Firmware blobs

The 4375 has **no open firmware** and linux-firmware carries **no 4375 PCIe
blob at all**. The only working firmware is this device's own
`/vendor/firmware/bcmdhd_sta*.bin` + `nvram_net.txt` (exact names: open item
P3). Policy, same as the D8 audio rule:

- **never commit the blobs to this repo**;
- the installer copies them from the device's own vendor partition at install
  time into `C:\S9WOA\wifi\`;
- `nvram_net.txt` is per-device calibration — it must come from this unit.

## License posture (read before extending)

- Protocol architecture is ported against **mainline brcmfmac (ISC)**:
  `commonring.c`, `msgbuf.c`, `pcie.c`, `chip.c`.
- **bcmdhd** (the vendor driver, GPLv2 + linking exception) is used as a
  *reference document* for BCM4375-only facts — constants, register values,
  quirks (it is the only complete 4375 implementation; mainline does not
  support chip 4375). Facts are not expression; but if a future function
  ends up transliterated from bcmdhd rather than brcmfmac, mark the function
  header `/* derived from bcmdhd dhd_*.c (GPLv2+LE) */` so the boundary
  stays auditable.
- The Broadcom firmware blobs are proprietary and stay on-device (above).

## Build (needs-windows)

```
msbuild drivers\Exynos9820Wlan\Exynos9820Wlan.vcxproj /p:Configuration=Release /p:Platform=ARM64
```

WDK 10.0.26100, ARM64, test-sign — same toolchain and posture as the M7 set.
The INF binds `ACPI\SAMS9825`, which only exists once the DSDT `Device (WLAN)`
task lands; installing before that is a no-op, not a failure.

## Prerequisites on the device side

1. `Beyond0LtePcieDxe` must print `[PCIe] R1: endpoint 14e4:4475` (closes
   open item P2 — the device ID constant is UNVERIFIED until that print).
2. The DSDT must publish `Device (WLAN)` with `_HID "SAMS9825"`, `_CRS` =
   CFG window `0x14FFE000+0x2000`, MEM window `0x14000000+0xFF0000`,
   `Interrupt(Level, ActiveHigh, Exclusive){285}`.
3. The installer must have copied the firmware files.
