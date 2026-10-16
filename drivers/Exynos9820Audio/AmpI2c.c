/*
 * AmpI2c.c - hsi2c_38 polled master + the two CS35L40 amps (@0x40 R/bottom speaker, @0x41 L/earpiece).
 *
 * Bus: a line-by-line port of the device-proven UEFI code in
 * firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteAudioProbeDxe/Beyond0LteAudioProbeDxe.c (I2cReset :157-175,
 * I2cXfer :177-283, AmpRead :285-306, the reset release of AmpProbe :395-424), itself the Pmic.c hsi2c_45 pattern.
 * Translations: MmioRead32/Write32 -> READ/WRITE_REGISTER_ULONG on the _CRS mappings, gBS->Stall ->
 * KeStallExecutionProcessor. Boot #92 (d8-audio-m1.md:582) read DEVID 35A40 REVID A0 OTPID 3 OTP_BOOT_DONE 1 on
 * both amps this way. Clocks: CMU_PERIC1 is never written (UEFI only reads it; it is not in our _CRS).
 *
 * Amp sequences = the device-validated UEFI code, main 24fb796 Beyond0LteAudioProbeDxe/AudioSpeaker.c (boot #99,
 * loudspeaker heard by the PC mic): AmpConfigure :274-339 (errata, OTP unpack :219-271 with AudioAmpOtp.h, bypass
 * trims, PLL from 6.144 MHz SCLK, ASP DSP-A 32-bit slot, gain 0x0A, muted), power-up :569-582 (with UAIF1 data
 * already flowing - no PUP_DONE otherwise), power-down :341-360. Rows keep the amp notes' row numbers.
 * Validated for @0x40 only; @0x41 (earpiece, slot 0) is configured the same way but only with Parameters\Stereo = 1.
 *
 * Every function runs at PASSIVE_LEVEL with Dev->BusLock held (taken by the public entry points).
 * NOT VALIDATED ON HARDWARE (stage Guided).
 */

#include "Exynos9820Audio.h"
#include "AmpOtpMap.h"   /* otp_map_2, copy of Beyond0LteAudioProbeDxe/AudioAmpOtp.h */

/* HSI2C register map (= AudioProbeHw.h, which cites i2c-exynos5.c and UsbHw.h). */
#define HSI2C_CTL                 0x00u
#define   HSI2C_CTL_RXCHON          (1u << 6)
#define   HSI2C_CTL_TXCHON          (1u << 7)
#define   HSI2C_CTL_MASTER          (1u << 3)
#define   HSI2C_CTL_SW_RST          (1u << 31)
#define HSI2C_FIFO_CTL            0x04u
#define   HSI2C_FIFO_CTL_RX_EN      (1u << 0)
#define   HSI2C_FIFO_CTL_TX_EN      (1u << 1)
#define   HSI2C_FIFO_CTL_RX_TRIG(x) (((ULONG)(x) & 0x7F) << 4)
#define   HSI2C_FIFO_CTL_TX_TRIG(x) (((ULONG)(x) & 0x7F) << 16)
#define HSI2C_TRAIL_CTL           0x08u
#define HSI2C_INT_EN              0x20u
#define HSI2C_INT_STAT            0x24u
#define   HSI2C_INT_TRANSFER_DONE   (1u << 7)
#define   HSI2C_INT_TRANSFER_ABORT  (1u << 8)
#define   HSI2C_INT_NO_ACK          (1u << 9)
#define   HSI2C_INT_NODEV           (1u << 10)
#define   HSI2C_INT_TIMEOUT         (1u << 11)
#define   HSI2C_INT_ERRORS          (HSI2C_INT_TRANSFER_ABORT | HSI2C_INT_NO_ACK | HSI2C_INT_NODEV | HSI2C_INT_TIMEOUT)
#define   HSI2C_INT_ALL             0x00000FFFu
#define HSI2C_ERROR_STATUS        0x2Cu
#define HSI2C_FIFO_STAT           0x30u
#define   HSI2C_FIFO_STAT_RX_LEVEL(x) (((x) >> 16) & 0x7F)
#define HSI2C_TX_DATA             0x34u
#define HSI2C_RX_DATA             0x38u
#define HSI2C_CONF                0x40u
#define   HSI2C_CONF_AUTO_MODE      (1u << 31)
#define HSI2C_AUTO_CONF           0x44u
#define   HSI2C_AUTO_CONF_READ_WRITE       (1u << 16)
#define   HSI2C_AUTO_CONF_STOP_AFTER_TRANS (1u << 17)
#define   HSI2C_AUTO_CONF_MASTER_RUN       (1u << 31)
#define HSI2C_TIMEOUT             0x48u
#define HSI2C_TRANS_STATUS        0x50u
#define   HSI2C_MASTER_BUSY         (1u << 17)
#define HSI2C_TIMING_FS1          0x60u
#define HSI2C_TIMING_FS2          0x64u
#define HSI2C_TIMING_FS3          0x68u
#define HSI2C_ADDR                0x70u
#define HSI2C_USI_CON             0xC4u
#define HSI2C_TIMING_FS1_400K     0x00F00000u   /* AudioProbeHw.h: 400 kHz from the 200 MHz default-clk */
#define HSI2C_TIMING_FS2_400K     0x000000E0u
#define HSI2C_TIMING_FS3_400K     0x00210000u
#define HSI2C_TIMEOUT_US          50000u        /* Beyond0LteAudioProbeDxe.c:70 */
#define HSI2C_MAX_XFER            16u           /* the UEFI port's FIFO bound */

/* CS35L40 registers (cs35l41 registers.h via notes/d8-audio-m3-amp.md; DEVID/REVID/OTPID/IRQ1_STATUS4 device-read). */
#define CS35L40_DEVID             0x00000000u
#define CS35L40_REVID             0x00000004u
#define CS35L40_OTPID             0x00000010u
#define CS35L40_TEST_KEY_CTL      0x00000040u
#define CS35L40_PWR_CTRL1         0x00002014u   /* bit0 GLOBAL_EN */
#define CS35L40_AMP_OUT_MUTE      0x00002024u   /* bit4 mute */
#define CS35L40_AMP_DIG_VOL_CTRL  0x00006000u   /* [13:3] signed 11-bit, 0.125 dB/step, 0x4CF = mute */
#define CS35L40_IRQ1_STATUS1      0x00010010u   /* bit24 PUP_DONE, bit23 PDN_DONE (W1C) */
#define CS35L40_IRQ1_STATUS3      0x00010018u   /* bit31 OTP_BOOT_ERR, bit1 PLL_LOCK */
#define CS35L40_IRQ1_STATUS4      0x0001001Cu   /* bit1 OTP_BOOT_DONE */
#define CS35L40_OTP_BOOT_DONE     (1u << 1)
#define CS35L40_OTP_BOOT_ERR      (1u << 31)
#define CS35L40_PUP_DONE          (1u << 24)
#define CS35L40_PDN_DONE          (1u << 23)
#define CS35L40_STATUS1_ERRORS    0x800281C0u   /* b31 AMP_SHORT, b17 TEMP_ERR, b15 TEMP_WARN, b8 BST_SHORT, b7 BST_DCM_UVP, b6 BST_OVP (amp notes 6.9) */
#define CS35L40_VOL_MASK          0x00003FF8u
#define CS35L40_VOL_SHIFT         3
#define CS35L40_VOL_MUTE_FIELD    0x4CFu        /* -817 steps = mute (amp notes F8) */
#define CS35L40_VOL_MIN_STEPS     (-816)        /* lowest non-mute step */
/* Fixed digital volume while Windows applies endpoint volume in software (HwVolume = 0): 0 dB (2026-10-16, user: "pretty
   low for 100%"; was -10 dB, the boot #99 tone level AudioSpeaker.c :85, :582). 0 dB is the top of the playback range the
   amp notes allow (notes/d8-audio-m3-amp.md Safety: -102 .. 0 dB, never positive, analog gain <= 0x0A); the earpiece
   6 dB lower. */
#define AMP_SPK_FIXED_STEPS       (0)
#define AMP_RCV_TRIM_STEPS        (-48)

/* ---------------------------------------------------------------------------------------------------------------- */

static __forceinline ULONG I2cRd(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Dev->Va[AUD_RES_HSI2C] + Off));
}

static __forceinline VOID I2cWr(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Dev->Va[AUD_RES_HSI2C] + Off), Val);
}

static __forceinline volatile ULONG *Gpio(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ ULONG Off)
{
    return (volatile ULONG *)(Dev->Va[AUD_RES_PERIC1_GPIO] + Off);
}

static VOID PinSetFunc(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ ULONG Bank, _In_ ULONG Pin, _In_ ULONG Func)
{
    ULONG Con = READ_REGISTER_ULONG(Gpio(Dev, Bank));

    WRITE_REGISTER_ULONG(Gpio(Dev, Bank), (Con & ~(0xFu << (Pin * 4))) | (Func << (Pin * 4)));
}

static ULONG PinFunc(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ ULONG Bank, _In_ ULONG Pin)
{
    return (READ_REGISTER_ULONG(Gpio(Dev, Bank)) >> (Pin * 4)) & 0xF;
}

static ULONG PinLevel(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ ULONG Bank, _In_ ULONG Pin)
{
    return (READ_REGISTER_ULONG(Gpio(Dev, Bank + AUD_GPIO_BANK_DAT)) >> Pin) & 1;
}

/* DAT first, then CON = output, so the pin never glitches (Beyond0LteAudioProbeDxe.c PinDriveHigh/Low). */
static VOID PinDrive(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ ULONG Bank, _In_ ULONG Pin, _In_ BOOLEAN High)
{
    ULONG Dat = READ_REGISTER_ULONG(Gpio(Dev, Bank + AUD_GPIO_BANK_DAT));

    Dat = High ? (Dat | (1u << Pin)) : (Dat & ~(1u << Pin));
    WRITE_REGISTER_ULONG(Gpio(Dev, Bank + AUD_GPIO_BANK_DAT), Dat);
    PinSetFunc(Dev, Bank, Pin, AUD_GPIO_FUNC_OUTPUT);
}

static VOID I2cReset(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    I2cWr(Dev, HSI2C_CTL, I2cRd(Dev, HSI2C_CTL) | HSI2C_CTL_SW_RST);
    I2cWr(Dev, HSI2C_CTL, I2cRd(Dev, HSI2C_CTL) & ~HSI2C_CTL_SW_RST);
    I2cWr(Dev, HSI2C_USI_CON, 0);
    I2cWr(Dev, HSI2C_TIMING_FS1, HSI2C_TIMING_FS1_400K);
    I2cWr(Dev, HSI2C_TIMING_FS2, HSI2C_TIMING_FS2_400K);
    I2cWr(Dev, HSI2C_TIMING_FS3, HSI2C_TIMING_FS3_400K);
    I2cWr(Dev, HSI2C_CTL, HSI2C_CTL_MASTER);
    I2cWr(Dev, HSI2C_TRAIL_CTL, 0xFFFFFF);
    I2cWr(Dev, HSI2C_TIMEOUT, 0);
    I2cWr(Dev, HSI2C_CONF, HSI2C_CONF_AUTO_MODE);
    I2cWr(Dev, HSI2C_FIFO_CTL, 0);
    I2cWr(Dev, HSI2C_AUTO_CONF, 0);
    I2cWr(Dev, HSI2C_ADDR, 0);
    I2cWr(Dev, HSI2C_INT_STAT, HSI2C_INT_ALL);
}

static NTSTATUS I2cXfer(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ UCHAR Slave, _In_ BOOLEAN Read,
                        _Inout_updates_(Length) PUCHAR Data, _In_ ULONG Length, _In_ BOOLEAN Stop)
{
    ULONG Ctl;
    ULONG AutoConf;
    ULONG Addr;
    ULONG IntStat;
    ULONG Index;
    ULONG Us;

    if (!Dev->BusReady) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (Length == 0 || Length > HSI2C_MAX_XFER) {
        return STATUS_INVALID_PARAMETER;
    }

    I2cWr(Dev, HSI2C_TIMEOUT, I2cRd(Dev, HSI2C_TIMEOUT) & ~(1u << 31));
    I2cWr(Dev, HSI2C_FIFO_CTL, HSI2C_FIFO_CTL_RX_EN | HSI2C_FIFO_CTL_TX_EN | HSI2C_FIFO_CTL_RX_TRIG(Length) |
                               HSI2C_FIFO_CTL_TX_TRIG(Length));

    Ctl  = I2cRd(Dev, HSI2C_CTL) | HSI2C_CTL_MASTER;
    Ctl &= ~(HSI2C_CTL_RXCHON | HSI2C_CTL_TXCHON);
    Ctl |= Read ? HSI2C_CTL_RXCHON : HSI2C_CTL_TXCHON;
    I2cWr(Dev, HSI2C_CTL, Ctl);

    AutoConf  = I2cRd(Dev, HSI2C_AUTO_CONF) & ~(0xFFFFu | HSI2C_AUTO_CONF_READ_WRITE | HSI2C_AUTO_CONF_STOP_AFTER_TRANS);
    AutoConf |= Length;
    if (Read) {
        AutoConf |= HSI2C_AUTO_CONF_READ_WRITE;
    }
    if (Stop) {
        AutoConf |= HSI2C_AUTO_CONF_STOP_AFTER_TRANS;
    }

    Addr  = I2cRd(Dev, HSI2C_ADDR) & ~((0x3FFu << 10) | 0x3FFu | (0xFFu << 24));
    Addr |= ((ULONG)(Slave & 0x7F) << 10) | (0x7u << 24);   /* i2c-exynos5.c:755-763, FS master id 0x7 */
    I2cWr(Dev, HSI2C_ADDR, Addr);

    for (Us = 0; (I2cRd(Dev, HSI2C_TRANS_STATUS) & HSI2C_MASTER_BUSY) != 0; Us += 100) {
        if (Us >= HSI2C_TIMEOUT_US) {
            AudLog(Dev->Log, "hsi2c38 master busy before @%x (TRANS_STATUS %08lX) - reset\r\n", Slave,
                   I2cRd(Dev, HSI2C_TRANS_STATUS));
            I2cReset(Dev);
            return STATUS_DEVICE_BUSY;
        }
        KeStallExecutionProcessor(100);
    }

    I2cWr(Dev, HSI2C_INT_STAT, HSI2C_INT_ALL);
    I2cWr(Dev, HSI2C_INT_EN, HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS);
    I2cWr(Dev, HSI2C_AUTO_CONF, AutoConf | HSI2C_AUTO_CONF_MASTER_RUN);

    if (!Read) {
        for (Index = 0; Index < Length; Index++) {
            WRITE_REGISTER_UCHAR((volatile UCHAR *)(Dev->Va[AUD_RES_HSI2C] + HSI2C_TX_DATA), Data[Index]);
        }
    }

    IntStat = 0;
    for (Us = 0; Us < HSI2C_TIMEOUT_US; Us += 50) {
        IntStat = I2cRd(Dev, HSI2C_INT_STAT);
        if ((IntStat & (HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS)) != 0) {
            break;
        }
        KeStallExecutionProcessor(50);
    }

    if ((IntStat & (HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS)) == 0) {
        AudLog(Dev->Log, "hsi2c38 @%x xfer timeout (INT_STAT %08lX TRANS_STATUS %08lX) - reset\r\n", Slave, IntStat,
               I2cRd(Dev, HSI2C_TRANS_STATUS));
        I2cReset(Dev);
        return STATUS_IO_TIMEOUT;
    }

    if ((IntStat & HSI2C_INT_ERRORS) != 0) {
        AudLog(Dev->Log, "hsi2c38 @%x %s: INT_STAT %08lX ERR %08lX - reset\r\n", Slave, Read ? "read" : "write", IntStat,
               I2cRd(Dev, HSI2C_ERROR_STATUS));
        I2cReset(Dev);
        return ((IntStat & (HSI2C_INT_NODEV | HSI2C_INT_NO_ACK)) != 0) ? STATUS_NO_SUCH_DEVICE : STATUS_DEVICE_DATA_ERROR;
    }

    I2cWr(Dev, HSI2C_INT_STAT, IntStat);

    if (Read) {
        for (Index = 0; Index < Length; Index++) {
            for (Us = 0; HSI2C_FIFO_STAT_RX_LEVEL(I2cRd(Dev, HSI2C_FIFO_STAT)) == 0; Us += 100) {
                if (Us >= HSI2C_TIMEOUT_US) {
                    AudLog(Dev->Log, "hsi2c38 @%x rx fifo empty - reset\r\n", Slave);
                    I2cReset(Dev);
                    return STATUS_IO_TIMEOUT;
                }
                KeStallExecutionProcessor(100);
            }
            Data[Index] = READ_REGISTER_UCHAR((volatile UCHAR *)(Dev->Va[AUD_RES_HSI2C] + HSI2C_RX_DATA));
        }
    }

    return STATUS_SUCCESS;
}

/* CS35L40 regmap: 32-bit big-endian address, 32-bit big-endian value (cs35l41-i2c.c:25-30). */
static NTSTATUS AmpRead(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ UCHAR Slave, _In_ ULONG Reg, _Out_ PULONG Value)
{
    NTSTATUS Status;
    UCHAR    Buf[4];

    Buf[0] = (UCHAR)(Reg >> 24);
    Buf[1] = (UCHAR)(Reg >> 16);
    Buf[2] = (UCHAR)(Reg >> 8);
    Buf[3] = (UCHAR)Reg;
    Status = I2cXfer(Dev, Slave, FALSE, Buf, 4, FALSE);
    if (NT_SUCCESS(Status)) {
        Buf[0] = Buf[1] = Buf[2] = Buf[3] = 0;
        Status = I2cXfer(Dev, Slave, TRUE, Buf, 4, TRUE);
    }

    *Value = NT_SUCCESS(Status) ? (((ULONG)Buf[0] << 24) | ((ULONG)Buf[1] << 16) | ((ULONG)Buf[2] << 8) | Buf[3]) : 0;
    return Status;
}

static NTSTATUS AmpWrite(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ UCHAR Slave, _In_ ULONG Reg, _In_ ULONG Value)
{
    UCHAR Buf[8];

    Buf[0] = (UCHAR)(Reg >> 24);
    Buf[1] = (UCHAR)(Reg >> 16);
    Buf[2] = (UCHAR)(Reg >> 8);
    Buf[3] = (UCHAR)Reg;
    Buf[4] = (UCHAR)(Value >> 24);
    Buf[5] = (UCHAR)(Value >> 16);
    Buf[6] = (UCHAR)(Value >> 8);
    Buf[7] = (UCHAR)Value;
    return I2cXfer(Dev, Slave, FALSE, Buf, 8, TRUE);
}

static NTSTATUS AmpUpdate(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ UCHAR Slave, _In_ ULONG Reg, _In_ ULONG Mask,
                          _In_ ULONG Value)
{
    NTSTATUS Status;
    ULONG    Old;

    Status = AmpRead(Dev, Slave, Reg, &Old);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    return AmpWrite(Dev, Slave, Reg, (Old & ~Mask) | (Value & Mask));
}

/* ---------------------------------------------------------------------------------------------------------------- *
 * Table-driven sequences (rows = notes/d8-audio-m3-amp.md "Proposed sequence"; ALL UNVERIFIED on the device).
 * ---------------------------------------------------------------------------------------------------------------- */

typedef enum _AMP_OP {
    AMP_OP_END = 0,
    AMP_OP_WRITE,       /* Reg = Value */
    AMP_OP_RMW,         /* Reg = (old & ~Mask) | (Value & Mask) */
    AMP_OP_POLL,        /* until (Reg & Mask) == Value, 1 ms steps, Arg ms; then W1C Mask if Value != 0 */
    AMP_OP_DELAY_US,    /* stall Arg us */
    AMP_OP_SLOT,        /* SP_FRAME_RX_SLOT 0x4820: per-amp value (R/bot plays slot 1, L/rcv slot 0; amp notes F6) */
} AMP_OP;

typedef struct _AMP_STEP {
    AMP_OP Op;
    ULONG  Reg;
    ULONG  Mask;
    ULONG  Value;
    ULONG  Arg;
    PCSTR  Row;
} AMP_STEP;

/* Rows 1.x (A0 errata, K:2040-2052) = AudioSpeaker.c :289-297. */
static const AMP_STEP AmpErrataSeq[] = {
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x00005555, 0, "1.1" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x0000AAAA, 0, "1.1" },
    { AMP_OP_WRITE, 0x00003854, 0, 0x05180240, 0, "1.2" },
    { AMP_OP_WRITE, 0x00007418, 0, 0x9091A1C8, 0, "1.3" },
    { AMP_OP_WRITE, 0x00003014, 0, 0x0200EE0E, 0, "1.4" },
    { AMP_OP_WRITE, 0x0000381C, 0, 0x00000051, 0, "1.5" },
    { AMP_OP_WRITE, 0x00000054, 0, 0x00000004, 0, "1.6" },
    { AMP_OP_WRITE, 0x00010318, 0, 0x00000000, 0, "1.7" },
    { AMP_OP_WRITE, 0x00010B18, 0, 0x00000000, 0, "1.7" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x0000CCCC, 0, "1.8" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x00003333, 0, "1.8" },
    { AMP_OP_END,   0, 0, 0, 0, NULL },
};

/* Rows 3.x-5.x = AudioSpeaker.c :305-333 (cap trim clear, IRQ mask, boost peak, hi-z, noise gate, PLL from 6.144 MHz
   SCLK code 0x28, 48 kHz, ASP DSP-A 32-bit slot, per-amp RX slot, DAC <- ASPRX1, gain 0x0A, muted). */
static const AMP_STEP AmpConfigSeq[] = {
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x00005555, 0, "3.1" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x0000AAAA, 0, "3.1" },
    { AMP_OP_RMW,   0x00007418, 0x00000F80, 0, 0, "3.2" },
    { AMP_OP_RMW,   0x0000741C, 0x0000001F, 0, 0, "3.3" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x0000CCCC, 0, "3.4" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x00003333, 0, "3.4" },
    { AMP_OP_WRITE, 0x00010110, 0, 0x7FFCFE3F, 0, "3.5" },
    { AMP_OP_RMW,   0x00003808, 0x0000007F, 0x00000042, 0, "3.6" },
    { AMP_OP_RMW,   0x0000480C, 0x00000003, 0x00000003, 0, "3.7" },
    { AMP_OP_WRITE, 0x00006808, 0, 0x00003F26, 0, "3.8" },
    { AMP_OP_WRITE, 0x00012004, 0, 0x00010206, 0, "3.9" },
    { AMP_OP_WRITE, 0x00012008, 0, 0x00010206, 0, "3.9" },
    { AMP_OP_RMW,   0x00004804, 0x0000003F, 0x00000028, 0, "4.1" },
    { AMP_OP_WRITE, 0x00002C04, 0, 0x00000810, 0, "4.2" },
    { AMP_OP_WRITE, 0x00002C04, 0, 0x00000D10, 0, "4.2" },
    { AMP_OP_WRITE, 0x00002C04, 0, 0x00000D00, 0, "4.2" },
    { AMP_OP_WRITE, 0x00002C04, 0, 0x00000D00, 0, "4.2" },
    { AMP_OP_WRITE, 0x00002C04, 0, 0x00000500, 0, "4.2" },
    { AMP_OP_WRITE, 0x00002C04, 0, 0x00000510, 0, "4.2" },
    { AMP_OP_RMW,   0x00002C0C, 0x0000001F, 0x00000003, 0, "4.3" },
    { AMP_OP_WRITE, 0x00004808, 0, 0x20200040, 0, "5.1" },
    { AMP_OP_SLOT,  0x00004820, 0, 0, 0, "5.2" },
    { AMP_OP_WRITE, 0x00004840, 0, 0x00000018, 0, "5.3" },
    { AMP_OP_WRITE, 0x00004C00, 0, 0x00000008, 0, "5.4" },
    { AMP_OP_RMW,   0x00006C04, 0x000003E0, 0x0A << 5, 0, "5.5" },
    { AMP_OP_RMW,   CS35L40_AMP_DIG_VOL_CTRL, CS35L40_VOL_MASK, CS35L40_VOL_MUTE_FIELD << CS35L40_VOL_SHIFT, 0, "5.6" },
    { AMP_OP_WRITE, CS35L40_AMP_OUT_MUTE, 0, 0x00000010, 0, "5.7" },
    { AMP_OP_END,   0, 0, 0, 0, NULL },
};

/* Rows 6.2-6.8 = AudioSpeaker.c :569-579 (UAIF1 data must already flow: AboxStart comes first). The PLL_LOCK bit is
   never seen set (boot #99), so it is only acknowledged, not polled. Row 6.9 is checked in code. The pup patch is
   wrapped in the 0x5555/0xAAAA test key, as the validated code does (the amp notes' 0x55/0xAA was not run). */
static const AMP_STEP AmpPowerUpSeq[] = {
    { AMP_OP_WRITE, CS35L40_IRQ1_STATUS3, 0, 0x00000002, 0, "6.2" },
    { AMP_OP_WRITE, 0x00004800, 0, 0x00010000, 0, "6.3" },
    { AMP_OP_RMW,   0x0000201C, 0x00000010, 0x00000010, 0, "6.4" },
    { AMP_OP_WRITE, 0x00002018, 0, 0x00000021, 0, "6.5" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x00005555, 0, "6.6" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x0000AAAA, 0, "6.6" },
    { AMP_OP_WRITE, 0x00002084, 0, 0x002F1AA0, 0, "6.6" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x0000CCCC, 0, "6.6" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x00003333, 0, "6.6" },
    { AMP_OP_RMW,   CS35L40_PWR_CTRL1, 0x00000001, 0x00000001, 0, "6.7" },
    { AMP_OP_POLL,  CS35L40_IRQ1_STATUS1, CS35L40_PUP_DONE, CS35L40_PUP_DONE, 100, "6.8" },
    { AMP_OP_END,   0, 0, 0, 0, NULL },
};

/* Rows 8.3-8.6 = AudioSpeaker.c :351-358 (mute 8.1/8.2 = AmpMuteLocked first). Run to the end even if a row fails. */
static const AMP_STEP AmpPowerDownSeq[] = {
    { AMP_OP_RMW,   CS35L40_PWR_CTRL1, 0x00000001, 0x00000000, 0, "8.3" },
    { AMP_OP_POLL,  CS35L40_IRQ1_STATUS1, CS35L40_PDN_DONE, CS35L40_PDN_DONE, 100, "8.4" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x00005555, 0, "8.5" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x0000AAAA, 0, "8.5" },
    { AMP_OP_WRITE, 0x00002084, 0, 0x002F1AA3, 0, "8.5" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x0000CCCC, 0, "8.5" },
    { AMP_OP_WRITE, CS35L40_TEST_KEY_CTL, 0, 0x00003333, 0, "8.5" },
    { AMP_OP_WRITE, 0x00004800, 0, 0x00000000, 0, "8.6" },
    { AMP_OP_WRITE, 0x00002018, 0, 0x00000000, 0, "8.6" },
    { AMP_OP_END,   0, 0, 0, 0, NULL },
};

/* StopOnError FALSE (teardown) runs every row and returns the first failure. */
static NTSTATUS AmpRunSeq(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ PAUD_AMP Amp, _In_ const AMP_STEP *Seq, _In_ PCSTR What,
                          _In_ BOOLEAN StopOnError)
{
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS First  = STATUS_SUCCESS;
    NTSTATUS W1c;
    ULONG    I;
    ULONG    V = 0;
    ULONG    Ms;

    for (I = 0; Seq[I].Op != AMP_OP_END; I++) {
        const AMP_STEP *S = &Seq[I];

        switch (S->Op) {
        case AMP_OP_WRITE:
            Status = AmpWrite(Dev, Amp->Addr, S->Reg, S->Value);
            break;
        case AMP_OP_RMW:
            Status = AmpUpdate(Dev, Amp->Addr, S->Reg, S->Mask, S->Value);
            break;
        case AMP_OP_SLOT:
            Status = AmpWrite(Dev, Amp->Addr, S->Reg, (Amp->Addr == AUD_AMP_R_I2C_ADDR) ? 0x00000001u :
                              (Dev->Cfg.RcvSlotReg != 0xFFFFFFFFu) ? Dev->Cfg.RcvSlotReg : 0x00000100u);
            break;
        case AMP_OP_POLL:
            for (Ms = 0;; Ms++) {
                Status = AmpRead(Dev, Amp->Addr, S->Reg, &V);
                if (!NT_SUCCESS(Status) || (V & S->Mask) == S->Value) {
                    break;
                }
                if (Ms >= S->Arg) {
                    Status = STATUS_IO_TIMEOUT;
                    break;
                }
                KeStallExecutionProcessor(1000);
            }
            if (S->Value != 0) {
                W1c = AmpWrite(Dev, Amp->Addr, S->Reg, S->Mask);   /* W1C, also after a timeout (AudioSpeaker.c :353, :579) */
                if (NT_SUCCESS(Status)) {
                    Status = W1c;
                }
            }
            AudLog(Dev->Log, "amp %s %s row %s: [%05lX] = %08lX after %lu ms, %08lX\r\n", Amp->Name, What, S->Row,
                   S->Reg, V, Ms, Status);
            break;
        case AMP_OP_DELAY_US:
            KeStallExecutionProcessor(S->Arg);
            break;
        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (!NT_SUCCESS(Status)) {
            AudLog(Dev->Log, "amp %s %s: row %s [%05lX] failed %08lX\r\n", Amp->Name, What, S->Row, S->Reg, Status);
            if (StopOnError) {
                return Status;
            }
            if (NT_SUCCESS(First)) {
                First = Status;
            }
            Status = STATUS_SUCCESS;
        }
    }

    AudLog(Dev->Log, "amp %s %s: %lu rows, %08lX\r\n", Amp->Name, What, I, First);
    return First;
}

/*
 * Rows 2.1-2.5: OTP trim unpack = AudioSpeaker.c AmpOtpUnpack :219-271 (vendor cs35l41_otp_unpack, cs35l41.c:544-632),
 * same header check, same bit walk over otp_map_2 (AmpOtpMap.h). Without it the boost current sense and references
 * run untrimmed, so AmpPowerUp refuses GLOBAL_EN unless this succeeded (Amp->OtpDone).
 */
#define CS35L40_OTP_MEM0  0x00000400u

static NTSTATUS AmpOtpUnpack(_Inout_ PAUD_DEVICE_CONTEXT Dev, _Inout_ PAUD_AMP Amp)
{
    ULONG    Mem[32];
    ULONG    OtpId = 0;
    ULONG    Val;
    ULONG    BitOffset;
    ULONG    ArrayOffset;
    ULONG    Size;
    ULONG    I;
    NTSTATUS Status;
    NTSTATUS First;

    First = AmpWrite(Dev, Amp->Addr, CS35L40_TEST_KEY_CTL, 0x00005555);
    Status = AmpWrite(Dev, Amp->Addr, CS35L40_TEST_KEY_CTL, 0x0000AAAA);
    First = NT_SUCCESS(First) ? Status : First;
    Status = AmpRead(Dev, Amp->Addr, CS35L40_OTPID, &OtpId);
    First = NT_SUCCESS(First) ? Status : First;
    for (I = 0; I < 32; I++) {
        Status = AmpRead(Dev, Amp->Addr, CS35L40_OTP_MEM0 + I * 4, &Mem[I]);
        First = NT_SUCCESS(First) ? Status : First;
    }

    if (!NT_SUCCESS(First) || (Mem[1] & 0xFFFFFFFEu) != 0x1500u || (Mem[2] & 0xFFFFu) != 0x0300u ||
        (OtpId != 2 && OtpId != 3)) {
        AudLog(Dev->Log, "amp %s: OTP header bad (I2C %08lX, OTPID %lX MEM1 %lX MEM2 %lX) - no unpack\r\n", Amp->Name,
               First, OtpId, Mem[1], Mem[2]);
        (VOID)AmpWrite(Dev, Amp->Addr, CS35L40_TEST_KEY_CTL, 0x0000CCCC);
        (VOID)AmpWrite(Dev, Amp->Addr, CS35L40_TEST_KEY_CTL, 0x00003333);
        return NT_SUCCESS(First) ? STATUS_DEVICE_DATA_ERROR : First;
    }

    BitOffset   = 16;
    ArrayOffset = 2;
    for (I = 0; I < ARRAYSIZE(mAmpOtpMap2); I++) {
        Size = mAmpOtpMap2[I].Size;
        if (BitOffset + Size - 1 >= 32) {
            Val  = (ULONG)(((ULONGLONG)Mem[ArrayOffset] & (0xFFFFFFFFull << BitOffset) & 0xFFFFFFFFull) >> BitOffset);
            ArrayOffset++;
            Val |= (Mem[ArrayOffset] & (ULONG)((1ull << (BitOffset + Size - 32)) - 1)) << (32 - BitOffset);
            BitOffset += Size - 32;
        } else {
            Val        = (ULONG)(((ULONGLONG)Mem[ArrayOffset] >> BitOffset) & ((1ull << Size) - 1));
            BitOffset += Size;
        }

        if (BitOffset == 32) {
            BitOffset = 0;
            ArrayOffset++;
        }

        if (mAmpOtpMap2[I].Reg != 0) {
            Status = AmpUpdate(Dev, Amp->Addr, mAmpOtpMap2[I].Reg, (ULONG)(((1ull << Size) - 1) << mAmpOtpMap2[I].Shift),
                               Val << mAmpOtpMap2[I].Shift);
            First = NT_SUCCESS(First) ? Status : First;
        }
    }

    Status = AmpWrite(Dev, Amp->Addr, CS35L40_TEST_KEY_CTL, 0x0000CCCC);
    First = NT_SUCCESS(First) ? Status : First;
    Status = AmpWrite(Dev, Amp->Addr, CS35L40_TEST_KEY_CTL, 0x00003333);
    First = NT_SUCCESS(First) ? Status : First;
    AudLog(Dev->Log, "amp %s: OTP unpacked (OTPID %lX, %u fields, ended at word %lu bit %lu): %08lX\r\n", Amp->Name, OtpId,
           (ULONG)ARRAYSIZE(mAmpOtpMap2), ArrayOffset, BitOffset, First);
    return First;
}

/* ---------------------------------------------------------------------------------------------------------------- */

static BOOLEAN AmpIdentify(_Inout_ PAUD_DEVICE_CONTEXT Dev, _Inout_ PAUD_AMP Amp)
{
    NTSTATUS Status;
    ULONG    DevId = 0;
    ULONG    Irq4  = 0;
    ULONG    Irq3  = 0;
    ULONG    Expected;

    Status = AmpRead(Dev, Amp->Addr, CS35L40_DEVID, &DevId);
    if (!NT_SUCCESS(Status)) {
        AudLog(Dev->Log, "amp %s @%x DEVID read %08lX\r\n", Amp->Name, Amp->Addr, Status);
        return FALSE;
    }

    (VOID)AmpRead(Dev, Amp->Addr, CS35L40_REVID, &Amp->RevId);
    (VOID)AmpRead(Dev, Amp->Addr, CS35L40_OTPID, &Amp->OtpId);
    (VOID)AmpRead(Dev, Amp->Addr, CS35L40_IRQ1_STATUS4, &Irq4);
    (VOID)AmpRead(Dev, Amp->Addr, CS35L40_IRQ1_STATUS3, &Irq3);
    Expected = ((Amp->RevId & 0x0F) % 2) ? AUD_CS35L40R_CHIP_ID : AUD_CS35L40_CHIP_ID;
    AudLog(Dev->Log, "amp %s @%x DEVID %lX REVID %lX OTPID %lX IRQ1_STATUS4 %lX (OTP_BOOT_DONE %u) STATUS3 %lX "
           "(OTP_BOOT_ERR %u) -> %s\r\n", Amp->Name, Amp->Addr, DevId, Amp->RevId, Amp->OtpId, Irq4,
           (Irq4 & CS35L40_OTP_BOOT_DONE) ? 1 : 0, Irq3, (Irq3 & CS35L40_OTP_BOOT_ERR) ? 1 : 0,
           (DevId == Expected) ? "CS35L40 ID OK" : "ID MISMATCH");
    return DevId == Expected;
}

/* OTP_BOOT_DONE poll, 1 ms x 100 (K:2187-2197); OTP_BOOT_ERR must be 0 (UC:1262-1263). */
static BOOLEAN AmpBootDone(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ PAUD_AMP Amp)
{
    ULONG V = 0;
    ULONG Ms;

    for (Ms = 0; Ms < 100; Ms++) {
        if (NT_SUCCESS(AmpRead(Dev, Amp->Addr, CS35L40_IRQ1_STATUS4, &V)) && (V & CS35L40_OTP_BOOT_DONE) != 0) {
            break;
        }
        KeStallExecutionProcessor(1000);
    }

    if ((V & CS35L40_OTP_BOOT_DONE) == 0) {
        AudLog(Dev->Log, "amp %s: OTP_BOOT_DONE not set after 100 ms (STATUS4 %lX)\r\n", Amp->Name, V);
        return FALSE;
    }

    V = 0;
    (VOID)AmpRead(Dev, Amp->Addr, CS35L40_IRQ1_STATUS3, &V);
    if ((V & CS35L40_OTP_BOOT_ERR) != 0) {
        AudLog(Dev->Log, "amp %s: OTP_BOOT_ERR set (STATUS3 %lX) - amp left idle\r\n", Amp->Name, V);
        return FALSE;
    }

    return TRUE;
}

/*
 * Bus up + reset release + identification: the boot #92 UEFI steps. Difference from UEFI: the resets stay
 * released afterwards (the amps idle with GLOBAL_EN = 0, as under Android), and the bus stays configured until
 * AmpRelease (ReleaseHardware) puts gph0 CON and USI_CON back.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmpInit(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    ULONG   I;
    BOOLEAN Released = FALSE;

    if (!Dev->Mapped || Dev->Va[AUD_RES_HSI2C] == NULL || Dev->Va[AUD_RES_PERIC1_GPIO] == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    WdfWaitLockAcquire(Dev->BusLock, NULL);

    if (!Dev->BusSaved) {
        Dev->SavedGph0Con = READ_REGISTER_ULONG(Gpio(Dev, AUD_GPIO_GPH0_OFF));
        Dev->SavedUsiCon  = I2cRd(Dev, HSI2C_USI_CON);
        Dev->BusSaved     = TRUE;
    }

    AudLog(Dev->Log, "amp bus at start: gph0 CON %08lX, gph1 CON %08lX DAT %08lX, hsi2c38 CTL %08lX USI_CON %08lX\r\n",
           Dev->SavedGph0Con, READ_REGISTER_ULONG(Gpio(Dev, AUD_GPIO_GPH1_OFF)),
           READ_REGISTER_ULONG(Gpio(Dev, AUD_GPIO_GPH1_OFF + AUD_GPIO_BANK_DAT)), I2cRd(Dev, HSI2C_CTL), Dev->SavedUsiCon);

    PinSetFunc(Dev, AUD_GPIO_GPH0_OFF, AUD_HSI2C38_SCL_PIN, AUD_GPIO_FUNC_HSI2C38);
    PinSetFunc(Dev, AUD_GPIO_GPH0_OFF, AUD_HSI2C38_SDA_PIN, AUD_GPIO_FUNC_HSI2C38);
    I2cReset(Dev);
    Dev->BusReady = TRUE;

    for (I = 0; I < ARRAYSIZE(Dev->Amp); I++) {
        Dev->Amp[I].Present = AmpIdentify(Dev, &Dev->Amp[I]);
    }

    /* cs35l41-core.c:72-92: reset low, 1 ms, high, 2 ms (+1 ms margin) - only for an amp that did not answer and
       whose reset line is not already an output driven high (the boot #88 rule). */
    for (I = 0; I < ARRAYSIZE(Dev->Amp); I++) {
        PAUD_AMP A = &Dev->Amp[I];

        if (!A->Present && (PinFunc(Dev, AUD_GPIO_GPH1_OFF, A->ResetPin) != AUD_GPIO_FUNC_OUTPUT ||
                            PinLevel(Dev, AUD_GPIO_GPH1_OFF, A->ResetPin) == 0)) {
            PinDrive(Dev, AUD_GPIO_GPH1_OFF, A->ResetPin, FALSE);
            A->OtpDone = A->Configured = A->Powered = FALSE;   /* a reset wipes every amp register */
            Released = TRUE;
        }
    }

    if (Released) {
        KeStallExecutionProcessor(1000);
        for (I = 0; I < ARRAYSIZE(Dev->Amp); I++) {
            if (!Dev->Amp[I].Present) {
                PinDrive(Dev, AUD_GPIO_GPH1_OFF, Dev->Amp[I].ResetPin, TRUE);
            }
        }
        KeStallExecutionProcessor(3000);
        AudLog(Dev->Log, "amp resets released: gph1 CON %08lX DAT %08lX\r\n", READ_REGISTER_ULONG(Gpio(Dev, AUD_GPIO_GPH1_OFF)),
               READ_REGISTER_ULONG(Gpio(Dev, AUD_GPIO_GPH1_OFF + AUD_GPIO_BANK_DAT)));
        for (I = 0; I < ARRAYSIZE(Dev->Amp); I++) {
            if (!Dev->Amp[I].Present) {
                Dev->Amp[I].Present = AmpIdentify(Dev, &Dev->Amp[I]);
            }
        }
    }

    for (I = 0; I < ARRAYSIZE(Dev->Amp); I++) {
        if (Dev->Amp[I].Present && !AmpBootDone(Dev, &Dev->Amp[I])) {
            Dev->Amp[I].Present = FALSE;
        }
    }

    AudLog(Dev->Log, "amps: R/bot %s, L/rcv %s\r\n", Dev->Amp[0].Present ? "ready (idle)" : "absent",
           Dev->Amp[1].Present ? "ready (idle)" : "absent");
    WdfWaitLockRelease(Dev->BusLock);
    return (Dev->Amp[0].Present || Dev->Amp[1].Present) ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

static NTSTATUS AmpSetStepsLocked(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ PAUD_AMP Amp, _In_ LONG Steps)
{
    if (Steps > 0) {
        Steps = 0;                       /* never positive (amp notes Safety) */
    }
    if (Steps < CS35L40_VOL_MIN_STEPS) {
        Steps = CS35L40_VOL_MIN_STEPS;
    }

    return AmpUpdate(Dev, Amp->Addr, CS35L40_AMP_DIG_VOL_CTRL, CS35L40_VOL_MASK,
                     ((ULONG)Steps & 0x7FFu) << CS35L40_VOL_SHIFT);
}

/* The digital volume a powered amp should sit at now (fixed, or the endpoint level when HwVolume = 1). */
static LONG AmpTargetSteps(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ PAUD_AMP Amp)
{
    LONG Steps = Dev->Cfg.HwVolume ? (Dev->VolumeLevel / 8192) : AMP_SPK_FIXED_STEPS;   /* 65536/8 KS units per step */

    return (Amp->Addr == AUD_AMP_L_I2C_ADDR) ? Steps + AMP_RCV_TRIM_STEPS : Steps;
}

/* Rows 7.1-7.2 (unmute) / 8.1-8.2 (mute). Caller holds BusLock. */
static NTSTATUS AmpMuteLocked(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ PAUD_AMP Amp, _In_ BOOLEAN Mute)
{
    NTSTATUS Status;

    if (Mute) {
        Status = AmpUpdate(Dev, Amp->Addr, CS35L40_AMP_DIG_VOL_CTRL, CS35L40_VOL_MASK,
                           CS35L40_VOL_MUTE_FIELD << CS35L40_VOL_SHIFT);
        KeStallExecutionProcessor(2000);   /* >= the volume ramp; AudioSpeaker.c :349 waits 2 ms */
        if (NT_SUCCESS(Status)) {
            Status = AmpWrite(Dev, Amp->Addr, CS35L40_AMP_OUT_MUTE, 0x10);
        }
    } else {
        Status = AmpWrite(Dev, Amp->Addr, CS35L40_AMP_OUT_MUTE, 0x00);
        if (NT_SUCCESS(Status)) {
            Status = AmpSetStepsLocked(Dev, Amp, AmpTargetSteps(Dev, Amp));
        }
    }

    return Status;
}

/* Power down one amp: mute, rows 8.3-8.6. Caller holds BusLock. */
/* Diagnostics (earpiece amp powers up clean but is silent, 2026-10-16): the amp notes' verification registers
   (notes/d8-audio-m3-amp.md "Read and log everything" table) - power, PLL, ASP slot/source, boost, gain/volume/mute,
   IRQ1 status + raw status (error bits). Caller holds BusLock. */
static VOID AmpDumpLocked(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ PAUD_AMP Amp, _In_z_ PCSTR When)
{
    static const ULONG Regs[] = { 0x2014, 0x2018, 0x201C, 0x2024, 0x2C04, 0x2C0C, 0x4804, 0x4808, 0x4820, 0x4840,
                                  0x4C00, 0x3800, 0x3804, 0x3808, 0x6000, 0x6C04, 0x10010, 0x10014, 0x10018, 0x1001C,
                                  0x10090, 0x10094, 0x10098, 0x1009C };
    ULONG V[ARRAYSIZE(Regs)];
    ULONG I;

    for (I = 0; I < ARRAYSIZE(Regs); I++) {
        V[I] = 0xDEADDEAD;
        (VOID)AmpRead(Dev, Amp->Addr, Regs[I], &V[I]);
    }
    AudLog(Dev->Log, "amp %s %s: PWR1 %08lX PWR2 %08lX PWR3 %08lX MUTE %08lX PLL %08lX 2C0C %08lX | ASP 4804 %08lX "
           "4808 %08lX RXSLOT %08lX 4840 %08lX DACSRC %08lX | BST %08lX %08lX %08lX | VOL %08lX GAIN %08lX\r\n",
           Amp->Name, When, V[0], V[1], V[2], V[3], V[4], V[5], V[6], V[7], V[8], V[9], V[10], V[11], V[12], V[13],
           V[14], V[15]);
    AudLog(Dev->Log, "amp %s %s: IRQ1 STATUS1-4 %08lX %08lX %08lX %08lX RAW %08lX %08lX %08lX %08lX\r\n", Amp->Name,
           When, V[16], V[17], V[18], V[19], V[20], V[21], V[22], V[23]);
}

static VOID AmpPowerDownLocked(_Inout_ PAUD_DEVICE_CONTEXT Dev, _Inout_ PAUD_AMP Amp)
{
    if (!Amp->Powered) {
        return;
    }

    AmpDumpLocked(Dev, Amp, "before power-down");
    (VOID)AmpMuteLocked(Dev, Amp, TRUE);
    (VOID)AmpRunSeq(Dev, Amp, AmpPowerDownSeq, "power-down", FALSE);
    Amp->Powered = FALSE;
}

/*
 * Configure (once: errata -> OTP unpack -> rows 3-5, the AudioSpeaker.c AmpConfigure order) + GLOBAL_EN on the
 * loudspeaker amp (and the earpiece amp with Parameters\Stereo = 1), then unmute. Requires Cfg.AmpEnable.
 * Order: UAIF1 SPK_EN + RDMA0 must already be running with data (AboxStart first): without data on UAIF1 the amp
 * never reports PUP_DONE (boot #98).
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmpPowerUp(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    NTSTATUS Status = STATUS_NO_SUCH_DEVICE;
    ULONG    I;
    ULONG    S1;

    if (!Dev->Cfg.AmpEnable) {
        AudLog(Dev->Log, "AmpPowerUp: Parameters\\AmpEnable = 0 - amps stay idle\r\n");
        return STATUS_NOT_SUPPORTED;
    }

    WdfWaitLockAcquire(Dev->BusLock, NULL);
    for (I = 0; I < ARRAYSIZE(Dev->Amp); I++) {
        PAUD_AMP A = &Dev->Amp[I];

        if (!A->Present || A->Powered || (A->Addr == AUD_AMP_L_I2C_ADDR && !Dev->Cfg.Stereo)) {
            continue;
        }

        /* Errata and OTP unpack are not idempotent (OTP RMWs on top of the errata writes): once per reset release. */
        if (!A->OtpDone) {
            Status = AmpRunSeq(Dev, A, AmpErrataSeq, "errata", TRUE);
            if (NT_SUCCESS(Status)) {
                Status = AmpOtpUnpack(Dev, A);
            }
            if (!NT_SUCCESS(Status)) {
                AudLog(Dev->Log, "amp %s: errata/OTP failed %08lX - GLOBAL_EN stays off\r\n", A->Name, Status);
                continue;
            }
            A->OtpDone = TRUE;
        }

        if (!A->Configured) {
            Status = AmpRunSeq(Dev, A, AmpConfigSeq, "config", TRUE);
            if (!NT_SUCCESS(Status)) {
                continue;
            }
            A->Configured = TRUE;
        }

        A->Powered = TRUE;   /* from here on AmpPowerDownLocked must undo GLOBAL_EN */
        Status = AmpRunSeq(Dev, A, AmpPowerUpSeq, "power-up", TRUE);
        S1 = 0;
        (VOID)AmpRead(Dev, A->Addr, CS35L40_IRQ1_STATUS1, &S1);
        if (NT_SUCCESS(Status) && (S1 & CS35L40_STATUS1_ERRORS) != 0) {
            AudLog(Dev->Log, "amp %s: error bits after power-up (IRQ1_STATUS1 %08lX) - powering down (row 6.9)\r\n",
                   A->Name, S1);
            Status = STATUS_DEVICE_HARDWARE_ERROR;
        }
        if (NT_SUCCESS(Status)) {
            Status = AmpMuteLocked(Dev, A, Dev->Muted != 0);
        }
        AmpDumpLocked(Dev, A, "after power-up");
        if (!NT_SUCCESS(Status)) {
            AmpPowerDownLocked(Dev, A);
        }
    }
    WdfWaitLockRelease(Dev->BusLock);
    return (Dev->Amp[0].Powered || Dev->Amp[1].Powered) ? STATUS_SUCCESS : Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmpMute(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ BOOLEAN Mute)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG    I;

    WdfWaitLockAcquire(Dev->BusLock, NULL);
    for (I = 0; I < ARRAYSIZE(Dev->Amp); I++) {
        if (Dev->Amp[I].Powered) {
            NTSTATUS S = AmpMuteLocked(Dev, &Dev->Amp[I], Mute);

            if (!NT_SUCCESS(S)) {
                Status = S;
            }
        }
    }
    WdfWaitLockRelease(Dev->BusLock);
    return Status;
}

/* HwVolume = 1 path: the endpoint level (KS 1/65536 dB) onto every powered, unmuted amp. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmpSetVolume(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ LONG LevelKs)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG    I;

    WdfWaitLockAcquire(Dev->BusLock, NULL);
    Dev->VolumeLevel = LevelKs;
    for (I = 0; I < ARRAYSIZE(Dev->Amp); I++) {
        if (Dev->Amp[I].Powered && Dev->Muted == 0) {
            NTSTATUS S = AmpSetStepsLocked(Dev, &Dev->Amp[I], AmpTargetSteps(Dev, &Dev->Amp[I]));

            if (!NT_SUCCESS(S)) {
                Status = S;
            }
        }
    }
    WdfWaitLockRelease(Dev->BusLock);
    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID AmpPowerDown(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    ULONG I;

    WdfWaitLockAcquire(Dev->BusLock, NULL);
    for (I = 0; I < ARRAYSIZE(Dev->Amp); I++) {
        AmpPowerDownLocked(Dev, &Dev->Amp[I]);
    }
    WdfWaitLockRelease(Dev->BusLock);
}

/* ReleaseHardware: amps powered down, controller idle, USI_CON and the I2C pin functions back to what we found.
   The reset lines stay released (amps idle at GLOBAL_EN = 0; an amp in reset needs the full init again anyway). */
_IRQL_requires_(PASSIVE_LEVEL)
VOID AmpRelease(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    if (Dev->BusLock == NULL) {
        return;
    }

    AmpPowerDown(Dev);
    WdfWaitLockAcquire(Dev->BusLock, NULL);
    if (Dev->BusSaved && Dev->Mapped) {
        I2cWr(Dev, HSI2C_INT_EN, 0);
        I2cWr(Dev, HSI2C_INT_STAT, HSI2C_INT_ALL);
        I2cWr(Dev, HSI2C_AUTO_CONF, 0);
        I2cWr(Dev, HSI2C_CTL, 0);
        I2cWr(Dev, HSI2C_USI_CON, Dev->SavedUsiCon);
        WRITE_REGISTER_ULONG(Gpio(Dev, AUD_GPIO_GPH0_OFF), Dev->SavedGph0Con);
        Dev->BusSaved = FALSE;
    }
    Dev->BusReady = FALSE;
    WdfWaitLockRelease(Dev->BusLock);
}
