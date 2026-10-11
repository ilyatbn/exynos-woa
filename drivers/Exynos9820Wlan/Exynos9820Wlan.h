/*  Exynos9820Wlan.h — S10e (beyond0lte) BCM4375 WLAN miniport: constants
    and adapter state.

    HARDWARE CONSTANTS below carry the same VERIFIED/UNVERIFIED discipline as
    firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h; protocol
    constants are cited to the open reference drivers:
      bcmdhd  = LineageOS/android_kernel_samsung_exynos9820 bcmdhd (GPLv2+LE) —
                used as a REFERENCE DOCUMENT for 4375 facts (see README.md for
                the license posture)
      brcmfmac = mainline Linux (ISC) — architecture reference

    NOTHING in this driver is validated on an S10e yet. The bring-up rungs
    (R3..R5) are defined in docs/exynos9820-port/M12-WIFI-DESIGN.md section 7.
*/

#ifndef _EXYNOS9820_WLAN_H
#define _EXYNOS9820_WLAN_H

#include <ndis.h>

/* ----------------------------------------------------------------------
 * ACPI-described device (design section 2). The UEFI half
 * (Beyond0LtePcieDxe) latches: link trained, iATU region1 CFG0 ->
 * 0x14FFE000 (bus1 dev0), region0 MEM -> 0x14000000 1:1, EP BAR0 assigned,
 * COMMAND = MSE|BME. This driver maps ONLY what _CRS hands it.
 * -------------------------------------------------------------------- */

#define WLAN_ACPI_HARDWARE_ID       L"ACPI\\SAMS9825"   /* must match DSDT _HID */

/* Resource indices inside _CRS (order fixed by the DSDT task). */
#define WLAN_RES_CFG_WINDOW         0   /* 0x14FFE000 +0x2000, EP config space */
#define WLAN_RES_MEM_WINDOW         1   /* 0x14000000 +0xFF0000, EP BAR0 lands here */
#define WLAN_RES_COUNT              2

/* ----------------------------------------------------------------------
 * Endpoint config space via the CFG0 window (VERIFIED: single iATU CFG0
 * region programmed for bus1/dev0/func0 by the UEFI half).
 * -------------------------------------------------------------------- */
#define EP_CFG_VENDOR_DEVICE        0x000
#define EP_CFG_COMMAND              0x004
#define EP_CFG_COMMAND_MEM_EN       0x00000002UL
#define EP_CFG_COMMAND_BUS_MASTER   0x00000004UL
#define EP_CFG_COMMAND_INTX_DISABLE 0x00000400UL

#define BCM4375_PCI_VENDOR_ID       0x14E4  /* VERIFIED (bcmdhd dhdpcie id table) */
#define BCM4375_EXPECTED_PCI_ID     0x4475  /* UNVERIFIED (bcmdevs.h:299; the UEFI
                                               R1 print closes it on device)     */

/* ----------------------------------------------------------------------
 * Chip BAR0 layout (VERIFIED: bcmdhd include/pcie_core.h:55-60 —
 * "PCIE Bar0 Address Mapping. Each function maps 16KB config space").
 * -------------------------------------------------------------------- */
#define PCIE_DEV_BAR0_SIZE          0x4000UL
#define PCIE_BAR0_WINMAPCORE_OFF    0x0000UL  /* window-mapping registers */
#define PCIE_BAR0_EXTSPROM_OFF      0x1000UL
#define PCIE_BAR0_PCIECORE_OFF      0x2000UL  /* PCIe core registers      */
#define PCIE_BAR0_CCCORE_OFF        0x3000UL  /* ChipCommon core registers */

/* ChipCommon register block, first register (VERIFIED: bcmdhd include/
 * sbchipc.h:177-178, struct starts `uint32 chipid; /* 0x0 */`).
 * bits 15:0 = chip id, bits 19:16 = chip revision (bcmdhd CHIPID()/CHIPREV()).
 */
#define CC_CHIPID_REG               0x0000UL
#define CC_CHIPID_MASK              0x0000FFFFUL
#define BCM4375_CHIP_ID             0x4375    /* VERIFIED (bcmdevs.h:520) */

/* ----------------------------------------------------------------------
 * Firmware download target (VERIFIED: bcmdhd dhd_pcie.c:1600-1602 maps
 * BCM4375_CHIP_ID to CR4_4369_RAM_BASE; sbchipc.h:3285 = 0x170000).
 * The firmware image is loaded from the device's own vendor partition copy
 * (installer stage); NEVER embedded in this package (README.md).
 * -------------------------------------------------------------------- */
#define BCM4375_FW_RAM_BASE         0x00170000UL

/* Firmware file location placed by the installer (blob names P3: needs the
 * live /vendor/firmware read; default names from CONFIG_BCMDHD_*_PATH). */
#define WLAN_FW_DIR                 L"\\??\\C:\\S9WOA\\wifi\\"
#define WLAN_FW_FILE                L"bcmdhd_sta.bin"          /* UNVERIFIED name (P3) */
#define WLAN_NVRAM_FILE             L"nvram_net.txt"           /* UNVERIFIED name (P3) */

/* ----------------------------------------------------------------------
 * Adapter extension
 * -------------------------------------------------------------------- */
typedef struct _WLAN_ADAPTER {
    NDIS_HANDLE              MiniportAdapterHandle;

    PUCHAR                   CfgWindow;      /* mapped _CRS resource 0 */
    PUCHAR                   MemWindow;      /* mapped _CRS resource 1 */
    NDIS_PHYSICAL_DESCRIPTOR CfgPhys;
    NDIS_PHYSICAL_DESCRIPTOR MemPhys;

    UINT32                   PciVendorId;    /* read at start, expect 14e4 */
    UINT32                   PciDeviceId;    /* read at start, expect 4475 */
    UINT32                   ChipId;         /* BAR0 CC chipid, expect 4375 */
    BOOLEAN                  ChipAlive;      /* ChipId matched 4375 */

    NDIS_HANDLE              NdisLock;

    /* Firmware/rings state — populated by later rungs (R4+). */
    PVOID                    FwImage;        /* loaded image buffer */
    UINT32                   FwLength;
    PVOID                    NvramBuffer;
    UINT32                   NvramLength;

    NDIS_MEDIUM              Medium;
    NDIS_PHYSICAL_MEDIUM     PhysicalMedium;
    UINT32                   PacketFilter;
    BOOLEAN                  MediaConnected; /* R5 flips this honestly */
} WLAN_ADAPTER, *PWLAN_ADAPTER;

/* ----------------------------------------------------------------------
 * Small helpers (WlanDriver.c)
 * -------------------------------------------------------------------- */
UINT32 WlanCfgRead (_In_ PWLAN_ADAPTER A, _In_ UINT32 Offset);
VOID   WlanCfgWrite (_In_ PWLAN_ADAPTER A, _In_ UINT32 Offset, _In_ UINT32 Value);
UINT32 WlanBar0Read (_In_ PWLAN_ADAPTER A, _In_ UINT32 Offset);
VOID   WlanBar0Write (_In_ PWLAN_ADAPTER A, _In_ UINT32 Offset, _In_ UINT32 Value);

#endif /* _EXYNOS9820_WLAN_H */
