/*  WlanMsg.c - S10e BCM4375 WLAN, milestone 3: msgbuf common rings + the first firmware ioctls (polled).

    Runs as WlanDlRun's fw-up callback while BAR0/BAR2 are mapped and the firmware has published
    pcie_shared. Order = bcmdhd dhd_prot_init (notes/m12-wifi-m3.md "Protocol facts", "DMA decision"):

      1. validate pcie_shared v7 (XORCSUM sync, indices in TCM), read ring_info
      2. one 32-bit common buffer (WdfCommonBuffer, non-cached: _CCA 0), carved per the MSG_OFF_* map
      3. host_cap, the five common rings' ring_mem (base / max_item / len_items), indices zeroed, scratch buffer
      4. EP bus mastering on, hostready doorbell (DB1)
      5. 8 ioctl-response buffers posted on the H2D control ring (DB0)
      6. GET_VAR "cur_etheraddr" and "ver" (IOCTLPTR_REQ), completions polled on the D2H control-complete ring
         with the XOR checksum + epoch check
      7. always: ARM halted, 1 ms, EP bus mastering off, then the common buffer is freed (no DMA into freed memory)

    Every step is a report line; result codes MSG_* (Exynos9820Wlan.h).
*/

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <bcrypt.h>
#include "WlanDl.h"
#include "WlanMsg.h"
#include "WlanSup.h"

typedef struct _MSG_RING {
    ULONG   Id;
    ULONG   ItemSize;
    ULONG   MaxItem;
    ULONG   Off;            /* offset in the common buffer */
    ULONG   WIdxAddr;       /* TCM address of the write index slot */
    ULONG   RIdxAddr;       /* TCM address of the read index slot */
    ULONG   Host;           /* host-side index: write index (H2D) / read index (D2H) */
    ULONG   Seq;            /* epoch sequence */
    PMDL    Mdl;            /* M7a: D2H ring = invalidate before reads; flowring = clean before the doorbell */
    ULONG   LastUnsynced;   /* ring index + 1 of the last "not synced" report (log once per item) */
    ULONG   Unsynced;       /* polls that stopped on an unsynced item */
} MSG_RING, *PMSG_RING;

typedef struct _MSG_STATE {
    PWLAN_REPORT        R;
    PWLAN_BUS           Bus;
    PUCHAR              Va;
    ULONGLONG           La;
    MSG_RING            Ring[RING_COMMON_COUNT];
    ULONG               Db0;
    ULONG               Db1;
    UCHAR               Resp[1024];     /* M7b: 256 -> 1024 for "assoc_req_ies" */
    ULONG               RespLen;
    LONG                RespStatus;
    BOOLEAN             Acked;
    BOOLEAN             Completed;
    /* milestone 4: events + escan */
    ULONG               RxOffset;       /* pcie_shared dma_rxoffset, applied to event buffers (dhd) */
    ULONG               Events;
    ULONG               EventLines;
    ULONG               ScanEvents;
    BOOLEAN             ScanDone;
    ULONG               ScanStatus;
    ULONG               BssCount;
    UCHAR               Bss[SCAN_MAX_BSS][6];
    UCHAR               VarBuf[IOCTL_REQ_MAX_INPUT];   /* iovar name + value (clmload chunks) */
    BOOLEAN             ScanV2;         /* escan params version chosen at milestone 4 */
    /* milestone 6 join tracking (EventPacket) */
    BOOLEAN             Joining;
    LONG                JoinSetSsid;    /* SET_SSID status, -1 = none yet */
    LONG                JoinPskSup;     /* PSK_SUP status, -1 = none yet */
    BOOLEAN             JoinLinkUp;
    BOOLEAN             JoinLost;       /* deauth / disassoc / link down while joining */
    /* Cache maintenance (Parameters\CacheMaint, run m5-2): the WDF common buffer is CPU-cached and device
       writes do not update CPU caches. Clean = KeFlushIoBuffers(Mdl, FALSE, TRUE) for regions the CPU writes
       and the device reads; invalidate = KeFlushIoBuffers(Mdl, TRUE, TRUE) for regions only the device writes
       (the CPU never dirties them after the initial clean, so nothing CPU-written can be dropped). */
    PMDL                MdlAll;         /* whole buffer: one clean after zeroing */
    PMDL                MdlH2d;         /* H2D ctrl + rxpost rings (clean before a doorbell) */
    PMDL                MdlReq;         /* ioctl request buffer (clean before a doorbell) */
    PMDL                MdlBufs;        /* ioctl response + event buffers (invalidate before reading) */
    /* (M7a: the D2H completion rings each carry their own MDL in MSG_RING.Mdl instead of one MdlD2h) */
    /* ---- milestone 7a data path (notes/m12-wifi-m7.md); valid when DataOn ---- */
    BOOLEAN             DataOn;         /* second common buffer mapped, rx buffers posted */
    PUCHAR              DVa;            /* data common buffer, DAT_OFF_* map */
    ULONGLONG           DLa;
    ULONG               H2dWBase;       /* ring_info h2d_w/r_idx_ptr: flowring slots continue after ring 1 */
    ULONG               H2dRBase;
    ULONG               MaxFlowrings;   /* ring_info max_tx_flowrings */
    UCHAR               Mac[6];         /* cur_etheraddr (flowring SA) */
    MSG_RING            Flow;           /* the one tx flowring (flowid FLOWRING_FIRST_ID), when FlowOpen */
    BOOLEAN             FlowRequested;
    BOOLEAN             FlowOpen;
    LONG                FlowStatus;     /* FLOW_RING_CREATE_CMPLT status, -1 = none yet */
    UCHAR               FlowDa[6];
    BOOLEAN             TxBusy[TX_BUF_COUNT];
    ULONG               TxNext;
    ULONG               RxCount;        /* rx buffers in use (posted once at start, reposted on completion) */
    PMDL                MdlData;        /* whole data buffer: one clean after zeroing */
    PMDL                MdlTx[TX_BUF_COUNT];
    PMDL                MdlRx[RX_BUF_COUNT];
    ULONG               TxPosted, TxDone, TxErrors, RxFrames, RxEapol, RxBad, RxLost, TxLogged, RxLogged;
    WLAN_RX_FN         *RxFn;
    PVOID               RxCtx;
    /* EAPOL frame for the handshake (M7b): the latest one, copied out of its rx buffer */
    UCHAR               Eapol[512];
    ULONG               EapolLen;
    BOOLEAN             EapolPending;
    ULONG               TxLastIndex;    /* tx buffer of the last TxFrame (M7b waits for msg 4's completion) */
    /* ---- milestone 7b host supplicant (WPA2-PSK, CCMP) ---- */
    UCHAR               Pmk[PMK_LEN];
    UCHAR               TargetSsid[32];
    ULONG               TargetSsidLen;  /* != 0: EscanResult records the target's BSSID + RSN IE */
    UCHAR               ApBssid[6];
    LONG                ApRssi;
    UCHAR               ApRsn[RSN_IE_MAX];
    ULONG               ApRsnLen;
    UCHAR               OwnRsn[RSN_IE_MAX];   /* the RSN IE of our assoc request = msg 2 key data */
    ULONG               OwnRsnLen;
    BOOLEAN             AssocIesRead;
    UCHAR               Aa[6];          /* authenticator address = source of msg 1 */
    UCHAR               ANonce[SUP_NONCE_LEN];
    UCHAR               SNonce[SUP_NONCE_LEN];
    UCHAR               Ptk[SUP_PTK_LEN];
    BOOLEAN             PtkSet;
    UCHAR               Replay[8];      /* last replay counter of a MIC-verified frame */
    BOOLEAN             ReplaySet;
    ULONG               Hs;             /* HS_* */
    ULONG               GtkIdx;
    ULONG               Rekeys;
    BOOLEAN             Authorized;     /* keys installed and the link not lost since */
} MSG_STATE, *PMSG_STATE;

static VOID SupEapol(_Inout_ PMSG_STATE S);   /* M7b host supplicant, defined with WlanMsgJoin */

static ULONG
Tcm32(_In_ PMSG_STATE S, _In_ ULONG Addr)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(S->Bus->Tcm + Addr));
}

static VOID
TcmW32(_In_ PMSG_STATE S, _In_ ULONG Addr, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(S->Bus->Tcm + Addr), Val);
}

static USHORT
Tcm16(_In_ PMSG_STATE S, _In_ ULONG Addr)
{
    return READ_REGISTER_USHORT((volatile USHORT *)(S->Bus->Tcm + Addr));
}

static VOID
TcmW16(_In_ PMSG_STATE S, _In_ ULONG Addr, _In_ USHORT Val)
{
    WRITE_REGISTER_USHORT((volatile USHORT *)(S->Bus->Tcm + Addr), Val);
}

static BOOLEAN
InRam(_In_ PMSG_STATE S, _In_ ULONG Addr, _In_ ULONG Len)
{
    return Addr >= S->Bus->RamBase && Addr + Len <= S->Bus->RamEnd && Addr + Len > Addr;
}

static VOID
Doorbell(_In_ PMSG_STATE S, _In_ ULONG Reg)
{
    KeMemoryBarrier();
    WlanBusBpWr(S->Bus, S->Bus->PcieCoreBase + Reg, PCIE2_DOORBELL_VALUE);
}

static VOID
CacheClean(_In_opt_ PMDL Mdl)
{
    if (Mdl != NULL) {
        KeFlushIoBuffers(Mdl, FALSE, TRUE);
    }
}

static VOID
CacheInvalidate(_In_opt_ PMDL Mdl)
{
    if (Mdl != NULL) {
        KeFlushIoBuffers(Mdl, TRUE, TRUE);
    }
}

static PMDL
RegionMdl(_In_ PMSG_STATE S, _In_ ULONG Off, _In_ ULONG Len)
{
    PMDL Mdl = IoAllocateMdl(S->Va + Off, Len, FALSE, FALSE, NULL);

    if (Mdl != NULL) {
        MmBuildMdlForNonPagedPool(Mdl);
    }
    return Mdl;
}

static VOID
MdlFree(_Inout_ PMDL *Mdl)
{
    if (*Mdl != NULL) {
        IoFreeMdl(*Mdl);
        *Mdl = NULL;
    }
}

static VOID
RegionMdlsFree(_Inout_ PMSG_STATE S)
{
    PMDL *All[] = { &S->MdlAll, &S->MdlH2d, &S->MdlReq, &S->MdlBufs, &S->MdlData, &S->Flow.Mdl };
    ULONG I;

    for (I = 0; I < ARRAYSIZE(All); I++) {
        MdlFree(All[I]);
    }
    for (I = 0; I < RING_COMMON_COUNT; I++) {
        MdlFree(&S->Ring[I].Mdl);
    }
    for (I = 0; I < TX_BUF_COUNT; I++) {
        MdlFree(&S->MdlTx[I]);
    }
    for (I = 0; I < RX_BUF_COUNT; I++) {
        MdlFree(&S->MdlRx[I]);
    }
}

/* MDL over Len bytes of the data common buffer (RegionMdl is over the control buffer). */
static PMDL
DataMdl(_In_ PMSG_STATE S, _In_ ULONG Off, _In_ ULONG Len)
{
    PMDL Mdl = IoAllocateMdl(S->DVa + Off, Len, FALSE, FALSE, NULL);

    if (Mdl != NULL) {
        MmBuildMdlForNonPagedPool(Mdl);
    }
    return Mdl;
}

static PUCHAR
RingItem(_In_ PMSG_STATE S, _In_ PMSG_RING G, _In_ ULONG Index)
{
    return S->Va + G->Off + Index * G->ItemSize;
}

/* Fill the next item of an H2D ring and advance the host write index (written to TCM by the commit).
   Base = the buffer the ring lives in (control buffer, or the data buffer for the flowring). NULL when the
   ring stays full for MaxWaitMs (the firmware has not consumed what was posted). */
static PUCHAR
H2dNext(_Inout_ PMSG_STATE S, _Inout_ PMSG_RING G, _In_ PUCHAR Base, _In_ UCHAR Type, _In_ ULONG RequestId,
        _In_ ULONG MaxWaitMs)
{
    PUCHAR It;
    ULONG  Tries;

    for (Tries = 0; ((G->Host + 1) % G->MaxItem) == Tcm16(S, G->RIdxAddr); Tries++) {
        if (Tries >= MaxWaitMs) {
            ReportLine(S->R, "[msg] H2D ring %lu full (w %lu r %u)\r\n", G->Id, G->Host, Tcm16(S, G->RIdxAddr));
            return NULL;
        }
        KeStallExecutionProcessor(1000);
    }
    It = Base + G->Off + G->Host * G->ItemSize;
    RtlZeroMemory(It, G->ItemSize);
    It[MSGH_TYPE]  = Type;
    It[MSGH_IFID]  = 0;
    It[MSGH_FLAGS] = 0;   /* no phase bit, as fmac brcmf_msgbuf_tx_ioctl: wrapping needs only the full check */
    It[MSGH_EPOCH] = (UCHAR)(G->Seq % EPOCH_MODULO);   /* dhd :9329 / txpost the same */
    G->Seq++;
    *(ULONG UNALIGNED *)(It + MSGH_REQUEST_ID) = RequestId;
    G->Host = (G->Host + 1) % G->MaxItem;
    return It;
}

/* Fill the next H2D control item (40 B). */
static PUCHAR
H2dCtrlNext(_Inout_ PMSG_STATE S, _In_ UCHAR Type, _In_ ULONG RequestId)
{
    return H2dNext(S, &S->Ring[RING_H2D_CTRL], S->Va, Type, RequestId, 100);
}

static VOID
H2dCtrlCommit(_Inout_ PMSG_STATE S)
{
    PMSG_RING G = &S->Ring[RING_H2D_CTRL];

    CacheClean(S->MdlH2d);
    CacheClean(S->MdlReq);
    KeMemoryBarrier();
    TcmW16(S, G->WIdxAddr, (USHORT)G->Host);
    Doorbell(S, S->Db0);
}

/* IOCTLRESP_BUF_POST / EVENT_BUF_POST (same item, bcmmsgbuf.h:333): one host buffer, no commit. */
static BOOLEAN
BufPost(_Inout_ PMSG_STATE S, _In_ UCHAR Type, _In_ ULONG PktId, _In_ ULONG Off, _In_ ULONG Len)
{
    PUCHAR    It = H2dCtrlNext(S, Type, PktId);
    ULONGLONG Ba = S->La + Off;

    if (It == NULL) {
        return FALSE;
    }
    *(USHORT UNALIGNED *)(It + BUFPOST_LEN)     = (USHORT)Len;
    *(ULONG UNALIGNED *)(It + BUFPOST_ADDR)     = (ULONG)Ba;
    *(ULONG UNALIGNED *)(It + BUFPOST_ADDR + 4) = (ULONG)(Ba >> 32);
    return TRUE;
}

static ULONG
Be32(_In_reads_(4) const UCHAR *P)
{
    return ((ULONG)P[0] << 24) | ((ULONG)P[1] << 16) | ((ULONG)P[2] << 8) | P[3];
}

/* One escan result payload (brcmf_escan_result_le, fwil_types.h:470): log each BSS once (BSSID dedupe). */
static VOID
EscanResult(_Inout_ PMSG_STATE S, _In_reads_bytes_(Len) const UCHAR *D, _In_ ULONG Len)
{
    ULONG Count, K, Off = ESCAN_RES_BSS_INFO, J;

    if (Len < ESCAN_RES_BSS_INFO) {
        return;
    }
    Count = *(const USHORT UNALIGNED *)(D + ESCAN_RES_BSS_COUNT);
    for (K = 0; K < Count && Off + BSS_INFO_MIN_LEN <= Len; K++) {
        const UCHAR *B    = D + Off;
        ULONG        BLen = *(const ULONG UNALIGNED *)(B + BSS_LENGTH);
        ULONG        SsidLen = B[BSS_SSID_LEN];
        CHAR         Ssid[33];

        if (BLen < BSS_INFO_MIN_LEN || BLen > Len - Off) {
            ReportLine(S->R, "[scan] bss_info length %lu at %lu of %lu - stop parsing this event\r\n", BLen, Off, Len);
            return;
        }
        /* M7b: the join target's BSSID + RSN IE (strongest BSS with that SSID) */
        if (S->TargetSsidLen != 0 && SsidLen == S->TargetSsidLen &&
            RtlCompareMemory(B + BSS_SSID, S->TargetSsid, SsidLen) == SsidLen &&
            (S->ApRsnLen == 0 || *(const SHORT UNALIGNED *)(B + BSS_RSSI) > S->ApRssi)) {
            ULONG IeOff = *(const USHORT UNALIGNED *)(B + BSS_IE_OFFSET);
            ULONG IeLen = *(const ULONG UNALIGNED *)(B + BSS_IE_LENGTH);
            ULONG P;

            if (IeOff >= BSS_INFO_MIN_LEN && IeOff <= BLen && IeLen <= BLen - IeOff) {
                for (P = IeOff; P + 2 <= IeOff + IeLen && P + 2 + B[P + 1] <= IeOff + IeLen; P += 2 + B[P + 1]) {
                    if (B[P] == RSN_IE_ID && 2 + B[P + 1] <= RSN_IE_MAX) {
                        RtlCopyMemory(S->ApRsn, B + P, 2 + B[P + 1]);
                        S->ApRsnLen = 2 + B[P + 1];
                        RtlCopyMemory(S->ApBssid, B + BSS_BSSID, 6);
                        S->ApRssi = *(const SHORT UNALIGNED *)(B + BSS_RSSI);
                        break;
                    }
                }
            }
        }
        for (J = 0; J < S->BssCount && RtlCompareMemory(S->Bss[J], B + BSS_BSSID, 6) != 6; J++) {
        }
        if (J == S->BssCount && S->BssCount < SCAN_MAX_BSS) {
            RtlCopyMemory(S->Bss[S->BssCount++], B + BSS_BSSID, 6);
            if (SsidLen > 32) {
                SsidLen = 32;
            }
            for (J = 0; J < SsidLen; J++) {
                Ssid[J] = (B[BSS_SSID + J] >= 0x20 && B[BSS_SSID + J] < 0x7F) ? (CHAR)B[BSS_SSID + J] : '?';
            }
            Ssid[SsidLen] = 0;
            ReportLine(S->R, "[scan] %02X:%02X:%02X:%02X:%02X:%02X ch %3u rssi %4d chanspec 0x%04X \"%s\"\r\n",
                       B[BSS_BSSID], B[BSS_BSSID + 1], B[BSS_BSSID + 2], B[BSS_BSSID + 3], B[BSS_BSSID + 4],
                       B[BSS_BSSID + 5], B[BSS_CTL_CH], *(const SHORT UNALIGNED *)(B + BSS_RSSI),
                       *(const USHORT UNALIGNED *)(B + BSS_CHANSPEC), Ssid);
        }
        Off += BLen;
    }
}

/* A WL_EVENT buffer: bcm event packet (fweh.h:202-248, big-endian header). */
static VOID
EventPacket(_Inout_ PMSG_STATE S, _In_reads_bytes_(Len) const UCHAR *P, _In_ ULONG Len)
{
    ULONG Type, Status, DataLen;

    S->Events++;
    if (Len < EVT_DATA || P[EVT_OUI] != 0x00 || P[EVT_OUI + 1] != 0x10 || P[EVT_OUI + 2] != 0x18 ||
        P[EVT_USR_SUBTYPE] != 0 || P[EVT_USR_SUBTYPE + 1] != BCMILCP_BCM_SUBTYPE_EVENT) {
        ReportLine(S->R, "[evt] not a bcm event (len %lu)\r\n", Len);
        return;
    }
    Type    = Be32(P + EVT_TYPE);
    Status  = Be32(P + EVT_STATUS);
    DataLen = Be32(P + EVT_DATALEN);
    if (DataLen > Len - EVT_DATA) {
        DataLen = Len - EVT_DATA;
    }
    if ((S->Joining || S->Authorized) &&
        (Type == WLC_E_SET_SSID || Type == WLC_E_AUTH || Type == WLC_E_ASSOC || Type == WLC_E_LINK ||
         Type == WLC_E_PSK_SUP || Type == WLC_E_DEAUTH_IND || Type == WLC_E_DISASSOC_IND)) {
        USHORT  Flags = (USHORT)((P[EVT_FLAGS] << 8) | P[EVT_FLAGS + 1]);
        BOOLEAN Lost  = (Type == WLC_E_DEAUTH_IND || Type == WLC_E_DISASSOC_IND ||
                         (Type == WLC_E_LINK && (Flags & WLC_EVENT_MSG_LINK) == 0));

        ReportLine(S->R, "[join] event %lu status %lu reason %lu flags 0x%04X addr %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                   Type, Status, Be32(P + EVT_REASON), Flags, P[EVT_ADDR], P[EVT_ADDR + 1], P[EVT_ADDR + 2],
                   P[EVT_ADDR + 3], P[EVT_ADDR + 4], P[EVT_ADDR + 5]);
        if (Type == WLC_E_SET_SSID) {
            S->JoinSetSsid = (LONG)Status;
        } else if (Type == WLC_E_PSK_SUP) {
            S->JoinPskSup = (LONG)Status;
        } else if (Type == WLC_E_LINK && !Lost) {
            S->JoinLinkUp = TRUE;
        }
        if (Lost) {
            S->JoinLost = TRUE;
            if (S->Authorized) {
                S->Authorized = FALSE;   /* M7b: keys are gone with the association */
                ReportLine(S->R, "[link] LOST after the handshake (event %lu reason %lu)\r\n", Type, Be32(P + EVT_REASON));
            }
        }
        return;
    }
    if (Type == WLC_E_ESCAN_RESULT) {
        S->ScanEvents++;
        EscanResult(S, P + EVT_DATA, DataLen);
        if (Status != WLC_E_STATUS_PARTIAL) {
            S->ScanDone   = TRUE;
            S->ScanStatus = Status;
        }
        return;
    }
    if (S->EventLines++ < 32) {
        ReportLine(S->R, "[evt] type %lu status %lu reason %lu datalen %lu\r\n", Type, Status, Be32(P + EVT_REASON),
                   DataLen);
    }
}

/* dhd dhd_prot_d2h_sync_xorcsum: complete when the epoch matches and the XOR of all words is 0. */
static BOOLEAN
D2hItemReady(_In_ PMSG_STATE S, _In_ PMSG_RING G, _In_ PUCHAR It)
{
    ULONG I, X = 0;

    KeMemoryBarrier();
    if (It[MSGH_EPOCH] != (UCHAR)(G->Seq % EPOCH_MODULO)) {
        return FALSE;
    }
    for (I = 0; I < G->ItemSize / 4; I++) {
        X ^= ((volatile ULONG *)It)[I];
    }
    UNREFERENCED_PARAMETER(S);
    return X == 0;
}

/* ---------------------------------------------------------------- milestone 7a: data path helpers */

/* Write the host write index of an H2D ring to TCM and ring DB0 (dhd ringbell is the same for every H2D
   ring, dhd_pcie.c:7786). Mdl = the region the CPU wrote (cleaned first). */
static VOID
H2dCommit(_Inout_ PMSG_STATE S, _In_ PMSG_RING G, _In_opt_ PMDL Mdl)
{
    CacheClean(Mdl);
    KeMemoryBarrier();
    TcmW16(S, G->WIdxAddr, (USHORT)G->Host);
    Doorbell(S, S->Db0);
}

/* RXBUF_POST (host_rxbuf_post_t, bcmmsgbuf.h:905-918) for data buffer I, no commit. */
static BOOLEAN
RxPost(_Inout_ PMSG_STATE S, _In_ ULONG I)
{
    PUCHAR    It = H2dNext(S, &S->Ring[RING_H2D_RXPOST], S->Va, MSG_TYPE_RXBUF_POST, RX_PKTID0 + I, 10);
    ULONGLONG Ba = S->DLa + DAT_OFF_RXBUF + (ULONGLONG)I * DATA_BUF_SIZE;

    if (It == NULL) {
        return FALSE;
    }
    *(USHORT UNALIGNED *)(It + RXP_META_LEN)      = 0;   /* no metadata (fmac rx_metadata_offset 0) */
    *(USHORT UNALIGNED *)(It + RXP_DATA_LEN)      = DATA_BUF_SIZE;
    *(ULONG UNALIGNED *)(It + RXP_DATA_ADDR)      = (ULONG)Ba;
    *(ULONG UNALIGNED *)(It + RXP_DATA_ADDR + 4)  = (ULONG)(Ba >> 32);
    return TRUE;
}

/* RX_CMPLT (host_rxbuf_cmpl_t, bcmmsgbuf.h:926-959): data at data_offset (or dma_rxoffset when 0, fmac
   msgbuf.c:1199-1203), data_len bytes; EAPOL frames are kept for the handshake, others go to RxFn. The
   buffer is reposted either way (the frame is consumed during this call). */
static VOID
RxComplete(_Inout_ PMSG_STATE S, _In_ PUCHAR It, _Inout_ PBOOLEAN Repost)
{
    ULONG  Pkt   = *(ULONG UNALIGNED *)(It + MSGH_REQUEST_ID);
    ULONG  I     = Pkt - RX_PKTID0;
    ULONG  Len   = *(USHORT UNALIGNED *)(It + RXC_DATA_LEN);
    ULONG  Off   = *(USHORT UNALIGNED *)(It + RXC_DATA_OFFSET);
    ULONG  Flags = *(USHORT UNALIGNED *)(It + RXC_FLAGS);
    LONG   St    = *(SHORT UNALIGNED *)(It + CPL_STATUS);
    PUCHAR F;
    ULONG  Type;

    if (Pkt < RX_PKTID0 || I >= S->RxCount) {
        S->RxBad++;
        ReportLine(S->R, "[rx] completion for unknown pktid 0x%lX (status %ld)\r\n", Pkt, St);
        return;
    }
    if (Off == 0) {
        Off = S->RxOffset;
    }
    if (Off >= DATA_BUF_SIZE || Len > DATA_BUF_SIZE - Off || Len < ETH_HLEN ||
        (Flags & PKT_FLAGS_FRAME_MASK) == PKT_FLAGS_FRAME_802_11 /* monitor frame, fmac :1205 */) {
        S->RxBad++;
        if (S->RxLogged++ < 16) {
            ReportLine(S->R, "[rx] pktid 0x%lX dropped: len %lu offset %lu flags 0x%04lX status %ld\r\n", Pkt, Len, Off,
                       Flags, St);
        }
    } else {
        CacheInvalidate(S->MdlRx[I]);
        F    = S->DVa + DAT_OFF_RXBUF + I * DATA_BUF_SIZE + Off;
        Type = ((ULONG)F[12] << 8) | F[13];
        S->RxFrames++;
        if (S->RxLogged++ < 16) {
            ReportLine(S->R, "[rx] #%lu len %lu off %lu if %u type 0x%04lX %02X:%02X:%02X:%02X:%02X:%02X -> "
                       "%02X:%02X:%02X:%02X:%02X:%02X\r\n", S->RxFrames, Len, Off, It[MSGH_IFID], Type, F[6], F[7],
                       F[8], F[9], F[10], F[11], F[0], F[1], F[2], F[3], F[4], F[5]);
        }
        if (Type == ETH_TYPE_EAPOL) {
            S->RxEapol++;
            if (Len <= sizeof(S->Eapol)) {
                RtlCopyMemory(S->Eapol, F, Len);
                S->EapolLen     = Len;
                S->EapolPending = TRUE;
            } else {
                ReportLine(S->R, "[rx] EAPOL frame of %lu B exceeds %u - dropped\r\n", Len, (ULONG)sizeof(S->Eapol));
            }
        } else if (S->RxFn != NULL) {
            S->RxFn(S->RxCtx, F, Len);
        }
    }
    if (RxPost(S, I)) {
        *Repost = TRUE;
    } else {
        /* cannot happen with <= 128 buffers on a 512-item ring; the buffer stays out of circulation (its
           pktid stays valid, so a late completion for it is still recognised) */
        S->RxLost++;
        ReportLine(S->R, "[rx] repost of buffer %lu failed - %lu rx buffers lost\r\n", I, S->RxLost);
    }
}

/* TX_STATUS (host_txbuf_cmpl_t, bcmmsgbuf.h:1046-1080): the tx buffer is free again. */
static VOID
TxStatus(_Inout_ PMSG_STATE S, _In_ PUCHAR It)
{
    ULONG Pkt = *(ULONG UNALIGNED *)(It + MSGH_REQUEST_ID);
    ULONG I   = Pkt - TX_PKTID0;
    LONG  St  = *(SHORT UNALIGNED *)(It + CPL_STATUS);

    if (Pkt < TX_PKTID0 || I >= TX_BUF_COUNT || !S->TxBusy[I]) {
        ReportLine(S->R, "[tx] status for unknown pktid 0x%lX (status %ld)\r\n", Pkt, St);
        return;
    }
    S->TxBusy[I] = FALSE;
    S->TxDone++;
    if (St != 0) {
        S->TxErrors++;
    }
    if (S->TxLogged < 32 || St != 0) {
        S->TxLogged++;
        ReportLine(S->R, "[tx] done pktid 0x%lX status %ld tx_status 0x%04X flowring %u (%lu done, %lu errors)\r\n",
                   Pkt, St, *(USHORT UNALIGNED *)(It + TXC_TX_STATUS), *(USHORT UNALIGNED *)(It + CPL_RING_ID),
                   S->TxDone, S->TxErrors);
    }
}

/* Drain one D2H completion ring once (dhd dhd_prot_process_msgtype); returns the number of items consumed. */
static ULONG
D2hPoll(_Inout_ PMSG_STATE S, _In_ ULONG Ring)
{
    PMSG_RING G = &S->Ring[Ring];
    ULONG     W, Count = 0, Tries;
    BOOLEAN   Repost = FALSE, RxRepost = FALSE;

    KeMemoryBarrier();
    W = Tcm16(S, G->WIdxAddr);
    if (W >= G->MaxItem) {
        ReportLine(S->R, "[msg] D2H ring %lu write index %lu out of range\r\n", Ring, W);
        return 0;
    }
    while (G->Host != W) {
        PUCHAR It = RingItem(S, G, G->Host);
        UCHAR  Type;

        for (Tries = 0; Tries < 100; Tries++) {
            CacheInvalidate(G->Mdl);
            if (D2hItemReady(S, G, It)) {
                break;
            }
            KeStallExecutionProcessor(10);
        }
        if (Tries == 100) {
            G->Unsynced++;
            if (G->LastUnsynced != G->Host + 1) {
                const ULONG *Wd = (const ULONG *)It;

                G->LastUnsynced = G->Host + 1;
                ReportLine(S->R, "[msg] D2H ring %lu item %lu not synced (epoch %u want %lu; fw w %u r %u; cache maint %u): "
                           "%08lX %08lX %08lX %08lX %08lX %08lX\r\n", Ring, G->Host, It[MSGH_EPOCH], G->Seq % EPOCH_MODULO,
                           Tcm16(S, G->WIdxAddr), Tcm16(S, G->RIdxAddr), G->Mdl != NULL, Wd[0], Wd[1], Wd[2],
                           Wd[3], Wd[4], Wd[5]);
            }
            break;
        }
        if (G->LastUnsynced == G->Host + 1) {
            ReportLine(S->R, "[msg] D2H ring %lu item %lu synced after %lu stalled polls\r\n", Ring, G->Host, G->Unsynced);
            G->LastUnsynced = 0;
        }
        Type = It[MSGH_TYPE];
        switch (Type) {
        case MSG_TYPE_IOCTLPTR_REQ_ACK:
            ReportLine(S->R, "[msg] IOCTL ACK status %d cmd %lu\r\n", *(SHORT UNALIGNED *)(It + CPL_STATUS),
                       *(ULONG UNALIGNED *)(It + CPL_ACK_CMD));
            S->Acked = TRUE;
            break;
        case MSG_TYPE_IOCTL_CMPLT: {
            ULONG  Pkt = *(ULONG UNALIGNED *)(It + MSGH_REQUEST_ID);
            ULONG  Len = *(USHORT UNALIGNED *)(It + CPL_IOCTL_RESP_LEN);
            LONG   St  = *(SHORT UNALIGNED *)(It + CPL_STATUS);

            ReportLine(S->R, "[msg] IOCTL CMPLT status %ld resp_len %lu trans %u cmd %lu pktid 0x%lX\r\n", St, Len,
                       *(USHORT UNALIGNED *)(It + CPL_IOCTL_TRANS_ID), *(ULONG UNALIGNED *)(It + CPL_IOCTL_CMD), Pkt);
            S->RespStatus = St;
            S->RespLen    = 0;
            if (Pkt >= IOCTL_RESP_PKTID0 && Pkt < IOCTL_RESP_PKTID0 + IOCTL_RESP_BUF_COUNT) {
                ULONG Copy = (Len < sizeof(S->Resp)) ? Len : sizeof(S->Resp);
                ULONG Off  = MSG_OFF_IOCTL_RESP + (Pkt - IOCTL_RESP_PKTID0) * IOCTL_RESP_BUF_SIZE;

                CacheInvalidate(S->MdlBufs);
                RtlCopyMemory(S->Resp, S->Va + Off, Copy);
                S->RespLen = Copy;
                Repost |= BufPost(S, MSG_TYPE_IOCTLRESP_BUF_POST, Pkt, Off, IOCTL_RESP_BUF_SIZE);   /* fmac reposts too */
            }
            S->Completed = TRUE;
            break;
        }
        case MSG_TYPE_WL_EVENT: {
            ULONG Pkt = *(ULONG UNALIGNED *)(It + MSGH_REQUEST_ID);
            ULONG Len = *(USHORT UNALIGNED *)(It + EVTCPL_DATA_LEN);

            if (Pkt >= EVENT_BUF_PKTID0 && Pkt < EVENT_BUF_PKTID0 + EVENT_BUF_COUNT) {
                ULONG Off = MSG_OFF_EVENT_BUF + (Pkt - EVENT_BUF_PKTID0) * EVENT_BUF_SIZE;

                if (S->RxOffset < EVENT_BUF_SIZE && Len <= EVENT_BUF_SIZE - S->RxOffset) {
                    CacheInvalidate(S->MdlBufs);
                    EventPacket(S, S->Va + Off + S->RxOffset, Len);
                } else {
                    ReportLine(S->R, "[evt] length %lu + rxoffset %lu exceeds the buffer\r\n", Len, S->RxOffset);
                }
                Repost |= BufPost(S, MSG_TYPE_EVENT_BUF_POST, Pkt, Off, EVENT_BUF_SIZE);
            } else {
                ReportLine(S->R, "[evt] unknown buffer pktid 0x%lX\r\n", Pkt);
            }
            break;
        }
        case MSG_TYPE_FLOW_RING_CREATE_CMPLT: {   /* ring_create_response_t bcmmsgbuf.h:758-764 */
            LONG  St = *(SHORT UNALIGNED *)(It + CPL_STATUS);
            ULONG Id = *(USHORT UNALIGNED *)(It + CPL_RING_ID);

            ReportLine(S->R, "[flow] FLOW_RING_CREATE_CMPLT status %ld flowid %lu\r\n", St, Id);
            if (Id == FLOWRING_FIRST_ID) {
                S->FlowStatus = St;
                S->FlowOpen   = (St == 0);
            }
            break;
        }
        case MSG_TYPE_TX_STATUS:
            TxStatus(S, It);
            break;
        case MSG_TYPE_RX_CMPLT:
            RxComplete(S, It, &RxRepost);
            break;
        case MSG_TYPE_GEN_STATUS:
        case MSG_TYPE_RING_STATUS:
            ReportLine(S->R, "[msg] %s status %d ring %u\r\n", (Type == MSG_TYPE_GEN_STATUS) ? "GEN" : "RING",
                       *(SHORT UNALIGNED *)(It + CPL_STATUS), *(USHORT UNALIGNED *)(It + CPL_RING_ID));
            break;
        default:
            ReportLine(S->R, "[msg] D2H ring %lu item type 0x%02X\r\n", Ring, Type);
            break;
        }
        G->Seq++;
        G->Host = (G->Host + 1) % G->MaxItem;
        Count++;
    }
    if (Count != 0) {
        KeMemoryBarrier();
        TcmW16(S, G->RIdxAddr, (USHORT)G->Host);
    }
    if (Repost) {
        H2dCtrlCommit(S);
    }
    if (RxRepost) {
        H2dCommit(S, &S->Ring[RING_H2D_RXPOST], S->MdlH2d);
    }
    return Count;
}

/* The control-complete ring only (ioctl waits, escan, M3/M4 bring-up). */
static ULONG
D2hCtrlPoll(_Inout_ PMSG_STATE S)
{
    return D2hPoll(S, RING_D2H_CTRL_CPL);
}

/* All three completion rings (fmac brcmf_proto_msgbuf_rx_trigger order: rx, tx, ctrl). The data rings are
   only drained once the data path is on (before that nothing is posted that could complete there). */
static ULONG
D2hPollAll(_Inout_ PMSG_STATE S)
{
    ULONG N = 0;

    if (S->DataOn) {
        N += D2hPoll(S, RING_D2H_RX_CPL);
        N += D2hPoll(S, RING_D2H_TX_CPL);
    }
    return N + D2hPoll(S, RING_D2H_CTRL_CPL);
}

/* One IOCTLPTR_REQ (fmac brcmf_msgbuf_tx_ioctl: get and set differ only by Cmd): In[InLen] is copied to the
   request buffer, which also receives up to OutLen bytes; waits for IOCTL_CMPLT (polled). Trans id counts up. */
static ULONG
Ioctl(_Inout_ PMSG_STATE S, _In_ ULONG Cmd, _In_reads_bytes_(InLen) const VOID *In, _In_ ULONG InLen,
      _In_ ULONG OutLen, _In_z_ PCSTR What)
{
    static USHORT TransId;
    PUCHAR        Req = S->Va + MSG_OFF_IOCTL_REQ;
    PUCHAR        It;
    ULONG         BufLen, Ms;
    LARGE_INTEGER Delay;

    if (InLen > IOCTL_REQ_MAX_INPUT || OutLen > IOCTL_REQ_MAX_INPUT) {
        return MSG_IOCTL_ERROR;
    }
    BufLen = (InLen > OutLen) ? InLen : OutLen;   /* the buffer carries the input in and the value out */
    RtlZeroMemory(Req, IOCTL_REQ_BUF_SIZE);
    RtlCopyMemory(Req, In, InLen);

    S->Acked = S->Completed = FALSE;
    S->RespLen = 0;
    It = H2dCtrlNext(S, MSG_TYPE_IOCTLPTR_REQ, IOCTL_REQ_PKTID);
    if (It == NULL) {
        return MSG_TIMEOUT;
    }
    TransId++;
    *(ULONG UNALIGNED *)(It + IOREQ_CMD)         = Cmd;
    *(USHORT UNALIGNED *)(It + IOREQ_TRANS_ID)   = TransId;
    *(USHORT UNALIGNED *)(It + IOREQ_INPUT_LEN)  = (USHORT)BufLen;
    *(USHORT UNALIGNED *)(It + IOREQ_OUTPUT_LEN) = (USHORT)BufLen;
    *(ULONG UNALIGNED *)(It + IOREQ_BUF_ADDR)     = (ULONG)(S->La + MSG_OFF_IOCTL_REQ);
    *(ULONG UNALIGNED *)(It + IOREQ_BUF_ADDR + 4) = (ULONG)((S->La + MSG_OFF_IOCTL_REQ) >> 32);
    H2dCtrlCommit(S);
    ReportLine(S->R, "[msg] ioctl %lu %s sent (len %lu trans %u), H2D ctrl w %lu r %u\r\n", Cmd, What, BufLen,
               TransId, S->Ring[RING_H2D_CTRL].Host, Tcm16(S, S->Ring[RING_H2D_CTRL].RIdxAddr));

    for (Ms = 0; Ms < IOCTL_RESP_TIMEOUT_MS && !S->Completed; Ms += 5) {
        (VOID)D2hCtrlPoll(S);
        if (S->Completed) {
            break;
        }
        Delay.QuadPart = -10000LL * 5;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    if (!S->Completed) {
        ReportLine(S->R, "[msg] ioctl %lu %s: no completion in %u ms (acked %u; H2D ctrl r %u, D2H ctrl w %u)\r\n",
                   Cmd, What, IOCTL_RESP_TIMEOUT_MS, S->Acked, Tcm16(S, S->Ring[RING_H2D_CTRL].RIdxAddr),
                   Tcm16(S, S->Ring[RING_D2H_CTRL_CPL].WIdxAddr));
        return MSG_TIMEOUT;
    }
    return (S->RespStatus == 0) ? MSG_OK : MSG_IOCTL_ERROR;
}

/* GET_VAR / SET_VAR <Name>: buffer = name + NUL [+ Val]. */
static ULONG
IoctlVar(_Inout_ PMSG_STATE S, _In_ BOOLEAN Set, _In_z_ PCSTR Name, _In_reads_bytes_opt_(ValLen) const VOID *Val,
         _In_ ULONG ValLen, _In_ ULONG OutLen)
{
    PUCHAR Buf = S->VarBuf;
    size_t NameLen;

    if (!NT_SUCCESS(RtlStringCbLengthA(Name, 64, &NameLen)) || NameLen + 1 + ValLen > sizeof(S->VarBuf)) {
        return MSG_IOCTL_ERROR;
    }
    RtlCopyMemory(Buf, Name, NameLen + 1);
    if (ValLen != 0) {
        RtlMoveMemory(Buf + NameLen + 1, Val, ValLen);
    }
    return Ioctl(S, Set ? WLC_SET_VAR : WLC_GET_VAR, Buf, (ULONG)NameLen + 1 + ValLen, OutLen, Name);
}

/* CLM (regulatory) blob download, as fmac common.c:102-198 brcmf_c_download_blob ("clmload", 1400 B chunks with
   a dload header, DL_BEGIN on the first, DL_END on the last) and dhd preinit dhd_apply_default_clm
   (dhd_linux.c:10199, before UP). Without it this fw refuses escan with BCME_EPERM (run m4-4). */
static ULONG
ClmLoad(_Inout_ PMSG_STATE S)
{
    PUCHAR   Blob = NULL;
    ULONG    Len = 0, Done = 0, Chunk, Rc = MSG_OK;
    USHORT   Flag = DL_BEGIN;
    UCHAR    Hdr[DLOAD_HDR_SIZE + MAX_CHUNK_LEN];
    NTSTATUS Status;

    Status = WlanLoadBlob(CLM_BLOB_NAME, &Blob, &Len, CLM_BLOB_MAX_SIZE);
    if (!NT_SUCCESS(Status)) {
        ReportLine(S->R, "[clm] %ws not loaded (0x%08lX) - firmware keeps its built-in limits\r\n", CLM_BLOB_NAME, Status);
        return MSG_IOCTL_ERROR;
    }
    while (Done < Len) {
        Chunk = Len - Done;
        if (Chunk > MAX_CHUNK_LEN) {
            Chunk = MAX_CHUNK_LEN;
        } else {
            Flag |= DL_END;
        }
        *(USHORT UNALIGNED *)(Hdr + DLOAD_FLAG) = (USHORT)(Flag | (DLOAD_HANDLER_VER << DLOAD_FLAG_VER_SHIFT));
        *(USHORT UNALIGNED *)(Hdr + DLOAD_TYPE) = DL_TYPE_CLM;
        *(ULONG UNALIGNED *)(Hdr + DLOAD_LEN)   = Chunk;
        *(ULONG UNALIGNED *)(Hdr + DLOAD_CRC)   = 0;
        RtlCopyMemory(Hdr + DLOAD_HDR_SIZE, Blob + Done, Chunk);
        Rc = IoctlVar(S, TRUE, "clmload", Hdr, DLOAD_HDR_SIZE + Chunk, 0);
        if (Rc != MSG_OK) {
            ULONG St = 0xFFFFFFFF;

            if (IoctlVar(S, FALSE, "clmload_status", NULL, 0, sizeof(ULONG)) == MSG_OK && S->RespLen >= 4) {
                St = *(ULONG UNALIGNED *)S->Resp;
            }
            ReportLine(S->R, "[clm] clmload chunk at %lu failed (status %ld), clmload_status %ld\r\n", Done, S->RespStatus,
                       (LONG)St);
            break;
        }
        Flag &= ~DL_BEGIN;
        Done += Chunk;
    }
    ReportLine(S->R, "[clm] %ws: %lu of %lu B loaded in %lu B chunks -> %s\r\n", CLM_BLOB_NAME, Done, Len, MAX_CHUNK_LEN,
               Rc == MSG_OK ? "OK" : "FAILED");
    ExFreePoolWithTag(Blob, 'lDlW');
    if (Rc == MSG_OK && IoctlVar(S, FALSE, "clmver", NULL, 0, 200) == MSG_OK) {
        ULONG I;

        S->Resp[(S->RespLen < sizeof(S->Resp)) ? S->RespLen : sizeof(S->Resp) - 1] = 0;
        for (I = 0; I < S->RespLen && S->Resp[I] != 0; I++) {
            if (S->Resp[I] < 0x20 || S->Resp[I] >= 0x7F) {
                S->Resp[I] = ' ';
            }
        }
        ReportLine(S->R, "[clm] clmver = %s\r\n", (PCSTR)S->Resp);
    }
    return Rc;
}

/* escan request (fmac cfg80211.c brcmf_run_escan / brcmf_escan_prep): all channels, broadcast SSID, active,
   default dwell times. Returns the length. */
static ULONG
EscanBuild(_Out_writes_bytes_(ESCAN_PARAMS_V2_SIZE) PUCHAR Esc, _In_ BOOLEAN V2)
{
    PUCHAR P = Esc + ESCAN_PARAMS;

    RtlZeroMemory(Esc, ESCAN_PARAMS_V2_SIZE);
    *(ULONG UNALIGNED *)(Esc + ESCAN_VERSION)  = V2 ? ESCAN_REQ_VERSION_V2 : ESCAN_REQ_VERSION;
    *(USHORT UNALIGNED *)(Esc + ESCAN_ACTION)  = WL_SCAN_ACTION_START;
    *(USHORT UNALIGNED *)(Esc + ESCAN_SYNC_ID) = 0x1234;
    if (V2) {
        *(USHORT UNALIGNED *)(P + SCANP2_VERSION) = SCAN_PARAMS_VERSION_V2;
        *(USHORT UNALIGNED *)(P + SCANP2_LENGTH)  = SCANP2_FIXED_SIZE;
        RtlFillMemory(P + SCANP2_BSSID, 6, 0xFF);
        P[SCANP2_BSS_TYPE] = DOT11_BSSTYPE_ANY;
        *(ULONG UNALIGNED *)(P + SCANP2_SCAN_TYPE)   = SCANTYPE_ACTIVE;
        *(LONG UNALIGNED *)(P + SCANP2_NPROBES)      = -1;
        *(LONG UNALIGNED *)(P + SCANP2_ACTIVE_TIME)  = -1;
        *(LONG UNALIGNED *)(P + SCANP2_PASSIVE_TIME) = -1;
        *(LONG UNALIGNED *)(P + SCANP2_HOME_TIME)    = -1;
        return ESCAN_PARAMS_V2_SIZE;
    }
    RtlFillMemory(P + SCANP_BSSID, 6, 0xFF);
    P[SCANP_BSS_TYPE]  = DOT11_BSSTYPE_ANY;
    P[SCANP_SCAN_TYPE] = SCANTYPE_ACTIVE;
    *(LONG UNALIGNED *)(P + SCANP_NPROBES)      = -1;
    *(LONG UNALIGNED *)(P + SCANP_ACTIVE_TIME)  = -1;
    *(LONG UNALIGNED *)(P + SCANP_PASSIVE_TIME) = -1;
    *(LONG UNALIGNED *)(P + SCANP_HOME_TIME)    = -1;
    return ESCAN_PARAMS_V1_SIZE;
}

/* One escan with the version chosen at milestone 4 (S->ScanV2), results logged as they arrive; SCAN_*. dhd
   wl_cfgscan.c:1327-1333 treats BCME_EPERM as "not permitted at this point of time": retried every 200 ms up to
   ESCAN_EPERM_RETRIES, draining events meanwhile. */
static ULONG
EscanRun(_Inout_ PMSG_STATE S)
{
    UCHAR         Esc[ESCAN_PARAMS_V2_SIZE];
    ULONG         EscLen, Try, Ms, Result;
    LARGE_INTEGER Delay;

    S->ScanDone   = FALSE;
    S->ScanStatus = 0;
    S->ScanEvents = 0;
    S->BssCount   = 0;
    EscLen = EscanBuild(Esc, S->ScanV2);
    for (Try = 0;; Try++) {
        if (IoctlVar(S, TRUE, "escan", Esc, EscLen, 0) == MSG_OK) {
            break;
        }
        if (S->RespStatus != BCME_EPERM || Try + 1 >= ESCAN_EPERM_RETRIES) {
            ReportLine(S->R, "[scan] escan refused (status %ld) after %lu attempts\r\n", S->RespStatus, Try + 1);
            return SCAN_FAILED;
        }
        for (Ms = 0; Ms < 200; Ms += 10) {
            (VOID)D2hCtrlPoll(S);
            Delay.QuadPart = -10000LL * 10;
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        }
    }
    ReportLine(S->R, "[scan] escan v%u accepted on attempt %lu\r\n", S->ScanV2 ? 2 : 1, Try + 1);
    for (Ms = 0; Ms < SCAN_TIMEOUT_MS && !S->ScanDone; Ms += 10) {
        (VOID)D2hCtrlPoll(S);
        Delay.QuadPart = -10000LL * 10;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    if (S->ScanDone && S->ScanStatus == 0) {
        Result = SCAN_OK;
    } else if (!S->ScanDone) {
        Result = SCAN_TIMEOUT;
    } else {
        Result = SCAN_FW_STATUS;   /* final escan event with a non-success status */
    }
    ReportLine(S->R, "SCAN %lu: %lu BSS from %lu escan events (%s, status %lu) after %lu ms; %lu events total\r\n",
               Result, S->BssCount, S->ScanEvents, S->ScanDone ? "done" : "not done", S->ScanStatus, Ms, S->Events);
    return Result;
}

/* Milestone 7a: second common buffer (tx flowring + tx/rx packet buffers), MDLs, rx buffers posted on the
   H2D rxpost ring. FALSE = no data path (logged); the control path is unaffected. */
static BOOLEAN
DataInit(_Inout_ PMSG_STATE S, _Inout_ PWLAN_MSG_CTX M)
{
    WDFCOMMONBUFFER Db = NULL;
    NTSTATUS        Status;
    ULONG           I, Max;

    ReportLine(S->R, "---- milestone 7a: msgbuf data path ----\r\n");
    if (S->MaxFlowrings == 0 || !InRam(S, S->H2dWBase, RW_INDEX_SZ * (FLOWRING_FIRST_ID + 1)) ||
        !InRam(S, S->H2dRBase, RW_INDEX_SZ * (FLOWRING_FIRST_ID + 1))) {
        ReportLine(S->R, "DATA 1: no tx flowring available (max flowrings %lu)\r\n", S->MaxFlowrings);
        return FALSE;
    }
    Status = WdfCommonBufferCreate(M->DmaEnabler, DAT_DMA_SIZE, WDF_NO_OBJECT_ATTRIBUTES, &Db);
    if (!NT_SUCCESS(Status)) {
        ReportLine(S->R, "DATA 2: WdfCommonBufferCreate(0x%X) 0x%08lX\r\n", DAT_DMA_SIZE, Status);
        return FALSE;
    }
    S->DVa = (PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(Db);
    S->DLa = (ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(Db).QuadPart;
    if ((S->DLa >> 32) != 0 || (S->DLa & 0xFFF) != 0) {
        ReportLine(S->R, "DATA 2: data buffer 0x%I64X not 4 KB aligned below 4 GB\r\n", S->DLa);
        goto Fail;
    }
    RtlZeroMemory(S->DVa, DAT_DMA_SIZE);
    if (S->MdlAll != NULL) {   /* cache maintenance on (Parameters\CacheMaint): MDLs as for the control buffer */
        S->MdlData  = DataMdl(S, 0, DAT_DMA_SIZE);
        S->Flow.Mdl = DataMdl(S, DAT_OFF_FLOWRING, H2D_TXPOST_MAX_ITEM * H2D_TXPOST_ITEMSIZE);
        for (I = 0; I < TX_BUF_COUNT; I++) {
            S->MdlTx[I] = DataMdl(S, DAT_OFF_TXBUF + I * DATA_BUF_SIZE, DATA_BUF_SIZE);
        }
        for (I = 0; I < RX_BUF_COUNT; I++) {
            S->MdlRx[I] = DataMdl(S, DAT_OFF_RXBUF + I * DATA_BUF_SIZE, DATA_BUF_SIZE);
        }
        if (S->MdlData == NULL || S->Flow.Mdl == NULL) {
            ReportLine(S->R, "DATA 2: data MDLs not allocated\r\n");
            goto Fail;
        }
        for (I = 0; I < TX_BUF_COUNT; I++) {
            if (S->MdlTx[I] == NULL) {
                ReportLine(S->R, "DATA 2: data MDLs not allocated\r\n");
                goto Fail;
            }
        }
        for (I = 0; I < RX_BUF_COUNT; I++) {
            if (S->MdlRx[I] == NULL) {
                ReportLine(S->R, "DATA 2: data MDLs not allocated\r\n");
                goto Fail;
            }
        }
        CacheClean(S->MdlData);   /* zeroed flowring + buffers reach memory before the device reads them */
    }

    /* tx flowring geometry (dhd_prot_flow_ring_create :9338-9341); created after association (FlowRingCreate) */
    S->Flow.Id       = FLOWRING_FIRST_ID;
    S->Flow.ItemSize = H2D_TXPOST_ITEMSIZE;
    S->Flow.MaxItem  = H2D_TXPOST_MAX_ITEM;
    S->Flow.Off      = DAT_OFF_FLOWRING;
    S->Flow.WIdxAddr = S->H2dWBase + RW_INDEX_SZ * FLOWRING_FIRST_ID;
    S->Flow.RIdxAddr = S->H2dRBase + RW_INDEX_SZ * FLOWRING_FIRST_ID;
    S->Flow.Seq      = EPOCH_INIT_VAL;
    S->Flow.Host     = 0;
    S->FlowStatus    = -1;

    /* rx buffers: min(RX_BUF_COUNT, max_host_rxbufs) (fmac pcie.c:1651 max_rxbufpost) */
    Max = Tcm16(S, S->Bus->SharedAddr + SH_MAX_HOST_RXBUFS);
    S->RxCount = (Max != 0 && Max < RX_BUF_COUNT) ? Max : RX_BUF_COUNT;
    for (I = 0; I < S->RxCount; I++) {
        if (!RxPost(S, I)) {
            S->RxCount = I;
            break;
        }
    }
    H2dCommit(S, &S->Ring[RING_H2D_RXPOST], S->MdlH2d);
    M->DataBuffer = Db;
    S->DataOn     = TRUE;
    ReportLine(S->R, "DATA 0: buffer 0x%X B at 0x%I64X; %lu rx buffers posted (max_host_rxbufs %lu), rxpost w %lu fw r %u; "
               "flowring %u slots w 0x%08lX r 0x%08lX (max flowrings %lu)\r\n", DAT_DMA_SIZE, S->DLa, S->RxCount, Max,
               S->Ring[RING_H2D_RXPOST].Host, Tcm16(S, S->Ring[RING_H2D_RXPOST].RIdxAddr), FLOWRING_FIRST_ID,
               S->Flow.WIdxAddr, S->Flow.RIdxAddr, S->MaxFlowrings);
    return TRUE;

Fail:
    MdlFree(&S->MdlData);
    MdlFree(&S->Flow.Mdl);
    for (I = 0; I < TX_BUF_COUNT; I++) {
        MdlFree(&S->MdlTx[I]);
    }
    for (I = 0; I < RX_BUF_COUNT; I++) {
        MdlFree(&S->MdlRx[I]);
    }
    WdfObjectDelete(Db);   /* nothing was posted yet: the device has no address in it */
    S->DVa = NULL;
    S->DLa = 0;
    return FALSE;
}

/* MSG_TYPE_FLOW_RING_CREATE (tx_flowring_create_request_t, dhd_prot_flow_ring_create dhd_msgbuf.c:9289-9392)
   for the one STA flowring: ifidx 0, tid 0, SA = our MAC, DA = the AP. The flowring write index is written
   to TCM first (dhd :9382). Waits for FLOW_RING_CREATE_CMPLT. TRUE = open. */
static BOOLEAN
FlowRingCreate(_Inout_ PMSG_STATE S, _In_reads_(6) const UCHAR *Da)
{
    PUCHAR        It;
    ULONGLONG     Ba = S->DLa + DAT_OFF_FLOWRING;
    ULONG         Ms;
    LARGE_INTEGER Delay;

    if (!S->DataOn) {
        return FALSE;
    }
    if (S->FlowOpen || S->FlowRequested) {
        return S->FlowOpen;   /* one create per device lifetime (no delete path yet) */
    }
    RtlCopyMemory(S->FlowDa, Da, 6);
    TcmW16(S, S->Flow.WIdxAddr, 0);
    It = H2dCtrlNext(S, MSG_TYPE_FLOW_RING_CREATE, 0);   /* request_id 0 as dhd :9326 */
    if (It == NULL) {
        return FALSE;
    }
    RtlCopyMemory(It + FLOWC_DA, Da, 6);
    RtlCopyMemory(It + FLOWC_SA, S->Mac, 6);
    It[FLOWC_TID]      = 0;
    It[FLOWC_IF_FLAGS] = 0;
    *(USHORT UNALIGNED *)(It + FLOWC_RING_ID)      = FLOWRING_FIRST_ID;
    *(USHORT UNALIGNED *)(It + FLOWC_MAX_ITEMS)    = H2D_TXPOST_MAX_ITEM;
    *(USHORT UNALIGNED *)(It + FLOWC_LEN_ITEM)     = H2D_TXPOST_ITEMSIZE;
    *(ULONG UNALIGNED *)(It + FLOWC_RING_PTR)      = (ULONG)Ba;
    *(ULONG UNALIGNED *)(It + FLOWC_RING_PTR + 4)  = (ULONG)(Ba >> 32);
    S->FlowRequested = TRUE;
    S->FlowStatus    = -1;
    H2dCtrlCommit(S);
    ReportLine(S->R, "[flow] FLOW_RING_CREATE flowid %u da %02X:%02X:%02X:%02X:%02X:%02X sa %02X:%02X:%02X:%02X:%02X:%02X "
               "ring 0x%I64X %u x %u\r\n", FLOWRING_FIRST_ID, Da[0], Da[1], Da[2], Da[3], Da[4], Da[5], S->Mac[0], S->Mac[1],
               S->Mac[2], S->Mac[3], S->Mac[4], S->Mac[5], Ba, H2D_TXPOST_MAX_ITEM, H2D_TXPOST_ITEMSIZE);
    for (Ms = 0; Ms < FLOW_CREATE_TIMEOUT_MS && S->FlowStatus < 0; Ms += 10) {
        (VOID)D2hPollAll(S);
        if (S->FlowStatus >= 0) {
            break;
        }
        Delay.QuadPart = -10000LL * 10;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    ReportLine(S->R, "FLOW %lu: flowring %u %s (status %ld) after %lu ms\r\n", S->FlowOpen ? 0UL : 1UL,
               FLOWRING_FIRST_ID, S->FlowOpen ? "open" : "NOT open", S->FlowStatus, Ms);
    return S->FlowOpen;
}

/* One 802.3 frame on the flowring (host_txbuf_post_t, fmac brcmf_msgbuf_txflow msgbuf.c:721-792): the 14 B
   header goes in the item, the payload is copied to a tx buffer, data_len = payload length. */
static ULONG
TxFrame(_Inout_ PMSG_STATE S, _In_reads_bytes_(Len) const UCHAR *Frame, _In_ ULONG Len)
{
    PUCHAR    It;
    ULONG     I = 0, J;
    ULONGLONG Ba;

    if (!S->FlowOpen) {
        return TX_NO_FLOWRING;
    }
    if (Len < ETH_HLEN || Len - ETH_HLEN > DATA_BUF_SIZE) {
        return TX_BAD_LENGTH;
    }
    for (J = 0; J < TX_BUF_COUNT; J++) {
        I = (S->TxNext + J) % TX_BUF_COUNT;
        if (!S->TxBusy[I]) {
            break;
        }
    }
    if (J == TX_BUF_COUNT) {
        (VOID)D2hPoll(S, RING_D2H_TX_CPL);   /* reclaim, then try once more */
        for (J = 0; J < TX_BUF_COUNT; J++) {
            I = (S->TxNext + J) % TX_BUF_COUNT;
            if (!S->TxBusy[I]) {
                break;
            }
        }
        if (J == TX_BUF_COUNT) {
            return TX_NO_BUFFER;
        }
    }
    It = H2dNext(S, &S->Flow, S->DVa, MSG_TYPE_TX_POST, TX_PKTID0 + I, 0);
    if (It == NULL) {
        return TX_RING_FULL;
    }
    Ba = S->DLa + DAT_OFF_TXBUF + (ULONGLONG)I * DATA_BUF_SIZE;
    RtlCopyMemory(S->DVa + DAT_OFF_TXBUF + I * DATA_BUF_SIZE, Frame + ETH_HLEN, Len - ETH_HLEN);
    RtlCopyMemory(It + TXP_ETHHDR, Frame, ETH_HLEN);
    It[TXP_FLAGS]   = PKT_FLAGS_FRAME_802_3;   /* priority 0 (best effort) on the tid-0 flowring */
    It[TXP_SEG_CNT] = 1;
    *(ULONG UNALIGNED *)(It + TXP_DATA_ADDR)     = (ULONG)Ba;
    *(ULONG UNALIGNED *)(It + TXP_DATA_ADDR + 4) = (ULONG)(Ba >> 32);
    *(USHORT UNALIGNED *)(It + TXP_DATA_LEN)     = (USHORT)(Len - ETH_HLEN);
    S->TxBusy[I] = TRUE;
    S->TxNext    = (I + 1) % TX_BUF_COUNT;
    S->TxPosted++;
    S->TxLastIndex = I;
    CacheClean(S->MdlTx[I]);
    H2dCommit(S, &S->Flow, S->Flow.Mdl);
    if (S->TxLogged < 32) {
        ReportLine(S->R, "[tx] post #%lu pktid 0x%lX len %lu type 0x%04X -> %02X:%02X:%02X:%02X:%02X:%02X, flowring w %lu r %u\r\n",
                   S->TxPosted, TX_PKTID0 + I, Len, (USHORT)((Frame[12] << 8) | Frame[13]), Frame[0], Frame[1], Frame[2],
                   Frame[3], Frame[4], Frame[5], S->Flow.Host, Tcm16(S, S->Flow.RIdxAddr));
    }
    return TX_OK;
}

_Use_decl_annotations_
VOID
WlanMsgRun(PWLAN_REPORT R, PWLAN_BUS Bus, PVOID Ctx)
{
    PWLAN_MSG_CTX    M = (PWLAN_MSG_CTX)Ctx;
    MSG_STATE       *S = NULL;
    WDFCOMMONBUFFER  Cb = NULL;
    ULONG            Flags = Bus->SharedFlags, Shared = Bus->SharedAddr, Ri, RingMem, I, HostCap, Cmd;
    ULONG            H2dW, H2dR, D2hW, D2hR;
    USHORT           MaxSub, MaxCpl;
    NTSTATUS         Status;
    BOOLEAN          BmeOn = FALSE;
    static const ULONG Geo[RING_COMMON_COUNT][3] = {
        { H2D_CTRL_ITEMSIZE,     H2D_CTRL_MAX_ITEM,     MSG_OFF_H2D_CTRL },
        { H2D_RXPOST_ITEMSIZE,   H2D_RXPOST_MAX_ITEM,   MSG_OFF_H2D_RXPOST },
        { D2H_CTRL_CPL_ITEMSIZE, D2H_CTRL_CPL_MAX_ITEM, MSG_OFF_D2H_CTRL_CPL },
        { D2H_TX_CPL_ITEMSIZE,   D2H_TX_CPL_MAX_ITEM,   MSG_OFF_D2H_TX_CPL },
        { D2H_RX_CPL_ITEMSIZE,   D2H_RX_CPL_MAX_ITEM,   MSG_OFF_D2H_RX_CPL },
    };

    M->Result = MSG_BAD_SHARED;
    ReportLine(R, "---- milestone 3: msgbuf rings + first ioctl ----\r\n");
    if (!M->Config->Msgbuf) {
        ReportLine(R, "[msg] skipped (Parameters\\Msgbuf = 0) - firmware left running, no DMA\r\n");
        M->Result = MSG_SKIPPED;
        return;
    }
    if (M->DmaEnabler == NULL) {
        ReportLine(R, "MSG %u: no DMA enabler (EvtDeviceAdd failed to create it)\r\n", MSG_NO_DMA);
        M->Result = MSG_NO_DMA;
        goto Stop;
    }

    /* 1. pcie_shared */
    if ((Flags & PCIE_SHARED_VERSION_MASK) != PCIE_SHARED_VERSION_7 || (Flags & PCIE_SHARED_DMA_INDEX) != 0 ||
        (Flags & PCIE_SHARED_2BYTE_INDICES) != 0 || (Flags & PCIE_SHARED_D2H_SYNC_XORCSUM) == 0) {
        ReportLine(R, "MSG %u: flags 0x%08lX not the v7/TCM-index/XORCSUM layout this driver implements\r\n",
                   MSG_BAD_SHARED, Flags);
        goto Stop;
    }
    S = (MSG_STATE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*S), 'mDlW');
    if (S == NULL) {
        goto Stop;
    }
    S->R   = R;
    S->Bus = Bus;
    Ri = Tcm32(S, Shared + SH_RINGS_INFO_PTR);
    ReportLine(R, "[msg] shared: lfrag %u max_host_rxbufs %u dma_rxoffset %lu flags2 0x%08lX h2d_mb 0x%08lX d2h_mb 0x%08lX ring_info 0x%08lX\r\n",
               Tcm16(S, Shared + SH_TOTAL_LFRAG_CNT), Tcm16(S, Shared + SH_MAX_HOST_RXBUFS), Tcm32(S, Shared + SH_DMA_RXOFFSET),
               Tcm32(S, Shared + SH_FLAGS2), Tcm32(S, Shared + SH_H2D_MB_DATA_PTR), Tcm32(S, Shared + SH_D2H_MB_DATA_PTR), Ri);
    if (!InRam(S, Ri, RI_SIZE)) {
        ReportLine(R, "MSG %u: ring_info pointer outside RAM\r\n", MSG_BAD_SHARED);
        goto Stop;
    }
    S->RxOffset = Tcm32(S, Shared + SH_DMA_RXOFFSET);
    RingMem = Tcm32(S, Ri + RI_RINGMEM_PTR);
    H2dW    = Tcm32(S, Ri + RI_H2D_W_IDX_PTR);
    H2dR    = Tcm32(S, Ri + RI_H2D_R_IDX_PTR);
    D2hW    = Tcm32(S, Ri + RI_D2H_W_IDX_PTR);
    D2hR    = Tcm32(S, Ri + RI_D2H_R_IDX_PTR);
    MaxSub  = Tcm16(S, Ri + RI_MAX_SUBMISSION_QUEUES);
    MaxCpl  = Tcm16(S, Ri + RI_MAX_COMPLETION_RINGS);
    ReportLine(R, "[msg] ring_info: ringmem 0x%08lX h2d w 0x%08lX r 0x%08lX d2h w 0x%08lX r 0x%08lX; max flowrings %u sub %u cpl %u vdevs %u\r\n",
               RingMem, H2dW, H2dR, D2hW, D2hR, Tcm16(S, Ri + RI_MAX_TX_FLOWRINGS), MaxSub, MaxCpl, Tcm16(S, Ri + RI_MAX_VDEVS));
    if (!InRam(S, RingMem, RM_SIZE * RING_COMMON_COUNT) || !InRam(S, H2dW, RW_INDEX_SZ * RING_H2D_COUNT) ||
        !InRam(S, H2dR, RW_INDEX_SZ * RING_H2D_COUNT) || !InRam(S, D2hW, RW_INDEX_SZ * 3) || !InRam(S, D2hR, RW_INDEX_SZ * 3) ||
        MaxSub == 0 || MaxCpl == 0) {
        ReportLine(R, "MSG %u: ring_info pointers/maxima not usable\r\n", MSG_BAD_SHARED);
        goto Stop;
    }
    S->H2dWBase     = H2dW;   /* M7a: flowring index slots continue these arrays (dhd_pcie.c:8914-8932) */
    S->H2dRBase     = H2dR;
    S->MaxFlowrings = Tcm16(S, Ri + RI_MAX_TX_FLOWRINGS);

    /* 2. DMA memory */
    M->Result = MSG_NO_DMA;
    Status = WdfCommonBufferCreate(M->DmaEnabler, MSG_DMA_SIZE, WDF_NO_OBJECT_ATTRIBUTES, &Cb);
    if (!NT_SUCCESS(Status)) {
        ReportLine(R, "MSG %u: WdfCommonBufferCreate(0x%X) 0x%08lX\r\n", MSG_NO_DMA, MSG_DMA_SIZE, Status);
        Cb = NULL;
        goto Stop;
    }
    S->Va = (PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(Cb);
    S->La = (ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(Cb).QuadPart;
    RtlZeroMemory(S->Va, MSG_DMA_SIZE);
    ReportLine(R, "[msg] common buffer 0x%X B at logical 0x%I64X\r\n", MSG_DMA_SIZE, S->La);
    if (M->Config->CacheMaint) {
        S->MdlAll  = RegionMdl(S, 0, MSG_DMA_SIZE);
        S->MdlH2d  = RegionMdl(S, MSG_OFF_H2D_CTRL, MSG_OFF_D2H_CTRL_CPL - MSG_OFF_H2D_CTRL);
        S->MdlReq  = RegionMdl(S, MSG_OFF_IOCTL_REQ, IOCTL_REQ_BUF_SIZE);
        /* M7a: one MDL per completion ring, so a poll invalidates only the ring it reads */
        S->Ring[RING_D2H_CTRL_CPL].Mdl = RegionMdl(S, MSG_OFF_D2H_CTRL_CPL, D2H_CTRL_CPL_MAX_ITEM * D2H_CTRL_CPL_ITEMSIZE);
        S->Ring[RING_D2H_TX_CPL].Mdl   = RegionMdl(S, MSG_OFF_D2H_TX_CPL, D2H_TX_CPL_MAX_ITEM * D2H_TX_CPL_ITEMSIZE);
        S->Ring[RING_D2H_RX_CPL].Mdl   = RegionMdl(S, MSG_OFF_D2H_RX_CPL, D2H_RX_CPL_MAX_ITEM * D2H_RX_CPL_ITEMSIZE);
        S->MdlBufs = RegionMdl(S, MSG_OFF_IOCTL_RESP, MSG_DMA_SIZE - MSG_OFF_IOCTL_RESP);
        if (S->MdlAll == NULL || S->MdlH2d == NULL || S->MdlReq == NULL || S->Ring[RING_D2H_CTRL_CPL].Mdl == NULL ||
            S->Ring[RING_D2H_TX_CPL].Mdl == NULL || S->Ring[RING_D2H_RX_CPL].Mdl == NULL || S->MdlBufs == NULL) {
            ReportLine(R, "MSG %u: cache-maintenance MDLs not allocated\r\n", MSG_NO_DMA);
            goto Stop;
        }
        CacheClean(S->MdlAll);   /* the zeroing above must reach memory before the device reads it */
        ReportLine(R, "[msg] cache maintenance on (clean H2D + request before doorbells, invalidate D2H + buffers before reads)\r\n");
    } else {
        ReportLine(R, "[msg] cache maintenance OFF (Parameters\\CacheMaint = 0)\r\n");
    }
    if ((S->La >> 32) != 0 || (S->La & 0xFFF) != 0) {
        ReportLine(R, "MSG %u: common buffer not 4 KB aligned below 4 GB\r\n", MSG_NO_DMA);
        goto Stop;
    }

    /* 3. host_cap, rings, scratch */
    M->Result = MSG_BAD_SHARED;
    HostCap = M->Config->HostCap;
    if (HostCap == WLAN_HOSTCAP_AUTO) {
        HostCap = PCIE_SHARED_VERSION_7 | HOSTCAP_DS_NO_OOB_DW | HOSTCAP_UR_FW_NO_TRAP |
                  (((Flags & PCIE_SHARED_HOSTRDY_SUPPORT) != 0) ? HOSTCAP_H2D_ENABLE_HOSTRDY : 0);
    }
    if (HostCap != WLAN_HOSTCAP_DONT_WRITE) {
        TcmW32(S, Shared + SH_HOST_CAP, HostCap);
    }
    for (I = 0; I < RING_COMMON_COUNT; I++) {
        PMSG_RING G  = &S->Ring[I];
        ULONG     Rm = RingMem + RM_SIZE * I;
        ULONGLONG Ba;

        G->Id       = I;
        G->ItemSize = Geo[I][0];
        G->MaxItem  = Geo[I][1];
        G->Off      = Geo[I][2];
        G->Seq      = EPOCH_INIT_VAL;
        G->Host     = 0;
        if (I < RING_H2D_COUNT) {
            G->WIdxAddr = H2dW + RW_INDEX_SZ * I;
            G->RIdxAddr = H2dR + RW_INDEX_SZ * I;
        } else {
            G->WIdxAddr = D2hW + RW_INDEX_SZ * (I - RING_H2D_COUNT);
            G->RIdxAddr = D2hR + RW_INDEX_SZ * (I - RING_H2D_COUNT);
        }
        Ba = S->La + G->Off;
        TcmW16(S, Rm + RM_MAX_ITEM, (USHORT)G->MaxItem);
        TcmW16(S, Rm + RM_LEN_ITEMS, (USHORT)G->ItemSize);
        TcmW32(S, Rm + RM_BASE_ADDR, (ULONG)Ba);
        TcmW32(S, Rm + RM_BASE_ADDR + 4, (ULONG)(Ba >> 32));
        TcmW16(S, G->WIdxAddr, 0);
        TcmW16(S, G->RIdxAddr, 0);
    }
    TcmW32(S, Shared + SH_SCRATCH_LEN, D2H_SCRATCH_LEN);
    TcmW32(S, Shared + SH_SCRATCH_ADDR, (ULONG)(S->La + MSG_OFF_SCRATCH));
    TcmW32(S, Shared + SH_SCRATCH_ADDR + 4, (ULONG)((S->La + MSG_OFF_SCRATCH) >> 32));
    KeMemoryBarrier();
    ReportLine(R, "[msg] host_cap 0x%08lX written; rings: ringmem[0] max %u len %u base 0x%08lX ... ringmem[4] max %u len %u\r\n",
               Tcm32(S, Shared + SH_HOST_CAP), Tcm16(S, RingMem + RM_MAX_ITEM), Tcm16(S, RingMem + RM_LEN_ITEMS),
               Tcm32(S, RingMem + RM_BASE_ADDR), Tcm16(S, RingMem + 4 * RM_SIZE + RM_MAX_ITEM),
               Tcm16(S, RingMem + 4 * RM_SIZE + RM_LEN_ITEMS));

    /* 4. bus mastering + hostready */
    S->Db0 = M->Config->DoorbellDar ? PCIE2_DAR64_H2D_DB0 : PCIE2_H2D_DB0;
    S->Db1 = M->Config->DoorbellDar ? PCIE2_DAR64_H2D_DB1 : PCIE2_H2D_DB1;
    Cmd = READ_REGISTER_ULONG((volatile ULONG *)(Bus->Cfg + EP_CFG_COMMAND)) & 0xFFFF;
    WRITE_REGISTER_ULONG((volatile ULONG *)(Bus->Cfg + EP_CFG_COMMAND), Cmd | EP_CFG_COMMAND_BME);
    BmeOn = TRUE;
    ReportLine(R, "[msg] EP COMMAND 0x%04lX -> 0x%08lX (bus master on); doorbells 0x%X/0x%X\r\n", Cmd,
               READ_REGISTER_ULONG((volatile ULONG *)(Bus->Cfg + EP_CFG_COMMAND)), S->Db0, S->Db1);
    if ((Flags & PCIE_SHARED_HOSTRDY_SUPPORT) != 0) {
        Doorbell(S, S->Db1);
        ReportLine(R, "[msg] hostready (DB1)\r\n");
    }

    /* 5. ioctl-response buffers */
    for (I = 0; I < IOCTL_RESP_BUF_COUNT; I++) {
        (VOID)BufPost(S, MSG_TYPE_IOCTLRESP_BUF_POST, IOCTL_RESP_PKTID0 + I, MSG_OFF_IOCTL_RESP + I * IOCTL_RESP_BUF_SIZE,
                      IOCTL_RESP_BUF_SIZE);
    }
    H2dCtrlCommit(S);
    {
        LARGE_INTEGER Delay;

        Delay.QuadPart = -10000LL * 50;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    (VOID)D2hCtrlPoll(S);
    ReportLine(R, "[msg] %u ioctl-resp buffers posted; H2D ctrl w %lu, fw read index %u\r\n", IOCTL_RESP_BUF_COUNT,
               S->Ring[RING_H2D_CTRL].Host, Tcm16(S, S->Ring[RING_H2D_CTRL].RIdxAddr));
    WlanBusConsoleDump(R, Bus);
    ReportWriteFile(R);

    /* 6. first ioctls */
    M->Result = IoctlVar(S, FALSE, "cur_etheraddr", NULL, 0, 32);
    if (M->Result == MSG_OK && S->RespLen >= 6) {
        RtlCopyMemory(M->Mac, S->Resp, 6);
        RtlCopyMemory(S->Mac, S->Resp, 6);
        M->MacValid = TRUE;
        ReportLine(R, "[msg] cur_etheraddr = %02X:%02X:%02X:%02X:%02X:%02X\r\n", S->Resp[0], S->Resp[1], S->Resp[2],
                   S->Resp[3], S->Resp[4], S->Resp[5]);
    }
    if (M->Result == MSG_OK) {
        M->Result = IoctlVar(S, FALSE, "ver", NULL, 0, 200);
        if (M->Result == MSG_OK) {
            S->Resp[(S->RespLen < sizeof(S->Resp)) ? S->RespLen : sizeof(S->Resp) - 1] = 0;
            for (I = 0; I < S->RespLen && S->Resp[I] != 0; I++) {
                if (S->Resp[I] < 0x20 || S->Resp[I] >= 0x7F) {
                    S->Resp[I] = ' ';
                }
            }
            ReportLine(R, "[msg] ver = %s\r\n", (PCSTR)S->Resp);
        }
    }
    if ((Tcm32(S, Shared + SH_FLAGS) & PCIE_SHARED_TRAP) != 0) {
        ReportLine(R, "[msg] firmware TRAP flag set (trap 0x%08lX)\r\n", Tcm32(S, Shared + SH_TRAP_ADDR));
        M->Result = MSG_TRAP;
    }
    WlanBusConsoleDump(R, Bus);
    if (M->Result == MSG_OK) {
        ReportLine(R, "MSG 0: firmware answered over msgbuf (MAC %s)\r\n", M->MacValid ? "read" : "not read");
    } else {
        ReportLine(R, "MSG %lu: msgbuf exchange failed\r\n", M->Result);
    }
    ReportWriteFile(R);

    /* 8. milestone 4 (notes/m12-wifi-m4.md): event buffers, ESCAN_RESULT in event_msgs, UP, escan, results */
    if (M->Result == MSG_OK && M->Config->Scan) {
        UCHAR         Mask[WL_EVENTING_MASK_LEN];
        ULONG         One = 1;
        BOOLEAN       V2;
        ULONG         Major;

        ReportLine(R, "---- milestone 4: events + escan ----\r\n");
        M->ScanResult = SCAN_FAILED;
        for (I = 0; I < EVENT_BUF_COUNT; I++) {
            (VOID)BufPost(S, MSG_TYPE_EVENT_BUF_POST, EVENT_BUF_PKTID0 + I, MSG_OFF_EVENT_BUF + I * EVENT_BUF_SIZE,
                          EVENT_BUF_SIZE);
        }
        H2dCtrlCommit(S);
        ReportLine(R, "[scan] %u event buffers posted (rxoffset %lu)\r\n", EVENT_BUF_COUNT, S->RxOffset);
        if (IoctlVar(S, FALSE, "event_msgs", NULL, 0, WL_EVENTING_MASK_LEN) != MSG_OK || S->RespLen < WL_EVENTING_MASK_LEN) {
            goto ScanEnd;
        }
        RtlCopyMemory(Mask, S->Resp, WL_EVENTING_MASK_LEN);
        ReportLine(R, "[scan] event_msgs byte 8 = 0x%02X (ESCAN_RESULT bit %s)\r\n", Mask[WLC_E_ESCAN_RESULT / 8],
                   (Mask[WLC_E_ESCAN_RESULT / 8] & (1 << (WLC_E_ESCAN_RESULT % 8))) ? "set" : "clear");
        Mask[WLC_E_ESCAN_RESULT / 8] |= (UCHAR)(1 << (WLC_E_ESCAN_RESULT % 8));
        (VOID)ClmLoad(S);   /* logged; a failure still lets the scan show what the fw does without it */
        if (IoctlVar(S, TRUE, "event_msgs", Mask, WL_EVENTING_MASK_LEN, 0) != MSG_OK ||
            Ioctl(S, WLC_UP, &One, sizeof(One), 0, "UP") != MSG_OK) {
            goto ScanEnd;
        }
        /* escan params version: v2 when "scan_ver" answers (fmac feature.c:342). Run m4-2: this fw reports
           wlc 12.1 (dhd wl_cfg80211.c:18506 would pick v1) but refuses v1 with BCME_VERSION (-37), so the fmac
           rule is the one that matches it. wlc_ver is still logged. */
        Major = 0;
        if (IoctlVar(S, FALSE, "wlc_ver", NULL, 0, WLC_VER_SIZE) == MSG_OK && S->RespLen >= WLC_VER_SIZE) {
            Major = *(USHORT UNALIGNED *)(S->Resp + WLC_VER_MAJOR);
            ReportLine(R, "[scan] wlc_ver: epi %u.%u.%u.%u wlc %u.%u\r\n", *(USHORT UNALIGNED *)(S->Resp + 4),
                       *(USHORT UNALIGNED *)(S->Resp + 6), *(USHORT UNALIGNED *)(S->Resp + 8),
                       *(USHORT UNALIGNED *)(S->Resp + 10), Major, *(USHORT UNALIGNED *)(S->Resp + WLC_VER_MAJOR + 2));
        }
        V2 = (IoctlVar(S, FALSE, "scan_ver", NULL, 0, sizeof(ULONG)) == MSG_OK);
        ReportLine(R, "[scan] scan_ver %s -> escan params v%u\r\n", V2 ? "answered" : "failed", V2 ? 2 : 1);
        S->ScanV2 = V2;
        M->ScanResult = EscanRun(S);
        M->BssCount   = S->BssCount;
ScanEnd:
        if (M->ScanResult == SCAN_FAILED) {
            ReportLine(R, "SCAN %u: setup ioctl or escan request failed\r\n", SCAN_FAILED);
        }
        if ((Tcm32(S, Shared + SH_FLAGS) & PCIE_SHARED_TRAP) != 0) {
            ReportLine(R, "[scan] firmware TRAP flag set (trap 0x%08lX)\r\n", Tcm32(S, Shared + SH_TRAP_ADDR));
        }
        WlanBusConsoleDump(R, Bus);
    }

    /* 9. milestone 5 (notes/m12-wifi-m5.md): keep the firmware, rings and buffer for the device lifetime. The
       copy of Bus lives in M (device context); WlanDlRun sees Bus->Keep and leaves BAR0/BAR2 + chip mapped. */
    if (M->Config->Resident && M->Result == MSG_OK && M->ScanResult == SCAN_OK) {
        M->Bus          = *Bus;
        M->Bus.Keep     = TRUE;
        Bus->Keep       = TRUE;
        S->Bus          = &M->Bus;
        M->State        = S;
        M->CommonBuffer = Cb;
        M->Resident     = TRUE;
        M->Quiesced     = FALSE;
        M->Config       = NULL;   /* points at PrepareHardware's stack */
        (VOID)DataInit(S, M);     /* milestone 7a; a failure leaves the control path resident without data */
        ReportLine(R, "RESIDENT: firmware kept running (ARM up, bus master on, buffer 0x%I64X, rings live)\r\n", S->La);
        ReportWriteFile(R);
        return;
    }

Stop:
    /* 7. lifetime rule: nothing may DMA into the buffer once it is freed */
    WlanBusArmHalt(Bus);
    KeStallExecutionProcessor(1000);
    if (BmeOn) {
        Cmd = READ_REGISTER_ULONG((volatile ULONG *)(Bus->Cfg + EP_CFG_COMMAND)) & 0xFFFF;
        WRITE_REGISTER_ULONG((volatile ULONG *)(Bus->Cfg + EP_CFG_COMMAND), Cmd & ~EP_CFG_COMMAND_BME);
    }
    ReportLine(R, "[msg] stop: ARM halted, EP COMMAND 0x%08lX\r\n", READ_REGISTER_ULONG((volatile ULONG *)(Bus->Cfg + EP_CFG_COMMAND)));
    if (Cb != NULL) {
        WdfObjectDelete(Cb);
    }
    if (S != NULL) {
        RegionMdlsFree(S);
        ExFreePoolWithTag(S, 'mDlW');
    }
    ReportWriteFile(R);
}

/* ---------------------------------------------------------------- milestone 5 runtime */

static PMSG_STATE
Live(_In_ PWLAN_MSG_CTX M, _In_ PWLAN_REPORT R)
{
    PMSG_STATE S;

    if (!M->Resident || M->Quiesced || M->State == NULL) {
        return NULL;
    }
    S    = (PMSG_STATE)M->State;
    S->R = R;
    WlanBusSetReport(&M->Bus, R);
    return S;
}

_Use_decl_annotations_
ULONG
WlanMsgPoll(PWLAN_MSG_CTX M, PWLAN_REPORT R)
{
    PMSG_STATE S = Live(M, R);

    if (S == NULL) {
        return 0;
    }
    if ((Tcm32(S, S->Bus->SharedAddr + SH_FLAGS) & PCIE_SHARED_TRAP) != 0) {
        ReportLine(R, "[run] firmware TRAP flag set (trap 0x%08lX) - quiescing\r\n",
                   Tcm32(S, S->Bus->SharedAddr + SH_TRAP_ADDR));
        WlanBusConsoleDump(R, &M->Bus);
        WlanMsgQuiesce(M, R);
        return 0;
    }
    {
        ULONG N = D2hPollAll(S);

        if (S->EapolPending) {
            SupEapol(S);   /* M7b: group rekeys after the join */
        }
        return N;
    }
}

_Use_decl_annotations_
ULONG
WlanMsgTx(PWLAN_MSG_CTX M, PWLAN_REPORT R, const UCHAR *Frame, ULONG Len)
{
    PMSG_STATE S = Live(M, R);

    if (S == NULL || !S->DataOn) {
        return TX_NO_FLOWRING;
    }
    return TxFrame(S, Frame, Len);
}

_Use_decl_annotations_
VOID
WlanMsgSetRx(PWLAN_MSG_CTX M, WLAN_RX_FN *Fn, PVOID Ctx)
{
    PMSG_STATE S = (M->Resident && M->State != NULL) ? (PMSG_STATE)M->State : NULL;

    if (S != NULL) {
        S->RxFn  = Fn;
        S->RxCtx = Ctx;
    }
}

_Use_decl_annotations_
ULONG
WlanMsgRescan(PWLAN_MSG_CTX M, PWLAN_REPORT R)
{
    PMSG_STATE S = Live(M, R);

    if (S == NULL) {
        return SCAN_SKIPPED;
    }
    return EscanRun(S);
}

_Use_decl_annotations_
VOID
WlanMsgQuiesce(PWLAN_MSG_CTX M, PWLAN_REPORT R)
{
    ULONG Cmd;

    if (!M->Resident || M->Quiesced) {
        return;
    }
    WlanBusSetReport(&M->Bus, R);
    WlanBusArmHalt(&M->Bus);
    KeStallExecutionProcessor(1000);
    Cmd = READ_REGISTER_ULONG((volatile ULONG *)(M->Bus.Cfg + EP_CFG_COMMAND)) & 0xFFFF;
    WRITE_REGISTER_ULONG((volatile ULONG *)(M->Bus.Cfg + EP_CFG_COMMAND), Cmd & ~EP_CFG_COMMAND_BME);
    M->Quiesced = TRUE;
    ReportLine(R, "[run] quiesced: ARM halted, EP COMMAND 0x%08lX\r\n",
               READ_REGISTER_ULONG((volatile ULONG *)(M->Bus.Cfg + EP_CFG_COMMAND)));
}

_Use_decl_annotations_
VOID
WlanMsgFree(PWLAN_MSG_CTX M)
{
    if (!M->Resident) {
        return;
    }
    NT_ASSERT(M->Quiesced);   /* lifetime rule: never free a buffer the firmware may still DMA into */
    if (!M->Quiesced) {
        return;               /* leak rather than free under live DMA */
    }
    if (M->CommonBuffer != NULL) {
        WdfObjectDelete(M->CommonBuffer);
        M->CommonBuffer = NULL;
    }
    if (M->DataBuffer != NULL) {
        WdfObjectDelete(M->DataBuffer);
        M->DataBuffer = NULL;
    }
    if (M->State != NULL) {
        RegionMdlsFree((PMSG_STATE)M->State);
        ExFreePoolWithTag(M->State, 'mDlW');
        M->State = NULL;
    }
    WlanDlRelease(&M->Bus);
    M->Resident = FALSE;
}

/* ---------------------------------------------------------------- milestone 6: WPA2-PSK join */

static BOOLEAN
AsciiFromUnicode(_In_ PCUNICODE_STRING U, _Out_writes_(Max) PCHAR Out, _In_ ULONG Max, _Out_ PULONG Len)
{
    ULONG I, N = U->Length / sizeof(WCHAR);

    *Len = 0;
    if (N > Max) {
        return FALSE;
    }
    for (I = 0; I < N; I++) {
        if (U->Buffer[I] < 0x20 || U->Buffer[I] >= 0x7F) {
            return FALSE;   /* printable ASCII only for now */
        }
        Out[I] = (CHAR)U->Buffer[I];
    }
    *Len = N;
    return TRUE;
}

static LONG
HexNibble(_In_ WCHAR C)
{
    if (C >= L'0' && C <= L'9') {
        return C - L'0';
    }
    if (C >= L'a' && C <= L'f') {
        return C - L'a' + 10;
    }
    if (C >= L'A' && C <= L'F') {
        return C - L'A' + 10;
    }
    return -1;
}

_Use_decl_annotations_
NTSTATUS
WlanJoinPrepare(PCUNICODE_STRING Ssid, PCUNICODE_STRING Passphrase, PCUNICODE_STRING Psk, PWLAN_JOIN_CONFIG J,
                PWLAN_REPORT R)
{
    CHAR              Pass[WSEC_MAX_PSK_LEN];
    ULONG             SsidLen, PassLen, I;
    BCRYPT_ALG_HANDLE Alg = NULL;
    NTSTATUS          Status;

    RtlZeroMemory(J, sizeof(*J));
    if (Ssid->Length == 0) {
        return STATUS_NOT_FOUND;   /* not configured: milestone 6 dormant */
    }
    if (!AsciiFromUnicode(Ssid, J->Ssid, sizeof(J->Ssid), &SsidLen) || SsidLen == 0) {
        ReportLine(R, "[join] Parameters\\Ssid must be 1..32 printable ASCII characters\r\n");
        return STATUS_INVALID_PARAMETER;
    }
    J->SsidLen = (UCHAR)SsidLen;
    if (Psk->Length == 2 * PMK_LEN * sizeof(WCHAR)) {
        for (I = 0; I < PMK_LEN; I++) {
            LONG Hi = HexNibble(Psk->Buffer[2 * I]), Lo = HexNibble(Psk->Buffer[2 * I + 1]);

            if (Hi < 0 || Lo < 0) {
                ReportLine(R, "[join] Parameters\\Psk must be 64 hex characters\r\n");
                return STATUS_INVALID_PARAMETER;
            }
            J->Pmk[I] = (UCHAR)((Hi << 4) | Lo);
        }
        J->Valid = TRUE;
        ReportLine(R, "[join] target \"%.*s\", PMK from Parameters\\Psk\r\n", (int)SsidLen, J->Ssid);
        return STATUS_SUCCESS;
    }
    if (!AsciiFromUnicode(Passphrase, Pass, sizeof(Pass) - 1, &PassLen) || PassLen < WSEC_MIN_PSK_LEN) {
        ReportLine(R, "[join] Parameters\\Passphrase must be 8..63 printable ASCII characters (or set Psk)\r\n");
        RtlSecureZeroMemory(Pass, sizeof(Pass));
        return STATUS_INVALID_PARAMETER;
    }
    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_SHA1_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (NT_SUCCESS(Status)) {
        Status = BCryptDeriveKeyPBKDF2(Alg, (PUCHAR)Pass, PassLen, (PUCHAR)J->Ssid, SsidLen, PBKDF2_ITERATIONS, J->Pmk,
                                       PMK_LEN, 0);
        BCryptCloseAlgorithmProvider(Alg, 0);
    }
    RtlSecureZeroMemory(Pass, sizeof(Pass));
    if (!NT_SUCCESS(Status)) {
        ReportLine(R, "[join] PBKDF2 failed 0x%08lX\r\n", Status);
        RtlSecureZeroMemory(J, sizeof(*J));
        return Status;
    }
    J->Valid = TRUE;
    ReportLine(R, "[join] target \"%.*s\", PMK derived from the passphrase (PBKDF2-HMAC-SHA1, %u)\r\n", (int)SsidLen,
               J->Ssid, PBKDF2_ITERATIONS);
    return STATUS_SUCCESS;
}

/* IEEE 802.11i-2004 H.4.1 test vector: passphrase "password", SSID "IEEE". Proves the kernel CNG PBKDF2 path
   without real credentials. */
_Use_decl_annotations_
BOOLEAN
WlanJoinSelfTest(PWLAN_REPORT R)
{
    static const UCHAR Want[PMK_LEN] = {
        0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef, 0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
        0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2, 0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e };
    UNICODE_STRING   Ssid, Pass, Psk;
    WLAN_JOIN_CONFIG J;
    WLAN_REPORT     *Quiet;
    BOOLEAN          Ok;

    Quiet = (WLAN_REPORT *)ExAllocatePool2(POOL_FLAG_PAGED, sizeof(*Quiet), 'tlWE');   /* swallow its lines */
    if (Quiet == NULL) {
        return FALSE;
    }
    RtlInitUnicodeString(&Ssid, L"IEEE");
    RtlInitUnicodeString(&Pass, L"password");
    RtlInitUnicodeString(&Psk, NULL);
    Ok = NT_SUCCESS(WlanJoinPrepare(&Ssid, &Pass, &Psk, &J, Quiet)) && RtlCompareMemory(J.Pmk, Want, PMK_LEN) == PMK_LEN;
    ExFreePoolWithTag(Quiet, 'tlWE');
    ReportLine(R, "[join] PBKDF2 self-test (IEEE 802.11i H.4.1): %s\r\n", Ok ? "PASS" : "FAIL");
    RtlSecureZeroMemory(&J, sizeof(J));
    return Ok;
}

static ULONG
SetInt(_Inout_ PMSG_STATE S, _In_z_ PCSTR Name, _In_ ULONG Value)
{
    return IoctlVar(S, TRUE, Name, &Value, sizeof(Value), 0);
}

static USHORT
Be16(_In_reads_(2) const UCHAR *P)
{
    return (USHORT)((P[0] << 8) | P[1]);
}

/* Our RSN IE (= the assoc request IE = msg 2 key data, 802.11i 7.3.2.25): version 1, the AP's group cipher,
   pairwise CCMP, AKM PSK, capabilities 0 (no MFP). NULL when the AP needs something else. */
static BOOLEAN
BuildOwnRsn(_Inout_ PMSG_STATE S)
{
    static const UCHAR Oui[3] = { 0x00, 0x0F, 0xAC };
    const UCHAR *A = S->ApRsn;
    ULONG        Caps = 0;

    if (S->ApRsnLen < 8 || A[0] != RSN_IE_ID || A[2] != 1 || A[3] != 0 || RtlCompareMemory(A + 4, Oui, 3) != 3) {
        return FALSE;
    }
    if (S->ApRsnLen >= 2UL + A[1] && A[1] >= 20) {
        Caps = A[2 + A[1] - 2] | (A[2 + A[1] - 1] << 8);   /* only valid for the single-suite layout */
    }
    if ((Caps & RSN_CAP_MFPR) != 0) {
        ReportLine(S->R, "[sup] AP requires management frame protection (RSN caps 0x%04lX) - not supported\r\n", Caps);
        return FALSE;
    }
    S->OwnRsn[0] = RSN_IE_ID;
    S->OwnRsn[1] = 20;
    S->OwnRsn[2] = 1;
    S->OwnRsn[3] = 0;
    RtlCopyMemory(S->OwnRsn + 4, A + 4, 4);            /* group cipher as the AP announces it */
    S->OwnRsn[8] = 1;
    S->OwnRsn[9] = 0;
    RtlCopyMemory(S->OwnRsn + 10, Oui, 3);
    S->OwnRsn[13] = RSN_SUITE_CCMP;
    S->OwnRsn[14] = 1;
    S->OwnRsn[15] = 0;
    RtlCopyMemory(S->OwnRsn + 16, Oui, 3);
    S->OwnRsn[19] = RSN_AKM_PSK_SUITE;
    S->OwnRsn[20] = 0;
    S->OwnRsn[21] = 0;
    S->OwnRsnLen  = 22;
    return TRUE;
}

/* "wsec_key" with wl_wsec_key_t (dhd wl_add_keyext / wl_cfg80211_add_key, wl_cfg80211.c:6838-6910, 7110-7150). */
static ULONG
InstallKey(_Inout_ PMSG_STATE S, _In_ ULONG Index, _In_reads_(SUP_KEY_LEN) const UCHAR *Key,
           _In_reads_opt_(6) const UCHAR *Ea, _In_reads_opt_(6) const UCHAR *Rsc)
{
    UCHAR K[WSK_SIZE];
    ULONG Rc;

    RtlZeroMemory(K, sizeof(K));
    *(ULONG UNALIGNED *)(K + WSK_INDEX) = Index;
    *(ULONG UNALIGNED *)(K + WSK_LEN)   = SUP_KEY_LEN;
    RtlCopyMemory(K + WSK_DATA, Key, SUP_KEY_LEN);
    *(ULONG UNALIGNED *)(K + WSK_ALGO)  = CRYPTO_ALGO_AES_CCM;
    if (Ea != NULL) {
        RtlCopyMemory(K + WSK_EA, Ea, 6);
    } else {
        *(ULONG UNALIGNED *)(K + WSK_FLAGS) = WL_PRIMARY_KEY;
    }
    if (Rsc != NULL) {
        *(ULONG UNALIGNED *)(K + WSK_RXIV_HI)  = ((ULONG)Rsc[5] << 24) | ((ULONG)Rsc[4] << 16) | ((ULONG)Rsc[3] << 8) | Rsc[2];
        *(USHORT UNALIGNED *)(K + WSK_RXIV_LO) = (USHORT)((Rsc[1] << 8) | Rsc[0]);
        *(ULONG UNALIGNED *)(K + WSK_IV_INIT)  = 1;
    }
    Rc = IoctlVar(S, TRUE, "wsec_key", K, sizeof(K), 0);
    RtlSecureZeroMemory(K, sizeof(K));
    RtlSecureZeroMemory(S->Va + MSG_OFF_IOCTL_REQ, IOCTL_REQ_BUF_SIZE);
    RtlSecureZeroMemory(S->VarBuf, sizeof(S->VarBuf));
    return Rc;
}

/* Send one EAPOL-Key frame to the AP (wpa_supplicant wpa_eapol_key_send): MIC over the EAPOL PDU with the
   MIC field zero. */
static ULONG
SendKey(_Inout_ PMSG_STATE S, _In_ USHORT Info, _In_reads_(8) const UCHAR *Replay,
        _In_reads_opt_(SUP_NONCE_LEN) const UCHAR *Nonce, _In_reads_bytes_opt_(DataLen) const UCHAR *Data, _In_ ULONG DataLen)
{
    UCHAR  F[ETH_HLEN + EK_DATA + RSN_IE_MAX];
    PUCHAR E = F + ETH_HLEN;
    ULONG  Body = EK_FIXED_BODY + DataLen;
    UCHAR  Mic[SUP_MIC_LEN];

    if (DataLen > RSN_IE_MAX) {
        return TX_BAD_LENGTH;
    }
    RtlZeroMemory(F, sizeof(F));
    RtlCopyMemory(F, S->Aa, 6);
    RtlCopyMemory(F + 6, S->Mac, 6);
    F[12] = (UCHAR)(ETH_TYPE_EAPOL >> 8);
    F[13] = (UCHAR)(ETH_TYPE_EAPOL & 0xFF);
    E[0] = EAPOL_VERSION_TX;
    E[1] = EAPOL_TYPE_KEY;
    E[2] = (UCHAR)(Body >> 8);
    E[3] = (UCHAR)Body;
    E[EK_DESC]     = EAPOL_KEY_DESC_RSN;
    E[EK_INFO]     = (UCHAR)(Info >> 8);
    E[EK_INFO + 1] = (UCHAR)Info;
    RtlCopyMemory(E + EK_REPLAY, Replay, 8);
    if (Nonce != NULL) {
        RtlCopyMemory(E + EK_NONCE, Nonce, SUP_NONCE_LEN);
    }
    E[EK_DATALEN]     = (UCHAR)(DataLen >> 8);
    E[EK_DATALEN + 1] = (UCHAR)DataLen;
    if (DataLen != 0) {
        RtlCopyMemory(E + EK_DATA, Data, DataLen);
    }
    if (!NT_SUCCESS(SupMic(S->Ptk + SUP_KCK_OFF, E, 4 + Body, Mic))) {
        return TX_BAD_LENGTH;
    }
    RtlCopyMemory(E + EK_MIC, Mic, SUP_MIC_LEN);
    return TxFrame(S, F, ETH_HLEN + 4 + Body);
}

/* Verify the MIC of a received EAPOL-Key PDU (E, Len = 4 + body). */
static BOOLEAN
MicOk(_Inout_ PMSG_STATE S, _In_reads_bytes_(Len) const UCHAR *E, _In_ ULONG Len)
{
    UCHAR Copy[sizeof(S->Eapol)];
    UCHAR Mic[SUP_MIC_LEN];

    if (Len > sizeof(Copy) || Len < EK_DATA) {
        return FALSE;
    }
    RtlCopyMemory(Copy, E, Len);
    RtlZeroMemory(Copy + EK_MIC, SUP_MIC_LEN);
    return NT_SUCCESS(SupMic(S->Ptk + SUP_KCK_OFF, Copy, Len, Mic)) && RtlCompareMemory(Mic, E + EK_MIC, SUP_MIC_LEN) == SUP_MIC_LEN;
}

/* GTK KDE (dd len 00-0F-AC-01 keyid/tx reserved GTK) from decrypted key data; returns the GTK length or 0. */
static ULONG
FindGtk(_In_reads_bytes_(Len) const UCHAR *D, _In_ ULONG Len, _Out_ PULONG Idx, _Out_writes_(32) PUCHAR Gtk)
{
    ULONG P = 0;

    while (P + 2 <= Len && D[P] != 0) {
        ULONG L = D[P + 1];

        if (P + 2 + L > Len) {
            break;
        }
        if (D[P] == 0xDD && L >= 6 + SUP_KEY_LEN && D[P + 2] == 0x00 && D[P + 3] == 0x0F && D[P + 4] == 0xAC &&
            D[P + 5] == KDE_GTK) {
            ULONG G = L - 6;

            if (G > 32) {
                G = 32;
            }
            *Idx = D[P + 6] & 3;
            RtlCopyMemory(Gtk, D + P + 8, G);
            return G;
        }
        P += 2 + L;
    }
    return 0;
}

/* Waits (polling the tx completion ring) until the last posted tx buffer is done: msg 4 must be on the air
   before the pairwise key is installed (dhd_wait_pend8021x). */
static VOID
WaitTxIdle(_Inout_ PMSG_STATE S)
{
    ULONG         Ms;
    LARGE_INTEGER Delay;

    for (Ms = 0; Ms < MSG4_TX_WAIT_MS && S->TxBusy[S->TxLastIndex]; Ms += 5) {
        (VOID)D2hPoll(S, RING_D2H_TX_CPL);
        Delay.QuadPart = -10000LL * 5;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
}

/* Host WPA2-PSK supplicant (IEEE 802.11i 8.5.3 4-way handshake, 8.5.4 group key handshake). */
static VOID
SupEapol(_Inout_ PMSG_STATE S)
{
    PUCHAR F = S->Eapol;
    PUCHAR E = F + ETH_HLEN;
    ULONG  Len = S->EapolLen, Pdu, DataLen, GtkLen, GtkIdx = 0, Rc;
    USHORT Info;
    UCHAR  Plain[RSN_IE_MAX * 4];
    UCHAR  Gtk[32];

    S->EapolPending = FALSE;
    if (Len < ETH_HLEN + EK_DATA || E[1] != EAPOL_TYPE_KEY || E[EK_DESC] != EAPOL_KEY_DESC_RSN) {
        ReportLine(S->R, "[sup] EAPOL frame ignored (len %lu type %u desc %u)\r\n", Len, E[1], E[EK_DESC]);
        return;
    }
    Pdu     = 4 + Be16(E + 2);
    Info    = Be16(E + EK_INFO);
    DataLen = Be16(E + EK_DATALEN);
    if (Pdu > Len - ETH_HLEN || EK_DATA + DataLen > Pdu || (Info & KI_VER_MASK) != KI_VER_HMAC_SHA1_AES) {
        ReportLine(S->R, "[sup] EAPOL-Key malformed or unsupported (info 0x%04X len %lu data %lu)\r\n", Info, Pdu, DataLen);
        return;
    }
    if (S->ReplaySet && (Info & KI_MIC) != 0 && RtlCompareMemory(E + EK_REPLAY, S->Replay, 8) != 8) {
        ULONG K;

        for (K = 0; K < 8 && E[EK_REPLAY + K] == S->Replay[K]; K++) {
        }
        if (K < 8 && E[EK_REPLAY + K] < S->Replay[K]) {
            ReportLine(S->R, "[sup] replayed EAPOL-Key dropped\r\n");
            return;
        }
    }

    if ((Info & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_PAIRWISE | KI_ACK)) {
        /* msg 1/4 */
        RtlCopyMemory(S->Aa, F + 6, 6);
        RtlCopyMemory(S->ANonce, E + EK_NONCE, SUP_NONCE_LEN);
        if (!NT_SUCCESS(SupRandom(S->SNonce, SUP_NONCE_LEN)) ||
            !NT_SUCCESS(SupPtk(S->Pmk, S->Aa, S->Mac, S->ANonce, S->SNonce, S->Ptk))) {
            S->Hs = HS_FAILED;
            ReportLine(S->R, "[sup] msg 1/4: PTK derivation failed\r\n");
            return;
        }
        S->PtkSet = TRUE;
        Rc = SendKey(S, KI_VER_HMAC_SHA1_AES | KI_PAIRWISE | KI_MIC, E + EK_REPLAY, S->SNonce, S->OwnRsn, S->OwnRsnLen);
        S->Hs = HS_SENT_M2;
        ReportLine(S->R, "[sup] msg 1/4 from %02X:%02X:%02X:%02X:%02X:%02X -> msg 2/4 sent (tx %lu)\r\n", S->Aa[0], S->Aa[1],
                   S->Aa[2], S->Aa[3], S->Aa[4], S->Aa[5], Rc);
        return;
    }
    if (!S->PtkSet || (Info & KI_MIC) == 0 || !MicOk(S, E, Pdu)) {
        ReportLine(S->R, "[sup] EAPOL-Key info 0x%04X: MIC %s - dropped\r\n", Info, S->PtkSet ? "BAD" : "without a PTK");
        if ((Info & KI_PAIRWISE) != 0 && S->Hs == HS_SENT_M2) {
            S->Hs = HS_FAILED;   /* msg 3 with a bad MIC = wrong passphrase */
        }
        return;
    }
    RtlCopyMemory(S->Replay, E + EK_REPLAY, 8);
    S->ReplaySet = TRUE;
    GtkLen = 0;
    if ((Info & KI_ENCR_DATA) != 0 && DataLen >= 24 && DataLen - 8 <= sizeof(Plain)) {
        if (NT_SUCCESS(SupAesUnwrap(S->Ptk + SUP_KEK_OFF, E + EK_DATA, DataLen, Plain))) {
            GtkLen = FindGtk(Plain, DataLen - 8, &GtkIdx, Gtk);
        } else {
            ReportLine(S->R, "[sup] key data unwrap failed\r\n");
        }
        RtlSecureZeroMemory(Plain, sizeof(Plain));
    }

    if ((Info & (KI_PAIRWISE | KI_ACK | KI_INSTALL)) == (KI_PAIRWISE | KI_ACK | KI_INSTALL)) {
        /* msg 3/4 */
        if (RtlCompareMemory(E + EK_NONCE, S->ANonce, SUP_NONCE_LEN) != SUP_NONCE_LEN) {
            ReportLine(S->R, "[sup] msg 3/4 ANonce differs from msg 1 - dropped\r\n");
            return;
        }
        Rc = SendKey(S, KI_VER_HMAC_SHA1_AES | KI_PAIRWISE | KI_MIC | KI_SECURE, E + EK_REPLAY, NULL, NULL, 0);
        WaitTxIdle(S);
        Rc = (Rc << 8) | InstallKey(S, 0, S->Ptk + SUP_TK_OFF, S->Aa, NULL);
        if (GtkLen >= SUP_KEY_LEN) {
            Rc = (Rc << 8) | InstallKey(S, GtkIdx, Gtk, NULL, E + EK_RSC);
            S->GtkIdx = GtkIdx;
        }
        RtlSecureZeroMemory(Gtk, sizeof(Gtk));
        S->Hs         = HS_DONE;
        S->Authorized = TRUE;
        ReportLine(S->R, "[sup] msg 3/4 MIC ok, GTK idx %lu (%lu B) -> msg 4/4 sent, keys installed (codes 0x%06lX)\r\n",
                   GtkIdx, GtkLen, Rc);
        return;
    }
    if ((Info & (KI_PAIRWISE | KI_ACK)) == KI_ACK && GtkLen >= SUP_KEY_LEN) {
        /* group key msg 1/2 (rekey) */
        Rc = SendKey(S, KI_VER_HMAC_SHA1_AES | KI_MIC | KI_SECURE, E + EK_REPLAY, NULL, NULL, 0);
        Rc = (Rc << 8) | InstallKey(S, GtkIdx, Gtk, NULL, E + EK_RSC);
        RtlSecureZeroMemory(Gtk, sizeof(Gtk));
        S->GtkIdx = GtkIdx;
        S->Rekeys++;
        ReportLine(S->R, "[sup] group rekey #%lu: GTK idx %lu installed, msg 2/2 sent (codes 0x%04lX)\r\n", S->Rekeys, GtkIdx, Rc);
        return;
    }
    RtlSecureZeroMemory(Gtk, sizeof(Gtk));
    ReportLine(S->R, "[sup] EAPOL-Key info 0x%04X not handled\r\n", Info);
}

/* M7b join: the firmware associates (WPA2-PSK, CCMP, no in-dongle supplicant), the host runs the 4-way
   handshake over the data path. Order as fmac brcmf_cfg80211_connect (cfg80211.c:2440-2620) minus sup_wpa,
   plus the RSN IE of the association request set with "wpaie" (dhd wl_cfg80211.c:6244). */
_Use_decl_annotations_
ULONG
WlanMsgJoin(PWLAN_MSG_CTX M, PWLAN_REPORT R, const WLAN_JOIN_CONFIG *J)
{
    PMSG_STATE    S = Live(M, R);
    UCHAR         Mask[WL_EVENTING_MASK_LEN];
    UCHAR         Join[JOIN_PARAMS_SIZE];
    ULONG         One = 1, I, Ms, Result = JOIN_SETUP_FAILED;
    LARGE_INTEGER Delay;
    static const UCHAR Events[] = { WLC_E_SET_SSID, WLC_E_AUTH, WLC_E_DEAUTH_IND, WLC_E_ASSOC, WLC_E_DISASSOC_IND,
                                    WLC_E_LINK, WLC_E_PSK_SUP };

    if (S == NULL) {
        return JOIN_SETUP_FAILED;
    }
    if (!J->Valid) {
        return JOIN_NOT_CONFIGURED;
    }
    ReportLine(R, "---- milestone 7b: join \"%.*s\" (WPA2-PSK, host supplicant) ----\r\n", (int)J->SsidLen, J->Ssid);
    (VOID)SupSelfTest(R);
    RtlCopyMemory(S->Pmk, J->Pmk, PMK_LEN);
    RtlCopyMemory(S->TargetSsid, J->Ssid, J->SsidLen);
    S->TargetSsidLen = J->SsidLen;
    S->ApRsnLen      = 0;
    S->Hs            = HS_IDLE;
    S->PtkSet        = FALSE;
    S->ReplaySet     = FALSE;
    S->Authorized    = FALSE;
    (VOID)EscanRun(S);   /* records the target's BSSID + RSN IE (EscanResult) */
    if (S->ApRsnLen == 0) {
        ReportLine(R, "JOIN %u: \"%.*s\" not found with an RSN IE in the scan\r\n", JOIN_NOT_FOUND, (int)J->SsidLen, J->Ssid);
        return JOIN_NOT_FOUND;
    }
    ReportLine(R, "[join] target %02X:%02X:%02X:%02X:%02X:%02X rssi %ld, AP RSN IE %lu B\r\n", S->ApBssid[0], S->ApBssid[1],
               S->ApBssid[2], S->ApBssid[3], S->ApBssid[4], S->ApBssid[5], S->ApRssi, S->ApRsnLen);
    if (!BuildOwnRsn(S)) {
        ReportLine(R, "JOIN %u: AP security not supported (needs RSN v1, PSK, CCMP, no MFP)\r\n", JOIN_SETUP_FAILED);
        return JOIN_SETUP_FAILED;
    }
    /* rx audit (notes/m12-wifi-m7-rx-audit.md fix 1): DHD and brcmfmac post the rx data buffers BEFORE the
       firmware is brought up; ours were posted after WLC_UP (run: fw r 128, rx cpl w 0). Bounce the interface so
       the firmware's UP sees the posted buffers. */
    {
        ULONG Zero = 0;
        ULONG Rd = Ioctl(S, WLC_DOWN, &Zero, sizeof(Zero), 0, "DOWN");
        ULONG Ru = Ioctl(S, WLC_UP, &One, sizeof(One), 0, "UP");

        ReportLine(R, "[join] interface bounce: DOWN %lu UP %lu (rxpost host w %lu fw r %u)\r\n", Rd, Ru,
                   S->Ring[RING_H2D_RXPOST].Host, Tcm16(S, S->Ring[RING_H2D_RXPOST].RIdxAddr));
        if (IoctlVar(S, FALSE, "rxbufpost_sz", NULL, 0, sizeof(ULONG)) == MSG_OK && S->RespLen >= 2) {
            ReportLine(R, "[join] firmware rxbufpost_sz %u (our rx buffers are %u B)\r\n",
                       *(USHORT UNALIGNED *)S->Resp, DATA_BUF_SIZE);
        }
    }
    S->Joining     = TRUE;
    S->JoinSetSsid = -1;
    S->JoinPskSup  = -1;
    S->JoinLinkUp  = FALSE;
    S->JoinLost    = FALSE;

    if (IoctlVar(S, FALSE, "event_msgs", NULL, 0, WL_EVENTING_MASK_LEN) != MSG_OK || S->RespLen < WL_EVENTING_MASK_LEN) {
        goto Done;
    }
    RtlCopyMemory(Mask, S->Resp, WL_EVENTING_MASK_LEN);
    for (I = 0; I < ARRAYSIZE(Events); I++) {
        Mask[Events[I] / 8] |= (UCHAR)(1 << (Events[I] % 8));
    }
    if (IoctlVar(S, TRUE, "event_msgs", Mask, WL_EVENTING_MASK_LEN, 0) != MSG_OK ||
        Ioctl(S, WLC_SET_INFRA, &One, sizeof(One), 0, "SET_INFRA") != MSG_OK ||
        SetInt(S, "wpa_auth", WPA2_AUTH_PSK) != MSG_OK ||
        SetInt(S, "auth", WL_AUTH_OPEN_SYSTEM) != MSG_OK ||
        SetInt(S, "wsec", AES_ENABLED) != MSG_OK) {
        ReportLine(R, "[join] security setup refused (last status %ld)\r\n", S->RespStatus);
        goto Done;
    }
    if (IoctlVar(S, TRUE, "wpaie", S->OwnRsn, S->OwnRsnLen, 0) != MSG_OK) {
        ReportLine(R, "[join] wpaie refused (status %ld) - the firmware builds its own RSN IE\r\n", S->RespStatus);
    }
    RtlCopyMemory(S->Aa, S->ApBssid, 6);
    RtlZeroMemory(Join, sizeof(Join));
    *(ULONG UNALIGNED *)Join = J->SsidLen;
    RtlCopyMemory(Join + 4, J->Ssid, J->SsidLen);
    if (Ioctl(S, WLC_SET_SSID, Join, sizeof(Join), 0, "SET_SSID") != MSG_OK) {
        ReportLine(R, "[join] SET_SSID refused (status %ld)\r\n", S->RespStatus);
        goto Done;
    }
    Result = JOIN_TIMEOUT;
    for (Ms = 0; Ms < JOIN_TIMEOUT_MS; Ms += 10) {
        (VOID)D2hPollAll(S);
        if (S->JoinSetSsid == 0 && !S->FlowRequested) {
            /* tx flowring once associated (dhd creates it on the first tx after the link is up); a msg 1 that
               arrived meanwhile stays pending and is answered below */
            if (!FlowRingCreate(S, S->ApBssid)) {
                ReportLine(R, "[join] no tx flowring - the handshake cannot be sent\r\n");
                Result = JOIN_SETUP_FAILED;
                break;
            }
        }
        if (S->EapolPending && S->FlowOpen) {
            SupEapol(S);
        }
        if (S->JoinSetSsid > 0 || S->JoinLost) {
            Result = JOIN_FW_FAILED;
            break;
        }
        if (S->Hs == HS_FAILED) {
            Result = JOIN_HANDSHAKE_FAILED;
            break;
        }
        if (S->Hs == HS_DONE) {
            Result = JOIN_OK;
            break;
        }
        Delay.QuadPart = -10000LL * 10;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    ReportLine(R, "JOIN %lu: SET_SSID %ld, handshake state %lu, link %s%s, eapol rx %lu tx %lu after %lu ms\r\n", Result,
               S->JoinSetSsid, S->Hs, S->JoinLinkUp ? "up" : "not up", S->JoinLost ? ", lost" : "", S->RxEapol, S->TxPosted, Ms);
    /* run w7b: no EAPOL arrived - show where the rx path stands and what the firmware says */
    ReportLine(R, "[join] rings: rxpost host w %lu fw r %u | rx cpl fw w %u host r %lu | tx cpl fw w %u | rx frames %lu bad %lu, "
               "tx posted %lu done %lu\r\n", S->Ring[RING_H2D_RXPOST].Host, Tcm16(S, S->Ring[RING_H2D_RXPOST].RIdxAddr),
               Tcm16(S, S->Ring[RING_D2H_RX_CPL].WIdxAddr), S->Ring[RING_D2H_RX_CPL].Host,
               Tcm16(S, S->Ring[RING_D2H_TX_CPL].WIdxAddr), S->RxFrames, S->RxBad, S->TxPosted, S->TxDone);
    if (Result != JOIN_OK) {
        WlanBusConsoleDump(R, S->Bus);
    }
Done:
    S->Joining = FALSE;
    if (Result != JOIN_OK) {
        RtlSecureZeroMemory(S->Ptk, sizeof(S->Ptk));
        S->PtkSet = FALSE;
    }
    if (Result == JOIN_SETUP_FAILED) {
        ReportLine(R, "JOIN %u: setup failed\r\n", JOIN_SETUP_FAILED);
    }
    return Result;
}
