/*
 * Decode.c - Exynos9820Mfc plain-mode (non-NAL-queue) H.264 decode session (D10 V2/V3, notes/d10-mfc-decode-flow.md).
 * The command sequence, register values and buffer formulas are the ones V-W2 proved bit-exact on the device
 * (vidc4): OPEN_INSTANCE -> SEQ_HEADER -> INIT_BUFFERS (dynamic DPB) -> NAL_START per access unit -> LAST_FRAME
 * drain -> CLOSE_INSTANCE. Every buffer lives above the RISC base (below it: OPEN_INSTANCE ERR_RET 10, vidc3).
 *
 * A session owns one region: [0, 2 MiB) instance context, [2 MiB, +CpbSize) CPB (the caller copies the stream window
 * to its start), then scratch + MV buffers and the DPB slots, laid out at SEQ_HEADER time.
 */
#include "Mfc.h"
#include "MfcIoctl.h"

/** Poll R2H (interrupts not connected) for up to TimeoutMs; returns the raw R2H command or 0. **/
ULONG
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

ULONG
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

/**
 * Cache maintenance for a CPU<->MFC hand-over (no-op when MFC_CACHED is 0). FromDevice FALSE: CPU-written data the
 * MFC will read (CPB, firmware); TRUE: data the MFC wrote that the CPU is about to read (displayed slot) - called
 * after the command completed, so lines speculatively fetched during the DMA are dropped too.
 */
VOID
MfcFlush(_In_ PVOID Va, _In_ ULONG Length, _In_ BOOLEAN FromDevice)
{
    PMDL Mdl;

    if (!MFC_CACHED || (Length == 0))
    {
        return;
    }

    Mdl = IoAllocateMdl(Va, Length, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        MfcTrace("flush: no MDL for 0x%x B\n", Length);
        return;
    }

    MmBuildMdlForNonPagedPool(Mdl);
    KeFlushIoBuffers(Mdl, FromDevice, TRUE);
    IoFreeMdl(Mdl);
}

/**
 * Issue an H2R command and wait for its R2H. Returns the reason (R2H & 0x1FFFF) or 0 on timeout; the R2H interrupt
 * is cleared on return (by the ISR, or here when polling) unless LeavePending (the boot ISR test). The result
 * registers (ERROR_CODE, D_*) stay valid until the next command.
 */
static ULONG
MfcExec(_In_ PMFC_DEVICE M, _In_ ULONG Cmd, _In_ ULONG TimeoutMs, _In_ BOOLEAN LeavePending, _Out_opt_ PULONG Microseconds)
{
    LARGE_INTEGER Freq;
    LARGE_INTEGER Start;
    LARGE_INTEGER End;
    LARGE_INTEGER Timeout;
    ULONG         Reason;

    Start = KeQueryPerformanceCounter(&Freq);
    if (M->InterruptsOn)
    {
        KeClearEvent(&M->CmdDone);
        InterlockedExchange((volatile LONG *)&M->LastR2hCmd, 0);
        Wr(M, MFC_REG_RISC2HOST_CMD, 0);
        Wr(M, MFC_REG_HOST2RISC_CMD, Cmd);
        Wr(M, MFC_REG_HOST2RISC_INT, 1);
        Timeout.QuadPart = -10000LL * TimeoutMs;
        Reason = (KeWaitForSingleObject(&M->CmdDone, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS) ?
                 (M->LastR2hCmd & MFC_R2H_REASON_MASK) : 0;
    }
    else
    {
        Wr(M, MFC_REG_RISC2HOST_CMD, 0);
        Wr(M, MFC_REG_HOST2RISC_CMD, Cmd);
        Wr(M, MFC_REG_HOST2RISC_INT, 1);
        Reason = MfcPollR2h(M, TimeoutMs) & MFC_R2H_REASON_MASK;
        if ((Reason != 0) && !LeavePending)
        {
            Wr(M, MFC_REG_RISC2HOST_CMD, 0);   // vendor mfc_clear_int
            Wr(M, MFC_REG_RISC2HOST_INT, 0);
        }
    }

    End = KeQueryPerformanceCounter(NULL);
    if (Microseconds != NULL)
    {
        *Microseconds = (ULONG)(((End.QuadPart - Start.QuadPart) * 1000000LL) / Freq.QuadPart);
    }

    return Reason;
}

static VOID
MfcSetStream(_In_ PMFC_DEVICE M, _In_ PMFC_SESSION S, _In_ ULONG Size)
{
    Wr(M, MFC_REG_D_STREAM_DATA_SIZE, Size);
    Wr(M, MFC_REG_D_CPB_ADDR, S->Pa + MFC_CPB_OFFSET);
    Wr(M, MFC_REG_D_CPB_SIZE, S->CpbSize);
    Wr(M, MFC_REG_D_CPB_OFFSET, 0);
}

static ULONG
MfcLumaToSlot(_In_ PMFC_SESSION S, _In_ ULONG Luma)
{
    ULONG Base = S->Pa + S->DpbOffset;

    if ((S->SlotSize == 0) || (Luma < Base) || (((Luma - Base) % S->SlotSize) != 0) ||
        (((Luma - Base) / S->SlotSize) >= S->NumDpb))
    {
        return MAXULONG;
    }

    return (Luma - Base) / S->SlotSize;
}

VOID
MfcSessionInit(_Out_ PMFC_SESSION S, _In_ PUCHAR Va, _In_ ULONG Pa, _In_ ULONG Size, _In_ ULONG CpbSize)
{
    RtlZeroMemory(S, sizeof(*S));
    S->Va          = Va;
    S->Pa          = Pa;
    S->Size        = Size;
    S->CpbSize     = CpbSize;
    S->CodecOffset = MFC_CPB_OFFSET + CpbSize;
}

NTSTATUS
MfcSessionOpen(_In_ PMFC_DEVICE M, _Inout_ PMFC_SESSION S)
{
    ULONG R2h;
    ULONG Err;

    Wr(M, MFC_REG_CODEC_CONTROL, Rd(M, MFC_REG_CODEC_CONTROL) & ~0xEu);   // non-OTF
    Wr(M, MFC_REG_CODEC_TYPE, 0x00010000u);                              // H.264 decode | CLEAR_CTX_MEM
    Wr(M, MFC_REG_CONTEXT_MEM_ADDR, S->Pa);
    Wr(M, MFC_REG_CONTEXT_MEM_SIZE, MFC_INST_SIZE);
    Wr(M, MFC_REG_D_CRC_CTRL, 0);
    R2h = MfcExec(M, MFC_H2R_OPEN_INSTANCE, 1000, FALSE, NULL);
    Err = Rd(M, MFC_REG_ERROR_CODE);
    S->InstNo = Rd(M, MFC_REG_RET_INSTANCE_ID);
    MfcTrace("session: OPEN_INSTANCE R2H %u err 0x%x inst %u (region PA 0x%08x, 0x%x B)\n", R2h, Err, S->InstNo,
             S->Pa, S->Size);
    if (R2h != MFC_H2R_OPEN_INSTANCE)
    {
        return (R2h == 0) ? STATUS_IO_TIMEOUT : STATUS_DEVICE_PROTOCOL_ERROR;
    }

    S->Opened = TRUE;
    return STATUS_SUCCESS;
}

/** SEQ_HEADER read-back -> layout of scratch / MV / DPB inside the region, then INIT_BUFFERS. **/
static NTSTATUS
MfcSessionInitBuffers(_In_ PMFC_DEVICE M, _Inout_ PMFC_SESSION S)
{
    ULONG MinDpb  = Rd(M, MFC_REG_D_MIN_NUM_DPB);
    ULONG Scratch = (Rd(M, MFC_REG_D_MIN_SCRATCH_SIZE) + 255u) & ~255u;
    ULONG NumMv   = Rd(M, MFC_REG_D_MIN_NUM_MV);
    ULONG Plane;
    ULONG MvSize;
    ULONG Mv;
    ULONG CodecEnd;
    ULONG Index;
    ULONG R2h;
    ULONG Err;

    S->Stride       = (S->Width + 15u) & ~15u;
    Plane           = S->Stride * ((S->Height + 15u) & ~15u);
    S->LumaSize     = max(Plane + 256u, Rd(M, MFC_REG_D_MIN_FIRST_PLANE));
    S->ChromaSize   = max(Plane / 2u + 256u, Rd(M, MFC_REG_D_MIN_SECOND_PLANE));
    S->ChromaOffset = (S->LumaSize + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    S->SlotSize     = (S->ChromaOffset + S->ChromaSize + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    MvSize          = ((((S->Width + 15u) / 16u) * ((((S->Height + 15u) / 16u) + 1u) / 2u) * 2u * 64u + 1024u) + 31u) & ~31u;
    CodecEnd        = S->CodecOffset + Scratch + NumMv * (MvSize + 16u);
    S->DpbOffset    = (CodecEnd + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    S->NumDpb       = min(MinDpb + 2u, MFC_MAX_DPB);
    while ((S->NumDpb > MinDpb) && (S->DpbOffset + S->NumDpb * S->SlotSize > S->Size))
    {
        S->NumDpb--;
    }

    if ((MinDpb == 0) || (MinDpb > MFC_MAX_DPB) || (NumMv > 32u) || (S->DpbOffset >= S->Size) ||
        (S->DpbOffset + S->NumDpb * S->SlotSize > S->Size))
    {
        MfcTrace("session: buffers do not fit 0x%x B (min dpb %u x 0x%x, scratch 0x%x, mv %u x 0x%x)\n", S->Size,
                 MinDpb, S->SlotSize, Scratch, NumMv, MvSize);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Wr(M, MFC_REG_PIXEL_FORMAT, 0x10u);
    Wr(M, MFC_REG_D_NUM_DPB, S->NumDpb);
    Wr(M, MFC_REG_D_FIRST_PLANE_SIZE, S->LumaSize);
    Wr(M, MFC_REG_D_FIRST_STRIDE, S->Stride);
    Wr(M, MFC_REG_D_SECOND_PLANE_SIZE, S->ChromaSize);
    Wr(M, MFC_REG_D_SECOND_STRIDE, S->Stride);
    Wr(M, MFC_REG_D_SCRATCH_ADDR, S->Pa + S->CodecOffset);
    Wr(M, MFC_REG_D_SCRATCH_SIZE, Scratch);
    Wr(M, MFC_REG_D_MV_BUFFER_SIZE, MvSize);
    Wr(M, MFC_REG_D_INIT_BUF_OPTIONS, 0x8u);       // dynamic DPB
    Wr(M, MFC_REG_D_NUM_MV, NumMv);
    Mv = S->Pa + S->CodecOffset + Scratch;
    for (Index = 0; Index < NumMv; Index++)
    {
        Mv = (Mv + 15u) & ~15u;
        Wr(M, MFC_REG_D_MV_BUFFER0 + Index * 4u, Mv);
        Mv += MvSize;
    }

    Wr(M, MFC_REG_INSTANCE_ID, S->InstNo);
    R2h = MfcExec(M, MFC_H2R_INIT_BUFFERS, 1000, FALSE, NULL);
    Err = Rd(M, MFC_REG_ERROR_CODE);
    MfcTrace("session: INIT_BUFFERS R2H %u err 0x%x (%ux%u, dpb %u (min %u) x 0x%x, stride %u, scratch 0x%x, mv %u x 0x%x)\n",
             R2h, Err, S->Width, S->Height, S->NumDpb, MinDpb, S->SlotSize, S->Stride, Scratch, NumMv, MvSize);
    if ((R2h != MFC_H2R_INIT_BUFFERS) || (Err != 0))
    {
        return (R2h == 0) ? STATUS_IO_TIMEOUT : STATUS_DEVICE_PROTOCOL_ERROR;
    }

    S->Ready = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS
MfcSessionHeader(_In_ PMFC_DEVICE M, _Inout_ PMFC_SESSION S, _In_ ULONG Length, _Out_ PMFC_RESULT R)
{
    ULONG Info1;
    ULONG Info2;
    ULONG Consumed;

    MfcSetStream(M, S, Length);
    // conceal 4<<8 | REALLOC | SPECIAL_PARSING (vendor default); decode order (mfc_cmd.c seq header): display delay
    // enable (bit 3) with D_DISPLAY_DELAY 0 + DECODING_ORDER_ENABLE (bit 17)
    if (S->DecodeOrder)
    {
        Wr(M, MFC_REG_D_DISPLAY_DELAY, 0);
    }
    Wr(M, MFC_REG_D_DEC_OPTIONS, 0x0000A400u | (S->DecodeOrder ? ((1u << 3) | (1u << 17)) : 0u));
    Wr(M, MFC_REG_D_FORCE_PIXEL_VAL, 0x08020000u);
    Wr(M, MFC_REG_PIXEL_FORMAT, 0x10u);            // NV12 | P010_decoding (9820 DT)
    Wr(M, MFC_REG_D_SEI_ENABLE, 0x74u);            // recovery | static info | HDR10+ (fw date >= 0x180709)
    Wr(M, MFC_REG_INSTANCE_ID, S->InstNo);
    R->R2h        = MfcExec(M, MFC_H2R_SEQ_HEADER, 1000, FALSE, &R->Microseconds);
    R->Error      = Rd(M, MFC_REG_ERROR_CODE);
    S->Width      = Rd(M, MFC_REG_D_DISPLAY_WIDTH);
    S->Height     = Rd(M, MFC_REG_D_DISPLAY_HEIGHT);
    Info1         = Rd(M, MFC_REG_D_DISPLAY_CROP_INFO1);
    Info2         = Rd(M, MFC_REG_D_DISPLAY_CROP_INFO2);
    Consumed      = Rd(M, MFC_REG_D_DECODED_NAL_SIZE);
    S->CropLeft   = Info1 & 0xFFFFu;
    S->CropRight  = Info1 >> 16;
    S->CropTop    = Info2 & 0xFFFFu;
    S->CropBottom = Info2 >> 16;
    MfcTrace("session: SEQ_HEADER R2H %u err 0x%x %ux%u crop %u/%u/%u/%u min dpb %u consumed %u of %u\n", R->R2h,
             R->Error, S->Width, S->Height, S->CropLeft, S->CropTop, S->CropRight, S->CropBottom,
             Rd(M, MFC_REG_D_MIN_NUM_DPB), Consumed, Length);
    if (R->R2h == 0)
    {
        return STATUS_IO_TIMEOUT;
    }

    if ((R->R2h != MFC_H2R_SEQ_HEADER) || (S->Width == 0) || (S->Height == 0))
    {
        R->Flags    = MFC_DEC_ERROR;     // vendor: drop the source buffer, stay waiting for a header
        R->Consumed = Length;
        return STATUS_SUCCESS;
    }

    // vendor (mfc_isr.c SEQ_DONE): 0 < consumed < bytesused -> continue from consumed, else resubmit the whole buffer
    R->Consumed = ((Consumed != 0) && (Consumed < Length)) ? Consumed : 0;
    R->Flags    = MFC_DEC_HEADER;
    return MfcSessionInitBuffers(M, S);
}

/**
 * One decode step on the window the caller copied to the CPB start (Length bytes; 0 = drain). Before the first
 * header this runs SEQ_HEADER + INIT_BUFFERS (no picture); afterwards one NAL_START / LAST_FRAME.
 */
NTSTATUS
MfcSessionStep(_In_ PMFC_DEVICE M, _Inout_ PMFC_SESSION S, _In_ ULONG Length, _Out_ PMFC_RESULT R)
{
    ULONG Cmd;
    ULONG Idx;
    ULONG Base;
    ULONG Disp;
    ULONG Luma;
    ULONG Slot;
    ULONG Consumed;
    ULONG Err;

    RtlZeroMemory(R, sizeof(*R));
    if (!S->Opened || (Length > S->CpbSize - 512u))
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (S->Ended || (!S->Ready && (Length == 0)))
    {
        R->Flags = MFC_DEC_EOS;
        return STATUS_SUCCESS;
    }

    MfcFlush(S->Va + MFC_CPB_OFFSET, Length, FALSE);   // the caller's window, before the MFC reads it
    if (!S->Ready)
    {
        return MfcSessionHeader(M, S, Length, R);
    }

    for (Idx = 0; (Idx < S->NumDpb) && ((S->Used >> Idx) & 1u); Idx++)
    {
    }

    if (Idx == S->NumDpb)
    {
        MfcTrace("session: no free DPB (used 0x%x)\n", S->Used);
        return STATUS_DEVICE_BUSY;
    }

    Cmd           = (Length != 0) ? MFC_H2R_NAL_START : MFC_H2R_LAST_FRAME;
    Base          = S->Pa + S->DpbOffset + Idx * S->SlotSize;
    S->Available |= 1u << Idx;
    MfcSetStream(M, S, Length);
    Wr(M, MFC_REG_D_FIRST_PLANE_SIZE, S->LumaSize);
    Wr(M, MFC_REG_D_SECOND_PLANE_SIZE, S->ChromaSize);
    Wr(M, MFC_REG_D_FIRST_PLANE_DPB0 + Idx * 4u, Base);
    Wr(M, MFC_REG_D_SECOND_PLANE_DPB0 + Idx * 4u, Base + S->ChromaOffset);
    Wr(M, MFC_REG_D_NAL_START_OPTIONS, Rd(M, MFC_REG_D_NAL_START_OPTIONS) & ~0x8u);
    Wr(M, MFC_REG_D_DYN_DPB_LOWER, 1u << Idx);
    Wr(M, MFC_REG_D_DYN_DPB_UPPER, 0);
    Wr(M, MFC_REG_D_AVAIL_DPB_LOWER, S->Available);
    Wr(M, MFC_REG_D_AVAIL_DPB_UPPER, 0);
    Wr(M, MFC_REG_D_SLICE_IF_ENABLE, 0);
    Wr(M, MFC_REG_DEC_TIMEOUT_VALUE, 200000000u);
    Wr(M, MFC_REG_INSTANCE_ID, S->InstNo);
    R->R2h           = MfcExec(M, Cmd, 1000, FALSE, &R->Microseconds);
    R->Error         = Rd(M, MFC_REG_ERROR_CODE);
    R->DisplayStatus = Rd(M, MFC_REG_D_DISPLAY_STATUS);
    R->FrameType     = Rd(M, MFC_REG_D_DISPLAY_TYPE) & 7u;
    Luma             = Rd(M, MFC_REG_D_DISPLAY_LUMA);
    Consumed         = Rd(M, MFC_REG_D_DECODED_NAL_SIZE);
    S->Used          = Rd(M, MFC_REG_D_USED_DPB_LOWER);
    Disp             = R->DisplayStatus & 7u;
    Err              = R->Error & 0xFFFFu;
    if (R->R2h == 0)
    {
        MfcTrace("session: %s timeout (dpb %u)\n", (Cmd == MFC_H2R_NAL_START) ? "NAL_START" : "LAST_FRAME", Idx);
        return STATUS_IO_TIMEOUT;
    }

    if ((R->R2h != MFC_R2H_FRAME_DONE_RET) && (R->R2h != 14u) && (R->R2h != 15u) && (R->R2h != 9u) &&
        (R->R2h != MFC_R2H_ERR_RET))
    {
        MfcTrace("session: unexpected R2H %u err 0x%x\n", R->R2h, R->Error);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    if ((R->R2h == MFC_R2H_ERR_RET) && ((Err < 160u) || (Err > 222u)))
    {
        // a real error: skip what the firmware consumed (or the window) - the vendor drops the source buffer
        MfcTrace("session: ERR_RET err 0x%x status 0x%x consumed %u of %u\n", R->Error, R->DisplayStatus, Consumed,
                 Length);
        R->Flags    = MFC_DEC_ERROR;
        R->Consumed = ((Consumed != 0) && (Consumed <= Length)) ? Consumed : Length;
        return STATUS_SUCCESS;
    }

    if ((Disp == 1u) || (Disp == 2u))
    {
        Slot = MfcLumaToSlot(S, Luma);
        if (Slot != MAXULONG)
        {
            S->Available &= ~(1u << Slot);
            if (R->FrameType != 0)          // H.264 "not coded": nothing to show
            {
                R->Frame  = S->Va + S->DpbOffset + Slot * S->SlotSize;
                MfcFlush(R->Frame, S->SlotSize, TRUE);
                R->Flags |= MFC_DEC_FRAME;
                if ((R->Error >> 16) != 0)
                {
                    R->Flags |= MFC_DEC_WARNING;
                }

                S->Frames++;
            }
        }
    }

    if (Disp == 3u)
    {
        S->Ended  = TRUE;
        R->Flags |= MFC_DEC_EOS;
    }

    if (Disp == 2u)
    {
        R->Flags   |= MFC_DEC_NOT_CONSUMED;    // display only: the input was not used
        R->Consumed = 0;
    }
    else if (Cmd == MFC_H2R_NAL_START)
    {
        // vendor (mfc_isr.c / mfc_queue.c): refeed from consumed while more than 4 bytes (STUFF_BYTE) remain
        R->Consumed = ((Consumed != 0) && (Consumed < Length) && ((Length - Consumed) > 4u)) ? Consumed : Length;
    }

    return STATUS_SUCCESS;
}

VOID
MfcSessionClose(_In_ PMFC_DEVICE M, _Inout_ PMFC_SESSION S, _In_ BOOLEAN LeavePending)
{
    ULONG R2h;

    if (S->Opened)
    {
        Wr(M, MFC_REG_INSTANCE_ID, S->InstNo);
        R2h = MfcExec(M, MFC_H2R_CLOSE_INSTANCE, 1000, LeavePending, NULL);
        MfcTrace("session: CLOSE_INSTANCE R2H %u err 0x%x - %u frame(s)\n", R2h, Rd(M, MFC_REG_ERROR_CODE), S->Frames);
    }

    S->Opened = FALSE;
    S->Ready  = FALSE;
    S->Ended  = FALSE;
}
