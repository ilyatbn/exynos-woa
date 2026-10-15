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
