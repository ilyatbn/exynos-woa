// SPDX-License-Identifier: GPL-2.0
/*
 * MaliG76Wddm's display: the GOP framebuffer the firmware left on the S10e's internal panel (1080x2280 portrait,
 * Exynos9820.h EXYNOS_FB_*), one source, one target, one mode, identity transforms. Nothing in the display
 * hardware (DECON/DSIM) is touched: the firmware's scan-out keeps running and every present is a CPU copy.
 *
 * Derived from wddm-mali-g52 drivers/maliwddm/display.c and drivers/malidod/malidod.c (commit 16f7e7c6,
 * GPL-2.0, Copyright gahingwoo). Changes (notes/gpu-m16-g10.md): the child is the internal panel
 * (D3DKMDT_VOT_INTERNAL) instead of HDMI (R14); the framebuffer dxgkrnl reports is compared with the GOP values
 * (R15); one file serves both modes - the display-only mode keeps malidod's unspecified target frequencies and
 * PresentDisplayOnly, the full adapter keeps maliwddm's synthesised 60 Hz timing.
 */
#include "MaliG76Wddm.h"

/* The full adapter's synthesised blanking (FillSignal, BuildEdid): a driver choice, nothing is programmed. */
#define MW_HBLANK 280u
#define MW_VBLANK 45u

static UINT BytesPerPixel(D3DDDIFORMAT F)
{
    return (F == D3DDDIFMT_A8R8G8B8 || F == D3DDDIFMT_X8R8G8B8) ? 4 : 0;
}

static void FillSourceMode(MW_ADAPTER *Dev, D3DKMDT_VIDPN_SOURCE_MODE *M)
{
    M->Type = D3DKMDT_RMT_GRAPHICS;
    M->Format.Graphics.PrimSurfSize.cx = Dev->Fb.Width;
    M->Format.Graphics.PrimSurfSize.cy = Dev->Fb.Height;
    M->Format.Graphics.VisibleRegionSize = M->Format.Graphics.PrimSurfSize;
    M->Format.Graphics.Stride = Dev->Fb.Pitch;
    M->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
    M->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
    M->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
}

/* The timing. Nothing is programmed; the panel runs whatever sboot set.
 * Full adapter (maliwddm display.c:28-49): a full WDDM driver may not leave the frequencies unspecified (their
 * board run 4: STATUS_GRAPHICS_INVALID_FREQUENCY from pfnAddMode, Windows stopped at boot), so 60 Hz with
 * +280/+45 blanking is synthesised. Display-only (malidod.c:129-141): 60 Hz without blanking for the monitor
 * mode; AddTargetMode then leaves the target's frequencies unspecified, as Basic Display does. */
static void FillSignal(MW_ADAPTER *Dev, D3DKMDT_VIDEO_SIGNAL_INFO *S)
{
    if (Dev->Mode == MwModeDisplayOnly) {
        S->VideoStandard = D3DKMDT_VSS_OTHER;
        S->TotalSize.cx = Dev->Fb.Width;
        S->TotalSize.cy = Dev->Fb.Height;
        S->ActiveSize = S->TotalSize;
        S->VSyncFreq.Numerator = 60;
        S->VSyncFreq.Denominator = 1;
        S->HSyncFreq.Numerator = 60 * Dev->Fb.Height;
        S->HSyncFreq.Denominator = 1;
        S->PixelRate = (SIZE_T)Dev->Fb.Width * Dev->Fb.Height * 60;
        S->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
        return;
    }
    UINT totalW = Dev->Fb.Width + MW_HBLANK, totalH = Dev->Fb.Height + MW_VBLANK;

    S->VideoStandard = D3DKMDT_VSS_OTHER;
    S->TotalSize.cx = totalW;
    S->TotalSize.cy = totalH;
    S->ActiveSize.cx = Dev->Fb.Width;
    S->ActiveSize.cy = Dev->Fb.Height;
    S->PixelRate = (SIZE_T)totalW * totalH * 60;
    S->HSyncFreq.Numerator = (UINT)S->PixelRate;
    S->HSyncFreq.Denominator = totalW;
    S->VSyncFreq.Numerator = (UINT)S->PixelRate;
    S->VSyncFreq.Denominator = totalW * totalH;
    S->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

/* G76: the refresh rate of the mode this driver reports, exactly as the target mode carries it (FillSignal), for
 * DescribeAllocation (memory.c). */
void MwModeRefresh(MW_ADAPTER *A, D3DDDI_RATIONAL *Refresh)
{
    D3DKMDT_VIDEO_SIGNAL_INFO s;
    RtlZeroMemory(&s, sizeof(s));
    FillSignal(A, &s);
    *Refresh = s.VSyncFreq;
}

/* ---- the firmware framebuffer ---- */

NTSTATUS MwDisplayStart(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    NTSTATUS st;
    BOOLEAN gop;

    st = A->Dxgk.DxgkCbAcquirePostDisplayOwnership(A->Dxgk.DeviceHandle, &A->Fb);
    MwLog(A, L"AcquirePostDisplay", (ULONG)st);
    if (!NT_SUCCESS(st))
        return STATUS_UNSUCCESSFUL;
    MwLog(A, L"FbWidth", A->Fb.Width);
    MwLog(A, L"FbHeight", A->Fb.Height);
    MwLog(A, L"FbPitch", A->Fb.Pitch);
    MwLog(A, L"FbFormat", (ULONG)A->Fb.ColorFormat);
    MwLog(A, L"FbPhysLo", (ULONG)A->Fb.PhysicAddress.QuadPart);
    MwLog(A, L"FbPhysHi", (ULONG)(A->Fb.PhysicAddress.QuadPart >> 32));
    /* R15: what Basic Display was given should be our GOP. A mismatch is recorded, not fatal: dxgkrnl's
     * description is the one the scan-out matches (it came from the same firmware). */
    gop = (ULONG64)A->Fb.PhysicAddress.QuadPart == MW_GOP_FB_PA && A->Fb.Width == MW_GOP_WIDTH &&
          A->Fb.Height == MW_GOP_HEIGHT && A->Fb.Pitch == MW_GOP_PITCH;
    MwLog(A, L"FbMatchesGop", gop ? 1 : 0);
    if (A->Fb.Width == 0 || A->Fb.Height == 0 || BytesPerPixel(A->Fb.ColorFormat) == 0 ||
        A->Fb.Pitch < A->Fb.Width * 4)
        return STATUS_UNSUCCESSFUL;
    A->FbSize = (SIZE_T)A->Fb.Pitch * A->Fb.Height;
    pa = A->Fb.PhysicAddress;
    A->FbVa = (PUCHAR)MmMapIoSpaceEx(pa, A->FbSize, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (A->FbVa == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    A->SourceVisible = TRUE;
    A->PathActive = TRUE;
    return STATUS_SUCCESS;
}

void MwDisplayStop(MW_ADAPTER *A)
{
    if (A->FbVa != NULL) {
        MmUnmapIoSpace(A->FbVa, A->FbSize);
        A->FbVa = NULL;
    }
}

/* Hand the framebuffer back, e.g. to Basic Display when we are disabled. */
NTSTATUS APIENTRY MwStopDeviceAndReleasePostDisplayOwnership(IN_CONST_PVOID MiniportDeviceContext,
                                                   IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                   PDXGK_DISPLAY_INFORMATION DisplayInfo)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(TargetId);
    *DisplayInfo = dev->Fb;
    MwDisplayStop(dev);
    return STATUS_SUCCESS;
}

/* ---- the display-only adapter's caps (malidod.c:331-368) ---- */

/* A field of a structure dxgkrnl sized for our interface version: write it
 * only if it lies wholly inside the buffer we were given. */
#define FITS(Size, Type, Field) \
    (FIELD_OFFSET(Type, Field) + RTL_FIELD_SIZE(Type, Field) <= (Size))

NTSTATUS MwDisplayOnlyCaps(const DXGKARG_QUERYADAPTERINFO *Info)
{
    /* dxgkrnl passes the size of each structure as it was for the interface
     * version we declared (WIN8). Today's WDK headers make these structures
     * larger, so comparing against sizeof() rejects a buffer that is right. Fill what fits instead. */
    switch (Info->Type) {
    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS *caps = (DXGK_DRIVERCAPS *)Info->pOutputData;
        UINT size = Info->OutputDataSize;
        if (!FITS(size, DXGK_DRIVERCAPS, HighestAcceptableAddress))
            return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(caps, size);
        /* As Microsoft's Basic Display fills them (malidod's M5.1 run 3: WDDMVersion 0 = code 43). */
        caps->WDDMVersion = DXGKDDI_WDDMv1_2;
        caps->HighestAcceptableAddress.QuadPart = -1;
        if (FITS(size, DXGK_DRIVERCAPS, SupportNonVGA))
            caps->SupportNonVGA = TRUE;
        if (FITS(size, DXGK_DRIVERCAPS, SupportSmoothRotation))
            caps->SupportSmoothRotation = TRUE;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* ---- the monitor: the internal panel, always connected, no EDID read ---- */

NTSTATUS APIENTRY MwQueryChildRelations(IN_CONST_PVOID MiniportDeviceContext,
                               PDXGK_CHILD_DESCRIPTOR ChildRelations, ULONG ChildRelationsSize)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    if (ChildRelationsSize < 2 * sizeof(DXGK_CHILD_DESCRIPTOR))
        return STATUS_BUFFER_TOO_SMALL;   /* one child plus the zeroed terminator */
    RtlZeroMemory(ChildRelations, sizeof(DXGK_CHILD_DESCRIPTOR));
    ChildRelations[0].ChildDeviceType = TypeVideoOutput;
    /* R14: the S10e's DSI panel (s6e3fa7), not the RK board's HDMI. */
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_INTERNAL;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    ChildRelations[0].AcpiUid = 0;
    ChildRelations[0].ChildUid = 0;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryChildStatus(IN_CONST_PVOID MiniportDeviceContext, INOUT_PDXGK_CHILD_STATUS ChildStatus,
                            IN_BOOLEAN NonDestructiveOnly)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    switch (ChildStatus->Type) {
    case StatusConnection:
        ChildStatus->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        ChildStatus->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* G76 (full adapter, Parameters\SynthEdid): an EDID 1.3 block describing exactly the timing this driver reports
 * (FillSignal: active = the GOP fb, +280/+45 blanking, 60 Hz), so the monitor that dxgkrnl builds for the panel has
 * that mode as its preferred detailed timing and a range-limits descriptor that contains it. Without a descriptor
 * the full adapter's every commit had zero paths (run g16full PRAM "Commit src.chk.paths 00000100"): the working
 * hypothesis is that a descriptor-less monitor gets default frequency ranges that a 189.72 MHz / 139.5 kHz timing
 * falls outside (UNVERIFIED - the next run answers it). Nothing here is read from the panel: identity "EXW"/0x9820,
 * no physical size (0 = undefined), standard sRGB chromaticity, sync offsets/widths chosen inside the blanking -
 * the panel keeps sboot's real timing whatever this says (nothing is programmed). Display-only: no descriptor. */

static BOOLEAN BuildEdid(const MW_ADAPTER *Dev, UCHAR E[128])
{
    static const UCHAR chroma[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };   /* sRGB */
    static const UCHAR name[13] = { 'S', '1', '0', 'e', ' ', 'p', 'a', 'n', 'e', 'l', 0x0A, ' ', ' ' };
    ULONG w = Dev->Fb.Width, h = Dev->Fb.Height;
    ULONG64 clk = (ULONG64)(w + MW_HBLANK) * (h + MW_VBLANK) * 60;      /* Hz, = FillSignal's PixelRate */
    ULONG clk10k = (ULONG)((clk + 5000) / 10000);
    UCHAR *t, sum = 0;

    if (w == 0 || h == 0 || w > 0xFFF || h > 0xFFF || clk10k > 0xFFFF || clk > 200000000ULL)
        return FALSE;
    RtlZeroMemory(E, 128);
    E[1] = E[2] = E[3] = E[4] = E[5] = E[6] = 0xFF;                /* header 00 FF x6 00 */
    E[8] = 0x17; E[9] = 0x17;                                      /* "EXW" (5,24,23) */
    E[10] = 0x20; E[11] = 0x98;                                    /* product 0x9820 */
    E[17] = 2026 - 1990;                                           /* year */
    E[18] = 1; E[19] = 3;                                          /* EDID 1.3 */
    E[20] = 0x80;                                                  /* digital input */
    E[23] = 120;                                                   /* gamma 2.2 */
    E[24] = 0x0A;                                                  /* RGB colour, preferred timing = DTD 1 */
    RtlCopyMemory(E + 25, chroma, sizeof(chroma));
    for (ULONG i = 38; i < 54; i++)
        E[i] = 0x01;                                               /* no standard timings */
    t = E + 54;                                                    /* DTD 1: our mode */
    t[0] = (UCHAR)clk10k; t[1] = (UCHAR)(clk10k >> 8);
    t[2] = (UCHAR)w; t[3] = (UCHAR)(MW_HBLANK & 0xFF); t[4] = (UCHAR)(((w >> 8) << 4) | (MW_HBLANK >> 8));
    t[5] = (UCHAR)h; t[6] = (UCHAR)(MW_VBLANK & 0xFF); t[7] = (UCHAR)(((h >> 8) << 4) | (MW_VBLANK >> 8));
    t[8] = 48; t[9] = 32;                                          /* h sync offset / width (inside 280) */
    t[10] = (3 << 4) | 5;                                          /* v sync offset 3 / width 5 (inside 45) */
    t[17] = 0x1E;                                                  /* digital separate sync, +h +v, progressive */
    t = E + 72;                                                    /* range limits */
    t[3] = 0xFD; t[5] = 50; t[6] = 70; t[7] = 30; t[8] = 160; t[9] = 20;   /* 50-70 Hz, 30-160 kHz, 200 MHz */
    t[11] = 0x0A; for (ULONG i = 12; i < 18; i++) t[i] = ' ';
    t = E + 90;                                                    /* name */
    t[3] = 0xFC; RtlCopyMemory(t + 5, name, sizeof(name));
    t = E + 108;                                                   /* dummy */
    t[3] = 0x10;
    for (ULONG i = 0; i < 127; i++)
        sum = (UCHAR)(sum + E[i]);
    E[127] = (UCHAR)(0x100 - sum);
    return TRUE;
}

NTSTATUS APIENTRY MwQueryDeviceDescriptor(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG ChildUid,
                                 INOUT_PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    UCHAR edid[128];
    ULONG off = DeviceDescriptor->DescriptorOffset, len = DeviceDescriptor->DescriptorLength;

    UNREFERENCED_PARAMETER(ChildUid);
    if (dev->Mode == MwModeDisplayOnly || !MwSynthEdid || !BuildEdid(dev, edid))
        return STATUS_MONITOR_NO_DESCRIPTOR;
    if (off >= sizeof(edid))
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    if (len > sizeof(edid) - off)
        len = sizeof(edid) - off;
    RtlCopyMemory(DeviceDescriptor->DescriptorBuffer, edid + off, len);
    MwPramText(FALSE, "EDID off.len ", NULL, (off << 16) | len, TRUE);
    return STATUS_SUCCESS;
}

/* ---- VidPN: one source, one target, one mode, identity everything ---- */

NTSTATUS APIENTRY MwIsSupportedVidPn(IN_CONST_HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    NTSTATUS st;

    Arg->IsVidPnSupported = FALSE;
    if (Arg->hDesiredVidPn == 0) {
        Arg->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }
    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st))
        return st;
    st = vidpn->pfnAcquireSourceModeSet(Arg->hDesiredVidPn, 0, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    /* Supported if nothing is pinned yet, or what is pinned is our mode. */
    if (NT_SUCCESS(st)) {
        Arg->IsVidPnSupported = pinned == NULL ||
            (pinned->Type == D3DKMDT_RMT_GRAPHICS &&
             pinned->Format.Graphics.PrimSurfSize.cx == dev->Fb.Width &&
             pinned->Format.Graphics.PrimSurfSize.cy == dev->Fb.Height);
        if (pinned != NULL)
            setIf->pfnReleaseModeInfo(set, pinned);
    }
    vidpn->pfnReleaseSourceModeSet(Arg->hDesiredVidPn, set);
    MwPramText(FALSE, "IsSupported pinned.ok ", NULL, ((pinned != NULL) ? 0x10u : 0u) | (Arg->IsVidPnSupported ? 1u : 0u), TRUE);
    return STATUS_SUCCESS;
}

/* G76: the full adapter's every CommitVidPn came back with no pinned source mode (0x401E0307
 * STATUS_GRAPHICS_MODE_NOT_PINNED, runs g13fullc + g15fullb; dxdiag "Current Mode: Unknown", "Displayable: Not
 * Supported"), so no desktop mode was ever set on it while the KMDOD (same VidPN code, unspecified frequencies) got
 * one. With RecommendVidPn the full adapter offers the one functional VidPN it has: path source 0 -> target 0,
 * identity transforms, our source mode and target mode pinned. Display-only keeps malidod's answer. */
static NTSTATUS PinSourceMode(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H)
{
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    D3DKMDT_VIDPN_SOURCE_MODE *mode;
    D3DKMDT_VIDEO_PRESENT_SOURCE_MODE_ID id;
    NTSTATUS st = VidPn->pfnCreateNewSourceModeSet(H, 0, &set, &setIf);

    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(st)) {
        FillSourceMode(Dev, mode);
        id = mode->Id;
        st = setIf->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st))
            setIf->pfnReleaseModeInfo(set, mode);
        else
            st = setIf->pfnPinMode(set, id);
    }
    if (NT_SUCCESS(st))
        st = VidPn->pfnAssignSourceModeSet(H, 0, set);
    if (!NT_SUCCESS(st))
        VidPn->pfnReleaseSourceModeSet(H, set);
    return st;
}

static NTSTATUS PinTargetMode(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H)
{
    D3DKMDT_HVIDPNTARGETMODESET set;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *setIf;
    D3DKMDT_VIDPN_TARGET_MODE *mode;
    D3DKMDT_VIDEO_PRESENT_TARGET_MODE_ID id;
    NTSTATUS st = VidPn->pfnCreateNewTargetModeSet(H, 0, &set, &setIf);

    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(st)) {
        FillSignal(Dev, &mode->VideoSignalInfo);
        mode->Preference = D3DKMDT_MP_PREFERRED;
        id = mode->Id;
        st = setIf->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st))
            setIf->pfnReleaseModeInfo(set, mode);
        else
            st = setIf->pfnPinMode(set, id);
    }
    if (NT_SUCCESS(st))
        st = VidPn->pfnAssignTargetModeSet(H, 0, set);
    if (!NT_SUCCESS(st))
        VidPn->pfnReleaseTargetModeSet(H, set);
    return st;
}

NTSTATUS APIENTRY MwRecommendFunctionalVidPn(IN_CONST_HANDLE hAdapter,
                                             const DXGKARG_RECOMMENDFUNCTIONALVIDPN *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNTOPOLOGY topo = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf = NULL;
    D3DKMDT_VIDPN_PRESENT_PATH *path = NULL;
    NTSTATUS st;

    if (dev->Mode == MwModeDisplayOnly || !MwRecommendVidPn)
        return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hRecommendedFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (NT_SUCCESS(st))
        st = vidpn->pfnGetTopology(Arg->hRecommendedFunctionalVidPn, &topo, &topoIf);
    if (NT_SUCCESS(st))
        st = topoIf->pfnCreateNewPathInfo(topo, &path);
    if (!NT_SUCCESS(st)) {
        MwPramText(TRUE, "RecommendVidPn path ", NULL, (ULONG)st, TRUE);
        return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;     /* as before: dxgkrnl builds its own */
    }
    path->VidPnSourceId = 0;
    path->VidPnTargetId = 0;
    path->ImportanceOrdinal = D3DKMDT_VPPI_PRIMARY;
    path->ContentTransformation.Scaling = D3DKMDT_VPPS_IDENTITY;
    path->ContentTransformation.ScalingSupport.Identity = 1;
    path->ContentTransformation.Rotation = D3DKMDT_VPPR_IDENTITY;
    path->ContentTransformation.RotationSupport.Identity = 1;
    path->VidPnTargetColorBasis = D3DKMDT_CB_SRGB;
    path->VidPnTargetColorCoeffDynamicRanges.FirstChannel = 8;
    path->VidPnTargetColorCoeffDynamicRanges.SecondChannel = 8;
    path->VidPnTargetColorCoeffDynamicRanges.ThirdChannel = 8;
    path->Content = D3DKMDT_VPPC_GRAPHICS;
    path->CopyProtection.CopyProtectionType = D3DKMDT_VPPMT_NOPROTECTION;
    path->GammaRamp.Type = D3DDDI_GAMMARAMP_DEFAULT;
    st = topoIf->pfnAddPath(topo, path);
    if (!NT_SUCCESS(st)) {
        topoIf->pfnReleasePathInfo(topo, path);
        MwPramText(TRUE, "RecommendVidPn addpath ", NULL, (ULONG)st, TRUE);
        return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
    }
    st = PinSourceMode(dev, vidpn, Arg->hRecommendedFunctionalVidPn);
    if (NT_SUCCESS(st))
        st = PinTargetMode(dev, vidpn, Arg->hRecommendedFunctionalVidPn);
    MwPramText(TRUE, "RecommendVidPn pinned ", NULL, (ULONG)st, TRUE);
    return NT_SUCCESS(st) ? STATUS_SUCCESS : STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

/* Give the source and target of a path our mode if nothing is pinned. */
static NTSTATUS AddSourceMode(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                              D3DDDI_VIDEO_PRESENT_SOURCE_ID Src)
{
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE *mode;
    NTSTATUS st;

    st = VidPn->pfnAcquireSourceModeSet(H, Src, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL)
        setIf->pfnReleaseModeInfo(set, pinned);
    VidPn->pfnReleaseSourceModeSet(H, set);
    if (!NT_SUCCESS(st) || pinned != NULL)
        return st;

    st = VidPn->pfnCreateNewSourceModeSet(H, Src, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(st)) {
        FillSourceMode(Dev, mode);
        st = setIf->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st))
            setIf->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(st))
        st = VidPn->pfnAssignSourceModeSet(H, Src, set);
    if (!NT_SUCCESS(st))
        VidPn->pfnReleaseSourceModeSet(H, set);
    return st;
}

static NTSTATUS AddTargetMode(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                              D3DDDI_VIDEO_PRESENT_TARGET_ID Tgt)
{
    D3DKMDT_HVIDPNTARGETMODESET set;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_TARGET_MODE *pinned = NULL;
    D3DKMDT_VIDPN_TARGET_MODE *mode;
    NTSTATUS st;

    st = VidPn->pfnAcquireTargetModeSet(H, Tgt, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL)
        setIf->pfnReleaseModeInfo(set, pinned);
    VidPn->pfnReleaseTargetModeSet(H, set);
    if (!NT_SUCCESS(st) || pinned != NULL)
        return st;

    st = VidPn->pfnCreateNewTargetModeSet(H, Tgt, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(st)) {
        FillSignal(Dev, &mode->VideoSignalInfo);
        if (Dev->Mode == MwModeDisplayOnly) {
            /* malidod.c:490-497: a display-only driver neither knows nor sets the timing; its made-up 60 Hz
             * timing got STATUS_GRAPHICS_INVALID_FREQUENCY here (M5.1 run 4). */
            mode->VideoSignalInfo.VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
            mode->VideoSignalInfo.VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
            mode->VideoSignalInfo.HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
            mode->VideoSignalInfo.HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
            mode->VideoSignalInfo.PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
        }
        mode->Preference = D3DKMDT_MP_PREFERRED;
        st = setIf->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st))
            setIf->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(st))
        st = VidPn->pfnAssignTargetModeSet(H, Tgt, set);
    if (!NT_SUCCESS(st))
        VidPn->pfnReleaseTargetModeSet(H, set);
    return st;
}

NTSTATUS APIENTRY MwEnumVidPnCofuncModality(IN_CONST_HANDLE hAdapter,
                                            const DXGKARG_ENUMVIDPNCOFUNCMODALITY *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next;
    NTSTATUS st;

    MwPramText(FALSE, "Cofunc pivot ", NULL, (ULONG)Arg->EnumPivotType, TRUE);
    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st))
        return st;
    st = vidpn->pfnGetTopology(Arg->hConstrainingVidPn, &topo, &topoIf);
    if (!NT_SUCCESS(st))
        return st;

    st = topoIf->pfnAcquireFirstPathInfo(topo, &path);
    while (st == STATUS_SUCCESS && path != NULL) {
        /* The pivot is what the OS is enumerating against: the driver must
         * leave that mode set (or that path's transformation) alone. */
        D3DKMDT_ENUMCOFUNCMODALITY_PIVOT_TYPE pt = Arg->EnumPivotType;
        BOOLEAN srcPivot = pt == D3DKMDT_EPT_VIDPNSOURCE && Arg->EnumPivot.VidPnSourceId == path->VidPnSourceId;
        BOOLEAN tgtPivot = pt == D3DKMDT_EPT_VIDPNTARGET && Arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
        BOOLEAN xfPivot = (pt == D3DKMDT_EPT_SCALING || pt == D3DKMDT_EPT_ROTATION) &&
                          Arg->EnumPivot.VidPnSourceId == path->VidPnSourceId &&
                          Arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
        NTSTATUS r = STATUS_SUCCESS;

        MwPramText(FALSE, "Cofunc path s.t.scl.rot ", NULL,
                   ((ULONG)path->VidPnSourceId << 24) | (((ULONG)path->VidPnTargetId & 0xFF) << 16) |
                   (((ULONG)path->ContentTransformation.Scaling & 0xFF) << 8) |
                   ((ULONG)path->ContentTransformation.Rotation & 0xFF), TRUE);
        if (!srcPivot) {
            r = AddSourceMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnSourceId);
            if (!NT_SUCCESS(r))
                MwLog(dev, L"Fail_AddSourceMode", (ULONG)r);
        }
        if (NT_SUCCESS(r) && !tgtPivot) {
            r = AddTargetMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnTargetId);
            if (!NT_SUCCESS(r))
                MwLog(dev, L"Fail_AddTargetMode", (ULONG)r);
        }

        /* Identity scaling and rotation, and nothing else. */
        if (NT_SUCCESS(r) && !xfPivot &&
            (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED ||
             path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)) {
            D3DKMDT_VIDPN_PRESENT_PATH upd = *path;
            if (upd.ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.ScalingSupport,
                              sizeof(upd.ContentTransformation.ScalingSupport));
                upd.ContentTransformation.ScalingSupport.Identity = 1;
            }
            if (upd.ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.RotationSupport,
                              sizeof(upd.ContentTransformation.RotationSupport));
                upd.ContentTransformation.RotationSupport.Identity = 1;
            }
            r = topoIf->pfnUpdatePathSupportInfo(topo, &upd);
        }
        if (!NT_SUCCESS(r)) {
            topoIf->pfnReleasePathInfo(topo, path);
            return r;
        }
        st = topoIf->pfnAcquireNextPathInfo(topo, path, &next);
        topoIf->pfnReleasePathInfo(topo, path);
        path = next;
    }
    return st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET ? STATUS_SUCCESS : st;
}

NTSTATUS APIENTRY MwSetVidPnSourceVisibility(IN_CONST_HANDLE hAdapter,
                                             const DXGKARG_SETVIDPNSOURCEVISIBILITY *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    dev->SourceVisible = Arg->Visible;
    /* Invisible means blank: clear the framebuffer once. */
    if (!Arg->Visible && dev->FbVa != NULL)
        RtlZeroMemory(dev->FbVa, dev->FbSize);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCommitVidPn(IN_CONST_HANDLE hAdapter, const DXGKARG_COMMITVIDPN *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    NTSTATUS st;

    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st))
        return st;
    {
        /* G76: what was committed - source id, connectivity checks, number of paths (PRAM). */
        D3DKMDT_HVIDPNTOPOLOGY topo;
        const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
        SIZE_T paths = 0;
        if (NT_SUCCESS(vidpn->pfnGetTopology(Arg->hFunctionalVidPn, &topo, &topoIf)))
            (void)topoIf->pfnGetNumPaths(topo, &paths);
        MwPramText(FALSE, "Commit src.chk.paths ", NULL,
                   ((ULONG)Arg->AffectedVidPnSourceId << 16) | ((ULONG)Arg->MonitorConnectivityChecks << 8) |
                   ((ULONG)paths & 0xFF), TRUE);
    }
    st = vidpn->pfnAcquireSourceModeSet(Arg->hFunctionalVidPn, 0, &set, &setIf);
    if (st == STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE || !NT_SUCCESS(st)) {
        /* No path for our source: it is being turned off. */
        dev->PathActive = FALSE;
        MwLog(dev, L"CommitNoPath", (ULONG)st);
        return STATUS_SUCCESS;
    }
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL) {
        BOOLEAN ours = pinned->Type == D3DKMDT_RMT_GRAPHICS &&
                       pinned->Format.Graphics.PrimSurfSize.cx == dev->Fb.Width &&
                       pinned->Format.Graphics.PrimSurfSize.cy == dev->Fb.Height;
        setIf->pfnReleaseModeInfo(set, pinned);
        st = ours ? STATUS_SUCCESS : STATUS_GRAPHICS_INVALID_VIDPN;
        dev->PathActive = ours;
    } else if (NT_SUCCESS(st)) {
        dev->PathActive = FALSE;
    }
    vidpn->pfnReleaseSourceModeSet(Arg->hFunctionalVidPn, set);
    MwLog(dev, L"CommitStatus", (ULONG)st);
    /* G76: STATUS_GRAPHICS_MODE_NOT_PINNED (0x401E0307) is informational from pfnAcquirePinnedModeInfo, not an answer
     * dxgkrnl accepts from CommitVidPn - ETW event 494 "Driver returned an invalid NTSTATUS code: 0x401e0307" (run
     * g18etw). A source without a pinned mode is simply not active (PathActive FALSE above): success. */
    if (st == STATUS_GRAPHICS_MODE_NOT_PINNED)
        st = STATUS_SUCCESS;
    return st;
}

NTSTATUS APIENTRY MwUpdateActiveVidPnPresentPath(IN_CONST_HANDLE hAdapter,
                                                 const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwRecommendMonitorModes(IN_CONST_HANDLE hAdapter,
                                          const DXGKARG_RECOMMENDMONITORMODES *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    D3DKMDT_MONITOR_SOURCE_MODE *mode;
    NTSTATUS st;

    st = Arg->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(Arg->hMonitorSourceModeSet, &mode);
    if (!NT_SUCCESS(st))
        return st;
    FillSignal(dev, &mode->VideoSignalInfo);
    mode->ColorBasis = D3DKMDT_CB_SRGB;
    mode->ColorCoeffDynamicRanges.FirstChannel = 8;
    mode->ColorCoeffDynamicRanges.SecondChannel = 8;
    mode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    mode->ColorCoeffDynamicRanges.FourthChannel = 0;
    mode->Origin = D3DKMDT_MCO_DRIVER;
    mode->Preference = D3DKMDT_MP_PREFERRED;
    st = Arg->pMonitorSourceModeSetInterface->pfnAddMode(Arg->hMonitorSourceModeSet, mode);
    MwLog(dev, L"MonitorAddMode", (ULONG)st);
    {
        SIZE_T n = 0;
        (void)Arg->pMonitorSourceModeSetInterface->pfnGetNumModes(Arg->hMonitorSourceModeSet, &n);
        MwPramText(FALSE, "Monitor modes ", NULL, (ULONG)n, TRUE);
    }
    if (!NT_SUCCESS(st) && st != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        Arg->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Arg->hMonitorSourceModeSet, mode);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryVidPnHWCapability(IN_CONST_HANDLE hAdapter,
                                           DXGKARG_QUERYVIDPNHWCAPABILITY *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    RtlZeroMemory(&Arg->VidPnHWCaps, sizeof(Arg->VidPnHWCaps));
    return STATUS_SUCCESS;
}

/* ---- display-only present: copy the changed rectangles into the framebuffer (malidod.c:667-720) ---- */

static void CopyRect(MW_ADAPTER *Dev, const UCHAR *Src, LONG SrcPitch, const RECT *R)
{
    LONG l = max(R->left, 0), t = max(R->top, 0);
    LONG r = min(R->right, (LONG)Dev->Fb.Width), b = min(R->bottom, (LONG)Dev->Fb.Height);
    if (r <= l || b <= t)
        return;
    SIZE_T bytes = (SIZE_T)(r - l) * 4;
    for (LONG y = t; y < b; y++)
        RtlCopyMemory(Dev->FbVa + (SIZE_T)y * Dev->Fb.Pitch + (SIZE_T)l * 4,
                      Src + (SIZE_T)y * SrcPitch + (SIZE_T)l * 4, bytes);
}

NTSTATUS APIENTRY MwPresentDisplayOnly(IN_CONST_HANDLE hAdapter, const DXGKARG_PRESENT_DISPLAYONLY *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    PMDL mdl;
    const UCHAR *src;
    SIZE_T size;

    if (!dev->SourceVisible || !dev->PathActive || dev->FbVa == NULL)
        return STATUS_SUCCESS;
    /* Identity only (EnumVidPnCofuncModality offers nothing else); the source pitch must cover a full row. */
    if (Arg->BytesPerPixel != 4 || Arg->Flags.Rotate || Arg->Pitch < (LONG)dev->Fb.Width * 4)
        return STATUS_NOT_SUPPORTED;

    /* The source image is in the presenting process; lock and map it. */
    size = (SIZE_T)Arg->Pitch * dev->Fb.Height;
    mdl = IoAllocateMdl(Arg->pSource, (ULONG)size, FALSE, FALSE, NULL);
    if (mdl == NULL)
        return STATUS_NO_MEMORY;
    /* DWM presents from user mode; CDD from kernel session space (malidod's PE run 6). */
    __try {
        MmProbeAndLockPages(mdl, (ULONG_PTR)Arg->pSource >= (ULONG_PTR)MM_USER_PROBE_ADDRESS ? KernelMode : UserMode,
                            IoReadAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(mdl);
        return STATUS_INVALID_PARAMETER;
    }
    src = (const UCHAR *)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
    if (src != NULL) {
        /* Moved regions are already in place in the source image, so they
         * are copied like dirty ones. */
        for (ULONG i = 0; i < Arg->NumMoves; i++)
            CopyRect(dev, src, Arg->Pitch, &Arg->pMoves[i].DestRect);
        for (ULONG i = 0; i < Arg->NumDirtyRects; i++)
            CopyRect(dev, src, Arg->Pitch, &Arg->pDirtyRect[i]);
    }
    MmUnlockPages(mdl);
    IoFreeMdl(mdl);
    if (++dev->DodPresents == 1 || (dev->DodPresents & 1023) == 0)
        MwLog(dev, L"Presents", dev->DodPresents);
    return src != NULL ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

/* ---- the bugcheck screen ---- */

NTSTATUS APIENTRY MwSystemDisplayEnable(IN_CONST_PVOID MiniportDeviceContext,
                               IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                               PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                               OUT UINT *Width, OUT UINT *Height, OUT D3DDDIFORMAT *ColorFormat)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Flags);
    if (dev->FbVa == NULL)
        return STATUS_UNSUCCESSFUL;
    *Width = dev->Fb.Width;
    *Height = dev->Fb.Height;
    *ColorFormat = dev->Fb.ColorFormat;
    return STATUS_SUCCESS;
}

VOID APIENTRY MwSystemDisplayWrite(IN_CONST_PVOID MiniportDeviceContext, IN_CONST_PVOID Source,
                          UINT SourceWidth, UINT SourceHeight, UINT SourceStride,
                          UINT PositionX, UINT PositionY)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    if (dev->FbVa == NULL)
        return;
    for (UINT y = 0; y < SourceHeight && PositionY + y < dev->Fb.Height; y++) {
        UINT w = min(SourceWidth, dev->Fb.Width > PositionX ? dev->Fb.Width - PositionX : 0);
        RtlCopyMemory(dev->FbVa + (SIZE_T)(PositionY + y) * dev->Fb.Pitch + (SIZE_T)PositionX * 4,
                      (const UCHAR *)Source + (SIZE_T)y * SourceStride, (SIZE_T)w * 4);
    }
}

NTSTATUS APIENTRY MwSetVidPnSourceAddress(IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    /* Step d of the original: nothing is reprogrammed; presents are CPU copies (command.c). G76: the address is
     * kept, the software vsync reports it as the one being scanned out (command.c SyncVsync). Up to DIRQL: a store. */
    dev->ScanoutAddress = Arg->PrimaryAddress;
    return STATUS_SUCCESS;
}
