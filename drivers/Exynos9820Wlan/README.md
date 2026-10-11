# Exynos9820Wlan — S10e BCM4375 WiFi, milestone-1 probe (Windows ARM64)

**Status: built + test-signed, not installed, not booted** (`notes/m12-wifi-m1.md`).
Architecture: [`docs/exynos9820-port/M12-WIFI-DESIGN.md`](../../docs/exynos9820-port/M12-WIFI-DESIGN.md);
chip evidence: [`notes/m12-wifi-chip.md`](../../notes/m12-wifi-chip.md).

## What this is (and why it is not an NDIS miniport yet)

A plain **KMDF** driver that binds `ACPI\SAMS9825` — the `Device (WLAN)` our DSDT publishes only on a
boot where `Beyond0LtePcieDxe` trained the PCIe link and read a Broadcom vendor ID (status word
`'WLAN'` at `0x8FE0F00C`). It maps the two `_CRS` windows (CFG0 `0x14FFE000+0x2000`, MEM
`0x14000000+0xFF0000`) and reports, without writing anything to the chip:

- endpoint VID/DID, COMMAND, class/rev, subsystem, BAR0..BAR5 (config space via the CFG0 window);
- ChipCommon `chipid` at BAR0 + 0x3000 (expected id `0x4375`).

Milestone 1 only has to prove "Windows binds the device and talks to the chip". An NDIS miniport
(`Class=Net`) would also need the `*IfType`/`*MediaType`/`*PhysicalMediaType`/`Characteristics`
keywords and a working NDIS registration before it could even start — all failure modes unrelated to
the question. The NDIS skeleton from the drivers branch (`5746999`, `cb6a90f`) is milestone 2+.

## Build

```
powershell -NoProfile -ExecutionPolicy Unrestricted -File drivers\Exynos9820Wlan\build-package.ps1 -Out <dir>
```

Same flow as `drivers/Exynos9820Hsi2c/build-package.ps1` (VS18 MSBuild with `VisualStudioVersion=17.0`,
signtool with the WDK test cert `5DD5DEE1…4E48`, Inf2Cat for `10_RS3_ARM64,10_NI_ARM64,10_GE_ARM64`,
signed catalog). Output: `Exynos9820Wlan.sys`, `Exynos9820Wlan.inf`, `exynos9820wlan.cat`.

## Install on the phone

Copy the package to the ESP as `\S10EDIAG\drivers\Exynos9820Wlan\` and
`drivers/S10eDiag/esp/extra-wlan.cmd` to `\S10EDIAG\extra.cmd`; the S10eAgent run installs it with
`pnputil /add-driver … /install` (TMP redirected to `C:\S10eTmp`, as for the touch packages).

## Reading the result

| Sink | Where | Read from TWRP |
|---|---|---|
| text report | `C:\Windows\System32\LogFiles\Exynos9820Wlan.txt` | mount the Windows NTFS volume, `cat` it; also copied by `extra-wlan.cmd` to the ESP `out\last\wlan\` |
| registry | `HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Wlan\Parameters` (`Report` REG_SZ, `Result`, `VendorDevice`, `Command`, `Bar0..5`, `ChipId`) | `params.txt` from `extra-wlan.cmd`, or the offline SYSTEM hive |
| kernel debug | `DbgPrintEx` IHVDRIVER/ERROR | KD / DbgView |

`Result`: 0 = BCM4375 alive (chipid 0x4375), 1 = `_CRS` mismatch (device fails start, Code 10),
2 = map failed, 3 = config space dead (0 / ~0), 4 = vendor not Broadcom, 5 = BAR0 unusable
(not a ≥16 KB memory BAR inside the MEM window, or MEM decode off), 6 = chipid mismatch.

## Firmware blobs (milestone 2, never committed)

The 4375 has no open firmware; the blobs stay on-device and are copied at install time. Paths for the
device session: `notes/m12-wifi-m1.md` "Firmware blobs for milestone 2".

## License posture

Facts (register offsets, IDs) are cited from bcmdhd (GPLv2 + linking exception) as a reference
document; no bcmdhd code is copied. Protocol work in later milestones follows mainline brcmfmac (ISC).
