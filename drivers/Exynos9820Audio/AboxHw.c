/*
 * AboxHw.c - the ABOX hardware layer: everything the render stream needs from ABOX goes through
 * AboxInit / AboxStart / AboxStop / AboxPosition, so the register sequence the main session proves in UEFI
 * (D8a m3 stage 1: UAIF1 clocks + pins, stage 2: RDMA0 tone) plugs in here and nowhere else.
 *
 * Facts and offsets: notes/d8-audio-m3-abox.md (F2 SFR map, F5 pd-aud, F10 UAIF1 fields, F11 RDMA, F12 routing),
 * all from the LineageOS lineage-22.2 vendor tree (abox_soc.h) = SOURCE-ONLY until the stage that writes them
 * is device-proven. Stage 0 (the read-only audit below) is the UEFI AudioAbox.c list, which boot #96 read live:
 * pd-aud ON, IP_INDEX "ABOX" non-secure, VERSION 0x02000100, every routing/UAIF/RDMA register at reset value.
 *
 * Rule (F5): nothing in the AUD block is touched unless PMU AUD_STATUS bit0 reads 1 first; an access to a
 * powered-down block is treated as a hang.
 *
 * NOT VALIDATED ON HARDWARE (stage Guided).
 */

#include "Exynos9820Audio.h"

/* ABOX IFR offsets (notes F2, abox_soc.h line refs there). */
#define ABOX_IP_INDEX            0x0000u
#define ABOX_IP_INDEX_ABOX       0x41424F58u   /* "ABOX", reg_defaults_9820 row 1; boot #96 read it */
#define ABOX_VERSION             0x0004u
#define ABOX_SYSPOWER_CTRL       0x0010u
#define ABOX_SYSPOWER_STATUS     0x0014u
#define ABOX_QCHANNEL_DISABLE    0x0038u       /* bit 3 = UAIF1 BCLK keep-alive (F4/F12) */
#define ABOX_ROUTE_CTRL0         0x0040u       /* [7:4] UAIF1 SPK source, 2 = SIFS1 (F12) */
#define ABOX_SPUS_CTRL0          0x0200u
#define ABOX_SPUS_CTRL1          0x0204u
#define ABOX_SPUS_SBANK_RDMA0    0x0220u
#define ABOX_SPUS_SIFS_CNT1      0x0294u
#define ABOX_UAIF1_CTRL0         0x0510u       /* bit0 SPK_ENABLE, bit2 MODE (1 = ABOX master) (F2/F10) */
#define ABOX_UAIF1_CTRL1         0x0514u
#define ABOX_UAIF1_STATUS        0x051Cu       /* bit0 ERROR_OF_SPK */
#define ABOX_RDMA0_CTRL0         0x1000u       /* bit0 ENABLE, [22:19] BURST_LEN; FORMAT position UNVERIFIED on 2.0.1 */
#define ABOX_RDMA0_BUF_STR       0x1008u
#define ABOX_RDMA0_BUF_END       0x100Cu
#define ABOX_RDMA0_BUF_OFFSET    0x1010u
#define ABOX_RDMA0_STR_POINT     0x1014u
#define ABOX_RDMA0_VOL_FACTOR    0x1018u
#define ABOX_RDMA0_STATUS        0x1030u       /* 2.0.1: PROGRESS 31, RBUF_OFFSET 27:12, RBUF_CNT 11:0 (F2) */

#define ABOX_UAIF_SPK_ENABLE     0x00000001u
#define ABOX_RDMA_ENABLE         0x00000001u

/* Register-sequence interpreter shared by the stage tables below (and by nothing else: the amp layer has its
   own, its registers live behind I2C). */
typedef enum _ABOX_OP {
    ABOX_OP_END = 0,
    ABOX_OP_WRITE,      /* Va[Region] + Reg = Value */
    ABOX_OP_RMW,        /* (old & ~Mask) | (Value & Mask) */
    ABOX_OP_POLL,       /* wait until (read & Mask) == Value, TimeoutUs */
    ABOX_OP_DELAY,      /* KeStallExecutionProcessor(TimeoutUs) */
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

/*
 * TODO(stage 1 - main session): UAIF1 clocks + pins + UAIF1 running with silence, device-proven in UEFI.
 * Paste the proven register writes here, in order, as ABOX_STEP rows. Candidate rows from
 * notes/d8-audio-m3-abox.md "Stage 1" steps 8-10 (ALL UNVERIFIED, inference - do not uncomment unproven):
 *   CMU_AUD  +0x1830 DIV_CLK_AUD_UAIF1 DIVRATIO 11 (6.144 MHz, 4x32) or 23 (3.072 MHz, 2x32), poll BUSY bit16
 *   GPIO_AUD +0x0020 gpb1 CON [15:0] = 0x2222; +0x0028 gpb1 PUD [15:0] = 0x1111
 *   SYSREG   +0x0504 MISC_CON = 1
 *   IFR      +0x0038 QCHANNEL_DISABLE bit3 = 1
 *   IFR      +0x0514 UAIF1 CTRL1 = 0x1B4DF7DF (4x32) / 0x1944F7DF (2x32)
 *   IFR      +0x0510 UAIF1 CTRL0 = 0x01000014 (MODE = master; SPK_ENABLE comes with AboxStart)
 * Boot #96 found DIV_AUDIF 1 (589.8 MHz), not the vendor's 73.728 MHz, so the divider rows depend on what the
 * stage-1 run settles on.
 */
static const ABOX_STEP AboxStage1Seq[] = {
    { ABOX_OP_END, AUD_RES_ABOX_IFR, 0, 0, 0, 0, NULL },
};

static __forceinline ULONG AudRd(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ AUD_RES Region, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Dev->Va[Region] + Off));
}

static __forceinline VOID AudWr(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ AUD_RES Region, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Dev->Va[Region] + Off), Val);
}

/* Returns the number of executed steps; *Status is the first failure (or SUCCESS). Logs every write + readback. */
static ULONG AboxRunSeq(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ const ABOX_STEP *Seq, _Out_ NTSTATUS *Status)
{
    ULONG I;
    ULONG V;
    ULONG Us;

    *Status = STATUS_SUCCESS;
    for (I = 0; Seq[I].Op != ABOX_OP_END; I++) {
        const ABOX_STEP *S = &Seq[I];

        if ((ULONG)S->Region >= AUD_RES_COUNT || Dev->Va[S->Region] == NULL || S->Reg + 4 > Dev->Len[S->Region]) {
            AudLog(Dev->Log, "abox seq %lu (%s): region %d off 0x%lX outside the mapping\r\n", I, S->Name, S->Region, S->Reg);
            *Status = STATUS_INVALID_PARAMETER;
            return I;
        }

        switch (S->Op) {
        case ABOX_OP_WRITE:
            AudWr(Dev, S->Region, S->Reg, S->Value);
            AudLog(Dev->Log, "abox %s [%d+0x%lX] <- 0x%08lX (reads 0x%08lX)\r\n", S->Name, S->Region, S->Reg, S->Value,
                   AudRd(Dev, S->Region, S->Reg));
            break;
        case ABOX_OP_RMW:
            V = AudRd(Dev, S->Region, S->Reg);
            AudWr(Dev, S->Region, S->Reg, (V & ~S->Mask) | (S->Value & S->Mask));
            AudLog(Dev->Log, "abox %s [%d+0x%lX] 0x%08lX -> 0x%08lX\r\n", S->Name, S->Region, S->Reg, V,
                   AudRd(Dev, S->Region, S->Reg));
            break;
        case ABOX_OP_POLL:
            for (Us = 0; ((V = AudRd(Dev, S->Region, S->Reg)) & S->Mask) != S->Value; Us += 10) {
                if (Us >= S->TimeoutUs) {
                    AudLog(Dev->Log, "abox %s [%d+0x%lX] poll timeout: 0x%08lX & 0x%lX != 0x%lX\r\n", S->Name,
                           S->Region, S->Reg, V, S->Mask, S->Value);
                    *Status = STATUS_IO_TIMEOUT;
                    return I;
                }
                KeStallExecutionProcessor(10);
            }
            break;
        case ABOX_OP_DELAY:
            KeStallExecutionProcessor(S->TimeoutUs);
            break;
        default:
            *Status = STATUS_INVALID_PARAMETER;
            return I;
        }
    }

    return I;
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
    { "CMU_AUD DIV_UAIF1",       AUD_RES_CMU_AUD,     0x1830 },
    { "CMU_AUD GATE_ACLK",       AUD_RES_CMU_AUD,     0x2010 },
    { "CMU_AUD GATE_BCLK_UAIF1", AUD_RES_CMU_AUD,     0x2024 },
    { "SYSREG_AUD MISC_CON",     AUD_RES_ABOX_SYSREG, 0x0504 },
    { "GPIO_AUD gpb1 CON",       AUD_RES_GPIO_AUD,    0x0020 },
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

/* Stage 1: UAIF1 clocks + pins + frame format, BCLK/WS running with SPK disabled. Idempotent (AboxReady). */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AboxInit(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    NTSTATUS Status;
    ULONG    Steps;

    if (Dev->AboxReady) {
        return STATUS_SUCCESS;
    }

    if (!Dev->AboxAlive || !AudPowerOn(Dev)) {
        AudLog(Dev->Log, "AboxInit: ABOX not alive or pd-aud off - refused\r\n");
        return STATUS_DEVICE_NOT_READY;
    }

    if (AboxStage1Seq[0].Op == ABOX_OP_END) {
        AudLog(Dev->Log, "AboxInit: stage-1 sequence not delivered yet (AboxStage1Seq empty) - timer-only streaming\r\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    Steps = AboxRunSeq(Dev, AboxStage1Seq, &Status);
    AudLog(Dev->Log, "AboxInit: %lu steps, status %08lX, UAIF1 CTRL0 %08lX CTRL1 %08lX STATUS %08lX\r\n", Steps, Status,
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0), AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL1),
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_STATUS));
    if (NT_SUCCESS(Status)) {
        Dev->AboxReady = TRUE;
    }

    return Status;
}

/*
 * Stage 2: RDMA0 plays the cyclic ring [PhysAddr, PhysAddr + Bytes), period PeriodBytes, into SIFS1 -> UAIF1.
 * The checks below are the ones every candidate sequence needs (32-bit SFRs, 16-byte units); the writes are not.
 *
 * TODO(stage 2 - main session): replace the STATUS_NOT_IMPLEMENTED return with the proven order. Candidate from
 * notes/d8-audio-m3-abox.md "Stage 2" steps 12-15 (ALL UNVERIFIED):
 *   SPUS_SBANK_RDMA0 (+0x220) = 0x02000000 (patch_9820), SYSPOWER_CTRL (+0x10) = 1 (DRAM source),
 *   RDMA0 BUF_STR = PhysAddr, BUF_END = PhysAddr + Bytes, BUF_OFFSET = PeriodBytes, STR_POINT = PhysAddr,
 *   VOL_FACTOR (reset 0 - unity value UNVERIFIED), CTRL0 = BURST_LEN 8 | FORMAT(Bits, Channels) | ENABLE,
 *   ROUTE_CTRL0[7:4] = 2 (SIFS1), SIFS_CNT1 (+0x294) = 0, then UAIF1 CTRL0 |= SPK_ENABLE.
 * Whether the RDMA takes a physical address (SysMMU CTRL 4 = bypass at boot #96) or an SRAM offset is what stage 2
 * decides; keep this signature either way (an SRAM bounce would copy from the ring in the packet DPC).
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AboxStart(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ PHYSICAL_ADDRESS PhysAddr, _In_ ULONG Bytes,
                   _In_ ULONG PeriodBytes, _In_ ULONG Rate, _In_ ULONG Channels, _In_ ULONG Bits)
{
    AudLog(Dev->Log, "AboxStart: ring 0x%I64X + 0x%lX, period 0x%lX, %lu Hz, %lu ch, %lu bit\r\n", PhysAddr.QuadPart,
           Bytes, PeriodBytes, Rate, Channels, Bits);

    if (!Dev->AboxReady || !AudPowerOn(Dev)) {
        AudLog(Dev->Log, "AboxStart: stage 1 not done or pd-aud off - refused\r\n");
        return STATUS_DEVICE_NOT_READY;
    }

    if (PhysAddr.QuadPart == 0 || (ULONGLONG)PhysAddr.QuadPart + Bytes > 0x100000000ULL ||
        (PhysAddr.QuadPart % AUD_DMA_ALIGN) != 0 || (Bytes % AUD_DMA_ALIGN) != 0 || PeriodBytes == 0 ||
        (PeriodBytes % AUD_DMA_ALIGN) != 0 || (Bytes % PeriodBytes) != 0) {
        AudLog(Dev->Log, "AboxStart: ring not usable by the 32-bit / 16-byte RDMA fields - refused\r\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (Rate != AUD_RATE || Channels != AUD_CHANNELS || (Bits != 16 && Bits != 32)) {
        AudLog(Dev->Log, "AboxStart: format outside the circuit's list - refused\r\n");
        return STATUS_INVALID_PARAMETER;
    }

    AudLog(Dev->Log, "AboxStart: stage-2 sequence not delivered yet - timer-only streaming\r\n");
    return STATUS_NOT_IMPLEMENTED;
}

/* Counterpart of AboxStart (abox_if.c shutdown order, F4: SPK off after a 600-1200 us drain, then the DMA). */
_IRQL_requires_(PASSIVE_LEVEL)
VOID AboxStop(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    if (!Dev->AboxRunning) {
        return;
    }

    Dev->AboxRunning = FALSE;
    if (!AudPowerOn(Dev)) {
        AudLog(Dev->Log, "AboxStop: pd-aud went off under a running stream - nothing written\r\n");
        return;
    }

    KeStallExecutionProcessor(1200);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0, AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0) & ~ABOX_UAIF_SPK_ENABLE);
    AudWr(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_CTRL0, AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_CTRL0) & ~ABOX_RDMA_ENABLE);
    AudLog(Dev->Log, "AboxStop: UAIF1 CTRL0 %08lX STATUS %08lX, RDMA0 CTRL0 %08lX STATUS %08lX\r\n",
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_CTRL0), AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_UAIF1_STATUS),
           AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_CTRL0), AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS));
}

/*
 * Byte offset of the RDMA read pointer inside the ring. RDMA0 STATUS RBUF_OFFSET [27:12] on 2.0.1 (F2); that it
 * counts 16-byte units from BUF_STR is UNVERIFIED (2.1 differs) - the render stream only logs it next to the timer
 * position until stage 2 confirms it.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AboxPosition(_In_ PAUD_DEVICE_CONTEXT Dev, _Out_ PULONG RingOffset)
{
    ULONG Status;

    *RingOffset = 0;
    if (!Dev->AboxRunning) {
        return STATUS_DEVICE_NOT_READY;
    }

    Status      = AudRd(Dev, AUD_RES_ABOX_IFR, ABOX_RDMA0_STATUS);
    *RingOffset = ((Status >> 12) & 0xFFFFu) * AUD_DMA_ALIGN;
    return STATUS_SUCCESS;
}
