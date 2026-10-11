/** @file
  Beyond0LteAudioProbeDxe - D8a audio milestone 1: prove the AP can talk to the S10e's audio chips.

  Reads, and logs to UEFILOG (DEBUG -> Beyond0LteFbSerialPortLib ring -> Beyond0LteBootLogDxe):
    * the Cirrus CS47L93 codec's silicon ID (reg 0x0000, expect 0x6371) over spi_2 (0x10470000),
    * both Cirrus CS35L40 amps' DEVID/REVID (expect 0x35A40 / 0x35B40) over hsi2c_38 (0x109A0000)
      at 0x40 (right, bottom speaker) and 0x41 (left, receiver).

  Every fact is cited in notes/d8-audio-m1.md §1 (LineageOS lineage-22.2 vendor kernel + cal-if) and
  every address comes from <Platform/Exynos9820.h>.

  Shape (the Beyond0LteUsbHostDxe/Pmic.c hsi2c_45 pattern, device-validated in M16/M17): log the state
  found at handoff, configure the bus (USI mode where the vendor kernel sets one, pin functions,
  controller init), read, then put every touched register back to the handoff value - a bus left
  configured behind Windows' back was once suspected of bugcheck 0x14C (M16 run 5).

  Reset lines: the chips only answer out of reset. The probe first reads with the reset line as found;
  only if that fails AND the line is not an output driven high does it release the reset (pin =
  output high, the vendor drivers' own sequence and delays), read again, and finally restore the pin,
  which puts the chip back in reset as sboot left it. Gated by BEYOND0LTE_AUDIO_PROBE_RELEASE_RESETS.

  What is NOT touched: PMIC rails (the codec's DCVDD is LDO18M, not boot-on in the vendor DT - if
  sboot left it off the codec cannot answer, and the log says so), CMU (clocks are read for the log,
  never written), the ABOX.

  Guard: boot progress word 0x8FE0F000[1] = 0x69 while the probe runs; a boot whose previous boot
  ended at 0x69 skips the probe (same self-healing shape as UsbHostDxe's 0x67).

  NOT VALIDATED ON HARDWARE (stage Guided).
**/

#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Platform/Exynos9820.h>

#include "AudioProbeHw.h"

// 0 = the driver does nothing (one log line).
#ifndef BEYOND0LTE_AUDIO_PROBE
#define BEYOND0LTE_AUDIO_PROBE  1
#endif

// 0 = never drive a reset pin: read-only w.r.t. chip state (the buses are still configured + restored).
#ifndef BEYOND0LTE_AUDIO_PROBE_RELEASE_RESETS
#define BEYOND0LTE_AUDIO_PROBE_RELEASE_RESETS  1
#endif

#define AUDIO_PROGRESS_TRYING  0x69u
STATIC volatile UINT32  *CONST  mProgress = (volatile UINT32 *)(UINTN)0x8FE0F000ULL;

#define SPI    ((UINTN)EXYNOS_AUDIO_SPI2_BASE)
#define I2C    ((UINTN)EXYNOS_AUDIO_HSI2C_BASE)
#define GPIO0  ((UINTN)EXYNOS_PERIC0_GPIO_BASE)
#define GPIO1  ((UINTN)EXYNOS_PERIC1_GPIO_BASE)

#define HSI2C_TIMEOUT_US  50000u   // a 4-byte transfer at 400 kHz is ~100 us
#define SPI_TIMEOUT_US    10000u   // 8 bytes at >= 1 MHz is < 100 us

// ---------------------------------------------------------------------------------------------------
// Pin helpers. Exynos bank: CON +0x0 (4 bits per pin), DAT +0x4 (1 bit per pin).
// ---------------------------------------------------------------------------------------------------

STATIC
UINT32
PinFunc (IN UINTN Gpio, IN UINT32 Bank, IN UINT32 Pin)
{
  return (MmioRead32 (Gpio + Bank) >> (Pin * 4)) & 0xF;
}

STATIC
UINT32
PinLevel (IN UINTN Gpio, IN UINT32 Bank, IN UINT32 Pin)
{
  return (MmioRead32 (Gpio + Bank + EXYNOS_GPIO_BANK_DAT) >> Pin) & 1;
}

STATIC
VOID
PinSetFunc (IN UINTN Gpio, IN UINT32 Bank, IN UINT32 Pin, IN UINT32 Func)
{
  MmioAndThenOr32 (Gpio + Bank, ~(0xFu << (Pin * 4)), Func << (Pin * 4));
}

// Reset pin out of reset: DAT high first, then CON = output, so the pin never glitches low.
STATIC
VOID
PinDriveHigh (IN UINTN Gpio, IN UINT32 Bank, IN UINT32 Pin)
{
  MmioOr32 (Gpio + Bank + EXYNOS_GPIO_BANK_DAT, 1u << Pin);
  PinSetFunc (Gpio, Bank, Pin, EXYNOS_GPIO_FUNC_OUTPUT);
}

// Reset pin into reset: DAT low first, then CON = output (the kernel's GPIOD_OUT_LOW request).
STATIC
VOID
PinDriveLow (IN UINTN Gpio, IN UINT32 Bank, IN UINT32 Pin)
{
  MmioAnd32 (Gpio + Bank + EXYNOS_GPIO_BANK_DAT, ~(1u << Pin));
  PinSetFunc (Gpio, Bank, Pin, EXYNOS_GPIO_FUNC_OUTPUT);
}

// TRUE unless the pin is an output driven high - the only state in which an active-low RESET is
// released. Boot #88: the amp resets were fn 0 (input) lvl 0 at handoff, and the old rule
// ("output AND low") never fired for them (notes/d8-audio-m1.md §5.1).
STATIC
BOOLEAN
PinNotDrivenHigh (IN UINTN Gpio, IN UINT32 Bank, IN UINT32 Pin)
{
  return (PinFunc (Gpio, Bank, Pin) != EXYNOS_GPIO_FUNC_OUTPUT) || (PinLevel (Gpio, Bank, Pin) == 0);
}

// ---------------------------------------------------------------------------------------------------
// Clock diagnostics (read only). A gate whose MANUAL bit is set and CG_VAL clear is forced off: the
// block's SFRs may then not respond, so the probe skips that bus instead of touching it.
// ---------------------------------------------------------------------------------------------------

STATIC
BOOLEAN
GateForcedOff (IN UINT32 Con)
{
  return ((Con & CMU_GATE_MANUAL) != 0) && ((Con & CMU_GATE_CG_VAL) == 0);
}

// ---------------------------------------------------------------------------------------------------
// hsi2c_38 + CS35L40. HSI2C sequence = Pmic.c (device-validated on hsi2c_45), base parameterised.
// ---------------------------------------------------------------------------------------------------

STATIC UINT32  mI2cSavedUsiCon;
STATIC UINT32  mGph0SavedCon;

STATIC
VOID
I2cDump (IN CONST CHAR8 *Label)
{
  DEBUG ((DEBUG_WARN, "AudioProbe: hsi2c38 %a: CTL %x FIFO_CTL %x INT_STAT %x ERR %x FIFO_STAT %x CONF %x AUTO_CONF %x"
                      " TRANS_STATUS %x FS1 %x FS3 %x ADDR %x USI_CON %x\n",
          Label, MmioRead32 (I2C + HSI2C_CTL), MmioRead32 (I2C + HSI2C_FIFO_CTL), MmioRead32 (I2C + HSI2C_INT_STAT),
          MmioRead32 (I2C + HSI2C_ERROR_STATUS), MmioRead32 (I2C + HSI2C_FIFO_STAT), MmioRead32 (I2C + HSI2C_CONF),
          MmioRead32 (I2C + HSI2C_AUTO_CONF), MmioRead32 (I2C + HSI2C_TRANS_STATUS), MmioRead32 (I2C + HSI2C_TIMING_FS1),
          MmioRead32 (I2C + HSI2C_TIMING_FS3), MmioRead32 (I2C + HSI2C_ADDR), MmioRead32 (I2C + HSI2C_USI_CON)));
}

STATIC
VOID
I2cReset (VOID)
{
  MmioOr32 (I2C + HSI2C_CTL, HSI2C_CTL_SW_RST);
  MmioAnd32 (I2C + HSI2C_CTL, ~HSI2C_CTL_SW_RST);
  MmioWrite32 (I2C + HSI2C_USI_CON, 0);                  // [I] exynos_usi_init: release the USI reset
  MmioWrite32 (I2C + HSI2C_TIMING_FS1, HSI2C_TIMING_FS1_400K);
  MmioWrite32 (I2C + HSI2C_TIMING_FS2, HSI2C_TIMING_FS2_400K);
  MmioWrite32 (I2C + HSI2C_TIMING_FS3, HSI2C_TIMING_FS3_400K);
  MmioWrite32 (I2C + HSI2C_CTL, HSI2C_CTL_MASTER);
  MmioWrite32 (I2C + HSI2C_TRAIL_CTL, 0xFFFFFF);
  MmioWrite32 (I2C + HSI2C_TIMEOUT, 0);
  MmioWrite32 (I2C + HSI2C_CONF, HSI2C_CONF_AUTO_MODE);
  MmioWrite32 (I2C + HSI2C_FIFO_CTL, 0);
  MmioWrite32 (I2C + HSI2C_AUTO_CONF, 0);
  MmioWrite32 (I2C + HSI2C_ADDR, 0);
  MmioWrite32 (I2C + HSI2C_INT_STAT, HSI2C_INT_ALL);
}

STATIC
EFI_STATUS
I2cXfer (IN UINT8 Slave, IN BOOLEAN Read, IN OUT UINT8 *Data, IN UINTN Length, IN BOOLEAN Stop)
{
  UINT32  Ctl;
  UINT32  AutoConf;
  UINT32  Addr;
  UINT32  IntStat;
  UINT32  FifoStat;
  UINTN   Index;
  UINTN   Us;

  if ((Length == 0) || (Length > 16)) {
    return EFI_INVALID_PARAMETER;
  }

  MmioAnd32 (I2C + HSI2C_TIMEOUT, ~BIT31);
  MmioWrite32 (I2C + HSI2C_FIFO_CTL, HSI2C_FIFO_CTL_RX_EN | HSI2C_FIFO_CTL_TX_EN |
               HSI2C_FIFO_CTL_RX_TRIG (Length) | HSI2C_FIFO_CTL_TX_TRIG (Length));

  Ctl  = MmioRead32 (I2C + HSI2C_CTL) | HSI2C_CTL_MASTER;
  Ctl &= ~(HSI2C_CTL_RXCHON | HSI2C_CTL_TXCHON);
  Ctl |= Read ? HSI2C_CTL_RXCHON : HSI2C_CTL_TXCHON;
  MmioWrite32 (I2C + HSI2C_CTL, Ctl);

  AutoConf  = MmioRead32 (I2C + HSI2C_AUTO_CONF) & ~(0xFFFFu | HSI2C_AUTO_CONF_READ_WRITE | HSI2C_AUTO_CONF_STOP_AFTER_TRANS);
  AutoConf |= (UINT32)Length;
  if (Read) {
    AutoConf |= HSI2C_AUTO_CONF_READ_WRITE;
  }

  if (Stop) {
    AutoConf |= HSI2C_AUTO_CONF_STOP_AFTER_TRANS;
  }

  Addr  = MmioRead32 (I2C + HSI2C_ADDR) & ~((0x3FFu << 10) | 0x3FFu | (0xFFu << 24));
  Addr |= ((UINT32)(Slave & 0x7F) << 10) | (0x7u << 24);   // [I:755-763] FS master id 0x7
  MmioWrite32 (I2C + HSI2C_ADDR, Addr);

  for (Us = 0; (MmioRead32 (I2C + HSI2C_TRANS_STATUS) & HSI2C_MASTER_BUSY) != 0; Us += 100) {
    if (Us >= HSI2C_TIMEOUT_US) {
      I2cDump ("master busy");
      I2cReset ();
      return EFI_DEVICE_ERROR;
    }

    gBS->Stall (100);
  }

  MmioWrite32 (I2C + HSI2C_INT_STAT, HSI2C_INT_ALL);
  MmioWrite32 (I2C + HSI2C_INT_EN, HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS);
  MmioWrite32 (I2C + HSI2C_AUTO_CONF, AutoConf | HSI2C_AUTO_CONF_MASTER_RUN);

  if (!Read) {
    for (Index = 0; Index < Length; Index++) {
      MmioWrite8 (I2C + HSI2C_TX_DATA, Data[Index]);   // Length <= 16 (checked above) = Pmic.c's FIFO bound
    }
  }

  IntStat = 0;
  for (Us = 0; Us < HSI2C_TIMEOUT_US; Us += 50) {
    IntStat = MmioRead32 (I2C + HSI2C_INT_STAT);
    if ((IntStat & (HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS)) != 0) {
      break;
    }

    gBS->Stall (50);
  }

  if ((IntStat & (HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS)) == 0) {
    I2cDump ("xfer timeout");
    I2cReset ();
    return EFI_TIMEOUT;
  }

  if ((IntStat & HSI2C_INT_ERRORS) != 0) {
    DEBUG ((DEBUG_WARN, "AudioProbe: hsi2c38 addr %x %a: INT_STAT %x ERR %x\n", Slave, Read ? "read" : "write",
            IntStat, MmioRead32 (I2C + HSI2C_ERROR_STATUS)));
    I2cReset ();
    return ((IntStat & (HSI2C_INT_NODEV | HSI2C_INT_NO_ACK)) != 0) ? EFI_NOT_FOUND : EFI_DEVICE_ERROR;
  }

  MmioWrite32 (I2C + HSI2C_INT_STAT, IntStat);

  if (Read) {
    for (Index = 0; Index < Length; Index++) {
      for (Us = 0; ; Us += 100) {
        FifoStat = MmioRead32 (I2C + HSI2C_FIFO_STAT);
        if (HSI2C_FIFO_STAT_RX_LEVEL (FifoStat) > 0) {
          break;
        }

        if (Us >= HSI2C_TIMEOUT_US) {
          I2cDump ("rx fifo empty");
          I2cReset ();   // like every other error path, so a half-read cannot wedge the next probe
          return EFI_TIMEOUT;
        }

        gBS->Stall (100);
      }

      Data[Index] = MmioRead8 (I2C + HSI2C_RX_DATA);
    }
  }

  return EFI_SUCCESS;
}

// CS35L40 register read: 32-bit big-endian address (no stop), restart, 32-bit big-endian value
// (cs35l41-i2c.c:25-30 regmap: reg_bits 32, val_bits 32, both REGMAP_ENDIAN_BIG).
STATIC
EFI_STATUS
AmpRead (IN UINT8 Slave, IN UINT32 Reg, OUT UINT32 *Value)
{
  EFI_STATUS  Status;
  UINT8       Buf[4];

  Buf[0] = (UINT8)(Reg >> 24);
  Buf[1] = (UINT8)(Reg >> 16);
  Buf[2] = (UINT8)(Reg >> 8);
  Buf[3] = (UINT8)Reg;
  Status = I2cXfer (Slave, FALSE, Buf, 4, FALSE);
  if (!EFI_ERROR (Status)) {
    Buf[0] = Buf[1] = Buf[2] = Buf[3] = 0;
    Status = I2cXfer (Slave, TRUE, Buf, 4, TRUE);
  }

  *Value = ((UINT32)Buf[0] << 24) | ((UINT32)Buf[1] << 16) | ((UINT32)Buf[2] << 8) | Buf[3];
  return Status;
}

// One amp: DEVID/REVID/OTPID/IRQ1_STATUS4, verdict against cs35l41.c:2143-2149.
STATIC
BOOLEAN
AmpIdentify (IN UINT8 Slave, IN CONST CHAR8 *Name)
{
  EFI_STATUS  Status;
  UINT32      DevId;
  UINT32      RevId;
  UINT32      OtpId;
  UINT32      Irq4;
  UINT32      Expected;

  DevId = RevId = OtpId = Irq4 = 0;
  Status = AmpRead (Slave, CS35L40_REG_DEVID, &DevId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "AudioProbe: amp %a @%x DEVID read %r\n", Name, Slave, Status));
    return FALSE;
  }

  (VOID)AmpRead (Slave, CS35L40_REG_REVID, &RevId);
  (VOID)AmpRead (Slave, CS35L40_REG_OTPID, &OtpId);
  (VOID)AmpRead (Slave, CS35L40_REG_IRQ1_STATUS4, &Irq4);
  Expected = ((RevId & 0x0F) % 2) ? EXYNOS_CS35L40R_CHIP_ID : EXYNOS_CS35L40_CHIP_ID;
  DEBUG ((DEBUG_WARN, "AudioProbe: amp %a @%x DEVID %x REVID %x OTPID %x IRQ1_STATUS4 %x (OTP_BOOT_DONE %u) -> %a\n",
          Name, Slave, DevId, RevId, OtpId, Irq4, (Irq4 & CS35L40_OTP_BOOT_DONE) ? 1 : 0,
          (DevId == Expected) ? "CS35L40 ID OK" : "ID MISMATCH"));
  return DevId == Expected;
}

STATIC
VOID
AmpProbe (VOID)
{
  STATIC CONST struct {
    UINT8          Addr;
    UINT32         ResetPin;
    CONST CHAR8    *Name;
  } Amps[] = {
    { EXYNOS_AMP_R_I2C_ADDR, EXYNOS_AMP_R_RESET_PIN, "R/bot" },
    { EXYNOS_AMP_L_I2C_ADDR, EXYNOS_AMP_L_RESET_PIN, "L/rcv" },
  };
  UINT32   Gate;
  UINT32   Gph1Con;
  UINT32   Gph1Dat;
  UINT32   Gph1Pud;
  UINTN    Index;
  BOOLEAN  Ok[2];
  BOOLEAN  Released;

  Gate = MmioRead32 ((UINTN)EXYNOS_CMU_PERIC1_BASE + EXYNOS_CMU_PERIC1_USI17I2C_PCLK);
  DEBUG ((DEBUG_WARN, "AudioProbe: CMU_PERIC1 USI_I2C mux %x div %x | USI17_I2C gate IPCLK %x PCLK %x QCH %x\n",
          MmioRead32 ((UINTN)EXYNOS_CMU_PERIC1_BASE + EXYNOS_CMU_PERIC1_USI_I2C_MUX),
          MmioRead32 ((UINTN)EXYNOS_CMU_PERIC1_BASE + EXYNOS_CMU_PERIC1_USI_I2C_DIV),
          MmioRead32 ((UINTN)EXYNOS_CMU_PERIC1_BASE + EXYNOS_CMU_PERIC1_USI17I2C_IPCLK), Gate,
          MmioRead32 ((UINTN)EXYNOS_CMU_PERIC1_BASE + EXYNOS_CMU_PERIC1_USI17I2C_QCH)));
  if (GateForcedOff (Gate)) {
    DEBUG ((DEBUG_ERROR, "AudioProbe: hsi2c38 PCLK gate forced off by sboot - amp probe skipped (CMU is not ours to write)\n"));
    return;
  }

  Gph1Con = MmioRead32 (GPIO1 + EXYNOS_GPIO_GPH1_OFF);
  Gph1Dat = MmioRead32 (GPIO1 + EXYNOS_GPIO_GPH1_OFF + EXYNOS_GPIO_BANK_DAT);
  Gph1Pud = MmioRead32 (GPIO1 + EXYNOS_GPIO_GPH1_OFF + EXYNOS_GPIO_BANK_PUD);
  DEBUG ((DEBUG_WARN, "AudioProbe: gph1 PUD %x at handoff (4 bits/pin; reset R %x L %x, 0 = no pull) - read only\n",
          Gph1Pud, (Gph1Pud >> (EXYNOS_AMP_R_RESET_PIN * 4)) & 0xF, (Gph1Pud >> (EXYNOS_AMP_L_RESET_PIN * 4)) & 0xF));
  mGph0SavedCon   = MmioRead32 (GPIO1 + EXYNOS_GPIO_GPH0_OFF);
  mI2cSavedUsiCon = MmioRead32 (I2C + HSI2C_USI_CON);
  DEBUG ((DEBUG_WARN, "AudioProbe: amp bus at handoff: usi_17_i2c mode %x (never written), gph0 CON %x, gph1 CON %x DAT %x"
                      " (reset R gph1-%u fn %x lvl %u, L gph1-%u fn %x lvl %u)\n",
          MmioRead32 ((UINTN)EXYNOS_PERIC1_USI_SYSREG_BASE + EXYNOS_USI17_I2C_MODE_OFFSET), mGph0SavedCon, Gph1Con, Gph1Dat,
          EXYNOS_AMP_R_RESET_PIN, PinFunc (GPIO1, EXYNOS_GPIO_GPH1_OFF, EXYNOS_AMP_R_RESET_PIN),
          PinLevel (GPIO1, EXYNOS_GPIO_GPH1_OFF, EXYNOS_AMP_R_RESET_PIN),
          EXYNOS_AMP_L_RESET_PIN, PinFunc (GPIO1, EXYNOS_GPIO_GPH1_OFF, EXYNOS_AMP_L_RESET_PIN),
          PinLevel (GPIO1, EXYNOS_GPIO_GPH1_OFF, EXYNOS_AMP_L_RESET_PIN)));
  I2cDump ("at handoff");

  // Bus up: pins to the I2C function, controller reset/init.
  PinSetFunc (GPIO1, EXYNOS_GPIO_GPH0_OFF, EXYNOS_HSI2C38_SCL_PIN, EXYNOS_GPIO_FUNC_HSI2C38);
  PinSetFunc (GPIO1, EXYNOS_GPIO_GPH0_OFF, EXYNOS_HSI2C38_SDA_PIN, EXYNOS_GPIO_FUNC_HSI2C38);
  I2cReset ();
  I2cDump ("after init");

  Released = FALSE;
  for (Index = 0; Index < ARRAY_SIZE (Amps); Index++) {
    Ok[Index] = AmpIdentify (Amps[Index].Addr, Amps[Index].Name);
  }

  if ((!Ok[0] || !Ok[1]) && BEYOND0LTE_AUDIO_PROBE_RELEASE_RESETS) {
    // cs35l41-core.c:72-92: gpiod_get(GPIOD_OUT_LOW) = output low (reset asserted), 1 ms, set 1, 2 ms.
    for (Index = 0; Index < ARRAY_SIZE (Amps); Index++) {
      if (!Ok[Index] && PinNotDrivenHigh (GPIO1, EXYNOS_GPIO_GPH1_OFF, Amps[Index].ResetPin)) {
        DEBUG ((DEBUG_WARN, "AudioProbe: amp %a in reset (gph1-%u fn %x lvl %u) - output low, 1 ms, high for the ID read\n",
                Amps[Index].Name, Amps[Index].ResetPin, PinFunc (GPIO1, EXYNOS_GPIO_GPH1_OFF, Amps[Index].ResetPin),
                PinLevel (GPIO1, EXYNOS_GPIO_GPH1_OFF, Amps[Index].ResetPin)));
        PinDriveLow (GPIO1, EXYNOS_GPIO_GPH1_OFF, Amps[Index].ResetPin);
        Released = TRUE;
      }
    }

    if (Released) {
      gBS->Stall (1000);   // cs35l41-core.c:88 usleep_range(1000, 1100) with the reset asserted
      for (Index = 0; Index < ARRAY_SIZE (Amps); Index++) {
        if (!Ok[Index]) {
          PinDriveHigh (GPIO1, EXYNOS_GPIO_GPH1_OFF, Amps[Index].ResetPin);
        }
      }

      DEBUG ((DEBUG_WARN, "AudioProbe: amp resets released: gph1 CON %x DAT %x\n", MmioRead32 (GPIO1 + EXYNOS_GPIO_GPH1_OFF),
              MmioRead32 (GPIO1 + EXYNOS_GPIO_GPH1_OFF + EXYNOS_GPIO_BANK_DAT)));
      gBS->Stall (3000);   // cs35l41-core.c:92 usleep_range(2000, 2100) after the release, + 1 ms margin
      for (Index = 0; Index < ARRAY_SIZE (Amps); Index++) {
        if (!Ok[Index]) {
          Ok[Index] = AmpIdentify (Amps[Index].Addr, Amps[Index].Name);
        }
      }
    }
  }

  // Release: controller idle + USI back in its handoff reset state (Pmic.c PmicBusRelease), pins and
  // reset lines exactly as found.
  MmioWrite32 (I2C + HSI2C_INT_EN, 0);
  MmioWrite32 (I2C + HSI2C_INT_STAT, HSI2C_INT_ALL);
  MmioWrite32 (I2C + HSI2C_AUTO_CONF, 0);
  MmioWrite32 (I2C + HSI2C_CTL, 0);
  MmioWrite32 (I2C + HSI2C_USI_CON, mI2cSavedUsiCon);
  MmioWrite32 (GPIO1 + EXYNOS_GPIO_GPH0_OFF, mGph0SavedCon);
  MmioWrite32 (GPIO1 + EXYNOS_GPIO_GPH1_OFF + EXYNOS_GPIO_BANK_DAT, Gph1Dat);
  MmioWrite32 (GPIO1 + EXYNOS_GPIO_GPH1_OFF, Gph1Con);
  DEBUG ((DEBUG_WARN, "AudioProbe: amps R %a, L %a; bus released: gph0 CON %x gph1 CON %x DAT %x USI_CON %x%a\n",
          Ok[0] ? "OK" : "no answer", Ok[1] ? "OK" : "no answer", MmioRead32 (GPIO1 + EXYNOS_GPIO_GPH0_OFF),
          MmioRead32 (GPIO1 + EXYNOS_GPIO_GPH1_OFF), MmioRead32 (GPIO1 + EXYNOS_GPIO_GPH1_OFF + EXYNOS_GPIO_BANK_DAT),
          MmioRead32 (I2C + HSI2C_USI_CON), Released ? " (resets back to their handoff level)" : ""));
}

// ---------------------------------------------------------------------------------------------------
// spi_2 + CS47L93. Polled, byte-wide, manual chip select: the vendor driver's hwinit / config /
// enable_datapath / enable_cs / wait_for_xfer / flush_fifo order ([S:1448-1477, 742-813, 522-606,
// 609-645, 647-721, 270-316]).
// ---------------------------------------------------------------------------------------------------

STATIC
VOID
SpiDump (IN CONST CHAR8 *Label)
{
  DEBUG ((DEBUG_WARN, "AudioProbe: spi2 %a: CH_CFG %x MODE_CFG %x CS %x INT_EN %x STATUS %x PACKET_CNT %x SWAP %x FB_CLK %x USI_CON %x\n",
          Label, MmioRead32 (SPI + SPI_CH_CFG), MmioRead32 (SPI + SPI_MODE_CFG), MmioRead32 (SPI + SPI_SLAVE_SEL),
          MmioRead32 (SPI + SPI_INT_EN), MmioRead32 (SPI + SPI_STATUS), MmioRead32 (SPI + SPI_PACKET_CNT),
          MmioRead32 (SPI + SPI_SWAP_CFG), MmioRead32 (SPI + SPI_FB_CLK), MmioRead32 (SPI + SPI_USI_CON)));
}

STATIC
VOID
SpiFlush (VOID)
{
  UINTN  Loops;

  MmioAnd32 (SPI + SPI_CH_CFG, ~(SPI_CH_RXCH_ON | SPI_CH_TXCH_ON));
  MmioWrite32 (SPI + SPI_PACKET_CNT, 0);
  MmioAndThenOr32 (SPI + SPI_CH_CFG, ~SPI_CH_HS_EN, SPI_CH_SW_RST);
  for (Loops = 0; (Loops < 1000) && (SPI_STATUS_TX_LVL (MmioRead32 (SPI + SPI_STATUS)) != 0); Loops++) {
    gBS->Stall (1);
  }

  for (Loops = 0; (Loops < 1000) && (SPI_STATUS_RX_LVL (MmioRead32 (SPI + SPI_STATUS)) != 0); Loops++) {
    (VOID)MmioRead32 (SPI + SPI_RX_DATA);
  }

  MmioAnd32 (SPI + SPI_CH_CFG, ~SPI_CH_SW_RST);
  MmioAnd32 (SPI + SPI_MODE_CFG, ~(SPI_MODE_TXDMA_ON | SPI_MODE_RXDMA_ON));
}

STATIC
VOID
SpiInit (VOID)
{
  MmioWrite32 (SPI + SPI_USI_CON, 0);                    // [S:1430] USI reset released
  // hwinit
  MmioWrite32 (SPI + SPI_SLAVE_SEL, SPI_SLAVE_SIG_INACT);
  MmioWrite32 (SPI + SPI_INT_EN, 0);
  MmioWrite32 (SPI + SPI_MODE_CFG, 0);
  MmioWrite32 (SPI + SPI_PACKET_CNT, 0);
  MmioWrite32 (SPI + SPI_PENDING_CLR, SPI_PND_ALL);
  MmioWrite32 (SPI + SPI_PENDING_CLR, 0);
  MmioWrite32 (SPI + SPI_SWAP_CFG, 0);
  MmioAndThenOr32 (SPI + SPI_MODE_CFG, ~(SPI_MODE_4BURST | SPI_MODE_TRAILCNT), SPI_MODE_TRAILCNT);
  SpiFlush ();
  // config: SPI mode 0 (no spi-cpol/spi-cpha in the codec node), master, 8-bit words, swap off
  MmioAnd32 (SPI + SPI_CH_CFG, ~(SPI_CH_SLAVE | SPI_CPOL_L | SPI_CPHA_B));
  MmioAnd32 (SPI + SPI_MODE_CFG, ~(SPI_MODE_BUS_TSZ_MASK | SPI_MODE_CH_TSZ_MASK));
  MmioWrite32 (SPI + SPI_FB_CLK, 1);                     // DT samsung,spi-feedback-delay = <1>
}

// Full-duplex transfer of Length (<= FIFO depth) bytes with CS asserted around it.
STATIC
EFI_STATUS
SpiXfer (IN CONST UINT8 *Tx, OUT UINT8 *Rx, IN UINTN Length)
{
  UINT32  ChCfg;
  UINT32  ModeCfg;
  UINTN   Index;
  UINTN   Us;

  if ((Length == 0) || (Length > SPI_FIFO_DEPTH)) {
    return EFI_INVALID_PARAMETER;
  }

  // enable_datapath (PIO): RX channel always on, packet count = bytes, TX bytes into the FIFO, TX on.
  ChCfg   = (MmioRead32 (SPI + SPI_CH_CFG) & ~SPI_CH_TXCH_ON) | SPI_CH_RXCH_ON;
  ModeCfg = MmioRead32 (SPI + SPI_MODE_CFG) & ~SPI_MODE_4BURST;
  MmioAnd32 (SPI + SPI_PACKET_CNT, ~SPI_PACKET_CNT_EN);
  MmioWrite32 (SPI + SPI_PACKET_CNT, (UINT32)Length & 0xFFFF);
  MmioOr32 (SPI + SPI_PACKET_CNT, SPI_PACKET_CNT_EN);
  MmioWrite32 (SPI + SPI_MODE_CFG, ModeCfg);
  ModeCfg = MmioRead32 (SPI + SPI_MODE_CFG) & ~(SPI_MODE_TXDMA_ON | SPI_MODE_RXDMA_ON);
  for (Index = 0; Index < Length; Index++) {
    MmioWrite8 (SPI + SPI_TX_DATA, Tx[Index]);
  }

  ChCfg |= SPI_CH_TXCH_ON;
  MmioWrite32 (SPI + SPI_MODE_CFG, ModeCfg);
  MmioWrite32 (SPI + SPI_CH_CFG, ChCfg);

  MmioWrite32 (SPI + SPI_SLAVE_SEL, 0);                  // manual CS asserted (enable_cs)

  for (Us = 0; SPI_STATUS_RX_LVL (MmioRead32 (SPI + SPI_STATUS)) < Length; Us += 10) {
    if (Us >= SPI_TIMEOUT_US) {
      SpiDump ("xfer timeout");
      MmioWrite32 (SPI + SPI_SLAVE_SEL, SPI_SLAVE_SIG_INACT);
      SpiFlush ();
      return EFI_TIMEOUT;
    }

    gBS->Stall (10);
  }

  for (Index = 0; Index < Length; Index++) {
    Rx[Index] = MmioRead8 (SPI + SPI_RX_DATA);
  }

  MmioWrite32 (SPI + SPI_SLAVE_SEL, SPI_SLAVE_SIG_INACT); // disable_cs
  SpiFlush ();
  return EFI_SUCCESS;
}

// Madera 16-bit register read: [0x80 | addr31..24, addr23..16, addr15..8, addr7..0, pad, pad] then two
// value bytes, big-endian (cs47l92_16bit_spi_regmap: reg_bits 32, pad_bits 16, val_bits 16).
STATIC
EFI_STATUS
CodecRead (IN UINT32 Reg, OUT UINT16 *Value)
{
  EFI_STATUS  Status;
  UINT8       Tx[8];
  UINT8       Rx[8];
  UINTN       Index;

  for (Index = 0; Index < 8; Index++) {
    Tx[Index] = 0;
    Rx[Index] = 0;
  }

  Tx[0]  = (UINT8)(CS47L93_SPI_READ | ((Reg >> 24) & 0x7F));
  Tx[1]  = (UINT8)(Reg >> 16);
  Tx[2]  = (UINT8)(Reg >> 8);
  Tx[3]  = (UINT8)Reg;
  Status = SpiXfer (Tx, Rx, 8);
  *Value = (UINT16)(((UINT16)Rx[6] << 8) | Rx[7]);
  return Status;
}

STATIC
BOOLEAN
CodecIdentify (VOID)
{
  EFI_STATUS  Status;
  UINT16      Id;
  UINT16      Rev;
  UINT16      Raw1;

  Id = Rev = Raw1 = 0;
  Status = CodecRead (CS47L93_REG_SOFTWARE_RESET, &Id);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "AudioProbe: codec ID read %r\n", Status));
    return FALSE;
  }

  (VOID)CodecRead (CS47L93_REG_HW_REVISION, &Rev);
  (VOID)CodecRead (CS47L93_REG_IRQ1_RAW_STATUS1, &Raw1);
  DEBUG ((DEBUG_WARN, "AudioProbe: codec ID %x HW_REV %x IRQ1_RAW_STATUS1 %x (BOOT_DONE %u) -> %a\n", Id, Rev & 0xFF,
          Raw1, (Raw1 & CS47L93_BOOT_DONE) ? 1 : 0,
          (Id == EXYNOS_CS47L93_SILICON_ID) ? "CS47L92/93 ID OK" :
          ((Id == 0x0000) || (Id == 0xFFFF)) ? "no answer (reset held, DCVDD off, or SCLK wrong)" : "ID MISMATCH"));
  return Id == EXYNOS_CS47L93_SILICON_ID;
}

STATIC
VOID
CodecProbe (VOID)
{
  UINTN    Cmu;
  UINT32   Gate;
  UINT32   SavedMode;
  UINT32   Gpp0Con;
  UINT32   Gpg1Con;
  UINT32   Gpg1Dat;
  UINT32   Saved[8];
  BOOLEAN  Ok;
  BOOLEAN  Released;
  volatile UINT32  *Mode;

  Cmu  = (UINTN)EXYNOS_CMU_PERIC0_BASE;
  Gate = MmioRead32 (Cmu + EXYNOS_CMU_PERIC0_USI01_PCLK);
  DEBUG ((DEBUG_WARN, "AudioProbe: CMU_PERIC0 USI01 mux %x div %x gate IPCLK %x PCLK %x QCH %x (SCLK = ipclk/4, rate UNVERIFIED)\n",
          MmioRead32 (Cmu + EXYNOS_CMU_PERIC0_USI01_MUX), MmioRead32 (Cmu + EXYNOS_CMU_PERIC0_USI01_DIV),
          MmioRead32 (Cmu + EXYNOS_CMU_PERIC0_USI01_IPCLK), Gate, MmioRead32 (Cmu + EXYNOS_CMU_PERIC0_USI01_QCH)));
  if (GateForcedOff (Gate)) {
    DEBUG ((DEBUG_ERROR, "AudioProbe: spi2 PCLK gate forced off by sboot - codec probe skipped (CMU is not ours to write)\n"));
    return;
  }

  Mode      = (volatile UINT32 *)((UINTN)EXYNOS_PERIC0_USI_SYSREG_BASE + EXYNOS_USI01_MODE_OFFSET);
  SavedMode = *Mode;
  Gpp0Con   = MmioRead32 (GPIO0 + EXYNOS_GPIO_GPP0_OFF);
  Gpg1Con   = MmioRead32 (GPIO0 + EXYNOS_GPIO_GPG1_OFF);
  Gpg1Dat   = MmioRead32 (GPIO0 + EXYNOS_GPIO_GPG1_OFF + EXYNOS_GPIO_BANK_DAT);
  Saved[0]  = MmioRead32 (SPI + SPI_CH_CFG);
  Saved[1]  = MmioRead32 (SPI + SPI_MODE_CFG);
  Saved[2]  = MmioRead32 (SPI + SPI_SLAVE_SEL);
  Saved[3]  = MmioRead32 (SPI + SPI_INT_EN);
  Saved[4]  = MmioRead32 (SPI + SPI_PACKET_CNT);
  Saved[5]  = MmioRead32 (SPI + SPI_SWAP_CFG);
  Saved[6]  = MmioRead32 (SPI + SPI_FB_CLK);
  Saved[7]  = MmioRead32 (SPI + SPI_USI_CON);
  DEBUG ((DEBUG_WARN, "AudioProbe: codec bus at handoff: usi_1 mode %x, gpp0 CON %x, gpg1 CON %x DAT %x"
                      " (reset gpg1-0 fn %x lvl %u), codec IRQ gpa2-2 lvl %u\n",
          SavedMode, Gpp0Con, Gpg1Con, Gpg1Dat,
          PinFunc (GPIO0, EXYNOS_GPIO_GPG1_OFF, EXYNOS_CODEC_RESET_PIN),
          PinLevel (GPIO0, EXYNOS_GPIO_GPG1_OFF, EXYNOS_CODEC_RESET_PIN),
          PinLevel ((UINTN)EXYNOS_ALIVE_GPIO_BASE, EXYNOS_ALIVE_GPA2_OFF, EXYNOS_CODEC_IRQ_PIN)));
  SpiDump ("at handoff");

  // Bus up: USI01 in SPI mode, gpp0-4..7 to the SPI function (CLK/MISO/MOSI + CS), controller init.
  *Mode = EXYNOS_USI_V2_MODE_SPI;
  {
    UINT32  Pin;

    for (Pin = EXYNOS_SPI2_FIRST_PIN; Pin < EXYNOS_SPI2_FIRST_PIN + EXYNOS_SPI2_PIN_COUNT; Pin++) {
      PinSetFunc (GPIO0, EXYNOS_GPIO_GPP0_OFF, Pin, EXYNOS_GPIO_FUNC_SPI2);
    }
  }

  SpiInit ();
  SpiDump ("after init");

  Released = FALSE;
  Ok       = CodecIdentify ();
  if (!Ok && BEYOND0LTE_AUDIO_PROBE_RELEASE_RESETS &&
      PinNotDrivenHigh (GPIO0, EXYNOS_GPIO_GPG1_OFF, EXYNOS_CODEC_RESET_PIN))
  {
    DEBUG ((DEBUG_WARN, "AudioProbe: codec in reset (gpg1-0 not driven high) - releasing it for the ID read\n"));
    PinDriveHigh (GPIO0, EXYNOS_GPIO_GPG1_OFF, EXYNOS_CODEC_RESET_PIN);
    Released = TRUE;
    gBS->Stall (2000);   // madera_disable_hard_reset: usleep_range(1000, 2000) after the release
    Ok = CodecIdentify ();
  }

  // Release: controller back to its handoff registers, USI reset/mode, pins and reset line as found.
  MmioAnd32 (SPI + SPI_CH_CFG, ~(SPI_CH_RXCH_ON | SPI_CH_TXCH_ON));
  MmioWrite32 (SPI + SPI_SLAVE_SEL, Saved[2]);
  MmioWrite32 (SPI + SPI_INT_EN, Saved[3]);
  MmioWrite32 (SPI + SPI_PACKET_CNT, Saved[4]);
  MmioWrite32 (SPI + SPI_MODE_CFG, Saved[1]);
  MmioWrite32 (SPI + SPI_SWAP_CFG, Saved[5]);
  MmioWrite32 (SPI + SPI_FB_CLK, Saved[6]);
  MmioWrite32 (SPI + SPI_CH_CFG, Saved[0]);
  MmioWrite32 (SPI + SPI_USI_CON, Saved[7]);
  *Mode = SavedMode;
  MmioWrite32 (GPIO0 + EXYNOS_GPIO_GPP0_OFF, Gpp0Con);
  MmioWrite32 (GPIO0 + EXYNOS_GPIO_GPG1_OFF + EXYNOS_GPIO_BANK_DAT, Gpg1Dat);
  MmioWrite32 (GPIO0 + EXYNOS_GPIO_GPG1_OFF, Gpg1Con);
  DEBUG ((DEBUG_WARN, "AudioProbe: codec %a; bus released: usi_1 mode %x gpp0 CON %x gpg1 CON %x DAT %x USI_CON %x%a\n",
          Ok ? "OK" : "no answer", *Mode, MmioRead32 (GPIO0 + EXYNOS_GPIO_GPP0_OFF), MmioRead32 (GPIO0 + EXYNOS_GPIO_GPG1_OFF),
          MmioRead32 (GPIO0 + EXYNOS_GPIO_GPG1_OFF + EXYNOS_GPIO_BANK_DAT), MmioRead32 (SPI + SPI_USI_CON),
          Released ? " (reset back to its handoff level)" : ""));
}

EFI_STATUS
EFIAPI
Beyond0LteAudioProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Saved;

  if (!BEYOND0LTE_AUDIO_PROBE) {
    DEBUG ((DEBUG_WARN, "AudioProbe: disabled (BEYOND0LTE_AUDIO_PROBE 0)\n"));
    return EFI_SUCCESS;
  }

  if (mProgress[0] == AUDIO_PROGRESS_TRYING) {
    DEBUG ((DEBUG_ERROR, "AudioProbe: previous boot hung in the audio probe (progress 0x69) - skipping it\n"));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_WARN, "AudioProbe: start (release resets %u)\n", BEYOND0LTE_AUDIO_PROBE_RELEASE_RESETS));
  Saved        = mProgress[1];
  mProgress[1] = AUDIO_PROGRESS_TRYING;
  AmpProbe ();
  CodecProbe ();
  mProgress[1] = Saved;
  DEBUG ((DEBUG_WARN, "AudioProbe: done\n"));
  return EFI_SUCCESS;
}
