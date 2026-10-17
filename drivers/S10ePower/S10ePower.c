/*
 * S10ePower - the Windows ACPM client for CPU/GPU clocks and temperatures (notes/clocks-dvfs.md).
 *
 * C0 (always): map the ACPM SRAM and record the AP channel table (ipc_channels / ipc_ap_max at SRAM+0x7000) in the
 * service key - read-only, so the live channel layout is known before anything is sent.
 * C1/C2 (only with the service-key REG_DWORD AllowSend = 1): one serialized, polling ACPM IPC client (a port of the
 * device-validated UEFI client Beyond0LteGpuProbeDxe/GpuAcpm.c, kernel acpm_ipc.c citations kept there):
 *   - READ_TEMP on the TMU channel (ch 9, stock DT acpm_tmu; used only if the channel table shows id 9 as a queue
 *     channel) every second for tz 1 MID, 2 LITTLE, 3 G3D;
 *   - FREQ_REQ on the DVFS channel (ch 0, device-validated from UEFI) for LITTLE (idx 2), MID (idx 3), G3D (idx 10),
 *     only to rates on the DT ladders, and never above the thermal-guard cap (notes/clocks-dvfs.md "thermal guard").
 * Without temperature readings the cap stays at the UEFI boot rates (no clock increase without thermal data -
 * memory thermals-before-clocks). The ACPM is load-bearing: one request at a time, every request waited for.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <ntddk.h>
#include <wdmsec.h>
#include "S10ePowerIoctl.h"

/* ---- VERIFIED addresses (firmware Exynos9820.h) ---- */
#define ACPM_MBOX_BASE        0x0000000015900000ULL   /* EXYNOS_ACPM_IPC_MBOX_BASE (live DT acpm_ipc reg[0]) */
#define ACPM_MBOX_SIZE        0x1000u
#define ACPM_SRAM_BASE        0x0000000002039000ULL   /* EXYNOS_ACPM_SRAM_BASE (live DT acpm_ipc reg[1]) */
#define ACPM_SRAM_SIZE        0x00030000u
#define ACPM_INITDATA_OFFSET  0x00007000u             /* EXYNOS_ACPM_INITDATA_OFFSET */

/* ---- protocol (GpuAcpm.c, kernel citations there) ---- */
#define ACPM_MBOX_INTGR0      0x0008u
#define ACPM_MBOX_INTGR1      0x001Cu
#define ACPM_MBOX_INTCR1      0x0020u
#define ACPM_MBOX_INTSR1      0x0028u
#define ACPM_FW_IPC_CHANNELS  0x08u
#define ACPM_FW_IPC_AP_MAX    0x18u
#define ACPM_CH_STRIDE        0x48u
#define ACPM_CH_ID            0x00u
#define ACPM_CH_TYPE          0x0Cu
#define ACPM_CH_RX_REAR       0x10u
#define ACPM_CH_RX_FRONT      0x14u
#define ACPM_CH_RX_BASE       0x18u
#define ACPM_CH_TX_REAR       0x28u
#define ACPM_CH_TX_FRONT      0x2Cu
#define ACPM_CH_TX_BASE       0x30u
#define ACPM_CH_Q_LEN         0x34u
#define ACPM_CH_Q_ELEM_SIZE   0x38u
#define ACPM_TYPE_QUEUE       1u
#define ACPM_SEQ_SHIFT        16u
#define ACPM_SEQ_MASK         0x3Fu
#define ACPM_POLL_US          10u
#define ACPM_QUEUE_FULL_US    15000u
#define ACPM_REPLY_US         100000u
#define ACPM_MAX_CHANNELS     64u

#define ACPM_CH_DVFS          0u      /* VERIFIED (notes/m15-perf.md run 30, live DT clock-controller acpm-ipc-channel) */
#define ACPM_FREQ_REQ         0u      /* acpm_dvfs.h:19 */
#define ACPM_CH_TMU           9u      /* stock DT acpm_tmu acpm-ipc-channel; checked against the live table at load */
#define TMU_READ_TEMP         0x02u   /* exynos_acpm_tmu.h */

/* ---- DT ladders (kHz, descending) ---- */
static const ULONG gLadderLittle[] = { 1950000, 1846000, 1742000, 1586000, 1456000, 1300000, 1157000, 1053000,
                                       949000, 806000, 650000, 546000, 442000 };          /* notes/m15-perf.md */
static const ULONG gLadderMid[] = { 2314000, 2210000, 2106000, 2002000, 1898000, 1794000, 1690000, 1586000,
                                    1508000, 1404000, 1222000, 1066000, 962000, 845000, 754000, 650000, 507000 };
static const ULONG gLadderG3d[] = { 702000, 650000, 572000, 433000, 377000, 325000, 260000, 200000, 156000 };
                                    /* exynos9820-mali_common.dtsi gpu_dvfs_table (LineageOS lineage-23.2) */
/* MIF: only the two device-verified ends of devfreq_mif (TWRP sysfs 421000..2093000, cur 2093000 under Linux;
 * notes/m15-perf.md:98) - the steps between are not recorded, so nothing else is requested. */
static const ULONG gLadderMif[] = { 2093000, 421000 };
/* UEFI boot rates (notes/m15-perf.md run 32, notes/gpu-m16-g3don.md): the cap when no temperature is known */
#define BOOT_LITTLE  1456000u
#define BOOT_MID     1794000u
#define BOOT_G3D     260000u

typedef struct {
    ULONG Id, Len, Size;
    volatile ULONG *TxRear, *TxFront, *RxRear, *RxFront;
    volatile UCHAR *TxBase, *RxBase;
    BOOLEAN Ok;
} ACPM_CH;

static volatile UCHAR *gMbox, *gSram;
static ACPM_CH gDvfs, gTmu;
static FAST_MUTEX gAcpmLock;
static ULONG gSeq;
static PDEVICE_OBJECT gDevice;
static WCHAR gKey[256];
static S10EPOWER_STATE gState;
static KEVENT gStopEvent;
static PVOID gGuardThread;
static volatile LONG gActive, gUnloading;   /* review fix (1): IOCTLs in flight vs. unload */

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD S10ePowerUnload;
static DRIVER_DISPATCH S10ePowerCreateClose, S10ePowerIoctl;

static VOID WriteDword(PCWSTR Name, ULONG Value)
{
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, gKey, Name, REG_DWORD, &Value, sizeof(Value));
}

static ULONG ReadDword(PCWSTR Name, ULONG Default)
{
    ULONG v = Default, def = Default;
    RTL_QUERY_REGISTRY_TABLE t[2];
    RtlZeroMemory(t, sizeof(t));
    t[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    t[0].Name = (PWSTR)Name;
    t[0].EntryContext = &v;
    t[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    t[0].DefaultData = &def;
    t[0].DefaultLength = sizeof(def);
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, gKey, t, NULL, NULL)))
        return Default;
    return v;
}

static ULONG R32(volatile const void *p) { return READ_REGISTER_ULONG((volatile ULONG *)p); }
static VOID W32(volatile void *p, ULONG v) { WRITE_REGISTER_ULONG((volatile ULONG *)p, v); }

static BOOLEAN SramOk(ULONGLONG Off, ULONGLONG Len)
{
    return (Off & 3) == 0 && Len <= ACPM_SRAM_SIZE && Off <= ACPM_SRAM_SIZE - Len;
}

/* channel_init + acpm_ipc_request_channel by id (GpuAcpm.c FindChannel) */
static BOOLEAN FindChannel(ULONG Wanted, ACPM_CH *Ch)
{
    ULONG tableOff = R32(gSram + ACPM_INITDATA_OFFSET + ACPM_FW_IPC_CHANNELS);
    ULONG apMax = R32(gSram + ACPM_INITDATA_OFFSET + ACPM_FW_IPC_AP_MAX), i;

    RtlZeroMemory(Ch, sizeof(*Ch));
    if (apMax == 0 || apMax > ACPM_MAX_CHANNELS || !SramOk(tableOff, (ULONGLONG)apMax * ACPM_CH_STRIDE))
        return FALSE;
    for (i = 0; i < apMax; i++) {
        volatile UCHAR *e = gSram + tableOff + i * ACPM_CH_STRIDE;
        ULONG txr, txf, txb, rxr, rxf, rxb;
        if (R32(e + ACPM_CH_ID) != Wanted)
            continue;
        Ch->Id = Wanted;
        Ch->Len = R32(e + ACPM_CH_Q_LEN);
        Ch->Size = R32(e + ACPM_CH_Q_ELEM_SIZE);
        txr = R32(e + ACPM_CH_RX_REAR); txf = R32(e + ACPM_CH_RX_FRONT); txb = R32(e + ACPM_CH_RX_BASE);
        rxr = R32(e + ACPM_CH_TX_REAR); rxf = R32(e + ACPM_CH_TX_FRONT); rxb = R32(e + ACPM_CH_TX_BASE);
        if (R32(e + ACPM_CH_TYPE) != ACPM_TYPE_QUEUE || Ch->Len < 2 || Ch->Len > 256 || Ch->Size < 16 ||
            Ch->Size > 64 || (Ch->Size & 3) != 0 || !SramOk(txr, 4) || !SramOk(txf, 4) || !SramOk(rxr, 4) ||
            !SramOk(rxf, 4) || !SramOk(txb, (ULONGLONG)Ch->Size * Ch->Len) ||
            !SramOk(rxb, (ULONGLONG)Ch->Size * Ch->Len))
            return FALSE;
        Ch->TxRear = (volatile ULONG *)(gSram + txr); Ch->TxFront = (volatile ULONG *)(gSram + txf);
        Ch->TxBase = gSram + txb;
        Ch->RxRear = (volatile ULONG *)(gSram + rxr); Ch->RxFront = (volatile ULONG *)(gSram + rxf);
        Ch->RxBase = gSram + rxb;
        Ch->Ok = TRUE;
        return TRUE;
    }
    return FALSE;
}

/* check_response (GpuAcpm.c CheckResponse) */
static BOOLEAN CheckResponse(ACPM_CH *Ch, ULONG Seq, ULONG *Reply, BOOLEAN *Broken)
{
    ULONG front = R32(Ch->RxFront), rear = R32(Ch->RxRear), i, steps, w;

    if (front >= Ch->Len || rear >= Ch->Len) {
        *Broken = TRUE;
        return FALSE;
    }
    for (i = rear, steps = 0; i != front && steps < Ch->Len; steps++) {
        volatile UCHAR *elem = Ch->RxBase + Ch->Size * i;
        if (((R32(elem) >> ACPM_SEQ_SHIFT) & ACPM_SEQ_MASK) == Seq) {
            for (w = 0; w < 4; w++)
                Reply[w] = R32(elem + w * 4);
            if (i != rear) {
                volatile UCHAR *re = Ch->RxBase + Ch->Size * rear;
                for (w = 0; w < Ch->Size / 4; w++)
                    W32(elem + w * 4, R32(re + w * 4));
            }
            rear = (rear + 1) % Ch->Len;
            W32(Ch->RxRear, rear);
            front = R32(Ch->RxFront);
            if (rear == front) {
                W32(gMbox + ACPM_MBOX_INTCR1, 1u << Ch->Id);
                if (rear != R32(Ch->RxFront))
                    W32(gMbox + ACPM_MBOX_INTGR1, 1u << Ch->Id);
            }
            return TRUE;
        }
        i = (i + 1) % Ch->Len;
    }
    return FALSE;
}

/* seq 1..63, skipping seqs still pending in the AP RX queue (GpuAcpm.c NextSeq) */
static ULONG NextSeq(ACPM_CH *Ch)
{
    ULONGLONG pending = 0;
    ULONG front = R32(Ch->RxFront), rear = R32(Ch->RxRear), steps, t;

    if (front < Ch->Len && rear < Ch->Len)
        for (steps = 0; rear != front && steps < Ch->Len; steps++) {
            pending |= 1ull << ((R32(Ch->RxBase + Ch->Size * rear) >> ACPM_SEQ_SHIFT) & ACPM_SEQ_MASK);
            rear = (rear + 1) % Ch->Len;
        }
    for (t = 0; t < 63; t++) {
        if (++gSeq >= 64)
            gSeq = 1;
        if ((pending & (1ull << gSeq)) == 0)
            break;
    }
    return gSeq;
}

/* __acpm_ipc_send_data, polling, response expected (GpuAcpm.c AcpmSend). Caller holds gAcpmLock. */
static NTSTATUS AcpmSend(ACPM_CH *Ch, ULONG *Cmd)
{
    ULONG front, next, seq, w, waited;
    BOOLEAN broken = FALSE;

    if (!Ch->Ok)
        return STATUS_DEVICE_NOT_READY;
    front = R32(Ch->TxFront);
    if (front >= Ch->Len || R32(Ch->TxRear) >= Ch->Len)
        return STATUS_DEVICE_DATA_ERROR;
    next = front + 1 >= Ch->Len ? 0 : front + 1;
    for (waited = 0; next == R32(Ch->TxRear); waited += ACPM_POLL_US) {
        if (waited >= ACPM_QUEUE_FULL_US)
            return STATUS_IO_TIMEOUT;
        KeStallExecutionProcessor(ACPM_POLL_US);
    }
    seq = NextSeq(Ch);
    Cmd[0] |= (seq & ACPM_SEQ_MASK) << ACPM_SEQ_SHIFT;
    for (w = 0; w < Ch->Size / 4; w++)
        W32(Ch->TxBase + Ch->Size * front + w * 4, w < 4 ? Cmd[w] : 0);
    W32(Ch->TxFront, next);
    W32(gMbox + ACPM_MBOX_INTGR0, (1u << Ch->Id) << 16);
    InterlockedIncrement((volatile LONG *)&gState.Sends);
    for (waited = 0;; waited += ACPM_POLL_US) {
        if ((R32(gMbox + ACPM_MBOX_INTSR1) & (1u << Ch->Id)) != 0 && CheckResponse(Ch, seq, Cmd, &broken))
            return STATUS_SUCCESS;
        if (broken)
            return STATUS_DEVICE_DATA_ERROR;
        if (waited >= ACPM_REPLY_US) {
            if (CheckResponse(Ch, seq, Cmd, &broken))
                return STATUS_SUCCESS;
            InterlockedIncrement((volatile LONG *)&gState.Timeouts);
            return STATUS_IO_TIMEOUT;
        }
        if (waited < 1000) {
            KeStallExecutionProcessor(ACPM_POLL_US);
        } else {
            /* review fix (2): past 1 ms do not burn the CPU - sleep 1 ms per check */
            LARGE_INTEGER ms;
            ms.QuadPart = -10000;
            KeDelayExecutionThread(KernelMode, FALSE, &ms);
            waited += 1000 - ACPM_POLL_US;
        }
    }
}

static NTSTATUS ReadTemp(ULONG Tz, ULONG *TempC)
{
    ULONG cmd[4] = { 0, TMU_READ_TEMP | (Tz << 16), 0, 0 };   /* w1: byte0 type, byte2 tzid */
    NTSTATUS st;
    ExAcquireFastMutex(&gAcpmLock);
    st = AcpmSend(&gTmu, cmd);
    ExReleaseFastMutex(&gAcpmLock);
    if (!NT_SUCCESS(st))
        return st;
    if ((cmd[1] & 0xFF) != TMU_READ_TEMP || ((cmd[1] >> 16) & 0xFF) != Tz || (CHAR)((cmd[1] >> 8) & 0xFF) < 0)
        return STATUS_DEVICE_DATA_ERROR;   /* reply: byte0 type, byte1 ret (s8), byte2 tzid, byte3 temp */
    *TempC = (cmd[1] >> 24) & 0xFF;
    return STATUS_SUCCESS;
}

static const ULONG *Ladder(ULONG Dom, ULONG *Count, ULONG *Slot)
{
    switch (Dom) {
    case S10EPOWER_DOM_LITTLE: *Count = ARRAYSIZE(gLadderLittle); *Slot = 0; return gLadderLittle;
    case S10EPOWER_DOM_MID:    *Count = ARRAYSIZE(gLadderMid);    *Slot = 1; return gLadderMid;
    case S10EPOWER_DOM_G3D:    *Count = ARRAYSIZE(gLadderG3d);    *Slot = 2; return gLadderG3d;
    default: return NULL;
    }
}

/* the highest ladder step <= Khz (the lowest step if Khz is below the ladder) */
static ULONG RoundDown(const ULONG *L, ULONG N, ULONG Khz)
{
    ULONG i;
    for (i = 0; i < N; i++)
        if (L[i] <= Khz)
            return L[i];
    return L[N - 1];
}

/* G17: MIF has no thermal cap here (Linux holds it at 2093000 all the time) and no slot in the 3-domain arrays. */
static NTSTATUS SetMif(ULONG Khz)
{
    ULONG cmd[4];
    NTSTATUS st;

    if (!gState.AllowSend || !gDvfs.Ok)
        return STATUS_INVALID_DEVICE_REQUEST;
    gState.MifReqKhz = Khz;
    cmd[0] = S10EPOWER_DOM_MIF; cmd[1] = RoundDown(gLadderMif, ARRAYSIZE(gLadderMif), Khz);
    cmd[2] = ACPM_FREQ_REQ; cmd[3] = 0;
    ExAcquireFastMutex(&gAcpmLock);
    st = AcpmSend(&gDvfs, cmd);
    if (NT_SUCCESS(st))
        gState.MifCurKhz = cmd[1];
    ExReleaseFastMutex(&gAcpmLock);
    if (!NT_SUCCESS(st))
        InterlockedIncrement((volatile LONG *)&gState.SendFails);
    return st;
}

static NTSTATUS SetFreq(ULONG Dom, ULONG Khz)
{
    ULONG n, slot, cmd[4];
    const ULONG *l = Ladder(Dom, &n, &slot);
    NTSTATUS st;

    if (Dom == S10EPOWER_DOM_MIF)
        return SetMif(Khz);
    if (l == NULL || !gState.AllowSend || !gDvfs.Ok)
        return STATUS_INVALID_DEVICE_REQUEST;
    gState.ReqKhz[slot] = Khz;
    Khz = RoundDown(l, n, min(Khz, gState.CapKhz[slot]));
    cmd[0] = Dom; cmd[1] = Khz; cmd[2] = ACPM_FREQ_REQ; cmd[3] = 0;
    ExAcquireFastMutex(&gAcpmLock);
    st = AcpmSend(&gDvfs, cmd);
    if (NT_SUCCESS(st))
        gState.CurKhz[slot] = cmd[1];   /* the reply echoes the granted rate (FREQ_GET returns 0 on this firmware);
                                         * review fix (3): updated under the lock */
    ExReleaseFastMutex(&gAcpmLock);
    if (!NT_SUCCESS(st))
        InterlockedIncrement((volatile LONG *)&gState.SendFails);
    return st;
}

/* ---- thermal guard: caps from the hottest CPU zone (MID/LITTLE) and the G3D zone; DT trips: CPU control 83 C,
 * GPU control 88 C, alerts 95..110 C, hot 115 C (notes/thermal-research.md). Hysteresis 5 C. ---- */
static VOID GuardApply(ULONG Cpu, ULONG Gpu)
{
    static ULONG level;
    ULONG want, hot = max(Cpu, Gpu > 5 ? Gpu - 5 : 0);   /* GPU trips sit 5 C above the CPU ones */

    want = hot >= 105 ? 3 : hot >= 95 ? 2 : hot >= 83 ? 1 : 0;
    if (want < level && hot + 5 > (level == 3 ? 105u : level == 2 ? 95u : 83u))
        want = level;                                   /* hysteresis on the way down */
    level = want;
    gState.GuardLevel = level;
    switch (level) {
    case 0: gState.CapKhz[0] = gLadderLittle[0]; gState.CapKhz[1] = gLadderMid[0]; gState.CapKhz[2] = gLadderG3d[0]; break;
    case 1: gState.CapKhz[0] = BOOT_LITTLE; gState.CapKhz[1] = BOOT_MID; gState.CapKhz[2] = 325000; break;
    case 2: gState.CapKhz[0] = 1053000; gState.CapKhz[1] = 1222000; gState.CapKhz[2] = 260000; break;
    default: gState.CapKhz[0] = 442000; gState.CapKhz[1] = 507000; gState.CapKhz[2] = 156000; break;
    }
    /* bring anything above its new cap down at once */
    if (gState.CurKhz[0] > gState.CapKhz[0]) (VOID)SetFreq(S10EPOWER_DOM_LITTLE, gState.CapKhz[0]);
    if (gState.CurKhz[1] > gState.CapKhz[1]) (VOID)SetFreq(S10EPOWER_DOM_MID, gState.CapKhz[1]);
    if (gState.CurKhz[2] > gState.CapKhz[2]) (VOID)SetFreq(S10EPOWER_DOM_G3D, gState.CapKhz[2]);
}

static VOID GuardThread(PVOID Context)
{
    LARGE_INTEGER period;
    UNREFERENCED_PARAMETER(Context);
    period.QuadPart = -10LL * 1000 * 1000;   /* 1 s */
    for (;;) {
        ULONG tz, cpu = 0, gpu = 0, okCount = 0;
        for (tz = 1; tz <= 3 && gTmu.Ok; tz++) {
            ULONG t;
            if (NT_SUCCESS(ReadTemp(tz, &t)) && t < 150) {
                gState.TempC[tz] = t;
                okCount++;
                if (tz == 3) gpu = t; else cpu = max(cpu, t);
            } else {
                InterlockedIncrement((volatile LONG *)&gState.TempFails);
            }
        }
        if (okCount == (gTmu.Ok ? 3u : 0u) && okCount != 0) {
            InterlockedIncrement((volatile LONG *)&gState.TempReads);
            GuardApply(cpu, gpu);
        } else {
            /* no trustworthy temperature: never above the UEFI boot rates */
            gState.CapKhz[0] = BOOT_LITTLE; gState.CapKhz[1] = BOOT_MID; gState.CapKhz[2] = BOOT_G3D;
            gState.GuardLevel = 0xFF;
        }
        if ((gState.TempReads % 10) == 1) {
            WriteDword(L"TempMid", gState.TempC[1]); WriteDword(L"TempLittle", gState.TempC[2]);
            WriteDword(L"TempG3d", gState.TempC[3]); WriteDword(L"TempReads", gState.TempReads);
            WriteDword(L"TempFails", gState.TempFails); WriteDword(L"GuardLevel", gState.GuardLevel);
        }
        if (KeWaitForSingleObject(&gStopEvent, Executive, KernelMode, FALSE, &period) == STATUS_SUCCESS)
            break;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- C0: the channel table dump ---- */
static VOID DumpChannels(VOID)
{
    ULONG tableOff = R32(gSram + ACPM_INITDATA_OFFSET + ACPM_FW_IPC_CHANNELS);
    ULONG apMax = R32(gSram + ACPM_INITDATA_OFFSET + ACPM_FW_IPC_AP_MAX), i, status = 0;
    WriteDword(L"AcpmTableOff", tableOff);
    WriteDword(L"AcpmApMax", apMax);
    if (apMax == 0 || apMax > ACPM_MAX_CHANNELS || !SramOk(tableOff, (ULONGLONG)apMax * ACPM_CH_STRIDE)) {
        status = 2;
    } else {
        ULONG *copy = (ULONG *)ExAllocatePool2(POOL_FLAG_PAGED, apMax * ACPM_CH_STRIDE, 'rPeS');
        if (copy == NULL) {
            status = 3;
        } else {
            for (i = 0; i < apMax * ACPM_CH_STRIDE / 4; i++)
                copy[i] = R32(gSram + tableOff + i * 4);
            (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, gKey, L"AcpmChannels", REG_BINARY, copy,
                                        apMax * ACPM_CH_STRIDE);
            ExFreePoolWithTag(copy, 'rPeS');
        }
    }
    WriteDword(L"AcpmStatus", status);
}

static NTSTATUS S10ePowerCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS S10ePowerIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG inLen = s->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = s->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG_PTR info = 0;

    UNREFERENCED_PARAMETER(DeviceObject);
    InterlockedIncrement(&gActive);
    if (gUnloading) {
        InterlockedDecrement(&gActive);
        Irp->IoStatus.Status = STATUS_DELETE_PENDING;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DELETE_PENDING;
    }
    switch (s->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_S10EPOWER_SET_FREQ:
        if (inLen < sizeof(S10EPOWER_SET_FREQ)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        st = SetFreq(((S10EPOWER_SET_FREQ *)buf)->Domain, ((S10EPOWER_SET_FREQ *)buf)->Khz);
        if (!NT_SUCCESS(st))
            break;
        __fallthrough;
    case IOCTL_S10EPOWER_GET_STATE:
        if (outLen < S10EPOWER_STATE_V1_SIZE) { st = STATUS_BUFFER_TOO_SMALL; break; }
        info = min(outLen, sizeof(gState));   /* version-1 callers get the version-1 prefix */
        RtlCopyMemory(buf, &gState, info);
        break;
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
    }
    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    InterlockedDecrement(&gActive);
    return st;
}

static VOID S10ePowerUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos;
    LARGE_INTEGER ms;
    UNREFERENCED_PARAMETER(DriverObject);
    InterlockedExchange(&gUnloading, 1);
    ms.QuadPart = -10000;
    while (gActive != 0)
        KeDelayExecutionThread(KernelMode, FALSE, &ms);   /* review fix (1): no IOCTL left touching the MMIO */
    if (gGuardThread != NULL) {
        KeSetEvent(&gStopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(gGuardThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(gGuardThread);
        gGuardThread = NULL;
    }
    RtlInitUnicodeString(&dos, S10EPOWER_DOS_NAME);
    (VOID)IoDeleteSymbolicLink(&dos);
    if (gDevice != NULL)
        IoDeleteDevice(gDevice);
    if (gSram != NULL)
        MmUnmapIoSpace((PVOID)gSram, ACPM_SRAM_SIZE);
    if (gMbox != NULL)
        MmUnmapIoSpace((PVOID)gMbox, ACPM_MBOX_SIZE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    PHYSICAL_ADDRESS phys;
    UNICODE_STRING name, dos, sddl;
    NTSTATUS st;
    ULONG i;
    static const GUID cls = { 0x5a10e9a1, 0x3c2d, 0x4f6e, { 0x9b, 0x21, 0x7e, 0x55, 0x1d, 0x0c, 0x8a, 0x42 } };

    DriverObject->DriverUnload = S10ePowerUnload;
    if (RegistryPath->Length >= sizeof(gKey))
        return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(gKey, RegistryPath->Buffer, RegistryPath->Length);
    gKey[RegistryPath->Length / sizeof(WCHAR)] = 0;
    ExInitializeFastMutex(&gAcpmLock);
    KeInitializeEvent(&gStopEvent, NotificationEvent, FALSE);
    RtlZeroMemory(&gState, sizeof(gState));
    gState.Version = 2;
    for (i = 0; i < S10EPOWER_TZ_COUNT; i++)
        gState.TempC[i] = 0xFF;
    gState.CurKhz[0] = BOOT_LITTLE; gState.CurKhz[1] = BOOT_MID; gState.CurKhz[2] = BOOT_G3D;
    gState.CapKhz[0] = BOOT_LITTLE; gState.CapKhz[1] = BOOT_MID; gState.CapKhz[2] = BOOT_G3D;

    phys.QuadPart = (LONGLONG)ACPM_SRAM_BASE;
    gSram = (volatile UCHAR *)MmMapIoSpaceEx(phys, ACPM_SRAM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (gSram == NULL) {
        WriteDword(L"AcpmStatus", 1);
        return STATUS_SUCCESS;   /* loaded, inert */
    }
    DumpChannels();
    gState.AllowSend = ReadDword(L"AllowSend", 0) == 1;
    WriteDword(L"AllowSendActive", gState.AllowSend);

    if (gState.AllowSend) {
        phys.QuadPart = (LONGLONG)ACPM_MBOX_BASE;
        gMbox = (volatile UCHAR *)MmMapIoSpaceEx(phys, ACPM_MBOX_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (gMbox == NULL) {
            gState.AllowSend = 0;
        } else {
            (VOID)FindChannel(ACPM_CH_DVFS, &gDvfs);
            (VOID)FindChannel(ACPM_CH_TMU, &gTmu);
            if (ReadDword(L"AllowTmu", 0) != 1)
                gTmu.Ok = FALSE;   /* temperature reads are enabled separately (C1 step) */
        }
        WriteDword(L"DvfsChannelOk", gDvfs.Ok);
        WriteDword(L"TmuChannelOk", gTmu.Ok);
        if (gState.AllowSend) {
            HANDLE h;
            st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, GuardThread, NULL);
            if (NT_SUCCESS(st)) {
                (VOID)ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode, &gGuardThread, NULL);
                ZwClose(h);
            }
        }
    }

    RtlInitUnicodeString(&name, S10EPOWER_DEVICE_NAME);
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");   /* SYSTEM + administrators */
    st = IoCreateDeviceSecure(DriverObject, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &sddl,
                              &cls, &gDevice);
    if (NT_SUCCESS(st)) {
        RtlInitUnicodeString(&dos, S10EPOWER_DOS_NAME);
        (VOID)IoCreateSymbolicLink(&dos, &name);
        DriverObject->MajorFunction[IRP_MJ_CREATE] = S10ePowerCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLOSE] = S10ePowerCreateClose;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S10ePowerIoctl;
        gDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    }
    return STATUS_SUCCESS;
}
