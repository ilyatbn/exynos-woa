/*
 * CodecTone.c - CS47L93 clocking (patch, 32 kHz, FLL1 -> SYSCLK 98.304 MHz) and the jack test tone
 * (notes/d8-jack-windows.md J2).
 *
 * Every register, field and value is copied from the UEFI milestone-2 tone that was heard in wired headphones
 * (firmware/.../Beyond0LteAudioProbeDxe/AudioTone.c, user ear test boot #94, notes/d8-audio-m1.md §6.1/§6.2 for the
 * madera kernel citations). Differences: Windows accessors (Codec.c), KeStall/KeDelay instead of gBS->Stall, and the
 * clock steps are split out (CodecClockOn/Off) for the J3 data path.
 */
#include "Exynos9820Audio.h"

#define R_SOFTWARE_RESET        0x0000u
#define R_TONE_GENERATOR_1      0x0020u
#define   TONE1_ENA               0x0001u
#define R_CLOCK_32K_1           0x0100u
#define   CLK_32K_ENA             0x0040u
#define   CLK_32K_SRC_MASK        0x0003u
#define   CLK_32K_SRC_MCLK2       0x0001u
#define R_SYSTEM_CLOCK_1        0x0101u
#define   SYSCLK_SET_MASK         0x870Fu
#define   SYSCLK_98MHZ_FLL1       0x0404u
#define   SYSCLK_ENA              0x0040u
#define R_SAMPLE_RATE_1         0x0102u
#define   SAMPLE_RATE_48K         0x0003u
#define   FLL_REFCLK_SRC_MASK     0xF000u
#define   FLL_REFCLK_SRC_SHIFT    12
#define   FLL_HOLD                0x0004u
#define   FLL_ENA                 0x0001u
#define   FLL_CTRL_UPD            0x8000u
#define   FLL_FB_DIV_MASK         0x03FFu
#define   FLL_REFCLK_DIV_MASK     0x00C0u
#define   FLL_REFCLK_DIV_SHIFT    6
#define   FLL_HP_MASK             0xC000u
#define   FLL_HP_SHIFT            14
#define   FLL_PHASEDET_ENA        0x1000u
#define   FLL_LOCKDET_THR_MASK    0x001Eu
#define   FLL_LOCKDET_THR_SHIFT   1
#define   FLL_LOCKDET             0x0001u
#define   FLL_DT1_MASK            0x0103u
#define R_OUTPUT_ENABLES_1      0x0400u
#define   OUT2_ENA_BOTH           0x000Cu
#define R_RAW_OUTPUT_STATUS_1   0x0406u
#define R_DAC_DIGITAL_VOLUME_2L 0x0419u
#define R_DAC_DIGITAL_VOLUME_2R 0x041Du
#define   DAC_VU                  0x0200u
#define   DAC_MUTE                0x0100u
#define   DAC_VOL_0DB             0x0080u
#define R_OUT2LMIX_INPUT_1_SRC  0x0690u
#define R_OUT2LMIX_INPUT_1_VOL  0x0691u
#define R_OUT2RMIX_INPUT_1_SRC  0x0698u
#define R_OUT2RMIX_INPUT_1_VOL  0x0699u
#define   MIX_SRC_TONE1           0x0004u
#define   MIX_VOL_0DB_REG         0x0080u
#define R_HEADPHONE_DETECT_0    0x0299u
#define R_IRQ1_RAW_STATUS_2     0x1881u
#define   FLL1_LOCK_STS           0x0100u
#define R_IRQ1_RAW_STATUS_12    0x188Bu
#define R_IRQ1_RAW_STATUS_13    0x188Cu
#define R_IRQ1_RAW_STATUS_14    0x188Du
#define   HP2_BOTH                0x000Cu

#define TONE_MIX_VOL_REG        (0x2Cu << 1)   /* -20 dB */
#define TONE_DAC_VOL            0x0068u        /* -12 dB -> -32 dBFS total, as the UEFI tone */
#define FLL_LOCK_MAX_MS         300u
#define OUT_UP_DELAY_US         12000u
#define OUT_DOWN_DELAY_US       2000u

static const USHORT CodecPatch16[][2] = {   /* cs47l92_patch, AudioTone.c mPatch16 */
    { 0x3A2, 0x2C29 }, { 0x3A3, 0x0E00 }, { 0x281, 0x0000 }, { 0x282, 0x0000 }, { 0x4EA, 0x0100 },
    { 0x22B, 0x0000 }, { 0x4A0, 0x0080 }, { 0x4A1, 0x0000 }, { 0x4A2, 0x0000 }, { 0x180B, 0x033F },
    { 0x190B, 0x033F }, { 0x442, 0x0304 }, { 0x34C, 0x0003 }, { 0x124, 0x0C49 }, { 0x120, 0x0345 },
    { 0x120, 0x0305 }, { 0x4FA, 0x5064 }, { 0x1300, 0x050E }, { 0x1302, 0x0101 }, { 0x1380, 0x02E0 },
    { 0x1381, 0xF942 }, { 0x1382, 0x04CE }, { 0x1383, 0xFF06 }, { 0x1390, 0x0304 }, { 0x1391, 0xF8FF },
    { 0x1392, 0x04F3 }, { 0x1393, 0xFF00 }, { 0x13A0, 0x02E0 }, { 0x13A1, 0xF942 }, { 0x13A2, 0x04CE },
    { 0x13A3, 0xFF06 }, { 0x13B0, 0x0304 }, { 0x13B1, 0xF8FF }, { 0x13B2, 0x04F3 }, { 0x13B3, 0xFF00 },
    { 0x412, 0x0005 }, { 0x41A, 0x0005 }, { 0x422, 0x0005 },
};

static const ULONG CodecPatch32[][2] = {    /* AudioTone.c mPatch32 */
    { 0x3030, 0x04A00C01 }, { 0x3032, 0x0225F501 }, { 0x3044, 0x04A00C00 }, { 0x3046, 0x0225FF01 },
    { 0x3080, 0x04A00C01 }, { 0x3082, 0x0226F501 }, { 0x3094, 0x04A00C00 }, { 0x3096, 0x0226FF01 },
    { 0x30D1, 0x04A10C01 }, { 0x30D2, 0x0227F501 }, { 0x30E4, 0x04A10C00 }, { 0x30E6, 0x0227FF01 },
    { 0x3120, 0x04A10C01 }, { 0x3122, 0x0228F501 }, { 0x3134, 0x04A10C00 }, { 0x3136, 0x0228FF01 },
    { 0x3170, 0x04A20C01 }, { 0x3172, 0x022B0101 }, { 0x3174, 0x0229F501 }, { 0x3184, 0x04A20C00 },
    { 0x3186, 0x022B0100 }, { 0x3188, 0x0229FF01 }, { 0x31C0, 0x04A20C01 }, { 0x31C2, 0x022B0001 },
    { 0x31C4, 0x022AF501 }, { 0x31D4, 0x04A20C00 }, { 0x31D6, 0x022B0000 }, { 0x31D8, 0x022AFF01 },
};


static const CODEC_FLL CodecFllMclk2 = { "MCLK2 (32 kHz)", 1, 375, 0, 1, 4, 0, 0x23F0, 0, 2, 0 };
static const CODEC_FLL CodecFllMclk1 = { "MCLK1 (26 MHz)", 0, 22, 0x045A, 0x0659, 1, 1, 0x21F0, 3, 8, 3 };

static USHORT Rd(_In_ PAUD_DEVICE_CONTEXT Dev, ULONG Reg)
{
    USHORT V = 0;

    (VOID)CodecRead(Dev, Reg, &V);
    return V;
}

static VOID SleepMs(ULONG Ms)
{
    LARGE_INTEGER T;

    T.QuadPart = -(LONGLONG)Ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &T);
}

static VOID CodecLogStatus(_In_ PAUD_DEVICE_CONTEXT Dev, _In_z_ PCSTR When)
{
    USHORT Raw2 = Rd(Dev, R_IRQ1_RAW_STATUS_2);

    AudLog(Dev->Log, "codec %s: FLL1 lock %u SYSCLK1 %04X OUT_EN1 %04X RAW_OUT1 %04X TONE1 %04X HP2 SC %X EN %X DIS %X HPD0 %04X\r\n",
           When, (Raw2 & FLL1_LOCK_STS) ? 1 : 0, Rd(Dev, R_SYSTEM_CLOCK_1), Rd(Dev, R_OUTPUT_ENABLES_1),
           Rd(Dev, R_RAW_OUTPUT_STATUS_1), Rd(Dev, R_TONE_GENERATOR_1), Rd(Dev, R_IRQ1_RAW_STATUS_12) & HP2_BOTH,
           Rd(Dev, R_IRQ1_RAW_STATUS_13) & HP2_BOTH, Rd(Dev, R_IRQ1_RAW_STATUS_14) & HP2_BOTH, Rd(Dev, R_HEADPHONE_DETECT_0));
}

static BOOLEAN CodecApplyPatch(_In_ PAUD_DEVICE_CONTEXT Dev)
{
    ULONG I, Bad16 = 0, Bad32 = 0, B32;

    for (I = 0; I < ARRAYSIZE(CodecPatch16); I++) {
        if (!NT_SUCCESS(CodecWrite(Dev, CodecPatch16[I][0], CodecPatch16[I][1])) || Rd(Dev, CodecPatch16[I][0]) != CodecPatch16[I][1]) {
            Bad16++;
        }
    }
    for (I = 0; I < ARRAYSIZE(CodecPatch32); I++) {
        B32 = 0;
        if (!NT_SUCCESS(CodecWrite32(Dev, CodecPatch32[I][0], CodecPatch32[I][1])) ||
            !NT_SUCCESS(CodecRead32(Dev, CodecPatch32[I][0], &B32)) || B32 != CodecPatch32[I][1]) {
            Bad32++;
        }
    }
    AudLog(Dev->Log, "codec: patch 16-bit %lu/%lu exact, 32-bit %lu/%lu\r\n", (ULONG)ARRAYSIZE(CodecPatch16) - Bad16,
           (ULONG)ARRAYSIZE(CodecPatch16), (ULONG)ARRAYSIZE(CodecPatch32) - Bad32, (ULONG)ARRAYSIZE(CodecPatch32));
    return Bad16 < ARRAYSIZE(CodecPatch16) || Bad32 < ARRAYSIZE(CodecPatch32);
}

/* madera_fllhj_disable for the FLL whose CONTROL_1 is at B + 1 (FLL1 B 0x170, FLL2 B 0x190); Lock = its IRQ1_RAW_STATUS_2 bit. */
VOID CodecFllDisable(_In_ PAUD_DEVICE_CONTEXT Dev, ULONG B, USHORT Lock)
{
    ULONG Ms;

    (VOID)CodecUpdate(Dev, B + 0xB, FLL_LOCKDET, 0);
    (VOID)CodecUpdate(Dev, B + 0x1, FLL_HOLD, FLL_HOLD);
    (VOID)CodecUpdate(Dev, B + 0x1, FLL_ENA, 0);
    for (Ms = 0; Ms < FLL_LOCK_MAX_MS && (Rd(Dev, R_IRQ1_RAW_STATUS_2) & Lock); Ms++) {
        KeStallExecutionProcessor(1000);
    }
    (VOID)CodecUpdate(Dev, B + 0x2, FLL_CTRL_UPD, 0);
}

/* madera_fllhj_enable + apply + wait_for_fll for the FLL at B (as CodecFllDisable). TRUE = locked. */
BOOLEAN CodecFllEnable(_In_ PAUD_DEVICE_CONTEXT Dev, ULONG B, USHORT Lock, _In_ const CODEC_FLL *C)
{
    ULONG Ms;

    (VOID)CodecUpdate(Dev, B + 0x1, FLL_HOLD, FLL_HOLD);
    (VOID)CodecWrite(Dev, B + 0x2, C->N);
    (VOID)CodecUpdate(Dev, B + 0x3, 0xFFFF, C->Theta);
    (VOID)CodecUpdate(Dev, B + 0x4, 0xFFFF, C->Lambda);
    (VOID)CodecUpdate(Dev, B + 0x5, FLL_FB_DIV_MASK, C->FbDiv);
    (VOID)CodecUpdate(Dev, B + 0x6, FLL_REFCLK_DIV_MASK, (USHORT)(C->RefDiv << FLL_REFCLK_DIV_SHIFT));
    (VOID)CodecUpdate(Dev, B + 0x8, 0xFFFF, C->Gains);
    (VOID)CodecUpdate(Dev, B + 0xA, FLL_HP_MASK | FLL_PHASEDET_ENA, (USHORT)((C->Hp << FLL_HP_SHIFT) | FLL_PHASEDET_ENA));
    (VOID)CodecUpdate(Dev, B + 0xB, FLL_LOCKDET_THR_MASK, (USHORT)(C->LockThr << FLL_LOCKDET_THR_SHIFT));
    (VOID)CodecUpdate(Dev, B + 0xD, FLL_DT1_MASK, C->FastClk);
    (VOID)CodecUpdate(Dev, B + 0x1, FLL_REFCLK_SRC_MASK, (USHORT)(C->RefSrc << FLL_REFCLK_SRC_SHIFT));
    (VOID)CodecUpdate(Dev, B + 0x1, FLL_ENA, FLL_ENA);
    (VOID)CodecUpdate(Dev, B + 0xB, FLL_LOCKDET, FLL_LOCKDET);
    (VOID)CodecUpdate(Dev, B + 0x2, FLL_CTRL_UPD, FLL_CTRL_UPD);
    (VOID)CodecUpdate(Dev, B + 0x1, FLL_HOLD, 0);
    for (Ms = 0; Ms <= FLL_LOCK_MAX_MS; Ms++) {
        if (Rd(Dev, R_IRQ1_RAW_STATUS_2) & Lock) {
            AudLog(Dev->Log, "codec: FLL@%03lX <- %s locked after %lu ms\r\n", B, C->Name, Ms);
            return TRUE;
        }
        KeStallExecutionProcessor(1000);
    }
    AudLog(Dev->Log, "codec: FLL@%03lX <- %s NO LOCK\r\n", B, C->Name);
    return FALSE;
}

/* Once per codec reset (madera-core probe order): cs47l92 patch, then the 32 kHz clock from MCLK2 (jack detection and
   the FLLs need it). */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecSetup(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    PAGED_CODE();

    if (!Dev->CodecUp) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (!CodecApplyPatch(Dev)) {
        return STATUS_IO_DEVICE_ERROR;
    }
    return CodecUpdate(Dev, R_CLOCK_32K_1, CLK_32K_ENA | CLK_32K_SRC_MASK, CLK_32K_ENA | CLK_32K_SRC_MCLK2);
}

/* 48 kHz rate + SYSCLK 98.304 MHz from FLL1 (MCLK2, fallback MCLK1), SYSCLK on. CodecSetup must have run. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecClockOn(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    ULONG I;

    PAGED_CODE();

    if (!Dev->CodecUp) {
        return STATUS_DEVICE_NOT_READY;
    }
    (VOID)CodecUpdate(Dev, R_SAMPLE_RATE_1, 0x001F, SAMPLE_RATE_48K);
    (VOID)CodecUpdate(Dev, R_SYSTEM_CLOCK_1, SYSCLK_SET_MASK, SYSCLK_98MHZ_FLL1);
    if (!CodecFllEnable(Dev, CODEC_FLL1_BASE, CODEC_FLL1_LOCK, &CodecFllMclk2)) {
        CodecFllDisable(Dev, CODEC_FLL1_BASE, CODEC_FLL1_LOCK);
        if (!CodecFllEnable(Dev, CODEC_FLL1_BASE, CODEC_FLL1_LOCK, &CodecFllMclk1)) {
            CodecFllDisable(Dev, CODEC_FLL1_BASE, CODEC_FLL1_LOCK);
            return STATUS_IO_TIMEOUT;
        }
    }
    (VOID)CodecUpdate(Dev, R_SYSTEM_CLOCK_1, SYSCLK_ENA, SYSCLK_ENA);
    for (I = 0; I < 4; I++) {
        (VOID)Rd(Dev, R_SOFTWARE_RESET);   /* madera_spin_sysclk */
    }
    KeStallExecutionProcessor(300);
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID CodecClockOff(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    PAGED_CODE();

    if (Dev->CodecUp) {
        (VOID)CodecUpdate(Dev, R_SYSTEM_CLOCK_1, SYSCLK_ENA, 0);
        CodecFllDisable(Dev, CODEC_FLL1_BASE, CODEC_FLL1_LOCK);
    }
}

/* J2 diagnostic: tone generator 1 -> OUT2 mixers -> HPOUT2 for Ms at -32 dBFS, teardown in DAPM order. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecTone(_Inout_ PAUD_DEVICE_CONTEXT Dev, ULONG Ms)
{
    NTSTATUS Status;
    USHORT   Hpd;
    BOOLEAN  Played = FALSE;

    PAGED_CODE();

    Status = CodecClockOn(Dev);
    AudLog(Dev->Log, "codec tone: clock on %08lX\r\n", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    (VOID)CodecUpdate(Dev, R_OUT2LMIX_INPUT_1_SRC, 0x00FF, MIX_SRC_TONE1);
    (VOID)CodecUpdate(Dev, R_OUT2RMIX_INPUT_1_SRC, 0x00FF, MIX_SRC_TONE1);
    (VOID)CodecUpdate(Dev, R_OUT2LMIX_INPUT_1_VOL, 0x00FE, TONE_MIX_VOL_REG);
    (VOID)CodecUpdate(Dev, R_OUT2RMIX_INPUT_1_VOL, 0x00FE, TONE_MIX_VOL_REG);
    (VOID)CodecUpdate(Dev, R_DAC_DIGITAL_VOLUME_2L, DAC_VU | DAC_MUTE | 0x00FF, DAC_VU | TONE_DAC_VOL);
    (VOID)CodecUpdate(Dev, R_DAC_DIGITAL_VOLUME_2R, DAC_VU | DAC_MUTE | 0x00FF, DAC_VU | TONE_DAC_VOL);
    (VOID)CodecUpdate(Dev, R_TONE_GENERATOR_1, 0xFFFF, TONE1_ENA);

    Hpd = Rd(Dev, R_HEADPHONE_DETECT_0);
    if (Hpd & 0x8000) {
        AudLog(Dev->Log, "codec tone: HP clamp set (HPD0 %04X) - outputs stay off\r\n", Hpd);
    } else {
        (VOID)CodecUpdate(Dev, R_OUTPUT_ENABLES_1, OUT2_ENA_BOTH, OUT2_ENA_BOTH);
        KeStallExecutionProcessor(OUT_UP_DELAY_US);
        CodecLogStatus(Dev, "tone outputs on");
        if (Rd(Dev, R_IRQ1_RAW_STATUS_12) & HP2_BOTH) {
            AudLog(Dev->Log, "codec tone: HP2 short-circuit status - outputs off at once\r\n");
        } else {
            SleepMs(Ms / 2);
            CodecLogStatus(Dev, "mid-tone");
            SleepMs(Ms - Ms / 2);
            Played = TRUE;
        }
    }

    (VOID)CodecUpdate(Dev, R_OUTPUT_ENABLES_1, OUT2_ENA_BOTH, 0);
    KeStallExecutionProcessor(OUT_DOWN_DELAY_US);
    (VOID)CodecUpdate(Dev, R_TONE_GENERATOR_1, 0xFFFF, 0);
    (VOID)CodecUpdate(Dev, R_DAC_DIGITAL_VOLUME_2L, DAC_VU | DAC_MUTE | 0x00FF, DAC_VU | DAC_MUTE | DAC_VOL_0DB);
    (VOID)CodecUpdate(Dev, R_DAC_DIGITAL_VOLUME_2R, DAC_VU | DAC_MUTE | 0x00FF, DAC_VU | DAC_MUTE | DAC_VOL_0DB);
    (VOID)CodecUpdate(Dev, R_OUT2LMIX_INPUT_1_SRC, 0x00FF, 0);
    (VOID)CodecUpdate(Dev, R_OUT2RMIX_INPUT_1_SRC, 0x00FF, 0);
    (VOID)CodecUpdate(Dev, R_OUT2LMIX_INPUT_1_VOL, 0x00FE, MIX_VOL_0DB_REG);
    (VOID)CodecUpdate(Dev, R_OUT2RMIX_INPUT_1_VOL, 0x00FE, MIX_VOL_0DB_REG);
    CodecClockOff(Dev);
    CodecLogStatus(Dev, "tone teardown");
    AudLog(Dev->Log, "codec tone: %s\r\n", Played ? "PLAYED" : "NOT PLAYED");
    return Played ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
