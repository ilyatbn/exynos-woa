// FuelGauge.c — read-only MAX77705 fuel-gauge report (battery % PoC).
//
// Purpose: prove the gauge path end-to-end from UEFI before the Windows
// battery class extension exists (notes/battery-driver.md). The gauge is a
// Maxim ModelGauge m5-class part at 7-bit 0x36 on hsi2c_45; it is already
// running from the bootloader, so EVERYTHING here is a read. We never write
// a gauge register, never touch CONFIG/ALERT/MISCCFG, and never touch CMU.
//
// Conversions are the vendor's, verbatim from
// drivers/battery_v2/max77705_fuelgauge.c (winnerx mirror, cited in
// notes/battery-driver.md). Where the vendor's integer math truncates, we
// reproduce the truncation rather than "fixing" it — the point of the PoC is
// to match what Android reports, not to be more correct than it.

#include "UsbHostInternal.h"

// Two-phase word read like the vendor's max77705_bulk_read(i2c, reg, 2, ..):
// register pointer without STOP, then two bytes with restart+STOP.
STATIC
EFI_STATUS
Max77705FgReadWord (
  IN  UINT8   Reg,
  OUT UINT16  *Value
  )
{
  EFI_STATUS  Status;
  UINT8       Buf;
  UINT8       Data[2];

  Buf = Reg;
  Status = Hsi2cXfer (MAX77705_FG_ADDR, FALSE, &Buf, 1, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Hsi2cXfer (MAX77705_FG_ADDR, TRUE, Data, 2, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Vendor order: w_data = (data[1] << 8) | data[0].
  *Value = (UINT16)((Data[1] << 8) | Data[0]);
  return EFI_SUCCESS;
}

// SOC in 0.1% units — vendor max77705_fg_read_soc():
//   soc = ((hi * 100) + (lo * 100 / 256)) / 10, clamped to 1000.
// NOTE the vendor's odd scaling: hi is treated as whole percent (hi*100),
// lo as the fractional byte. Reproduced exactly.
STATIC
EFI_STATUS
FgReadSoc (
  OUT UINTN  *TenthsPercent
  )
{
  EFI_STATUS  Status;
  UINT16      w;

  Status = Max77705FgReadWord (MAX77705_FG_SOCREP, &w);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *TenthsPercent = (((w >> 8) * 100) + ((w & 0xFF) * 100 / 256)) / 10;
  if (*TenthsPercent > 1000) {
    *TenthsPercent = 1000;
  }

  return EFI_SUCCESS;
}

// Cell voltage — vendor max77705_fg_read_vcell() verbatim, including its
// integer truncation of the low 12 bits (they contribute 0 mV; the integer
// mV come from the high nibble at 1.25 V/step).
STATIC
EFI_STATUS
FgReadVcell (
  OUT UINTN  *Millivolts
  )
{
  EFI_STATUS  Status;
  UINT16      w;
  UINTN       Temp;

  Status = Max77705FgReadWord (MAX77705_FG_VCELL, &w);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Temp  = (w & 0xFFF) * 78125;
  *Millivolts = Temp / 1000000;

  Temp  = ((w & 0xF000) >> 4) * 78125;
  Temp /= 1000000;
  *Millivolts += Temp << 4;

  return EFI_SUCCESS;
}

// Average current, mA — vendor max77705_fg_read_avg_current(): signed 16-bit
// two's complement, magnitude = raw * 15625 * fg_resistor / 100000.
STATIC
EFI_STATUS
FgReadAvgCurrent (
  OUT INTN  *Milliamps
  )
{
  EFI_STATUS  Status;
  UINT16      w;
  UINTN       Temp;
  BOOLEAN     Negative;

  Status = Max77705FgReadWord (MAX77705_FG_AVG_CURRENT, &w);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Temp     = w;
  Negative = (Temp & BIT15) != 0;
  if (Negative) {
    Temp = (~Temp & 0xFFFF) + 1;
  }

  Temp = Temp * 15625 * MAX77705_FG_RESISTOR / 100000;
  *Milliamps = Negative ? -(INTN)Temp : (INTN)Temp;
  return EFI_SUCCESS;
}

// Temperature, degC*10 — vendor max77705_fg_read_temp(): hi byte sign bit 7,
// magnitude hi*1000 + lo*39/10 (0.039 degC LSB), result /10 -> degC*10.
STATIC
EFI_STATUS
FgReadTemp (
  OUT INTN  *TenthsDegC
  )
{
  EFI_STATUS  Status;
  UINT16      w;
  UINTN       Hi;
  UINTN       Lo;
  INTN       Temper;

  Status = Max77705FgReadWord (MAX77705_FG_TEMPERATURE, &w);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Hi = w >> 8;
  Lo = w & 0xFF;
  if ((Hi & 0x80) != 0) {
    Temper = (INTN)(((~Hi) & 0xFF) + 1);
    Temper *= -1000;
    Temper -= (INTN)((~((INTN)Lo)) + 1) * 39 / 10;
  } else {
    Temper = (INTN)(Hi & 0x7F) * 1000;
    Temper += (INTN)Lo * 39 / 10;
  }

  *TenthsDegC = Temper / 10;
  return EFI_SUCCESS;
}

EFI_STATUS
FuelGaugeReport (VOID)
{
  EFI_STATUS  Status;
  EFI_STATUS  FirstError;
  UINT8       Lo;
  UINTN       Soc;
  UINTN       Vcell;
  INTN        AvgMa;
  INTN        Temp;
  UINT16      RemCap;
  UINT16      FullCap;

  Status = Max77705RegRead (MAX77705_FG_ADDR, MAX77705_FG_STATUS, &Lo);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: gauge 0x%x not answering (STATUS read %r)\n",
            __func__, MAX77705_FG_ADDR, Status));
    return Status;
  }

  if ((Lo & MAX77705_FG_STATUS_ABSENT) != 0) {
    DEBUG ((DEBUG_WARN, "%a: gauge reports battery ABSENT (STATUS=%x)\n",
            __func__, Lo));
    return EFI_NO_MEDIA;
  }

  FirstError = EFI_SUCCESS;

  Status = FgReadSoc (&Soc);          if (EFI_ERROR (Status) && (FirstError == EFI_SUCCESS)) { FirstError = Status; }
  Status = FgReadVcell (&Vcell);      if (EFI_ERROR (Status) && (FirstError == EFI_SUCCESS)) { FirstError = Status; }
  Status = FgReadAvgCurrent (&AvgMa); if (EFI_ERROR (Status) && (FirstError == EFI_SUCCESS)) { FirstError = Status; }
  Status = FgReadTemp (&Temp);        if (EFI_ERROR (Status) && (FirstError == EFI_SUCCESS)) { FirstError = Status; }
  Status = Max77705FgReadWord (MAX77705_FG_REMCAP_REP, &RemCap);
  if (EFI_ERROR (Status) && (FirstError == EFI_SUCCESS)) { FirstError = Status; }
  Status = Max77705FgReadWord (MAX77705_FG_FULLCAP_REP, &FullCap);
  if (EFI_ERROR (Status) && (FirstError == EFI_SUCCESS)) { FirstError = Status; }

  if (FirstError != EFI_SUCCESS) {
    return FirstError;
  }

  // The headline number for M14: SOC %. Everything else is cross-check
  // material against Android's own /sys/class/power_supply readings.
  DEBUG ((DEBUG_INFO, "[FG] SOC %d.%d%%  VCELL %dmV  AVG_I %dmA  TEMP %d.%dC  "
          "REMCAP_REP %d  FULLCAP_REP %d (x fg_res/2 = %d/%d mAh)\n",
          Soc / 10, Soc % 10, Vcell, AvgMa,
          Temp / 10, (Temp < 0 ? -Temp : Temp) % 10,
          RemCap, FullCap,
          RemCap * MAX77705_FG_RESISTOR / 2,
          FullCap * MAX77705_FG_RESISTOR / 2));

  return EFI_SUCCESS;
}

// ChargerMode.c: the SOC on its own (0.1 % units, vendor scaling).
EFI_STATUS
FuelGaugeReadSoc (OUT UINTN *TenthsPercent)
{
  return FgReadSoc (TenthsPercent);
}
