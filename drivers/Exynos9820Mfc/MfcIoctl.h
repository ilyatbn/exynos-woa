/*
 * MfcIoctl.h - Exynos9820Mfc user-mode interface (D10 V3, notes/d10-mfc-decode-flow.md). Shared by the driver and
 * tools/mfc/mfcdec. One decode session per open handle, one session device-wide (the MFC runs one instance here).
 *
 * Flow: IOCTL_MFC_OPEN (H.264) -> IOCTL_MFC_DECODE repeatedly -> IOCTL_MFC_CLOSE (or close the handle).
 * DECODE input  = a window of the Annex-B stream starting where the previous call's Consumed ended
 *                 (<= MFC_MAX_WINDOW bytes; it must hold at least one whole access unit). Empty input = drain
 *                 (end of stream): repeat until MFC_DEC_EOS.
 * DECODE output = MFC_DECODE_OUT followed, when MFC_DEC_FRAME is set, by one NV12 picture: Width x Height luma rows
 *                 then Width x Height/2 interleaved CbCr rows, tightly packed (stride = Width). Crop is reported, not
 *                 applied.
 */
#pragma once

// {6A2E8C51-0B7D-4C3E-8F14-982AC0DE0D10}
DEFINE_GUID(GUID_DEVINTERFACE_EXYNOS9820_MFC, 0x6a2e8c51, 0x0b7d, 0x4c3e, 0x8f, 0x14, 0x98, 0x2a, 0xc0, 0xde, 0x0d, 0x10);

#define IOCTL_MFC_OPEN    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MFC_DECODE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_MFC_CLOSE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define MFC_IOCTL_VERSION  1u
#define MFC_CODEC_H264     0u
#define MFC_MAX_WINDOW     ((4u << 20) - 512u)   // CPB 4 MiB, 512 B kept free (mfc_macros.h)

#define MFC_OPEN_DECODE_ORDER 0x1u   // pictures come back in decode order, each with the call that decoded it (DXVA)

typedef struct _MFC_OPEN_IN
{
    unsigned int Version;   // MFC_IOCTL_VERSION
    unsigned int Codec;     // MFC_CODEC_H264
    unsigned int Flags;     // MFC_OPEN_* (driver 1.0.0.7+; an 8-byte input = 0)
} MFC_OPEN_IN;

#define MFC_DEC_HEADER        0x01u   // sequence header parsed and buffers set up; no picture this call
#define MFC_DEC_FRAME         0x02u   // a displayed picture follows the header
#define MFC_DEC_EOS           0x04u   // drain finished: no more pictures
#define MFC_DEC_NOT_CONSUMED  0x08u   // display-only result: resend the same window (Consumed = 0)
#define MFC_DEC_ERROR         0x10u   // the firmware reported an error (Error); Consumed says what to skip
#define MFC_DEC_WARNING       0x20u   // the displayed picture carries a decode warning (Error >> 16)

typedef struct _MFC_DECODE_OUT
{
    unsigned int Flags;
    unsigned int Consumed;      // bytes of the input window used; continue from there
    unsigned int Width;         // picture size of the NV12 data (D_DISPLAY_FRAME_WIDTH/HEIGHT)
    unsigned int Height;
    unsigned int CropLeft;
    unsigned int CropTop;
    unsigned int CropRight;
    unsigned int CropBottom;
    unsigned int FrameType;     // 1 I, 2 P, 3 B (D_DISPLAY_FRAME_TYPE)
    unsigned int R2h;           // firmware completion code
    unsigned int Error;         // ERROR_CODE (error [15:0], warning [31:16])
    unsigned int DisplayStatus; // D_DISPLAY_STATUS
    unsigned int FrameBytes;    // bytes of NV12 data after this header (0 without MFC_DEC_FRAME)
    unsigned int HwMicroseconds;// time from command to completion
} MFC_DECODE_OUT;
