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

/* Fill the next H2D control item (40 B) and advance the host write index (written to TCM by the caller). */
static PUCHAR
H2dCtrlNext(_Inout_ PMSG_STATE S, _In_ UCHAR Type, _In_ ULONG RequestId)
{
    PMSG_RING G = &S->Ring[RING_H2D_CTRL];
    PUCHAR    It = RingItem(S, G, G->Host);

    RtlZeroMemory(It, G->ItemSize);
    It[MSGH_TYPE]  = Type;
    It[MSGH_IFID]  = 0;
    It[MSGH_FLAGS] = 0;   /* current_phase 0: the ring never wraps in milestone 3 */
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
            if (Pkt >= 0x1000 && Pkt < 0x1000 + IOCTL_RESP_BUF_COUNT) {
                ULONG Copy = (Len < sizeof(S->Resp)) ? Len : sizeof(S->Resp);

                RtlCopyMemory(S->Resp, S->Va + MSG_OFF_IOCTL_RESP + (Pkt - 0x1000) * IOCTL_RESP_BUF_SIZE, Copy);
                S->RespLen = Copy;
            }
            S->Completed = TRUE;
            break;
        }
        case MSG_TYPE_GEN_STATUS:
        case MSG_TYPE_RING_STATUS:
            ReportLine(S->R, "[msg] %s status %d ring %u\r\n", (Type == MSG_TYPE_GEN_STATUS) ? "GEN" : "RING",
                       *(SHORT UNALIGNED *)(It + CPL_STATUS), *(USHORT UNALIGNED *)(It + CPL_RING_ID));
            break;
        case MSG_TYPE_WL_EVENT:
            ReportLine(S->R, "[msg] WL event (buffer not posted)\r\n");
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
    return Count;
}

/* GET_VAR <Name>: request buffer = name + NUL, response up to OutLen bytes. */
static ULONG
IoctlGetVar(_Inout_ PMSG_STATE S, _In_z_ PCSTR Name, _In_ ULONG OutLen, _In_ USHORT TransId)
{
    PUCHAR        Req = S->Va + MSG_OFF_IOCTL_REQ;
    PUCHAR        It;
    size_t        NameLen;
    ULONG         InLen, Ms;
    LARGE_INTEGER Delay;

    if (!NT_SUCCESS(RtlStringCbLengthA(Name, 64, &NameLen))) {
        return MSG_IOCTL_ERROR;
    }
    InLen = (ULONG)NameLen + 1;
    if (InLen < OutLen) {
        InLen = OutLen;   /* the buffer carries the name in and the value out */
    }
    if (InLen > IOCTL_REQ_MAX_INPUT) {
        InLen = IOCTL_REQ_MAX_INPUT;
    }
    RtlZeroMemory(Req, IOCTL_REQ_BUF_SIZE);
    RtlCopyMemory(Req, Name, NameLen + 1);

    S->Acked = S->Completed = FALSE;
    S->RespLen = 0;
    It = H2dCtrlNext(S, MSG_TYPE_IOCTLPTR_REQ, IOCTL_REQ_PKTID);
    *(ULONG UNALIGNED *)(It + IOREQ_CMD)         = WLC_GET_VAR;
    *(USHORT UNALIGNED *)(It + IOREQ_TRANS_ID)   = TransId;
    *(USHORT UNALIGNED *)(It + IOREQ_INPUT_LEN)  = (USHORT)InLen;
    *(USHORT UNALIGNED *)(It + IOREQ_OUTPUT_LEN) = (USHORT)OutLen;
    *(ULONG UNALIGNED *)(It + IOREQ_BUF_ADDR)     = (ULONG)(S->La + MSG_OFF_IOCTL_REQ);
    *(ULONG UNALIGNED *)(It + IOREQ_BUF_ADDR + 4) = (ULONG)((S->La + MSG_OFF_IOCTL_REQ) >> 32);
    H2dCtrlCommit(S);
    ReportLine(S->R, "[msg] GET_VAR \"%s\" sent (in %lu out %lu trans %u), H2D ctrl w %lu r %u\r\n", Name, InLen, OutLen,
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
        ReportLine(S->R, "[msg] GET_VAR \"%s\": no completion in %u ms (acked %u; H2D ctrl r %u, D2H ctrl w %u)\r\n",
                   Name, IOCTL_RESP_TIMEOUT_MS, S->Acked, Tcm16(S, S->Ring[RING_H2D_CTRL].RIdxAddr),
                   Tcm16(S, S->Ring[RING_D2H_CTRL_CPL].WIdxAddr));
        return MSG_TIMEOUT;
    }
    return (S->RespStatus == 0) ? MSG_OK : MSG_IOCTL_ERROR;
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
        PUCHAR    It = H2dCtrlNext(S, MSG_TYPE_IOCTLRESP_BUF_POST, 0x1000 + I);
        ULONGLONG Ba = S->La + MSG_OFF_IOCTL_RESP + (ULONGLONG)I * IOCTL_RESP_BUF_SIZE;

        *(USHORT UNALIGNED *)(It + BUFPOST_LEN)     = (USHORT)IOCTL_RESP_BUF_SIZE;
        *(ULONG UNALIGNED *)(It + BUFPOST_ADDR)     = (ULONG)Ba;
        *(ULONG UNALIGNED *)(It + BUFPOST_ADDR + 4) = (ULONG)(Ba >> 32);
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
    M->Result = IoctlGetVar(S, "cur_etheraddr", 32, 1);
    if (M->Result == MSG_OK && S->RespLen >= 6) {
        RtlCopyMemory(M->Mac, S->Resp, 6);
        M->MacValid = TRUE;
        ReportLine(R, "[msg] cur_etheraddr = %02X:%02X:%02X:%02X:%02X:%02X\r\n", S->Resp[0], S->Resp[1], S->Resp[2],
                   S->Resp[3], S->Resp[4], S->Resp[5]);
    }
    if (M->Result == MSG_OK) {
        M->Result = IoctlGetVar(S, "ver", 200, 2);
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
