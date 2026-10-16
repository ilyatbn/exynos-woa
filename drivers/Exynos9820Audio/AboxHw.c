/*
 * AboxHw.c - the ABOX hardware layer: everything the render stream needs from ABOX goes through
 * AboxInit / AboxStart / AboxStop / AboxRingPosition / AboxRelease.
 *
 * Sequence = the device-validated UEFI code, main 24fb796
 * firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteAudioProbeDxe/AudioSpeaker.c (boot #99: loudspeaker tone heard
 * by the PC mic, SNR 44-51 dB; notes/d8-audio-m3-abox.md "boot #99 RESULT"):
 *   stage 1 (SpeakerTonePlay :540-556): DIV_UAIF1 = 95 (AUDIF left at sboot's /2 -> 6.144 MHz BCLK), gpb1 pins
 *     function 2, SYSREG MISC_CON 1, SYSPOWER_CTRL 1, QCHANNEL_DISABLE bit3, UAIF1 CTRL1 DSP-A 4 x 32, CTRL0 master,
 *     SBANK_RDMA0, ROUTE_CTRL0 UAIF1 <- SIFS1;
 *   stage 2 (:558, VariantStart :439-444): UAIF1 SPK_EN, RDMA0 ring by PHYSICAL address (AUD SysMMU bypassed),
 *     VOL_FACTOR 0x800000, CTRL0 BURST 8 | FORMAT 0x1B (4 x 32) | ENABLE;
 *   teardown (:600-604): SPK_EN off, CTRL0 back to 0x00400000, wait for STATUS bit31 (progress) to clear.
 * Every stage-1 register is saved before it is written and restored by AboxRelease (UEFI :530-538, :606-619).
 * RDMA0 STATUS: [27:12] current period start (16-byte units), [11:0] frames into that period (run audD, A12;
 * boot #99 saw a one-period ring, where [27:12] stays 0 and [11:0] looked like a free frame counter).
 *
 * Rule (abox notes F5): nothing in the AUD block is touched unless PMU AUD_STATUS bit0 reads 1 first.
 * Validated in UEFI only; NOT VALIDATED UNDER WINDOWS (stage Guided).
 */

#include "Exynos9820Audio.h"

/* ABOX IFR offsets (abox notes F2; AudioSpeaker.c :90-113). */
#define ABOX_IP_INDEX            0x0000u
#define ABOX_IP_INDEX_ABOX       0x41424F58u   /* "ABOX"; boot #96 read it */
#define ABOX_VERSION             0x0004u
#define ABOX_SYSPOWER_CTRL       0x0010u
#define ABOX_SYSPOWER_STATUS     0x0014u
#define ABOX_QCHANNEL_DISABLE    0x0038u
#define   ABOX_QCH_BCLK_UAIF1      0x00000008u   /* bit3: UAIF1 BCLK keep-alive */
#define ABOX_ROUTE_CTRL0         0x0040u
#define   ABOX_ROUTE_UAIF1_SIFS1   0x00000020u   /* [7:4] = 2 "SIFS1" */
#define ABOX_SPUS_CTRL0          0x0200u
#define ABOX_SPUS_CTRL1          0x0204u
#define ABOX_SPUS_SBANK_RDMA0    0x0220u
#define   ABOX_SBANK_RDMA0_VAL     0x02000000u   /* (512 << 16) | 0, patch_9820 */
#define   ABOX_QCH_BCLK_UAIF0      0x00000004u   /* bit2: UAIF0 BCLK keep-alive (research Q2, abox.h:157-167) */
#define   ABOX_ROUTE_UAIF0_MASK    0x0000000Fu   /* [3:0] "ABOX UAIF0 SPK" (abox_cmpnt_v20.c:3354-3361) */
#define   ABOX_ROUTE_UAIF0_SIFS1   0x00000002u
#define   ABOX_ROUTE_UAIF1_MASK    0x000000F0u
#define ABOX_UAIF0_CTRL0         0x0500u       /* abox_soc.h:305-355 */
#define ABOX_UAIF0_CTRL1         0x0504u
#define ABOX_UAIF0_STATUS        0x050Cu
#define ABOX_UAIF1_CTRL0         0x0510u
#define   ABOX_UAIF1_CTRL0_MST     0x01000014u   /* reset 0x01000010 + MODE (ABOX master) */
#define   ABOX_UAIF_SPK_ENABLE     0x00000001u
#define ABOX_UAIF1_CTRL1         0x0514u
#define   ABOX_UAIF1_CTRL1_4X32    0x1B4DF7DFu   /* DSP-A, 4 slots x 32 bit */
#define ABOX_UAIF1_STATUS        0x051Cu
#define ABOX_RDMA0_CTRL0         0x1000u
#define   ABOX_RDMA_CTRL0_RESET    0x00400000u   /* BURST_LEN 8 (audit) */
#define   ABOX_RDMA_FORMAT_4X32    (0x1Bu << 8)  /* FORMAT [12:8]; boot #99 proves the 2.1 position on 2.0.1 */
#define   ABOX_RDMA_ENABLE         0x00000001u
#define ABOX_RDMA0_BUF_STR       0x1008u
#define ABOX_RDMA0_BUF_END       0x100Cu
#define ABOX_RDMA0_BUF_OFFSET    0x1010u
#define ABOX_RDMA0_STR_POINT     0x1014u
#define ABOX_RDMA0_VOL_FACTOR    0x1018u
#define   ABOX_RDMA_VOL_UNITY      0x00800000u   /* boot #99: correct level */
#define ABOX_RDMA0_STATUS        0x1030u
#define   ABOX_RDMA_PROGRESS       0x80000000u
#define   ABOX_RDMA_RBUF_CNT_MASK  0x00000FFFu   /* RBUF_CNT: frames inside the current period (run audD, A12) */
#define   ABOX_RDMA_RBUF_OFF_SHIFT 12           /* RBUF_OFFSET 27:12 (2.0.1 layout, notes/d8-audio-m3-abox.md:55) */
#define   ABOX_RDMA_RBUF_OFF_MASK  0xFFFFu       /*   in 16-byte units = frame index of the period start (audD) */

/* CMU_AUD / GPIO_AUD / SYSREG_AUD (AudioSpeaker.c :115-125). */
#define CMU_AUD_DIV_UAIF1        0x1830u
#define   CMU_DIV_MASK             0x000001FFu
#define   CMU_DIV_BUSY             0x00010000u
#define   CMU_DIV_UAIF1_6M144      95u           /* AUDIF 589.824 MHz (sboot /2) / 96 = 6.144 MHz */
#define CMU_AUD_MUX_UAIF0        0x100Cu       /* SELECT bit0 {0 DIV, 1 CLKIO = BCLK pin}, BUSY bit16 (research Q2) */
#define   CMU_MUX_SELECT           0x00000001u
#define   CMU_MUX_BUSY             0x00010000u
#define   ABOX_UAIF_CTRL0_SLAVE    0x01000010u   /* reset value, MODE bit2 clear */
#define CMU_AUD_DIV_UAIF0        0x182Cu       /* cmucal-sfr.c:3398 */
#define CMU_AUD_GATE_UAIF0_A     0x2020u       /* read only */
#define CMU_AUD_GATE_UAIF0_B     0x20A4u
#define GPIO_AUD_GPB0_CON        0x0000u       /* gpb0 = first GPIO_AUD bank (gpb1 at +0x20) */
#define   GPB0_UAIF0_MASK          0x000FFFF0u   /* gpb0-1..4 aud_i2s0_bus; gpb0-0 = codec MCLK, untouched */
#define   GPB0_UAIF0_FUNC          0x00022220u
#define GPIO_AUD_GPB1_CON        0x0020u
#define   GPB1_UAIF1_MASK          0x0000FFFFu
#define   GPB1_UAIF1_FUNC          0x00002222u   /* gpb1-0..3 function 2 (aud_i2s1_bus) */
#define SYSREG_MISC_CON          0x0504u

typedef enum _ABOX_OP {
    ABOX_OP_END = 0,
    ABOX_OP_WRITE,      /* Va[Region] + Reg = Value */
    ABOX_OP_RMW,        /* (old & ~Mask) | (Value & Mask) */
    ABOX_OP_POLL_CLEAR, /* wait until (read & Mask) == 0, TimeoutUs */
} ABOX_OP;

typedef struct _ABOX_STEP {
    ABOX_OP  Op;
    AUD_RES  Region;
    ULONG    Reg;
    ULONG    Mask;
    ULONG    Value;
    ULONG    TimeoutUs;
    PCSTR    Name;
} ABOX_STEP;

/* Stage 1, in the validated order (AudioSpeaker.c :541-556). WRITE/RMW rows are saved and restored. */
static const ABOX_STEP AboxStage1Seq[] = {
    { ABOX_OP_RMW,        AUD_RES_CMU_AUD,     CMU_AUD_DIV_UAIF1,     CMU_DIV_MASK,     CMU_DIV_UAIF1_6M144, 0,    "DIV_UAIF1" },
    { ABOX_OP_POLL_CLEAR, AUD_RES_CMU_AUD,     CMU_AUD_DIV_UAIF1,     CMU_DIV_BUSY,     0,                   1000, "DIV_UAIF1 busy" },
    { ABOX_OP_RMW,        AUD_RES_GPIO_AUD,    GPIO_AUD_GPB1_CON,     GPB1_UAIF1_MASK,  GPB1_UAIF1_FUNC,     0,    "gpb1 CON" },
    { ABOX_OP_WRITE,      AUD_RES_ABOX_SYSREG, SYSREG_MISC_CON,       0,                1,                   0,    "MISC_CON" },
    { ABOX_OP_WRITE,      AUD_RES_ABOX_IFR,    ABOX_SYSPOWER_CTRL,    0,                1,                   0,    "SYSPOWER_CTRL" },
    { ABOX_OP_RMW,        AUD_RES_ABOX_IFR,    ABOX_QCHANNEL_DISABLE, ABOX_QCH_BCLK_UAIF1, ABOX_QCH_BCLK_UAIF1, 0,  "QCHANNEL_DISABLE" },
    { ABOX_OP_WRITE,      AUD_RES_ABOX_IFR,    ABOX_UAIF1_CTRL1,      0,                ABOX_UAIF1_CTRL1_4X32, 0,  "UAIF1_CTRL1" },
    { ABOX_OP_WRITE,      AUD_RES_ABOX_IFR,    ABOX_UAIF1_CTRL0,      0,                ABOX_UAIF1_CTRL0_MST, 0,   "UAIF1_CTRL0" },
    { ABOX_OP_WRITE,      AUD_RES_ABOX_IFR,    ABOX_SPUS_SBANK_RDMA0, 0,                ABOX_SBANK_RDMA0_VAL, 0,   "SBANK_RDMA0" },
    { ABOX_OP_WRITE,      AUD_RES_ABOX_IFR,    ABOX_ROUTE_CTRL0,      0,                ABOX_ROUTE_UAIF1_SIFS1, 0, "ROUTE_CTRL0" },
    { ABOX_OP_END,        AUD_RES_ABOX_IFR,    0, 0, 0, 0, NULL },
};

C_ASSERT(ARRAYSIZE(AboxStage1Seq) <= 16);   /* AUD_DEVICE_CONTEXT.AboxSaved */

/* Headphone link (notes/d8-jack-windows.md J3, research Q6 option b): UAIF0 with the UAIF1 frame - 6.144 MHz BCLK
   (same divider value as UAIF1), DSP-A 4 x 32, ABOX master. Applied once on the first headphone stream, restored
   by AboxRelease. */
static const ABOX_STEP AboxUaif0Seq[] = {
    { ABOX_OP_RMW,        AUD_RES_CMU_AUD,     CMU_AUD_DIV_UAIF0,     CMU_DIV_MASK,     CMU_DIV_UAIF1_6M144, 0,    "DIV_UAIF0" },
    { ABOX_OP_POLL_CLEAR, AUD_RES_CMU_AUD,     CMU_AUD_DIV_UAIF0,     CMU_DIV_BUSY,     0,                   1000, "DIV_UAIF0 busy" },
    { ABOX_OP_RMW,        AUD_RES_CMU_AUD,     CMU_AUD_MUX_UAIF0,     CMU_MUX_SELECT,   0,                   0,    "MUX_UAIF0" },  /* HpMaster 1: 1 */
    { ABOX_OP_POLL_CLEAR, AUD_RES_CMU_AUD,     CMU_AUD_MUX_UAIF0,     CMU_MUX_BUSY,     0,                   1000, "MUX_UAIF0 busy" },
    { ABOX_OP_RMW,        AUD_RES_GPIO_AUD,    GPIO_AUD_GPB0_CON,     GPB0_UAIF0_MASK,  GPB0_UAIF0_FUNC,     0,    "gpb0 CON" },
    { ABOX_OP_RMW,        AUD_RES_ABOX_IFR,    ABOX_QCHANNEL_DISABLE, ABOX_QCH_BCLK_UAIF0, ABOX_QCH_BCLK_UAIF0, 0,  "QCH UAIF0" },
    { ABOX_OP_WRITE,      AUD_RES_ABOX_IFR,    ABOX_UAIF0_CTRL1,      0,                ABOX_UAIF1_CTRL1_4X32, 0,  "UAIF0_CTRL1" },
    { ABOX_OP_WRITE,      AUD_RES_ABOX_IFR,    ABOX_UAIF0_CTRL0,      0,                ABOX_UAIF1_CTRL0_MST, 0,   "UAIF0_CTRL0" },  /* HpMaster 1: slave */
    { ABOX_OP_END,        AUD_RES_ABOX_IFR,    0, 0, 0, 0, NULL },
};

C_ASSERT(ARRAYSIZE(AboxUaif0Seq) <= 10);     /* AUD_DEVICE_CONTEXT.AboxUaif0Saved */

static __forceinline ULONG AudRd(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ AUD_RES Region, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Dev->Va[Region] + Off));
}

static __forceinline VOID AudWr(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ AUD_RES Region, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Dev->Va[Region] + Off), Val);
}

static NTSTATUS AboxPollClear(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ const ABOX_STEP *S)
{
    ULONG Us;

    for (Us = 0; (AudRd(Dev, S->Region, S->Reg) & S->Mask) != 0; Us += 10) {
        if (Us >= S->TimeoutUs) {
            AudLog(Dev->Log, "abox %s [%d+0x%lX] = 0x%08lX: still set after %lu us\r\n", S->Name, S->Region, S->Reg,
                   AudRd(Dev, S->Region, S->Reg), S->TimeoutUs);
            return STATUS_IO_TIMEOUT;
        }
        KeStallExecutionProcessor(10);
    }

    return STATUS_SUCCESS;
}

/* PMU AUD_STATUS bit0, read fresh every time (F5). */
static BOOLEAN AudPowerOn(_In_ PAUD_DEVICE_CONTEXT Dev)
{
    if (Dev->Va[AUD_RES_PMU_AUD] == NULL) {
        return FALSE;
    }

    return (AudRd(Dev, AUD_RES_PMU_AUD, AUD_PMU_AUD_STATUS) & 1u) != 0;
}

typedef struct _ABOX_AUDIT_REG {
    PCSTR   Name;
    AUD_RES Region;
    ULONG   Off;
} ABOX_AUDIT_REG;

/* = firmware/.../Beyond0LteAudioProbeDxe/AudioAbox.c mIfrRegs + mOtherRegs, minus SysMMU (not in our _CRS). */
static const ABOX_AUDIT_REG AboxAuditRegs[] = {
    { "VERSION",                 AUD_RES_ABOX_IFR,    ABOX_VERSION },
    { "SYSPOWER_STATUS",         AUD_RES_ABOX_IFR,    ABOX_SYSPOWER_STATUS },
    { "QCHANNEL_DISABLE",        AUD_RES_ABOX_IFR,    ABOX_QCHANNEL_DISABLE },
    { "ROUTE_CTRL0",             AUD_RES_ABOX_IFR,    ABOX_ROUTE_CTRL0 },
    { "SPUS_CTRL0",              AUD_RES_ABOX_IFR,    ABOX_SPUS_CTRL0 },
    { "SPUS_CTRL1",              AUD_RES_ABOX_IFR,    ABOX_SPUS_CTRL1 },
    { "SPUS_SBANK_RDMA0",        AUD_RES_ABOX_IFR,    ABOX_SPUS_SBANK_RDMA0 },
    { "UAIF1_CTRL0",             AUD_RES_ABOX_IFR,    ABOX_UAIF1_CTRL0 },
    { "UAIF1_CTRL1",             AUD_RES_ABOX_IFR,    ABOX_UAIF1_CTRL1 },
    { "UAIF1_STATUS",            AUD_RES_ABOX_IFR,    ABOX_UAIF1_STATUS },
    { "RDMA0_CTRL0",             AUD_RES_ABOX_IFR,    ABOX_RDMA0_CTRL0 },
    { "RDMA0_BUF_STR",           AUD_RES_ABOX_IFR,    ABOX_RDMA0_BUF_STR },
    { "RDMA0_BUF_END",           AUD_RES_ABOX_IFR,    ABOX_RDMA0_BUF_END },
    { "RDMA0_BUF_OFFSET",        AUD_RES_ABOX_IFR,    ABOX_RDMA0_BUF_OFFSET },
    { "RDMA0_STR_POINT",         AUD_RES_ABOX_IFR,    ABOX_RDMA0_STR_POINT },
    { "RDMA0_VOL_FACTOR",        AUD_RES_ABOX_IFR,    ABOX_RDMA0_VOL_FACTOR },
    { "RDMA0_STATUS",            AUD_RES_ABOX_IFR,    ABOX_RDMA0_STATUS },
    { "CMU_AUD PLL_AUD0_CON0",   AUD_RES_CMU_AUD,     0x0120 },
    { "CMU_AUD PLL_AUD0_CON3",   AUD_RES_CMU_AUD,     0x012C },
    { "CMU_AUD MUX_UAIF1",       AUD_RES_CMU_AUD,     0x1010 },
    { "CMU_AUD DIV_AUDIF",       AUD_RES_CMU_AUD,     0x1800 },
    { "CMU_AUD DIV_BUS",         AUD_RES_CMU_AUD,     0x1804 },
    { "CMU_AUD DIV_UAIF1",       AUD_RES_CMU_AUD,     CMU_AUD_DIV_UAIF1 },
    { "CMU_AUD GATE_ACLK",       AUD_RES_CMU_AUD,     0x2010 },
    { "CMU_AUD GATE_BCLK_UAIF1", AUD_RES_CMU_AUD,     0x2024 },
    { "SYSREG_AUD MISC_CON",     AUD_RES_ABOX_SYSREG, SYSREG_MISC_CON },
    { "GPIO_AUD gpb1 CON",       AUD_RES_GPIO_AUD,    GPIO_AUD_GPB1_CON },
    { "GPIO_AUD gpb1 PUD",       AUD_RES_GPIO_AUD,    0x0028 },
};

_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN AboxAudit(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    ULONG I;
    ULONG Ip;

    Dev->AudOn     = FALSE;
    Dev->AboxAlive = FALSE;
    if (!Dev->Mapped) {
        AudLog(Dev->Log, "abox audit: resources not mapped - skipped\r\n");
        return FALSE;
    }

    AudLog(Dev->Log, "abox audit (read-only): pd-aud CONFIGURATION %lx STATUS %lx (bit0 = on), pad %lx (bit11 = normal)\r\n",
           AudRd(Dev, AUD_RES_PMU_AUD, AUD_PMU_AUD_CONFIGURATION), AudRd(Dev, AUD_RES_PMU_AUD, AUD_PMU_AUD_STATUS),
           AudRd(Dev, AUD_RES_PMU_AUD, AUD_PMU_AUD_PAD));
    if (!AudPowerOn(Dev)) {
        AudLog(Dev->Log, "abox audit: pd-aud OFF - nothing in the AUD block is read; ABOX disabled for this start\r\n");
        return FALSE;
    }

    Dev->AudOn = TRUE;
    Ip = AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_IP_INDEX);
    AudLog(Dev->Log, "abox IP_INDEX = %08lX (%s)\r\n", Ip,
           (Ip == ABOX_IP_INDEX_ABOX) ? "\"ABOX\", non-secure access works" : "NOT \"ABOX\" - ABOX disabled");
    if (Ip != ABOX_IP_INDEX_ABOX) {
        return FALSE;
    }

    Dev->AboxAlive = TRUE;
    for (I = 0; I < ARRAYSIZE(AboxAuditRegs); I++) {
        AudLog(Dev->Log, "abox   %-24s = %08lX\r\n", AboxAuditRegs[I].Name,
               AudRd(Dev, AboxAuditRegs[I].Region, AboxAuditRegs[I].Off));
    }

    return TRUE;
}

/* Stage 1: UAIF1 6.144 MHz BCLK, pins, ABOX master DSP-A 4 x 32, RDMA0 -> SIFS1 -> UAIF1 route. Idempotent. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AboxInit(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG    I;
    ULONG    Old;

    if (Dev->AboxReady) {
        return STATUS_SUCCESS;
    }

    if (!Dev->AboxAlive || !AudPowerOn(Dev)) {
        AudLog(Dev->Log, "AboxInit: ABOX not alive or pd-aud off - refused\r\n");
        return STATUS_DEVICE_NOT_READY;
    }

    Dev->AboxSavedCount = 0;
    for (I = 0; AboxStage1Seq[I].Op != ABOX_OP_END; I++) {
        const ABOX_STEP *S = &AboxStage1Seq[I];

        if (S->Op == ABOX_OP_POLL_CLEAR) {
            Status = AboxPollClear(Dev, S);
            if (!NT_SUCCESS(Status)) {
                break;
            }
            continue;
        }

        Old = AudRd(Dev, S->Region, S->Reg);
        Dev->AboxSaved[I] = Old;
        Dev->AboxSavedCount = I + 1;
        AudWr(Dev, S->Region, S->Reg, (S->Op == ABOX_OP_RMW) ? ((Old & ~S->Mask) | (S->Value & S->Mask)) : S->Value);
        AudLog(Dev->Log, "abox %-16s 0x%08lX -> 0x%08lX\r\n", S->Name, Old, AudRd(Dev, S->Region, S->Reg));
    }

    AudLog(Dev->Log, "AboxInit: %08lX, UAIF1 CTRL0 %08lX CTRL1 %08lX STATUS %08lX\r\n", Status,
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0), AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL1),
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_STATUS));
    if (!NT_SUCCESS(Status)) {
        AboxRelease(Dev);
        return Status;
    }

    Dev->AboxReady = TRUE;
    return STATUS_SUCCESS;
}

/* Before AboxStart, with stage 1 done: RDMA0 -> SIFS1 to the headphone codec (UAIF0) or the amps (UAIF1), never both
   (SIFS1 feeding two UAIFs is UNVERIFIED). The UAIF0 link is set up on first use; its prior state is logged. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AboxSelectOutput(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ BOOLEAN Headphones)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG    I;
    ULONG    Old;
    ULONG    Route;

    if (!Dev->AboxReady || Dev->AboxRunning || !AudPowerOn(Dev)) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Headphones && Dev->AboxUaif0Count == 0) {
        AudLog(Dev->Log, "abox UAIF0 before: CTRL0 %08lX CTRL1 %08lX STATUS %08lX | MUX %08lX DIV %08lX GATE %08lX %08lX | gpb0 CON %08lX\r\n",
               AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF0_CTRL0), AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF0_CTRL1),
               AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF0_STATUS), AudRd(Dev, AUD_RES_CMU_AUD, CMU_AUD_MUX_UAIF0),
               AudRd(Dev, AUD_RES_CMU_AUD, CMU_AUD_DIV_UAIF0), AudRd(Dev, AUD_RES_CMU_AUD, CMU_AUD_GATE_UAIF0_A),
               AudRd(Dev, AUD_RES_CMU_AUD, CMU_AUD_GATE_UAIF0_B), AudRd(Dev, AUD_RES_GPIO_AUD, GPIO_AUD_GPB0_CON));
        for (I = 0; AboxUaif0Seq[I].Op != ABOX_OP_END; I++) {
            const ABOX_STEP *S = &AboxUaif0Seq[I];

            if (S->Op == ABOX_OP_POLL_CLEAR) {
                Status = AboxPollClear(Dev, S);
                if (!NT_SUCCESS(Status)) {
                    break;
                }
                continue;
            }
            Old = AudRd(Dev, S->Region, S->Reg);
            Dev->AboxUaif0Saved[I] = Old;
            Dev->AboxUaif0Count = I + 1;
            {
                ULONG Val = S->Value;   /* HpMaster 1 = codec drives BCLK/LRCLK: UAIF0 slave, clock from its BCLK pin */

                if (Dev->Cfg.HpMaster && S->Reg == CMU_AUD_MUX_UAIF0 && S->Region == AUD_RES_CMU_AUD) {
                    Val = CMU_MUX_SELECT;
                } else if (Dev->Cfg.HpMaster && S->Reg == ABOX_UAIF0_CTRL0 && S->Region == AUD_RES_ABOX_IFR) {
                    Val = ABOX_UAIF_CTRL0_SLAVE;
                }
                AudWr(Dev, S->Region, S->Reg, (S->Op == ABOX_OP_RMW) ? ((Old & ~S->Mask) | (Val & S->Mask)) : Val);
            }
            AudLog(Dev->Log, "abox %-16s 0x%08lX -> 0x%08lX\r\n", S->Name, Old, AudRd(Dev, S->Region, S->Reg));
        }
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    Old   = AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_ROUTE_CTRL0);
    Route = (Old & ~(ABOX_ROUTE_UAIF0_MASK | ABOX_ROUTE_UAIF1_MASK)) | (Headphones ? ABOX_ROUTE_UAIF0_SIFS1 : ABOX_ROUTE_UAIF1_SIFS1);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_ROUTE_CTRL0, Route);
    Dev->AboxUaifCtrl0 = Headphones ? ABOX_UAIF0_CTRL0 : ABOX_UAIF1_CTRL0;
    AudLog(Dev->Log, "abox output %s: ROUTE_CTRL0 0x%08lX -> 0x%08lX\r\n", Headphones ? "HEADPHONES (UAIF0)" : "speakers (UAIF1)",
           Old, AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_ROUTE_CTRL0));
    return STATUS_SUCCESS;
}

/* Undo AboxInit: every saved register back, in reverse order (a divider restore waits for BUSY like UEFI :619). */
_IRQL_requires_(PASSIVE_LEVEL)
VOID AboxRelease(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    ULONG I;

    AboxStop(Dev);
    if (Dev->AboxUaif0Count != 0 && AudPowerOn(Dev)) {
        for (I = Dev->AboxUaif0Count; I-- > 0;) {
            const ABOX_STEP *S = &AboxUaif0Seq[I];

            if (S->Op == ABOX_OP_POLL_CLEAR) {
                continue;
            }
            AudWr(Dev, S->Region, S->Reg, Dev->AboxUaif0Saved[I]);
            if (AboxUaif0Seq[I + 1].Op == ABOX_OP_POLL_CLEAR) {
                (VOID)AboxPollClear(Dev, &AboxUaif0Seq[I + 1]);
            }
        }
        AudLog(Dev->Log, "AboxRelease: %lu UAIF0 registers restored\r\n", Dev->AboxUaif0Count);
    }
    Dev->AboxUaif0Count = 0;
    Dev->AboxUaifCtrl0  = 0;
    if (Dev->AboxSavedCount == 0) {
        Dev->AboxReady = FALSE;
        return;
    }

    if (!AudPowerOn(Dev)) {
        AudLog(Dev->Log, "AboxRelease: pd-aud off - nothing restored\r\n");
    } else {
        for (I = Dev->AboxSavedCount; I-- > 0;) {
            const ABOX_STEP *S = &AboxStage1Seq[I];

            if (S->Op == ABOX_OP_POLL_CLEAR) {
                continue;
            }
            AudWr(Dev, S->Region, S->Reg, Dev->AboxSaved[I]);
            if (AboxStage1Seq[I + 1].Op == ABOX_OP_POLL_CLEAR) {
                (VOID)AboxPollClear(Dev, &AboxStage1Seq[I + 1]);
            }
        }
        AudLog(Dev->Log, "AboxRelease: %lu stage-1 registers restored\r\n", Dev->AboxSavedCount);
    }

    Dev->AboxSavedCount = 0;
    Dev->AboxReady = FALSE;
}

/* J3 diagnostic: do the active UAIF's pins toggle? GPIO_AUD bank DAT (+0x4) over 64 reads, AND / OR (gpb1 = UAIF1 is
   the known-good control for gpb0 = UAIF0). */
_IRQL_requires_(PASSIVE_LEVEL)
VOID AboxSamplePins(_In_ PAUD_DEVICE_CONTEXT Dev, _In_z_ PCSTR When)
{
    ULONG Bank = (Dev->AboxUaifCtrl0 == ABOX_UAIF0_CTRL0) ? GPIO_AUD_GPB0_CON : GPIO_AUD_GPB1_CON;
    ULONG AndV = 0xFFFFFFFFu, OrV = 0, V, I;

    if (!Dev->Mapped || !AudPowerOn(Dev)) {
        return;
    }
    KeStallExecutionProcessor(2000);
    for (I = 0; I < 64; I++) {
        V = AudRd(Dev, AUD_RES_GPIO_AUD, Bank + 4);
        AndV &= V;
        OrV  |= V;
        KeStallExecutionProcessor(3);
    }
    AudLog(Dev->Log, "%s: gpb%u DAT over 64 reads: and %02lX or %02lX (CON %08lX, MUX_UAIF0 %08lX)\r\n", When,
           (Bank == GPIO_AUD_GPB0_CON) ? 0 : 1, AndV & 0xFF, OrV & 0xFF, AudRd(Dev, AUD_RES_GPIO_AUD, Bank),
           AudRd(Dev, AUD_RES_CMU_AUD, CMU_AUD_MUX_UAIF0));
}

/*
 * Stage 2: UAIF1 SPK_EN, then RDMA0 plays the cyclic ring [PhysAddr, PhysAddr + Bytes) (physical: the AUD SysMMU is
 * bypassed, boot #99), PeriodBytes per BUF_OFFSET. The hardware frame is fixed: 48 kHz, 4 slots x 32 bit.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AboxStart(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ PHYSICAL_ADDRESS PhysAddr, _In_ ULONG Bytes,
                   _In_ ULONG PeriodBytes, _In_ ULONG Rate, _In_ ULONG Channels, _In_ ULONG Bits)
{
    ULONG Str;
    ULONG Uaif;

    if (!Dev->AboxReady || !AudPowerOn(Dev)) {
        AudLog(Dev->Log, "AboxStart: stage 1 not done or pd-aud off - refused\r\n");
        return STATUS_DEVICE_NOT_READY;
    }

    if (PhysAddr.QuadPart == 0 || (ULONGLONG)PhysAddr.QuadPart + Bytes > 0x100000000ULL ||
        (PhysAddr.QuadPart % AUD_DMA_ALIGN) != 0 || (Bytes % AUD_DMA_ALIGN) != 0 || PeriodBytes == 0 ||
        (PeriodBytes % AUD_DMA_ALIGN) != 0 || (Bytes % PeriodBytes) != 0) {
        AudLog(Dev->Log, "AboxStart: ring 0x%I64X + 0x%lX / 0x%lX not usable by the 32-bit / 16-byte RDMA fields\r\n",
               PhysAddr.QuadPart, Bytes, PeriodBytes);
        return STATUS_INVALID_PARAMETER;
    }

    if (Rate != AUD_RATE || Channels != AUD_HW_SLOTS || Bits != 32) {
        AudLog(Dev->Log, "AboxStart: %lu Hz %lu x %lu bit is not the UAIF1 frame (48 kHz 4 x 32)\r\n", Rate, Channels, Bits);
        return STATUS_INVALID_PARAMETER;
    }

    Str = (ULONG)PhysAddr.QuadPart;
    Uaif = Dev->AboxUaifCtrl0 ? Dev->AboxUaifCtrl0 : ABOX_UAIF1_CTRL0;
    AudWr(Dev, AUD_RES_ABOX_IFR, Uaif, AudRd(Dev, AUD_RES_ABOX_IFR, Uaif) | ABOX_UAIF_SPK_ENABLE);
    KeMemoryBarrier();
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_BUF_STR, Str);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_BUF_END, Str + Bytes);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_BUF_OFFSET, PeriodBytes);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STR_POINT, Str);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_VOL_FACTOR, ABOX_RDMA_VOL_UNITY);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_CTRL0, ABOX_RDMA_CTRL0_RESET | ABOX_RDMA_FORMAT_4X32 | ABOX_RDMA_ENABLE);
    Dev->AboxRunning = TRUE;

    AboxSamplePins(Dev, "AboxStart");

    AudLog(Dev->Log, "AboxStart: ring 0x%08lX + 0x%lX, period 0x%lX: RDMA0 CTRL0 %08lX STATUS %08lX, UAIF%u CTRL0 %08lX STATUS %08lX\r\n",
           Str, Bytes, PeriodBytes, AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_CTRL0),
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS), (Uaif == ABOX_UAIF0_CTRL0) ? 0 : 1, AudRd(Dev, AUD_RES_ABOX_IFR, Uaif),
           AudRd(Dev, AUD_RES_ABOX_IFR, Uaif + (ABOX_UAIF1_STATUS - ABOX_UAIF1_CTRL0)));
    return STATUS_SUCCESS;
}

/* Teardown (AudioSpeaker.c :600-604): interface off, DMA off, wait for the DMA to go idle. Amps must be off first. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID AboxStop(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    ULONG Ms;
    ULONG Uaif;

    if (!Dev->AboxRunning) {
        return;
    }

    Dev->AboxRunning = FALSE;
    if (!AudPowerOn(Dev)) {
        AudLog(Dev->Log, "AboxStop: pd-aud went off under a running stream - nothing written\r\n");
        return;
    }

    Uaif = Dev->AboxUaifCtrl0 ? Dev->AboxUaifCtrl0 : ABOX_UAIF1_CTRL0;
    AudWr(Dev, AUD_RES_ABOX_IFR, Uaif, AudRd(Dev, AUD_RES_ABOX_IFR, Uaif) & ~ABOX_UAIF_SPK_ENABLE);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_CTRL0, ABOX_RDMA_CTRL0_RESET);
    for (Ms = 0; Ms < 50 && (AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS) & ABOX_RDMA_PROGRESS) != 0; Ms++) {
        KeStallExecutionProcessor(1000);
    }

    if ((AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS) & ABOX_RDMA_PROGRESS) != 0) {
        Dev->AboxDmaStuck = TRUE;   /* the ring memory must not be freed (UEFI :621-625 keeps the pages) */
    }

    AudLog(Dev->Log, "AboxStop: after %lu ms RDMA0 STATUS %08lX%s, UAIF%u CTRL0 %08lX STATUS %08lX\r\n", Ms,
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS), Dev->AboxDmaStuck ? " STILL IN PROGRESS" : "",
           (Uaif == ABOX_UAIF0_CTRL0) ? 0 : 1, AudRd(Dev, AUD_RES_ABOX_IFR, Uaif),
           AudRd(Dev, AUD_RES_ABOX_IFR, Uaif + (ABOX_UAIF1_STATUS - ABOX_UAIF1_CTRL0)));
}

/*
 * RDMA0 read position in ring frames (one frame = 16 bytes = 4 x 32-bit slots) while the DMA runs:
 * RBUF_OFFSET (27:12, 16-byte units = the current period's first frame) + RBUF_CNT (11:0, frames into that period).
 * Run audD (A12): every RBUF_CNT value was below the 480-frame period and RBUF_OFFSET was period-aligned
 * (0x3C0 = period 2, 0x800 = period 1 of 2048-frame periods); boot #99's "frames mod 4096" reading came from a
 * one-period ring where RBUF_OFFSET stays 0. The caller takes the result modulo the ring size. DISPATCH_LEVEL ok.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AboxRingPosition(_In_ PAUD_DEVICE_CONTEXT Dev, _Out_ PULONG Frames, _Out_ PULONG RawStatus)
{
    ULONG S;
    ULONG I;

    *Frames    = 0;
    *RawStatus = 0;
    if (!Dev->AboxRunning) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Review R8: OFFSET and CNT are two fields of one register updated by the DMA at a period boundary; a read that
       straddles the update could pair the old OFFSET with the new (small) CNT, which the caller would take as a ring
       wrap (~3 periods forward). Re-read until OFFSET is stable on two consecutive reads (<= 4 reads). */
    S = AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS);
    for (I = 0; I < 3; I++) {
        ULONG S2 = AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS);

        if (((S2 ^ S) >> ABOX_RDMA_RBUF_OFF_SHIFT & ABOX_RDMA_RBUF_OFF_MASK) == 0) {
            S = S2;
            break;
        }
        S = S2;
    }
    *RawStatus = S;
    *Frames    = ((S >> ABOX_RDMA_RBUF_OFF_SHIFT) & ABOX_RDMA_RBUF_OFF_MASK) + (S & ABOX_RDMA_RBUF_CNT_MASK);
    return STATUS_SUCCESS;
}
