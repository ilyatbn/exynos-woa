/*  CxWifi.cpp - Exynos9820WlanCx: WiFiCx capabilities and the WDI command dispatcher (notes/m12-wifi-wificx.md).

    Message flow (learn.microsoft.com "Develop a WiFiCx client driver", WiFiCx sample wifirequest.cpp:34-97):
    EvtWifiDeviceSendCommand -> (queue) -> worker: WifiRequestGetInOutBuffer / GetMessageId, M3 =
    WifiRequestComplete; a task then sends its completion indication (M4) with WifiDeviceReceiveIndication and the
    request's TransactionId; unsolicited indications carry TransactionId 0.
*/

#include "Cx.h"

/* ---------------------------------------------------------------- TLV dump into the run log */

static ULONG CxDumps;
#define CX_DUMP_MAX     400     /* host choice: commands TLV-dumped per driver load (the run log rotates anyway) */

static void __stdcall
CxTlvDump(UINT_PTR Context, PCSTR Fmt, ...)
{
    PCX_DEVICE Dev = (PCX_DEVICE)Context;
    CHAR       Line[400];
    size_t     N = 0;
    va_list    Args;

    va_start(Args, Fmt);
    Line[0] = 0;
    (VOID)RtlStringCbVPrintfA(Line, sizeof(Line), Fmt, Args);
    va_end(Args);
    (VOID)RtlStringCbLengthA(Line, sizeof(Line), &N);
    while (N != 0 && (Line[N - 1] == '\n' || Line[N - 1] == '\r')) {
        Line[--N] = 0;
    }
    CxLog(Dev, "[tlv] %s\r\n", Line);
}

/* ---------------------------------------------------------------- capabilities */

/* IEEE 802.11 channel -> centre frequency: 2.4 GHz f = 2407 + 5 ch (ch 1..13), 5 GHz f = 5000 + 5 ch (802.11-2016
   19.3.15 / 17.3.8.4.2). Channel sets = the 20 MHz channels of the two bands the BCM4375 (dual-band 11ax) uses; the
   firmware's CLM (regulatory blob, ClmLoad) decides what it really transmits on. */
static const UCHAR CxCh24[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
static const UCHAR CxCh5[]  = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
                                144, 149, 153, 157, 161, 165 };

_Use_decl_annotations_
NTSTATUS
CxSetCapabilities(PCX_DEVICE Dev)
{
    WIFI_DEVICE_CAPABILITIES   DevCaps;
    WIFI_STATION_CAPABILITIES  Sta;
    WIFI_BAND_CAPABILITIES     Bands;
    WIFI_BAND_INFO             BandInfo[2];
    WIFI_PHY_CAPABILITIES      Phys;
    WIFI_PHY_INFO              PhyInfo[5];
    WDI_CHANNEL_MAPPING_ENTRY  Map24[ARRAYSIZE(CxCh24)];
    WDI_CHANNEL_MAPPING_ENTRY  Map5[ARRAYSIZE(CxCh5)];
    WDI_PHY_TYPE               Phy24[] = { WDI_PHY_TYPE_HRDSSS, WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT };
    WDI_PHY_TYPE               Phy5[]  = { WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT, WDI_PHY_TYPE_VHT };
    UINT32                     Width20 = 20;
    /* WPA2-Personal (CCMP) and open only: the OS supplicant runs the handshake; no WEP/TKIP/SAE/MFP claimed. */
    DOT11_AUTH_CIPHER_PAIR     Unicast[] = { { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE },
                                             { DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP } };
    DOT11_AUTH_CIPHER_PAIR     McastMgmt[] = { { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE } };
    /* legacy rates in 500 kbit/s units (WiFiCx sample wifihal.cpp:470-476 uses the same unit) */
    static const UINT16        RatesB[]    = { 2, 4, 11, 22 };
    static const UINT16        RatesOfdm[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
    ULONG                      I, K;
    NTSTATUS                   Status;
    static const struct { WDI_PHY_TYPE Type; BOOLEAN B; BOOLEAN Ofdm; } PhyRates[5] = {
        { WDI_PHY_TYPE_HRDSSS, TRUE, FALSE }, { WDI_PHY_TYPE_ERP, TRUE, TRUE }, { WDI_PHY_TYPE_OFDM, FALSE, TRUE },
        { WDI_PHY_TYPE_HT, FALSE, TRUE }, { WDI_PHY_TYPE_VHT, FALSE, TRUE } };

    WIFI_DEVICE_CAPABILITIES_INIT(&DevCaps);
    DevCaps.HardwareRadioState = TRUE;
    DevCaps.SoftwareRadioState = TRUE;
    (VOID)RtlStringCbCopyA((NTSTRSAFE_PSTR)DevCaps.FirmwareVersion, sizeof(DevCaps.FirmwareVersion), "BCM4375 fw (see run log)");
    DevCaps.ActionFramesSupported = FALSE;
    DevCaps.NumRxStreams = 1;      /* conservative: no MIMO claim made from this driver (UNVERIFIED chain count) */
    DevCaps.NumTxStreams = 1;
    DevCaps.BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_UNKNOWN;
    Status = WifiDeviceSetDeviceCapabilities(Dev->Device, &DevCaps);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WIFI_STATION_CAPABILITIES_INIT(&Sta);
    Sta.ScanSSIDListSize          = 1;     /* broadcast escan only (no directed probe for hidden SSIDs yet) */
    Sta.DesiredSSIDListSize       = 1;
    Sta.PrivacyExemptionListSize  = 1;
    Sta.KeyMappingTableSize       = 4;
    Sta.DefaultKeyTableSize       = 4;
    Sta.WEPKeyValueMaxLength      = 13;
    Sta.MaxNumPerSTA              = 4;
    Sta.NumSupportedUnicastAlgorithms       = ARRAYSIZE(Unicast);
    Sta.UnicastAlgorithmsList               = Unicast;
    Sta.NumSupportedMulticastDataAlgorithms = ARRAYSIZE(Unicast);
    Sta.MulticastDataAlgorithmsList         = Unicast;
    Sta.NumSupportedMulticastMgmtAlgorithms = ARRAYSIZE(McastMgmt);
    Sta.MulticastMgmtAlgorithmsList         = McastMgmt;
    Status = WifiDeviceSetStationCapabilities(Dev->Device, &Sta);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    for (I = 0; I < ARRAYSIZE(CxCh24); I++) {
        Map24[I].ChannelNumber          = CxCh24[I];
        Map24[I].ChannelCenterFrequency = 2407 + 5 * CxCh24[I];
    }
    for (I = 0; I < ARRAYSIZE(CxCh5); I++) {
        Map5[I].ChannelNumber          = CxCh5[I];
        Map5[I].ChannelCenterFrequency = 5000 + 5 * CxCh5[I];
    }
    RtlZeroMemory(BandInfo, sizeof(BandInfo));
    BandInfo[0].BandID              = WDI_BAND_ID_2400;
    BandInfo[0].BandState           = TRUE;
    BandInfo[0].NumValidPhyTypes    = ARRAYSIZE(Phy24);
    BandInfo[0].ValidPhyTypeList    = Phy24;
    BandInfo[0].NumValidChannelTypes = ARRAYSIZE(Map24);
    BandInfo[0].ValidChannelTypes   = Map24;
    BandInfo[0].NumChannelWidths    = 1;
    BandInfo[0].ChannelWidthList    = &Width20;
    BandInfo[1].BandID              = WDI_BAND_ID_5000;
    BandInfo[1].BandState           = TRUE;
    BandInfo[1].NumValidPhyTypes    = ARRAYSIZE(Phy5);
    BandInfo[1].ValidPhyTypeList    = Phy5;
    BandInfo[1].NumValidChannelTypes = ARRAYSIZE(Map5);
    BandInfo[1].ValidChannelTypes   = Map5;
    BandInfo[1].NumChannelWidths    = 1;
    BandInfo[1].ChannelWidthList    = &Width20;
    RtlZeroMemory(&Bands, sizeof(Bands));
    Bands.Size         = sizeof(Bands);
    Bands.NumBands     = ARRAYSIZE(BandInfo);
    Bands.BandInfoList = BandInfo;
    Status = WifiDeviceSetBandCapabilities(Dev->Device, &Bands);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    RtlZeroMemory(PhyInfo, sizeof(PhyInfo));
    for (I = 0; I < ARRAYSIZE(PhyRates); I++) {
        SIZE_T N = 0;

        PhyInfo[I].PhyType = PhyRates[I].Type;
        if (PhyRates[I].B) {
            for (K = 0; K < ARRAYSIZE(RatesB); K++, N++) {
                PhyInfo[I].DataRateList[N].DataRateFlag  = WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE;
                PhyInfo[I].DataRateList[N].DataRateValue = RatesB[K];
            }
        }
        if (PhyRates[I].Ofdm) {
            for (K = 0; K < ARRAYSIZE(RatesOfdm); K++, N++) {
                PhyInfo[I].DataRateList[N].DataRateFlag  = WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE;
                PhyInfo[I].DataRateList[N].DataRateValue = RatesOfdm[K];
            }
        }
        PhyInfo[I].NumberDataRateEntries = N;
    }
    RtlZeroMemory(&Phys, sizeof(Phys));
    Phys.Size         = sizeof(Phys);
    Phys.NumPhyTypes  = ARRAYSIZE(PhyInfo);
    Phys.PhyInfoList  = PhyInfo;
    Status = WifiDeviceSetPhyCapabilities(Dev->Device, &Phys);
    if (NT_SUCCESS(Status)) {
        Dev->CapsSet = TRUE;
    }
    return Status;
}

/* ---------------------------------------------------------------- indications */

/* One WDI indication: header (+ TLVs). Req = the request header for a task completion (its PortId +
   TransactionId), NULL for an unsolicited one (TransactionId 0, the station port). */
static VOID
CxIndicate(PCX_DEVICE Dev, UINT16 MessageId, const WDI_MESSAGE_HEADER *Req, NDIS_STATUS Status,
           const UINT8 *Tlv, ULONG TlvLen)
{
    WDF_OBJECT_ATTRIBUTES Attr;
    WDFMEMORY             Mem;
    PUCHAR                Buf;
    PWDI_MESSAGE_HEADER   H;
    NTSTATUS              St;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
    Attr.ParentObject = Dev->Device;
    St = WdfMemoryCreate(&Attr, NonPagedPoolNx, CX_POOL_TAG, sizeof(WDI_MESSAGE_HEADER) + TlvLen, &Mem, (PVOID *)&Buf);
    if (!NT_SUCCESS(St)) {
        CxLog(Dev, "[wdi] indication %u not sent: WdfMemoryCreate 0x%08lX\r\n", MessageId, St);
        return;
    }
    RtlZeroMemory(Buf, sizeof(WDI_MESSAGE_HEADER) + TlvLen);
    H = (PWDI_MESSAGE_HEADER)Buf;
    H->PortId        = (Req != NULL) ? Req->PortId : Dev->PortId;
    H->Status        = Status;
    H->TransactionId = (Req != NULL) ? Req->TransactionId : 0;
    H->IhvSpecificId = (Req != NULL) ? Req->IhvSpecificId : 0;
    if (TlvLen != 0) {
        RtlCopyMemory(Buf + sizeof(WDI_MESSAGE_HEADER), Tlv, TlvLen);
    }
    WifiDeviceReceiveIndication(Dev->Device, MessageId, Mem);
    WdfObjectDelete(Mem);
    CxLog(Dev, "[wdi] -> indication %u (%s, trans %lu) status 0x%08lX tlv %lu B\r\n", MessageId,
          (Req != NULL) ? "M4" : "unsolicited", (Req != NULL) ? Req->TransactionId : 0UL, Status, TlvLen);
}

static VOID
CxM3(PCX_DEVICE Dev, PCX_CMD Cmd, NTSTATUS Status)
{
    WifiRequestComplete(Cmd->Request, Status, sizeof(WDI_MESSAGE_HEADER));
    CxLog(Dev, "[wdi] M3 id %u status 0x%08lX\r\n", Cmd->MessageId, Status);
}

_Use_decl_annotations_
VOID
CxFailCommand(PCX_DEVICE Dev, PCX_CMD Cmd, NTSTATUS Status)
{
    UNREFERENCED_PARAMETER(Dev);
    WifiRequestComplete(Cmd->Request, Status, sizeof(WDI_MESSAGE_HEADER));
}

/* ---------------------------------------------------------------- commands */

/* WDI_SET_* properties this driver accepts without acting on them (logged, TLV-dumped): filters, power hints,
   connection quality, privacy exemption, etc. Message ids dot11wificxintf.h:36-167. Anything not listed and not
   handled below is answered STATUS_NOT_SUPPORTED. */
static const UINT16 CxAcceptedSets[] = {
    WDI_SET_POWER_STATE, WDI_SET_PRIVACY_EXEMPTION_LIST, WDI_SET_DEFAULT_KEY_ID, WDI_SET_CONNECTION_QUALITY,
    WDI_SET_RECEIVE_PACKET_FILTER, WDI_SET_RECEIVE_COALESCING, WDI_SET_AUTO_POWER_SAVE, WDI_SET_MULTICAST_LIST,
    WDI_SET_ADAPTER_CONFIGURATION, WDI_SET_CLEAR_RECEIVE_COALESCING, WDI_SET_ADVERTISEMENT_INFORMATION,
    WDI_SET_FLUSH_BSS_ENTRY, WDI_SET_END_DWELL_TIME, WDI_SET_LOCATION_PRIVACY,
};

static BOOLEAN
CxIsAcceptedSet(UINT16 Id)
{
    ULONG I;

    for (I = 0; I < ARRAYSIZE(CxAcceptedSets); I++) {
        if (CxAcceptedSets[I] == Id) {
            return TRUE;
        }
    }
    return FALSE;
}

static VOID
CxTaskSetRadioState(PCX_DEVICE Dev, PCX_CMD Cmd, const WDI_MESSAGE_HEADER *H, const UINT8 *Tlv, ULONG TlvLen)
{
    WDI_SET_RADIO_STATE_PARAMETERS P;
    NDIS_STATUS                    Ns = ParseWdiTaskSetRadioState(TlvLen, Tlv, &Dev->Tlv, &P);
    WDI_MESSAGE_HEADER             Copy = *H;   /* the request buffer is gone after the M3 */
    BOOLEAN                        On;

    if (Ns != NDIS_STATUS_SUCCESS) {
        CxM3(Dev, Cmd, STATUS_INVALID_PARAMETER);
        return;
    }
    On = (P.SoftwareRadioState != 0);
    CleanupParsedWdiTaskSetRadioState(&P);
    CxM3(Dev, Cmd, STATUS_SUCCESS);
    CxLog(Dev, "[wdi] radio %s requested (was %s; the firmware radio itself is not switched yet, W5)\r\n",
          On ? "on" : "off", Dev->RadioOn ? "on" : "off");
    if (On != Dev->RadioOn) {
        WDI_INDICATION_RADIO_STATUS_PARAMETERS Rs;
        UINT8                                 *Out = NULL;
        ULONG                                  OutLen = 0;

        Dev->RadioOn = On;
        Rs.RadioState.HardwareState = TRUE;
        Rs.RadioState.SoftwareState = On;
        if (GenerateWdiIndicationRadioStatus(&Rs, 0, &Dev->Tlv, &OutLen, &Out) == NDIS_STATUS_SUCCESS) {
            CxIndicate(Dev, WDI_INDICATION_RADIO_STATUS, NULL, NDIS_STATUS_SUCCESS, Out, OutLen);
            FreeGenerated(Out);
        }
    }
    CxIndicate(Dev, WDI_INDICATION_SET_RADIO_STATE_COMPLETE, &Copy, NDIS_STATUS_SUCCESS, NULL, 0);
}

static VOID
CxTaskDot11Reset(PCX_DEVICE Dev, PCX_CMD Cmd, const WDI_MESSAGE_HEADER *H)
{
    WDI_MESSAGE_HEADER Copy = *H;

    CxM3(Dev, Cmd, STATUS_SUCCESS);
    CxIndicate(Dev, WDI_INDICATION_DOT11_RESET_COMPLETE, &Copy, NDIS_STATUS_SUCCESS, NULL, 0);
}

_Use_decl_annotations_
VOID
CxRunCommand(PCX_DEVICE Dev, PCX_CMD Cmd)
{
    UINT                 InLen = 0, OutLen = 0;
    PUCHAR               Buf = (PUCHAR)WifiRequestGetInOutBuffer(Cmd->Request, &InLen, &OutLen);
    PWDI_MESSAGE_HEADER  H = (PWDI_MESSAGE_HEADER)Buf;
    const UINT8         *Tlv;
    ULONG                TlvLen;

    Dev->Commands++;
    if (Buf == NULL || InLen < sizeof(WDI_MESSAGE_HEADER)) {
        CxLog(Dev, "[wdi] <- id %u: buffer %p in %u - rejected\r\n", Cmd->MessageId, Buf, InLen);
        CxM3(Dev, Cmd, STATUS_INVALID_PARAMETER);
        return;
    }
    Tlv    = Buf + sizeof(WDI_MESSAGE_HEADER);
    TlvLen = InLen - sizeof(WDI_MESSAGE_HEADER);
    CxLog(Dev, "[wdi] <- id %u port 0x%X trans %lu in %u out %u\r\n", Cmd->MessageId, H->PortId, H->TransactionId,
          InLen, OutLen);
    if (TlvLen != 0 && CxDumps < CX_DUMP_MAX) {
        CxDumps++;
        DumpMessageTlvByteStream(Cmd->MessageId, TRUE, Dev->Tlv.PeerVersion, TlvLen, Tlv, (ULONG_PTR)Dev, CxTlvDump);
    }
    if (H->PortId != WDI_PORT_ID_ADAPTER) {
        Dev->PortId = H->PortId;   /* the station port (the only one this driver creates) */
    }

    switch (Cmd->MessageId) {
    case WDI_TASK_SET_RADIO_STATE:
        CxTaskSetRadioState(Dev, Cmd, H, Tlv, TlvLen);
        break;
    case WDI_TASK_DOT11_RESET:
        CxTaskDot11Reset(Dev, Cmd, H);
        break;
    default:
        if (CxIsAcceptedSet(Cmd->MessageId)) {
            CxM3(Dev, Cmd, STATUS_SUCCESS);
        } else {
            CxM3(Dev, Cmd, STATUS_NOT_SUPPORTED);
        }
        break;
    }
}

/* <= DISPATCH_LEVEL: queue for the worker (the msgbuf state is PASSIVE-only and single-owner). */
_Use_decl_annotations_
VOID
CxEvtSendCommand(WDFDEVICE Device, WIFIREQUEST SendRequest)
{
    PCX_DEVICE Dev = CxGetDevice(Device);
    PCX_CMD    Cmd = (PCX_CMD)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(CX_CMD), CX_POOL_TAG);
    KIRQL      Irql;

    if (Cmd == NULL) {
        WifiRequestComplete(SendRequest, STATUS_INSUFFICIENT_RESOURCES, sizeof(WDI_MESSAGE_HEADER));
        return;
    }
    Cmd->Request   = SendRequest;
    Cmd->MessageId = WifiRequestGetMessageId(SendRequest);
    KeAcquireSpinLock(&Dev->CmdLock, &Irql);
    InsertTailList(&Dev->CmdList, &Cmd->Link);
    KeReleaseSpinLock(&Dev->CmdLock, Irql);
    CxWake(Dev);
}

/* No Wi-Fi Direct: WiFiCx is not given Wi-Fi Direct capabilities, so this is not expected to be called. */
_Use_decl_annotations_
NTSTATUS
CxEvtCreateWifiDirectDevice(WDFDEVICE Device, WIFIDIRECT_DEVICE_INIT *Init)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Init);
    return STATUS_NOT_SUPPORTED;
}
