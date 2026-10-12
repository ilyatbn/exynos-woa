/*
 * AboxHw.c - the ABOX hardware layer: everything the render stream needs from ABOX goes through
 * AboxInit / AboxStart / AboxStop / AboxFrameCount / AboxRelease.
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
 * RDMA0 STATUS[11:0] counts 48 kHz frames mod 4096 (boot #99: +0xF0 per 5 ms); [27:12] stays 0 on 2.0.1.
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
#define   ABOX_RDMA_FRAMES_MASK    0x00000FFFu   /* 48 kHz frames mod 4096 */

/* CMU_AUD / GPIO_AUD / SYSREG_AUD (AudioSpeaker.c :115-125). */
#define CMU_AUD_DIV_UAIF1        0x1830u
#define   CMU_DIV_MASK             0x000001FFu
#define   CMU_DIV_BUSY             0x00010000u
#define   CMU_DIV_UAIF1_6M144      95u           /* AUDIF 589.824 MHz (sboot /2) / 96 = 6.144 MHz */
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

/* Undo AboxInit: every saved register back, in reverse order (a divider restore waits for BUSY like UEFI :619). */
_IRQL_requires_(PASSIVE_LEVEL)
VOID AboxRelease(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    ULONG I;

    AboxStop(Dev);
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

/*
 * Stage 2: UAIF1 SPK_EN, then RDMA0 plays the cyclic ring [PhysAddr, PhysAddr + Bytes) (physical: the AUD SysMMU is
 * bypassed, boot #99), PeriodBytes per BUF_OFFSET. The hardware frame is fixed: 48 kHz, 4 slots x 32 bit.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AboxStart(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ PHYSICAL_ADDRESS PhysAddr, _In_ ULONG Bytes,
                   _In_ ULONG PeriodBytes, _In_ ULONG Rate, _In_ ULONG Channels, _In_ ULONG Bits)
{
    ULONG Str;

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
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0, AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0) | ABOX_UAIF_SPK_ENABLE);
    KeMemoryBarrier();
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_BUF_STR, Str);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_BUF_END, Str + Bytes);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_BUF_OFFSET, PeriodBytes);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STR_POINT, Str);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_VOL_FACTOR, ABOX_RDMA_VOL_UNITY);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_CTRL0, ABOX_RDMA_CTRL0_RESET | ABOX_RDMA_FORMAT_4X32 | ABOX_RDMA_ENABLE);
    Dev->AboxRunning = TRUE;

    AudLog(Dev->Log, "AboxStart: ring 0x%08lX + 0x%lX, period 0x%lX: RDMA0 CTRL0 %08lX STATUS %08lX, UAIF1 CTRL0 %08lX STATUS %08lX\r\n",
           Str, Bytes, PeriodBytes, AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_CTRL0),
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS), AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0),
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_STATUS));
    return STATUS_SUCCESS;
}

/* Teardown (AudioSpeaker.c :600-604): interface off, DMA off, wait for the DMA to go idle. Amps must be off first. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID AboxStop(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    ULONG Ms;

    if (!Dev->AboxRunning) {
        return;
    }

    Dev->AboxRunning = FALSE;
    if (!AudPowerOn(Dev)) {
        AudLog(Dev->Log, "AboxStop: pd-aud went off under a running stream - nothing written\r\n");
        return;
    }

    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0, AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0) & ~ABOX_UAIF_SPK_ENABLE);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_CTRL0, ABOX_RDMA_CTRL0_RESET);
    for (Ms = 0; Ms < 50 && (AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS) & ABOX_RDMA_PROGRESS) != 0; Ms++) {
        KeStallExecutionProcessor(1000);
    }

    if ((AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS) & ABOX_RDMA_PROGRESS) != 0) {
        Dev->AboxDmaStuck = TRUE;   /* the ring memory must not be freed (UEFI :621-625 keeps the pages) */
    }

    AudLog(Dev->Log, "AboxStop: after %lu ms RDMA0 STATUS %08lX%s, UAIF1 CTRL0 %08lX STATUS %08lX\r\n", Ms,
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS), Dev->AboxDmaStuck ? " STILL IN PROGRESS" : "",
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0), AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_STATUS));
}

/* RDMA0 STATUS[11:0]: 48 kHz frames mod 4096 while the DMA runs (boot #99). Legal at DISPATCH_LEVEL. */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AboxFrameCount(_In_ PAUD_DEVICE_CONTEXT Dev, _Out_ PULONG Count12)
{
    *Count12 = 0;
    if (!Dev->AboxRunning) {
        return STATUS_DEVICE_NOT_READY;
    }

    *Count12 = AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS) & ABOX_RDMA_FRAMES_MASK;
    return STATUS_SUCCESS;
}
