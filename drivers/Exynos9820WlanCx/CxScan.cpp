/*  CxScan.cpp - Exynos9820WlanCx W2: WDI_TASK_SCAN -> firmware escan -> WDI_INDICATION_BSS_ENTRY_LIST ->
    WDI_INDICATION_SCAN_COMPLETE (notes/m12-wifi-wificx.md).

    The firmware reports wl_bss_info_v109 records (dhd wlioctl.h:384-424): BSSID @8, beacon_period @14, capability
    @16, chanspec @72, RSSI @78, ctl_ch @88, ie_offset @116, ie_length @120 (Exynos9820Wlan.h BSS_*). WDI wants each
    BSS as a WDI_BSS_ENTRY_CONTAINER with the probe response / beacon frame body: 802.11 management body = timestamp
    (8) + beacon interval (2) + capability (2) + IEs (802.11-2016 9.3.3.3/9.3.3.11). The firmware does not hand us
    the timestamp, so it is sent as 0.
*/

#include "Cx.h"

#define CX_FIXED_BODY   12      /* timestamp 8 + beacon interval 2 + capability 2 */

_Use_decl_annotations_
PCX_BSS
CxFindBss(PCX_DEVICE Dev, const UCHAR *Bssid)
{
    ULONG I;

    for (I = 0; I < Dev->BssN; I++) {
        if (RtlCompareMemory(Dev->Bss[I].Bssid, Bssid, 6) == 6) {
            return &Dev->Bss[I];
        }
    }
    return NULL;
}

/* WLAN_BSS_FN, from WlanMsg.c EscanResult on the worker thread: upsert by BSSID (latest record wins). */
_Use_decl_annotations_
VOID
CxBssRecord(PVOID Ctx, const UCHAR *B, ULONG Len)
{
    PCX_DEVICE Dev = (PCX_DEVICE)Ctx;
    PCX_BSS    E;
    ULONG      IeOff, IeLen;

    if (Dev == NULL || Dev->Bss == NULL || Len < BSS_INFO_MIN_LEN) {
        return;
    }
    Dev->BssRecords++;
    E = CxFindBss(Dev, B + BSS_BSSID);
    if (E == NULL) {
        if (Dev->BssN >= CX_MAX_BSS) {
            return;
        }
        E = &Dev->Bss[Dev->BssN++];
    }
    RtlCopyMemory(E->Bssid, B + BSS_BSSID, 6);
    E->BeaconPeriod = *(const USHORT UNALIGNED *)(B + BSS_BEACON_PERIOD);
    E->Capability   = *(const USHORT UNALIGNED *)(B + BSS_CAPABILITY);
    E->Chanspec     = *(const USHORT UNALIGNED *)(B + BSS_CHANSPEC);
    E->Rssi         = *(const SHORT UNALIGNED *)(B + BSS_RSSI);
    E->Channel      = B[BSS_CTL_CH];
    IeOff = *(const USHORT UNALIGNED *)(B + BSS_IE_OFFSET);
    IeLen = *(const ULONG UNALIGNED *)(B + BSS_IE_LENGTH);
    E->IesLen = 0;
    E->Cut    = FALSE;
    if (IeOff >= BSS_INFO_MIN_LEN && IeOff <= Len && IeLen <= Len - IeOff) {
        if (IeLen > CX_BSS_IES_MAX) {
            IeLen  = CX_BSS_IES_MAX;
            E->Cut = TRUE;
        }
        RtlCopyMemory(E->Ies, B + IeOff, IeLen);
        E->IesLen = IeLen;
    }
}

/* RSSI (dBm) -> WDI link quality 0..100: linear between -100 dBm (0) and -50 dBm (100). Host choice (no WDI rule). */
static UINT32
CxQuality(LONG Rssi)
{
    if (Rssi <= -100) {
        return 0;
    }
    if (Rssi >= -50) {
        return 100;
    }
    return (UINT32)(2 * (Rssi + 100));
}

/* One BSS_ENTRY_LIST indication for entries [First, First + N). The frame bodies are built in Body (N x
   (CX_FIXED_BODY + CX_BSS_IES_MAX)). */
static VOID
CxIndicateBss(PCX_DEVICE Dev, ULONG First, ULONG N, PUCHAR Body, ULONGLONG Now)
{
    WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS P;
    WDI_BSS_ENTRY_CONTAINER                 *E;
    UINT8                                   *Out = NULL;
    ULONG                                    OutLen = 0, I;
    NDIS_STATUS                              Ns;

    E = new WDI_BSS_ENTRY_CONTAINER[CX_BSS_PER_INDICATION];
    if (E == NULL) {
        CxLog(Dev, "[scan] no memory for %lu BSS entries\r\n", N);
        return;
    }
    for (I = 0; I < N; I++) {
        PCX_BSS B = &Dev->Bss[First + I];
        PUCHAR  F = Body + I * (CX_FIXED_BODY + CX_BSS_IES_MAX);

        RtlZeroMemory(F, CX_FIXED_BODY);
        *(USHORT UNALIGNED *)(F + 8)  = B->BeaconPeriod;
        *(USHORT UNALIGNED *)(F + 10) = B->Capability;
        RtlCopyMemory(F + CX_FIXED_BODY, B->Ies, B->IesLen);
        RtlCopyMemory(E[I].BSSID.Address, B->Bssid, 6);
        E[I].Optional.ProbeResponseFrame_IsPresent = 1;
        E[I].ProbeResponseFrame.SimpleAssign(F, CX_FIXED_BODY + B->IesLen);
        E[I].SignalInfo.RSSI        = B->Rssi;
        E[I].SignalInfo.LinkQuality = CxQuality(B->Rssi);
        E[I].ChannelInfo.ChannelNumber = B->Channel;
        /* 2.4 GHz channels are 1..14, 5 GHz ones >= 32 (802.11-2016 Annex E); the BCM4375 has no 6 GHz radio */
        E[I].ChannelInfo.BandId = (B->Channel <= 14) ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
        E[I].Optional.EntryAgeInfo_IsPresent = 1;
        E[I].EntryAgeInfo.HostTimeStamp      = Now;
        E[I].EntryAgeInfo.CachedInformation  = FALSE;
    }
    P.Optional.DeviceDescriptor_IsPresent = 1;
    P.DeviceDescriptor.SimpleAssign(E, N);
    Ns = GenerateWdiIndicationBssEntryList(&P, 0, &Dev->Tlv, &OutLen, &Out);
    if (Ns == NDIS_STATUS_SUCCESS) {
        CxIndicate(Dev, WDI_INDICATION_BSS_ENTRY_LIST, NULL, NDIS_STATUS_SUCCESS, Out, OutLen);
        FreeGenerated(Out);
    } else {
        CxLog(Dev, "[scan] BSS list generation failed 0x%08lX\r\n", Ns);
    }
    P.DeviceDescriptor.SimpleAssign(NULL, 0);   /* E is ours: do not let the parameter destructor touch it */
    for (I = 0; I < N; I++) {
        E[I].ProbeResponseFrame.SimpleAssign(NULL, 0);
    }
    delete[] E;
}

_Use_decl_annotations_
VOID
CxTaskScan(PCX_DEVICE Dev, PCX_CMD Cmd, const WDI_MESSAGE_HEADER *H, const UINT8 *Tlv, ULONG TlvLen)
{
    WDI_SCAN_PARAMETERS P;
    WDI_MESSAGE_HEADER  Copy = *H;
    NDIS_STATUS         Ns = ParseWdiTaskScan(TlvLen, Tlv, &Dev->Tlv, &P);
    ULONG               Rc, I, Ssids;
    PUCHAR              Body;
    LARGE_INTEGER       Now;

    if (Ns != NDIS_STATUS_SUCCESS) {
        CxM3(Dev, Cmd, STATUS_INVALID_PARAMETER);
        return;
    }
    Ssids = P.SSIDList.ElementCount;
    CleanupParsedWdiTaskScan(&P);
    if (Dev->Bss == NULL) {
        CxM3(Dev, Cmd, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    CxM3(Dev, Cmd, STATUS_SUCCESS);
    /* broadcast escan only: a directed probe for the host's SSID list (hidden networks) is not implemented */
    Dev->BssN       = 0;
    Dev->BssRecords = 0;
    Rc = WlanMsgRescan(&Dev->Msg, Dev->RunR);
    KeQuerySystemTimePrecise(&Now);
    CxLog(Dev, "[scan] WDI scan (%lu SSIDs requested, broadcast used): SCAN %lu, %lu records -> %lu BSS\r\n", Ssids, Rc,
          Dev->BssRecords, Dev->BssN);
    for (I = 0; I < Dev->BssN; I++) {
        PCX_BSS B = &Dev->Bss[I];

        CxLog(Dev, "[scan]   %02X:%02X:%02X:%02X:%02X:%02X ch %3u rssi %4d cap 0x%04X ies %lu%s\r\n", B->Bssid[0],
              B->Bssid[1], B->Bssid[2], B->Bssid[3], B->Bssid[4], B->Bssid[5], B->Channel, B->Rssi, B->Capability,
              B->IesLen, B->Cut ? " (cut)" : "");
    }
    Body = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, CX_BSS_PER_INDICATION * (CX_FIXED_BODY + CX_BSS_IES_MAX),
                                   CX_POOL_TAG);
    if (Body != NULL) {
        for (I = 0; I < Dev->BssN; I += CX_BSS_PER_INDICATION) {
            CxIndicateBss(Dev, I, min(CX_BSS_PER_INDICATION, Dev->BssN - I), Body, (ULONGLONG)Now.QuadPart);
        }
        ExFreePoolWithTag(Body, CX_POOL_TAG);
    }
    CxIndicate(Dev, WDI_INDICATION_SCAN_COMPLETE, &Copy,
               (Rc == SCAN_OK || Dev->BssN != 0) ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE, NULL, 0);
}
