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
#define MFC_REG_MFC_BUS_STATUS       0x7018u     // mfc_regs.h:35 (mfc_risc_off waits for 0)
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
#define MFC_REG_D_DISPLAY_CROP_INFO1 0xF61Cu
#define MFC_REG_D_DISPLAY_CROP_INFO2 0xF620u
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
#define MFC_TEST_AREA_OFFSET         (8u << 20)  // boot self-test session, inside the firmware allocation
#define MFC_TEST_AREA_SIZE           (8u << 20)
#define MFC_SESSION_OFFSET           (16u << 20) // user session region (V3), above the RISC base like everything else
#define MFC_SESSION_SIZE_MAX         (112u << 20)
#define MFC_SESSION_SIZE_MIN         (32u << 20)

// Session region layout (Decode.c): instance context, CPB, then codec (scratch + MV) and DPB slots sized at
// SEQ_HEADER time from the firmware's minimums.
#define MFC_INST_SIZE                0x200000u   // mfc.c h264_dec_ctx = PAGE_ALIGN(0x200000)
#define MFC_CPB_OFFSET               0x200000u
#define MFC_CODEC_OFFSET_SMALL       (3u << 20)  // test area: CPB 1 MiB
#define MFC_CODEC_OFFSET_LARGE       (6u << 20)  // session: CPB 4 MiB (MFC_MAX_WINDOW)
#define MFC_MAX_DPB                  32u         // D_*_DPB_FLAG_LOWER: 32 slots

// 1: the whole MFC allocation is cached and every CPU<->MFC hand-over goes through KeFlushIoBuffers (a real routine
// on ARM64, wdm.h; the MFC is treated as non-coherent). 0: non-cached (V-W2/V3 vidc4-6: correct, but reading a
// 1080p picture out of it cost ~30 ms).
#ifndef MFC_CACHED
#define MFC_CACHED                   1
#endif
#define MFC_CACHE_TYPE               (MFC_CACHED ? MmCached : MmNonCached)

#define MFC_H2R_CMD_SYS_INIT         1u
#define MFC_R2H_SYS_INIT_RET         1u
#define MFC_R2H_FW_STATUS_RET        12u
#define MFC_IP_VER                   0x1300u

#define MFC_DEV_CTX_SIZE             0x8000u     // mfc.c mfc_ctx_buf_size.dev_ctx = PAGE_ALIGN(0x7800)
#define MFC_FW_BUFFER_SIZE           (16u << 20) // fw code + common ctx + the self-test area (vidc3: buffers below the base -> ERR 10)
#define MFC_CTX_OFFSET               (1u << 20)  // mfc_buf.c: common ctx at fw_daddr + firmware_code (1 MiB)
#define MFC_BUFFER_ALIGN             (1u << 20)
#define MFC_FW_PATH                  L"\\SystemRoot\\System32\\drivers\\mfc_fw.bin"
#define MFC_POOL_TAG                 'cfMx'

typedef struct _MFC_SESSION
{
    PUCHAR   Va;              // region (non-cached) and its 32-bit device address
    ULONG    Pa;
    ULONG    Size;
    ULONG    CpbSize;
    ULONG    CodecOffset;
    BOOLEAN  Opened;
    BOOLEAN  Ready;           // INIT_BUFFERS done
    BOOLEAN  Ended;           // drain reported DECODING_EMPTY
    ULONG    InstNo;
    ULONG    Width;
    ULONG    Height;
    ULONG    Stride;
    ULONG    CropLeft, CropTop, CropRight, CropBottom;
    ULONG    LumaSize;
    ULONG    ChromaOffset;    // page aligned inside the slot (vendor: each plane its own page-aligned buffer)
    ULONG    ChromaSize;
    ULONG    SlotSize;
    ULONG    DpbOffset;
    ULONG    NumDpb;
    ULONG    Available;
    ULONG    Used;
    ULONG    Frames;
} MFC_SESSION, *PMFC_SESSION;

typedef struct _MFC_RESULT    // one Decode step (MfcSessionStep)
{
    ULONG    Flags;           // MFC_DEC_*
    ULONG    Consumed;
    ULONG    R2h;
    ULONG    Error;
    ULONG    DisplayStatus;
    ULONG    FrameType;
    PUCHAR   Frame;           // displayed slot (luma at +0, chroma at +ChromaOffset) or NULL
    ULONG    Microseconds;
} MFC_RESULT, *PMFC_RESULT;

typedef struct _MFC_DEVICE
{
    PUCHAR            Regs;
    SIZE_T            RegsLength;
    WDFINTERRUPT      Interrupt;
    PVOID             BufferAlloc;      // MmAllocateContiguousMemorySpecifyCache result (unaligned)
    SIZE_T            BufferAllocSize;
    PUCHAR            Buffer;           // 1 MiB aligned firmware buffer (non-cached), session region after it
    PHYSICAL_ADDRESS  BufferPa;
    ULONG             SessionSize;      // 0 when the large allocation failed (self-test only)
    BOOLEAN           FirmwareUp;
    BOOLEAN           InterruptsOn;     // commands complete through the ISR/DPC instead of polling
    KEVENT            CmdDone;
    volatile LONG     IsrCount;
    volatile ULONG    LastR2hCmd;
    volatile ULONG    LastError;
    WDFWAITLOCK       SessionLock;      // one command sequence at a time
    WDFFILEOBJECT     SessionOwner;     // handle that owns the user session (NULL = free)
    MFC_SESSION       Session;
} MFC_DEVICE, *PMFC_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MFC_DEVICE, MfcGetDevice)

#define MfcTrace(...)  DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Exynos9820Mfc: " __VA_ARGS__)

static __forceinline ULONG Rd(PMFC_DEVICE M, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(M->Regs + Off)); }
static __forceinline VOID Wr(PMFC_DEVICE M, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(M->Regs + Off), V); }

// Decode.c
ULONG    MfcPollR2h(_In_ PMFC_DEVICE M, _In_ ULONG TimeoutMs);
VOID     MfcSessionInit(_Out_ PMFC_SESSION S, _In_ PUCHAR Va, _In_ ULONG Pa, _In_ ULONG Size, _In_ ULONG CpbSize);
NTSTATUS MfcSessionOpen(_In_ PMFC_DEVICE M, _Inout_ PMFC_SESSION S);
NTSTATUS MfcSessionStep(_In_ PMFC_DEVICE M, _Inout_ PMFC_SESSION S, _In_ ULONG Length, _Out_ PMFC_RESULT R);
VOID     MfcSessionClose(_In_ PMFC_DEVICE M, _Inout_ PMFC_SESSION S, _In_ BOOLEAN LeavePending);
VOID     MfcFlush(_In_ PVOID Va, _In_ ULONG Length, _In_ BOOLEAN FromDevice);
ULONG    MfcFnv(_In_ const volatile UCHAR *Base, _In_ ULONG Width, _In_ ULONG Rows, _In_ ULONG Stride);
