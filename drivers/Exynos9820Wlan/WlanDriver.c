/*  WlanDriver.c — Exynos9820Wlan: ARM64 NDIS miniport for the S10e's own
    BCM4375 WLAN chip, reached through the PCIe0 windows the UEFI half
    (Beyond0LtePcieDxe) latched across ExitBootServices.

    SCOPE OF THIS FILE (rung R3, honest state): it binds the ACPI-described
    device, maps the two windows, and performs the two hardware handshakes
    that prove the inherited controller state survived:
      1. endpoint config VID/DID via the CFG0 window  (expect 14e4:4475)
      2. ChipCommon chipid via the BAR0 window        (expect 0x4375)
    Everything past that — firmware download, common ring, msgbuf, flow
    rings, Native 802.11 OIDs — is staged behind explicit NOT-IMPLEMENTED
    markers naming the reference file and the rung that closes it. A
    transmit path that silently succeeds would be a lie; sends complete as
    NDIS_STATUS_NOT_ACCEPTED until R4.

    Protocol references: brcmfmac (ISC) primary; bcmdhd (GPLv2+LE) as the
    4375 fact source — see README.md for the license posture.
*/

#include "Exynos9820Wlan.h"
#include <ntddk.h>

#define WLAN_POOL_TAG 'naLW'

static NDIS_STRING SupportedOids[] = {
    NDIS_STRING_CONST("GenSupportedOids"),
    NDIS_STRING_CONST("GenMediumType"),
    NDIS_STRING_CONST("GenPhysicalMedium"),
    NDIS_STRING_CONST("GenCurrentPacketFilter"),
    NDIS_STRING_CONST("GenMediaConnectStatus"),
    /* Native 802.11 OIDs arrive with the R5 WDI work:
       80211Authentication, 80211Encryption, 80211Ssid, 80211NetworkType,
       and the 802_11 handler objects. NOT yet. */
};

UINT32
WlanCfgRead (_In_ PWLAN_ADAPTER A, _In_ UINT32 Offset)
{
    return READ_REGISTER_ULONG((PULONG)(A->CfgWindow + Offset));
}

VOID
WlanCfgWrite (_In_ PWLAN_ADAPTER A, _In_ UINT32 Offset, _In_ UINT32 Value)
{
    WRITE_REGISTER_ULONG((PULONG)(A->CfgWindow + Offset), Value);
}

UINT32
WlanBar0Read (_In_ PWLAN_ADAPTER A, _In_ UINT32 Offset)
{
    return READ_REGISTER_ULONG((PULONG)(A->MemWindow + Offset));
}

VOID
WlanBar0Write (_In_ PWLAN_ADAPTER A, _In_ UINT32 Offset, _In_ UINT32 Value)
{
    WRITE_REGISTER_ULONG((PULONG)(A->MemWindow + Offset), Value);
}

/* ----------------------------------------------------------------------
 * Hardware handshake (rung R3): prove the UEFI-latched state is alive.
 * -------------------------------------------------------------------- */
static
BOOLEAN
WlanChipHandshake (
    _Inout_ PWLAN_ADAPTER A
    )
{
    UINT32 VidDid = WlanCfgRead(A, EP_CFG_VENDOR_DEVICE);

    A->PciVendorId = VidDid & 0xFFFF;
    A->PciDeviceId = VidDid >> 16;

    if (A->PciVendorId != BCM4375_PCI_VENDOR_ID) {
        return FALSE;
    }

    /* The 4375's BAR0 backplane map: ChipCommon registers at +0x3000,
       chipid is the first of them (pcie_core.h:60, sbchipc.h:178). */
    A->ChipId = WlanBar0Read(A, PCIE_BAR0_CCCORE_OFF + CC_CHIPID_REG)
                & CC_CHIPID_MASK;

    if (A->ChipId != BCM4375_CHIP_ID) {
        return FALSE;
    }

    A->ChipAlive = TRUE;
    return TRUE;
}

/* ----------------------------------------------------------------------
 * Resource mapping from the ACPI _CRS (order: CFG, MEM — DSDT task).
 * -------------------------------------------------------------------- */
static
NDIS_STATUS
WlanMapResources (
    _Inout_ PWLAN_ADAPTER A
    )
{
    PNDIS_RESOURCE_LIST ResList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Res;
    UINT32 Idx;
    UINT32 Seen = 0;

    if (NdisMGetDeviceProperty(A->MiniportAdapterHandle,
            NdisDeviceInput, NULL, &ResList, NULL, NULL, NULL, NULL)
        != NDIS_STATUS_SUCCESS || ResList == NULL) {
        return NDIS_STATUS_FAILURE;
    }

    for (Idx = 0; Idx < ResList->Count && Seen < WLAN_RES_COUNT; Idx++) {
        Res = &ResList->PartialResourceList.PartialDescriptors[Idx];

        if (Res->Type != ResTypeMemory) {
            continue;
        }

        /* Resources arrive in _CRS order; index 0 = CFG, 1 = MEM. */
        if (Seen == WLAN_RES_CFG_WINDOW) {
            A->CfgPhys.LowPart = Res->u.Memory.Start.LowPart;
            A->CfgPhys.HighPart = Res->u.Memory.Start.HighPart;
            A->CfgWindow = NdisMMapIoSpace(A->CfgPhys,
                A->MiniportAdapterHandle, FALSE);
            if (A->CfgWindow == NULL) {
                return NDIS_STATUS_FAILURE;
            }
        } else {
            A->MemPhys.LowPart = Res->u.Memory.Start.LowPart;
            A->MemPhys.HighPart = Res->u.Memory.Start.HighPart;
            A->MemWindow = NdisMMapIoSpace(A->MemPhys,
                A->MiniportAdapterHandle, FALSE);
            if (A->MemWindow == NULL) {
                return NDIS_STATUS_FAILURE;
            }
        }
        Seen++;
    }

    return (Seen == WLAN_RES_COUNT) ? NDIS_STATUS_SUCCESS
                                    : NDIS_STATUS_FAILURE;
}

/* ----------------------------------------------------------------------
 * Per-adapter request handlers (set via GENERAL_INFO attributes).
 * -------------------------------------------------------------------- */
static
NDIS_STATUS
WlanOidRequest (
    _In_ PWLAN_ADAPTER A,
    _Inout_ PNDIS_OID_REQUEST OidRequest
    )
{
    NDIS_STATUS Status = NDIS_STATUS_SUCCESS;

    switch (OidRequest->RequestType) {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics: {
        PNDIS_OID Oid = (PNDIS_OID)
            OidRequest->DATA.REQUEST_INFORMATION.Oid;
        PVOID Buf = OidRequest->DATA.REQUEST_INFORMATION.InformationBuffer;
        PULONG BufLen =
            &OidRequest->DATA.REQUEST_INFORMATION.InformationBufferLength;

        switch (*Oid) {
        case OID_GEN_SUPPORTED_LIST:
            if (*BufLen < sizeof(SupportedOids)) {
                return NDIS_STATUS_BUFFER_TOO_SMALL;
            }
            NdisMoveMemory(Buf, SupportedOids, sizeof(SupportedOids));
            *BufLen = sizeof(SupportedOids);
            break;
        case OID_GEN_MEDIUM_TYPE:
            if (*BufLen < sizeof(NDIS_MEDIUM)) {
                return NDIS_STATUS_BUFFER_TOO_SMALL;
            }
            *(PNDIS_MEDIUM)Buf = NdisMediumNative802_11;
            *BufLen = sizeof(NDIS_MEDIUM);
            break;
        case OID_GEN_PHYSICAL_MEDIUM:
            if (*BufLen < sizeof(NDIS_PHYSICAL_MEDIUM)) {
                return NDIS_STATUS_BUFFER_TOO_SMALL;
            }
            *(PNDIS_PHYSICAL_MEDIUM)Buf = NdisPhysicalMediumNative802_11;
            *BufLen = sizeof(NDIS_PHYSICAL_MEDIUM);
            break;
        case OID_GEN_CURRENT_PACKET_FILTER:
            if (*BufLen < sizeof(ULONG)) {
                return NDIS_STATUS_BUFFER_TOO_SMALL;
            }
            *(PULONG)Buf = A->PacketFilter;
            *BufLen = sizeof(ULONG);
            break;
        case OID_GEN_MEDIA_CONNECT_STATUS:
            if (*BufLen < sizeof(ULONG)) {
                return NDIS_STATUS_BUFFER_TOO_SMALL;
            }
            *(PULONG)Buf = A->MediaConnected
                ? NDIS_MEDIA_STATE_CONNECTED
                : NDIS_MEDIA_STATE_DISCONNECTED;
            *BufLen = sizeof(ULONG);
            break;
        default:
            /* 802.11 OID surface = rung R5 (WDI/IHV ExtSTA). */
            Status = NDIS_STATUS_NOT_SUPPORTED;
            break;
        }
        break;
    }

    case NdisRequestSetInformation: {
        PNDIS_OID Oid = (PNDIS_OID)
            OidRequest->DATA.SET_INFORMATION.Oid;

        if (*Oid == OID_GEN_CURRENT_PACKET_FILTER &&
            OidRequest->DATA.SET_INFORMATION.InformationBufferLength
                >= sizeof(ULONG)) {
            A->PacketFilter = *(PULONG)
                OidRequest->DATA.SET_INFORMATION.InformationBuffer;
            Status = NDIS_STATUS_SUCCESS;
        } else {
            Status = NDIS_STATUS_NOT_SUPPORTED;
        }
        break;
    }

    default:
        Status = NDIS_STATUS_NOT_SUPPORTED;
        break;
    }

    return Status;
}

static
VOID
WlanSend (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_reads_(NumberOfPackets) PNET_BUFFER_LIST PacketChain,
    _In_ UINT32 NumberOfPackets
    )
{
    PWLAN_ADAPTER A = (PWLAN_ADAPTER)MiniportAdapterContext;
    PNET_BUFFER_LIST Cur;
    PNET_BUFFER_LIST Next;

    UNREFERENCED_PARAMETER(NumberOfPackets);

    /* NOT-IMPLEMENTED (rung R4): the flow-control h2d rings. Completing
       honestly as NOT_ACCEPTED beats pretending. */
    for (Cur = PacketChain; Cur != NULL; ) {
        Next = Cur->Next;
        Cur->Status = NDIS_STATUS_NOT_ACCEPTED;
        NdisMSendNetBufferListsComplete(A->MiniportAdapterHandle, Cur, 0);
        Cur = Next;
    }
}

static
VOID
WlanReturn (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ UINT32 ReturnFlags
    )
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(NetBufferLists);
    UNREFERENCED_PARAMETER(ReturnFlags);
}

static
VOID
WlanPnpEvent (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_PNP_EVENT NetPnpEvent
    )
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(NetPnpEvent);
}

static
NDIS_STATUS
WlanPause (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_PAUSE_STATE PauseState,
    _In_ NDIS_PAUSE_ACTION Action
    )
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(PauseState);
    UNREFERENCED_PARAMETER(Action);
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
WlanResume (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_RESUME_STATE ResumeState
    )
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ResumeState);
    return NDIS_STATUS_SUCCESS;
}

static
VOID
WlanOidRequestHandler (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _Inout_ PNDIS_OID_REQUEST OidRequest
    )
{
    PWLAN_ADAPTER A = (PWLAN_ADAPTER)MiniportAdapterContext;

    OidRequest->RequestStatus = WlanOidRequest(A, OidRequest);
    NdisMCompleteRequest(A->MiniportAdapterHandle, OidRequest, FALSE);
}

/* ----------------------------------------------------------------------
 * Per-adapter initialize (called by NDIS via the global init params).
 * -------------------------------------------------------------------- */
static VOID WlanMiniportHaltEx (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction
    );

static
VOID
WlanAdapterHaltEx (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction
    )
{
    WlanMiniportHaltEx(MiniportAdapterContext, HaltAction);
}

static
NDIS_STATUS
WlanInitialize (
    _Out_ NDIS_HANDLE *MiniportAdapterContext,
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_MINIPORT_ADAPTER_GLOBAL_INITIALIZATION_PARAMETERS InitParams
    )
{
    PWLAN_ADAPTER A;
    NDIS_MINIPORT_ADAPTER_REGISTRATION_INFO RegInfo = { 0 };
    NDIS_MINIPORT_ADAPTER_GENERAL_INITIALIZATION_PARAMETERS GenInfo = { 0 };

    UNREFERENCED_PARAMETER(InitParams);

    *MiniportAdapterContext = NULL;

    A = NdisAllocateMemoryWithTagPriority(MiniportAdapterHandle,
        sizeof(*A), WLAN_POOL_TAG, NormalPoolPriority);
    if (A == NULL) {
        return NDIS_STATUS_RESOURCES;
    }
    NdisZeroMemory(A, sizeof(*A));
    A->MiniportAdapterHandle = MiniportAdapterHandle;

    RegInfo.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    RegInfo.Header.Revision =
        NDIS_MINIPORT_ADAPTER_REGISTRATION_INFO_REVISION;
    RegInfo.Header.Size = sizeof(RegInfo);
    RegInfo.MiniportAdapterContext = A;
    RegInfo.NumberOfReceiveQueues = 1;
    RegInfo.NumberOfSendQueues = 1;
    RegInfo.BusType = NdisBusTypeAcpi;   /* ACPI-described, not PCI-enumerated */
    /* Rung R4 fills InterruptHardTime/InterruptDpc and enables the
       adapter interrupt (GSIV 285, level). */

    if (NdisMSetMiniportAttributes(MiniportAdapterHandle,
            (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&RegInfo)
        != NDIS_STATUS_SUCCESS) {
        NdisFreeMemory(A, 0, 0);
        return NDIS_STATUS_FAILURE;
    }

    /* Per-adapter dispatch + medium (NDIS 6.20 general init params). */
    GenInfo.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    GenInfo.Header.Revision =
        NDIS_MINIPORT_ADAPTER_GENERAL_INITIALIZATION_PARAMETERS_REVISION;
    GenInfo.Header.Size = sizeof(GenInfo);
    GenInfo.OidRequestHandler = WlanOidRequestHandler;
    GenInfo.SendNetBufferListsHandler = WlanSend;
    GenInfo.ReturnNetBufferListsHandler = WlanReturn;
    GenInfo.PnPEventHandler = WlanPnpEvent;
    GenInfo.PauseHandler = WlanPause;
    GenInfo.ResumeHandler = WlanResume;
    GenInfo.Medium = NdisMediumNative802_11;
    GenInfo.PhysicalMedium = NdisPhysicalMediumNative802_11;

    if (NdisMSetMiniportAttributes(MiniportAdapterHandle,
            (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&GenInfo)
        != NDIS_STATUS_SUCCESS) {
        NdisFreeMemory(A, 0, 0);
        return NDIS_STATUS_FAILURE;
    }

    *MiniportAdapterContext = (NDIS_HANDLE)A;
    return NDIS_STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * Driver-level entry points (NDIS 6.20 model).
 * -------------------------------------------------------------------- */
static
NDIS_STATUS
WlanMiniportInitializeEx (
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS InitParameters
    )
{
    NDIS_STATUS Status;
    NDIS_MINIPORT_ADAPTER_GLOBAL_INITIALIZATION_PARAMETERS Glob = { 0 };
    NDIS_HANDLE AdapterContext = NULL;

    UNREFERENCED_PARAMETER(MiniportDriverContext);

    Glob.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Glob.Header.Revision =
        NDIS_MINIPORT_ADAPTER_GLOBAL_INITIALIZATION_PARAMETERS_REVISION;
    Glob.Header.Size = sizeof(Glob);
    Glob.InitializeHandlerEx = WlanInitialize;
    Glob.HaltHandlerEx = WlanAdapterHaltEx;

    Status = NdisMInitializeAdapter(&AdapterContext, MiniportAdapterHandle,
        MiniportDriverContext, &Glob, InitParameters->MiniportParameters.RegistryPath);
    if (Status != NDIS_STATUS_SUCCESS) {
        return Status;
    }

    /* R3 handshakes on the inherited controller state. */
    {
        PWLAN_ADAPTER A = (PWLAN_ADAPTER)AdapterContext;

        if (WlanMapResources(A) != NDIS_STATUS_SUCCESS) {
            return NDIS_STATUS_FAILURE;
        }

        if (!WlanChipHandshake(A)) {
            /* R3 FAIL: the UEFI latch did not survive, or P2/P5 is wrong.
               Log and refuse to start; do not blue-screen the machine. */
            DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_ERROR_LEVEL,
                "Exynos9820Wlan: handshake failed vid=%x did=%x chipid=%x\n",
                A->PciVendorId, A->PciDeviceId, A->ChipId);
            return NDIS_STATUS_FAILURE;
        }

        DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
            "Exynos9820Wlan: R3 OK vid=%x did=%x chipid=4375\n",
            A->PciVendorId, A->PciDeviceId);

        /* NOT-IMPLEMENTED (rung R4): firmware download (brcmfmac
           brcmf_pcie_download_fw_nvram / bcmdhd dhd_bus_download_firmware),
           shared-area handshake, common ring 0, msgbuf, flow rings,
           NdisMEnableAdapterInterrupt. */

        A->Medium = NdisMediumNative802_11;
        A->PhysicalMedium = NdisPhysicalMediumNative802_11;
        A->MediaConnected = FALSE;
    }

    return NDIS_STATUS_SUCCESS;
}

static
VOID
WlanMiniportHaltEx (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction
    )
{
    PWLAN_ADAPTER A = (PWLAN_ADAPTER)MiniportAdapterContext;

    UNREFERENCED_PARAMETER(HaltAction);

    if (A == NULL) {
        return;
    }
    if (A->CfgWindow != NULL) {
        NdisMUnmapIoSpace(A->MiniportAdapterHandle, A->CfgWindow);
        A->CfgWindow = NULL;
    }
    if (A->MemWindow != NULL) {
        NdisMUnmapIoSpace(A->MiniportAdapterHandle, A->MemWindow);
        A->MemWindow = NULL;
    }
}

static
VOID
WlanMiniportShutdownEx (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction
    )
{
    UNREFERENCED_PARAMETER(ShutdownAction);
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    /* Role-latch contract (D3 pattern): leave the PCIe controller exactly
       as-is; nothing here touches ELBI/DBI (not even mapped). */
}

static
NDIS_STATUS
WlanMiniportPause (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_PAUSE_STATE PauseState,
    _In_ NDIS_PAUSE_ACTION Action
    )
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(PauseState);
    UNREFERENCED_PARAMETER(Action);
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
WlanMiniportResume (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_RESUME_STATE ResumeState
    )
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ResumeState);
    /* NOT-IMPLEMENTED (R4+): resume re-runs the R3 handshake at minimum. */
    return NDIS_STATUS_NOT_SUPPORTED;
}

static
VOID
WlanMiniportUnbind (
    _In_ NDIS_HANDLE MiniportAdapterContext
    )
{
    PWLAN_ADAPTER A = (PWLAN_ADAPTER)MiniportAdapterContext;

    if (A != NULL) {
        NdisFreeMemory(A, 0, 0);
    }
}

static
VOID
WlanDevicePnpNotify (
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_PNP_EVENT_NOTIFICATION Notification
    )
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(Notification);
}

VOID
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS Chars = { 0 };
    NDIS_HANDLE NdisDriverHandle = NULL;
    NDIS_STATUS Status;

    Chars.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Chars.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION;
    Chars.Header.Size = sizeof(Chars);
    Chars.MajorNdisVersion = 6;
    Chars.MinorNdisVersion = 80;   /* WDI-era NDIS; design doc section 4 */
    Chars.MajorDriverVersion = 1;
    Chars.MinorDriverVersion = 0;

    Chars.InitializeHandlerEx = WlanMiniportInitializeEx;
    Chars.HaltHandlerEx = WlanMiniportHaltEx;
    Chars.UnbindAdapterHandlerEx = WlanMiniportUnbind;
    Chars.PauseHandler = WlanMiniportPause;
    Chars.ResumeHandler = WlanMiniportResume;
    Chars.PnpEventHandlerEx = NULL;   /* driver-level PnP not needed at R3 */
    Chars.ShutdownHandlerEx = WlanMiniportShutdownEx;
    Chars.DevicePnPEventNotifyHandler = WlanDevicePnpNotify;

    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
        &Chars, &NdisDriverHandle, NULL);
    if (Status != NDIS_STATUS_SUCCESS) {
        DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_ERROR_LEVEL,
            "Exynos9820Wlan: NdisMRegisterMiniportDriver failed %x\n", Status);
    }
}
