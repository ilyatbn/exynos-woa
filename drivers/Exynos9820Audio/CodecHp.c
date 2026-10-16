/*
 * CodecHp.c - CS47L93 headphone playback path: ABOX UAIF0 -> AIF1 -> OUT2 -> HPOUT2 (notes/d8-jack-windows.md J3).
 *
 * Link (research notes/d8-jack-research.md Q6 option b): UAIF0 is ABOX master, DSP-A, 4 slots x 32 bit, 48 kHz =
 * BCLK 6.144 MHz, the same frame and BCLK polarity as the proven UAIF1 speaker link (UAIF CTRL1 0x1B4DF7DF has
 * BCLK_POLARITY 0 = inverted per abox_if.c), so the codec takes an inverted BCLK too. Stock runs I2S 2 x 24 instead
 * (Q1); the 4-slot frame keeps the RDMA0 -> SIFS1 chain unchanged. Slots 0/1 = left/right.
 *
 * Clocking as stock (Q1): AIF1, the OUT mixers and the DACs run on ASYNCCLK = FLL2 locked to AIF1BCLK, so the codec
 * follows the ABOX clock exactly; SYSCLK (FLL1 <- MCLK2, CodecClockOn) stays up for the rest of the chip.
 * Every codec value below is VERIFIED-by-code (madera.c set_fmt / hw_params / set_tdm_slot, fllhj_apply) in the
 * research notes, NOT device-tested before this driver.
 */
#include "Exynos9820Audio.h"

#define R_ASYNC_CLOCK_1          0x0112u
#define   ASYNC_98M_FLL2_ENA       0x0445u   /* FREQ 4 (98.304 MHz) | ENA 0x40 | SRC 5 (FLL2) */
#define   ASYNC_ENA                0x0040u
#define R_ASYNC_SAMPLE_RATE_1    0x0113u
#define   ASYNC_RATE_48K           0x0003u
#define R_OUTPUT_ENABLES_1       0x0400u
#define   OUT2_ENA_BOTH            0x000Cu
#define R_OUTPUT_RATE_1          0x0408u
#define   OUT_RATE_ASYNC           0x4041u   /* OUT_RATE 8 (ASYNC rate 1) | CP_DAC_MODE bit6 | OUT_CLK_SRC 1 (ASYNCCLK) */
#define R_DAC_DIGITAL_VOLUME_2L  0x0419u
#define R_DAC_DIGITAL_VOLUME_2R  0x041Du
#define   DAC_VU                   0x0200u
#define   DAC_MUTE                 0x0100u
#define   DAC_VOL_MASK             0x00FFu
#define   DAC_VOL_STOCK_HP         0x006Bu   /* stock gain-media-headset "HPOUT2L/R Impedance Volume" 107 = -10.5 dB */
#define   DAC_VOL_0DB              0x0080u
#define R_HEADPHONE_DETECT_0     0x0299u
#define   HPD_CLAMP                0x8000u
#define R_AIF1_BCLK_CTRL         0x0500u
#define   AIF1_BCLK_INV_6M144      0x008Fu   /* BCLK_INV 0x80 | BCLK_FREQ 15 = 6.144 MHz (32 x 4 x 48 k), slave */
#define   AIF1_BCLK_MSTR           0x0020u   /* MADERA_AIF1_BCLK_MSTR (registers.h:5007) */
#define R_AIF1_TX_PIN_CTRL       0x0501u
#define   AIF1_LRCLK_MSTR          0x0001u   /* MADERA_AIF1TX/RX_LRCLK_MSTR (registers.h:5032, 5046) */
#define R_AIF1_RX_PIN_CTRL       0x0502u
#define R_AIF1_RATE_CTRL         0x0503u
#define   AIF1_RATE_ASYNC1         0x4000u   /* RATE 8 = ASYNCCLK rate 1 */
#define   AIF1_RATE_SYSCLK1        0x0000u   /* RATE 0 = SAMPLE_RATE_1 on SYSCLK (48 kHz, CodecClockOn) */
#define R_AIF1_FORMAT            0x0504u
#define   AIF1_FMT_DSP_A           0x0000u
#define R_AIF1_RX_BCLK_RATE      0x0506u
#define   AIF1_BCPF_128            0x0080u
#define R_AIF1_FRAME_CTRL_1      0x0507u
#define R_AIF1_FRAME_CTRL_2      0x0508u
#define   AIF1_FRAME_32_32         0x2020u   /* word length 32 | slot length 32 */
#define R_AIF1_FRAME_CTRL_11     0x0511u     /* RX slot of AIF1RX1 */
#define R_AIF1_FRAME_CTRL_12     0x0512u     /* RX slot of AIF1RX2 */
#define R_AIF1_RX_ENABLES        0x051Au
#define   AIF1_RX12                0x0003u
#define R_OUT2LMIX_INPUT_1_SRC   0x0690u
#define R_OUT2RMIX_INPUT_1_SRC   0x0698u
#define   MIX_SRC_AIF1RX1          0x0020u
#define   MIX_SRC_AIF1RX2          0x0021u
#define R_GPIO5_CTRL_1           0x1708u     /* GPIO5..8 CTRL_1 at 0x1708/0x170A/0x170C/0x170E */
#define   GPIO_FN_AIF              0x2000u   /* reset 0x2001 = plain GPIO; AIF1 needs the alternate function */
#define R_IRQ1_RAW_STATUS_2      0x1881u
#define R_IRQ1_RAW_STATUS_12     0x188Bu
#define   HP2_SC_BOTH              0x000Cu

#define OUT_UP_DELAY_US          12000u
#define OUT_DOWN_DELAY_US        2000u

/* FLL2 <- AIF1BCLK 6.144 MHz -> 49.152 MHz (ASYNCCLK 98.304): refdiv 0, integer N 16, HIGH gains (research Q6). */
static const CODEC_FLL CodecFll2Bclk = { "AIF1BCLK (6.144 MHz)", 8, 16, 0, 1, 1, 0, 0x21F0, 0, 8, 0 };

static USHORT Rd(_In_ PAUD_DEVICE_CONTEXT Dev, ULONG Reg)
{
    USHORT V = 0;

    (VOID)CodecRead(Dev, Reg, &V);
    return V;
}

/* Before the ABOX DMA starts: SYSCLK, AIF1 pins + format + rates. Clocks that need BCLK come in CodecHpStart. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecHpPrepare(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    NTSTATUS Status;
    ULONG    I;

    PAGED_CODE();

    Status = CodecClockOn(Dev);
    if (!NT_SUCCESS(Status)) {
        AudLog(Dev->Log, "hp: SYSCLK not up %08lX\r\n", (ULONG)Status);
        return Status;
    }
    for (I = 0; I < 4; I++) {
        (VOID)CodecWrite(Dev, R_GPIO5_CTRL_1 + 2 * I, GPIO_FN_AIF);
    }
    (VOID)CodecWrite(Dev, R_AIF1_FORMAT, AIF1_FMT_DSP_A);
    /* HpMaster 1 (CBM_CFM, madera_set_fmt): BCLK_MSTR 0x20 + LRCLK_MSTR 0x0001 in TX/RX pin ctrl, AIF1 on SYSCLK */
    (VOID)CodecWrite(Dev, R_AIF1_BCLK_CTRL, AIF1_BCLK_INV_6M144 | (Dev->Cfg.HpMaster ? AIF1_BCLK_MSTR : 0));
    (VOID)CodecWrite(Dev, R_AIF1_TX_PIN_CTRL, Dev->Cfg.HpMaster ? AIF1_LRCLK_MSTR : 0);
    (VOID)CodecWrite(Dev, R_AIF1_RX_PIN_CTRL, Dev->Cfg.HpMaster ? AIF1_LRCLK_MSTR : 0);
    (VOID)CodecWrite(Dev, R_AIF1_RX_BCLK_RATE, AIF1_BCPF_128);
    (VOID)CodecWrite(Dev, R_AIF1_FRAME_CTRL_1, AIF1_FRAME_32_32);
    (VOID)CodecWrite(Dev, R_AIF1_FRAME_CTRL_2, AIF1_FRAME_32_32);
    (VOID)CodecWrite(Dev, R_AIF1_FRAME_CTRL_11, 0);
    (VOID)CodecWrite(Dev, R_AIF1_FRAME_CTRL_12, 1);
    (VOID)CodecWrite(Dev, R_ASYNC_SAMPLE_RATE_1, ASYNC_RATE_48K);
    (VOID)CodecWrite(Dev, R_AIF1_RATE_CTRL, Dev->Cfg.HpMaster ? AIF1_RATE_SYSCLK1 : AIF1_RATE_ASYNC1);
    Dev->HpSavedOutRate = Rd(Dev, R_OUTPUT_RATE_1);
    AudLog(Dev->Log, "hp: prepared: GPIO5 %04X AIF1 BCLK %04X FMT %04X BCPF %04X FRAME %04X/%04X SLOTS %u/%u RATE %04X ASR1 %04X\r\n",
           Rd(Dev, R_GPIO5_CTRL_1), Rd(Dev, R_AIF1_BCLK_CTRL), Rd(Dev, R_AIF1_FORMAT), Rd(Dev, R_AIF1_RX_BCLK_RATE),
           Rd(Dev, R_AIF1_FRAME_CTRL_1), Rd(Dev, R_AIF1_FRAME_CTRL_2), Rd(Dev, R_AIF1_FRAME_CTRL_11), Rd(Dev, R_AIF1_FRAME_CTRL_12),
           Rd(Dev, R_AIF1_RATE_CTRL), Rd(Dev, R_ASYNC_SAMPLE_RATE_1));
    return STATUS_SUCCESS;
}

/* With UAIF0 running (BCLK present): FLL2 lock, ASYNCCLK, output rate, mixers, AIF1 RX, OUT2 on, unmute. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecHpStart(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    PAGED_CODE();

    /* AIF1 RX on before the FLL2 lock wait: with RX off the AIF1 pins read constant 0 and FLL2 never locked although
       gpb0-1/2 toggle on the AP side (1.0.1.27). Stock gets away with the opposite order because madera_fllhj_enable's
       wait only warns and DAPM powers AIF1RX right after. */
    (VOID)CodecWrite(Dev, R_AIF1_RX_ENABLES, AIF1_RX12);
    if (Dev->Cfg.HpMaster) {
        AboxSamplePins(Dev, "hp codec-master");   /* does the codec's BCLK/LRCLK reach the AP pins? */
    } else if (!CodecFllEnable(Dev, CODEC_FLL2_BASE, CODEC_FLL2_LOCK, &CodecFll2Bclk)) {
        USHORT AndV[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF }, OrV[4] = { 0 };
        ULONG  I, P;

        /* Diagnostic: is anything toggling on the AIF1 pins (GPIO5..8)? GPn_LVL = bit15 of GPIOn_CTRL_1 (madera
           registers.h); 64 SPI reads ~20 us apart see both levels on a running 6.144 MHz BCLK / 48 kHz LRCLK. */
        for (I = 0; I < 64; I++) {
            for (P = 0; P < 4; P++) {
                USHORT V = Rd(Dev, R_GPIO5_CTRL_1 + 2 * P);

                AndV[P] &= V;
                OrV[P]  |= V;
            }
        }
        AudLog(Dev->Log, "hp: AIF1 pin levels over 64 reads (bit15 and/or): GPIO5 %u/%u GPIO6 %u/%u GPIO7 %u/%u GPIO8 %u/%u\r\n",
               AndV[0] >> 15, OrV[0] >> 15, AndV[1] >> 15, OrV[1] >> 15, AndV[2] >> 15, OrV[2] >> 15, AndV[3] >> 15, OrV[3] >> 15);
        AudLog(Dev->Log, "hp: GPIO5-8 CTRL_1/CTRL_2 %04X/%04X %04X/%04X %04X/%04X %04X/%04X | FLL2 191-19D: %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X | RAW1-3 %04X %04X %04X\r\n",
               Rd(Dev, 0x1708), Rd(Dev, 0x1709), Rd(Dev, 0x170A), Rd(Dev, 0x170B), Rd(Dev, 0x170C), Rd(Dev, 0x170D), Rd(Dev, 0x170E), Rd(Dev, 0x170F),
               Rd(Dev, 0x191), Rd(Dev, 0x192), Rd(Dev, 0x193), Rd(Dev, 0x194), Rd(Dev, 0x195), Rd(Dev, 0x196), Rd(Dev, 0x197),
               Rd(Dev, 0x198), Rd(Dev, 0x199), Rd(Dev, 0x19A), Rd(Dev, 0x19B), Rd(Dev, 0x19C), Rd(Dev, 0x19D),
               Rd(Dev, 0x1880), Rd(Dev, 0x1881), Rd(Dev, 0x1882));
        CodecFllDisable(Dev, CODEC_FLL2_BASE, CODEC_FLL2_LOCK);
        /* Control: does FLL2 lock at all? Same MCLK2 32 kHz config as the proven FLL1 (CodecTone.c CodecFllMclk2). */
        {
            static const CODEC_FLL Fll2Mclk2 = { "MCLK2 (32 kHz) control", 1, 375, 0, 1, 4, 0, 0x23F0, 0, 2, 0 };

            AudLog(Dev->Log, "hp: control - FLL2 from MCLK2: %s\r\n",
                   CodecFllEnable(Dev, CODEC_FLL2_BASE, CODEC_FLL2_LOCK, &Fll2Mclk2) ? "LOCKED (FLL2 works; no BCLK at AIF1)" : "no lock either");
            CodecFllDisable(Dev, CODEC_FLL2_BASE, CODEC_FLL2_LOCK);
        }
        (VOID)CodecWrite(Dev, R_AIF1_RX_ENABLES, 0);
        AudLog(Dev->Log, "hp: FLL2 did not lock to AIF1BCLK (RAW2 %04X) - no BCLK from UAIF0? outputs stay off\r\n",
               Rd(Dev, R_IRQ1_RAW_STATUS_2));
        return STATUS_IO_TIMEOUT;
    }
    if (!Dev->Cfg.HpMaster) {
        Dev->HpFll2On = TRUE;
        (VOID)CodecWrite(Dev, R_ASYNC_CLOCK_1, ASYNC_98M_FLL2_ENA);
        (VOID)CodecWrite(Dev, R_OUTPUT_RATE_1, OUT_RATE_ASYNC);       /* OUT2 still off here */
    }
    (VOID)CodecUpdate(Dev, R_OUT2LMIX_INPUT_1_SRC, 0x00FF, MIX_SRC_AIF1RX1);
    (VOID)CodecUpdate(Dev, R_OUT2RMIX_INPUT_1_SRC, 0x00FF, MIX_SRC_AIF1RX2);
    if (Rd(Dev, R_HEADPHONE_DETECT_0) & HPD_CLAMP) {
        AudLog(Dev->Log, "hp: HP clamp set (HPD0 %04X) - outputs stay off\r\n", Rd(Dev, R_HEADPHONE_DETECT_0));
        return STATUS_DEVICE_NOT_READY;
    }
    (VOID)CodecUpdate(Dev, R_OUTPUT_ENABLES_1, OUT2_ENA_BOTH, OUT2_ENA_BOTH);
    KeStallExecutionProcessor(OUT_UP_DELAY_US);
    if (Rd(Dev, R_IRQ1_RAW_STATUS_12) & HP2_SC_BOTH) {
        (VOID)CodecUpdate(Dev, R_OUTPUT_ENABLES_1, OUT2_ENA_BOTH, 0);
        AudLog(Dev->Log, "hp: HP2 short-circuit status - outputs off\r\n");
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    (VOID)CodecWrite(Dev, R_DAC_DIGITAL_VOLUME_2L, DAC_VU | DAC_VOL_STOCK_HP);
    (VOID)CodecWrite(Dev, R_DAC_DIGITAL_VOLUME_2R, DAC_VU | DAC_VOL_STOCK_HP);
    Dev->HpOn = TRUE;
    AudLog(Dev->Log, "hp: ON: ASYNC1 %04X OUTRATE %04X MIX %04X/%04X RXEN %04X OUT_EN1 %04X DAC %04X/%04X RAW2 %04X\r\n",
           Rd(Dev, R_ASYNC_CLOCK_1), Rd(Dev, R_OUTPUT_RATE_1), Rd(Dev, R_OUT2LMIX_INPUT_1_SRC), Rd(Dev, R_OUT2RMIX_INPUT_1_SRC),
           Rd(Dev, R_AIF1_RX_ENABLES), Rd(Dev, R_OUTPUT_ENABLES_1), Rd(Dev, R_DAC_DIGITAL_VOLUME_2L),
           Rd(Dev, R_DAC_DIGITAL_VOLUME_2R), Rd(Dev, R_IRQ1_RAW_STATUS_2));
    return STATUS_SUCCESS;
}

/* Stock shutdown order (beyond_madera.c:254-279): outputs off, then ASYNCCLK + FLL2, then the rest back to defaults.
   Must run while UAIF0 still clocks (before AboxStop). */
_IRQL_requires_(PASSIVE_LEVEL)
VOID CodecHpStop(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    PAGED_CODE();

    if (!Dev->CodecUp) {
        return;
    }
    (VOID)CodecWrite(Dev, R_DAC_DIGITAL_VOLUME_2L, DAC_VU | DAC_MUTE | DAC_VOL_0DB);
    (VOID)CodecWrite(Dev, R_DAC_DIGITAL_VOLUME_2R, DAC_VU | DAC_MUTE | DAC_VOL_0DB);
    (VOID)CodecUpdate(Dev, R_OUTPUT_ENABLES_1, OUT2_ENA_BOTH, 0);
    KeStallExecutionProcessor(OUT_DOWN_DELAY_US);
    (VOID)CodecWrite(Dev, R_AIF1_RX_ENABLES, 0);
    (VOID)CodecUpdate(Dev, R_OUT2LMIX_INPUT_1_SRC, 0x00FF, 0);
    (VOID)CodecUpdate(Dev, R_OUT2RMIX_INPUT_1_SRC, 0x00FF, 0);
    (VOID)CodecUpdate(Dev, R_ASYNC_CLOCK_1, ASYNC_ENA, 0);
    if (Dev->HpFll2On) {
        CodecFllDisable(Dev, CODEC_FLL2_BASE, CODEC_FLL2_LOCK);
        Dev->HpFll2On = FALSE;
    }
    if (Dev->HpSavedOutRate != 0) {
        (VOID)CodecWrite(Dev, R_OUTPUT_RATE_1, Dev->HpSavedOutRate);
    }
    CodecClockOff(Dev);
    Dev->HpOn = FALSE;
    AudLog(Dev->Log, "hp: off\r\n");
}
