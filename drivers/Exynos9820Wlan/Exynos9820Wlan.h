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

/* ======================================================================
 * Milestone 3: msgbuf common rings, bus mastering (polled), first ioctl
 * (notes/m12-wifi-m3.md carries the citation table; dhd = bcmdhd lineage-23.2,
 * fmac = brcmfmac master). Facts only, no code copied.
 * ==================================================================== */

/* More pciedev_shared_t members (dhd include/bcmpcie.h:330-395). */
#define SH_TOTAL_LFRAG_CNT          0x20           /* u16 */
#define SH_MAX_HOST_RXBUFS          0x22           /* u16 */
#define SH_SCRATCH_LEN              0x34
#define SH_SCRATCH_ADDR             0x38           /* sh_addr_t */
#define SH_FLAGS2                   0x50
#define SH_HOST_CAP                 0x54
#define SH_SIZE_V7                  0x78           /* through host_cap3 */

/* pcie_shared flags (bcmpcie.h:67-124). */
#define PCIE_SHARED_DMA_INDEX       0x00010000UL
#define PCIE_SHARED_D2H_SYNC_SEQNUM 0x00020000UL
#define PCIE_SHARED_D2H_SYNC_XORCSUM 0x00040000UL
#define PCIE_SHARED_2BYTE_INDICES   0x00100000UL
#define PCIE_SHARED_HOSTRDY_SUPPORT 0x10000000UL
#define PCIE_SHARED_DAR             0x80000000UL
#define PCIE_SHARED_VERSION_7       7

/* host_cap bits (bcmpcie.h:403-418) and the value the driver writes by default (notes "host_cap"). */
#define HOSTCAP_H2D_ENABLE_HOSTRDY  0x00000400UL
#define HOSTCAP_DS_NO_OOB_DW        0x00001000UL
#define HOSTCAP_UR_FW_NO_TRAP       0x00800000UL
#define WLAN_HOSTCAP_AUTO           0xFFFFFFFEUL   /* registry default: compute like dhd_set_host_cap */
#define WLAN_HOSTCAP_DONT_WRITE     0xFFFFFFFFUL   /* registry: leave host_cap alone (fmac style) */

/* ring_info_t (bcmpcie.h:290-324). */
#define RI_RINGMEM_PTR              0x00
#define RI_H2D_W_IDX_PTR            0x04
#define RI_H2D_R_IDX_PTR            0x08
#define RI_D2H_W_IDX_PTR            0x0C
#define RI_D2H_R_IDX_PTR            0x10
#define RI_MAX_TX_FLOWRINGS         0x34           /* u16 */
#define RI_MAX_SUBMISSION_QUEUES    0x36           /* u16 */
#define RI_MAX_COMPLETION_RINGS     0x38           /* u16 */
#define RI_MAX_VDEVS                0x3A           /* u16 */
#define RI_SIZE                     0x3C

/* ring_mem_t (bcmpcie.h:275-282): 16 bytes per common ring at ringmem_ptr + 16 * ring id. */
#define RM_SIZE                     16
#define RM_IDX                      0x00           /* u16 */
#define RM_TYPE                     0x02           /* u8 */
#define RM_MAX_ITEM                 0x04           /* u16 */
#define RM_LEN_ITEMS                0x06           /* u16 */
#define RM_BASE_ADDR                0x08           /* sh_addr_t */
#define RW_INDEX_SZ                 4              /* flags lack 2BYTE_INDICES (dhd_pcie.c:8597-8598) */

/* Common rings: ids (bcmpcie.h:171-175), item sizes + depths (dhd include/bcmmsgbuf.h:47-77, v7 sizes). */
#define RING_H2D_CTRL               0
#define RING_H2D_RXPOST             1
#define RING_D2H_CTRL_CPL           2
#define RING_D2H_TX_CPL             3
#define RING_D2H_RX_CPL             4
#define RING_COMMON_COUNT           5
#define RING_H2D_COUNT              2
#define H2D_CTRL_ITEMSIZE           40
#define H2D_CTRL_MAX_ITEM           64
#define H2D_RXPOST_ITEMSIZE         32
#define H2D_RXPOST_MAX_ITEM         512
#define D2H_CTRL_CPL_ITEMSIZE       24
#define D2H_CTRL_CPL_MAX_ITEM       64
#define D2H_TX_CPL_ITEMSIZE         24
#define D2H_TX_CPL_MAX_ITEM         1024
#define D2H_RX_CPL_ITEMSIZE         40
#define D2H_RX_CPL_MAX_ITEM         512

/* Message types (bcmmsgbuf.h:160-177) and header layout (cmn_msg_hdr_t :145-156). */
#define MSG_TYPE_GEN_STATUS         0x01
#define MSG_TYPE_RING_STATUS        0x02
#define MSG_TYPE_IOCTLPTR_REQ       0x09
#define MSG_TYPE_IOCTLPTR_REQ_ACK   0x0A
#define MSG_TYPE_IOCTLRESP_BUF_POST 0x0B
#define MSG_TYPE_IOCTL_CMPLT        0x0C
#define MSG_TYPE_WL_EVENT           0x0E
#define MSGH_TYPE                   0x00           /* u8 */
#define MSGH_IFID                   0x01           /* u8 */
#define MSGH_FLAGS                  0x02           /* u8 */
#define MSGH_EPOCH                  0x03           /* u8 */
#define MSGH_REQUEST_ID             0x04           /* u32 */
/* ioctl_req_msg_t (bcmmsgbuf.h:313-330) */
#define IOREQ_CMD                   0x08
#define IOREQ_TRANS_ID              0x0C           /* u16 */
#define IOREQ_INPUT_LEN             0x0E           /* u16 */
#define IOREQ_OUTPUT_LEN            0x10           /* u16 */
#define IOREQ_BUF_ADDR              0x18           /* bcm_addr64_t */
/* ioctl_resp_evt_buf_post_msg_t (bcmmsgbuf.h:333-343) */
#define BUFPOST_LEN                 0x08           /* u16 */
#define BUFPOST_ADDR                0x10           /* bcm_addr64_t */
/* D2H completions (bcmmsgbuf.h:584-756): compl_msg_hdr_t at +8, XOR marker in the last word */
#define CPL_STATUS                  0x08           /* s16 */
#define CPL_RING_ID                 0x0A           /* u16 */
#define CPL_ACK_CMD                 0x0C           /* ioctl_req_ack_msg_t.cmd */
#define CPL_IOCTL_RESP_LEN          0x0C           /* ioctl_comp_resp_msg_t, u16 */
#define CPL_IOCTL_TRANS_ID          0x0E           /* u16 */
#define CPL_IOCTL_CMD               0x10           /* u32 */
#define EPOCH_MODULO                253            /* D2H/H2D_EPOCH_MODULO bcmmsgbuf.h:41,44 */
#define EPOCH_INIT_VAL              254            /* *_EPOCH_INIT_VAL bcmmsgbuf.h:42,45 */

#define IOCTL_REQ_PKTID             0xFFFEUL       /* DHD_IOCTL_REQ_PKTID dhd_msgbuf.c:1670 */
#define IOCTL_REQ_BUF_SIZE          2048           /* DHD_IOCTL_REQ_PKTBUFSZ dhd_msgbuf.c:114 */
#define IOCTL_REQ_MAX_INPUT         (IOCTL_REQ_BUF_SIZE - H2D_CTRL_ITEMSIZE)   /* dhd_msgbuf.c:115 */
#define IOCTL_RESP_BUF_SIZE         8192           /* DHD_FLOWRING_IOCTL_BUFPOST_PKTSZ dhd_msgbuf.c:128 */
#define IOCTL_RESP_BUF_COUNT        8              /* DHD_FLOWRING_MAX_IOCTLRESPBUF_POST dhd_msgbuf.c:132 */
#define D2H_SCRATCH_LEN             8              /* DMA_D2H_SCRATCH_BUF_LEN dhd_msgbuf.c:119 */
#define WLC_GET_VAR                 262            /* fmac fwil.h:80 BRCMF_C_GET_VAR */
#define IOCTL_RESP_TIMEOUT_MS       2000           /* fmac msgbuf.c:28 MSGBUF_IOCTL_RESP_TIMEOUT */

/* PCIE2 core registers, through the BAR0 window at the PCIE2 core base (dhd include/pcie_core.h). */
#define PCIE2_H2D_DB0               0x140          /* PCIH2D_MailBox, pcie_core.h:946 */
#define PCIE2_H2D_DB1               0x144          /* PCIH2D_DB1, pcie_core.h:947 */
#define PCIE2_DAR64_H2D_DB0         0xA20          /* dar_64.h2d_db_0_0, pcie_core.h:335 (fmac pcie.c:149) */
#define PCIE2_DAR64_H2D_DB1         0xA24          /* fmac pcie.c:150 */
#define PCIE2_MAILBOXINT_REV64      0xC30          /* PCIMailBoxInt(rev >= 64), pcie_core.h:961 */
#define PCIE2_MAILBOXINT_OLD        0x48           /* PCIMailBoxInt(rev < 64) */
#define PCIE2_DOORBELL_VALUE        0x12345678UL   /* dhd ringbell dhd_pcie.c:7786, hostready :6160 */

/* One common buffer, every region 4 KB aligned (notes "DMA decision" 6). */
#define MSG_OFF_H2D_CTRL            0x00000
#define MSG_OFF_H2D_RXPOST          0x01000        /* 512 x 32 = 0x4000 */
#define MSG_OFF_D2H_CTRL_CPL        0x05000        /* 64 x 24 = 0x600 */
#define MSG_OFF_D2H_TX_CPL          0x06000        /* 1024 x 24 = 0x6000 */
#define MSG_OFF_D2H_RX_CPL          0x0C000        /* 512 x 40 = 0x5000 */
#define MSG_OFF_IOCTL_REQ           0x11000        /* 2048 */
#define MSG_OFF_SCRATCH             0x12000        /* 8 */
#define MSG_OFF_IOCTL_RESP          0x13000        /* 8 x 8192 = 0x10000 */
#define MSG_OFF_EVENT_BUF           0x23000        /* milestone 4: 8 x 2048 = 0x4000 */
#define MSG_DMA_SIZE                0x27000

/* MsgResult values (README.md). */
#define MSG_OK                      0
#define MSG_SKIPPED                 1
#define MSG_NO_DMA                  2
#define MSG_BAD_SHARED              3
#define MSG_BME                     4
#define MSG_TIMEOUT                 5
#define MSG_IOCTL_ERROR             6
#define MSG_TRAP                    7

/* ---- milestone 4: events + escan (notes/m12-wifi-m4.md; dhd = bcmdhd lineage-23.2, fmac = brcmfmac master) ---- */
#define MSG_TYPE_EVENT_BUF_POST     0x0D           /* bcmmsgbuf.h:176 */
#define IOCTL_RESP_PKTID0           0x1000         /* host-chosen buffer ids (any value except 0xFFFE) */
#define EVENT_BUF_PKTID0            0x2000
#define EVENT_BUF_COUNT             8              /* fmac msgbuf.c:60 BRCMF_MSGBUF_MAX_EVENTBUF_POST */
#define EVENT_BUF_SIZE              2048           /* fmac msgbuf.c:56 BRCMF_MSGBUF_MAX_PKT_SIZE */
#define EVTCPL_DATA_LEN             0x0C           /* wlevent_req_msg_t.event_data_len u16, bcmmsgbuf.h:708-721 */
/* bcm event packet, big-endian header fields (fmac fweh.h:202-248) */
#define EVT_OUI                     19             /* brcm_ethhdr.oui[3] = 00 10 18 (BRCM_OUI fweh.h:202) */
#define EVT_USR_SUBTYPE             22             /* be16 */
#define BCMILCP_BCM_SUBTYPE_EVENT   1              /* fweh.h:203 */
#define EVT_TYPE                    28
#define EVT_STATUS                  32
#define EVT_REASON                  36
#define EVT_DATALEN                 44
#define EVT_DATA                    72
#define WLC_E_ESCAN_RESULT          69             /* dhd bcmevent.h:184, fmac fweh.h:89 */
#define WLC_E_STATUS_PARTIAL        8              /* dhd bcmevent.h:330 */
#define WL_EVENTING_MASK_LEN        16             /* dhd wlioctl_defs.h:1438 */
#define WLC_UP                      2              /* fmac fwil.h:15 BRCMF_C_UP */
#define WLC_SET_VAR                 263            /* fmac fwil.h:81 BRCMF_C_SET_VAR */
/* escan request v1: brcmf_escan_params_le (fmac fwil_types.h:460) + brcmf_scan_params_le (:368) */
#define ESCAN_VERSION               0              /* u32 */
#define ESCAN_ACTION                4              /* u16 */
#define ESCAN_SYNC_ID               6              /* u16 */
#define ESCAN_PARAMS                8
#define ESCAN_REQ_VERSION           1              /* dhd wlioctl.h:871, fmac fwil_types.h:73 */
#define WL_SCAN_ACTION_START        1              /* dhd wlioctl_defs.h:307 */
#define SCANP_BSSID                 36
#define SCANP_BSS_TYPE              42
#define SCANP_SCAN_TYPE             43
#define SCANP_NPROBES               44
#define SCANP_ACTIVE_TIME           48
#define SCANP_PASSIVE_TIME          52
#define SCANP_HOME_TIME             56
#define SCANP_FIXED_SIZE            64             /* fmac fwil_types.h:50, dhd wlioctl.h:907 */
#define ESCAN_PARAMS_V1_SIZE        (ESCAN_PARAMS + SCANP_FIXED_SIZE)
/* v2: brcmf_scan_params_v2_le (fmac fwil_types.h:409), natural alignment */
#define ESCAN_REQ_VERSION_V2        2              /* fmac fwil_types.h:74 */
#define SCAN_PARAMS_VERSION_V2      2              /* fmac fwil_types.h:54 */
#define SCANP2_VERSION              0              /* u16 */
#define SCANP2_LENGTH               2              /* u16 = fixed size + channels/ssids (none here) */
#define SCANP2_BSSID                40
#define SCANP2_BSS_TYPE             46
#define SCANP2_SCAN_TYPE            48             /* u32 in v2 */
#define SCANP2_NPROBES              52
#define SCANP2_ACTIVE_TIME          56
#define SCANP2_PASSIVE_TIME         60
#define SCANP2_HOME_TIME            64
#define SCANP2_FIXED_SIZE           72             /* fmac fwil_types.h:51 BRCMF_SCAN_PARAMS_V2_FIXED_SIZE */
#define ESCAN_PARAMS_V2_SIZE        (ESCAN_PARAMS + SCANP2_FIXED_SIZE)
#define MIN_ESCAN_PARAM_V2_FW_MAJOR 14             /* dhd wl_cfg80211.h:833 */
#define WLC_VER_SIZE                16             /* wl_wlc_version_t, dhd wlioctl.h:14092-14104 (8 x u16) */
#define WLC_VER_MAJOR               12             /* wlc_ver_major u16 (version, length, 4 x epi, major, minor) */
#define DOT11_BSSTYPE_ANY           2              /* fmac fwil_types.h:72 */
#define SCANTYPE_ACTIVE             0              /* fmac fwil_types.h:62 */
/* escan result (fmac fwil_types.h:470) and wl_bss_info v109 (dhd wlioctl.h:384-424) */
#define ESCAN_RES_BSS_COUNT         10             /* u16 */
#define ESCAN_RES_BSS_INFO          12
#define BSS_LENGTH                  4              /* u32, record length incl. IEs */
#define BSS_BSSID                   8
#define BSS_SSID_LEN                18
#define BSS_SSID                    19
#define BSS_CHANSPEC                72
#define BSS_RSSI                    78             /* s16 dBm */
#define BSS_CTL_CH                  88
#define BSS_INFO_MIN_LEN            128            /* v109 fixed part (ie_offset default) */
#define SCAN_MAX_BSS                64             /* distinct BSSIDs logged */
#define SCAN_TIMEOUT_MS             10000
#define SCAN_OK                     0
#define SCAN_FAILED                 1
#define SCAN_TIMEOUT                2
#define SCAN_SKIPPED                3
#define SCAN_FW_STATUS              4

#endif /* _EXYNOS9820_WLAN_H */
