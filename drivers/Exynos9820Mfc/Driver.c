/*
 * Driver.c - Exynos9820Mfc V-W1 (D10, notes/d10-mfc-main.md): binds ACPI\SAMS982A (DSDT VIDC), boots the MFC v13
 * firmware and runs SYS_INIT, then proves the R2H interrupt (GSIV 370). V-W2: polled H.264 decode
 * self-test of mfc_test.h264 (next to mfc_fw.bin) when that file exists - MfcDecodeTest.
 *
 * Sequence = vendor __mfc_init_hw (mfc_run.c) as proven from UEFI (Beyond0LteMfcDxe R3/R4): reset (R2H/H2R/FW_VERSION
 * cleared, 1024 words from 0xF000 cleared, MFC_RESET 0x1FFF then 0), RISC_BASE_ADDRESS = buffer physical address
 * (SYSMMUs left in reset bypass), RISC_ON 1 / MFC_OFF 0, R2H FW_STATUS_RET 12, then CONTEXT_MEM_ADDR/SIZE + H2R
 * SYS_INIT, R2H SYS_INIT_RET 1. The last R2H interrupt is deliberately left pending: once WDF connects the interrupt
 * (after D0Entry) the ISR must fire - that is the interrupt-path proof.
 */
#include "Mfc.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MfcEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE MfcEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE MfcEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED MfcEvtD0EntryPostInterruptsEnabled;
EVT_WDF_INTERRUPT_ISR MfcEvtIsr;
EVT_WDF_INTERRUPT_DPC MfcEvtDpc;

static __forceinline ULONG Rd(PMFC_DEVICE M, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(M->Regs + Off)); }
static __forceinline VOID Wr(PMFC_DEVICE M, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(M->Regs + Off), V); }

static VOID
MfcReset(_In_ PMFC_DEVICE M)
{
    ULONG Index;

    Wr(M, MFC_REG_RISC2HOST_CMD, 0);
    Wr(M, MFC_REG_HOST2RISC_CMD, 0);
    Wr(M, MFC_REG_FW_VERSION, 0);
    for (Index = 0; Index < MFC_REG_REG_CLEAR_COUNT; Index++)
    {
        Wr(M, MFC_REG_REG_CLEAR_BEGIN + Index * 4, 0);
    }

    Wr(M, MFC_REG_MFC_RESET, 0x1FFF);
    Wr(M, MFC_REG_MFC_RESET, 0);
}

/** Poll R2H (interrupts not connected yet) for up to TimeoutMs; returns the R2H command or 0. **/
static ULONG
MfcPollR2h(_In_ PMFC_DEVICE M, _In_ ULONG TimeoutMs)
{
    ULONG Ms;

    for (Ms = 0; Ms < TimeoutMs; Ms++)
    {
        if (Rd(M, MFC_REG_RISC2HOST_INT) != 0)
        {
            return Rd(M, MFC_REG_RISC2HOST_CMD);
        }

        KeStallExecutionProcessor(1000);
    }

    return 0;
}

static NTSTATUS
MfcLoadFile(_In_ PCWSTR Path, _Out_writes_bytes_(MaxSize) PUCHAR Dest, _In_ ULONG MaxSize, _Out_ PULONG Size)
{
    UNICODE_STRING            Name;
    OBJECT_ATTRIBUTES         Attr;
    IO_STATUS_BLOCK           Iosb;
    HANDLE                    File;
    FILE_STANDARD_INFORMATION Info;
    NTSTATUS                  Status;

    *Size = 0;
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attr, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Attr, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(Status))
    {
        MfcTrace("open %ws 0x%08X\n", Path, Status);
        return Status;
    }

    Status = ZwQueryInformationFile(File, &Iosb, &Info, sizeof(Info), FileStandardInformation);
    if (NT_SUCCESS(Status) && ((Info.EndOfFile.QuadPart <= 0) || (Info.EndOfFile.QuadPart > MaxSize)))
    {
        Status = STATUS_INVALID_IMAGE_FORMAT;
    }

    if (NT_SUCCESS(Status))
    {
        *Size  = (ULONG)Info.EndOfFile.QuadPart;
        Status = ZwReadFile(File, NULL, NULL, NULL, &Iosb, Dest, *Size, NULL, NULL);
    }

    ZwClose(File);
    return Status;
}

static NTSTATUS
MfcBoot(_In_ PMFC_DEVICE M)
{
    PHYSICAL_ADDRESS Low;
    PHYSICAL_ADDRESS High;
    PHYSICAL_ADDRESS Boundary;
    ULONG            FwSize;
    ULONG            R2h;
    NTSTATUS         Status;

    Low.QuadPart      = 0;
    High.QuadPart     = 0xFFFFFFFFLL;   // the MFC takes 32-bit addresses (raw daddr writes in the vendor driver)
    Boundary.QuadPart = 0;
    M->BufferAlloc = MmAllocateContiguousMemorySpecifyCache(MFC_FW_BUFFER_SIZE + MFC_BUFFER_ALIGN, Low, High, Boundary,
                                                            MmNonCached);
    if (M->BufferAlloc == NULL)
    {
        MfcTrace("no contiguous buffer below 4 GiB\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    {
        PHYSICAL_ADDRESS Pa = MmGetPhysicalAddress(M->BufferAlloc);
        ULONG Pad = (ULONG)(((Pa.QuadPart + MFC_BUFFER_ALIGN - 1) & ~(LONGLONG)(MFC_BUFFER_ALIGN - 1)) - Pa.QuadPart);

        M->Buffer            = (PUCHAR)M->BufferAlloc + Pad;
        M->BufferPa.QuadPart = Pa.QuadPart + Pad;
    }

    RtlZeroMemory(M->Buffer, MFC_FW_BUFFER_SIZE);
    Status = MfcLoadFile(MFC_FW_PATH, M->Buffer, MFC_CTX_OFFSET, &FwSize);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    MfcReset(M);
    Wr(M, MFC_REG_RISC_BASE_ADDRESS, M->BufferPa.LowPart);
    Wr(M, MFC_REG_RISC2HOST_CMD, 0);
    Wr(M, MFC_REG_RISC2HOST_INT, 0);
    Wr(M, MFC_REG_RISC_ON, 1);
    Wr(M, MFC_REG_MFC_OFF, 0);
    R2h = MfcPollR2h(M, 2000);
    MfcTrace("fw %u B at PA 0x%llx: R2H %u (FW_STATUS_RET %u), MFC_VERSION 0x%x, FW_VERSION 0x%08x\n", FwSize,
             M->BufferPa.QuadPart, R2h, MFC_R2H_FW_STATUS_RET, Rd(M, MFC_REG_MFC_VERSION), Rd(M, MFC_REG_FW_VERSION));
    if ((R2h != MFC_R2H_FW_STATUS_RET) || (Rd(M, MFC_REG_MFC_VERSION) != MFC_IP_VER))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    Wr(M, MFC_REG_RISC2HOST_CMD, 0);
    Wr(M, MFC_REG_RISC2HOST_INT, 0);
    Wr(M, MFC_REG_CONTEXT_MEM_ADDR, M->BufferPa.LowPart + MFC_CTX_OFFSET);
    Wr(M, MFC_REG_CONTEXT_MEM_SIZE, MFC_DEV_CTX_SIZE);
    Wr(M, MFC_REG_HOST2RISC_CMD, MFC_H2R_CMD_SYS_INIT);
    Wr(M, MFC_REG_HOST2RISC_INT, 1);
    R2h = MfcPollR2h(M, 1000);
    MfcTrace("SYS_INIT: R2H %u (SYS_INIT_RET %u) - interrupt left pending for the ISR test\n", R2h, MFC_R2H_SYS_INIT_RET);
    if (R2h != MFC_R2H_SYS_INIT_RET)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    M->FirmwareUp = TRUE;
    return STATUS_SUCCESS;
}

//
// V-W2 decode self-test (notes/d10-mfc-decode-flow.md): plain-mode H.264 decode of mfc_test.h264 (tools/mfc/
// mkh264pcm.py, bit-exact I_PCM / P_Skip vector), polled before the interrupt is connected. Logs every R2H and an
// FNV-1a 32 hash of each displayed luma / NV12 CbCr plane for comparison with the generator's output.
//
typedef struct _MFC_TEST
{
    PUCHAR  Va;
    ULONG   Pa;
    ULONG   InstNo;
    BOOLEAN Opened;
    ULONG   Width;
    ULONG   Height;
    ULONG   Stride;
    ULONG   LumaSize;
    ULONG   ChromaOffset;   // page aligned inside the slot (vendor: each plane its own page-aligned buffer)
    ULONG   ChromaSize;
    ULONG   SlotSize;
    ULONG   NumDpb;
    ULONG   Available;
    ULONG   Used;
    ULONG   Frames;
} MFC_TEST, *PMFC_TEST;

/** Issue an H2R command and poll for the R2H; returns the reason (R2H & 0x1FFFF) or 0. The interrupt stays set. **/
static ULONG
MfcCmd(_In_ PMFC_DEVICE M, _In_ ULONG Cmd, _In_ ULONG TimeoutMs)
{
    Wr(M, MFC_REG_RISC2HOST_CMD, 0);
    Wr(M, MFC_REG_HOST2RISC_CMD, Cmd);
    Wr(M, MFC_REG_HOST2RISC_INT, 1);
    return MfcPollR2h(M, TimeoutMs) & MFC_R2H_REASON_MASK;
}

static VOID
MfcClearInt(_In_ PMFC_DEVICE M)
{
    Wr(M, MFC_REG_RISC2HOST_CMD, 0);   // vendor mfc_clear_int
    Wr(M, MFC_REG_RISC2HOST_INT, 0);
}

static ULONG
MfcFnv(_In_ const volatile UCHAR *Base, _In_ ULONG Width, _In_ ULONG Rows, _In_ ULONG Stride)
{
    ULONG Hash = 0x811C9DC5u;
    ULONG Row;
    ULONG Col;

    for (Row = 0; Row < Rows; Row++)
    {
        for (Col = 0; Col < Width; Col++)
        {
            Hash = (Hash ^ Base[Row * Stride + Col]) * 0x01000193u;
        }
    }

    return Hash;
}

static VOID
MfcSetStream(_In_ PMFC_DEVICE M, _In_ PMFC_TEST T, _In_ ULONG Offset, _In_ ULONG Size)
{
    Wr(M, MFC_REG_D_STREAM_DATA_SIZE, Size);
    Wr(M, MFC_REG_D_CPB_ADDR, T->Pa + MFC_TEST_CPB_OFFSET);
    Wr(M, MFC_REG_D_CPB_SIZE, MFC_TEST_CPB_SIZE);
    Wr(M, MFC_REG_D_CPB_OFFSET, Offset);
}

static ULONG
MfcLumaToSlot(_In_ PMFC_TEST T, _In_ ULONG Luma)
{
    ULONG Base = T->Pa + MFC_TEST_DPB_OFFSET;

    if ((Luma < Base) || (((Luma - Base) % T->SlotSize) != 0) || (((Luma - Base) / T->SlotSize) >= T->NumDpb))
    {
        return MAXULONG;
    }

    return (Luma - Base) / T->SlotSize;
}

static BOOLEAN
MfcTestOpenAndHeader(_In_ PMFC_DEVICE M, _In_ PMFC_TEST T, _In_ ULONG StreamSize, _Out_ PULONG Consumed)
{
    ULONG R2h;
    ULONG Err;

    *Consumed = 0;
    Wr(M, MFC_REG_CODEC_CONTROL, Rd(M, MFC_REG_CODEC_CONTROL) & ~0xEu);   // non-OTF
    Wr(M, MFC_REG_CODEC_TYPE, 0x00010000u);                              // H.264 decode | CLEAR_CTX_MEM
    Wr(M, MFC_REG_CONTEXT_MEM_ADDR, T->Pa);
    Wr(M, MFC_REG_CONTEXT_MEM_SIZE, MFC_TEST_INST_SIZE);
    Wr(M, MFC_REG_D_CRC_CTRL, 0);
    R2h = MfcCmd(M, MFC_H2R_OPEN_INSTANCE, 1000);
    Err = Rd(M, MFC_REG_ERROR_CODE);
    T->InstNo = Rd(M, MFC_REG_RET_INSTANCE_ID);
    MfcClearInt(M);
    MfcTrace("test: OPEN_INSTANCE R2H %u err 0x%x inst %u\n", R2h, Err, T->InstNo);
    if (R2h != MFC_H2R_OPEN_INSTANCE)
    {
        return FALSE;
    }

    T->Opened = TRUE;

    MfcSetStream(M, T, 0, StreamSize);
    Wr(M, MFC_REG_D_DEC_OPTIONS, 0x0000A400u);     // conceal 4<<8 | REALLOC | SPECIAL_PARSING (vendor default)
    Wr(M, MFC_REG_D_FORCE_PIXEL_VAL, 0x08020000u);
    Wr(M, MFC_REG_PIXEL_FORMAT, 0x10u);            // NV12 | P010_decoding (9820 DT)
    Wr(M, MFC_REG_D_SEI_ENABLE, 0x74u);            // recovery | static info | HDR10+ (fw date >= 0x180709)
    Wr(M, MFC_REG_INSTANCE_ID, T->InstNo);
    R2h = MfcCmd(M, MFC_H2R_SEQ_HEADER, 1000);
    Err = Rd(M, MFC_REG_ERROR_CODE);
    T->Width  = Rd(M, MFC_REG_D_DISPLAY_WIDTH);
    T->Height = Rd(M, MFC_REG_D_DISPLAY_HEIGHT);
    *Consumed = Rd(M, MFC_REG_D_DECODED_NAL_SIZE);
    MfcTrace("test: SEQ_HEADER R2H %u err 0x%x %ux%u min dpb %u scratch 0x%x plane 0x%x/0x%x mv %u consumed %u\n", R2h,
             Err, T->Width, T->Height, Rd(M, MFC_REG_D_MIN_NUM_DPB), Rd(M, MFC_REG_D_MIN_SCRATCH_SIZE),
             Rd(M, MFC_REG_D_MIN_FIRST_PLANE), Rd(M, MFC_REG_D_MIN_SECOND_PLANE), Rd(M, MFC_REG_D_MIN_NUM_MV), *Consumed);
    return (R2h == MFC_H2R_SEQ_HEADER) && (T->Width != 0) && (T->Height != 0);
}

static BOOLEAN
MfcTestInitBuffers(_In_ PMFC_DEVICE M, _In_ PMFC_TEST T)
{
    ULONG MinDpb  = Rd(M, MFC_REG_D_MIN_NUM_DPB);
    ULONG Scratch = (Rd(M, MFC_REG_D_MIN_SCRATCH_SIZE) + 255u) & ~255u;
    ULONG NumMv   = Rd(M, MFC_REG_D_MIN_NUM_MV);
    ULONG Plane;
    ULONG MvSize;
    ULONG Mv;
    ULONG Index;
    ULONG R2h;
    ULONG Err;

    T->Stride     = (T->Width + 15u) & ~15u;
    Plane         = T->Stride * ((T->Height + 15u) & ~15u);
    T->LumaSize   = max(Plane + 256u, Rd(M, MFC_REG_D_MIN_FIRST_PLANE));
    T->ChromaSize = max(Plane / 2u + 256u, Rd(M, MFC_REG_D_MIN_SECOND_PLANE));
    T->ChromaOffset = (T->LumaSize + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    T->SlotSize   = (T->ChromaOffset + T->ChromaSize + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    T->NumDpb     = min(MinDpb + 2u, MFC_TEST_MAX_DPB);
    MvSize        = ((((T->Width + 15u) / 16u) * ((((T->Height + 15u) / 16u) + 1u) / 2u) * 2u * 64u + 1024u) + 31u) & ~31u;
    if ((MinDpb == 0) || (MinDpb > T->NumDpb) || (T->NumDpb * T->SlotSize > MFC_TEST_DPB_SIZE) ||
        (Scratch + NumMv * (MvSize + 16u) > MFC_TEST_CODEC_SIZE))
    {
        MfcTrace("test: buffers do not fit (dpb %u x 0x%x, scratch 0x%x, mv %u x 0x%x)\n", T->NumDpb, T->SlotSize,
                 Scratch, NumMv, MvSize);
        return FALSE;
    }

    Wr(M, MFC_REG_PIXEL_FORMAT, 0x10u);
    Wr(M, MFC_REG_D_NUM_DPB, T->NumDpb);
    Wr(M, MFC_REG_D_FIRST_PLANE_SIZE, T->LumaSize);
    Wr(M, MFC_REG_D_FIRST_STRIDE, T->Stride);
    Wr(M, MFC_REG_D_SECOND_PLANE_SIZE, T->ChromaSize);
    Wr(M, MFC_REG_D_SECOND_STRIDE, T->Stride);
    Wr(M, MFC_REG_D_SCRATCH_ADDR, T->Pa + MFC_TEST_CODEC_OFFSET);
    Wr(M, MFC_REG_D_SCRATCH_SIZE, Scratch);
    Wr(M, MFC_REG_D_MV_BUFFER_SIZE, MvSize);
    Wr(M, MFC_REG_D_INIT_BUF_OPTIONS, 0x8u);       // dynamic DPB
    Wr(M, MFC_REG_D_NUM_MV, NumMv);
    Mv = T->Pa + MFC_TEST_CODEC_OFFSET + Scratch;
    for (Index = 0; Index < NumMv; Index++)
    {
        Mv = (Mv + 15u) & ~15u;
        Wr(M, MFC_REG_D_MV_BUFFER0 + Index * 4u, Mv);
        Mv += MvSize;
    }

    Wr(M, MFC_REG_INSTANCE_ID, T->InstNo);
    R2h = MfcCmd(M, MFC_H2R_INIT_BUFFERS, 1000);
    Err = Rd(M, MFC_REG_ERROR_CODE);
    MfcClearInt(M);
    MfcTrace("test: INIT_BUFFERS R2H %u err 0x%x (dpb %u x luma 0x%x chroma 0x%x stride %u, scratch 0x%x, mv %u x 0x%x)\n",
             R2h, Err, T->NumDpb, T->LumaSize, T->ChromaSize, T->Stride, Scratch, NumMv, MvSize);
    return (R2h == MFC_H2R_INIT_BUFFERS) && (Err == 0);
}

/** One NAL_START / LAST_FRAME; returns FALSE when decoding must stop. Updates Offset / Remaining. **/
static BOOLEAN
MfcTestDecodeOne(_In_ PMFC_DEVICE M, _In_ PMFC_TEST T, _Inout_ PULONG Offset, _Inout_ PULONG Remaining)
{
    ULONG Cmd = (*Remaining != 0) ? MFC_H2R_NAL_START : MFC_H2R_LAST_FRAME;
    ULONG Idx;
    ULONG R2h, Err, Status, Disp, Decoded, Luma, Type, Consumed, Slot;
    ULONG Base;

    for (Idx = 0; (Idx < T->NumDpb) && ((T->Used >> Idx) & 1u); Idx++)
    {
    }

    if (Idx == T->NumDpb)
    {
        MfcTrace("test: no free DPB (used 0x%x)\n", T->Used);
        return FALSE;
    }

    Base         = T->Pa + MFC_TEST_DPB_OFFSET + Idx * T->SlotSize;
    T->Available |= 1u << Idx;
    MfcSetStream(M, T, *Offset, *Remaining);
    Wr(M, MFC_REG_D_FIRST_PLANE_SIZE, T->LumaSize);
    Wr(M, MFC_REG_D_SECOND_PLANE_SIZE, T->ChromaSize);
    Wr(M, MFC_REG_D_FIRST_PLANE_DPB0 + Idx * 4u, Base);
    Wr(M, MFC_REG_D_SECOND_PLANE_DPB0 + Idx * 4u, Base + T->ChromaOffset);
    Wr(M, MFC_REG_D_NAL_START_OPTIONS, Rd(M, MFC_REG_D_NAL_START_OPTIONS) & ~0x8u);
    Wr(M, MFC_REG_D_DYN_DPB_LOWER, 1u << Idx);
    Wr(M, MFC_REG_D_DYN_DPB_UPPER, 0);
    Wr(M, MFC_REG_D_AVAIL_DPB_LOWER, T->Available);
    Wr(M, MFC_REG_D_AVAIL_DPB_UPPER, 0);
    Wr(M, MFC_REG_D_SLICE_IF_ENABLE, 0);
    Wr(M, MFC_REG_DEC_TIMEOUT_VALUE, 200000000u);
    Wr(M, MFC_REG_INSTANCE_ID, T->InstNo);
    R2h      = MfcCmd(M, Cmd, 1000);
    Err      = Rd(M, MFC_REG_ERROR_CODE);
    Status   = Rd(M, MFC_REG_D_DISPLAY_STATUS);
    Disp     = Status & 7u;
    Decoded  = Rd(M, MFC_REG_D_DECODED_LUMA);
    Luma     = Rd(M, MFC_REG_D_DISPLAY_LUMA);
    Type     = Rd(M, MFC_REG_D_DISPLAY_TYPE) & 7u;
    Consumed = Rd(M, MFC_REG_D_DECODED_NAL_SIZE);
    T->Used  = Rd(M, MFC_REG_D_USED_DPB_LOWER);
    MfcClearInt(M);
    MfcTrace("test: %s dpb %u: R2H %u err 0x%x status 0x%x decoded slot %d display slot %d type %u used 0x%x consumed %u\n",
             (Cmd == MFC_H2R_NAL_START) ? "NAL_START" : "LAST_FRAME", Idx, R2h, Err, Status,
             (LONG)MfcLumaToSlot(T, Decoded), (LONG)MfcLumaToSlot(T, Luma), Type, T->Used, Consumed);
    if ((R2h == 0) || ((R2h != MFC_R2H_FRAME_DONE_RET) && (R2h != MFC_R2H_ERR_RET)) ||
        ((R2h == MFC_R2H_ERR_RET) && (((Err & 0xFFFFu) < 160u) || ((Err & 0xFFFFu) > 222u))))
    {
        return FALSE;
    }

    if ((Disp == 1u) || (Disp == 2u))
    {
        Slot = MfcLumaToSlot(T, Luma);
        if (Slot != MAXULONG)
        {
            PUCHAR Va = T->Va + MFC_TEST_DPB_OFFSET + Slot * T->SlotSize;

            T->Available &= ~(1u << Slot);
            if (Type != 0)
            {
                MfcTrace("test: frame %u %ux%u type %u: luma fnv 0x%08x chroma fnv 0x%08x\n", T->Frames, T->Width,
                         T->Height, Type, MfcFnv(Va, T->Width, T->Height, T->Stride),
                         MfcFnv(Va + T->ChromaOffset, T->Width, T->Height / 2u, T->Stride));
                T->Frames++;
            }
        }
    }

    if (Disp == 3u)
    {
        return FALSE;   // drained
    }

    if ((Disp != 2u) && (Cmd == MFC_H2R_NAL_START))
    {
        if ((Consumed != 0) && (Consumed < *Remaining) && ((*Remaining - Consumed) > 4u))
        {
            *Offset    += Consumed;
            *Remaining -= Consumed;
        }
        else
        {
            *Remaining = 0;
        }
    }

    return TRUE;
}

static VOID
MfcDecodeTest(_In_ PMFC_DEVICE M)
{
    PHYSICAL_ADDRESS Low, High, Boundary;
    MFC_TEST         T;
    ULONG            Size;
    ULONG            Offset;
    ULONG            Remaining;
    ULONG            Steps;
    ULONG            R2h;
    PVOID            Alloc;

    RtlZeroMemory(&T, sizeof(T));
    Low.QuadPart      = 0;
    High.QuadPart     = 0xFFFFFFFFLL;
    Boundary.QuadPart = 0;
    Alloc = MmAllocateContiguousMemorySpecifyCache(MFC_TEST_BUFFER_SIZE, Low, High, Boundary, MmNonCached);
    if (Alloc == NULL)
    {
        MfcTrace("test: no contiguous buffer\n");
        return;
    }

    T.Va = (PUCHAR)Alloc;
    T.Pa = MmGetPhysicalAddress(Alloc).LowPart;
    RtlZeroMemory(T.Va, MFC_TEST_BUFFER_SIZE);
    if (!NT_SUCCESS(MfcLoadFile(MFC_TEST_PATH, T.Va + MFC_TEST_CPB_OFFSET, MFC_TEST_CPB_SIZE - 512u, &Size)))
    {
        MmFreeContiguousMemorySpecifyCache(Alloc, MFC_TEST_BUFFER_SIZE, MmNonCached);
        return;   // no test stream installed: nothing to do
    }

    MfcClearInt(M);   // the SYS_INIT R2H
    MfcTrace("test: %u B stream, buffer PA 0x%08x\n", Size, T.Pa);
    if (MfcTestOpenAndHeader(M, &T, Size, &Offset))
    {
        MfcClearInt(M);
        Remaining = Size;
        if ((Offset != 0) && (Offset < Size))
        {
            Remaining = Size - Offset;
        }
        else
        {
            Offset = 0;
        }

        if (MfcTestInitBuffers(M, &T))
        {
            for (Steps = 0; (Steps < 16u) && MfcTestDecodeOne(M, &T, &Offset, &Remaining); Steps++)
            {
            }
        }
    }
    else
    {
        MfcClearInt(M);
    }

    R2h = 0;
    if (T.Opened)
    {
        Wr(M, MFC_REG_INSTANCE_ID, T.InstNo);
        R2h = MfcCmd(M, MFC_H2R_CLOSE_INSTANCE, 1000);   // R2H left pending for the ISR test
        MfcTrace("test: CLOSE_INSTANCE R2H %u err 0x%x - %u frame(s) displayed\n", R2h, Rd(M, MFC_REG_ERROR_CODE),
                 T.Frames);
    }

    if (!T.Opened || (R2h == MFC_H2R_CLOSE_INSTANCE))
    {
        MmFreeContiguousMemorySpecifyCache(Alloc, MFC_TEST_BUFFER_SIZE, MmNonCached);
    }
    else
    {
        MfcTrace("test: instance not closed - test buffer kept (the firmware may still own it)\n");
    }
}

NTSTATUS
MfcEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    PMFC_DEVICE                     M = MfcGetDevice(Device);
    ULONG                           Index;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc;
    WDF_INTERRUPT_CONFIG            Config;
    NTSTATUS                        Status;

    for (Index = 0; Index < WdfCmResourceListGetCount(Translated); Index++)
    {
        Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if ((Desc->Type == CmResourceTypeMemory) && (M->Regs == NULL) && (Desc->u.Memory.Length >= MFC_WINDOW_MIN))
        {
            M->Regs = (PUCHAR)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
            M->RegsLength = Desc->u.Memory.Length;
        }
        else if ((Desc->Type == CmResourceTypeInterrupt) && (M->Interrupt == NULL))
        {
            WDF_INTERRUPT_CONFIG_INIT(&Config, MfcEvtIsr, MfcEvtDpc);
            Config.InterruptRaw        = WdfCmResourceListGetDescriptor(Raw, Index);
            Config.InterruptTranslated = Desc;
            Status = WdfInterruptCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, &M->Interrupt);
            if (!NT_SUCCESS(Status))
            {
                MfcTrace("WdfInterruptCreate 0x%08X\n", Status);
                return Status;
            }
        }
    }

    if ((M->Regs == NULL) || (M->Interrupt == NULL))
    {
        MfcTrace("_CRS needs the MFC window (>= 0x10000) and one interrupt\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Status = MfcBoot(M);
    MfcTrace("boot 0x%08X\n", Status);
    if (NT_SUCCESS(Status))
    {
        MfcDecodeTest(M);
    }

    return Status;
}

NTSTATUS
MfcEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PMFC_DEVICE M = MfcGetDevice(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (M->Regs != NULL)
    {
        //
        // Vendor teardown, never a reset of a running core (vidc2: V-W1's RISC_ON 0 + MfcReset here raised an
        // SError, bugcheck 0x122): mfc_risc_off = wait MFC_BUS_STATUS == 0 (MFC_BW_TIMEOUT 500 ms), RISC_ON 0;
        // __mfc_deinit_hw = MFC_OFF 1. The next MfcBoot resets the stopped core.
        //
        ULONG Ms;

        for (Ms = 0; (Ms < 500) && (Rd(M, MFC_REG_MFC_BUS_STATUS) != 0); Ms++)
        {
            KeStallExecutionProcessor(1000);
        }

        MfcTrace("release: bus status 0x%x after %u ms\n", Rd(M, MFC_REG_MFC_BUS_STATUS), Ms);
        Wr(M, MFC_REG_RISC_ON, 0);
        Wr(M, MFC_REG_MFC_OFF, 1);
        MmUnmapIoSpace(M->Regs, M->RegsLength);
        M->Regs = NULL;
        if (Ms == 500)
        {
            M->BufferAlloc = NULL;   // bus never went idle: the core may still DMA into it - leak rather than free
        }
    }

    if (M->BufferAlloc != NULL)
    {
        MmFreeContiguousMemorySpecifyCache(M->BufferAlloc, MFC_FW_BUFFER_SIZE + MFC_BUFFER_ALIGN, MmNonCached);
        M->BufferAlloc = NULL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
MfcEvtD0EntryPostInterruptsEnabled(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PMFC_DEVICE   M = MfcGetDevice(Device);
    LARGE_INTEGER Wait;

    UNREFERENCED_PARAMETER(PreviousState);
    Wait.QuadPart = -10000LL * 100;   // 100 ms for the pending R2H interrupt to be taken
    KeDelayExecutionThread(KernelMode, FALSE, &Wait);
    MfcTrace("interrupt test: ISR ran %d time(s), last R2H cmd %u, R2H int now %u\n", M->IsrCount, M->LastR2hCmd,
             Rd(M, MFC_REG_RISC2HOST_INT));
    return STATUS_SUCCESS;
}

BOOLEAN
MfcEvtIsr(_In_ WDFINTERRUPT Interrupt, _In_ ULONG MessageId)
{
    PMFC_DEVICE M = MfcGetDevice(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(MessageId);
    if (Rd(M, MFC_REG_RISC2HOST_INT) == 0)
    {
        return FALSE;
    }

    M->LastR2hCmd = Rd(M, MFC_REG_RISC2HOST_CMD);
    Wr(M, MFC_REG_RISC2HOST_CMD, 0);   // vendor mfc_clear_int
    Wr(M, MFC_REG_RISC2HOST_INT, 0);
    InterlockedIncrement(&M->IsrCount);
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID
MfcEvtDpc(_In_ WDFINTERRUPT Interrupt, _In_ WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(AssociatedObject);
}

NTSTATUS
MfcEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES        Attr;
    WDFDEVICE                    Device;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware              = MfcEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware              = MfcEvtReleaseHardware;
    Pnp.EvtDeviceD0EntryPostInterruptsEnabled = MfcEvtD0EntryPostInterruptsEnabled;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, MFC_DEVICE);
    return WdfDeviceCreate(&DeviceInit, &Attr, &Device);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, MfcEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
