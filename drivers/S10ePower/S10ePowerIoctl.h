/*
 * S10ePowerIoctl.h - user/kernel interface of S10ePower (notes/clocks-dvfs.md). Shared by the driver, the governor
 * service and the test tool.
 * SPDX-License-Identifier: GPL-2.0
 */
#pragma once

#define S10EPOWER_DEVICE_NAME  L"\\Device\\S10ePower"
#define S10EPOWER_DOS_NAME     L"\\DosDevices\\S10ePower"
#define S10EPOWER_USER_PATH    L"\\\\.\\S10ePower"

#define S10EPOWER_TYPE  0x8A53u
#define S10EPOWER_CTL(n) CTL_CODE(S10EPOWER_TYPE, 0x800 + (n), METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Domains (ACPM DVFS fvmap indices / ids on channel 0 - device-validated from UEFI, notes/m15-perf.md runs 31/32 and
 * notes/thermal-research.md Q5) */
#define S10EPOWER_DOM_LITTLE  2u
#define S10EPOWER_DOM_MID     3u
#define S10EPOWER_DOM_G3D     10u

/* Thermal zones (ACPM ch 9 READ_TEMP tzid; DT ids 0 BIG, 1 MID, 2 LITTLE, 3 G3D, 4 ISP - tz->id equality UNVERIFIED) */
#define S10EPOWER_TZ_COUNT    5u

typedef struct _S10EPOWER_STATE {
    unsigned int Version;              /* 1 */
    unsigned int AllowSend;            /* registry switch: 0 = read-only (C0) */
    unsigned int TempC[S10EPOWER_TZ_COUNT];   /* last READ_TEMP per tz, 0xFF = not read */
    unsigned int TempReads, TempFails;
    unsigned int CurKhz[3];            /* last FREQ_GET / granted: LITTLE, MID, G3D */
    unsigned int CapKhz[3];            /* thermal-guard cap per domain */
    unsigned int ReqKhz[3];            /* last requested (before the cap) */
    unsigned int Sends, SendFails, Timeouts;
    unsigned int GuardLevel;           /* 0 none, 1 control temp, 2 alert, 3 emergency minimum */
} S10EPOWER_STATE;

typedef struct _S10EPOWER_SET_FREQ {
    unsigned int Domain;               /* S10EPOWER_DOM_* */
    unsigned int Khz;                  /* rounded down to the domain's ladder, then capped */
} S10EPOWER_SET_FREQ;

#define IOCTL_S10EPOWER_GET_STATE  S10EPOWER_CTL(0)   /* out: S10EPOWER_STATE */
#define IOCTL_S10EPOWER_SET_FREQ   S10EPOWER_CTL(1)   /* in: S10EPOWER_SET_FREQ, out: S10EPOWER_STATE */
