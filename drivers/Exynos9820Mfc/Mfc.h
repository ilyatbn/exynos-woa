/*
 * Mfc.h - Exynos 9820 MFC v13 (ip_ver 0x1300) video codec, Windows driver V-W1 (notes/d10-mfc-main.md).
 * Register offsets: Samsung mfc_regs.h (LineageOS android_kernel_samsung_exynos9820, lineage-22.2), reference only.
 * The boot + SYS_INIT sequence is the one UEFI Beyond0LteMfcDxe proved on the device (R3/R4).
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>

#define MFC_REG_RISC_ON              0x0000u
#define MFC_REG_RISC2HOST_INT        0x003Cu
#define MFC_REG_HOST2RISC_INT        0x0044u
#define MFC_REG_RISC_BASE_ADDRESS    0x0054u
#define MFC_REG_MFC_RESET            0x1070u
#define MFC_REG_HOST2RISC_CMD        0x1100u
#define MFC_REG_RISC2HOST_CMD        0x1104u
#define MFC_REG_MFC_OFF              0x7120u
#define MFC_REG_FW_VERSION           0xF000u
#define MFC_REG_CONTEXT_MEM_ADDR     0xF014u
#define MFC_REG_CONTEXT_MEM_SIZE     0xF018u
#define MFC_REG_MFC_VERSION          0xF028u
#define MFC_REG_REG_CLEAR_BEGIN      0xF000u
#define MFC_REG_REG_CLEAR_COUNT      1024u
#define MFC_WINDOW_MIN               0x10000u

// V-W2 plain-mode decode (notes/d10-mfc-decode-flow.md; offsets mfc_regs.h, cited there)
#define MFC_REG_INSTANCE_ID          0xF008u
#define MFC_REG_CODEC_TYPE           0xF00Cu
#define MFC_REG_PIXEL_FORMAT         0xF020u
#define MFC_REG_CODEC_CONTROL        0xF038u
#define MFC_REG_DEC_TIMEOUT_VALUE    0xF03Cu
#define MFC_REG_RET_INSTANCE_ID      0xF070u
#define MFC_REG_ERROR_CODE           0xF074u
#define MFC_REG_D_CRC_CTRL           0xF0B0u
#define MFC_REG_D_DEC_OPTIONS        0xF0B4u
#define MFC_REG_D_SEI_ENABLE         0xF0C4u
#define MFC_REG_D_FORCE_PIXEL_VAL    0xF0C8u
#define MFC_REG_D_MIN_NUM_DPB        0xF0F0u
#define MFC_REG_D_MIN_FIRST_PLANE    0xF0F4u
#define MFC_REG_D_MIN_SECOND_PLANE   0xF0F8u
#define MFC_REG_D_MIN_NUM_MV         0xF100u
#define MFC_REG_D_MIN_SCRATCH_SIZE   0xF108u
#define MFC_REG_D_NUM_DPB            0xF130u
#define MFC_REG_D_NUM_MV             0xF134u
#define MFC_REG_D_FIRST_STRIDE       0xF138u
#define MFC_REG_D_SECOND_STRIDE      0xF13Cu
#define MFC_REG_D_FIRST_PLANE_SIZE   0xF144u
#define MFC_REG_D_SECOND_PLANE_SIZE  0xF148u
#define MFC_REG_D_MV_BUFFER_SIZE     0xF150u
#define MFC_REG_D_INIT_BUF_OPTIONS   0xF154u
#define MFC_REG_D_FIRST_PLANE_DPB0   0xF160u
#define MFC_REG_D_SECOND_PLANE_DPB0  0xF260u
#define MFC_REG_D_MV_BUFFER0         0xF460u
#define MFC_REG_D_SCRATCH_ADDR       0xF560u
#define MFC_REG_D_SCRATCH_SIZE       0xF564u
#define MFC_REG_D_NAL_START_OPTIONS  0xF5ACu
#define MFC_REG_D_CPB_ADDR           0xF5B0u
#define MFC_REG_D_CPB_SIZE           0xF5B4u
#define MFC_REG_D_AVAIL_DPB_UPPER    0xF5B8u
#define MFC_REG_D_AVAIL_DPB_LOWER    0xF5BCu
#define MFC_REG_D_CPB_OFFSET         0xF5C0u
#define MFC_REG_D_SLICE_IF_ENABLE    0xF5C4u
#define MFC_REG_D_STREAM_DATA_SIZE   0xF5D0u
#define MFC_REG_D_DYN_DPB_UPPER      0xF5D4u
#define MFC_REG_D_DYN_DPB_LOWER      0xF5D8u
#define MFC_REG_D_DISPLAY_WIDTH      0xF600u
#define MFC_REG_D_DISPLAY_HEIGHT     0xF604u
#define MFC_REG_D_DISPLAY_STATUS     0xF608u
#define MFC_REG_D_DISPLAY_LUMA       0xF60Cu
#define MFC_REG_D_DISPLAY_CHROMA     0xF610u
#define MFC_REG_D_DISPLAY_TYPE       0xF618u
#define MFC_REG_D_DECODED_LUMA       0xF648u
#define MFC_REG_D_DECODED_NAL_SIZE   0xF664u
#define MFC_REG_D_USED_DPB_LOWER     0xF724u

#define MFC_H2R_OPEN_INSTANCE        2u
#define MFC_H2R_SEQ_HEADER           3u
#define MFC_H2R_INIT_BUFFERS         4u
#define MFC_H2R_NAL_START            5u
#define MFC_H2R_CLOSE_INSTANCE       6u
#define MFC_H2R_LAST_FRAME           9u
#define MFC_R2H_REASON_MASK          0x1FFFFu
#define MFC_R2H_ERR_RET              32u
#define MFC_R2H_FRAME_DONE_RET       13u

#define MFC_TEST_PATH                L"\\SystemRoot\\System32\\drivers\\mfc_test.h264"
#define MFC_TEST_BUFFER_SIZE         (8u << 20)
#define MFC_TEST_INST_SIZE           0x200000u   // mfc.c h264_dec_ctx = PAGE_ALIGN(0x200000)
#define MFC_TEST_CPB_OFFSET          (2u << 20)
#define MFC_TEST_CPB_SIZE            (1u << 20)
#define MFC_TEST_CODEC_OFFSET        (3u << 20)
#define MFC_TEST_CODEC_SIZE          (2u << 20)
#define MFC_TEST_DPB_OFFSET          (5u << 20)
#define MFC_TEST_DPB_SIZE            (3u << 20)
#define MFC_TEST_MAX_DPB             16u

#define MFC_H2R_CMD_SYS_INIT         1u
#define MFC_R2H_SYS_INIT_RET         1u
#define MFC_R2H_FW_STATUS_RET        12u
#define MFC_IP_VER                   0x1300u

#define MFC_DEV_CTX_SIZE             0x8000u     // mfc.c mfc_ctx_buf_size.dev_ctx = PAGE_ALIGN(0x7800)
#define MFC_FW_BUFFER_SIZE           (8u << 20)  // image + firmware working memory (UEFI R3/R4 used the same)
#define MFC_CTX_OFFSET               (4u << 20)  // common context buffer inside the firmware buffer
#define MFC_BUFFER_ALIGN             (1u << 20)
#define MFC_FW_PATH                  L"\\SystemRoot\\System32\\drivers\\mfc_fw.bin"
#define MFC_POOL_TAG                 'cfMx'

typedef struct _MFC_DEVICE
{
    PUCHAR            Regs;
    SIZE_T            RegsLength;
    WDFINTERRUPT      Interrupt;
    PVOID             BufferAlloc;      // MmAllocateContiguousMemorySpecifyCache result (unaligned)
    PUCHAR            Buffer;           // 1 MiB aligned firmware buffer (non-cached)
    PHYSICAL_ADDRESS  BufferPa;
    BOOLEAN           FirmwareUp;
    volatile LONG     IsrCount;
    volatile ULONG    LastR2hCmd;
} MFC_DEVICE, *PMFC_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MFC_DEVICE, MfcGetDevice)

#define MfcTrace(...)  DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Exynos9820Mfc: " __VA_ARGS__)
