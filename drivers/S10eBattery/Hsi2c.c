/*
 * Hsi2c.c - polled HSI2C45 master, a line-by-line port of the device-proven UEFI code in
 * firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteUsbHostDxe/Pmic.c (Hsi2cBusSetup, Hsi2cReset, Hsi2cXfer,
 * PmicBusRelease, Max77705RegReadN). Same register writes in the same order; the translations are
 * MmioRead32/MmioWrite32 -> READ/WRITE_REGISTER_ULONG on the _CRS mappings and gBS->Stall -> KeStallExecutionProcessor.
 * Not ported: Pmic.c's Hsi2cDump diagnostic register reads (reads only; the failure prints here log INT_STAT,
 * ERROR_STATUS and TRANS_STATUS instead).
 *
 * Every function here is called with Fdo->BusLock held, at PASSIVE_LEVEL.
 * Clocks: the hsi2c_45 gates are ACPM-owned and left running by the bootloader; like Pmic.c we never touch CMU.
 */

#include "S10eBattery.h"

static __forceinline ULONG I2cRd(_In_ PS10E_BATT_FDO Fdo, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Fdo->I2c + Off));
}

static __forceinline VOID I2cWr(_In_ PS10E_BATT_FDO Fdo, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Fdo->I2c + Off), Val);
}

static __forceinline volatile ULONG *UsiMode(_In_ PS10E_BATT_FDO Fdo)
{
    return (volatile ULONG *)(Fdo->UsiPage + S10E_CMGP_USI03_MODE_OFFSET);
}

static __forceinline volatile ULONG *GpioCon(_In_ PS10E_BATT_FDO Fdo, _In_ ULONG Off)
{
    return (volatile ULONG *)(Fdo->GpioPage + Off);
}

/* Pmic.c Hsi2cReset: SW_RST set/clear, USI reset released, FS timing, MASTER, trailing count, TIMEOUT 0,
   AUTO_MODE, FIFO/AUTO/ADDR cleared, all interrupts acked. */
static VOID Hsi2cReset(_Inout_ PS10E_BATT_FDO Fdo)
{
    I2cWr(Fdo, HSI2C_CTL, I2cRd(Fdo, HSI2C_CTL) | HSI2C_CTL_SW_RST);
    I2cWr(Fdo, HSI2C_CTL, I2cRd(Fdo, HSI2C_CTL) & ~HSI2C_CTL_SW_RST);
    I2cWr(Fdo, HSI2C_USI_CON, 0);
    I2cWr(Fdo, HSI2C_TIMING_FS1, HSI2C_TIMING_FS1_BOOT);
    I2cWr(Fdo, HSI2C_TIMING_FS2, HSI2C_TIMING_FS2_BOOT);
    I2cWr(Fdo, HSI2C_TIMING_FS3, HSI2C_TIMING_FS3_BOOT);
    I2cWr(Fdo, HSI2C_CTL, HSI2C_CTL_MASTER);
    I2cWr(Fdo, HSI2C_TRAIL_CTL, 0xFFFFFF);
    I2cWr(Fdo, HSI2C_TIMEOUT, 0);
    I2cWr(Fdo, HSI2C_CONF, HSI2C_CONF_AUTO_MODE);
    I2cWr(Fdo, HSI2C_FIFO_CTL, 0);
    I2cWr(Fdo, HSI2C_AUTO_CONF, 0);
    I2cWr(Fdo, HSI2C_ADDR, 0);
    I2cWr(Fdo, HSI2C_INT_STAT, HSI2C_INT_ALL);
}

/* Pmic.c Hsi2cBusSetup + PmicBusInit: save the handoff state, USI_CMGP3 mode = I2C, gpm12/gpm13 function 2,
   then the block reset/init (mode + pin mux first: M16 run 4). */
VOID S10eHsi2cBusInit(_Inout_ PS10E_BATT_FDO Fdo)
{
    ULONG Scl;
    ULONG Sda;

    Scl = READ_REGISTER_ULONG(GpioCon(Fdo, S10E_CMGP_GPM12_CON_OFFSET));
    Sda = READ_REGISTER_ULONG(GpioCon(Fdo, S10E_CMGP_GPM13_CON_OFFSET));
    if (!Fdo->BusSaved) {
        Fdo->SavedMode   = READ_REGISTER_ULONG(UsiMode(Fdo));
        Fdo->SavedScl    = Scl;
        Fdo->SavedSda    = Sda;
        Fdo->SavedUsiCon = I2cRd(Fdo, HSI2C_USI_CON);
        Fdo->BusSaved    = TRUE;
    }

    S10eBattPrint(DPFLTR_INFO_LEVEL, "bus at D0Entry: USI_CMGP3 mode %x, gpm12 CON %x, gpm13 CON %x, USI_CON %x\n",
                  READ_REGISTER_ULONG(UsiMode(Fdo)), Scl, Sda, I2cRd(Fdo, HSI2C_USI_CON));

    WRITE_REGISTER_ULONG(UsiMode(Fdo), S10E_USI_V2_MODE_I2C);
    WRITE_REGISTER_ULONG(GpioCon(Fdo, S10E_CMGP_GPM12_CON_OFFSET), (Scl & ~0xFu) | S10E_GPIO_FUNC_HSI2C45);
    WRITE_REGISTER_ULONG(GpioCon(Fdo, S10E_CMGP_GPM13_CON_OFFSET), (Sda & ~0xFu) | S10E_GPIO_FUNC_HSI2C45);

    Hsi2cReset(Fdo);
    Fdo->BusReady = TRUE;
}

/* Pmic.c PmicBusRelease: block idle with interrupts off, USI_CON (reset) back, then mode and both pins back. */
VOID S10eHsi2cBusRelease(_Inout_ PS10E_BATT_FDO Fdo)
{
    Fdo->BusReady = FALSE;
    if (!Fdo->BusSaved) {
        return;
    }

    I2cWr(Fdo, HSI2C_INT_EN, 0);
    I2cWr(Fdo, HSI2C_INT_STAT, HSI2C_INT_ALL);
    I2cWr(Fdo, HSI2C_AUTO_CONF, 0);
    I2cWr(Fdo, HSI2C_CTL, 0);
    I2cWr(Fdo, HSI2C_USI_CON, Fdo->SavedUsiCon);
    WRITE_REGISTER_ULONG(UsiMode(Fdo), Fdo->SavedMode);
    WRITE_REGISTER_ULONG(GpioCon(Fdo, S10E_CMGP_GPM12_CON_OFFSET), Fdo->SavedScl);
    WRITE_REGISTER_ULONG(GpioCon(Fdo, S10E_CMGP_GPM13_CON_OFFSET), Fdo->SavedSda);
    Fdo->BusSaved = FALSE;
}

/* Pmic.c Hsi2cPollIntStat: INT_STAT once any Wanted bit is set, 0 on timeout (100 us steps, 50 ms). */
static ULONG Hsi2cPollIntStat(_In_ PS10E_BATT_FDO Fdo, _In_ ULONG Wanted, _Out_ PULONG Seen)
{
    ULONG ElapsedUs;

    *Seen = 0;
    for (ElapsedUs = 0; ElapsedUs < HSI2C_XFER_TIMEOUT_US; ElapsedUs += 100) {
        *Seen = I2cRd(Fdo, HSI2C_INT_STAT);
        if ((*Seen & Wanted) != 0) {
            return *Seen;
        }

        KeStallExecutionProcessor(100);
    }

    return 0;
}

/* Pmic.c Hsi2cXfer, same steps in the same order. */
NTSTATUS S10eHsi2cXfer(_Inout_ PS10E_BATT_FDO Fdo, _In_ UCHAR SlaveAddr, _In_ BOOLEAN Read,
                       _Inout_updates_(Length) PUCHAR Data, _In_ ULONG Length, _In_ BOOLEAN Stop)
{
    ULONG Ctl;
    ULONG AutoConf;
    ULONG Addr;
    ULONG FifoStat;
    ULONG IntStat;
    ULONG Index;
    ULONG Wait;

    if (!Fdo->BusReady) {
        return STATUS_DEVICE_NOT_READY;
    }

    if ((Length == 0) || (Length > HSI2C_FIFO_DEPTH)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* TIMEOUT register: clear the watchdog-enable bit (kernel clears 1<<31). */
    I2cWr(Fdo, HSI2C_TIMEOUT, I2cRd(Fdo, HSI2C_TIMEOUT) & ~(1u << 31));

    /* FIFO: enable both, trigger level = transfer length. */
    I2cWr(Fdo, HSI2C_FIFO_CTL, HSI2C_FIFO_CTL_RX_EN | HSI2C_FIFO_CTL_TX_EN |
                               HSI2C_FIFO_CTL_RX_TRIG(Length) | HSI2C_FIFO_CTL_TX_TRIG(Length));

    Ctl  = I2cRd(Fdo, HSI2C_CTL);
    Ctl |= HSI2C_CTL_MASTER;
    if (Read) {
        Ctl |= HSI2C_CTL_RXCHON;
        Ctl &= ~HSI2C_CTL_TXCHON;
    } else {
        Ctl |= HSI2C_CTL_TXCHON;
        Ctl &= ~HSI2C_CTL_RXCHON;
    }
    I2cWr(Fdo, HSI2C_CTL, Ctl);

    AutoConf  = I2cRd(Fdo, HSI2C_AUTO_CONF);
    AutoConf &= ~0xFFFFu;
    AutoConf |= Length;
    AutoConf &= ~HSI2C_AUTO_CONF_READ_WRITE;
    if (Read) {
        AutoConf |= HSI2C_AUTO_CONF_READ_WRITE;
    }
    if (Stop) {
        AutoConf |= HSI2C_AUTO_CONF_STOP_AFTER_TRANS;
    } else {
        AutoConf &= ~HSI2C_AUTO_CONF_STOP_AFTER_TRANS;
    }

    /* ADDR: FS master mode - target in bits 19:10, master-id 0x7 in 31:24 [K i2c-exynos5.c:755-763]. */
    Addr  = I2cRd(Fdo, HSI2C_ADDR);
    Addr &= ~(0x3FFu << 10);
    Addr &= ~(0x3FFu << 0);
    Addr &= ~(0xFFu << 24);
    Addr |= ((ULONG)(SlaveAddr & 0x7F)) << 10;
    Addr |= (0x7u << 24);
    I2cWr(Fdo, HSI2C_ADDR, Addr);

    /* Wait for the master to go idle before kicking a new transaction (100 x 1 ms). */
    for (Wait = 0; Wait < 100; Wait++) {
        if ((I2cRd(Fdo, HSI2C_TRANS_STATUS) & HSI2C_MASTER_BUSY) == 0) {
            break;
        }

        KeStallExecutionProcessor(1000);
    }

    if ((I2cRd(Fdo, HSI2C_TRANS_STATUS) & HSI2C_MASTER_BUSY) != 0) {
        S10eBattPrint(DPFLTR_ERROR_LEVEL, "master busy before addr=%x, resetting\n", SlaveAddr);
        Hsi2cReset(Fdo);
        return STATUS_DEVICE_BUSY;
    }

    I2cWr(Fdo, HSI2C_INT_STAT, HSI2C_INT_ALL);
    I2cWr(Fdo, HSI2C_INT_EN, HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS);

    /* Kick. */
    I2cWr(Fdo, HSI2C_AUTO_CONF, AutoConf | HSI2C_AUTO_CONF_MASTER_RUN);

    if (!Read) {
        for (Index = 0; Index < Length; Index++) {
            FifoStat = I2cRd(Fdo, HSI2C_FIFO_STAT);
            if (HSI2C_FIFO_STAT_TX_LEVEL(FifoStat) >= HSI2C_FIFO_DEPTH) {
                return STATUS_DEVICE_DATA_ERROR;
            }

            WRITE_REGISTER_UCHAR((volatile UCHAR *)(Fdo->I2c + HSI2C_TX_DATA), Data[Index]);
        }
    }

    if (Hsi2cPollIntStat(Fdo, HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS, &IntStat) == 0) {
        S10eBattPrint(DPFLTR_ERROR_LEVEL, "timeout addr=%x first=%x INT_STAT=%x\n", SlaveAddr, Data[0], IntStat);
        Hsi2cReset(Fdo);
        return STATUS_IO_TIMEOUT;
    }

    if ((IntStat & HSI2C_INT_ERRORS) != 0) {
        S10eBattPrint(DPFLTR_ERROR_LEVEL, "addr=%x first=%x INT_STAT=%x ERR=%x TRANS_STATUS=%x\n", SlaveAddr,
                      Data[0], IntStat, I2cRd(Fdo, HSI2C_ERROR_STATUS), I2cRd(Fdo, HSI2C_TRANS_STATUS));
        Hsi2cReset(Fdo);
        return ((IntStat & (HSI2C_INT_NODEV | HSI2C_INT_NO_ACK)) != 0) ? STATUS_NO_SUCH_DEVICE
                                                                         : STATUS_DEVICE_DATA_ERROR;
    }

    /* Ack the interrupt (write back). */
    I2cWr(Fdo, HSI2C_INT_STAT, IntStat);

    if (Read) {
        for (Index = 0; Index < Length; Index++) {
            FifoStat = 0;
            for (Wait = 0; Wait < 100; Wait++) {
                FifoStat = I2cRd(Fdo, HSI2C_FIFO_STAT);
                if (HSI2C_FIFO_STAT_RX_LEVEL(FifoStat) > 0) {
                    break;
                }

                KeStallExecutionProcessor(1000);
            }

            if (HSI2C_FIFO_STAT_RX_LEVEL(FifoStat) == 0) {
                return STATUS_IO_TIMEOUT;
            }

            Data[Index] = READ_REGISTER_UCHAR((volatile UCHAR *)(Fdo->I2c + HSI2C_RX_DATA));
        }
    }

    return STATUS_SUCCESS;
}

/* Pmic.c Max77705RegReadN: register pointer WITHOUT stop, then read with restart+stop. Callers read >= 2 bytes:
   on this bus every 1-byte read returned 0 (M16/M17 run 9) while multi-byte reads were right. */
NTSTATUS S10eMax77705ReadN(_Inout_ PS10E_BATT_FDO Fdo, _In_ UCHAR ClientAddr, _In_ UCHAR Reg,
                           _Out_writes_(Length) PUCHAR Buf, _In_ ULONG Length)
{
    NTSTATUS Status;
    UCHAR    Pointer;

    RtlZeroMemory(Buf, Length);
    Pointer = Reg;
    Status  = S10eHsi2cXfer(Fdo, ClientAddr, FALSE, &Pointer, 1, FALSE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    return S10eHsi2cXfer(Fdo, ClientAddr, TRUE, Buf, Length, TRUE);
}
