/*  Exynos9820Wlan.h - S10e (beyond0lte) BCM4375 WLAN, milestone-1 PROBE driver: constants.

    Milestone 1 (notes/m12-wifi-m1.md) is a plain KMDF driver, not an NDIS miniport: it binds the
    ACPI-described device the DSDT publishes (ACPI\SAMS9825, present only when the UEFI half trained
    the PCIe link), maps the two _CRS windows, and reports the endpoint's VID/DID, COMMAND, BARs and
    the ChipCommon chipid. Nothing else - no firmware download, no interrupt, no network stack.
    The NDIS miniport skeleton from the drivers branch (5746999/cb6a90f) is milestone 2+ material.

    HARDWARE CONSTANTS carry the same VERIFIED/UNVERIFIED discipline as
    firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h (where the same values live for the
    firmware); protocol constants are cited to bcmdhd (LineageOS android_kernel_samsung_exynos9820,
    used as a reference document for 4375 facts - see README.md).
*/

#ifndef _EXYNOS9820_WLAN_H
#define _EXYNOS9820_WLAN_H

/* ----------------------------------------------------------------------
 * _CRS contract (Dsdt.asl Device (WLAN)): memory descriptor 0 = CFG0 iATU
 * window (EP config space, bus1 dev0 func0), memory descriptor 1 = MEM iATU
 * window (1:1 CPU == PCI; the EP BARs Beyond0LtePcieDxe assigned land here),
 * then one interrupt (GSIV 285) which milestone 1 does not connect.
 * -------------------------------------------------------------------- */
#define WLAN_CFG_WINDOW_BASE        0x14FFE000ULL  /* VERIFIED (EXYNOS_PCIE0_CFG_BASE) */
#define WLAN_CFG_WINDOW_SIZE        0x2000UL       /* VERIFIED (EXYNOS_PCIE0_CFG_SIZE) */
#define WLAN_MEM_WINDOW_BASE        0x14000000ULL  /* VERIFIED (EXYNOS_PCIE0_MEM_BASE) */
#define WLAN_MEM_WINDOW_SIZE        0xFF0000UL     /* VERIFIED (EXYNOS_PCIE0_MEM_SIZE) */

/* Type-0 config header (PCI Local Bus 3.0). */
#define EP_CFG_VENDOR_DEVICE        0x000
#define EP_CFG_COMMAND              0x004
#define EP_CFG_CLASS_REV            0x008
#define EP_CFG_BAR0                 0x010          /* BARn at 0x10 + 4*n */
#define EP_CFG_BAR_COUNT            6
#define EP_CFG_SUBSYSTEM            0x02C
#define EP_CFG_COMMAND_MEM_EN       0x00000002UL
#define EP_CFG_BAR_IO               0x1UL
#define EP_CFG_BAR_TYPE_MASK        0x6UL
#define EP_CFG_BAR_TYPE_64          0x4UL
#define EP_CFG_BAR_MEM_MASK         0xFFFFFFF0UL

#define BCM4375_PCI_VENDOR_ID       0x14E4         /* VERIFIED (bcmdhd dhdpcie id table) */
#define BCM4375_EXPECTED_PCI_ID_UNVERIFIED 0x4475  /* UNVERIFIED (bcmdevs.h:299; the R1 print / this
                                                      driver's report closes it on device) */

/* Broadcom PCIe BAR0 layout (VERIFIED: bcmdhd include/pcie_core.h:55-60 -
 * "PCIE Bar0 Address Mapping. Each function maps 16KB config space"). */
#define PCIE_BAR0_MIN_SIZE          0x4000UL
#define PCIE_BAR0_CCCORE_OFF        0x3000UL       /* ChipCommon core registers */

/* ChipCommon chipid (VERIFIED: bcmdhd include/sbchipc.h:177-178, first register):
 * bits 15:0 chip id, bits 19:16 chip revision. */
#define CC_CHIPID_REG               0x0000UL
#define CC_CHIPID_MASK              0x0000FFFFUL
#define BCM4375_CHIP_ID             0x4375         /* VERIFIED (bcmdevs.h:520) */

/* Report sinks (README.md "Reading the result"). */
/* Not Windows\Temp: that directory's index was corrupt on this unit (notes/m12-touch.md run 6). */
#define WLAN_REPORT_FILE            L"\\SystemRoot\\System32\\LogFiles\\Exynos9820Wlan.txt"
#define WLAN_REPORT_MAX             2048

#endif /* _EXYNOS9820_WLAN_H */
