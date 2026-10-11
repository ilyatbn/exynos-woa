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

// A 1-3 byte PMIC transfer takes well under 1 ms at FS; 50 ms keeps a dead bus from costing a second per
// register (run 1: one 1000 ms timeout; the CC attach poll would have made it ~33 s).
#define HSI2C_TIMEOUT_MS  50

// Error interrupt bits - from our device-validated Windows HSI2C driver (drivers/Exynos9820Hsi2c/
// exynos9820hsi2c.h EHI2C_INTERRUPT_*, same exynos5-hsi2c IP, touch bus works with it). Run 1 enabled
// only TRANSFER_DONE, so a NACK and a dead block looked the same (INT_STAT=0).
#define HSI2C_INT_TRANSFER_ABORT  BIT8
#define HSI2C_INT_NO_ACK          BIT9
#define HSI2C_INT_TIMEOUT         BIT11
#define HSI2C_INT_ERRORS          (HSI2C_INT_TRANSFER_ABORT | HSI2C_INT_NO_ACK | HSI2C_INT_NODEV | HSI2C_INT_TIMEOUT)
#define HSI2C_INT_ALL             0x00000FFFu
#define HSI2C_ERROR_STATUS        0x2C


STATIC BOOLEAN  mBusReady = FALSE;
STATIC UINTN    mDumps    = 0;

// Mirrors our device-validated Windows driver's ControllerReset (drivers/Exynos9820Hsi2c/controller.cpp):
// SW_RST set/clear, USI reset released, FS timing, MASTER, trailing count, TIMEOUT 0, AUTO_MODE, FIFO/AUTO/
// ADDR cleared, all interrupts acked. Timing: the HSI2C_TIMING_FS*_BOOT values equal what the Windows
// driver's ControllerSetBusTiming computes for 400 kHz from the 200 MHz default-clk (divider 0x21, high
// mask 0xE0, start mask 0xF0) - the formula that runs the touch bus. Run 4 (notes/m16-usb.md) showed the
// bootloader leaves this block unconfigured (all zero), so there is nothing of its to preserve.
// Also used after a failed transfer, as the Windows driver does, so one NACK cannot wedge later probes.
STATIC
VOID
Hsi2cReset (VOID)
{
  MmioWrite32 (I2C + HSI2C_CTL, MmioRead32 (I2C + HSI2C_CTL) | HSI2C_CTL_SW_RST);
  MmioWrite32 (I2C + HSI2C_CTL, MmioRead32 (I2C + HSI2C_CTL) & ~HSI2C_CTL_SW_RST);
  MmioWrite32 (I2C + HSI2C_USI_CON, 0);
  MmioWrite32 (I2C + HSI2C_TIMING_FS1, HSI2C_TIMING_FS1_BOOT);
  MmioWrite32 (I2C + HSI2C_TIMING_FS2, HSI2C_TIMING_FS2_BOOT);
  MmioWrite32 (I2C + HSI2C_TIMING_FS3, HSI2C_TIMING_FS3_BOOT);
  MmioWrite32 (I2C + HSI2C_CTL, HSI2C_CTL_MASTER);
  MmioWrite32 (I2C + HSI2C_TRAIL_CTL, 0xFFFFFF);
  MmioWrite32 (I2C + HSI2C_TIMEOUT, 0);
  MmioWrite32 (I2C + HSI2C_CONF, HSI2C_CONF_AUTO_MODE);
  MmioWrite32 (I2C + HSI2C_FIFO_CTL, 0);
  MmioWrite32 (I2C + HSI2C_AUTO_CONF, 0);
  MmioWrite32 (I2C + HSI2C_ADDR, 0);
  MmioWrite32 (I2C + HSI2C_INT_STAT, HSI2C_INT_ALL);
}

// The bus outside the I2C block: USI_CMGP3 mode = I2C and the two pins muxed to the I2C function. Same
// three things the Windows driver does for USI03 (sysreg mode, GPP1 CON), with the CMGP addresses from
// <Platform/Exynos9820.h>. Pull/drive are left as found (the DT values could not be read on device).
// State found at handoff, put back by PmicBusRelease (run 5: with the bus left configured, Windows
// bugchecked 0x14C FATAL_ABNORMAL_RESET_ERROR on every boot; run 4 - bus untouched - booted).
STATIC BOOLEAN  mBusSaved = FALSE;
STATIC UINT32   mSavedMode;
STATIC UINT32   mSavedScl;
STATIC UINT32   mSavedSda;
STATIC UINT32   mSavedUsiCon;

STATIC
VOID
Hsi2cBusSetup (VOID)
{
  volatile UINT32  *Mode = (volatile UINT32 *)(UINTN)EXYNOS_CMGP_USI03_MODE;
  UINTN            Gpio  = (UINTN)EXYNOS_CMGP_GPIO_BASE;
  UINT32           Scl, Sda;

  Scl = MmioRead32 (Gpio + EXYNOS_CMGP_GPM12_CON_OFFSET);
  Sda = MmioRead32 (Gpio + EXYNOS_CMGP_GPM13_CON_OFFSET);
  DEBUG ((DEBUG_WARN, "Hsi2c45 bus at handoff: USI_CMGP3 mode %x, gpm12 CON %x, gpm13 CON %x\n", *Mode, Scl, Sda));
  mSavedMode   = *Mode;
  mSavedScl    = Scl;
  mSavedSda    = Sda;
  mSavedUsiCon = MmioRead32 (I2C + HSI2C_USI_CON);
  mBusSaved    = TRUE;

  *Mode = EXYNOS_USI_V2_MODE_I2C;
  MmioWrite32 (Gpio + EXYNOS_CMGP_GPM12_CON_OFFSET, (Scl & ~0xFu) | EXYNOS_GPIO_FUNC_HSI2C45);
  MmioWrite32 (Gpio + EXYNOS_CMGP_GPM13_CON_OFFSET, (Sda & ~0xFu) | EXYNOS_GPIO_FUNC_HSI2C45);

  DEBUG ((DEBUG_WARN, "Hsi2c45 bus set: USI_CMGP3 mode %x, gpm12 CON %x, gpm13 CON %x\n", *Mode,
          MmioRead32 (Gpio + EXYNOS_CMGP_GPM12_CON_OFFSET), MmioRead32 (Gpio + EXYNOS_CMGP_GPM13_CON_OFFSET)));
}

VOID
Hsi2cDump (IN CONST CHAR8 *Label)
{
  if (mDumps++ >= 6) {
    return;
  }
  DEBUG ((DEBUG_WARN, "Hsi2c45 %a: CTL %x FIFO_CTL %x TRAIL %x INT_EN %x INT_STAT %x ERR %x FIFO_STAT %x\n",
          Label, MmioRead32 (I2C + HSI2C_CTL), MmioRead32 (I2C + HSI2C_FIFO_CTL), MmioRead32 (I2C + HSI2C_TRAIL_CTL),
          MmioRead32 (I2C + HSI2C_INT_EN), MmioRead32 (I2C + HSI2C_INT_STAT), MmioRead32 (I2C + HSI2C_ERROR_STATUS),
          MmioRead32 (I2C + HSI2C_FIFO_STAT)));
  DEBUG ((DEBUG_WARN, "Hsi2c45 %a: CONF %x AUTO_CONF %x TIMEOUT %x TRANS_STATUS %x FS1 %x FS2 %x FS3 %x ADDR %x USI_CON %x USI_OPT %x\n",
          Label, MmioRead32 (I2C + HSI2C_CONF), MmioRead32 (I2C + HSI2C_AUTO_CONF), MmioRead32 (I2C + HSI2C_TIMEOUT),
          MmioRead32 (I2C + HSI2C_TRANS_STATUS), MmioRead32 (I2C + HSI2C_TIMING_FS1), MmioRead32 (I2C + HSI2C_TIMING_FS2),
          MmioRead32 (I2C + HSI2C_TIMING_FS3), MmioRead32 (I2C + HSI2C_ADDR), MmioRead32 (I2C + HSI2C_USI_CON),
          MmioRead32 (I2C + HSI2C_USI_OPTION)));
}

// Returns the INT_STAT seen once any Wanted bit is set, 0 on timeout.
STATIC
UINT32
Hsi2cPollIntStat (IN UINT32 Wanted, OUT UINT32 *Seen)
{
  UINTN  ElapsedUs;

  for (ElapsedUs = 0; ElapsedUs < HSI2C_TIMEOUT_MS * 1000; ElapsedUs += 100) {
    *Seen = MmioRead32 (I2C + HSI2C_INT_STAT);
    if ((*Seen & Wanted) != 0) {
      return *Seen;
    }

    gBS->Stall (100);
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
      DEBUG ((DEBUG_ERROR, "%a: master busy before addr=%x, resetting\n", __func__, SlaveAddr));
      Hsi2cDump ("master busy");
      Hsi2cReset ();
      return EFI_DEVICE_ERROR;
    }
  }

  MmioWrite32 (I2C + HSI2C_INT_STAT, HSI2C_INT_ALL);   // ack stale bits (Windows driver does the same)
  MmioWrite32 (I2C + HSI2C_INT_EN, HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS);

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
  if (Hsi2cPollIntStat (HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS, &IntStat) == 0) {
    DEBUG ((DEBUG_ERROR, "%a: timeout addr=%x reg/first=%x INT_STAT=%x\n",
            __func__, SlaveAddr, Data[0], IntStat));
    Hsi2cDump ("xfer timeout");
    Hsi2cReset ();
    return EFI_TIMEOUT;
  }

  if ((IntStat & HSI2C_INT_ERRORS) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: addr=%x reg/first=%x INT_STAT=%x ERR=%x TRANS_STATUS=%x\n", __func__, SlaveAddr,
            Data[0], IntStat, MmioRead32 (I2C + HSI2C_ERROR_STATUS), MmioRead32 (I2C + HSI2C_TRANS_STATUS)));
    Hsi2cReset ();
    return ((IntStat & (HSI2C_INT_NODEV | HSI2C_INT_NO_ACK)) != 0) ? EFI_NOT_FOUND : EFI_DEVICE_ERROR;
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
  Hsi2cDump ("at handoff");

  // Run 4: USI held in reset, every I2C register 0, CTL writes did not stick - the USI was never put in
  // I2C mode. Mode + pin mux first, then the block reset/init (Windows driver order).
  Hsi2cBusSetup ();
  Hsi2cReset ();
  mBusReady = TRUE;
  Hsi2cDump ("after init");
  return EFI_SUCCESS;
}

// Hand the bus back exactly as the bootloader left it: I2C block idle with interrupts off and the USI
// back in reset, then the USI mode and both pin functions restored. The MAX77705 keeps whatever state we
// programmed (e.g. the VBUS boost) - that lives in the PMIC, not on the bus.
VOID
PmicBusRelease (VOID)
{
  if (!mBusSaved) {
    return;
  }

  MmioWrite32 (I2C + HSI2C_INT_EN, 0);
  MmioWrite32 (I2C + HSI2C_INT_STAT, HSI2C_INT_ALL);
  MmioWrite32 (I2C + HSI2C_AUTO_CONF, 0);
  MmioWrite32 (I2C + HSI2C_CTL, 0);
  MmioWrite32 (I2C + HSI2C_USI_CON, mSavedUsiCon);
  *(volatile UINT32 *)(UINTN)EXYNOS_CMGP_USI03_MODE = mSavedMode;
  MmioWrite32 ((UINTN)EXYNOS_CMGP_GPIO_BASE + EXYNOS_CMGP_GPM12_CON_OFFSET, mSavedScl);
  MmioWrite32 ((UINTN)EXYNOS_CMGP_GPIO_BASE + EXYNOS_CMGP_GPM13_CON_OFFSET, mSavedSda);
  mBusReady = FALSE;
  mBusSaved = FALSE;
  DEBUG ((DEBUG_WARN, "Hsi2c45 bus released: USI_CMGP3 mode %x, gpm12 CON %x, gpm13 CON %x, USI_CON %x\n",
          *(volatile UINT32 *)(UINTN)EXYNOS_CMGP_USI03_MODE,
          MmioRead32 ((UINTN)EXYNOS_CMGP_GPIO_BASE + EXYNOS_CMGP_GPM12_CON_OFFSET),
          MmioRead32 ((UINTN)EXYNOS_CMGP_GPIO_BASE + EXYNOS_CMGP_GPM13_CON_OFFSET),
          MmioRead32 (I2C + HSI2C_USI_CON)));
}

// M16 run 2: which MAX77705 clients answer at all (register 0x00 of each; reads only). Returns TRUE when
// the CCIC (0x25) answered, i.e. the CC-attach poll is worth running.
BOOLEAN
PmicBusProbe (VOID)
{
  // PMIC top 0x66 (live DT child max77705@66), CCIC, charger, fuel gauge.
  STATIC CONST UINT8  Clients[] = { 0x66, MAX77705_CCIC_ADDR, MAX77705_CHG_ADDR, MAX77705_FG_ADDR };
  EFI_STATUS          Status;
  UINT8               Value;
  UINTN               Index;
  BOOLEAN             CcicOk;

  CcicOk = FALSE;
  for (Index = 0; Index < ARRAY_SIZE (Clients); Index++) {
    Value  = 0;
    Status = Max77705RegRead (Clients[Index], 0x00, &Value);
    DEBUG ((DEBUG_WARN, "PmicBusProbe: client %x reg 0 -> %r value %x\n", Clients[Index], Status, Value));
    if ((Clients[Index] == MAX77705_CCIC_ADDR) && !EFI_ERROR (Status)) {
      CcicOk = TRUE;
    }
  }
  return CcicOk;
}

EFI_STATUS
Max77705RegWrite (IN UINT8 ClientAddr, IN UINT8 Reg, IN UINT8 Value)
{
  UINT8  Buf[2];

  Buf[0] = Reg;
  Buf[1] = Value;
  return Hsi2cXfer (ClientAddr, FALSE, Buf, 2, TRUE);
}

STATIC
EFI_STATUS
Max77705RegReadN (IN UINT8 ClientAddr, IN UINT8 Reg, OUT UINT8 *Buf, IN UINTN Length)
{
  EFI_STATUS  Status;
  UINT8       Pointer;

  // Two-phase like the kernel's i2c msg pair: write the register pointer
  // WITHOUT stop, then read with restart+stop.
  Pointer = Reg;
  Status  = Hsi2cXfer (ClientAddr, FALSE, &Pointer, 1, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Hsi2cXfer (ClientAddr, TRUE, Buf, Length, TRUE);
}

EFI_STATUS
Max77705RegRead (IN UINT8 ClientAddr, IN UINT8 Reg, OUT UINT8 *Value)
{
  EFI_STATUS  Status;
  UINT8       Buf[2];

  // M16/M17 (run 9 UEFILOG): every 1-byte read returned 0 (USBC FW_REV 0 where Linux reports 1A, CC_STATUS0 0
  // with a dock attached) while every multi-byte read was right (fuel-gauge words, opcode responses). Read two
  // bytes (MAX77705 auto-increments) and keep the first. The extra byte is the next register; the only
  // read-to-clear case is UIC_INT -> CC_INT, which nothing in this driver consumes.
  Buf[0] = Buf[1] = 0;
  Status = Max77705RegReadN (ClientAddr, Reg, Buf, 2);
  *Value = Buf[0];
  return Status;
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

//
// M17 (notes/m17-muic-switch.md): one USBC firmware opcode - [OPCODE_WRITE, opcode, data...], then OPCODE_WRITE_END
// = 0, wait for APCmdResI, read the response from OPCODE_READ ([0] must echo the opcode). The kernel waits for the
// interrupt; we poll UIC_INT (read-to-clear) and fall back to the elapsed time if the bit never latches while masked.
//
STATIC
EFI_STATUS
Max77705Opcode (IN UINT8 Opcode, IN CONST UINT8 *Data, IN UINTN DataLength, OUT UINT8 *Response, IN UINTN ResponseLength)
{
  EFI_STATUS  Status;
  UINT8       Buf[16];
  UINT8       Int;
  UINTN       Ms;
  UINTN       Index;

  if ((DataLength > sizeof (Buf) - 2) || (ResponseLength == 0) || (ResponseLength > 16)) {
    return EFI_INVALID_PARAMETER;
  }

  (VOID)Max77705RegRead (MAX77705_CCIC_ADDR, MAX77705_UIC_INT, &Int);   // drop a stale response flag
  Buf[0] = MAX77705_OPCODE_WRITE;
  Buf[1] = Opcode;
  for (Index = 0; Index < DataLength; Index++) {
    Buf[2 + Index] = Data[Index];
  }

  Status = Hsi2cXfer (MAX77705_CCIC_ADDR, FALSE, Buf, 2 + DataLength, TRUE);
  if (!EFI_ERROR (Status)) {
    Status = Max77705RegWrite (MAX77705_CCIC_ADDR, MAX77705_OPCODE_WRITE_END, 0);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Int = 0;
  for (Ms = 0; Ms < 100; Ms++) {
    gBS->Stall (1000);
    if (!EFI_ERROR (Max77705RegRead (MAX77705_CCIC_ADDR, MAX77705_UIC_INT, &Int)) && (Int & MAX77705_UIC_INT_APCMDRES)) {
      break;
    }
  }

  Buf[0] = MAX77705_OPCODE_READ;
  Status = Hsi2cXfer (MAX77705_CCIC_ADDR, FALSE, Buf, 1, FALSE);
  if (!EFI_ERROR (Status)) {
    Status = Hsi2cXfer (MAX77705_CCIC_ADDR, TRUE, Response, ResponseLength, TRUE);
  }

  DEBUG ((DEBUG_INFO, "Max77705Opcode %x: response after %u ms (UIC_INT %x) echo %x data %x, %r\n", Opcode, Ms, Int,
          Response[0], (ResponseLength > 1) ? Response[1] : 0, Status));
  if (!EFI_ERROR (Status) && (Response[0] != Opcode)) {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}

EFI_STATUS
Max77705UsbSwitchToAp (VOID)
{
  EFI_STATUS  Status;
  UINT8       Rev;
  UINT8       Bc;
  UINT8       St1;
  UINT8       St2;
  UINT8       Value;
  UINT8       Resp[2];

  Rev = Bc = St1 = St2 = 0;
  Resp[0] = Resp[1] = 0;
  {
    UINT8  One;

    One = 0xEE;
    (VOID)Max77705RegReadN (MAX77705_CCIC_ADDR, MAX77705_USBC_FW_REV, &One, 1);
    DEBUG ((DEBUG_INFO, "%a: FW_REV 1-byte read = %x (diagnostic of the 1-byte read bug)\n", __func__, One));
  }

  (VOID)Max77705RegRead (MAX77705_CCIC_ADDR, MAX77705_USBC_FW_REV, &Rev);
  (VOID)Max77705RegRead (MAX77705_CCIC_ADDR, MAX77705_BC_STATUS, &Bc);
  (VOID)Max77705RegRead (MAX77705_CCIC_ADDR, MAX77705_USBC_STATUS1, &St1);
  (VOID)Max77705RegRead (MAX77705_CCIC_ADDR, MAX77705_USBC_STATUS2, &St2);
  DEBUG ((DEBUG_INFO, "%a: USBC fw %x BC_STATUS %x (VBUSDet %u ChgTyp %u DCDTmo %u) STATUS1 %x STATUS2 %x\n", __func__,
          Rev, Bc, (Bc >> 7) & 1, Bc & 3, (Bc >> 2) & 1, St1, St2));

  Status = Max77705Opcode (MAX77705_OPC_CTRL1_R, NULL, 0, Resp, 2);
  DEBUG ((DEBUG_INFO, "%a: CONTROL1 before = %x (%a)\n", __func__, Resp[1],
          (Resp[1] == MAX77705_COM_USB_AP) ? "USB-AP" : (Resp[1] == MAX77705_COM_OPEN) ? "OPEN" : "other"));

  Value  = MAX77705_COM_USB_AP;
  Status = Max77705Opcode (MAX77705_OPC_CTRL1_W, &Value, 1, Resp, 2);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: CTRL1_W %r\n", __func__, Status));
    return Status;
  }

  Status = Max77705Opcode (MAX77705_OPC_CTRL1_R, NULL, 0, Resp, 2);
  DEBUG ((DEBUG_INFO, "%a: CONTROL1 after = %x, %r\n", __func__, Resp[1], Status));
  if (!EFI_ERROR (Status) && (Resp[1] != MAX77705_COM_USB_AP)) {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}
