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
#define WLAN_REPORT_MAX             16384

/* ======================================================================
 * Milestone 2: firmware download + ARM start (notes/m12-wifi-m2.md, which
 * carries the full citation table). dhd = bcmdhd (LineageOS exynos9820,
 * lineage-23.2), fmac = mainline brcmfmac. Facts only, no code copied.
 * ==================================================================== */

/* EP BAR geometry the UEFI assigned (VERIFIED: boot #88 UEFI sizing print, notes/m12-wifi-m1.md).
 * BAR0 = 64-bit 64 KB register window, BAR2 = 64-bit 4 MB TCM window (dhd dhd_pcie.h:78
 * DONGLE_TCM_MAP_SIZE is the same 4 MB). */
#define WLAN_BAR0_SIZE              0x10000UL
#define WLAN_BAR2_SIZE              0x400000UL
#define EP_CFG_COMMAND_BME          0x00000004UL   /* PCI COMMAND bus master enable: must stay 0 */

/* Config-space backplane windows (dhd include/pcicfg.h:170-171; fmac pcie.c:122-123). */
#define BCM_CFG_BAR0_WIN            0x80           /* BAR0+0x0000..0x0FFF -> backplane (4 KB) */
#define BCM_CFG_BAR1_WIN            0x84           /* BAR2 (dhd "BAR1") -> backplane base */
#define BCM_BAR0_WIN_SIZE           0x1000UL

/* Backplane layout (dhd include/hndsoc.h). */
#define SI_ENUM_BASE                0x18000000UL   /* hndsoc.h:55 */
#define SI_CORE_SIZE                0x1000UL       /* hndsoc.h:67 */
#define CC_EROMPTR_REG              0x00FCUL       /* fmac chipcommon.h:107 */
#define CC_CHIPID_TYPE_SHIFT        28             /* fmac CID_TYPE: 1 = AXI (AI) */

#define CORE_ID_CC                  0x800          /* hndsoc.h:148 */
#define CORE_ID_SOCRAM              0x80E          /* hndsoc.h:162 */
#define CORE_ID_D11                 0x812          /* hndsoc.h:166 */
#define CORE_ID_PMU                 0x827          /* hndsoc.h:187 */
#define CORE_ID_PCIE2               0x83C          /* hndsoc.h:205 */
#define CORE_ID_ARMCR4              0x83E          /* hndsoc.h:207 */
#define CORE_ID_GCI                 0x840          /* hndsoc.h:208 */
#define CORE_ID_SR                  0x841          /* hndsoc.h:209 */
#define CORE_ID_ARMCA7              0x847          /* hndsoc.h:212 */
#define CORE_ID_SYSMEM              0x849          /* hndsoc.h:213 */
#define CORE_ID_HUB                 0x84B          /* hndsoc.h:214 */
#define CORE_ID_OOBR                0x85C          /* hndsoc.h:215 */
#define CORE_ID_NS_CCB              0x50B          /* hndsoc.h:234 */
#define CORE_ID_DEF_AI_COMP         0xFFF          /* hndsoc.h:220 */
#define MFG_ID_ARM                  0x43B          /* aidmp.h:34 */

/* EROM descriptors (dhd include/aidmp.h:49-115). */
#define ER_REMAPCONTROL             0xE00
#define ER_SZ_MAX                   4096
#define ER_TAG                      0xEUL
#define ER_TAG1                     0x6UL
#define ER_VALID                    0x1UL
#define ER_CI                       0x0UL
#define ER_MP                       0x2UL
#define ER_ADD                      0x4UL
#define ER_END                      0xEUL
#define CIA_MFG_MASK                0xFFF00000UL
#define CIA_MFG_SHIFT               20
#define CIA_CID_MASK                0x000FFF00UL
#define CIA_CID_SHIFT               8
#define CIB_REV_MASK                0xFF000000UL
#define CIB_REV_SHIFT               24
#define CIB_NSW_MASK                0x00F80000UL
#define CIB_NSW_SHIFT               19
#define CIB_NMW_MASK                0x0007C000UL
#define CIB_NMW_SHIFT               14
#define CIB_NSP_MASK                0x00003E00UL
#define CIB_NSP_SHIFT               9
#define CIB_NMP_MASK                0x000001F0UL
#define CIB_NMP_SHIFT               4
#define AD_ADDR_MASK                0xFFFFF000UL
#define AD_SP_MASK                  0x00000F00UL
#define AD_SP_SHIFT                 8
#define AD_ST_MASK                  0x000000C0UL
#define AD_ST_SLAVE                 0x00000000UL
#define AD_ST_BRIDGE                0x00000040UL
#define AD_ST_SWRAP                 0x00000080UL
#define AD_ST_MWRAP                 0x000000C0UL
#define AD_SZ_MASK                  0x00000030UL
#define AD_SZ_SHIFT                 4
#define AD_SZ_SZD                   0x00000030UL
#define AD_AG32                     0x00000008UL
#define AD_SZ_BASE                  0x00001000UL
#define SD_SZ_MASK                  0xFFFFF000UL
#define SD_SG32                     0x00000008UL

/* AI (AXI) wrapper registers (dhd aidmp.h:311-314,363) and ioctrl bits (hndsoc.h:264-265, siutils.h:211). */
#define AI_IOCTRL                   0x408
#define AI_RESETCTRL                0x800
#define AI_RESETSTATUS              0x804
#define AIRC_RESET                  0x1UL
#define SICF_CLOCK_EN               0x0001UL
#define SICF_FGC                    0x0002UL
#define SICF_CPUHALT                0x0020UL

/* ARM CR4 core registers (dhd siutils.h:201-215 == fmac chip.c:205-217). */
#define ARMCR4_CAP                  0x04
#define ARMCR4_BANKIDX              0x40
#define ARMCR4_BANKINFO             0x44
#define ARMCR4_TCBANB_MASK          0x0FUL
#define ARMCR4_TCBBNB_MASK          0xF0UL
#define ARMCR4_TCBBNB_SHIFT         4
#define ARMCR4_BSZ_MASK             0x7FUL
#define ARMCR4_BUNITSZ_MASK         0x200UL
#define ARMCR4_BSZ_8K               8192UL
#define ARMCR4_BSZ_1K               1024UL

/* BCM4375 dongle RAM base (dhd dhd_pcie.c:1600-1602 -> CR4_4369_RAM_BASE, sbchipc.h:3285). */
#define BCM4375_RAM_BASE            0x170000UL

/* PCIE2 core: indirect config access, used for the BAR1-window sizing WAR
 * (dhd pcie_core.h:411-412; WAR dhd_pcie.c:1522-1525 == fmac pcie.c brcmf_pcie_attach). */
#define PCIE2_CONFIGADDR            0x120
#define PCIE2_CONFIGDATA            0x124
#define PCIE2_BAR1_SIZE_WAR_REG     0x4E0

/* Random seed below nvram (dhd dhd_pcie.c:11047-11070, bcmutils.h:948-965). */
#define BCM_ENTROPY_HOST_NBYTES     128UL
#define BCM_NVRAM_RNG_SIGNATURE     0xFEEDC0DEUL

/* nvram (fmac firmware.c:20-23). */
#define NVRAM_MAX_SIZE              64000UL
#define NVRAM_DEFAULT_BOARDREV      "boardrev=0xff"

/* pcie_shared (dhd bcmpcie.h:50-82, 330-395; fmac pcie.c:117,226-236). */
#define FW_UP_TIMEOUT_MS            5000
#define FW_UP_POLL_MS               10
#define PCIE_SHARED_VERSION_MASK    0x000000FFUL
#define PCIE_SHARED_ASSERT_BUILT    0x00000100UL
#define PCIE_SHARED_ASSERT          0x00000200UL
#define PCIE_SHARED_TRAP            0x00000400UL
#define PCIE_SHARED_MIN_VERSION     5              /* fmac pcie.c BRCMF_PCIE_MIN_SHARED_VERSION */
#define PCIE_SHARED_MAX_VERSION     7              /* dhd dhd_pcie.h:74 PCIE_SHARED_VERSION_7 */
#define SH_FLAGS                    0x00
#define SH_TRAP_ADDR                0x04
#define SH_ASSERT_EXP_ADDR          0x08
#define SH_ASSERT_FILE_ADDR         0x0C
#define SH_ASSERT_LINE              0x10
#define SH_CONSOLE_ADDR             0x14
#define SH_MSGTRACE_ADDR            0x18
#define SH_FWID                     0x1C
#define SH_DMA_RXOFFSET             0x24
#define SH_H2D_MB_DATA_PTR          0x28
#define SH_D2H_MB_DATA_PTR          0x2C
#define SH_RINGS_INFO_PTR           0x30
/* hnd_cons_t (dhd hnd_cons.h:54-84): log.buf, log.buf_size, log.idx */
#define CONS_LOG_BUF                0x08
#define CONS_LOG_BUF_SIZE           0x0C
#define CONS_LOG_IDX                0x10
#define CONS_DUMP_MAX               3072

/* Module identification from OTP CIS (dhd dhd_pcie.c:2265-2277, 2405-2489; dhd_custom_cis.c:803-825).
 * Samsung's dhd writes these two registers before reading the CIS; the driver only does so when the
 * EROM puts the GCI core at OTP_GCI_CORE_BASE and the PMU core at OTP_PMU_CORE_BASE. */
#define OTP_GCI_CORE_BASE           0x18010000UL
#define OTP_PMU_CORE_BASE           0x18012000UL
#define OTP_CTRL1_ADDR              0x18010324UL
#define OTP_CTRL1_VALUE             0x00FA0000UL
#define PMU_MIN_RES_MASK_ADDR       0x18012618UL   /* PMU min_res_mask, sbchipc.h:63 */
#define PMU_MIN_RES_MASK_VALUE      0x0E4FFFFFUL
#define CIS_TUPLE_START             0x18011120UL
#define CIS_TUPLE_END               0x18011177UL
#define CIS_TUPLE_TAG_START         0x80
#define CIS_TUPLE_TAG_VENDOR        0x81
#define CIS_TUPLE_TAG_BOARDTYPE     0x1B

/* Blob location (copied there by drivers/S10eDiag/esp/extra-wlan2.cmd). */
#define WLAN_BLOB_DIR               L"\\SystemRoot\\System32\\drivers\\bcm4375\\"
#define WLAN_FW_DEFAULT             L"bcmdhd_sta.bin_b1"
#define WLAN_NVRAM_MURATA_1RH_ES43  L"nvram.txt_1rh_es43_b1"
#define WLAN_NVRAM_SEMCO_CS01       L"nvram.txt_CS01_semco_b1"
#define WLAN_NVRAM_DEFAULT          WLAN_NVRAM_MURATA_1RH_ES43   /* choice, not a fact: notes "nvram" */
#define WLAN_FW_MAX_SIZE            (WLAN_BAR2_SIZE)

/* DlResult values (README.md). */
#define DL_OK                       0
#define DL_SKIPPED                  1
#define DL_BACKPLANE                2
#define DL_BLOB                     3
#define DL_SIZE                     4
#define DL_BME_SET                  5
#define DL_VERIFY                   6
#define DL_TIMEOUT                  7
#define DL_BAD_SHARED               8

#endif /* _EXYNOS9820_WLAN_H */
