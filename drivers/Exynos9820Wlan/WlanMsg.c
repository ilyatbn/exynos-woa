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
#include "WlanDl.h"
#include "WlanMsg.h"

typedef struct _MSG_RING {
    ULONG   Id;
    ULONG   ItemSize;
    ULONG   MaxItem;
    ULONG   Off;            /* offset in the common buffer */
    ULONG   WIdxAddr;       /* TCM address of the write index slot */
    ULONG   RIdxAddr;       /* TCM address of the read index slot */
    ULONG   Host;           /* host-side index: write index (H2D) / read index (D2H) */
    ULONG   Seq;            /* epoch sequence */
} MSG_RING, *PMSG_RING;

typedef struct _MSG_STATE {
    PWLAN_REPORT        R;
    PWLAN_BUS           Bus;
    PUCHAR              Va;
    ULONGLONG           La;
    MSG_RING            Ring[RING_COMMON_COUNT];
    ULONG               Db0;
    ULONG               Db1;
    UCHAR               Resp[256];
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
} MSG_STATE, *PMSG_STATE;

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

static PUCHAR
RingItem(_In_ PMSG_STATE S, _In_ PMSG_RING G, _In_ ULONG Index)
{
    return S->Va + G->Off + Index * G->ItemSize;
}

/* Fill the next H2D control item (40 B) and advance the host write index (written to TCM by the caller).
   NULL when the ring stays full for 100 ms (the firmware has not consumed what was posted). */
static PUCHAR
H2dCtrlNext(_Inout_ PMSG_STATE S, _In_ UCHAR Type, _In_ ULONG RequestId)
{
    PMSG_RING G = &S->Ring[RING_H2D_CTRL];
    PUCHAR    It;
    ULONG     Tries;

    for (Tries = 0; ((G->Host + 1) % G->MaxItem) == Tcm16(S, G->RIdxAddr); Tries++) {
        if (Tries == 100) {
            ReportLine(S->R, "[msg] H2D ctrl ring full (w %lu r %u)\r\n", G->Host, Tcm16(S, G->RIdxAddr));
            return NULL;
        }
        KeStallExecutionProcessor(1000);
    }
    It = RingItem(S, G, G->Host);
    RtlZeroMemory(It, G->ItemSize);
    It[MSGH_TYPE]  = Type;
    It[MSGH_IFID]  = 0;
    It[MSGH_FLAGS] = 0;   /* no phase bit, as fmac brcmf_msgbuf_tx_ioctl: wrapping needs only the full check */
    It[MSGH_EPOCH] = (UCHAR)(G->Seq % EPOCH_MODULO);
    G->Seq++;
    *(ULONG UNALIGNED *)(It + MSGH_REQUEST_ID) = RequestId;
    G->Host = (G->Host + 1) % G->MaxItem;
    return It;
}

static VOID
H2dCtrlCommit(_Inout_ PMSG_STATE S)
{
    PMSG_RING G = &S->Ring[RING_H2D_CTRL];

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

/* Drain the D2H control-complete ring once; returns the number of items consumed. */
static ULONG
D2hCtrlPoll(_Inout_ PMSG_STATE S)
{
    PMSG_RING G = &S->Ring[RING_D2H_CTRL_CPL];
    ULONG     W, Count = 0, Tries;
    BOOLEAN   Repost = FALSE;

    KeMemoryBarrier();
    W = Tcm16(S, G->WIdxAddr);
    if (W >= G->MaxItem) {
        ReportLine(S->R, "[msg] D2H ctrl write index %lu out of range\r\n", W);
        return 0;
    }
    while (G->Host != W) {
        PUCHAR It = RingItem(S, G, G->Host);
        UCHAR  Type;

        for (Tries = 0; Tries < 100 && !D2hItemReady(S, G, It); Tries++) {
            KeStallExecutionProcessor(10);
        }
        if (Tries == 100) {
            ReportLine(S->R, "[msg] D2H item %lu not synced (epoch %u want %lu)\r\n", G->Host, It[MSGH_EPOCH],
                       G->Seq % EPOCH_MODULO);
            break;
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
        case MSG_TYPE_GEN_STATUS:
        case MSG_TYPE_RING_STATUS:
            ReportLine(S->R, "[msg] %s status %d ring %u\r\n", (Type == MSG_TYPE_GEN_STATUS) ? "GEN" : "RING",
                       *(SHORT UNALIGNED *)(It + CPL_STATUS), *(USHORT UNALIGNED *)(It + CPL_RING_ID));
            break;
        default:
            ReportLine(S->R, "[msg] D2H ctrl item type 0x%02X\r\n", Type);
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
    return Count;
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
    UCHAR  Buf[128];
    size_t NameLen;

    if (!NT_SUCCESS(RtlStringCbLengthA(Name, 64, &NameLen)) || NameLen + 1 + ValLen > sizeof(Buf)) {
        return MSG_IOCTL_ERROR;
    }
    RtlCopyMemory(Buf, Name, NameLen + 1);
    if (ValLen != 0) {
        RtlCopyMemory(Buf + NameLen + 1, Val, ValLen);
    }
    return Ioctl(S, Set ? WLC_SET_VAR : WLC_GET_VAR, Buf, (ULONG)NameLen + 1 + ValLen, OutLen, Name);
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
        ULONG         One = 1, Ms;
        UCHAR         Esc[ESCAN_PARAMS_V2_SIZE];
        ULONG         EscLen;
        BOOLEAN       V2;
        ULONG         Major;
        LARGE_INTEGER Delay;

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
        if (IoctlVar(S, TRUE, "event_msgs", Mask, WL_EVENTING_MASK_LEN, 0) != MSG_OK ||
            Ioctl(S, WLC_UP, &One, sizeof(One), 0, "UP") != MSG_OK) {
            goto ScanEnd;
        }
        /* escan params version, dhd rule for this firmware (wl_cfg80211.c:18501-18511): v2 only when
           "wlc_ver" major >= 14 (wl_cfg80211.h:833) AND "scan_ver" answers; fmac uses scan_ver alone
           (feature.c:342). Run m4-1: v2 on scan_ver alone -> escan BCME_EPERM (-26). If the chosen version is
           refused, the other one is tried once so one run answers it. */
        Major = 0;
        if (IoctlVar(S, FALSE, "wlc_ver", NULL, 0, WLC_VER_SIZE) == MSG_OK && S->RespLen >= WLC_VER_SIZE) {
            Major = *(USHORT UNALIGNED *)(S->Resp + WLC_VER_MAJOR);
            ReportLine(R, "[scan] wlc_ver: epi %u.%u.%u.%u wlc %u.%u\r\n", *(USHORT UNALIGNED *)(S->Resp + 4),
                       *(USHORT UNALIGNED *)(S->Resp + 6), *(USHORT UNALIGNED *)(S->Resp + 8),
                       *(USHORT UNALIGNED *)(S->Resp + 10), Major, *(USHORT UNALIGNED *)(S->Resp + WLC_VER_MAJOR + 2));
        }
        V2 = (IoctlVar(S, FALSE, "scan_ver", NULL, 0, sizeof(ULONG)) == MSG_OK);
        ReportLine(R, "[scan] scan_ver %s, wlc major %lu (dhd min %u) -> escan params v%u\r\n",
                   V2 ? "answered" : "failed", Major, MIN_ESCAN_PARAM_V2_FW_MAJOR,
                   (V2 && Major >= MIN_ESCAN_PARAM_V2_FW_MAJOR) ? 2 : 1);
        V2 = V2 && (Major >= MIN_ESCAN_PARAM_V2_FW_MAJOR);
        EscLen = EscanBuild(Esc, V2);
        if (IoctlVar(S, TRUE, "escan", Esc, EscLen, 0) != MSG_OK) {
            ReportLine(R, "[scan] escan v%u refused (status %ld) - trying v%u\r\n", V2 ? 2 : 1, S->RespStatus, V2 ? 1 : 2);
            V2 = !V2;
            EscLen = EscanBuild(Esc, V2);
            if (IoctlVar(S, TRUE, "escan", Esc, EscLen, 0) != MSG_OK) {
                ReportLine(R, "[scan] escan v%u refused too (status %ld)\r\n", V2 ? 2 : 1, S->RespStatus);
                goto ScanEnd;
            }
        }
        ReportLine(R, "[scan] escan v%u accepted\r\n", V2 ? 2 : 1);
        for (Ms = 0; Ms < SCAN_TIMEOUT_MS && !S->ScanDone; Ms += 10) {
            (VOID)D2hCtrlPoll(S);
            Delay.QuadPart = -10000LL * 10;
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        }
        M->BssCount = S->BssCount;
        if (S->ScanDone && S->ScanStatus == 0) {
            M->ScanResult = SCAN_OK;
        } else if (!S->ScanDone) {
            M->ScanResult = SCAN_TIMEOUT;
        } else {
            M->ScanResult = SCAN_FW_STATUS;   /* final escan event with a non-success status */
        }
        ReportLine(R, "SCAN %lu: %lu BSS from %lu escan events (%s, status %lu) after %lu ms; %lu events total\r\n",
                   M->ScanResult, S->BssCount, S->ScanEvents, S->ScanDone ? "done" : "not done", S->ScanStatus, Ms,
                   S->Events);
ScanEnd:
        if (M->ScanResult == SCAN_FAILED) {
            ReportLine(R, "SCAN %u: setup ioctl failed\r\n", SCAN_FAILED);
        }
        if ((Tcm32(S, Shared + SH_FLAGS) & PCIE_SHARED_TRAP) != 0) {
            ReportLine(R, "[scan] firmware TRAP flag set (trap 0x%08lX)\r\n", Tcm32(S, Shared + SH_TRAP_ADDR));
        }
        WlanBusConsoleDump(R, Bus);
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
        ExFreePoolWithTag(S, 'mDlW');
    }
    ReportWriteFile(R);
}
