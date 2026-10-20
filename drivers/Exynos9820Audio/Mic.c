/*
 * Mic.c - the two digital microphones (notes/d8-mic.md): DMIC1/2 -> CS47L93 IN3L/IN4L -> AIF1TX1/2 -> UAIF0 MIC ->
 * NSRC0 -> SPUM -> SIFM0 -> WDMA0 (AboxCaptureStart). The codec is the AIF1 clock master (HpMaster 1), on SYSCLK
 * (CodecClockOn); the AIF1 format / frame are the headphone link's (CodecHpPrepare: DSP-A, 4 x 32 bit, 48 kHz).
 *
 * Codec values: MD = madera registers.h / madera.c / cs47l92.c / cs47l92-tables.c (LineageOS exynos9820 tree);
 * every one is cited in notes/d8-mic.md. NOT VALIDATED ON HARDWARE until the MicTest run is recorded there.
 *
 * MicSelfTest (Parameters\MicTest = milliseconds, at PrepareHardware): records the mics into a ring, copies it out
 * while it runs, then logs per-channel level (integer statistics; the frequency analysis is done on the PC by
 * tools/phone/audio/micanalyze.py) and writes the raw samples to
 * C:\Windows\System32\LogFiles\Exynos9820Audio-mic.raw (int32 little endian, the 4 frame slots interleaved, 48 kHz).
 */
#include "Exynos9820Audio.h"

#define R_MIC_CHARGE_PUMP_1      0x0200u   /* MICVDD: CPMIC_ENA 0x1, BYPASS 0x2, DISCH 0x4; reset 0x0006 = OFF */
#define   MICVDD_ON_REGULATED      0x0005u   /* ENA | DISCH, bypass off: LDO2 VSEL reset 0x1F = 2.5 V (madera micsupp) */
#define MICVDD_ENABLE_US         3000u     /* arizona-micsupp.c .enable_time */
#define R_MIC_BIAS_CTRL_1        0x0218u
#define   MICB1_1V8_EXTCAP_ENA     0x8065u   /* EXT_CAP 0x8000 | LVL 3 (1.8 V) << 5 | DISCH 0x4 | ENA 0x1 */
#define R_MIC_BIAS_CTRL_5        0x021Cu
#define   MICB1AB_ENA              0x0011u   /* MICB1A_ENA 0x1 | MICB1B_ENA 0x10 */
#define R_INPUT_ENABLES          0x0300u
#define   IN3L4L_ENA               0x00A0u   /* IN3L_ENA 0x20 | IN4L_ENA 0x80 */
#define R_IN3L_CONTROL           0x0320u
#define R_IN4L_CONTROL           0x0328u
#define   IN_CTRL_MASK             0x9800u   /* HPF 15 | DMIC_SUP 12:11 */
#define   IN_CTRL_HPF_MICBIAS1     0x8800u   /* HPF on, DMIC supply = MICBIAS1 (dmic-ref 1) */
#define R_ADC_DIG_VOL_3L         0x0321u
#define R_ADC_DIG_VOL_4L         0x0329u
#define   IN_VU                    0x0200u
#define   IN_MUTE                  0x0100u
#define   IN_VOL_0DB               0x0080u   /* 0.5 dB steps, max 0xBF = +31.5 dB */
#define R_AIF1TX1MIX_SRC1        0x0700u
#define R_AIF1TX2MIX_SRC1        0x0708u
#define   MIX_SRC_IN3L             0x0014u
#define   MIX_SRC_IN4L             0x0016u
#define R_AIF1_TX_ENABLES        0x0519u
#define   AIF1_TX12                0x0003u

#define MIC_BIAS_SETTLE_US       10000u
#define MIC_RING_FRAMES          4800u    /* 100 ms per period */
#define MIC_RING_PERIODS         5u

static USHORT Rd(_In_ PAUD_DEVICE_CONTEXT Dev, ULONG Reg)
{
    USHORT V = 0;

    (VOID)CodecRead(Dev, Reg, &V);
    return V;
}

/* Gain in 0.5 dB steps above 0 dB (0..63 = 0..+31.5 dB) on both ADC digital volumes. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS MicSetGain(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ ULONG HalfDb)
{
    USHORT V;

    PAGED_CODE();

    if (HalfDb > MIC_GAIN_MAX) {
        HalfDb = MIC_GAIN_MAX;
    }
    WdfWaitLockAcquire(Dev->MicLock, NULL);
    Dev->MicGain = HalfDb;
    if (Dev->MicCodecOn) {   /* else applied at the next MicCodecOn */
        V = (USHORT)(IN_VOL_0DB + HalfDb);
        (VOID)CodecWrite(Dev, R_ADC_DIG_VOL_3L, V);
        (VOID)CodecWrite(Dev, R_ADC_DIG_VOL_4L, (USHORT)(IN_VU | V));   /* VU latches both */
    }
    WdfWaitLockRelease(Dev->MicLock);
    return STATUS_SUCCESS;
}

static VOID MicCodecOn(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    USHORT V = (USHORT)(IN_VOL_0DB + min(Dev->MicGain, MIC_GAIN_MAX));

    /* 1.0.2.6 run: both DMIC inputs read a constant (HPF decay from full scale, then -63 dBFS DC, no 1 kHz tone) - the
       MICBIAS supply MICVDD was off (MIC_CHARGE_PUMP_1 reset 0x0006, ENA clear; Linux enables it as the "MICVDD"
       supply of the MICBIAS regulators, arizona-micsupp.c madera_micsupp). */
    Dev->MicSavedCp = Rd(Dev, R_MIC_CHARGE_PUMP_1);
    (VOID)CodecWrite(Dev, R_MIC_CHARGE_PUMP_1, MICVDD_ON_REGULATED);
    KeStallExecutionProcessor(MICVDD_ENABLE_US);
    Dev->MicSavedBias1 = Rd(Dev, R_MIC_BIAS_CTRL_1);
    (VOID)CodecWrite(Dev, R_MIC_BIAS_CTRL_1, MICB1_1V8_EXTCAP_ENA);
    (VOID)CodecUpdate(Dev, R_MIC_BIAS_CTRL_5, MICB1AB_ENA, MICB1AB_ENA);
    KeStallExecutionProcessor(MIC_BIAS_SETTLE_US);
    (VOID)CodecUpdate(Dev, R_IN3L_CONTROL, IN_CTRL_MASK, IN_CTRL_HPF_MICBIAS1);
    (VOID)CodecUpdate(Dev, R_IN4L_CONTROL, IN_CTRL_MASK, IN_CTRL_HPF_MICBIAS1);
    (VOID)CodecWrite(Dev, R_ADC_DIG_VOL_3L, V);
    (VOID)CodecWrite(Dev, R_ADC_DIG_VOL_4L, (USHORT)(IN_VU | V));
    (VOID)CodecUpdate(Dev, R_AIF1TX1MIX_SRC1, 0x00FF, MIX_SRC_IN3L);
    (VOID)CodecUpdate(Dev, R_AIF1TX2MIX_SRC1, 0x00FF, MIX_SRC_IN4L);
    (VOID)CodecUpdate(Dev, R_INPUT_ENABLES, IN3L4L_ENA, IN3L4L_ENA);
    (VOID)CodecUpdate(Dev, R_AIF1_TX_ENABLES, AIF1_TX12, AIF1_TX12);
    /* 1.0.2.1 run: with only TX enabled the codec drove BCLK (gpb0 bit1 toggled) but not LRCLK (bit2 static) and WDMA0
       never moved; the headphone link got both clocks after AIF1 RX enable. RX on too (its slots feed nothing unless
       the headphone outputs are on); shared with the headphone path (CodecShareOn: off with the last user). */
    (VOID)CodecShareOn(Dev, CODEC_USE_RX_MIC, NULL);
    Dev->MicCodecOn = TRUE;
    AudLog(Dev->Log, "mic: codec ON: CP %04X LDO2 %04X BIAS1 %04X BIAS5 %04X IN3L %04X/%04X/%04X IN4L %04X/%04X/%04X MIX %04X/%04X "
           "INEN %04X TXEN %04X AIF1 BCLK %04X\r\n", Rd(Dev, R_MIC_CHARGE_PUMP_1), Rd(Dev, 0x0213u),
           Rd(Dev, R_MIC_BIAS_CTRL_1), Rd(Dev, R_MIC_BIAS_CTRL_5),
           Rd(Dev, R_IN3L_CONTROL), Rd(Dev, R_ADC_DIG_VOL_3L), Rd(Dev, R_IN3L_CONTROL + 2), Rd(Dev, R_IN4L_CONTROL),
           Rd(Dev, R_ADC_DIG_VOL_4L), Rd(Dev, R_IN4L_CONTROL + 2), Rd(Dev, R_AIF1TX1MIX_SRC1), Rd(Dev, R_AIF1TX2MIX_SRC1),
           Rd(Dev, R_INPUT_ENABLES), Rd(Dev, R_AIF1_TX_ENABLES), Rd(Dev, 0x0500));
}

static VOID MicCodecOff(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    if (!Dev->MicCodecOn) {
        return;
    }
    (VOID)CodecUpdate(Dev, R_AIF1_TX_ENABLES, AIF1_TX12, 0);
    CodecShareOff(Dev, CODEC_USE_RX_MIC);
    (VOID)CodecUpdate(Dev, R_INPUT_ENABLES, IN3L4L_ENA, 0);
    (VOID)CodecWrite(Dev, R_ADC_DIG_VOL_3L, IN_MUTE | IN_VOL_0DB);
    (VOID)CodecWrite(Dev, R_ADC_DIG_VOL_4L, IN_VU | IN_MUTE | IN_VOL_0DB);
    (VOID)CodecUpdate(Dev, R_AIF1TX1MIX_SRC1, 0x00FF, 0);
    (VOID)CodecUpdate(Dev, R_AIF1TX2MIX_SRC1, 0x00FF, 0);
    (VOID)CodecUpdate(Dev, R_MIC_BIAS_CTRL_5, MICB1AB_ENA, 0);
    (VOID)CodecWrite(Dev, R_MIC_BIAS_CTRL_1, Dev->MicSavedBias1);
    (VOID)CodecWrite(Dev, R_MIC_CHARGE_PUMP_1, Dev->MicSavedCp);
    Dev->MicCodecOn = FALSE;
    AudLog(Dev->Log, "mic: codec off\r\n");
}

/* Codec first (its AIF1 TX makes BCLK/LRCLK), then the ABOX capture chain into [Phys, Phys + Bytes). */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS MicHwStart(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ PHYSICAL_ADDRESS Phys, _In_ ULONG Bytes, _In_ ULONG PeriodBytes)
{
    NTSTATUS Status;

    PAGED_CODE();

    if (!Dev->CodecUp || !Dev->Cfg.AboxEnable || !Dev->Cfg.HpMaster) {
        AudLog(Dev->Log, "mic: start refused (codec up %u, AboxEnable %lu, HpMaster %lu)\r\n", Dev->CodecUp,
               Dev->Cfg.AboxEnable, Dev->Cfg.HpMaster);
        return STATUS_DEVICE_NOT_READY;
    }
    WdfWaitLockAcquire(Dev->MicLock, NULL);
    Status = CodecAif1Prepare(Dev, CODEC_USE_CLK_MIC);   /* SYSCLK + AIF1 link (set up once, shared with headphones) */
    if (NT_SUCCESS(Status)) {
        MicCodecOn(Dev);
        Status = AboxCaptureStart(Dev, Phys, Bytes, PeriodBytes);
        if (!NT_SUCCESS(Status)) {
            MicCodecOff(Dev);
            CodecShareOff(Dev, CODEC_USE_CLK_MIC);
        }
    }
    WdfWaitLockRelease(Dev->MicLock);
    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID MicHwStop(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    PAGED_CODE();

    WdfWaitLockAcquire(Dev->MicLock, NULL);
    AboxCaptureStop(Dev);
    MicCodecOff(Dev);
    CodecShareOff(Dev, CODEC_USE_CLK_MIC);   /* SYSCLK off unless the headphones play */
    WdfWaitLockRelease(Dev->MicLock);
}

/* ---- self test ---- */

static VOID MicWriteRaw(_In_ PAUD_DEVICE_CONTEXT Dev, _In_reads_bytes_(Bytes) const VOID *Buf, ULONG Bytes)
{
    UNICODE_STRING    Name;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK   Io;
    HANDLE            H;
    NTSTATUS          Status;

    RtlInitUnicodeString(&Name, L"\\SystemRoot\\System32\\LogFiles\\Exynos9820Audio-mic.raw");
    InitializeObjectAttributes(&Oa, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&H, GENERIC_WRITE | SYNCHRONIZE, &Oa, &Io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (NT_SUCCESS(Status)) {
        Status = ZwWriteFile(H, NULL, NULL, NULL, &Io, (PVOID)Buf, Bytes, NULL, NULL);
        ZwClose(H);
    }
    AudLog(Dev->Log, "mic: raw file %lu bytes: %08lX\r\n", Bytes, (ULONG)Status);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID MicSelfTest(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ ULONG Ms)
{
    PHYSICAL_ADDRESS Low, High, Skip, Phys;
    ULONG    RingBytes = MIC_RING_FRAMES * MIC_RING_PERIODS * AUD_HW_FRAME_BYTES;
    ULONG    Want, Got = 0, Last = 0, Pos, Raw, Polls = 0, Bad = 0, Ch, I;
    PUCHAR   Ring;
    PUCHAR   LoopVa = NULL;
    PLONG    Out;
    LARGE_INTEGER Delay;
    NTSTATUS Status;

    PAGED_CODE();

    Ms   = min(max(Ms, 500u), 10000u);
    Want = AUD_RATE / 1000u * Ms;
    Low.QuadPart = 0;
    High.QuadPart = 0xFFFFFFFFLL;
    Skip.QuadPart = 0;
    Ring = (PUCHAR)MmAllocateContiguousNodeMemory(RingBytes, Low, High, Skip, PAGE_READWRITE | PAGE_NOCACHE, MM_ANY_NODE_OK);
    Out  = (PLONG)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)Want * 4 * sizeof(LONG), AUD_POOL_TAG);
    if (Ring == NULL || Out == NULL) {
        AudLog(Dev->Log, "mic test: no memory\r\n");
        goto done;
    }
    RtlZeroMemory(Ring, RingBytes);
    Phys = MmGetPhysicalAddress(Ring);

    {
        WDFKEY         Key;
        UNICODE_STRING Name;
        ULONG          V = 0;

        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
            RtlInitUnicodeString(&Name, L"MicVariant");
            (VOID)WdfRegistryQueryULong(Key, &Name, &V);
            Dev->MicWdmaIdx = 1;   /* SIFM0 -> WDMA1 (device-proven 1.0.2.5); Parameters\MicWdma overrides (diagnosis) */
            Dev->MicWdmaCtrl = 0;
            Dev->MicWdmaLimit = 0;
            RtlInitUnicodeString(&Name, L"MicWdma");
            (VOID)WdfRegistryQueryULong(Key, &Name, &Dev->MicWdmaIdx);
            Dev->MicWdmaIdx &= 7;
            RtlInitUnicodeString(&Name, L"MicWdmaCtrl");
            (VOID)WdfRegistryQueryULong(Key, &Name, &Dev->MicWdmaCtrl);
            RtlInitUnicodeString(&Name, L"MicWdmaLimit");
            (VOID)WdfRegistryQueryULong(Key, &Name, &Dev->MicWdmaLimit);
            WdfRegistryClose(Key);
        }
        Dev->MicVariant = V;
    }
    AudLog(Dev->Log, "mic test: variant 0x%lX WDMA%lu ctrl %08lX limit %08lX\r\n", Dev->MicVariant, Dev->MicWdmaIdx,
           Dev->MicWdmaCtrl, Dev->MicWdmaLimit);
    AudLog(Dev->Log, "mic test: %lu ms, gain +%lu.%lu dB, ring 0x%I64X + 0x%lX\r\n", Ms, Dev->MicGain / 2, (Dev->MicGain & 1) * 5,
           Phys.QuadPart, RingBytes);
    if (Dev->MicVariant & MIC_VAR_LOOP) {
        /* the proven playback chain RDMA0 -> SIFS1 -> UAIF1 (ABOX master, amps untouched = silent) feeds NSRC0 */
        LoopVa = (PUCHAR)MmAllocateContiguousNodeMemory(RingBytes, Low, High, Skip, PAGE_READWRITE | PAGE_NOCACHE, MM_ANY_NODE_OK);
        if (LoopVa != NULL) {
            RtlZeroMemory(LoopVa, RingBytes);
            Status = AboxInit(Dev);
            if (NT_SUCCESS(Status)) Status = AboxSelectOutput(Dev, FALSE);
            if (NT_SUCCESS(Status)) Status = AboxStart(Dev, MmGetPhysicalAddress(LoopVa), RingBytes,
                                                       MIC_RING_FRAMES * AUD_HW_FRAME_BYTES, AUD_RATE, AUD_HW_SLOTS, 32);
            AudLog(Dev->Log, "mic test: loopback source RDMA0 started %08lX\r\n", (ULONG)Status);
        }
    }
    Status = MicHwStart(Dev, Phys, RingBytes, MIC_RING_FRAMES * AUD_HW_FRAME_BYTES);
    if (!NT_SUCCESS(Status)) {
        AudLog(Dev->Log, "mic test: start failed %08lX\r\n", (ULONG)Status);
        goto done;
    }

    {
        /* diagnosis: do the codec's BCLK / LRCLK / TX data reach the AP pins (gpb0 = UAIF0)? codec side: GPIO5..8 level
           bit15 over 64 reads (and / or), as CodecHp.c does for the headphone link */
        ULONG  SavedUaif = Dev->AboxUaifCtrl0;
        USHORT AndV[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF }, OrV[4] = { 0 };
        ULONG  P, R;

        Dev->AboxUaifCtrl0 = 0x0500u;   /* ABOX_UAIF0_CTRL0: AboxSamplePins picks gpb0 */
        AboxSamplePins(Dev, "mic test");
        Dev->AboxUaifCtrl0 = SavedUaif;
        for (R = 0; R < 64; R++) {
            for (P = 0; P < 4; P++) {
                USHORT V = Rd(Dev, 0x1708u + 2 * P);

                AndV[P] &= V;
                OrV[P]  |= V;
            }
        }
        AudLog(Dev->Log, "mic test: codec AIF1 pins GPIO5-8 level and/or %u/%u %u/%u %u/%u %u/%u\r\n", AndV[0] >> 15,
               OrV[0] >> 15, AndV[1] >> 15, OrV[1] >> 15, AndV[2] >> 15, OrV[2] >> 15, AndV[3] >> 15, OrV[3] >> 15);
    }
    Delay.QuadPart = -10LL * 1000 * 20;   /* 20 ms */
    while (Got < Want && Polls < Ms / 20 + 100) {
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        Polls++;
        if (!NT_SUCCESS(AboxCapturePosition(Dev, &Pos, &Raw))) {
            break;
        }
        Pos %= MIC_RING_FRAMES * MIC_RING_PERIODS;
        if (Polls <= 5 || (Polls % 50) == 0) {
            AudLog(Dev->Log, "mic test: poll %lu WDMA0 STATUS %08lX pos %lu UAIF0 STATUS %08lX\r\n", Polls, Raw, Pos,
                   *(volatile ULONG *)(Dev->Va[AUD_RES_ABOX_IFR] + 0x050C));
        }
        while (Last != Pos && Got < Want) {   /* frames [Last, Pos) are written */
            const LONG *F = (const LONG *)(Ring + (SIZE_T)Last * AUD_HW_FRAME_BYTES);

            RtlCopyMemory(&Out[4 * Got], F, 16);   /* all 4 slots (layout check) */
            Got++;
            Last = (Last + 1) % (MIC_RING_FRAMES * MIC_RING_PERIODS);
        }
        if (Polls == 10 && Got == 0) {
            Bad = 1;   /* no progress after 200 ms */
            break;
        }
    }
    MicHwStop(Dev);
    if (LoopVa != NULL) {
        AboxStop(Dev);
        if (!Dev->AboxDmaStuck) {
            MmFreeContiguousMemory(LoopVa);
        }
        LoopVa = NULL;
    }

    AudLog(Dev->Log, "mic test: %lu frames of %lu in %lu polls%s\r\n", Got, Want, Polls, Bad ? " - WDMA0 NOT MOVING" : "");
    for (Ch = 0; Ch < 4 && Got > 0; Ch++) {
        LONGLONG Sum = 0, Peak = 0, Mean;
        ULONGLONG Sq = 0;
        LONG     First[4] = { 0 };

        /* integer statistics only (no floating point in this driver): the frequency analysis runs on the PC from the
           raw file (tools/phone/audio/micanalyze.py) */
        for (I = 0; I < Got; I++) {
            LONG V = Out[4 * I + Ch];

            Sum += V;
            if (V > Peak) Peak = V;
            if (-(LONGLONG)V > Peak) Peak = -(LONGLONG)V;
            if (I < 4) First[I] = V;
        }
        Mean = Sum / (LONGLONG)Got;
        for (I = 0; I < Got; I++) {
            LONGLONG D = ((LONGLONG)Out[4 * I + Ch] - Mean) >> 8;   /* 24-bit units: D^2 < 2^48, the sum fits */
            Sq += (ULONGLONG)(D * D);
        }
        AudLog(Dev->Log, "mic test ch%lu: mean %I64d, mean square %I64u (24-bit units^2), peak %I64d (%08lX %08lX %08lX %08lX)\r\n",
               Ch, Mean, Sq / Got, Peak, First[0], First[1], First[2], First[3]);
    }
    if (Got > 0) {
        MicWriteRaw(Dev, Out, Got * 4 * sizeof(LONG));
    }

done:
    /* review F2 of a808ccf: the diagnosis knobs apply to this test only, never to the Windows capture streams */
    Dev->MicVariant   = 0;
    Dev->MicWdmaIdx   = 1;
    Dev->MicWdmaCtrl  = 0;
    Dev->MicWdmaLimit = 0;
    if (Ring != NULL && !Dev->AboxCapDmaStuck) {
        MmFreeContiguousMemory(Ring);
    }
    if (Out != NULL) {
        ExFreePoolWithTag(Out, AUD_POOL_TAG);
    }
    AudLogFlush(Dev->Log);
}
