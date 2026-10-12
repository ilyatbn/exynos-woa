/*  WlanSup.h - milestone 7b host WPA2-PSK supplicant crypto (WlanSup.c): IEEE 802.11i PRF (HMAC-SHA1),
    EAPOL-Key MIC, AES key unwrap (RFC 3394), nonces - all through kernel CNG (ksecdd). notes/m12-wifi-m7.md. */

#ifndef _WLAN_SUP_H
#define _WLAN_SUP_H

#include "WlanDl.h"

#define SUP_PTK_LEN         48      /* CCMP: KCK 16 + KEK 16 + TK 16 (wpa_common.c:597-650) */
#define SUP_KCK_OFF         0
#define SUP_KEK_OFF         16
#define SUP_TK_OFF          32
#define SUP_KEY_LEN         16
#define SUP_NONCE_LEN       32      /* WPA_NONCE_LEN wpa_common.h:23 */
#define SUP_MIC_LEN         16

/* PRF-n (802.11i 8.5.1.1): Out = first OutLen bytes of HMAC-SHA1(K, Label 0 B i) for i = 0, 1, ... */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SupPrf(_In_reads_bytes_(KLen) const UCHAR *K, _In_ ULONG KLen, _In_z_ PCSTR Label,
                _In_reads_bytes_(BLen) const UCHAR *B, _In_ ULONG BLen, _Out_writes_bytes_(OutLen) PUCHAR Out,
                _In_ ULONG OutLen);

/* PTK = PRF-384(PMK, "Pairwise key expansion", min/max(AA, SPA), min/max(ANonce, SNonce)). */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SupPtk(_In_reads_(32) const UCHAR *Pmk, _In_reads_(6) const UCHAR *Aa, _In_reads_(6) const UCHAR *Spa,
                _In_reads_(SUP_NONCE_LEN) const UCHAR *ANonce, _In_reads_(SUP_NONCE_LEN) const UCHAR *SNonce,
                _Out_writes_(SUP_PTK_LEN) PUCHAR Ptk);

/* EAPOL-Key MIC: HMAC-SHA1-128(KCK, Frame) (the caller zeroes the MIC field first). */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SupMic(_In_reads_(SUP_KEY_LEN) const UCHAR *Kck, _In_reads_bytes_(Len) const UCHAR *Frame, _In_ ULONG Len,
                _Out_writes_(SUP_MIC_LEN) PUCHAR Mic);

/* RFC 3394 AES key unwrap with a 128-bit KEK: In = Len bytes (multiple of 8, >= 24), Out = Len - 8 bytes.
   STATUS_DATA_ERROR when the integrity check value is not A6A6A6A6A6A6A6A6. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SupAesUnwrap(_In_reads_(SUP_KEY_LEN) const UCHAR *Kek, _In_reads_bytes_(Len) const UCHAR *In, _In_ ULONG Len,
                      _Out_writes_bytes_(Len - 8) PUCHAR Out);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SupRandom(_Out_writes_bytes_(Len) PUCHAR Out, _In_ ULONG Len);

/* PRF (802.11i H.3 cases 1-2) + RFC 3394 4.1 unwrap; one report line. */
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN SupSelfTest(_Inout_ PWLAN_REPORT R);

#endif /* _WLAN_SUP_H */
