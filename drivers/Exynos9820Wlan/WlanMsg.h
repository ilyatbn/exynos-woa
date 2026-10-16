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
    /* milestone 7a: data path (tx flowring + tx/rx buffers), NULL when its allocation failed */
    WDFCOMMONBUFFER       DataBuffer;
} WLAN_MSG_CTX, *PWLAN_MSG_CTX;

/* Milestone 7a receive hook: one 802.3 frame (not EAPOL - those go to the handshake), called from the poll
   path at PASSIVE_LEVEL with the msgbuf state held; Frame is valid only during the call. */
typedef VOID WLAN_RX_FN(_In_opt_ PVOID Ctx, _In_reads_bytes_(Len) const UCHAR *Frame, _In_ ULONG Len);

/* Milestone 6 join target, prepared at PrepareHardware from the registry (the passphrase is not kept). */
typedef struct _WLAN_JOIN_CONFIG {
    BOOLEAN               Valid;
    UCHAR                 SsidLen;
    CHAR                  Ssid[32];
    UCHAR                 Pmk[PMK_LEN];
} WLAN_JOIN_CONFIG, *PWLAN_JOIN_CONFIG;

/* Ssid: 1..32 printable ASCII; Passphrase: 8..63 ASCII (PBKDF2-HMAC-SHA1, 4096) or Psk: 64 hex chars. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS WlanJoinPrepare(_In_ PCUNICODE_STRING Ssid, _In_ PCUNICODE_STRING Passphrase, _In_ PCUNICODE_STRING Psk,
                         _Out_ PWLAN_JOIN_CONFIG J, _Inout_ PWLAN_REPORT R);

_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN WlanJoinSelfTest(_Inout_ PWLAN_REPORT R);   /* PBKDF2 against the 802.11i test vector, logged */

/* WLAN_MSG_CTX must outlive PrepareHardware (device context) once Resident is set. */
_IRQL_requires_(PASSIVE_LEVEL)
WLAN_FW_UP_FN WlanMsgRun;

/* Resident-firmware runtime (all PASSIVE_LEVEL, serialized by the caller; no-ops unless Resident). */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG WlanMsgPoll(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R);      /* drain the 3 D2H completion rings */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG WlanMsgTx(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_reads_bytes_(Len) const UCHAR *Frame,
                _In_ ULONG Len);                                           /* one 802.3 frame, TX_* */
_IRQL_requires_(PASSIVE_LEVEL)
VOID  WlanMsgSetRx(_Inout_ PWLAN_MSG_CTX M, _In_opt_ WLAN_RX_FN *Fn, _In_opt_ PVOID Ctx);
_IRQL_requires_(PASSIVE_LEVEL)
ULONG WlanMsgRescan(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R);    /* one more escan, SCAN_* */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG WlanMsgJoin(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_ const WLAN_JOIN_CONFIG *J);   /* JOIN_* */
/* ---- M9: the WiFiCx client (drivers/Exynos9820WlanCx, notes/m12-wifi-wificx.md). The OS supplicant runs the 4-way
   handshake over the data path, so the hooks also switch EAPOL to pass-through (RxFn, no M7b capture). ---- */

/* One wl_bss_info_v109 record (Len bytes, little-endian, IEs at its ie_offset) from an escan event, called from the
   poll path during WlanMsgRescan; every record is passed (no BSSID dedupe). */
typedef VOID WLAN_BSS_FN(_In_opt_ PVOID Ctx, _In_reads_bytes_(Len) const UCHAR *Bss, _In_ ULONG Len);
/* Association lost after WlanMsgConnect succeeded (WLC_E_DEAUTH_IND / DISASSOC_IND / LINK down), from the poll path. */
typedef VOID WLAN_LINK_FN(_In_opt_ PVOID Ctx, _In_ ULONG Event, _In_ ULONG Status, _In_ ULONG Reason);

typedef struct _WLAN_CONNECT {
    UCHAR               Bssid[6];
    UCHAR               SsidLen;
    UCHAR               Ssid[32];
    USHORT              Chanspec;       /* firmware chanspec of the BSS from the scan (0 = unknown: SSID-only join) */
    BOOLEAN             Rsn;            /* WPA2-PSK / CCMP; FALSE = open */
    ULONG               ApRsnLen;       /* the AP's RSN IE (from its beacon / probe response) when Rsn */
    UCHAR               ApRsn[RSN_IE_MAX];
} WLAN_CONNECT, *PWLAN_CONNECT;

typedef struct _WLAN_ASSOC_INFO {
    LONG                SetSsid;        /* WLC_E_SET_SSID status, -1 = none */
    ULONG               Flags;          /* assoc_info flags */
    USHORT              ReqCap, ReqListen, RespCap, RespStatus, RespAid;
    ULONG               ReqIesLen;
    ULONG               RespIesLen;
    UCHAR               ReqIes[WLAN_ASSOC_IES_MAX];
    UCHAR               RespIes[WLAN_ASSOC_IES_MAX];
    BOOLEAN             FlowOpen;
    ULONG               Ms;
} WLAN_ASSOC_INFO, *PWLAN_ASSOC_INFO;

_IRQL_requires_(PASSIVE_LEVEL)
VOID  WlanMsgSetCxHooks(_Inout_ PWLAN_MSG_CTX M, _In_opt_ WLAN_BSS_FN *BssFn, _In_opt_ WLAN_LINK_FN *LinkFn,
                        _In_opt_ PVOID Ctx);
_IRQL_requires_(PASSIVE_LEVEL)
ULONG WlanMsgConnect(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_ const WLAN_CONNECT *C,
                     _Out_ PWLAN_ASSOC_INFO A);                            /* JOIN_*: associated + flowring */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG WlanMsgAmpduTid(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_ UCHAR Tid, _In_ BOOLEAN Enable);  /* MSG_* */
ULONG WlanMsgScbAuthorize(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_reads_(6) const UCHAR *Peer);  /* MSG_* */
ULONG WlanMsgDump(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_z_ PCSTR Section);  /* MSG_* */
VOID  WlanMsgCounters(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_z_ PCSTR When);  /* "[cnt]" lines */
ULONG WlanMsgStaInfo(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_reads_(6) const UCHAR *Peer);  /* "[sta]" */
VOID  WlanMsgBw40Experiment(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R);  /* debug "bw40" */
VOID  WlanMsgSetCountry(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_reads_(2) PCSTR Cc);  /* debug */
ULONG WlanMsgSetIntVar(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_z_ PCSTR Name, _In_ ULONG Value);
ULONG WlanMsgGetVarRaw(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_z_ PCSTR Name);  /* "[var]" line */
ULONG WlanMsgGetIntVar(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_z_ PCSTR Name, _Out_ PULONG Value);
ULONG WlanMsgSetPm(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_ ULONG Mode, _Out_ PULONG Old);  /* MSG_* */
ULONG WlanMsgGetRate(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _Out_ PULONG Rate500k);  /* MSG_* */
ULONG WlanMsgDisconnect(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R);  /* MSG_* */
/* CCMP key (16 B): Ea != NULL = pairwise for that peer (index 0), Ea == NULL = group key Index (primary), Rsc =
   6-byte receive sequence counter or NULL. MSG_*. */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG WlanMsgSetKey(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_ ULONG Index, _In_reads_(16) const UCHAR *Key,
                    _In_reads_opt_(6) const UCHAR *Ea, _In_reads_opt_(6) const UCHAR *Rsc);
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN WlanMsgWaitTxIdle(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R, _In_ ULONG Ms);   /* TRUE = no tx pending */

_IRQL_requires_(PASSIVE_LEVEL)
VOID  WlanMsgQuiesce(_Inout_ PWLAN_MSG_CTX M, _Inout_ PWLAN_REPORT R);   /* ARM halt, 1 ms, BME off */
_IRQL_requires_(PASSIVE_LEVEL)
VOID  WlanMsgFree(_Inout_ PWLAN_MSG_CTX M);                              /* after Quiesce: buffer, state, BARs */

#endif /* _WLAN_MSG_H */
