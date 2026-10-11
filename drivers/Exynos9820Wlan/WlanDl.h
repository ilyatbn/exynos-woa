/*  WlanDl.h - milestone-2 BCM4375 firmware download/start: interface between WlanProbe.c (report,
    resources, registry) and WlanDl.c (backplane, EROM, ARM CR4, TCM). notes/m12-wifi-m2.md.
*/

#ifndef _WLAN_DL_H
#define _WLAN_DL_H

#include "Exynos9820Wlan.h"

typedef struct _WLAN_REPORT {
    CHAR    Text[WLAN_REPORT_MAX];
    size_t  Used;
} WLAN_REPORT, *PWLAN_REPORT;

VOID ReportLine(_Inout_ PWLAN_REPORT R, _In_z_ _Printf_format_string_ PCSTR Fmt, ...);
VOID ReportWriteFile(_In_ PWLAN_REPORT R);   /* (re)writes the whole report file now */

#define WLAN_NAME_MAX   64

/* Settings from Services\Exynos9820Wlan\Parameters (README.md "Settings"). */
typedef struct _WLAN_DL_CONFIG {
    ULONG   Download;                   /* 0 = milestone-1 probe only; default 1 */
    ULONG   OtpProbe;                   /* 0 = never write the OTP/PMU registers; default 1 */
    WCHAR   NvramFile[WLAN_NAME_MAX];   /* empty = choose from OTP, else WLAN_NVRAM_DEFAULT */
    WCHAR   FirmwareFile[WLAN_NAME_MAX];/* empty = WLAN_FW_DEFAULT */
} WLAN_DL_CONFIG, *PWLAN_DL_CONFIG;

/* What the download did; also kept in the device context for EvtDeviceReleaseHardware. */
typedef struct _WLAN_DL_RESULT {
    ULONG   DlResult;                   /* DL_* (Exynos9820Wlan.h) */
    ULONG   PcieRev;
    ULONG   Cr4Rev;
    ULONG   RamSize;
    ULONG   FwSize;
    ULONG   NvramLen;                   /* processed length incl. the 4-byte token */
    ULONG   NvramToken;
    ULONG   OtpVid;                     /* (vid[0] << 8) | vid[1]; 0 = not read */
    ULONG   SharedAddr;
    ULONG   SharedFlags;
    ULONG   ConsoleAddr;
    ULONG   Cr4Wrap[3];                 /* AI wrappers of the ARM CR4 (0 = none) */
    BOOLEAN ArmReleased;                /* TRUE while our firmware may be running */
    WCHAR   NvramUsed[WLAN_NAME_MAX];
} WLAN_DL_RESULT, *PWLAN_DL_RESULT;

_IRQL_requires_(PASSIVE_LEVEL)
VOID WlanDlRun(_Inout_ PWLAN_REPORT R, _In_ volatile UCHAR *Cfg, _In_ ULONG Bar0Lo, _In_ ULONG Bar2Lo,
               _In_ ULONG Command, _In_ const WLAN_DL_CONFIG *Config, _Out_ PWLAN_DL_RESULT Out);

#endif /* _WLAN_DL_H */
