/*
 * panel.c - the S6E3FA7 panel over DSIM: DECON into command mode, DCS command packets, and the Windows brightness
 * interface (DXGK_BRIGHTNESS_INTERFACE_2). notes/brightness-research.md D10 (2026-10-14) is the device record:
 *
 *  - sboot hands over DECON_F in VIDEO operation mode while DSIM is in command mode; DSIM then never reaches a frame
 *    boundary (MIPI_STATUS FRM_PROCESSING forever) and command packets stay queued. Samsung's kernel switches DECON to
 *    command mode at probe (cal_9820 decon_reg_init -> decon_reg_init_probe -> decon_reg_set_operation_mode). Doing
 *    the same here let the first DCS commands through and kept the TE-triggered frames running.
 *  - Brightness is AOR only (register B1, inside the F0 5A 5A level-2 key, latched by F7 03): between sboot's own value
 *    (100 %, never brighter - no HBM/ELVSS change) and Samsung's 2-nit AOR (0 %). Gamma (CA) is left as sboot set it.
 *
 * Everything here runs at PASSIVE_LEVEL under PanelLock. Any timeout or unexpected reply disables the panel commands
 * for the rest of the boot (no reset, no retry): the screen keeps working, only brightness stops.
 */

#include "MaliG76Wddm.h"

/* dispmprt.h GUID_DEVINTERFACE_BRIGHTNESS / _2 (no INITGUID unit in this driver) */
static const GUID MwGuidBrightness = { 0xFDE5BBA4, 0xB3F9, 0x46FB, { 0xBD, 0xAA, 0x07, 0x28, 0xCE, 0x31, 0x00, 0xB4 } };
static const GUID MwGuidBrightness2 = { 0x148A3C98, 0x0ECD, 0x465A, { 0xB6, 0x34, 0xB0, 0x5F, 0x19, 0x5F, 0x77, 0x39 } };

ULONG MwPanelCmdMode = 1;   /* Parameters\PanelCmdMode: 1 = put DECON into command mode at start (D10) */
ULONG MwPanelBrightness = 1;    /* Parameters\PanelBrightness: 1 = offer the brightness interface */

/* Luminance fraction (x1000) of each 5 % brightness step: (p/100)^2.2, so the slider is roughly perceptual. */
static const USHORT MwBrightnessCurve[21] = {
    0, 1, 6, 15, 29, 47, 71, 99, 133, 173, 218, 268, 325, 388, 456, 531, 612, 699, 793, 893, 1000
};

static void MwSleepMs(ULONG Ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * (LONGLONG)Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static ULONG Rd(volatile ULONG *Base, ULONG Offset)
{
    return READ_REGISTER_ULONG(Base + Offset / 4);
}

static void Wr(volatile ULONG *Base, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG(Base + Offset / 4, Value);
}

/* The header FIFO drains once the packet went out (D10: within one frame). */
static BOOLEAN DsimWaitSent(MW_ADAPTER *A)
{
    ULONG ms;
    for (ms = 0; ms < MW_DSIM_WRITE_TIMEOUT_MS; ms++) {
        if (Rd(A->PanelDsim, MW_DSIM_FIFOCTRL) & MW_DSIM_FIFOCTRL_EMPTY_PH)
            return TRUE;
        MwSleepMs(1);
    }
    return FALSE;
}

static void PanelFail(MW_ADAPTER *A, ULONG Step)
{
    A->PanelOk = FALSE;
    MwLog(A, L"PanelFailStep", Step);
    MwLog(A, L"PanelFailIntsrc", Rd(A->PanelDsim, MW_DSIM_INTSRC));
    MwLog(A, L"PanelFailFifo", Rd(A->PanelDsim, MW_DSIM_FIFOCTRL));
    MwLog(A, L"PanelFailMipi", Rd(A->PanelDsim, MW_DSIM_MIPI_STATUS));
}

/* DCS long write (DT 0x39). Bytes go into the payload FIFO little-endian, then the header. */
static BOOLEAN DsimLongWrite(MW_ADAPTER *A, const UCHAR *Bytes, ULONG Count)
{
    ULONG i, w;
    if (Count == 0 || Count > 8)
        return FALSE;
    for (i = 0; i < Count; i += 4) {
        w = 0;
        if (i + 0 < Count) w |= (ULONG)Bytes[i + 0];
        if (i + 1 < Count) w |= (ULONG)Bytes[i + 1] << 8;
        if (i + 2 < Count) w |= (ULONG)Bytes[i + 2] << 16;
        if (i + 3 < Count) w |= (ULONG)Bytes[i + 3] << 24;
        Wr(A->PanelDsim, MW_DSIM_PAYLOAD, w);
    }
    Wr(A->PanelDsim, MW_DSIM_PKTHDR, MW_DSI_DCS_LONG_WRITE | (Count << 8));
    return DsimWaitSent(A);
}

/* DCS read of up to 2 bytes (short response DT 0x21/0x22). Anything left in the RX FIFO afterwards is drained
 * (D10: a stray word follows some replies). */
static BOOLEAN DsimRead(MW_ADAPTER *A, UCHAR Reg, ULONG Count, UCHAR *Out)
{
    ULONG ms, w, dt, i;
    BOOLEAN ok = FALSE;

    Wr(A->PanelDsim, MW_DSIM_INTSRC, MW_DSIM_INTSRC_RX_DATA_DONE);
    Wr(A->PanelDsim, MW_DSIM_PKTHDR, MW_DSI_SET_MAX_RETURN | (Count << 8));
    Wr(A->PanelDsim, MW_DSIM_PKTHDR, MW_DSI_DCS_READ | ((ULONG)Reg << 8) | MW_DSIM_PKTHDR_BTA);
    for (ms = 0; ms < MW_DSIM_READ_TIMEOUT_MS; ms++) {
        if (Rd(A->PanelDsim, MW_DSIM_INTSRC) & MW_DSIM_INTSRC_RX_DATA_DONE)
            break;
        MwSleepMs(1);
    }
    if (ms < MW_DSIM_READ_TIMEOUT_MS && !(Rd(A->PanelDsim, MW_DSIM_FIFOCTRL) & MW_DSIM_FIFOCTRL_EMPTY_RX)) {
        w = Rd(A->PanelDsim, MW_DSIM_RXFIFO);
        dt = w & 0xFF;
        if ((dt == MW_DSI_RESP_SHORT1 && Count == 1) || (dt == MW_DSI_RESP_SHORT2 && Count == 2)) {
            Out[0] = (UCHAR)(w >> 8);
            if (Count == 2)
                Out[1] = (UCHAR)(w >> 16);
            ok = TRUE;
        } else {
            A->PanelRxUnexpected = w;   /* logged by the caller after PanelLock (MwLog needs PASSIVE) */
        }
    }
    for (i = 0; i < 16 && !(Rd(A->PanelDsim, MW_DSIM_FIFOCTRL) & MW_DSIM_FIFOCTRL_EMPTY_RX); i++)
        (void)Rd(A->PanelDsim, MW_DSIM_RXFIFO);
    return ok;
}

static const UCHAR KeyOn[3] = { 0xF0, 0x5A, 0x5A };
static const UCHAR KeyOff[3] = { 0xF0, 0xA5, 0xA5 };
static const UCHAR GammaUpdate[2] = { 0xF7, 0x03 };

/* Caller holds PanelLock and has checked PanelOk. */
static BOOLEAN PanelSetAor(MW_ADAPTER *A, USHORT Aor)
{
    UCHAR b1[3];
    b1[0] = 0xB1;
    b1[1] = (UCHAR)(Aor >> 8);
    b1[2] = (UCHAR)Aor;
    if (!DsimLongWrite(A, KeyOn, 3) || !DsimLongWrite(A, b1, 3) || !DsimLongWrite(A, GammaUpdate, 2) ||
        !DsimLongWrite(A, KeyOff, 3)) {
        A->PanelOk = FALSE;     /* PanelFail (it logs) runs after the caller drops PanelLock */
        return FALSE;
    }
    A->PanelAor = Aor;
    return TRUE;
}

static USHORT LevelToAor(MW_ADAPTER *A, UCHAR Level)
{
    ULONG idx, frac, f;
    if (Level >= 100)
        return A->PanelAor0;
    idx = Level / 5;
    frac = Level % 5;
    f = (MwBrightnessCurve[idx] * (5 - frac) + MwBrightnessCurve[idx + 1] * frac) / 5;    /* x1000 */
    return (USHORT)(MW_PANEL_AOR_MIN_LUMA - ((ULONG)(MW_PANEL_AOR_MIN_LUMA - A->PanelAor0) * f) / 1000);
}

void MwPanelStart(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    ULONG gc, cfg, idle = 0, i, ms, step;
    UCHAR aor[2];

    ExInitializeFastMutex(&A->PanelLock);
    A->PanelOk = FALSE;
    A->Brightness = 100;
    MwLog(A, L"PanelCmdMode", MwPanelCmdMode);
    if (!MwPanelCmdMode)
        return;
    pa.QuadPart = MW_DSIM_PA;
    A->PanelDsim = (volatile ULONG *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = MW_DECON_PA;
    A->PanelDecon = (volatile ULONG *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (A->PanelDsim == NULL || A->PanelDecon == NULL) {
        MwLog(A, L"PanelStep", 1);
        return;
    }
    gc = Rd(A->PanelDecon, MW_DECON_GLOBAL_CONTROL);
    cfg = Rd(A->PanelDsim, MW_DSIM_CONFIG);
    MwLog(A, L"PanelDeconGlobal", gc);
    MwLog(A, L"PanelDsimConfig", cfg);
    MwLog(A, L"PanelCmdStatus", Rd(A->PanelDsim, MW_DSIM_CMD_STATUS));
    /* Only the state D10 was taken from: DECON running in video mode, DSIM in command mode. */
    if (!(gc & MW_DECON_GLOBAL_RUN_STATUS) || (cfg & MW_DSIM_CONFIG_VIDEO_MODE)) {
        MwLog(A, L"PanelStep", 2);
        return;
    }
    if (!(gc & MW_DECON_GLOBAL_OPMODE_CMD)) {
        Wr(A->PanelDecon, MW_DECON_GLOBAL_CONTROL, gc | MW_DECON_GLOBAL_OPMODE_CMD);
        Wr(A->PanelDecon, MW_DECON_SHADOW_REQ, Rd(A->PanelDecon, MW_DECON_SHADOW_REQ) | MW_DECON_SHADOW_GLOBAL);
        for (ms = 0; ms < 100 && (Rd(A->PanelDecon, MW_DECON_SHADOW_REQ) & MW_DECON_SHADOW_GLOBAL); ms++)
            MwSleepMs(1);
        MwLog(A, L"PanelShadowMs", ms);
        MwLog(A, L"PanelDeconGlobalAfter", Rd(A->PanelDecon, MW_DECON_GLOBAL_CONTROL));
    }
    /* The link must reach frame boundaries now (D10: idle in ~23 % of samples). ~50 ms, 2000 samples. */
    for (i = 0; i < 2000; i++) {
        if (!(Rd(A->PanelDsim, MW_DSIM_MIPI_STATUS) & MW_DSIM_MIPI_FRM_PROCESSING))
            idle++;
        KeStallExecutionProcessor(25);
    }
    MwLog(A, L"PanelIdleOf2000", idle);
    if (idle == 0) {
        MwLog(A, L"PanelStep", 3);
        return;
    }
    Wr(A->PanelDsim, MW_DSIM_INTSRC, 0xFFFFFFFF);       /* W1C: the sboot-era sticky bits */
    A->PanelOk = TRUE;

    if (!MwPanelBrightness) {
        MwLog(A, L"PanelStep", 4);
        return;
    }
    /* MwLog writes only at PASSIVE_LEVEL and the FAST_MUTEX raises to APC_LEVEL: log after the release. */
    ExAcquireFastMutex(&A->PanelLock);
    if (!DsimLongWrite(A, KeyOn, 3) || !DsimRead(A, 0xB1, 2, aor) || !DsimLongWrite(A, KeyOff, 3)) {
        step = 5;
    } else {
        A->PanelAor0 = (USHORT)((aor[0] << 8) | aor[1]);
        A->PanelAor = A->PanelAor0;
        if (A->PanelAor0 >= MW_PANEL_AOR_MIN_LUMA) {    /* sboot already dimmer than our floor: nothing to map */
            step = 6;
        } else {
            A->BrightnessOk = TRUE;
            step = 0;
        }
    }
    ExReleaseFastMutex(&A->PanelLock);
    if (step == 5)
        PanelFail(A, 5);
    MwLog(A, L"PanelAor0", A->PanelAor0);
    MwLog(A, L"PanelRxUnexpected", A->PanelRxUnexpected);
    MwLog(A, L"PanelStep", step);
}

void MwPanelStop(MW_ADAPTER *A)
{
    BOOLEAN restoreFailed = FALSE;

    if (A->PanelDsim != NULL && A->PanelDecon != NULL) {
        /* Leave the panel as sboot set it for whoever drives the screen next (Basic Display has no brightness). */
        ExAcquireFastMutex(&A->PanelLock);
        if (A->PanelOk && A->BrightnessOk && A->PanelAor != A->PanelAor0 && !PanelSetAor(A, A->PanelAor0))
            restoreFailed = TRUE;
        A->BrightnessOk = FALSE;
        ExReleaseFastMutex(&A->PanelLock);
        if (restoreFailed)
            PanelFail(A, 11);
    }
    if (A->PanelDsim != NULL) {
        MmUnmapIoSpace((PVOID)A->PanelDsim, PAGE_SIZE);
        A->PanelDsim = NULL;
    }
    if (A->PanelDecon != NULL) {
        MmUnmapIoSpace((PVOID)A->PanelDecon, PAGE_SIZE);
        A->PanelDecon = NULL;
    }
}

/* ---- DXGK_BRIGHTNESS_INTERFACE_2 (Monitor.sys, PASSIVE_LEVEL) ---- */

static VOID BrInterfaceReference(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static NTSTATUS BrGetPossible(PVOID Context, ULONG BufferSize, PUCHAR LevelCount, PUCHAR Levels)
{
    ULONG i, n = 21;
    UNREFERENCED_PARAMETER(Context);
    if (BufferSize < n)
        n = BufferSize;
    for (i = 0; i < n; i++)
        Levels[i] = (UCHAR)(i * 5);
    *LevelCount = (UCHAR)n;
    return STATUS_SUCCESS;
}

static NTSTATUS BrSet(PVOID Context, UCHAR Brightness)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    NTSTATUS st = STATUS_SUCCESS;
    USHORT aor;

    if (Brightness > 100)
        Brightness = 100;
    ExAcquireFastMutex(&a->PanelLock);
    if (!a->PanelOk || !a->BrightnessOk) {
        st = STATUS_DEVICE_NOT_READY;
    } else {
        aor = LevelToAor(a, Brightness);
        if (aor != a->PanelAor && !PanelSetAor(a, aor))
            st = STATUS_IO_TIMEOUT;
        else
            a->Brightness = Brightness;
    }
    ExReleaseFastMutex(&a->PanelLock);
    if (st == STATUS_IO_TIMEOUT)
        PanelFail(a, 10);
    InterlockedIncrement(&a->BrightnessSets);
    MwLog(a, L"BrightnessLast", ((ULONG)Brightness << 24) | ((ULONG)a->PanelAor << 8) | (NT_SUCCESS(st) ? 0 : 1));
    return st;
}

static NTSTATUS BrGet(PVOID Context, PUCHAR Brightness)
{
    *Brightness = ((MW_ADAPTER *)Context)->Brightness;
    return STATUS_SUCCESS;
}

static NTSTATUS BrGetCaps(PVOID Context, DXGK_BRIGHTNESS_CAPS *Caps)
{
    UNREFERENCED_PARAMETER(Context);
    Caps->Value = 0;    /* no smooth (panel-side) transitions, no adaptive brightness yet */
    return STATUS_SUCCESS;
}

static NTSTATUS BrSetState(PVOID Context, DXGK_BRIGHTNESS_STATE *State)
{
    UNREFERENCED_PARAMETER(Context);
    return State->SmoothBrightness ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
}

static NTSTATUS BrSetOptimization(PVOID Context, DXGK_BACKLIGHT_OPTIMIZATION_LEVEL Level)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Level);
    return STATUS_SUCCESS;  /* content-adaptive backlight is not implemented; accepting the level changes nothing */
}

static NTSTATUS BrGetReduction(PVOID Context, DXGK_BACKLIGHT_INFO *Info)
{
    UNREFERENCED_PARAMETER(Context);
    RtlZeroMemory(Info, sizeof(*Info));
    return STATUS_SUCCESS;
}

NTSTATUS MwPanelQueryInterface(MW_ADAPTER *A, PQUERY_INTERFACE Qi)
{
    BOOLEAN v2 = IsEqualGUID(Qi->InterfaceType, &MwGuidBrightness2) != 0;
    BOOLEAN v1 = IsEqualGUID(Qi->InterfaceType, &MwGuidBrightness) != 0;
    PDXGK_BRIGHTNESS_INTERFACE_2 br;

    if (!v1 && !v2)
        return STATUS_NOT_SUPPORTED;
    InterlockedIncrement(&A->BrightnessQueries);
    MwLog(A, L"BrightnessQuery", (v2 ? 2u : 1u) | ((ULONG)Qi->Version << 8) | ((ULONG)Qi->Size << 16));
    if (!A->BrightnessOk)
        return STATUS_NOT_SUPPORTED;
    if ((v2 && (Qi->Version != DXGK_BRIGHTNESS_INTERFACE_VERSION_2 || Qi->Size < sizeof(DXGK_BRIGHTNESS_INTERFACE_2))) ||
        (v1 && (Qi->Version != DXGK_BRIGHTNESS_INTERFACE_VERSION_1 || Qi->Size < sizeof(DXGK_BRIGHTNESS_INTERFACE))))
        return STATUS_NOT_SUPPORTED;
    br = (PDXGK_BRIGHTNESS_INTERFACE_2)Qi->Interface;
    br->Size = v2 ? sizeof(DXGK_BRIGHTNESS_INTERFACE_2) : sizeof(DXGK_BRIGHTNESS_INTERFACE);
    br->Version = v2 ? DXGK_BRIGHTNESS_INTERFACE_VERSION_2 : DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
    br->Context = A;
    br->InterfaceReference = BrInterfaceReference;
    br->InterfaceDereference = BrInterfaceReference;
    br->GetPossibleBrightness = BrGetPossible;
    br->SetBrightness = BrSet;
    br->GetBrightness = BrGet;
    if (v2) {
        br->GetBrightnessCaps = BrGetCaps;
        br->SetBrightnessState = BrSetState;
        br->SetBacklightOptimization = BrSetOptimization;
        br->GetBacklightReduction = BrGetReduction;
    }
    return STATUS_SUCCESS;
}
