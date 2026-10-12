// SPDX-License-Identifier: GPL-2.0
/*
 * MaliG76Probe: M16 G9b on the Galaxy S10e (Exynos 9820, Mali-G76 MP12). Binds the DSDT's GPU0
 * (ACPI\SAMS9828) and makes the GPU run one WRITE_VALUE job under Windows.
 *
 * Derived from m1probe (drivers/m1probe/m1probe.c of wddm-mali-g52, https://github.com/gahingwoo/wddm-mali-g52,
 * commit 16f7e7c6, GPL-2.0, Copyright gahingwoo), which ran the same job on the RK3576's Mali-G52. Retargeted
 * per notes/gpu-wddm-mali-g52.md R1-R19 and notes/gpu-m16-g9b.md (every constant is cited there or below):
 *   R4  "powered" = PMU+0x2104 (EMBEDDED_G3D_STATUS) bit0 == 1 AND PMU+0x2384 (G3D_STATUS) bit0 == 1, read
 *       through the PMU page in _CRS (RK: STATUS0 bit25 / ACK0 bit0 == 0 at a hard-coded PA).
 *   R5  GPU_ID gate ((id >> 16) & 0xFF0F) == 0x7201 (G76) instead of (id >> 16) == 0x7402 (G52).
 *   R6  panfrost_gpu_init_quirks for G76: SHADER_CONFIG = SC_TLS_HASH_ENABLE, TILER_CONFIG write-back,
 *       JM_CONFIG IDVS group size (m1probe wrote none of them).
 *   R7  power the L2/tiler/shader masks the GPU reports; refuse more than one core group.
 *   R10 Mali-LPAE ("legacy") AS0 exactly as m1probe - the format the reused KMD uses (decision in the notes).
 *   R17 the same WRITE_VALUE job on JS0.
 * Additions: resources are checked against the Exynos9820.h values, the three _CRS interrupts are connected
 * (completion is still polled; the ISR only records which line fired), and the results go to a run log file
 * \SystemRoot\System32\LogFiles\MaliG76Probe-run.txt instead of the registry.
 *
 * Safety rule: no GPU register is read or written unless both PMU status bits read 1 immediately before.
 * NOT VALIDATED ON HARDWARE.
 */
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>

/* ---- Resources: must equal the DSDT GPU0 _CRS (Exynos9820.h EXYNOS_G3D_BASE/SIZE, EXYNOS_PMU_BASE). ---- */
#define MP_GPU_PA               0x18500000ULL   /* EXYNOS_G3D_BASE (SOURCE-ONLY, mali@18500000) */
#define MP_GPU_LEN              0x5000u         /* EXYNOS_G3D_SIZE */
#define MP_PMU_PAGE_PA          0x15862000ULL   /* EXYNOS_PMU_BASE 0x15860000 + 0x2000 */
#define MP_PMU_PAGE_LEN         0x1000u
#define MP_PMU_EMB_G3D_STATUS   0x104u          /* EXYNOS_PMU_EMBEDDED_G3D_STATUS_OFFSET 0x2104 - 0x2000 */
#define MP_PMU_G3D_STATUS       0x384u          /* EXYNOS_PMU_G3D_STATUS_OFFSET 0x2384 - 0x2000 */
#define MP_MAX_IRQ              3u

/* ---- Mali JM registers (mainline drm/panfrost panfrost_regs.h; same names as m1probe.c:20-55). ---- */
#define GPU_ID                  0x000
#define GPU_L2_FEATURES         0x004
#define GPU_CORE_FEATURES       0x008
#define GPU_TILER_FEATURES      0x00C
#define GPU_MEM_FEATURES        0x010
#define GPU_MMU_FEATURES        0x014           /* PA bits = (v >> 8) & 0xFF (regs.h:18-20) */
#define GPU_AS_PRESENT          0x018
#define GPU_JS_PRESENT          0x01C
#define GPU_INT_RAWSTAT         0x020
#define GPU_INT_CLEAR           0x024
#define GPU_INT_MASK            0x028
#define GPU_INT_STAT            0x02C
#define GPU_CMD                 0x030
#define GPU_CMD_SOFT_RESET      0x01
#define GPU_STATUS              0x034
#define GPU_IRQ_RESET_COMPLETED 0x100           /* BIT(8) */
#define GPU_THREAD_MAX_THREADS  0x0A0
#define GPU_JS0_FEATURES        0x0C0           /* GPU_JS_FEATURES(0) */
#define SHADER_PRESENT_LO       0x100
#define SHADER_PRESENT_HI       0x104
#define TILER_PRESENT_LO        0x110
#define L2_PRESENT_LO           0x120
#define SHADER_READY_LO         0x140
#define TILER_READY_LO          0x150
#define L2_READY_LO             0x160
#define SHADER_PWRON_LO         0x180
#define TILER_PWRON_LO          0x190
#define L2_PWRON_LO             0x1A0
#define GPU_COHERENCY_FEATURES  0x300
#define GPU_JM_CONFIG           0xF00
#define GPU_SHADER_CONFIG       0xF04
#define GPU_TILER_CONFIG        0xF08
#define SC_TLS_HASH_ENABLE      (1u << 17)      /* regs.h:211; panfrost_gpu.c:141-145 (G76 has TLS_HASHING) */
#define JM_IDVS_GROUP_SIZE_SHIFT 16             /* regs.h:227 */
#define JM_DEFAULT_IDVS_GROUP_SIZE 0xFu         /* regs.h:228; panfrost_gpu.c:166-170 */
#define JOB_INT_RAWSTAT         0x1000
#define JOB_INT_CLEAR           0x1004
#define JOB_INT_MASK            0x1008
#define JOB_INT_STAT            0x100C
#define JOB_INT_JS0             0x00010001u     /* DONE(0) | ERR(0) = BIT(0) | BIT(16), regs.h:241-242 */
#define JS0                     0x1800
#define JS_STATUS               0x24
#define JS_HEAD_NEXT_LO         0x40
#define JS_HEAD_NEXT_HI         0x44
#define JS_AFFINITY_NEXT_LO     0x50
#define JS_AFFINITY_NEXT_HI     0x54
#define JS_CONFIG_NEXT          0x58
#define JS_COMMAND_NEXT         0x60
#define JS_COMMAND_START        0x01
#define MMU_INT_RAWSTAT         0x2000
#define MMU_INT_CLEAR           0x2004
#define MMU_INT_MASK            0x2008
#define MMU_INT_STAT            0x200C
#define MMU_INT_AS0             0x00010001u     /* AS0 page fault BIT(0) | bus error BIT(16) */
#define AS0                     0x2400
#define AS_TRANSTAB_LO          0x00
#define AS_TRANSTAB_HI          0x04
#define AS_MEMATTR_LO           0x08
#define AS_MEMATTR_HI           0x0C
#define AS_COMMAND              0x18
#define AS_FAULTSTATUS          0x1C
#define AS_FAULTADDRESS_LO      0x20
#define AS_FAULTADDRESS_HI      0x24
#define AS_STATUS               0x28
#define AS_TRANSCFG_LO          0x30
#define AS_TRANSCFG_HI          0x34
#define AS_CMD_UPDATE           1

#define G76_ID_MASK             0xFF0Fu         /* R5: masks arch_rev (bits 7:4 of the high half) */
#define G76_ID_MODEL            0x7201u         /* PAN_PROD_ID(7,2,1), mesa pan_model.c:80 */

/* m1probe.c:57-63, unchanged: AS 0, thread priority 8, flush clean+invalidate at start and end. */
#define JS_CONFIG               ((8u << 16) | (3u << 8) | (3u << 12))
#define GPU_VA                  0x2000000ULL
#define TARGET_OFFSET           0x100
#define MAGIC                   0xC0FFEE42u
#define NPAGES                  5

#define MP_RUNLOG_FILE  L"\\SystemRoot\\System32\\LogFiles\\MaliG76Probe-run.txt"
#define MP_RUNLOG_MAX   (16u * 1024u)
#define MP_POOL_TAG     '67GM'

typedef struct _MP_RUNLOG {
    KSPIN_LOCK Lock;
    ULONG      Used;
    ULONG      Dropped;
    LONGLONG   Start;
    CHAR       Text[MP_RUNLOG_MAX];
} MP_RUNLOG, *PMP_RUNLOG;

typedef struct _DEVICE_CONTEXT {
    volatile ULONG *Gpu;
    volatile ULONG *Pmu;
    BOOLEAN         Usable;      /* resources ok, powered, G76: interrupts exist and the run may happen */
    BOOLEAN         Ran;
    ULONG           IrqCount;
    ULONG           IrqGsiv[MP_MAX_IRQ];
    volatile LONG   IsrHits[MP_MAX_IRQ];
    volatile LONG   IsrEmpty;    /* ISR calls that found no GPU status bit (misrouted line / shared) */
    volatile LONG   IsrJob;      /* JOB_INT_STAT bits the ISR saw and cleared */
    volatile LONG   IsrMmu;
    volatile LONG   IsrGpu;
    PMP_RUNLOG      Log;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetContext)

typedef struct _IRQ_CONTEXT {
    ULONG Index;                 /* position among the _CRS interrupts: 0 JOB, 1 MMU, 2 GPU */
} IRQ_CONTEXT, *PIRQ_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IRQ_CONTEXT, GetIrqContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MpEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE MpEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE MpEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED MpEvtD0EntryPostInterruptsEnabled;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED MpEvtD0ExitPreInterruptsDisabled;
EVT_WDF_INTERRUPT_ISR MpEvtIsr;

#define RD(b, o)    READ_REGISTER_ULONG((PULONG)((PUCHAR)(b) + (o)))
#define WR(b, o, v) WRITE_REGISTER_ULONG((PULONG)((PUCHAR)(b) + (o)), (v))

/* ======================================================================
 * Run log (drivers/Exynos9820Audio/RunLog.c pattern: DbgPrintEx + buffer, whole file rewritten at checkpoints).
 * ====================================================================== */
static VOID MpLog(_In_ PDEVICE_CONTEXT C, _In_z_ _Printf_format_string_ PCSTR Fmt, ...)
{
    CHAR    Line[256];
    CHAR    Prefix[24];
    va_list Args;
    size_t  PrefixLen, LineLen;
    ULONG   Ms;
    KIRQL   OldIrql;

    va_start(Args, Fmt);
    if (!NT_SUCCESS(RtlStringCbVPrintfA(Line, sizeof(Line) - 2, Fmt, Args))) {
        Line[sizeof(Line) - 3] = '\0';
    }
    va_end(Args);
    (VOID)RtlStringCbCatA(Line, sizeof(Line), "\r\n");

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "MaliG76Probe: %s", Line);
    if (C->Log == NULL) {
        return;
    }

    Ms = (ULONG)((KeQueryInterruptTime() - C->Log->Start) / 10000);
    (VOID)RtlStringCbPrintfA(Prefix, sizeof(Prefix), "[%lu.%03lu] ", Ms / 1000, Ms % 1000);
    PrefixLen = strlen(Prefix);
    LineLen   = strlen(Line);

    KeAcquireSpinLock(&C->Log->Lock, &OldIrql);
    if (C->Log->Used + PrefixLen + LineLen + 1 < sizeof(C->Log->Text)) {
        RtlCopyMemory(C->Log->Text + C->Log->Used, Prefix, PrefixLen);
        RtlCopyMemory(C->Log->Text + C->Log->Used + PrefixLen, Line, LineLen);
        C->Log->Used += (ULONG)(PrefixLen + LineLen);
    } else {
        C->Log->Dropped++;
    }
    KeReleaseSpinLock(&C->Log->Lock, OldIrql);
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID MpLogFlush(_In_ PDEVICE_CONTEXT C)
{
    UNICODE_STRING    Name;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK   Iosb;
    HANDLE            File;
    NTSTATUS          Status;
    PCHAR             Copy;
    ULONG             Used, Dropped;
    CHAR              Tail[64];
    size_t            TailLen = 0;
    KIRQL             OldIrql;

    if (C->Log == NULL) {
        return;
    }
    Copy = (PCHAR)ExAllocatePool2(POOL_FLAG_PAGED, MP_RUNLOG_MAX + sizeof(Tail), MP_POOL_TAG);
    if (Copy == NULL) {
        return;
    }
    KeAcquireSpinLock(&C->Log->Lock, &OldIrql);
    Used    = C->Log->Used;
    Dropped = C->Log->Dropped;
    RtlCopyMemory(Copy, C->Log->Text, Used);
    KeReleaseSpinLock(&C->Log->Lock, OldIrql);
    if (Dropped != 0) {
        (VOID)RtlStringCbPrintfA(Tail, sizeof(Tail), "(log full: %lu lines dropped)\r\n", Dropped);
        TailLen = strlen(Tail);
        RtlCopyMemory(Copy + Used, Tail, TailLen);
    }

    RtlInitUnicodeString(&Name, MP_RUNLOG_FILE);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_WRITE | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OVERWRITE_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, NULL, 0);
    if (NT_SUCCESS(Status)) {
        (VOID)ZwWriteFile(File, NULL, NULL, NULL, &Iosb, Copy, (ULONG)(Used + TailLen), NULL, NULL);
        ZwClose(File);
    } else {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "MaliG76Probe: run log file %08x\n", Status);
    }
    ExFreePoolWithTag(Copy, MP_POOL_TAG);
}

/* ======================================================================
 * Hardware helpers.
 * ====================================================================== */

/* R4: both PMU status bits must read 1. The PMU is always on, so this read is always safe. */
static BOOLEAN MpPowered(_In_ PDEVICE_CONTEXT C, _Out_opt_ PULONG Emb, _Out_opt_ PULONG G3d)
{
    ULONG E = RD(C->Pmu, MP_PMU_EMB_G3D_STATUS);
    ULONG G = RD(C->Pmu, MP_PMU_G3D_STATUS);

    if (Emb != NULL) {
        *Emb = E;
    }
    if (G3d != NULL) {
        *G3d = G;
    }
    return ((E & 1u) != 0) && ((G & 1u) != 0);
}

/* Poll (reg & Mask) == Want every 10 us for up to Us microseconds (m1probe.c:93-102 with a bound). */
static BOOLEAN MpPoll(volatile ULONG *Base, ULONG Off, ULONG Mask, ULONG Want, ULONG Us, _Out_ PULONG Last)
{
    for (ULONG i = 0; i <= Us / 10; i++) {
        *Last = RD(Base, Off);
        if ((*Last & Mask) == Want) {
            return TRUE;
        }
        KeStallExecutionProcessor(10);
    }
    return FALSE;
}

static ULONG MpPopCount(ULONG V)
{
    ULONG N = 0;
    for (; V != 0; V &= V - 1) {
        N++;
    }
    return N;
}

/* Soft reset and wait for RESET_COMPLETED (m1probe.c:134-140). Leaves every interrupt mask 0. */
static BOOLEAN MpSoftReset(_In_ PDEVICE_CONTEXT C)
{
    volatile ULONG *Gpu = C->Gpu;
    ULONG Last;
    BOOLEAN Ok;

    WR(Gpu, GPU_INT_MASK, 0);
    WR(Gpu, JOB_INT_MASK, 0);
    WR(Gpu, MMU_INT_MASK, 0);
    WR(Gpu, GPU_INT_CLEAR, 0xFFFFFFFF);
    WR(Gpu, GPU_CMD, GPU_CMD_SOFT_RESET);
    Ok = MpPoll(Gpu, GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 100000, &Last);
    MpLog(C, "soft reset: GPU_INT_RAWSTAT %08lx -> %s", Last, Ok ? "RESET_COMPLETED" : "TIMEOUT (100 ms)");
    WR(Gpu, GPU_INT_CLEAR, 0xFFFFFFFF);
    WR(Gpu, JOB_INT_CLEAR, 0xFFFFFFFF);
    WR(Gpu, MMU_INT_CLEAR, 0xFFFFFFFF);
    return Ok;
}

/* ======================================================================
 * The probe run (PASSIVE_LEVEL, interrupts connected).
 * ====================================================================== */
static VOID MpRun(_In_ PDEVICE_CONTEXT C)
{
    volatile ULONG *Gpu = C->Gpu;
    PHYSICAL_ADDRESS Low, High, Skip;
    ULONG Emb, G3d, Last, V;
    ULONG L2, Shader, ShaderHi, Tiler, PaBits;
    ULONG64 Base = 0, Target;
    PUCHAR Mem = NULL;
    ULONG JobBits = 0;
    BOOLEAN Done = FALSE, Fault = FALSE, Free = TRUE;

    MpLog(C, "run: start");
    if (!MpPowered(C, &Emb, &G3d)) {
        MpLog(C, "run: G3D NOT powered any more (EMBEDDED_G3D_STATUS %08lx G3D_STATUS %08lx) - not touching the GPU",
              Emb, G3d);
        MpLog(C, "RESULT: SKIPPED (not powered)");
        return;
    }

    /* 1. Identify + features (read-only). */
    MpLog(C, "GPU_ID %08lx L2_FEATURES %08lx CORE_FEATURES %08lx TILER_FEATURES %08lx MEM_FEATURES %08lx",
          RD(Gpu, GPU_ID), RD(Gpu, GPU_L2_FEATURES), RD(Gpu, GPU_CORE_FEATURES), RD(Gpu, GPU_TILER_FEATURES),
          RD(Gpu, GPU_MEM_FEATURES));
    MpLog(C, "MMU_FEATURES %08lx AS_PRESENT %08lx JS_PRESENT %08lx JS0_FEATURES %08lx GPU_STATUS %08lx "
             "THREAD_MAX_THREADS %08lx COHERENCY_FEATURES %08lx",
          RD(Gpu, GPU_MMU_FEATURES), RD(Gpu, GPU_AS_PRESENT), RD(Gpu, GPU_JS_PRESENT), RD(Gpu, GPU_JS0_FEATURES),
          RD(Gpu, GPU_STATUS), RD(Gpu, GPU_THREAD_MAX_THREADS), RD(Gpu, GPU_COHERENCY_FEATURES));
    Shader   = RD(Gpu, SHADER_PRESENT_LO);
    ShaderHi = RD(Gpu, SHADER_PRESENT_HI);
    Tiler    = RD(Gpu, TILER_PRESENT_LO);
    L2       = RD(Gpu, L2_PRESENT_LO);
    MpLog(C, "SHADER_PRESENT %08lx:%08lx (%lu cores) TILER_PRESENT %08lx L2_PRESENT %08lx",
          ShaderHi, Shader, MpPopCount(Shader) + MpPopCount(ShaderHi), Tiler, L2);
    if ((RD(Gpu, GPU_AS_PRESENT) & 1u) == 0 || (RD(Gpu, GPU_JS_PRESENT) & 1u) == 0) {
        MpLog(C, "RESULT: FAIL (AS0 or JS0 not present)");
        return;
    }
    if (MpPopCount(L2) != 1 || ShaderHi != 0 || Shader == 0 || Tiler == 0) {
        /* panfrost_get_core_mask (panfrost_gpu.c:429-450) uses all cores only when L2_PRESENT == 1. */
        MpLog(C, "RESULT: FAIL (core layout not handled: L2_PRESENT must have one bit, cores in the low word)");
        return;
    }

    /* 2. Soft reset (m1probe step 3). */
    if (!MpSoftReset(C)) {
        MpLog(C, "RESULT: FAIL (soft reset)");
        return;
    }

    /* 3. R6: panfrost_gpu_init_quirks for a G76 (panfrost_gpu.c:117-175). */
    WR(Gpu, GPU_SHADER_CONFIG, SC_TLS_HASH_ENABLE);
    WR(Gpu, GPU_TILER_CONFIG, RD(Gpu, GPU_TILER_CONFIG));
    WR(Gpu, GPU_JM_CONFIG, JM_DEFAULT_IDVS_GROUP_SIZE << JM_IDVS_GROUP_SIZE_SHIFT);
    MpLog(C, "quirks: SHADER_CONFIG %08lx TILER_CONFIG %08lx JM_CONFIG %08lx",
          RD(Gpu, GPU_SHADER_CONFIG), RD(Gpu, GPU_TILER_CONFIG), RD(Gpu, GPU_JM_CONFIG));

    /* 4. L2 first, then tiler + shader cores (panfrost_gpu.c:461-471, m1probe.c:142-152); 20 ms each. */
    WR(Gpu, L2_PWRON_LO, L2);
    if (!MpPoll(Gpu, L2_READY_LO, L2, L2, 20000, &Last)) {
        MpLog(C, "power: L2_READY %08lx != %08lx after 20 ms", Last, L2);
        MpLog(C, "RESULT: FAIL (L2 power-up)");
        return;
    }
    WR(Gpu, TILER_PWRON_LO, Tiler);
    WR(Gpu, SHADER_PWRON_LO, Shader);
    if (!MpPoll(Gpu, TILER_READY_LO, Tiler, Tiler, 20000, &Last)) {
        MpLog(C, "power: TILER_READY %08lx != %08lx after 20 ms", Last, Tiler);
        MpLog(C, "RESULT: FAIL (tiler power-up)");
        return;
    }
    if (!MpPoll(Gpu, SHADER_READY_LO, Shader, Shader, 20000, &Last)) {
        MpLog(C, "power: SHADER_READY %08lx != %08lx after 20 ms", Last, Shader);
        MpLog(C, "RESULT: FAIL (shader power-up)");
        return;
    }
    MpLog(C, "power: L2_READY %08lx TILER_READY %08lx SHADER_READY %08lx", RD(Gpu, L2_READY_LO),
          RD(Gpu, TILER_READY_LO), RD(Gpu, SHADER_READY_LO));

    /* 5. Page tables + job in uncached contiguous memory below the GPU's PA reach (R11, m1probe.c:154-166). */
    PaBits = (RD(Gpu, GPU_MMU_FEATURES) >> 8) & 0xFFu;
    if (PaBits == 0 || PaBits > 40) {
        PaBits = 40;
    }
    Low.QuadPart  = 0;
    High.QuadPart = (LONGLONG)((1ULL << PaBits) - 1);
    Skip.QuadPart = 0;
    Mem = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(NPAGES * PAGE_SIZE, Low, High, Skip, MmNonCached);
    if (Mem == NULL) {
        MpLog(C, "RESULT: FAIL (no %u pages below 2^%lu)", NPAGES, PaBits);
        return;
    }
    RtlZeroMemory(Mem, NPAGES * PAGE_SIZE);
    Base = (ULONG64)MmGetPhysicalAddress(Mem).QuadPart;
    MpLog(C, "memory: %u pages at PA %016llx (limit 2^%lu)", NPAGES, Base, PaBits);

    {
        volatile ULONG64 *L0d = (volatile ULONG64 *)(Mem + 0 * PAGE_SIZE);
        volatile ULONG64 *L1d = (volatile ULONG64 *)(Mem + 1 * PAGE_SIZE);
        volatile ULONG64 *L2d = (volatile ULONG64 *)(Mem + 2 * PAGE_SIZE);
        volatile ULONG64 *L3d = (volatile ULONG64 *)(Mem + 3 * PAGE_SIZE);
        volatile ULONG   *Data = (volatile ULONG *)(Mem + 4 * PAGE_SIZE);

        /* R10: Mali-LPAE, byte-identical to m1probe.c:174-179 (tables type 3, leaf 0x2C5). */
        L0d[(GPU_VA >> 39) & 0x1FF] = (Base + 1 * PAGE_SIZE) | 3;
        L1d[(GPU_VA >> 30) & 0x1FF] = (Base + 2 * PAGE_SIZE) | 3;
        L2d[(GPU_VA >> 21) & 0x1FF] = (Base + 3 * PAGE_SIZE) | 3;
        L3d[(GPU_VA >> 12) & 0x1FF] = (Base + 4 * PAGE_SIZE) | 0x2C5;

        /* R17: WRITE_VALUE job, m1probe.c:181-186. */
        Target   = GPU_VA + TARGET_OFFSET;
        Data[4]  = 1u | (2u << 1) | (1u << 16);   /* is_64b, WRITE_VALUE, index 1 */
        Data[8]  = (ULONG)Target;
        Data[9]  = (ULONG)(Target >> 32);
        Data[10] = 6;                             /* immediate 32 */
        Data[12] = MAGIC;
        KeMemoryBarrier();

        /* 6. Address space 0 (m1probe.c:189-199). */
        WR(Gpu, MMU_INT_CLEAR, 0xFFFFFFFF);
        WR(Gpu, AS0 + AS_TRANSTAB_LO, (ULONG)(Base | 0x7));
        WR(Gpu, AS0 + AS_TRANSTAB_HI, (ULONG)(Base >> 32));
        WR(Gpu, AS0 + AS_MEMATTR_LO, 0x00888D88);
        WR(Gpu, AS0 + AS_MEMATTR_HI, 0);
        WR(Gpu, AS0 + AS_TRANSCFG_LO, 0);
        WR(Gpu, AS0 + AS_TRANSCFG_HI, 0);
        WR(Gpu, AS0 + AS_COMMAND, AS_CMD_UPDATE);
        if (!MpPoll(Gpu, AS0 + AS_STATUS, 1, 0, 100000, &Last)) {
            MpLog(C, "AS0: AS_STATUS %08lx still active after 100 ms", Last);
            MpLog(C, "RESULT: FAIL (AS0 update)");
            goto Out;
        }
        MpLog(C, "AS0: TRANSTAB %08lx:%08lx MEMATTR %08lx TRANSCFG %08lx (Mali-LPAE) - updated",
              RD(Gpu, AS0 + AS_TRANSTAB_HI), RD(Gpu, AS0 + AS_TRANSTAB_LO), RD(Gpu, AS0 + AS_MEMATTR_LO),
              RD(Gpu, AS0 + AS_TRANSCFG_LO));

        /* 7. Interrupts on for the job only, then JS0 (m1probe.c:200-211). */
        WR(Gpu, JOB_INT_CLEAR, 0xFFFFFFFF);
        WR(Gpu, MMU_INT_CLEAR, 0xFFFFFFFF);
        WR(Gpu, JOB_INT_MASK, JOB_INT_JS0);
        WR(Gpu, MMU_INT_MASK, MMU_INT_AS0);
        WR(Gpu, JS0 + JS_HEAD_NEXT_LO, (ULONG)GPU_VA);
        WR(Gpu, JS0 + JS_HEAD_NEXT_HI, (ULONG)(GPU_VA >> 32));
        WR(Gpu, JS0 + JS_AFFINITY_NEXT_LO, Shader);
        WR(Gpu, JS0 + JS_AFFINITY_NEXT_HI, 0);
        WR(Gpu, JS0 + JS_CONFIG_NEXT, JS_CONFIG);
        WR(Gpu, JS0 + JS_COMMAND_NEXT, JS_COMMAND_START);
        MpLog(C, "job: WRITE_VALUE %08lx -> GPU VA %llx submitted on JS0 (affinity %08lx)", MAGIC, Target, Shader);

        for (ULONG i = 0; i < 50000; i++) {       /* 500 ms */
            JobBits = (RD(Gpu, JOB_INT_RAWSTAT) | (ULONG)C->IsrJob) & JOB_INT_JS0;
            if (JobBits != 0) {
                Done = TRUE;                      /* the slot finished: DONE bit 0 or ERR bit 16 */
                break;
            }
            if ((RD(Gpu, MMU_INT_RAWSTAT) | (ULONG)C->IsrMmu) & MMU_INT_AS0) {
                Fault = TRUE;
                break;
            }
            KeStallExecutionProcessor(10);
        }
        KeStallExecutionProcessor(1000);          /* let a just-raised line reach the ISR before the masks go */
        WR(Gpu, JOB_INT_MASK, 0);
        WR(Gpu, MMU_INT_MASK, 0);

        MpLog(C, "job: %s; JOB_INT_RAWSTAT %08lx JS0_STATUS %08lx MMU_INT_RAWSTAT %08lx AS0_FAULTSTATUS %08lx "
                 "AS0_FAULTADDRESS %08lx:%08lx",
              !Done ? (Fault ? "MMU FAULT" : "TIMEOUT (500 ms)") :
                      ((JobBits & 0x00010000u) ? "JS0 FAILED (JOB_INT ERR bit 16)" : "JS0 DONE (JOB_INT bit 0)"),
              RD(Gpu, JOB_INT_RAWSTAT),
              RD(Gpu, JS0 + JS_STATUS), RD(Gpu, MMU_INT_RAWSTAT), RD(Gpu, AS0 + AS_FAULTSTATUS),
              RD(Gpu, AS0 + AS_FAULTADDRESS_HI), RD(Gpu, AS0 + AS_FAULTADDRESS_LO));
        MpLog(C, "irq: ISR saw JOB %08lx MMU %08lx GPU %08lx; hits per line %ld/%ld/%ld (GSIV %lu/%lu/%lu), empty %ld",
              (ULONG)C->IsrJob, (ULONG)C->IsrMmu, (ULONG)C->IsrGpu, C->IsrHits[0], C->IsrHits[1], C->IsrHits[2],
              C->IrqGsiv[0], C->IrqGsiv[1], C->IrqGsiv[2], C->IsrEmpty);

        V = Data[TARGET_OFFSET / 4];
        MpLog(C, "memory: job header word0 (exception status) %08lx, target %08lx (want %08lx)", Data[0], V, MAGIC);
        MpLog(C, "RESULT: %s", (Done && JobBits == 0x1u && V == MAGIC) ? "PASS (the G76 wrote the value)" : "FAIL");

        WR(Gpu, JOB_INT_CLEAR, 0xFFFFFFFF);
        WR(Gpu, MMU_INT_CLEAR, 0xFFFFFFFF);
    }

Out:
    if (!Done) {
        /* The job may still be live: stop the GPU before the memory goes back to Windows. */
        if (!MpSoftReset(C)) {
            Free = FALSE;
            MpLog(C, "cleanup: GPU did not reset - leaking %u pages at %016llx on purpose", NPAGES, Base);
        }
    }
    if (Free) {
        MmFreeContiguousMemorySpecifyCache(Mem, NPAGES * PAGE_SIZE, MmNonCached);
    }
}

/* ======================================================================
 * ISR: records and clears whatever the GPU raised; never touches an unpowered GPU.
 * ====================================================================== */
BOOLEAN MpEvtIsr(WDFINTERRUPT Interrupt, ULONG MessageID)
{
    PDEVICE_CONTEXT C = GetContext(WdfInterruptGetDevice(Interrupt));
    ULONG Job, Mmu, Gp;
    UNREFERENCED_PARAMETER(MessageID);

    if (!C->Usable || C->Gpu == NULL || !MpPowered(C, NULL, NULL)) {
        InterlockedIncrement(&C->IsrEmpty);
        return FALSE;
    }
    Job = RD(C->Gpu, JOB_INT_STAT);
    Mmu = RD(C->Gpu, MMU_INT_STAT);
    Gp  = RD(C->Gpu, GPU_INT_STAT);
    if ((Job | Mmu | Gp) == 0) {
        InterlockedIncrement(&C->IsrEmpty);
        return FALSE;
    }
    if (Job != 0) {
        WR(C->Gpu, JOB_INT_CLEAR, Job);
        InterlockedOr(&C->IsrJob, (LONG)Job);
    }
    if (Mmu != 0) {
        WR(C->Gpu, MMU_INT_CLEAR, Mmu);
        InterlockedOr(&C->IsrMmu, (LONG)Mmu);
    }
    if (Gp != 0) {
        WR(C->Gpu, GPU_INT_CLEAR, Gp);
        InterlockedOr(&C->IsrGpu, (LONG)Gp);
    }
    InterlockedIncrement(&C->IsrHits[GetIrqContext(Interrupt)->Index]);
    return TRUE;
}

/* ======================================================================
 * PnP.
 * ====================================================================== */
NTSTATUS MpEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT C = GetContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Mem[2] = { NULL, NULL };
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Irq[MP_MAX_IRQ] = { NULL };
    ULONG IrqRawIndex[MP_MAX_IRQ] = { 0 };
    ULONG MemCount = 0, IrqCount = 0, Emb, G3d, Id;
    NTSTATUS Status = STATUS_SUCCESS;

    /* A stop/restart (rebalance) runs this again: start from clean counters (the run itself happens once). */
    C->Usable   = FALSE;
    C->IrqCount = 0;
    C->IsrEmpty = 0;
    C->IsrJob   = 0;
    C->IsrMmu   = 0;
    C->IsrGpu   = 0;
    RtlZeroMemory((PVOID)C->IsrHits, sizeof(C->IsrHits));
    RtlZeroMemory(C->IrqGsiv, sizeof(C->IrqGsiv));

    C->Log = (PMP_RUNLOG)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(MP_RUNLOG), MP_POOL_TAG);
    if (C->Log != NULL) {
        KeInitializeSpinLock(&C->Log->Lock);
        C->Log->Start = KeQueryInterruptTime();
    }
    MpLog(C, "MaliG76Probe (M16 G9b) start - %lu resources", WdfCmResourceListGetCount(Translated));

    for (ULONG i = 0; i < WdfCmResourceListGetCount(Translated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR D = WdfCmResourceListGetDescriptor(Translated, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR R = WdfCmResourceListGetDescriptor(Raw, i);
        if (D->Type == CmResourceTypeMemory) {
            MpLog(C, "res %lu: memory %016llx len %lx", i, D->u.Memory.Start.QuadPart, D->u.Memory.Length);
            if (MemCount < 2) {
                Mem[MemCount] = D;
            }
            MemCount++;
        } else if (D->Type == CmResourceTypeInterrupt) {
            MpLog(C, "res %lu: interrupt raw vector %lu (expect the _CRS GSIV) %s, translated vector %lu level %lu", i,
                  R->u.Interrupt.Vector, (R->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? "edge" : "level",
                  D->u.Interrupt.Vector, D->u.Interrupt.Level);
            if (IrqCount < MP_MAX_IRQ) {
                Irq[IrqCount] = D;
                IrqRawIndex[IrqCount] = i;
                C->IrqGsiv[IrqCount] = R->u.Interrupt.Vector;
            }
            IrqCount++;
        }
    }

    /* R1: the GPU block first, then the PMU page; anything else means the DSDT and this driver disagree. */
    if (MemCount != 2 || IrqCount != MP_MAX_IRQ ||
        (ULONG64)Mem[0]->u.Memory.Start.QuadPart != MP_GPU_PA || Mem[0]->u.Memory.Length != MP_GPU_LEN ||
        (ULONG64)Mem[1]->u.Memory.Start.QuadPart != MP_PMU_PAGE_PA || Mem[1]->u.Memory.Length != MP_PMU_PAGE_LEN) {
        MpLog(C, "resources do not match the DSDT GPU0 contract (2 memory: %llx/%x then %llx/%x, 3 interrupts) - "
                 "nothing mapped", MP_GPU_PA, MP_GPU_LEN, MP_PMU_PAGE_PA, MP_PMU_PAGE_LEN);
        MpLogFlush(C);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    C->Pmu = (volatile ULONG *)MmMapIoSpaceEx(Mem[1]->u.Memory.Start, MP_PMU_PAGE_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    if (C->Pmu == NULL) {
        MpLog(C, "PMU page map failed");
        MpLogFlush(C);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* R4: nothing below touches the GPU unless both status bits are 1. */
    if (!MpPowered(C, &Emb, &G3d)) {
        MpLog(C, "G3D NOT powered (EMBEDDED_G3D_STATUS %08lx G3D_STATUS %08lx) - GPU not mapped, no interrupts, "
                 "nothing else runs", Emb, G3d);
        MpLog(C, "RESULT: SKIPPED (not powered)");
        MpLogFlush(C);
        return STATUS_SUCCESS;
    }
    MpLog(C, "G3D powered (EMBEDDED_G3D_STATUS %08lx G3D_STATUS %08lx)", Emb, G3d);

    C->Gpu = (volatile ULONG *)MmMapIoSpaceEx(Mem[0]->u.Memory.Start, MP_GPU_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    if (C->Gpu == NULL) {
        MpLog(C, "GPU map failed");
        MpLogFlush(C);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* R5. */
    Id = RD(C->Gpu, GPU_ID);
    MpLog(C, "GPU_ID %08lx -> ((id >> 16) & %04x) = %04lx, G76 %04x: %s", Id, G76_ID_MASK, (Id >> 16) & G76_ID_MASK,
          G76_ID_MODEL, (((Id >> 16) & G76_ID_MASK) == G76_ID_MODEL) ? "yes" : "NO");
    if (((Id >> 16) & G76_ID_MASK) != G76_ID_MODEL) {
        MpLog(C, "RESULT: FAIL (not a G76) - no interrupts, nothing else runs");
        MpLogFlush(C);
        return STATUS_SUCCESS;
    }

    /* Quiet the GPU's interrupt outputs before the lines are connected. */
    WR(C->Gpu, GPU_INT_MASK, 0);
    WR(C->Gpu, JOB_INT_MASK, 0);
    WR(C->Gpu, MMU_INT_MASK, 0);

    for (ULONG n = 0; n < MP_MAX_IRQ; n++) {
        WDF_INTERRUPT_CONFIG Cfg;
        WDF_OBJECT_ATTRIBUTES Attr;
        WDFINTERRUPT Interrupt;

        WDF_INTERRUPT_CONFIG_INIT(&Cfg, MpEvtIsr, NULL);
        Cfg.InterruptRaw        = WdfCmResourceListGetDescriptor(Raw, IrqRawIndex[n]);
        Cfg.InterruptTranslated = Irq[n];
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IRQ_CONTEXT);
        Status = WdfInterruptCreate(Device, &Cfg, &Attr, &Interrupt);
        if (!NT_SUCCESS(Status)) {
            MpLog(C, "WdfInterruptCreate %lu (GSIV %lu) failed %08lx", n, C->IrqGsiv[n], Status);
            MpLogFlush(C);
            return Status;
        }
        GetIrqContext(Interrupt)->Index = n;
        C->IrqCount++;
    }

    C->Usable = TRUE;
    MpLog(C, "ready: %lu interrupts created; the run starts once they are connected", C->IrqCount);
    MpLogFlush(C);
    return STATUS_SUCCESS;
}

NTSTATUS MpEvtD0EntryPostInterruptsEnabled(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT C = GetContext(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    if (C->Usable && !C->Ran) {
        C->Ran = TRUE;
        MpRun(C);
        MpLogFlush(C);
    }
    return STATUS_SUCCESS;
}

NTSTATUS MpEvtD0ExitPreInterruptsDisabled(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT C = GetContext(Device);
    UNREFERENCED_PARAMETER(TargetState);

    if (C->Usable && C->Gpu != NULL && MpPowered(C, NULL, NULL)) {
        WR(C->Gpu, GPU_INT_MASK, 0);
        WR(C->Gpu, JOB_INT_MASK, 0);
        WR(C->Gpu, MMU_INT_MASK, 0);
    }
    return STATUS_SUCCESS;
}

NTSTATUS MpEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT C = GetContext(Device);
    UNREFERENCED_PARAMETER(Translated);

    C->Usable = FALSE;
    if (C->Gpu != NULL) {
        MmUnmapIoSpace((PVOID)C->Gpu, MP_GPU_LEN);
        C->Gpu = NULL;
    }
    if (C->Pmu != NULL) {
        MmUnmapIoSpace((PVOID)C->Pmu, MP_PMU_PAGE_LEN);
        C->Pmu = NULL;
    }
    if (C->Log != NULL) {
        ExFreePoolWithTag(C->Log, MP_POOL_TAG);
        C->Log = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS MpEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attr;
    WDFDEVICE Device;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware              = MpEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware              = MpEvtReleaseHardware;
    Pnp.EvtDeviceD0EntryPostInterruptsEnabled = MpEvtD0EntryPostInterruptsEnabled;
    Pnp.EvtDeviceD0ExitPreInterruptsDisabled  = MpEvtD0ExitPreInterruptsDisabled;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, DEVICE_CONTEXT);
    return WdfDeviceCreate(&DeviceInit, &Attr, &Device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, MpEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
