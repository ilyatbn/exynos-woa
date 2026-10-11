/*
 * S10eDiagIoctl.h - interface between S10eDiag.sys and its user-mode tools (s10ediag.exe, S10eAgent.exe).
 * notes/m11-s10ediag.md.
 */

#pragma once

#define S10EDIAG_DEVICE_NAME      L"\\Device\\S10eDiag"
#define S10EDIAG_DOS_NAME         L"\\DosDevices\\S10eDiag"
#define S10EDIAG_USER_PATH        L"\\\\.\\S10eDiag"

#define S10EDIAG_TYPE             0x8D10u

// in: S10EDIAG_READ_IN, out: Length bytes
#define IOCTL_S10EDIAG_READ_PHYS        CTL_CODE(S10EDIAG_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
// in: S10EDIAG_WRITE32_IN
#define IOCTL_S10EDIAG_WRITE32          CTL_CODE(S10EDIAG_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
// no buffers: INFORM3 = recovery now, and again from the shutdown notification (clean restart -> TWRP)
#define IOCTL_S10EDIAG_RECOVERY_ON_RESET CTL_CODE(S10EDIAG_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
// no buffers: INFORM3 = recovery + SWRESET immediately (unclean)
#define IOCTL_S10EDIAG_HARD_RESET       CTL_CODE(S10EDIAG_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
// in: ULONG seconds (0 = cancel): hard reset to recovery when it expires
#define IOCTL_S10EDIAG_FALLBACK         CTL_CODE(S10EDIAG_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
// out: S10EDIAG_INFO
#define IOCTL_S10EDIAG_INFO             CTL_CODE(S10EDIAG_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
// in: ASCII text (no terminator needed): one line in the PRAM mark ring (survives a reset; the firmware's boot
// pair saves 0x8FE00000+64 KiB as \WINPRAM.BIN, the ring is at +0xA000)
#define IOCTL_S10EDIAG_MARK             CTL_CODE(S10EDIAG_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Mark ring: firmware-reserved WINPRAM (BEYOND0LTE_WINPRAM_BASE) + 0xA000, 16 KiB, unused by Exynos9820Ufs
// (it uses +0x0..0x3F60, +0x8000..0x8FFF, +0xF000). Header line 0: "S10EMARK" + next index; then 255 lines
// of 64 bytes: "<uptime ms, 10 digits> <text>".
#define S10EDIAG_MARK_PHYSICAL  0x8FE0A000ULL
#define S10EDIAG_MARK_SIZE      0x4000u
#define S10EDIAG_MARK_LINE      64u

#define S10EDIAG_READ_MMIO   0u   // map non-cached, 32-bit reads (registers)
#define S10EDIAG_READ_RAM    1u   // MmCopyMemory(MM_COPY_MEMORY_PHYSICAL) (system RAM)
#define S10EDIAG_READ_MAX    0x10000u

typedef struct _S10EDIAG_READ_IN {
    unsigned long long Address;
    unsigned long Length;      // bytes; MMIO: multiple of 4
    unsigned long Mode;        // S10EDIAG_READ_*
} S10EDIAG_READ_IN;

typedef struct _S10EDIAG_WRITE32_IN {
    unsigned long long Address;
    unsigned long Value;
    unsigned long Reserved;
} S10EDIAG_WRITE32_IN;

typedef struct _S10EDIAG_INFO {
    unsigned long Version;
    unsigned long RecoveryArmed;
    unsigned long FallbackSeconds;
    unsigned long Inform3;           // PMU INFORM3 as read now
    unsigned long long UptimeMs;
} S10EDIAG_INFO;

#define S10EDIAG_VERSION  2u
