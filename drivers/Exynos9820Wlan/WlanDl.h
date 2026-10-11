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
    ULONG   Msgbuf;                     /* 0 = stop after milestone 2 (fw left running, no DMA); default 1 */
    ULONG   HostCap;                    /* WLAN_HOSTCAP_AUTO (default), WLAN_HOSTCAP_DONT_WRITE, or a literal */
    ULONG   DoorbellDar;                /* 1 = ring the DAR doorbells 0xA20/0xA24 instead of 0x140/0x144 */
    ULONG   Scan;                       /* milestone 4: 1 (default) = events + one escan after the M3 ioctls */
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

/* Milestone 3 hand-over: WlanDlRun builds this once the firmware is up (DL 0) and calls the
   WLAN_FW_UP_FN it was given, before it unmaps anything. Valid only during that call. */
typedef struct _WLAN_BUS {
    PVOID           Chip;               /* WlanDl.c private (BAR0 window state, EROM) */
    volatile UCHAR *Cfg;                /* EP config space (CFG0 window) */
    volatile UCHAR *Tcm;                /* BAR2: offset == backplane address */
    ULONG           RamBase;
    ULONG           RamEnd;
    ULONG           SharedAddr;
    ULONG           SharedFlags;
    ULONG           ConsoleAddr;
    ULONG           ConsoleIdx;         /* console write index at the last dump (0xFFFFFFFF = none) */
    ULONG           PcieCoreBase;
    ULONG           PcieRev;
    ULONG           Cr4Wrap[3];
    ULONG           Command;            /* EP COMMAND as the probe read it */
    BOOLEAN         ArmHalted;          /* set by WlanBusArmHalt */
} WLAN_BUS, *PWLAN_BUS;

typedef VOID WLAN_FW_UP_FN(_Inout_ PWLAN_REPORT R, _Inout_ PWLAN_BUS Bus, _Inout_opt_ PVOID Ctx);

ULONG WlanBusBpRd(_Inout_ PWLAN_BUS Bus, _In_ ULONG Addr);              /* backplane via the BAR0 window */
VOID  WlanBusBpWr(_Inout_ PWLAN_BUS Bus, _In_ ULONG Addr, _In_ ULONG Val);
/* Reads WLAN_BLOB_DIR\Name into a paged pool buffer (tag 'lDlW', caller frees); fails above Max bytes. */
NTSTATUS WlanLoadBlob(_In_z_ PCWSTR Name, _Outptr_result_bytebuffer_(*Len) PUCHAR *Buf, _Out_ PULONG Len, _In_ ULONG Max);
VOID  WlanBusArmHalt(_Inout_ PWLAN_BUS Bus);                            /* CR4 core reset, CPUHALT */
VOID  WlanBusConsoleDump(_Inout_ PWLAN_REPORT R, _Inout_ PWLAN_BUS Bus);  /* new console bytes since last */

_IRQL_requires_(PASSIVE_LEVEL)
VOID WlanDlRun(_Inout_ PWLAN_REPORT R, _In_ volatile UCHAR *Cfg, _In_ ULONG Bar0Lo, _In_ ULONG Bar2Lo,
               _In_ ULONG Command, _In_ const WLAN_DL_CONFIG *Config, _Out_ PWLAN_DL_RESULT Out,
               _In_opt_ WLAN_FW_UP_FN *FwUp, _Inout_opt_ PVOID FwUpCtx);

#endif /* _WLAN_DL_H */
