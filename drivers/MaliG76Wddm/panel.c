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

/* DCS long write (DT 0x39) queued: bytes into the payload FIFO little-endian, then the header. Several packets may be
 * queued and then waited for once (DsimWaitSent) - they go out in order at the next frame boundary. */
static BOOLEAN DsimQueueLong(MW_ADAPTER *A, const UCHAR *Bytes, ULONG Count)
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
    return TRUE;
}

/* DCS short write without a parameter (DT 0x05), e.g. 0x28 display off / 0x29 display on. */
static BOOLEAN DsimShortWrite(MW_ADAPTER *A, UCHAR Command)
{
    Wr(A->PanelDsim, MW_DSIM_PKTHDR, MW_DSI_DCS_SHORT_WRITE | ((ULONG)Command << 8));
    return DsimWaitSent(A);
}

static BOOLEAN DsimLongWrite(MW_ADAPTER *A, const UCHAR *Bytes, ULONG Count)
{
    return DsimQueueLong(A, Bytes, Count) && DsimWaitSent(A);
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

/* B1 + F7 03, queued together and sent in one frame gap. Caller holds PanelLock, checked PanelOk and has the level-2
 * key open (F0 5A 5A). */
static BOOLEAN PanelWriteAor(MW_ADAPTER *A, USHORT Aor)
{
    UCHAR b1[3];
    b1[0] = 0xB1;
    b1[1] = (UCHAR)(Aor >> 8);
    b1[2] = (UCHAR)Aor;
    if (!DsimQueueLong(A, b1, 3) || !DsimQueueLong(A, GammaUpdate, 2) || !DsimWaitSent(A)) {
        A->PanelOk = FALSE;     /* PanelFail (it logs) runs after the caller drops PanelLock */
        return FALSE;
    }
    A->PanelAor = Aor;
    return TRUE;
}

/* One AOR change with the key opened and closed around it. Caller holds PanelLock and has checked PanelOk. */
static BOOLEAN PanelSetAor(MW_ADAPTER *A, USHORT Aor)
{
    if (!DsimLongWrite(A, KeyOn, 3) || !PanelWriteAor(A, Aor) || !DsimLongWrite(A, KeyOff, 3)) {
        A->PanelOk = FALSE;
        return FALSE;
    }
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

/* The fade (user request 2026-10-14: no light-switch jumps): SetBrightness only records the target; this thread walks
 * the panel there MW_PANEL_RAMP_STEP levels per tick (~one frame), through the same perceptual curve, so 100 -> 0 takes
 * ~20 ticks. A new target mid-fade simply redirects it. The key stays open for the whole fade. */
static VOID PanelRampThread(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    ULONG64 t0;
    ULONG steps, ms;
    UCHAR level, target;
    BOOLEAN ok, keyOpen;

    for (;;) {
        KeWaitForSingleObject(&a->RampEvent, Executive, KernelMode, FALSE, NULL);
        if (a->RampStop)
            break;
        ok = TRUE;
        keyOpen = FALSE;
        steps = 0;
        t0 = KeQueryInterruptTime();
        ExAcquireFastMutex(&a->PanelLock);
        while (!a->RampStop && a->PanelOk && a->BrightnessOk && a->RampLevel != a->Brightness) {
            target = a->Brightness;     /* re-read every step: a new slider position redirects the fade */
            level = a->RampLevel;
            if (target > level)
                level = (UCHAR)(target - level > MW_PANEL_RAMP_STEP ? level + MW_PANEL_RAMP_STEP : target);
            else
                level = (UCHAR)(level - target > MW_PANEL_RAMP_STEP ? level - MW_PANEL_RAMP_STEP : target);
            if (LevelToAor(a, level) != a->PanelAor) {
                if (!keyOpen) {
                    ok = DsimLongWrite(a, KeyOn, 3);
                    keyOpen = ok;
                }
                if (ok)
                    ok = PanelWriteAor(a, LevelToAor(a, level));
                if (!ok) {
                    a->PanelOk = FALSE;
                    break;
                }
            }
            a->RampLevel = level;
            steps++;
            if (level != a->Brightness)
                MwSleepMs(MW_PANEL_RAMP_TICK_MS);
        }
        if (keyOpen && !DsimLongWrite(a, KeyOff, 3)) {
            a->PanelOk = FALSE;
            ok = FALSE;
        }
        ExReleaseFastMutex(&a->PanelLock);
        ms = (ULONG)((KeQueryInterruptTime() - t0) / 10000);
        if (!ok)
            PanelFail(a, 12);
        MwLog(a, L"BrightnessLast", ((ULONG)a->RampLevel << 24) | ((ULONG)a->PanelAor << 8) | (ok ? 0 : 1));
        MwLog(a, L"BrightnessRamp", (steps << 16) | (ms & 0xFFFF));
        /* The next boot's firmware starts at this level (survives a warm reset; a cold boot falls back to 50 %). */
        if (ok && a->PanelHandoff != NULL)
            a->PanelHandoff[MW_PANEL_HANDOFF_LEVEL] = ((ULONG)MW_PANEL_TAG_LEVEL << 16) | a->RampLevel;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void PanelRampStart(MW_ADAPTER *A, UCHAR StartLevel)
{
    HANDLE h;
    NTSTATUS st;

    KeInitializeEvent(&A->RampEvent, SynchronizationEvent, FALSE);
    A->RampStop = 0;
    A->RampLevel = StartLevel;      /* where the panel is now (firmware boot level, or sboot's 100 %) */
    A->Brightness = StartLevel;
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, PanelRampThread, A);
    if (NT_SUCCESS(st)) {
        st = ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode, &A->RampThread, NULL);
        ZwClose(h);
    }
    if (!NT_SUCCESS(st)) {
        A->RampThread = NULL;
        A->BrightnessOk = FALSE;    /* no fade thread, no brightness interface */
    }
    MwLog(A, L"BrightnessRampThread", (ULONG)st);
}

static void PanelRampStop(MW_ADAPTER *A)
{
    if (A->RampThread == NULL)
        return;
    InterlockedExchange(&A->RampStop, 1);
    KeSetEvent(&A->RampEvent, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(A->RampThread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(A->RampThread);
    A->RampThread = NULL;
}

void MwPanelStart(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    ULONG gc, cfg, idle = 0, i, ms, step, startLevel = 100;
    UCHAR aor[2];
    BOOLEAN fromFirmware = FALSE;

    ExInitializeFastMutex(&A->PanelLock);
    A->PanelOk = FALSE;
    A->PanelDisplayOn = TRUE;
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
    /* The firmware hand-off page (UEFI SimpleFramebufferDxe, 2026-10-14): UEFI may already have dimmed the panel for
     * the boot screen; it publishes sboot's own AOR (= our 100 %) in word [7]. Without it the panel still shows
     * sboot's value and the read below is 100 %. */
    pa.QuadPart = (LONGLONG)MW_PANEL_HANDOFF_PA;
    A->PanelHandoff = (volatile ULONG *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    ExAcquireFastMutex(&A->PanelLock);
    if (!DsimLongWrite(A, KeyOn, 3) || !DsimRead(A, 0xB1, 2, aor) || !DsimLongWrite(A, KeyOff, 3)) {
        step = 5;
    } else {
        A->PanelAor = (USHORT)((aor[0] << 8) | aor[1]);
        A->PanelAor0 = A->PanelAor;
        if (A->PanelHandoff != NULL &&
            (A->PanelHandoff[MW_PANEL_HANDOFF_AOR0] >> 16) == MW_PANEL_TAG_AOR0) {
            A->PanelAor0 = (USHORT)A->PanelHandoff[MW_PANEL_HANDOFF_AOR0];
            fromFirmware = TRUE;
        }
        if (A->PanelAor0 >= MW_PANEL_AOR_MIN_LUMA) {    /* sboot already dimmer than our floor: nothing to map */
            step = 6;
        } else {
            A->BrightnessOk = TRUE;
            step = 0;
            /* The level the panel is at now: the step whose AOR the firmware set (100 if it set none). */
            startLevel = 100;
            for (i = 0; i <= 100; i++) {
                if (LevelToAor(A, (UCHAR)i) == A->PanelAor) {
                    startLevel = i;
                    break;
                }
            }
        }
    }
    ExReleaseFastMutex(&A->PanelLock);
    MwLog(A, L"PanelHandoff", fromFirmware ? 1u : 0u);
    MwLog(A, L"PanelAorNow", A->PanelAor);
    MwLog(A, L"PanelStartLevel", startLevel);
    if (step == 5)
        PanelFail(A, 5);
    MwLog(A, L"PanelAor0", A->PanelAor0);
    MwLog(A, L"PanelRxUnexpected", A->PanelRxUnexpected);
    MwLog(A, L"PanelStep", step);
    if (A->BrightnessOk)
        PanelRampStart(A, (UCHAR)startLevel);
}

void MwPanelStop(MW_ADAPTER *A)
{
    BOOLEAN restoreFailed = FALSE;

    PanelRampStop(A);       /* before the restore: the fade thread may hold PanelLock */
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
    if (A->PanelHandoff != NULL) {
        MmUnmapIoSpace((PVOID)A->PanelHandoff, PAGE_SIZE);
        A->PanelHandoff = NULL;
    }
}

/* Display power for the internal panel (DxgkDdiSetPowerState on the monitor child, PASSIVE_LEVEL; user 2026-10-14:
 * the power button's "screen off" left the taskbar showing over a black window). DCS 0x28 display off / 0x29 display
 * on (MIPI DCS, the panel stays awake - no sleep in/out, no re-init): AMOLED pixels go dark, the scan-out keeps
 * running, so turning back on is instant. */
void MwPanelDisplayPower(MW_ADAPTER *A, BOOLEAN On)
{
    BOOLEAN ok = FALSE;

    if (A->PanelDsim == NULL)
        return;
    ExAcquireFastMutex(&A->PanelLock);
    if (A->PanelOk)
        ok = DsimShortWrite(A, On ? MW_DCS_DISPLAY_ON : MW_DCS_DISPLAY_OFF);
    if (ok)
        A->PanelDisplayOn = On;
    ExReleaseFastMutex(&A->PanelLock);
    InterlockedIncrement(&A->PanelPowerChanges);
    MwLog(A, L"PanelDisplayPower", (On ? 0x100u : 0u) | (ok ? 1u : 0u));
    MwLog(A, L"PanelPowerChanges", (ULONG)A->PanelPowerChanges);
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

/* Records the target and wakes the fade thread; returns at once (Monitor.sys calls this for every slider move). */
static NTSTATUS BrSet(PVOID Context, UCHAR Brightness)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;

    if (Brightness > 100)
        Brightness = 100;
    InterlockedIncrement(&a->BrightnessSets);
    if (!a->PanelOk || !a->BrightnessOk || a->RampThread == NULL)
        return STATUS_DEVICE_NOT_READY;
    a->Brightness = Brightness;
    KeSetEvent(&a->RampEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
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
