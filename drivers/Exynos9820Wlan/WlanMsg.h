/*  WlanMsg.h - milestone-3 msgbuf exchange (WlanMsg.c), run as WlanDlRun's fw-up callback.
    notes/m12-wifi-m3.md. */

#ifndef _WLAN_MSG_H
#define _WLAN_MSG_H

#include "WlanDl.h"

typedef struct _WLAN_MSG_CTX {
    WDFDMAENABLER         DmaEnabler;   /* created in EvtDeviceAdd (NULL = none) */
    const WLAN_DL_CONFIG *Config;
    ULONG                 Result;       /* MSG_* */
    UCHAR                 Mac[6];
    BOOLEAN               MacValid;
    ULONG                 ScanResult;   /* SCAN_* (milestone 4) */
    ULONG                 BssCount;
    /* milestone 5: firmware resident after PrepareHardware (Config->Resident and M3/M4 OK) */
    BOOLEAN               Resident;     /* State/CommonBuffer/Bus below are live */
    BOOLEAN               Quiesced;     /* ARM halted + bus mastering off (WlanMsgQuiesce) */
    PVOID                 State;        /* WlanMsg.c MSG_STATE */
    WDFCOMMONBUFFER       CommonBuffer;
    WLAN_BUS              Bus;          /* kept copy; owns BAR0/BAR2 + chip until WlanMsgFree */
} WLAN_MSG_CTX, *PWLAN_MSG_CTX;

/* WLAN_MSG_CTX must outlive PrepareHardware (device context) once Resident is set. */
_IRQL_requires_(PASSIVE_LEVEL)
WLAN_FW_UP_FN WlanMsgRun;

/* Resident-firmware runtime (all PASSIVE_LEVEL, serialized by the caller; no-ops unless Resident). */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG WlanMsgPoll(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R);      /* drain D2H control completions */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG WlanMsgRescan(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R);    /* one more escan, SCAN_* */
_IRQL_requires_(PASSIVE_LEVEL)
VOID  WlanMsgQuiesce(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R);   /* ARM halt, 1 ms, BME off */
_IRQL_requires_(PASSIVE_LEVEL)
VOID  WlanMsgFree(_Inout_ PWLAN_MSG_CTX M);                              /* after Quiesce: buffer, state, BARs */

#endif /* _WLAN_MSG_H */
