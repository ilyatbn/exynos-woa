/*
 * Codec.c - CS47L93 (3.5 mm jack codec) over spi_2: bus bring-up, reset, ID, BOOT_DONE (notes/d8-jack-windows.md J1).
 *
 * Port of the device-proven UEFI code (Beyond0LteAudioProbeDxe.c:465-745: SpiInit / SpiXfer / CodecRead / CodecWrite,
 * CodecIdentify, CodecWaitBoot). Polled, byte-wide, manual chip select.
 *
 * The three register pages are NOT in AUD0 _CRS: DSDT I2C0 (touch) already claims the CMU_PERIC0 / USI sysreg / PERIC0
 * GPIO pages, so they are mapped with MmMapIoSpaceEx directly, as Exynos9820Ufs does for PMU/WDT (no resource-list
 * validation). Only registers no other driver uses are written: USI01 mode (+0x0C; touch uses USI03 +0x1C), gpp0
 * pins 4-7 (touch uses gpp1), gpg1-0 (codec reset).
 */
#include "Exynos9820Audio.h"

/* Exynos9820.h (VERIFIED there) */
#define CODEC_SPI2_BASE         0x10470000ULL   /* EXYNOS_AUDIO_SPI2_BASE, reg size 0x100 */
#define CODEC_USI_SYSREG_BASE   0x10421000ULL   /* EXYNOS_PERIC0_USI_SYSREG_BASE */
#define CODEC_GPIO_BASE         0x10430000ULL   /* EXYNOS_PERIC0_GPIO_BASE */
#define CODEC_USI01_MODE        0x0Cu           /* EXYNOS_USI01_MODE_OFFSET */
#define CODEC_USI_MODE_SPI      0x2u            /* EXYNOS_USI_V2_MODE_SPI */
#define CODEC_GPP0              0x000u          /* EXYNOS_GPIO_GPP0_OFF */
#define CODEC_GPG1              0x0A0u          /* EXYNOS_GPIO_GPG1_OFF */
#define CODEC_GPIO_DAT          0x004u          /* EXYNOS_GPIO_BANK_DAT */
#define CODEC_SPI_FIRST_PIN     4u              /* gpp0-4..7, EXYNOS_SPI2_FIRST_PIN / _PIN_COUNT */
#define CODEC_SPI_PIN_COUNT     4u
#define CODEC_GPIO_FUNC_SPI     0x2u            /* EXYNOS_GPIO_FUNC_SPI2 */
#define CODEC_GPIO_FUNC_OUT     0x1u
#define CODEC_RESET_PIN         0u              /* gpg1-0 */
#define CODEC_SILICON_ID        0x6371u         /* EXYNOS_CS47L93_SILICON_ID */

/* Beyond0LteAudioProbeDxe/AudioProbeHw.h (Samsung spi-s3c64xx citations there) */
#define SPI_CH_CFG              0x00u
#define   SPI_CH_HS_EN            (1u << 6)
#define   SPI_CH_SW_RST           (1u << 5)
#define   SPI_CH_SLAVE            (1u << 4)
#define   SPI_CPOL_L              (1u << 3)
#define   SPI_CPHA_B              (1u << 2)
#define   SPI_CH_RXCH_ON          (1u << 1)
#define   SPI_CH_TXCH_ON          (1u << 0)
#define SPI_MODE_CFG            0x08u
#define   SPI_MODE_CH_TSZ_MASK    (3u << 29)
#define   SPI_MODE_BUS_TSZ_MASK   (3u << 17)
#define   SPI_MODE_TRAILCNT       (0x3FFu << 19)
#define   SPI_MODE_RXDMA_ON       (1u << 2)
#define   SPI_MODE_TXDMA_ON       (1u << 1)
#define   SPI_MODE_4BURST         (1u << 0)
#define SPI_SLAVE_SEL           0x0Cu
#define   SPI_SLAVE_SIG_INACT     (1u << 0)
#define SPI_INT_EN              0x10u
#define SPI_STATUS              0x14u
#define   SPI_STATUS_TX_LVL(s)    (((s) >> 6) & 0x7F)
#define   SPI_STATUS_RX_LVL(s)    (((s) >> 15) & 0x7F)
#define SPI_TX_DATA             0x18u
#define SPI_RX_DATA             0x1Cu
#define SPI_PACKET_CNT          0x20u
#define   SPI_PACKET_CNT_EN       (1u << 16)
#define SPI_PENDING_CLR         0x24u
#define   SPI_PND_ALL             0x1Eu
#define SPI_SWAP_CFG            0x28u
#define SPI_FB_CLK              0x2Cu
#define SPI_USI_CON             0xC4u
#define SPI_FIFO_DEPTH          64u
#define SPI_TIMEOUT_US          10000u

#define CS47L93_REG_SOFTWARE_RESET   0x0000u
#define CS47L93_REG_HW_REVISION      0x0001u
#define CS47L93_REG_IRQ1_RAW_STATUS1 0x1880u
#define CS47L93_BOOT_DONE            (1u << 7)
#define CS47L93_SPI_READ             0x80u

#define R32(B, O)       READ_REGISTER_ULONG((volatile ULONG *)((B) + (O)))
#define W32(B, O, V)    WRITE_REGISTER_ULONG((volatile ULONG *)((B) + (O)), (V))
#define RMW32(B, O, C, S) W32(B, O, (R32(B, O) & ~(ULONG)(C)) | (ULONG)(S))

static VOID CodecPinFunc(_In_ PAUD_DEVICE_CONTEXT Dev, ULONG Bank, ULONG Pin, ULONG Func)
{
    RMW32(Dev->CodecGpio, Bank, 0xFu << (Pin * 4), Func << (Pin * 4));
}

static VOID SpiFlush(_In_ PAUD_DEVICE_CONTEXT Dev)
{
    PUCHAR S = Dev->CodecSpi;
    ULONG  Loops;

    RMW32(S, SPI_CH_CFG, SPI_CH_RXCH_ON | SPI_CH_TXCH_ON, 0);
    W32(S, SPI_PACKET_CNT, 0);
    RMW32(S, SPI_CH_CFG, SPI_CH_HS_EN, SPI_CH_SW_RST);
    for (Loops = 0; Loops < 1000 && SPI_STATUS_TX_LVL(R32(S, SPI_STATUS)) != 0; Loops++) {
        KeStallExecutionProcessor(1);
    }
    for (Loops = 0; Loops < 1000 && SPI_STATUS_RX_LVL(R32(S, SPI_STATUS)) != 0; Loops++) {
        (VOID)R32(S, SPI_RX_DATA);
    }
    RMW32(S, SPI_CH_CFG, SPI_CH_SW_RST, 0);
    RMW32(S, SPI_MODE_CFG, SPI_MODE_TXDMA_ON | SPI_MODE_RXDMA_ON, 0);
}

static VOID SpiInit(_In_ PAUD_DEVICE_CONTEXT Dev)
{
    PUCHAR S = Dev->CodecSpi;

    W32(S, SPI_USI_CON, 0);
    W32(S, SPI_SLAVE_SEL, SPI_SLAVE_SIG_INACT);
    W32(S, SPI_INT_EN, 0);
    W32(S, SPI_MODE_CFG, 0);
    W32(S, SPI_PACKET_CNT, 0);
    W32(S, SPI_PENDING_CLR, SPI_PND_ALL);
    W32(S, SPI_PENDING_CLR, 0);
    W32(S, SPI_SWAP_CFG, 0);
    RMW32(S, SPI_MODE_CFG, SPI_MODE_4BURST | SPI_MODE_TRAILCNT, SPI_MODE_TRAILCNT);
    SpiFlush(Dev);
    RMW32(S, SPI_CH_CFG, SPI_CH_SLAVE | SPI_CPOL_L | SPI_CPHA_B, 0);              /* mode 0, master */
    RMW32(S, SPI_MODE_CFG, SPI_MODE_BUS_TSZ_MASK | SPI_MODE_CH_TSZ_MASK, 0);      /* 8-bit */
    W32(S, SPI_FB_CLK, 1);                                                         /* spi-feedback-delay 1 */
}

/* Full duplex, Length <= FIFO depth, CS asserted around it. Caller holds CodecLock. */
static NTSTATUS SpiXfer(_In_ PAUD_DEVICE_CONTEXT Dev, _In_reads_(Length) const UCHAR *Tx, _Out_writes_(Length) PUCHAR Rx,
                        ULONG Length)
{
    PUCHAR S = Dev->CodecSpi;
    ULONG  ChCfg, ModeCfg, I, Us;

    if (Length == 0 || Length > SPI_FIFO_DEPTH) {
        return STATUS_INVALID_PARAMETER;
    }
    ChCfg   = (R32(S, SPI_CH_CFG) & ~SPI_CH_TXCH_ON) | SPI_CH_RXCH_ON;
    ModeCfg = R32(S, SPI_MODE_CFG) & ~SPI_MODE_4BURST;
    RMW32(S, SPI_PACKET_CNT, SPI_PACKET_CNT_EN, 0);
    W32(S, SPI_PACKET_CNT, Length & 0xFFFF);
    RMW32(S, SPI_PACKET_CNT, 0, SPI_PACKET_CNT_EN);
    W32(S, SPI_MODE_CFG, ModeCfg);
    ModeCfg = R32(S, SPI_MODE_CFG) & ~(SPI_MODE_TXDMA_ON | SPI_MODE_RXDMA_ON);
    for (I = 0; I < Length; I++) {
        WRITE_REGISTER_UCHAR(S + SPI_TX_DATA, Tx[I]);
    }
    W32(S, SPI_MODE_CFG, ModeCfg);
    W32(S, SPI_CH_CFG, ChCfg | SPI_CH_TXCH_ON);
    W32(S, SPI_SLAVE_SEL, 0);
    for (Us = 0; SPI_STATUS_RX_LVL(R32(S, SPI_STATUS)) < Length; Us += 10) {
        if (Us >= SPI_TIMEOUT_US) {
            W32(S, SPI_SLAVE_SEL, SPI_SLAVE_SIG_INACT);
            SpiFlush(Dev);
            RtlZeroMemory(Rx, Length);
            return STATUS_IO_TIMEOUT;
        }
        KeStallExecutionProcessor(10);
    }
    for (I = 0; I < Length; I++) {
        Rx[I] = READ_REGISTER_UCHAR(S + SPI_RX_DATA);
    }
    W32(S, SPI_SLAVE_SEL, SPI_SLAVE_SIG_INACT);
    SpiFlush(Dev);
    return STATUS_SUCCESS;
}

/* Madera regmap-spi framing: 32-bit address (bit 31 = read), 16 pad bits, value big-endian. */
static VOID CodecFrame(_Out_writes_(6) PUCHAR Tx, ULONG Reg, BOOLEAN Read)
{
    Tx[0] = (UCHAR)((Read ? CS47L93_SPI_READ : 0) | ((Reg >> 24) & 0x7F));
    Tx[1] = (UCHAR)(Reg >> 16);
    Tx[2] = (UCHAR)(Reg >> 8);
    Tx[3] = (UCHAR)Reg;
    Tx[4] = 0;
    Tx[5] = 0;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecRead(_Inout_ PAUD_DEVICE_CONTEXT Dev, ULONG Reg, _Out_ PUSHORT Value)
{
    UCHAR    Tx[8] = { 0 }, Rx[8] = { 0 };
    NTSTATUS Status;

    CodecFrame(Tx, Reg, TRUE);
    WdfWaitLockAcquire(Dev->CodecLock, NULL);
    Status = SpiXfer(Dev, Tx, Rx, 8);
    WdfWaitLockRelease(Dev->CodecLock);
    *Value = (USHORT)((Rx[6] << 8) | Rx[7]);
    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecWrite(_Inout_ PAUD_DEVICE_CONTEXT Dev, ULONG Reg, USHORT Value)
{
    UCHAR    Tx[8], Rx[8];
    NTSTATUS Status;

    CodecFrame(Tx, Reg, FALSE);
    Tx[6] = (UCHAR)(Value >> 8);
    Tx[7] = (UCHAR)Value;
    WdfWaitLockAcquire(Dev->CodecLock, NULL);
    Status = SpiXfer(Dev, Tx, Rx, 8);
    WdfWaitLockRelease(Dev->CodecLock);
    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecUpdate(_Inout_ PAUD_DEVICE_CONTEXT Dev, ULONG Reg, USHORT Mask, USHORT Value)
{
    USHORT   Old = 0;
    NTSTATUS Status = CodecRead(Dev, Reg, &Old);

    return NT_SUCCESS(Status) ? CodecWrite(Dev, Reg, (USHORT)((Old & ~Mask) | (Value & Mask))) : Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecWrite32(_Inout_ PAUD_DEVICE_CONTEXT Dev, ULONG Reg, ULONG Value)
{
    UCHAR    Tx[10], Rx[10];
    NTSTATUS Status;

    CodecFrame(Tx, Reg, FALSE);
    Tx[6] = (UCHAR)(Value >> 24);
    Tx[7] = (UCHAR)(Value >> 16);
    Tx[8] = (UCHAR)(Value >> 8);
    Tx[9] = (UCHAR)Value;
    WdfWaitLockAcquire(Dev->CodecLock, NULL);
    Status = SpiXfer(Dev, Tx, Rx, 10);
    WdfWaitLockRelease(Dev->CodecLock);
    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecRead32(_Inout_ PAUD_DEVICE_CONTEXT Dev, ULONG Reg, _Out_ PULONG Value)
{
    UCHAR    Tx[10] = { 0 }, Rx[10] = { 0 };
    NTSTATUS Status;

    CodecFrame(Tx, Reg, TRUE);
    WdfWaitLockAcquire(Dev->CodecLock, NULL);
    Status = SpiXfer(Dev, Tx, Rx, 10);
    WdfWaitLockRelease(Dev->CodecLock);
    *Value = ((ULONG)Rx[6] << 24) | ((ULONG)Rx[7] << 16) | ((ULONG)Rx[8] << 8) | Rx[9];
    return Status;
}

static VOID CodecUnmap(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    if (Dev->CodecSpi != NULL)    { MmUnmapIoSpace(Dev->CodecSpi, PAGE_SIZE);    Dev->CodecSpi = NULL; }
    if (Dev->CodecSysreg != NULL) { MmUnmapIoSpace(Dev->CodecSysreg, PAGE_SIZE); Dev->CodecSysreg = NULL; }
    if (Dev->CodecGpio != NULL)   { MmUnmapIoSpace(Dev->CodecGpio, PAGE_SIZE);   Dev->CodecGpio = NULL; }
}

/* Map, bus up, hard reset pulse, ID + BOOT_DONE. Dev->CodecUp = TRUE on success. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecInit(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    PHYSICAL_ADDRESS Pa;
    USHORT           Id = 0, Rev = 0, Raw1 = 0;
    ULONG            Pin, Ms;
    NTSTATUS         Status;

    PAGED_CODE();

    Pa.QuadPart = CODEC_SPI2_BASE;       Dev->CodecSpi    = (PUCHAR)MmMapIoSpaceEx(Pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = CODEC_USI_SYSREG_BASE; Dev->CodecSysreg = (PUCHAR)MmMapIoSpaceEx(Pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = CODEC_GPIO_BASE;       Dev->CodecGpio   = (PUCHAR)MmMapIoSpaceEx(Pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Dev->CodecSpi == NULL || Dev->CodecSysreg == NULL || Dev->CodecGpio == NULL) {
        AudLog(Dev->Log, "codec: map failed\r\n");
        CodecUnmap(Dev);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    AudLog(Dev->Log, "codec: at entry usi01 mode %lX gpp0 CON %08lX gpg1 CON %08lX DAT %02lX spi CH %lX MODE %lX USI_CON %lX\r\n",
           R32(Dev->CodecSysreg, CODEC_USI01_MODE), R32(Dev->CodecGpio, CODEC_GPP0), R32(Dev->CodecGpio, CODEC_GPG1),
           R32(Dev->CodecGpio, CODEC_GPG1 + CODEC_GPIO_DAT), R32(Dev->CodecSpi, SPI_CH_CFG), R32(Dev->CodecSpi, SPI_MODE_CFG),
           R32(Dev->CodecSpi, SPI_USI_CON));

    /* reset asserted (DAT low, then output), bus up, then release: every register at its default */
    RMW32(Dev->CodecGpio, CODEC_GPG1 + CODEC_GPIO_DAT, 1u << CODEC_RESET_PIN, 0);
    CodecPinFunc(Dev, CODEC_GPG1, CODEC_RESET_PIN, CODEC_GPIO_FUNC_OUT);
    W32(Dev->CodecSysreg, CODEC_USI01_MODE, CODEC_USI_MODE_SPI);
    for (Pin = CODEC_SPI_FIRST_PIN; Pin < CODEC_SPI_FIRST_PIN + CODEC_SPI_PIN_COUNT; Pin++) {
        CodecPinFunc(Dev, CODEC_GPP0, Pin, CODEC_GPIO_FUNC_SPI);
    }
    SpiInit(Dev);
    KeStallExecutionProcessor(2000);
    RMW32(Dev->CodecGpio, CODEC_GPG1 + CODEC_GPIO_DAT, 0, 1u << CODEC_RESET_PIN);
    KeStallExecutionProcessor(2000);   /* madera_disable_hard_reset: usleep_range(1000, 2000) */

    Status = CodecRead(Dev, CS47L93_REG_SOFTWARE_RESET, &Id);
    (VOID)CodecRead(Dev, CS47L93_REG_HW_REVISION, &Rev);
    for (Ms = 0; NT_SUCCESS(Status) && Ms <= 25; Ms += 5) {   /* madera_wait_for_boot: 5 ms steps, 25 ms */
        Status = CodecRead(Dev, CS47L93_REG_IRQ1_RAW_STATUS1, &Raw1);
        if (Raw1 & CS47L93_BOOT_DONE) {
            break;
        }
        KeStallExecutionProcessor(5000);
    }
    Dev->CodecUp = NT_SUCCESS(Status) && Id == CODEC_SILICON_ID && (Raw1 & CS47L93_BOOT_DONE);
    AudLog(Dev->Log, "codec: ID %04X HW_REV %02X IRQ1_RAW_STATUS1 %04X (BOOT_DONE after %lu ms) %08lX -> %s\r\n", Id, Rev & 0xFF,
           Raw1, Ms, (ULONG)Status, Dev->CodecUp ? "CS47L92/93 OK" : "NOT UP");
    if (!Dev->CodecUp) {
        CodecRelease(Dev);
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

/* Codec back into hard reset (outputs dead), bus left in SPI mode, pages unmapped. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID CodecRelease(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    PAGED_CODE();

    if (Dev->CodecGpio != NULL) {
        RMW32(Dev->CodecGpio, CODEC_GPG1 + CODEC_GPIO_DAT, 1u << CODEC_RESET_PIN, 0);
    }
    Dev->CodecUp = FALSE;
    CodecUnmap(Dev);
}
