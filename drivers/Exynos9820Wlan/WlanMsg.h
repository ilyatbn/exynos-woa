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
} WLAN_MSG_CTX, *PWLAN_MSG_CTX;

_IRQL_requires_(PASSIVE_LEVEL)
WLAN_FW_UP_FN WlanMsgRun;

#endif /* _WLAN_MSG_H */
