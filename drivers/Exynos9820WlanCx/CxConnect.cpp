/*  CxConnect.cpp - Exynos9820WlanCx W4: connect / keys / disconnect / link loss (notes/m12-wifi-wificx.md).

    WDI_TASK_CONNECT (learn.microsoft.com OID_WDI_TASK_CONNECT): join one of the host's BSS entries with the firmware
    (WlanMsgConnect: no host supplicant in the driver), report each attempt with WDI_INDICATION_ASSOCIATION_RESULT
    (association request + response frame bodies, "Windows requires that the driver indicates the Association
    Request and Response frames", wificx-wi-fi-7 page), then LINK_STATE_CHANGE and the M4 CONNECT_COMPLETE.
    The OS supplicant then runs the 4-way handshake over the data path (EAPOL passes through CxData.cpp) and plumbs
    the keys with WDI_SET_ADD_CIPHER_KEYS -> "wsec_key" (WlanMsgSetKey), after the pending tx (EAPOL 4/4) is on the
    air (dhd wl_cfg80211.c:6903-6904).
*/

#include "Cx.h"

#define CX_CONNECT_TRIES    2           /* host choice: BSS entries tried per WDI connect (each may take 20 s) */
#define CX_LINK_KBPS        54000       /* placeholder Tx/Rx link speed for LINK_STATE_CHANGE (UNVERIFIED unit kbit/s;
                                           the firmware rate is not read yet) */

/* notes/wifi-throughput.md (no Block Ack session): IE ids + lengths of an association frame, full bytes only for the
   capability elements that decide aggregation (HT cap 45, HT op 61, ext cap 127, VHT cap 191, VHT op 192, WMM
   vendor 221 00-50-F2 type 2). The SSID and other elements are not printed. */
static VOID
CxLogIes(PCX_DEVICE Dev, PCSTR Tag, const UCHAR *Ies, ULONG Len)
{
    CHAR  Line[200];
    ULONG Off = 0;

    while (Off + 2 <= Len && Off + 2 + Ies[Off + 1] <= Len) {
        UCHAR  Id = Ies[Off], L = Ies[Off + 1];
        const UCHAR *V = Ies + Off + 2;
        BOOLEAN Full = (Id == 48 || Id == 45 || Id == 61 || Id == 127 || Id == 191 || Id == 192 ||
                        (Id == 221 && L >= 4 && V[0] == 0x00 && V[1] == 0x50 && V[2] == 0xF2 && V[3] == 0x02));
        size_t Pos = 0;

        (VOID)RtlStringCbPrintfA(Line, sizeof(Line), "[ie %s] id %u len %u", Tag, Id, L);
        if (Full) {
            (VOID)RtlStringCbLengthA(Line, sizeof(Line), &Pos);
            for (ULONG I = 0; I < L && Pos + 4 < sizeof(Line); I++, Pos += 3) {
                (VOID)RtlStringCbPrintfA(Line + Pos, sizeof(Line) - Pos, " %02X", V[I]);
            }
        }
        CxLog(Dev, "%s\r\n", Line);
        Off += 2 + L;
    }
}

/* First IE with Id in an IE blob, or NULL. */
static const UCHAR *
CxFindIe(const UCHAR *Ies, ULONG Len, UCHAR Id)
{
    ULONG P = 0;

    while (P + 2 <= Len && P + 2 + Ies[P + 1] <= Len) {
        if (Ies[P] == Id) {
            return Ies + P;
        }
        P += 2 + Ies[P + 1];
    }
    return NULL;
}

_Use_decl_annotations_
VOID
CxSetLinked(PCX_DEVICE Dev, BOOLEAN Linked)
{
    NET_ADAPTER_LINK_STATE Ls;

    if (Linked == Dev->Linked) {
        return;
    }
    Dev->Linked = Linked;
    if (!Linked) {
        Dev->KeysSet = FALSE;   /* the firmware drops the keys with the association */
    }
    /* 5 ms worker waits need a 1 ms system timer while associated (rx latency); released on disconnect */
    (VOID)ExSetTimerResolution(10000, Linked);
    if (Dev->Adapter != NULL) {
        NET_ADAPTER_LINK_STATE_INIT(&Ls, Linked ? (ULONG64)CX_LINK_KBPS * 1000 : NDIS_LINK_SPEED_UNKNOWN,
                                    Linked ? MediaConnectStateConnected : MediaConnectStateDisconnected,
                                    MediaDuplexStateHalf, NetAdapterPauseFunctionTypeUnsupported,
                                    NetAdapterAutoNegotiationFlagNone);
        NetAdapterSetLinkState(Dev->Adapter, &Ls);
    }
    CxLog(Dev, "[link] %s\r\n", Linked ? "connected (adapter link up, 5 ms worker poll)" : "disconnected");
    if (Linked) {
        ULONG Old;
        ULONG Rc = WlanMsgSetPm(&Dev->Msg, Dev->RunR, WL_PM_FAST, &Old);
        CxLog(Dev, "[link] power save: firmware had PM %lu, set PM_FAST -> MSG %lu\r\n", Old, Rc);
        /* throughput diagnostics (notes/wifi-throughput.md: 144 Mbit/s PHY but ~14 Mbit/s TCP, deep tx queue) */
        static const PCSTR Vars[] = { "ampdu", "ampdu_ba_wsize", "ampdu_mpdu", "frameburst", "nmode", "vhtmode", "wme", "wme_apsd", "ampdu_rx", "ampdu_tx" };
        for (ULONG I = 0; I < ARRAYSIZE(Vars); I++) {
            ULONG Val;
            ULONG VRc = WlanMsgGetIntVar(&Dev->Msg, Dev->RunR, Vars[I], &Val);
            CxLog(Dev, "[link] iovar %s = %ld (MSG %lu)\r\n", Vars[I], (LONG)Val, VRc);
        }
    }
    CxUpdateRate(Dev);
}

/* Worker thread only (it owns the firmware command path). Reads the firmware's current rate and reports it to
   NetAdapterCx instead of the CX_LINK_KBPS placeholder (notes/wifi-throughput.md). */
VOID
CxUpdateRate(PCX_DEVICE Dev)
{
    NET_ADAPTER_LINK_STATE Ls;
    ULONG                  R500 = 0;
    ULONG                  Rc;

    if (!Dev->Linked) {
        return;
    }
    Rc = WlanMsgGetRate(&Dev->Msg, Dev->RunR, &R500);
    CxLog(Dev, "[link] firmware rate %lu.%lu Mbit/s (MSG %lu)\r\n", R500 / 2, (R500 & 1) ? 5ul : 0ul, Rc);
    if (Rc != MSG_OK || R500 == 0 || Dev->Adapter == NULL) {
        return;
    }
    NET_ADAPTER_LINK_STATE_INIT(&Ls, (ULONG64)R500 * 500000, MediaConnectStateConnected, MediaDuplexStateHalf,
                                NetAdapterPauseFunctionTypeUnsupported, NetAdapterAutoNegotiationFlagNone);
    NetAdapterSetLinkState(Dev->Adapter, &Ls);
}

/* Unsolicited WDI_INDICATION_DISASSOCIATION for the current peer. */
static VOID
CxIndicateDisassoc(PCX_DEVICE Dev, WDI_ASSOC_STATUS Reason)
{
    WDI_INDICATION_DISASSOCIATION_PARAMETERS P;
    UINT8                                   *Out = NULL;
    ULONG                                    OutLen = 0;

    RtlCopyMemory(P.DisconnectIndicationParameters.MacAddress.Address, Dev->Bssid, 6);
    P.DisconnectIndicationParameters.DisassociationWABIReason = Reason;
    if (GenerateWdiIndicationDisassociation(&P, 0, &Dev->Tlv, &OutLen, &Out) == NDIS_STATUS_SUCCESS) {
        CxIndicate(Dev, WDI_INDICATION_DISASSOCIATION, NULL, NDIS_STATUS_SUCCESS, Out, OutLen);
        FreeGenerated(Out);
    }
}

/* WLAN_LINK_FN: the firmware lost the association (worker thread, inside WlanMsgPoll). */
_Use_decl_annotations_
VOID
CxLinkLost(PVOID Ctx, ULONG Event, ULONG Status, ULONG Reason)
{
    PCX_DEVICE Dev = (PCX_DEVICE)Ctx;

    CxLog(Dev, "[link] firmware event %lu status %lu reason %lu: association lost\r\n", Event, Status, Reason);
    if (!Dev->Linked) {
        return;
    }
    CxIndicateDisassoc(Dev, (Event == WLC_E_DEAUTH_IND) ? WDI_ASSOC_STATUS_PEER_DEAUTHENTICATED :
                            (Event == WLC_E_DISASSOC_IND) ? WDI_ASSOC_STATUS_PEER_DISASSOCIATED :
                            WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN);
    CxSetLinked(Dev, FALSE);
}

/* Host-side disconnect (TASK_DISCONNECT, DOT11_RESET): firmware disassociation + indication. */
_Use_decl_annotations_
VOID
CxDisconnect(PCX_DEVICE Dev, WDI_ASSOC_STATUS Reason)
{
    if (!Dev->Linked) {
        return;
    }
    (VOID)WlanMsgDisconnect(&Dev->Msg, Dev->RunR);
    CxIndicateDisassoc(Dev, Reason);
    CxSetLinked(Dev, FALSE);
}

/* WDI_INDICATION_ASSOCIATION_RESULT for one attempt. A = NULL for an attempt that did not reach SET_SSID 0. */
static VOID
CxIndicateAssoc(PCX_DEVICE Dev, const UCHAR *Bssid, WDI_ASSOC_STATUS St, const WLAN_ASSOC_INFO *A, PCX_BSS B,
                WDI_AUTH_ALGORITHM Auth, WDI_CIPHER_ALGORITHM Ucast, WDI_CIPHER_ALGORITHM Mcast)
{
    WDI_INDICATION_ASSOCIATION_RESULT_LIST P;
    WDI_ASSOCIATION_RESULT_CONTAINER      *R = new WDI_ASSOCIATION_RESULT_CONTAINER[1];
    PUCHAR                                 Req = NULL, Resp = NULL, Bcn = NULL;
    UINT8                                 *Out = NULL;
    ULONG                                  OutLen = 0;
    NDIS_STATUS                            Ns;
    WDI_PHY_TYPE                           Phy;
    const UCHAR                           *RespIes = (A != NULL) ? A->RespIes : NULL;
    ULONG                                  RespIesLen = (A != NULL) ? A->RespIesLen : 0;

    if (R == NULL) {
        return;
    }
    RtlCopyMemory(R->BSSID.Address, Bssid, 6);
    R->AssociationResultParameters.AssociationStatus            = St;
    R->AssociationResultParameters.StatusCode                   = (A != NULL) ? A->RespStatus : 0;
    R->AssociationResultParameters.ReAssociation                = FALSE;
    R->AssociationResultParameters.AuthAlgorithm                = Auth;
    R->AssociationResultParameters.UnicastCipherAlgorithm       = Ucast;
    R->AssociationResultParameters.MulticastDataCipherAlgorithm = Mcast;
    R->AssociationResultParameters.MulticastMgmtCipherAlgorithm = WDI_CIPHER_ALGO_NONE;
    R->AssociationResultParameters.PortAuthorized               = FALSE;   /* the OS supplicant authorizes */
    R->AssociationResultParameters.DSInfo                       = WDI_DS_UNKNOWN;
    R->AssociationResultParameters.BandID = (B != NULL && B->Channel > 14) ? WDI_BAND_ID_5000 : WDI_BAND_ID_2400;
    /* WDI_TLV_PHY_TYPE_LIST is not optional in WDI_ASSOCIATION_RESULT_CONTAINER (no _IsPresent bit; 
       :4099-4132) and the WiFiCx sample always sends it (wifihaltestdata.h:226-231); the cx3 run's generator failure
       ("association result generation failed") came from leaving it empty (UNVERIFIED until the next run, the NDIS
       status is logged now). The PHY in use from the association response IEs: VHT capabilities (191) -> VHT, HT
       capabilities (45) -> HT, else OFDM (5 GHz) / ERP (2.4 GHz) (802.11-2016 9.4.2.56 / 9.4.2.158). */
    if (RespIes != NULL && CxFindIe(RespIes, RespIesLen, 191) != NULL) {
        Phy = WDI_PHY_TYPE_VHT;
    } else if (RespIes != NULL && CxFindIe(RespIes, RespIesLen, 45) != NULL) {
        Phy = WDI_PHY_TYPE_HT;
    } else {
        Phy = (R->AssociationResultParameters.BandID == WDI_BAND_ID_5000) ? WDI_PHY_TYPE_OFDM : WDI_PHY_TYPE_ERP;
    }
    R->ActivePhyTypeList.SimpleAssign(&Phy, 1);
    /* WMM negotiated = the AP's response carries the WMM parameter element (vendor IE 00-50-F2 type 2, WMM spec
       2.2.2); the firmware does the WMM itself, this only informs the OS. */
    if (RespIes != NULL) {
        ULONG Q = 0;

        while (Q + 2 <= RespIesLen && Q + 2 + RespIes[Q + 1] <= RespIesLen) {
            if (RespIes[Q] == 0xDD && RespIes[Q + 1] >= 4 && RespIes[Q + 2] == 0x00 && RespIes[Q + 3] == 0x50 &&
                RespIes[Q + 4] == 0xF2 && RespIes[Q + 5] == 0x02) {
                R->AssociationResultParameters.WMMQoSEnabled = TRUE;
                break;
            }
            Q += 2 + RespIes[Q + 1];
        }
    }
    if (A != NULL && St == WDI_ASSOC_STATUS_SUCCESS) {
        CxLogIes(Dev, "req", A->ReqIes, A->ReqIesLen);
        CxLogIes(Dev, "resp", A->RespIes, A->RespIesLen);
        /* frame bodies: assoc request = capability + listen interval + IEs, response = capability + status + AID
           + IEs (802.11-2016 9.3.3.6 / 9.3.3.7; firmware gives the fixed fields in assoc_info, the IEs separately) */
        Req  = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 4 + WLAN_ASSOC_IES_MAX, CX_POOL_TAG);
        Resp = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 6 + WLAN_ASSOC_IES_MAX, CX_POOL_TAG);
        if (Req != NULL) {
            *(USHORT UNALIGNED *)Req       = A->ReqCap;
            *(USHORT UNALIGNED *)(Req + 2) = A->ReqListen;
            RtlCopyMemory(Req + 4, A->ReqIes, A->ReqIesLen);
            R->Optional.AssociationRequestFrame_IsPresent = 1;
            R->AssociationRequestFrame.SimpleAssign(Req, 4 + A->ReqIesLen);
        }
        if (Resp != NULL) {
            *(USHORT UNALIGNED *)Resp       = A->RespCap;
            *(USHORT UNALIGNED *)(Resp + 2) = A->RespStatus;
            *(USHORT UNALIGNED *)(Resp + 4) = A->RespAid;
            RtlCopyMemory(Resp + 6, A->RespIes, A->RespIesLen);
            R->Optional.AssociationResponseFrame_IsPresent = 1;
            R->AssociationResponseFrame.SimpleAssign(Resp, 6 + A->RespIesLen);
        }
        if (B != NULL) {
            Bcn = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 12 + CX_BSS_IES_MAX, CX_POOL_TAG);
            if (Bcn != NULL) {
                RtlZeroMemory(Bcn, 12);
                *(USHORT UNALIGNED *)(Bcn + 8)  = B->BeaconPeriod;
                *(USHORT UNALIGNED *)(Bcn + 10) = B->Capability;
                RtlCopyMemory(Bcn + 12, B->Ies, B->IesLen);
                R->Optional.BeaconProbeResponse_IsPresent = 1;
                R->BeaconProbeResponse.SimpleAssign(Bcn, 12 + B->IesLen);
            }
        }
    }
    P.AssociationResults.SimpleAssign(R, 1);
    Ns = GenerateWdiIndicationAssociationResult(&P, 0, &Dev->Tlv, &OutLen, &Out);
    if (Ns == NDIS_STATUS_SUCCESS) {
        CxLog(Dev, "[join] association result: status %u, phy %u, wmm %u, req %lu B resp %lu B beacon %u\r\n", St, Phy,
              R->AssociationResultParameters.WMMQoSEnabled, R->AssociationRequestFrame.ElementCount,
              R->AssociationResponseFrame.ElementCount, R->Optional.BeaconProbeResponse_IsPresent);
        CxIndicate(Dev, WDI_INDICATION_ASSOCIATION_RESULT, NULL, NDIS_STATUS_SUCCESS, Out, OutLen);
        FreeGenerated(Out);
    } else {
        CxLog(Dev, "[join] association result generation failed: NDIS 0x%08lX (peer WDI 0x%08lX)\r\n", Ns,
              Dev->Tlv.PeerVersion);
    }
    P.AssociationResults.SimpleAssign(NULL, 0);
    R->ActivePhyTypeList.SimpleAssign(NULL, 0);
    R->AssociationRequestFrame.SimpleAssign(NULL, 0);
    R->AssociationResponseFrame.SimpleAssign(NULL, 0);
    R->BeaconProbeResponse.SimpleAssign(NULL, 0);
    delete[] R;
    if (Req != NULL) {
        ExFreePoolWithTag(Req, CX_POOL_TAG);
    }
    if (Resp != NULL) {
        ExFreePoolWithTag(Resp, CX_POOL_TAG);
    }
    if (Bcn != NULL) {
        ExFreePoolWithTag(Bcn, CX_POOL_TAG);
    }
}

static VOID
CxIndicateLinkState(PCX_DEVICE Dev, PCX_BSS B)
{
    WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS P;
    UINT8                                      *Out = NULL;
    ULONG                                       OutLen = 0;

    RtlCopyMemory(P.LinkStateChangeParameters.PeerMACAddress.Address, Dev->Bssid, 6);
    P.LinkStateChangeParameters.TxLinkSpeed = CX_LINK_KBPS;
    P.LinkStateChangeParameters.RxLinkSpeed = CX_LINK_KBPS;
    P.LinkStateChangeParameters.LinkQuality = (UINT8)((B == NULL || B->Rssi <= -100) ? 0 :
                                                      (B->Rssi >= -50) ? 100 : 2 * (B->Rssi + 100));
    if (GenerateWdiIndicationLinkStateChange(&P, 0, &Dev->Tlv, &OutLen, &Out) == NDIS_STATUS_SUCCESS) {
        CxIndicate(Dev, WDI_INDICATION_LINK_STATE_CHANGE, NULL, NDIS_STATUS_SUCCESS, Out, OutLen);
        FreeGenerated(Out);
    }
}

_Use_decl_annotations_
VOID
CxTaskConnect(PCX_DEVICE Dev, PCX_CMD Cmd, const WDI_MESSAGE_HEADER *H, const UINT8 *Tlv, ULONG TlvLen)
{
    WDI_TASK_CONNECT_PARAMETERS *P = new WDI_TASK_CONNECT_PARAMETERS;
    WDI_MESSAGE_HEADER           Copy = *H;
    PWLAN_CONNECT                C = NULL;
    PWLAN_ASSOC_INFO             A = NULL;
    WDI_AUTH_ALGORITHM           Auth = WDI_AUTH_ALGO_80211_OPEN;
    WDI_CIPHER_ALGORITHM         Ucast = WDI_CIPHER_ALGO_NONE, Mcast = WDI_CIPHER_ALGO_NONE;
    ULONG                        I, N, Rc = JOIN_SETUP_FAILED;
    BOOLEAN                      Ok = FALSE;

    if (P == NULL) {
        CxM3(Dev, Cmd, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    if (ParseWdiTaskConnect(TlvLen, Tlv, &Dev->Tlv, P) != NDIS_STATUS_SUCCESS) {
        delete P;
        CxM3(Dev, Cmd, STATUS_INVALID_PARAMETER);
        return;
    }
    if (P->ConnectParameters.AuthenticationAlgorithms.ElementCount != 0) {
        Auth = P->ConnectParameters.AuthenticationAlgorithms.pElements[0];
    }
    if (P->ConnectParameters.UnicastCipherAlgorithms.ElementCount != 0) {
        Ucast = P->ConnectParameters.UnicastCipherAlgorithms.pElements[0];
    }
    if (P->ConnectParameters.MulticastCipherAlgorithms.ElementCount != 0) {
        Mcast = P->ConnectParameters.MulticastCipherAlgorithms.pElements[0];
    }
    N = P->PreferredBSSEntryList.ElementCount;
    CxLog(Dev, "[join] WDI connect: %lu BSS entries, auth %u unicast %u multicast %u, %lu SSIDs\r\n", N, Auth, Ucast,
          Mcast, P->ConnectParameters.SSIDList.ElementCount);
    if (N == 0 || P->ConnectParameters.SSIDList.ElementCount == 0 ||
        P->ConnectParameters.SSIDList.pElements[0].ElementCount == 0 ||
        P->ConnectParameters.SSIDList.pElements[0].ElementCount > 32 ||
        !((Auth == WDI_AUTH_ALGO_80211_OPEN && Ucast == WDI_CIPHER_ALGO_NONE) ||
          (Auth == WDI_AUTH_ALGO_RSNA_PSK && Ucast == WDI_CIPHER_ALGO_CCMP && Mcast == WDI_CIPHER_ALGO_CCMP))) {
        CleanupParsedWdiTaskConnect(P);
        delete P;
        CxM3(Dev, Cmd, STATUS_NOT_SUPPORTED);   /* only open and WPA2-Personal CCMP/CCMP (station capabilities) */
        return;
    }
    C = (PWLAN_CONNECT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*C), CX_POOL_TAG);
    A = (PWLAN_ASSOC_INFO)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*A), CX_POOL_TAG);
    if (C == NULL || A == NULL) {
        CleanupParsedWdiTaskConnect(P);
        delete P;
        if (C != NULL) {
            ExFreePoolWithTag(C, CX_POOL_TAG);
        }
        if (A != NULL) {
            ExFreePoolWithTag(A, CX_POOL_TAG);
        }
        CxM3(Dev, Cmd, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    CxM3(Dev, Cmd, STATUS_SUCCESS);
    if (Dev->Linked) {
        CxDisconnect(Dev, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
    }

    for (I = 0; I < N && I < CX_CONNECT_TRIES && !Ok; I++) {
        WDI_CONNECT_BSS_ENTRY_CONTAINER *E = &P->PreferredBSSEntryList.pElements[I];
        PCX_BSS                          B = CxFindBss(Dev, E->BSSID.Address);
        const UCHAR                     *Ies = NULL, *Rsn;
        ULONG                            IesLen = 0;

        RtlZeroMemory(C, sizeof(*C));
        RtlCopyMemory(C->Bssid, E->BSSID.Address, 6);
        C->SsidLen = (UCHAR)P->ConnectParameters.SSIDList.pElements[0].ElementCount;
        RtlCopyMemory(C->Ssid, P->ConnectParameters.SSIDList.pElements[0].pElements, C->SsidLen);
        C->Rsn = (Auth == WDI_AUTH_ALGO_RSNA_PSK);
        if (B != NULL) {
            C->Chanspec = B->Chanspec;
            Ies    = B->Ies;
            IesLen = B->IesLen;
        } else if (E->Optional.ProbeResponseFrame_IsPresent && E->ProbeResponseFrame.ElementCount > 12) {
            Ies    = E->ProbeResponseFrame.pElements + 12;   /* body: timestamp 8, interval 2, capability 2 */
            IesLen = E->ProbeResponseFrame.ElementCount - 12;
        } else if (E->Optional.BeaconFrame_IsPresent && E->BeaconFrame.ElementCount > 12) {
            Ies    = E->BeaconFrame.pElements + 12;
            IesLen = E->BeaconFrame.ElementCount - 12;
        }
        Rsn = (Ies != NULL) ? CxFindIe(Ies, IesLen, RSN_IE_ID) : NULL;
        if (Rsn != NULL) {
            CxLogIes(Dev, "ap-rsn", Rsn, 2UL + Rsn[1]);   /* AP's RSN capabilities (PMF bits) - notes/wifi-throughput.md */
        }
        if (C->Rsn) {
            if (Rsn == NULL || 2UL + Rsn[1] > RSN_IE_MAX) {
                CxLog(Dev, "[join] entry %lu: no usable RSN IE for %02X:%02X:%02X:%02X:%02X:%02X\r\n", I, C->Bssid[0],
                      C->Bssid[1], C->Bssid[2], C->Bssid[3], C->Bssid[4], C->Bssid[5]);
                CxIndicateAssoc(Dev, C->Bssid, WDI_ASSOC_STATUS_FAILURE, NULL, B, Auth, Ucast, Mcast);
                continue;
            }
            C->ApRsnLen = 2UL + Rsn[1];
            RtlCopyMemory(C->ApRsn, Rsn, C->ApRsnLen);
        }
        RtlCopyMemory(Dev->Bssid, C->Bssid, 6);
        Rc = WlanMsgConnect(&Dev->Msg, Dev->RunR, C, A);
        Ok = (Rc == JOIN_OK);
        CxIndicateAssoc(Dev, C->Bssid, Ok ? WDI_ASSOC_STATUS_SUCCESS :
                        (Rc == JOIN_TIMEOUT) ? WDI_ASSOC_STATUS_NO_ASSOC_RESPONSE : WDI_ASSOC_STATUS_ASSOC_FAILED_BY_PEER,
                        A, B, Auth, Ucast, Mcast);
        if (Ok) {
            CxSetLinked(Dev, TRUE);
            CxIndicateLinkState(Dev, B);
        }
    }
    CxLog(Dev, "[join] WDI connect done: %s (CONNECT %lu)\r\n", Ok ? "associated" : "failed", Rc);
    CxIndicate(Dev, WDI_INDICATION_CONNECT_COMPLETE, &Copy, Ok ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE, NULL, 0);
    CleanupParsedWdiTaskConnect(P);
    delete P;
    ExFreePoolWithTag(C, CX_POOL_TAG);
    ExFreePoolWithTag(A, CX_POOL_TAG);
}

_Use_decl_annotations_
VOID
CxTaskDisconnect(PCX_DEVICE Dev, PCX_CMD Cmd, const WDI_MESSAGE_HEADER *H)
{
    WDI_MESSAGE_HEADER Copy = *H;

    CxM3(Dev, Cmd, STATUS_SUCCESS);
    CxDisconnect(Dev, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
    CxIndicate(Dev, WDI_INDICATION_DISCONNECT_COMPLETE, &Copy, NDIS_STATUS_SUCCESS, NULL, 0);
}

/* WDI_SET_ADD_CIPHER_KEYS: CCMP pairwise / group keys -> "wsec_key". */
_Use_decl_annotations_
VOID
CxSetAddKeys(PCX_DEVICE Dev, PCX_CMD Cmd, const UINT8 *Tlv, ULONG TlvLen)
{
    WDI_SET_ADD_CIPHER_KEYS_PARAMETERS *P = new WDI_SET_ADD_CIPHER_KEYS_PARAMETERS;
    NTSTATUS                            Status = STATUS_SUCCESS;
    ULONG                               I;

    if (P == NULL) {
        CxM3(Dev, Cmd, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    if (ParseWdiSetAddCipherKeys(TlvLen, Tlv, &Dev->Tlv, P) != NDIS_STATUS_SUCCESS) {
        delete P;
        CxM3(Dev, Cmd, STATUS_INVALID_PARAMETER);
        return;
    }
    /* EAPOL 4/4 (queued by the OS just before this) must leave before the pairwise key exists */
    CxTxDrain(Dev);
    (VOID)WlanMsgWaitTxIdle(&Dev->Msg, Dev->RunR, 200);
    for (I = 0; I < P->SetCipherKey.ElementCount && NT_SUCCESS(Status); I++) {
        WDI_SET_ADD_CIPHER_KEYS_CONTAINER *K = &P->SetCipherKey.pElements[I];
        BOOLEAN                            Pairwise = (K->CipherKeyTypeInfo.KeyType == WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY);
        ULONG                              Rc;

        CxLog(Dev, "[key] add: type %u algo %u id %lu%s ccmp %u B\r\n", K->CipherKeyTypeInfo.KeyType,
              K->CipherKeyTypeInfo.CipherAlgorithm, K->Optional.CipherKeyID_IsPresent ? K->CipherKeyID.CipherKeyID : 0,
              K->Optional.ReceiveSequenceCount_IsPresent ? " rsc" : "",
              K->Optional.CCMPKey_IsPresent ? K->CCMPKey.ElementCount : 0);
        if (K->CipherKeyTypeInfo.CipherAlgorithm != WDI_CIPHER_ALGO_CCMP || !K->Optional.CCMPKey_IsPresent ||
            K->CCMPKey.ElementCount != 16 ||
            (K->CipherKeyTypeInfo.KeyType != WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY &&
             K->CipherKeyTypeInfo.KeyType != WDI_CIPHER_KEY_TYPE_GROUP_KEY)) {
            Status = STATUS_NOT_SUPPORTED;
            break;
        }
        if (!Pairwise && !K->Optional.CipherKeyID_IsPresent) {
            Status = STATUS_INVALID_PARAMETER;   /* a group key without its key id cannot be installed (no guess) */
            break;
        }
        /* RSC byte order: assumed = the EAPOL-Key RSC field (lowest PN octet first) that InstallKey expects;
           UNVERIFIED for WDI (WDI_TLV_RECEIVE_SEQUENCE_COUNT is "the initial 48-bit PN"). */
        Rc = WlanMsgSetKey(&Dev->Msg, Dev->RunR, Pairwise ? 0 : K->CipherKeyID.CipherKeyID,
                           K->CCMPKey.pElements,
                           Pairwise ? (K->Optional.PeerMacAddress_IsPresent ? K->PeerMacAddress.Address : Dev->Bssid) : NULL,
                           (!Pairwise && K->Optional.ReceiveSequenceCount_IsPresent) ? K->ReceiveSequenceCount.ReceiveSequenceCount : NULL);
        if (Rc != MSG_OK) {
            Status = STATUS_UNSUCCESSFUL;
        } else if (Pairwise) {
            Dev->KeysSet = TRUE;
            /* the pairwise key = the OS supplicant finished the 4-way handshake: port authorized (dhd does the same,
               WlanMsgScbAuthorize) - lets the firmware set up Block Ack (notes/wifi-throughput.md) */
            (VOID)WlanMsgScbAuthorize(&Dev->Msg, Dev->RunR,
                                      K->Optional.PeerMacAddress_IsPresent ? K->PeerMacAddress.Address : Dev->Bssid);
            /* 1.0.10.2 still showed ini_off / txaddbareq 5 rxaddbaresp 0: reset TID 0's initiator now that the port
               is authorized, so the firmware tries ADDBA again (WlanMsgAmpduTid) */
            (VOID)WlanMsgAmpduTid(&Dev->Msg, Dev->RunR, 0, FALSE);
            (VOID)WlanMsgAmpduTid(&Dev->Msg, Dev->RunR, 0, TRUE);
        }
    }
    CleanupParsedWdiSetAddCipherKeys(P);
    delete P;
    CxM3(Dev, Cmd, Status);
}
