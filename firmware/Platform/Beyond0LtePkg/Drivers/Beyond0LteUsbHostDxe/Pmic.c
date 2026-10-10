// Pmic.c — polled HSI2C master on the MAX77705 bus + host-role VBUS.
//
// The 9820 has NO VBUS pad and no over-current input (is_not_vbus_pad=1,
// use_io_for_ovc=0): the only 5 V source on the connector is the MAX77705
// interface PMIC's boost converter, reached over hsi2c_45 (0x15D60000).
// Sequence pinned in notes/d1b-usb-vbus-hcd.md Q1/Q2 from the LineageOS
// exynos9820 kernel (drivers/i2c/busses/i2c-exynos5.c POLLING branch,
// drivers/battery_v2/max77705_charger.c, drivers/ccic/*).
//
// The CCIC Type-C controller is firmware-loaded and left DRP (CCCTRL1=0x19)
// by the bootloader, so a plain hub/dock (Rd) attaches the phone as power
// SOURCE on its own; our job is to notice (CC_STATUS0.CCStat == 2) and turn
// the boost on. We never touch the CC mailbox.
//
// Clocks: the hsi2c_45 gates are ACPM-owned vclk/QCH entries — same as USB.
// The bus is registered (hence clocked + pinned) under the bootloader
// (15d60000 live in /sys/devices/platform, notes/s10e-live-reads.md §2), so
// we audit via CTRL_VER-like reads and never write CMU.

#include "UsbHostInternal.h"

#define I2C  EXYNOS_PMIC_HSI2C_BASE

#define HSI2C_TIMEOUT_MS  1000

STATIC BOOLEAN  mBusReady = FALSE;

STATIC
UINT32
Hsi2cPollIntStat (IN UINT32 Wanted, OUT UINT32 *Seen)
{
  UINTN  ElapsedUs;

  for (ElapsedUs = 0; ElapsedUs < HSI2C_TIMEOUT_MS * 1000; ElapsedUs += 1000) {
    *Seen = MmioRead32 (I2C + HSI2C_INT_STAT);
    if ((*Seen & Wanted) == Wanted) {
      return *Seen;
    }

    gBS->Stall (1000);
  }

  return 0;
}

EFI_STATUS
Hsi2cXfer (IN UINT8 SlaveAddr, IN BOOLEAN Read, IN OUT UINT8 *Data, IN UINTN Length, IN BOOLEAN Stop)
{
  UINT32  Ctl;
  UINT32  AutoConf;
  UINT32  Addr;
  UINT32  FifoStat;
  UINT32  IntStat;
  UINTN   Index;

  if (!mBusReady) {
    return EFI_NOT_READY;
  }

  if ((Length == 0) || (Length > 16)) {
    return EFI_INVALID_PARAMETER;   // single FIFO, 16 deep [K FIFO_SIZE]
  }

  // TIMEOUT register: clear the watchdog-enable bit (kernel clears 1<<31).
  MmioWrite32 (I2C + HSI2C_TIMEOUT, MmioRead32 (I2C + HSI2C_TIMEOUT) & ~BIT31);

  // FIFO: enable both, trigger level = transfer length (short-xfer branch of
  // the kernel's xfer_msg).
  MmioWrite32 (I2C + HSI2C_FIFO_CTL,
               HSI2C_FIFO_CTL_RX_EN | HSI2C_FIFO_CTL_TX_EN |
               HSI2C_FIFO_CTL_RX_TRIG (Length) | HSI2C_FIFO_CTL_TX_TRIG (Length));

  Ctl  = MmioRead32 (I2C + HSI2C_CTL);
  Ctl |= HSI2C_CTL_MASTER;
  if (Read) {
    Ctl |= HSI2C_CTL_RXCHON;
    Ctl &= ~HSI2C_CTL_TXCHON;
  } else {
    Ctl |= HSI2C_CTL_TXCHON;
    Ctl &= ~HSI2C_CTL_RXCHON;
  }

  MmioWrite32 (I2C + HSI2C_CTL, Ctl);

  AutoConf  = MmioRead32 (I2C + HSI2C_AUTO_CONF);
  AutoConf &= ~0xFFFF;                       // length field
  AutoConf |= (UINT32)Length;
  AutoConf &= ~HSI2C_AUTO_CONF_READ_WRITE;
  if (Read) {
    AutoConf |= HSI2C_AUTO_CONF_READ_WRITE;
  }

  if (Stop) {
    AutoConf |= HSI2C_AUTO_CONF_STOP_AFTER_TRANS;
  } else {
    AutoConf &= ~HSI2C_AUTO_CONF_STOP_AFTER_TRANS;
  }

  // ADDR: FS master mode — target in bits 19:10, master-id 0x7 in 31:24
  // [K i2c-exynos5.c:755-763].
  Addr  = MmioRead32 (I2C + HSI2C_ADDR);
  Addr &= ~(0x3FFu << 10);
  Addr &= ~(0x3FFu << 0);
  Addr &= ~(0xFFu << 24);
  Addr |= ((UINT32)(SlaveAddr & 0x7F)) << 10;
  Addr |= (0x7u << 24);
  MmioWrite32 (I2C + HSI2C_ADDR, Addr);

  // Wait for the master to go idle before kicking a new transaction.
  {
    UINTN  Wait;

    for (Wait = 0; Wait < 100; Wait++) {
      if ((MmioRead32 (I2C + HSI2C_TRANS_STATUS) & HSI2C_MASTER_BUSY) == 0) {
        break;
      }

      gBS->Stall (1000);
    }

    if ((MmioRead32 (I2C + HSI2C_TRANS_STATUS) & HSI2C_MASTER_BUSY) != 0) {
      return EFI_DEVICE_ERROR;
    }
  }

  MmioWrite32 (I2C + HSI2C_INT_EN, HSI2C_INT_TRANSFER_DONE);

  // Kick.
  MmioWrite32 (I2C + HSI2C_AUTO_CONF, AutoConf | HSI2C_AUTO_CONF_MASTER_RUN);

  if (!Read) {
    // Feed the TX FIFO while it has room (kernel polling branch).
    for (Index = 0; Index < Length; Index++) {
      FifoStat = MmioRead32 (I2C + HSI2C_FIFO_STAT);
      if (HSI2C_FIFO_STAT_TX_LEVEL (FifoStat) >= 16) {
        // FIFO full before we finished feeding: this driver only ever sends
        // 1-3 byte PMIC writes, so this is a hard error, not a wait.
        return EFI_DEVICE_ERROR;
      }

      MmioWrite8 (I2C + HSI2C_TX_DATA, Data[Index]);
    }
  }

  // Wait for TRANSFER_DONE and (for writes) the FIFO to drain.
  if (Hsi2cPollIntStat (HSI2C_INT_TRANSFER_DONE, &IntStat) == 0) {
    DEBUG ((DEBUG_ERROR, "%a: timeout addr=%x reg/first=%x INT_STAT=%x\n",
            __func__, SlaveAddr, Data[0], IntStat));
    return EFI_TIMEOUT;
  }

  if ((IntStat & HSI2C_INT_NODEV) != 0) {
    return EFI_NOT_FOUND;
  }

  // Ack the interrupt (write back).
  MmioWrite32 (I2C + HSI2C_INT_STAT, IntStat);

  if (Read) {
    for (Index = 0; Index < Length; Index++) {
      UINTN  Wait;

      for (Wait = 0; Wait < 100; Wait++) {
        FifoStat = MmioRead32 (I2C + HSI2C_FIFO_STAT);
        if (HSI2C_FIFO_STAT_RX_LEVEL (FifoStat) > 0) {
          break;
        }

        gBS->Stall (1000);
      }

      if (HSI2C_FIFO_STAT_RX_LEVEL (FifoStat) == 0) {
        return EFI_TIMEOUT;
      }

      Data[Index] = MmioRead8 (I2C + HSI2C_RX_DATA);
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
PmicBusInit (VOID)
{
  // USI_CON: release the USI block reset (kernel writes 0; USI_RESET=(0<<0)).
  MmioWrite32 (I2C + HSI2C_USI_CON, 0);

  // SW reset pulse. [K exynos5_i2c_reset] — SW_RST self-clears; the read is
  // the confirmation, the value is not used.
  MmioWrite32 (I2C + HSI2C_CTL, HSI2C_CTL_SW_RST);
  (VOID)MmioRead32 (I2C + HSI2C_CTL);

  // Timing: the block is live from the bootloader; compare against the
  // kernel-computed FS values for this ipclk and only write if different
  // (UsbHw.h note: values UNVERIFIED on our silicon, reads are the fact).
  if ((MmioRead32 (I2C + HSI2C_TIMING_FS1) & 0x00FF0000) != (HSI2C_TIMING_FS1_BOOT & 0x00FF0000)) {
    UINT32  T;

    T  = MmioRead32 (I2C + HSI2C_TIMING_FS1) & ~0x00FF0000;
    MmioWrite32 (I2C + HSI2C_TIMING_FS1, T | (HSI2C_TIMING_FS1_BOOT & 0x00FF0000));
    T  = MmioRead32 (I2C + HSI2C_TIMING_FS2) & ~0xFF;
    MmioWrite32 (I2C + HSI2C_TIMING_FS2, T | (HSI2C_TIMING_FS2_BOOT & 0xFF));
    T  = MmioRead32 (I2C + HSI2C_TIMING_FS3) & ~0x00FF0000;
    MmioWrite32 (I2C + HSI2C_TIMING_FS3, T | (HSI2C_TIMING_FS3_BOOT & 0x00FF0000));
  }

  // Master mode, trailing count, AUTO_MODE. [K exynos5_i2c_init]
  MmioWrite32 (I2C + HSI2C_CTL, HSI2C_CTL_MASTER);
  MmioWrite32 (I2C + HSI2C_TRAIL_CTL, 0xFFFFFF);
  MmioWrite32 (I2C + HSI2C_CONF, MmioRead32 (I2C + HSI2C_CONF) | HSI2C_CONF_AUTO_MODE);

  mBusReady = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
Max77705RegWrite (IN UINT8 ClientAddr, IN UINT8 Reg, IN UINT8 Value)
{
  UINT8  Buf[2];

  Buf[0] = Reg;
  Buf[1] = Value;
  return Hsi2cXfer (ClientAddr, FALSE, Buf, 2, TRUE);
}

EFI_STATUS
Max77705RegRead (IN UINT8 ClientAddr, IN UINT8 Reg, OUT UINT8 *Value)
{
  EFI_STATUS  Status;
  UINT8       Buf;

  // Two-phase like the kernel's i2c msg pair: write the register pointer
  // WITHOUT stop, then read one byte with restart+stop.
  Buf = Reg;
  Status = Hsi2cXfer (ClientAddr, FALSE, &Buf, 1, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Hsi2cXfer (ClientAddr, TRUE, &Buf, 1, TRUE);
}

EFI_STATUS
Max77705VbusOn (VOID)
{
  EFI_STATUS  Status;
  UINT8       Reg = 0;   // init: gcc -Wmaybe-uninitialized across the I2C
                         // read helpers; the reads guard use, but be explicit

  // OTG current limit first: 500 mA (the safe setting for a NON-POWERED hub:
  // USB 2.0 bus-powered hub budget is 5 unit loads = 500 mA total,
  // notes/d1-usb-host.md Q5).
  Status = Max77705RegRead (MAX77705_CHG_ADDR, MAX77705_CHG_CNFG_02, &Reg);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Reg = (UINT8)((Reg & ~0xC0) | (MAX77705_OTG_ILIM_500MA << 6));
  Status = Max77705RegWrite (MAX77705_CHG_ADDR, MAX77705_CHG_CNFG_02, Reg);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Mode transition through BUCK-only with a 1 ms delay — explicit in the
  // kernel's UNO->OTG branches (max77705_charger.c "mode 0x4, and 1msec
  // delay, and then otg on"). 0xE = BUCK+BOOST+OTG with the charger engine
  // OFF (0xF would also run charge; OFF is the conservative choice while the
  // phone also sits on battery).
  Status = Max77705RegRead (MAX77705_CHG_ADDR, MAX77705_CHG_CNFG_00, &Reg);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Max77705RegWrite (MAX77705_CHG_ADDR, MAX77705_CHG_CNFG_00,
                             (UINT8)((Reg & ~MAX77705_MODE_MASK) | MAX77705_MODE_BUCK));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  gBS->Stall (2000);

  Status = Max77705RegRead (MAX77705_CHG_ADDR, MAX77705_CHG_CNFG_00, &Reg);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Max77705RegWrite (MAX77705_CHG_ADDR, MAX77705_CHG_CNFG_00,
                           (UINT8)((Reg & ~MAX77705_MODE_MASK) | MAX77705_MODE_OTG_ON));
}

EFI_STATUS
Max77705WaitSourceAttach (IN UINTN TimeoutMs, OUT UINT8 *CcStatus)
{
  UINTN   ElapsedMs;
  UINT8   Status0 = 0;
  UINTN   Chunk;

  for (ElapsedMs = 0; ElapsedMs < TimeoutMs; ElapsedMs += Chunk) {
    Chunk = 100;
    if (Chunk > (TimeoutMs - ElapsedMs)) {
      Chunk = TimeoutMs - ElapsedMs;
    }

    if (!EFI_ERROR (Max77705RegRead (MAX77705_CCIC_ADDR, MAX77705_CC_STATUS0, &Status0))) {
      if (CcStatus != NULL) {
        *CcStatus = Status0;
      }

      if ((Status0 & 0x7) == MAX77705_CCSTAT_SOURCE) {
        return EFI_SUCCESS;
      }
    }

    gBS->Stall (Chunk * 1000);
  }

  return EFI_TIMEOUT;
}
