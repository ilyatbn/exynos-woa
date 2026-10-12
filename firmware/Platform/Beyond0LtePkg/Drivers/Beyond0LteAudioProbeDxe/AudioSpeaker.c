/** @file
  D8a audio milestone 3, stages 1+2: a ~2 s 1.5 kHz test tone on the LOUDSPEAKER, without the ABOX DSP and
  without the amp's DSP: ABOX RDMA0 (DMA from a DRAM buffer) -> SPUS -> SIFS1 -> UAIF1 (DSP-A, ABOX clock
  master, 4 x 32-bit slots, 6.144 MHz BCLK) -> CS35L40 @0x40 ("bot", loudspeaker, RX slot 1) in DSP bypass.
  The earpiece amp @0x41 is not touched.

  Called by AmpProbe with the hsi2c_38 bus up, right after both amps answered their ID read on amps this
  driver took out of reset itself (all amp registers at POR). AmpProbe puts the reset lines back afterwards,
  which wipes every amp register written here. Every ABOX/CMU/pin register written is restored to the value
  read before.

  Sources (file:line in the notes): amp sequence = notes/d8-audio-m3-amp.md "Proposed sequence" rows 0.2-8.6;
  ABOX = notes/d8-audio-m3-abox.md F2-F12 + Stage 1/2; device facts = the boot #96 stage-0 audit.
  UNVERIFIED (the run decides): RDMA CTRL0 FORMAT position (ABOX 2.1 layout bits 12:8 used), RDMA VOL_FACTOR
  unity (0x800000 used; 24-bit linear field), RDMA addressing = physical DRAM with the AUD SysMMU disabled
  (audit: CTRL 4, enable bit0 = 0), SYSREG MISC_CON meaning (vendor writes 1).

  Level: tone at -12 dBFS in slot 1, amp digital volume -20 dB, amp analog gain field 0x0A (amp notes Safety).
  NOT VALIDATED ON HARDWARE (stage Guided).
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/CacheMaintenanceLib.h>

#include <Platform/Exynos9820.h>

#include "AudioCodec.h"
#include "AudioAmpOtp.h"

#define IFR  ((UINTN)EXYNOS_ABOX_IFR_BASE)
#define CMU  ((UINTN)EXYNOS_CMU_AUD_BASE)

//
// CS35L40 registers (vendor include/linux/mfd/cs35l41/registers.h; rows of the amp notes table).
//
#define A_TEST_KEY_CTL     0x00000040u
#define A_OTPID            0x00000010u
#define A_OTP_MEM0         0x00000400u
#define A_IRQ1_STATUS1     0x00010010u
#define A_IRQ1_STATUS3     0x00010018u
#define A_IRQ1_STATUS4     0x0001001Cu
#define A_IRQ1_MASK1       0x00010110u
#define A_IRQ1_DB3         0x00010318u
#define A_IRQ2_DB3         0x00010B18u
#define A_RAW_STATUS1      0x00010090u
#define A_PWR_CTRL1        0x00002014u
#define A_PWR_CTRL2        0x00002018u
#define A_PWR_CTRL3        0x0000201Cu
#define A_AMP_OUT_MUTE     0x00002024u
#define A_PUP_PATCH        0x00002084u
#define A_PLL_CLK_CTRL     0x00002C04u
#define A_GLOBAL_CLK_CTRL  0x00002C0Cu
#define A_BST_PEAK_CUR     0x00003808u
#define A_SP_ENABLES       0x00004800u
#define A_SP_RATE_CTRL     0x00004804u
#define A_SP_FORMAT        0x00004808u
#define A_SP_HIZ_CTRL      0x0000480Cu
#define A_SP_FRAME_RX_SLOT 0x00004820u
#define A_SP_RX_WL         0x00004840u
#define A_DAC_PCM1_SRC     0x00004C00u
#define A_AMP_DIG_VOL      0x00006000u
#define A_AMP_GAIN_CTRL    0x00006C04u
#define A_NG_CFG           0x00006808u
#define A_MIXER_NGATE_CH1  0x00012004u
#define A_MIXER_NGATE_CH2  0x00012008u
#define A_OTP_TRIM_30      0x00007418u
#define A_OTP_TRIM_31      0x0000741Cu

#define A_OTP_BOOT_DONE    BIT1    // IRQ1_STATUS4
#define A_OTP_BOOT_ERR     BIT31   // IRQ1_STATUS3
#define A_PLL_LOCK         BIT1    // IRQ1_STATUS3
#define A_PUP_DONE         BIT24   // IRQ1_STATUS1
#define A_PDN_DONE         BIT23   // IRQ1_STATUS1
#define A_ERR_BITS         (BIT31 | BIT17 | BIT15 | BIT8 | BIT7 | BIT6)   // amp short, OT, temp warn, bst short/UV/OV

#define AMP_SLAVE          EXYNOS_AMP_R_I2C_ADDR   // 0x40 "bot" = loudspeaker
#define AMP_RX_SLOT        0x00000001u             // @40 (right-channel-amp) takes slot 1 (table row 5.2)
#define AMP_GAIN_FIELD     0x0Au                   // [9:5], Safety: first test value
#define AMP_VOL_MUTE       0x4CFu                  // [13:3]
#define AMP_VOL_M20DB      0x760u                  // [13:3] = -20 dB (11-bit two's complement, 1/8 dB steps)
#define AMP_VOL_M10DB      0x7B0u                  // [13:3] = -10 dB

//
// ABOX (ifr) / CMU_AUD / GPIO_AUD / SYSREG_AUD offsets (abox notes F2, F6, F7, F8, F10-F12).
//
#define X_SYSPOWER_CTRL    0x0010u
#define X_QCHANNEL_DISABLE 0x0038u
#define   X_QCH_BCLK_UAIF1   BIT3
#define X_ROUTE_CTRL0      0x0040u
#define   X_ROUTE_UAIF1_SIFS1 0x00000020u   // ROUTE_CTRL0[7:4] = 2 "SIFS1"
#define X_SBANK_RDMA0      0x0220u
#define   X_SBANK_RDMA0_VAL  0x02000000u    // (SZ_512 << 16) | 0, patch_9820
#define X_UAIF1_CTRL0      0x0510u
#define X_UAIF1_CTRL1      0x0514u
#define X_UAIF1_STATUS     0x051Cu
#define   X_UAIF1_CTRL1_4X32 0x1B4DF7DFu    // F10: DSP-A, 4 slots x 32 bit
#define   X_UAIF1_CTRL0_MST  0x01000014u    // F10: reset 0x01000010 + MODE (ABOX master)
#define   X_UAIF1_SPK_EN     BIT0
#define X_RDMA0            0x1000u
#define   R_CTRL0            0x00u
#define   R_BUF_STR          0x08u
#define   R_BUF_END          0x0Cu
#define   R_BUF_OFFSET       0x10u
#define   R_STR_POINT        0x14u
#define   R_VOL_FACTOR       0x18u
#define   R_STATUS           0x30u          // ABOX > 1.0.0: STATUS at +0x30 (abox_soc.h:418-423)
#define   R_CTRL0_RESET      0x00400000u    // audit: BURST_LEN 8
#define   R_FORMAT_4X32      (0x1Bu << 8)   // UNVERIFIED position on 2.0.1 (2.1 layout)
#define   R_VOL_UNITY        0x00800000u    // UNVERIFIED unity

#define C_MUX_UAIF1        0x1010u
#define C_DIV_AUDIF        0x1800u
#define C_DIV_UAIF1        0x1830u
#define   C_DIV_MASK         0x1FFu         // 9-bit DIVRATIO
#define   C_DIV_BUSY         BIT16
#define   C_DIV_UAIF1_6M144  95u            // audit: AUDIF = PLL 1179.648 / 2 = 589.824 MHz; /96 = 6.144 MHz

#define G_GPB1_CON         0x0020u
#define   G_GPB1_UAIF1_MASK  0xFFFFu
#define   G_GPB1_UAIF1_FUNC  0x2222u        // aud_i2s1_bus gpb1-0..3 function 2 (F7)
#define S_MISC_CON         0x0504u

//
// Tone buffer: 48 kHz, 4 x 32-bit slots, 1.5 kHz = 32 frames per period, 100 ms ring.
//
#define TONE_FRAMES        4800u
#define TONE_SLOTS         4u
#define TONE_BYTES         (TONE_FRAMES * TONE_SLOTS * 4u)
#define TONE_PERIOD_BYTES  (TONE_BYTES / 2u)
#define TONE_HOLD_MS       2000u
#define SRAM_TONE_OFFSET   0x10000u           // inside ABOX SRAM (0x59000 B), unused while the DSP is off

// One 1.5 kHz period, sin * 0.25 (-12 dBFS) as left-justified 32-bit (round(0.25 * 2^31 * sin(2 pi k / 32))).
STATIC CONST INT32  mSine32[32] = {
  0,          104755299,  205482679,  298297290,  379625062,  446349399,  495930060,  526493219,
  536870912,  526493219,  495930060,  446349399,  379625062,  298297290,  205482679,  104755299,
  0,          -104755299, -205482679, -298297290, -379625062, -446349399, -495930060, -526493219,
  -536870912, -526493219, -495930060, -446349399, -379625062, -298297290, -205482679, -104755299
};

STATIC UINTN  mAmpErrors;

STATIC
UINT32
ARd (IN UINT32 Reg)
{
  UINT32  V = 0;

  if (EFI_ERROR (AmpRead (AMP_SLAVE, Reg, &V))) {
    mAmpErrors++;
  }

  return V;
}

STATIC
VOID
AWr (IN UINT32 Reg, IN UINT32 Val)
{
  if (EFI_ERROR (AmpWrite (AMP_SLAVE, Reg, Val))) {
    mAmpErrors++;
  }
}

STATIC
VOID
ARmw (IN UINT32 Reg, IN UINT32 Mask, IN UINT32 Val)
{
  UINT32  V;

  if (EFI_ERROR (AmpRead (AMP_SLAVE, Reg, &V))) {
    mAmpErrors++;
    return;
  }

  AWr (Reg, (V & ~Mask) | (Val & Mask));
}

STATIC
VOID
AUnlock (VOID)
{
  AWr (A_TEST_KEY_CTL, 0x00005555);
  AWr (A_TEST_KEY_CTL, 0x0000AAAA);
}

STATIC
VOID
ALock (VOID)
{
  AWr (A_TEST_KEY_CTL, 0x0000CCCC);
  AWr (A_TEST_KEY_CTL, 0x00003333);
}

// Polls Reg until (Reg & Bit) != 0, 1 ms steps; returns the ms waited or MaxMs + 1.
STATIC
UINTN
APoll (IN UINT32 Reg, IN UINT32 Bit, IN UINTN MaxMs, OUT UINT32 *Last)
{
  UINTN  Ms;

  for (Ms = 0; Ms <= MaxMs; Ms++) {
    *Last = ARd (Reg);
    if ((*Last & Bit) != 0) {
      return Ms;
    }

    gBS->Stall (1000);
  }

  return MaxMs + 1;
}

// cs35l41_otp_unpack (vendor cs35l41.c:544-632), same bit walk.
STATIC
BOOLEAN
AmpOtpUnpack (VOID)
{
  UINT32  Mem[32];
  UINT32  OtpId;
  UINT32  Val;
  UINT32  BitOffset;
  UINT32  ArrayOffset;
  UINT32  Size;
  UINTN   I;

  AUnlock ();
  OtpId = ARd (A_OTPID);
  for (I = 0; I < 32; I++) {
    Mem[I] = ARd (A_OTP_MEM0 + (UINT32)I * 4);
  }

  if (((Mem[1] & 0xFFFFFFFEu) != 0x1500u) || ((Mem[2] & 0xFFFFu) != 0x0300u) || ((OtpId != 2) && (OtpId != 3))) {
    DEBUG ((DEBUG_ERROR, "AudioSpk: OTP header bad (OTPID %x MEM1 %x MEM2 %x) - abort\n", OtpId, Mem[1], Mem[2]));
    ALock ();
    return FALSE;
  }

  BitOffset   = 16;
  ArrayOffset = 2;
  for (I = 0; I < ARRAY_SIZE (mAmpOtpMap2); I++) {
    Size = mAmpOtpMap2[I].Size;
    if (BitOffset + Size - 1 >= 32) {
      Val  = (UINT32)(((UINT64)Mem[ArrayOffset] & (0xFFFFFFFFull << BitOffset) & 0xFFFFFFFFull) >> BitOffset);
      ArrayOffset++;
      Val |= (Mem[ArrayOffset] & (UINT32)((1ull << (BitOffset + Size - 32)) - 1)) << (32 - BitOffset);
      BitOffset += Size - 32;
    } else {
      Val        = (UINT32)(((UINT64)Mem[ArrayOffset] >> BitOffset) & ((1ull << Size) - 1));
      BitOffset += Size;
    }

    if (BitOffset == 32) {
      BitOffset = 0;
      ArrayOffset++;
    }

    if (mAmpOtpMap2[I].Reg != 0) {
      ARmw (mAmpOtpMap2[I].Reg, (UINT32)(((1ull << Size) - 1) << mAmpOtpMap2[I].Shift), Val << mAmpOtpMap2[I].Shift);
    }
  }

  ALock ();
  DEBUG ((DEBUG_WARN, "AudioSpk: OTP unpacked (OTPID %x, %u fields, ended at word %u bit %u), I2C errors so far %u\n",
          OtpId, (UINT32)ARRAY_SIZE (mAmpOtpMap2), ArrayOffset, BitOffset, (UINT32)mAmpErrors));
  return TRUE;
}

// Amp table rows 0.2-5.7: from POR to "configured, muted, powered down".
STATIC
BOOLEAN
AmpConfigure (VOID)
{
  UINT32  V;
  UINTN   Ms;

  Ms = APoll (A_IRQ1_STATUS4, A_OTP_BOOT_DONE, 100, &V);
  DEBUG ((DEBUG_WARN, "AudioSpk: amp @%x OTP_BOOT_DONE after %u ms (IRQ1_STATUS4 %x), IRQ1_STATUS3 %x\n",
          AMP_SLAVE, (UINT32)Ms, V, ARd (A_IRQ1_STATUS3)));
  if ((Ms > 100) || ((ARd (A_IRQ1_STATUS3) & A_OTP_BOOT_ERR) != 0)) {
    return FALSE;
  }

  // 1.x REVID A0 errata (vendor cs35l41.c:2040-2052).
  AUnlock ();
  AWr (0x00003854, 0x05180240);
  AWr (A_OTP_TRIM_30, 0x9091A1C8);
  AWr (0x00003014, 0x0200EE0E);
  AWr (0x0000381C, 0x00000051);
  AWr (0x00000054, 0x00000004);
  AWr (A_IRQ1_DB3, 0);
  AWr (A_IRQ2_DB3, 0);
  ALock ();

  if (!AmpOtpUnpack ()) {
    return FALSE;
  }

  // 3.x vendor DSP-bypass cap-trim clear, IRQ mask, boost peak, SDOUT hi-z, noise gate (stock DT values).
  DEBUG ((DEBUG_WARN, "AudioSpk: trims before bypass clear: OTP_TRIM_30 %x OTP_TRIM_31 %x\n", ARd (A_OTP_TRIM_30), ARd (A_OTP_TRIM_31)));
  AUnlock ();
  ARmw (A_OTP_TRIM_30, 0x00000F80u, 0);
  ARmw (A_OTP_TRIM_31, 0x0000001Fu, 0);
  ALock ();
  AWr (A_IRQ1_MASK1, 0x7FFCFE3F);
  ARmw (A_BST_PEAK_CUR, 0x7Fu, 0x42u);
  ARmw (A_SP_HIZ_CTRL, 0x3u, 0x3u);
  AWr (A_NG_CFG, 0x00003F26);
  AWr (A_MIXER_NGATE_CH1, 0x00010206);
  AWr (A_MIXER_NGATE_CH2, 0x00010206);

  // 4.x clocking: SCLK 6.144 MHz (code 0x28), PLL from SCLK, 48 kHz.
  ARmw (A_SP_RATE_CTRL, 0x3Fu, 0x28u);
  AWr (A_PLL_CLK_CTRL, 0x810);
  AWr (A_PLL_CLK_CTRL, 0xD10);
  AWr (A_PLL_CLK_CTRL, 0xD00);
  AWr (A_PLL_CLK_CTRL, 0xD00);
  AWr (A_PLL_CLK_CTRL, 0x500);
  AWr (A_PLL_CLK_CTRL, 0x510);
  ARmw (A_GLOBAL_CLK_CTRL, 0x1Fu, 0x03u);

  // 5.x ASP + data path + gain, muted.
  AWr (A_SP_FORMAT, 0x20200040);
  AWr (A_SP_FRAME_RX_SLOT, AMP_RX_SLOT);
  AWr (A_SP_RX_WL, 0x18);
  AWr (A_DAC_PCM1_SRC, 0x08);
  ARmw (A_AMP_GAIN_CTRL, 0x3E0u, AMP_GAIN_FIELD << 5);
  ARmw (A_AMP_DIG_VOL, 0x3FF8u, AMP_VOL_MUTE << 3);
  AWr (A_AMP_OUT_MUTE, 0x10);
  DEBUG ((DEBUG_WARN, "AudioSpk: amp configured: SP_FORMAT %x RX_SLOT %x PLL_CLK_CTRL %x GLOBAL_CLK %x DAC_SRC %x GAIN %x DIGVOL %x"
                      " BST_PEAK %x, I2C errors %u\n",
          ARd (A_SP_FORMAT), ARd (A_SP_FRAME_RX_SLOT), ARd (A_PLL_CLK_CTRL), ARd (A_GLOBAL_CLK_CTRL), ARd (A_DAC_PCM1_SRC),
          ARd (A_AMP_GAIN_CTRL), ARd (A_AMP_DIG_VOL), ARd (A_BST_PEAK_CUR), (UINT32)mAmpErrors));
  return mAmpErrors == 0;
}

STATIC
VOID
AmpPowerDown (VOID)
{
  UINT32  V;
  UINTN   Ms;

  ARmw (A_AMP_DIG_VOL, 0x3FF8u, AMP_VOL_MUTE << 3);
  gBS->Stall (2000);
  AWr (A_AMP_OUT_MUTE, 0x10);
  ARmw (A_PWR_CTRL1, BIT0, 0);
  Ms = APoll (A_IRQ1_STATUS1, A_PDN_DONE, 100, &V);
  AWr (A_IRQ1_STATUS1, A_PDN_DONE);
  AUnlock ();
  AWr (A_PUP_PATCH, 0x002F1AA3);
  ALock ();
  AWr (A_SP_ENABLES, 0);
  AWr (A_PWR_CTRL2, 0);
  DEBUG ((DEBUG_WARN, "AudioSpk: amp powered down: PDN_DONE after %u ms (IRQ1_STATUS1 %x)\n", (UINT32)Ms, V));
}

STATIC
VOID
LogState (IN CONST CHAR8 *When, IN UINTN RdmaReg)
{
  DEBUG ((DEBUG_WARN, "AudioSpk: %a: RDMA0 CTRL0 %x STATUS %x | UAIF1 CTRL0 %x CTRL1 %x STATUS %x | ROUTE0 %x QCH %x SYSPOWER %x/%x"
                      " | amp IRQ1_STATUS1 %x STATUS3 %x RAW1 %x PWR1 %x\n",
          When, MmioRead32 (RdmaReg + R_CTRL0), MmioRead32 (RdmaReg + R_STATUS), MmioRead32 (IFR + X_UAIF1_CTRL0),
          MmioRead32 (IFR + X_UAIF1_CTRL1), MmioRead32 (IFR + X_UAIF1_STATUS), MmioRead32 (IFR + X_ROUTE_CTRL0),
          MmioRead32 (IFR + X_QCHANNEL_DISABLE), MmioRead32 (IFR + X_SYSPOWER_CTRL), MmioRead32 (IFR + 0x14),
          ARd (A_IRQ1_STATUS1), ARd (A_IRQ1_STATUS3), ARd (A_RAW_STATUS1), ARd (A_PWR_CTRL1)));
}

STATIC
BOOLEAN
SetDiv (IN UINTN Reg, IN UINT32 Ratio)
{
  UINTN  Us;

  MmioAndThenOr32 (Reg, ~C_DIV_MASK, Ratio & C_DIV_MASK);
  for (Us = 0; Us < 1000; Us += 10) {
    if ((MmioRead32 (Reg) & C_DIV_BUSY) == 0) {
      return TRUE;
    }

    gBS->Stall (10);
  }

  return FALSE;
}

//
// m3 run 2: the RDMA address space is UNVERIFIED, so one boot tries three buffers, each with its own pitch
// so the PC microphone tells which one sounded: physical DRAM, ABOX SRAM by physical address, ABOX SRAM by
// DSP-side offset (abox_addr_to_phys_addr: addresses < 0x80000000 are SRAM offsets, abox.c:249-262).
//
typedef struct {
  CONST CHAR8  *Name;
  UINT32       Kind;     // 0 = DRAM buffer, 1 = SRAM, DMA address = physical, 2 = SRAM, DMA address = offset
  UINT32       Step;     // sine table step: 1 = 1.5 kHz, 2 = 3 kHz, 4 = 6 kHz
} TONE_VARIANT;

STATIC CONST TONE_VARIANT  mVariants[] = {
  { "DRAM physical, 1.5 kHz",       0, 1 },
  { "SRAM physical, 3 kHz",         1, 2 },
  { "SRAM offset 0x10000, 6 kHz",   2, 4 },
};

// Fill the variant's buffer, program RDMA0 and enable it (UAIF1 only clocks while the DMA feeds it: run b98).
STATIC
VOID
VariantStart (IN CONST TONE_VARIANT *T, IN UINTN Rdma, IN EFI_PHYSICAL_ADDRESS DramBuf)
{
  volatile INT32  *Pcm;
  UINT32          Dma;
  UINT32          St[8];
  UINTN           I;

  if (T->Kind == 0) {
    Pcm = (volatile INT32 *)(UINTN)DramBuf;
    Dma = (UINT32)DramBuf;
  } else {
    Pcm = (volatile INT32 *)(UINTN)(EXYNOS_ABOX_SRAM_BASE + SRAM_TONE_OFFSET);
    Dma = (T->Kind == 1) ? (UINT32)(EXYNOS_ABOX_SRAM_BASE + SRAM_TONE_OFFSET) : SRAM_TONE_OFFSET;
  }

  for (I = 0; I < TONE_FRAMES; I++) {
    Pcm[I * TONE_SLOTS + 0] = 0;
    Pcm[I * TONE_SLOTS + 1] = mSine32[(I * T->Step) % 32];
    Pcm[I * TONE_SLOTS + 2] = 0;
    Pcm[I * TONE_SLOTS + 3] = 0;
  }

  if (T->Kind == 0) {
    WriteBackDataCacheRange ((VOID *)(UINTN)DramBuf, TONE_BYTES);
  }

  MemoryFence ();
  MmioWrite32 (Rdma + R_BUF_STR, Dma);
  MmioWrite32 (Rdma + R_BUF_END, Dma + TONE_BYTES);
  MmioWrite32 (Rdma + R_BUF_OFFSET, TONE_PERIOD_BYTES);
  MmioWrite32 (Rdma + R_STR_POINT, Dma);
  MmioWrite32 (Rdma + R_VOL_FACTOR, R_VOL_UNITY);
  MmioWrite32 (Rdma + R_CTRL0, R_CTRL0_RESET | R_FORMAT_4X32 | BIT0);
  for (I = 0; I < ARRAY_SIZE (St); I++) {
    St[I] = MmioRead32 (Rdma + R_STATUS);
    gBS->Stall (5000);
  }

  DEBUG ((DEBUG_WARN, "AudioSpk: variant \"%a\": DMA %x, RDMA0 STATUS every 5 ms: %x %x %x %x %x %x %x %x | UAIF1 STATUS %x\n",
          T->Name, Dma, St[0], St[1], St[2], St[3], St[4], St[5], St[6], St[7], MmioRead32 (IFR + X_UAIF1_STATUS)));
}

// Hold the running variant for TONE_HOLD_MS (amp error bits checked every 250 ms), then disable RDMA0.
STATIC
BOOLEAN
VariantHoldStop (IN CONST TONE_VARIANT *T, IN UINTN Rdma)
{
  UINTN   I;
  UINT32  V;

  V = 0;
  for (I = 0; I < TONE_HOLD_MS / 250; I++) {
    gBS->Stall (250 * 1000);
    V = ARd (A_IRQ1_STATUS1);
    if ((V & A_ERR_BITS) != 0) {
      DEBUG ((DEBUG_ERROR, "AudioSpk: amp error bits %x at %u ms - stopping\n", V, (UINT32)((I + 1) * 250)));
      break;
    }
  }

  LogState (T->Name, Rdma);
  MmioWrite32 (Rdma + R_CTRL0, R_CTRL0_RESET);
  for (I = 0; I < 50 && (MmioRead32 (Rdma + R_STATUS) & BIT31) != 0; I++) {
    gBS->Stall (1000);
  }

  DEBUG ((DEBUG_WARN, "AudioSpk: variant \"%a\" stopped: RDMA0 STATUS %x after %u ms\n", T->Name, MmioRead32 (Rdma + R_STATUS), (UINT32)I));
  return (V & A_ERR_BITS) == 0;
}

BOOLEAN
SpeakerTonePlay (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Buf;
  UINTN                 Pages;
  UINTN                 Ms;
  UINTN                 Rdma;
  UINT32                V;
  UINT32                SavedDiv;
  UINT32                SavedGpb1;
  UINT32                SavedMisc;
  UINT32                SavedSyspower;
  UINT32                SavedQch;
  UINT32                SavedRoute;
  UINT32                SavedSbank;
  UINT32                SavedCtrl0;
  UINT32                SavedCtrl1;
  UINTN                 Variant;
  BOOLEAN               Played;

  Rdma       = IFR + X_RDMA0;
  mAmpErrors = 0;
  Played     = FALSE;
  if ((MmioRead32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_AUD_STATUS_OFFSET) & 1u) == 0) {
    DEBUG ((DEBUG_ERROR, "AudioSpk: pd-aud is OFF - no speaker tone\n"));
    return FALSE;
  }

  DEBUG ((DEBUG_WARN, "AudioSpk: start - 1.5 kHz -12 dBFS tone, slot 1 of 4 x 32 bit, RDMA0 -> SIFS1 -> UAIF1 -> amp @%x,"
                      " amp gain field %x, digital -20 dB, %u ms\n", AMP_SLAVE, AMP_GAIN_FIELD, TONE_HOLD_MS));
  if (!AmpConfigure ()) {
    DEBUG ((DEBUG_ERROR, "AudioSpk: amp configure failed (I2C errors %u) - abort before any ABOX write\n", (UINT32)mAmpErrors));
    return FALSE;
  }

  // Tone ring in DRAM below 4 GiB (RDMA BUF_STR is a 32-bit field), written back from the CPU caches.
  Pages = EFI_SIZE_TO_PAGES (TONE_BYTES);
  Buf   = 0xFFFFFFFFull;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Pages, &Buf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AudioSpk: buffer allocation %r - abort\n", Status));
    return FALSE;
  }

  // Save everything the ABOX side writes.
  SavedDiv      = MmioRead32 (CMU + C_DIV_UAIF1);
  SavedGpb1     = MmioRead32 ((UINTN)EXYNOS_PINCTRL_AUD_BASE + G_GPB1_CON);
  SavedMisc     = MmioRead32 ((UINTN)EXYNOS_ABOX_SYSREG_BASE + S_MISC_CON);
  SavedSyspower = MmioRead32 (IFR + X_SYSPOWER_CTRL);
  SavedQch      = MmioRead32 (IFR + X_QCHANNEL_DISABLE);
  SavedRoute    = MmioRead32 (IFR + X_ROUTE_CTRL0);
  SavedSbank    = MmioRead32 (IFR + X_SBANK_RDMA0);
  SavedCtrl0    = MmioRead32 (IFR + X_UAIF1_CTRL0);
  SavedCtrl1    = MmioRead32 (IFR + X_UAIF1_CTRL1);

  // Stage 1: UAIF1 bit clock 6.144 MHz, pins, ABOX master, 4 x 32.
  if (!SetDiv (CMU + C_DIV_UAIF1, C_DIV_UAIF1_6M144)) {
    DEBUG ((DEBUG_ERROR, "AudioSpk: DIV_UAIF1 stayed BUSY (%x)\n", MmioRead32 (CMU + C_DIV_UAIF1)));
  }

  DEBUG ((DEBUG_WARN, "AudioSpk: CMU_AUD DIV_AUDIF %x DIV_UAIF1 %x -> %x MUX_UAIF1 %x\n", MmioRead32 (CMU + C_DIV_AUDIF), SavedDiv,
          MmioRead32 (CMU + C_DIV_UAIF1), MmioRead32 (CMU + C_MUX_UAIF1)));
  MmioAndThenOr32 ((UINTN)EXYNOS_PINCTRL_AUD_BASE + G_GPB1_CON, ~G_GPB1_UAIF1_MASK, G_GPB1_UAIF1_FUNC);
  MmioWrite32 ((UINTN)EXYNOS_ABOX_SYSREG_BASE + S_MISC_CON, 1);
  MmioWrite32 (IFR + X_SYSPOWER_CTRL, 1);   // abox_request_dram_on: the RDMA reads DRAM
  MmioOr32 (IFR + X_QCHANNEL_DISABLE, X_QCH_BCLK_UAIF1);
  MmioWrite32 (IFR + X_UAIF1_CTRL1, X_UAIF1_CTRL1_4X32);
  MmioWrite32 (IFR + X_UAIF1_CTRL0, X_UAIF1_CTRL0_MST);

  // Stage 2: RDMA0 -> SIFS1 -> UAIF1 route; the ring is programmed per variant (PlayVariant).
  MmioWrite32 (IFR + X_SBANK_RDMA0, X_SBANK_RDMA0_VAL);
  MmioWrite32 (IFR + X_ROUTE_CTRL0, X_ROUTE_UAIF1_SIFS1);
  LogState ("route set, RDMA0 off", Rdma);
  MmioOr32 (IFR + X_UAIF1_CTRL0, X_UAIF1_SPK_EN);
  DEBUG ((DEBUG_WARN, "AudioSpk: buffer %lx..%lx; gpb1 CON %x; RDMA0 BUF_STR %x END %x OFFSET %x STR_POINT %x VOL %x\n",
          Buf, Buf + TONE_BYTES, MmioRead32 ((UINTN)EXYNOS_PINCTRL_AUD_BASE + G_GPB1_CON), MmioRead32 (Rdma + R_BUF_STR),
          MmioRead32 (Rdma + R_BUF_END), MmioRead32 (Rdma + R_BUF_OFFSET), MmioRead32 (Rdma + R_STR_POINT),
          MmioRead32 (Rdma + R_VOL_FACTOR)));
  LogState ("UAIF1 SPK on", Rdma);

  // Amp power-up (rows 6.2-7.2) with the bit clock running.
  VariantStart (&mVariants[0], Rdma, Buf);   // data must flow before the amp PLL can lock (b98: no lock, no PUP_DONE)
  Ms = APoll (A_IRQ1_STATUS3, A_PLL_LOCK, 100, &V);
  DEBUG ((DEBUG_WARN, "AudioSpk: amp PLL_LOCK after %u ms (IRQ1_STATUS3 %x)\n", (UINT32)Ms, V));
  AWr (A_IRQ1_STATUS3, A_PLL_LOCK);
  AWr (A_SP_ENABLES, 0x00010000);
  ARmw (A_PWR_CTRL3, BIT4, BIT4);
  AWr (A_PWR_CTRL2, 0x00000021);
  AUnlock ();
  AWr (A_PUP_PATCH, 0x002F1AA0);
  ALock ();
  ARmw (A_PWR_CTRL1, BIT0, BIT0);
  Ms = APoll (A_IRQ1_STATUS1, A_PUP_DONE, 100, &V);
  DEBUG ((DEBUG_WARN, "AudioSpk: amp PUP_DONE after %u ms (IRQ1_STATUS1 %x)\n", (UINT32)Ms, V));
  AWr (A_IRQ1_STATUS1, A_PUP_DONE);
  if ((Ms <= 100) && ((V & A_ERR_BITS) == 0)) {
    AWr (A_AMP_OUT_MUTE, 0);
    ARmw (A_AMP_DIG_VOL, 0x3FF8u, AMP_VOL_M10DB << 3);
    Played = TRUE;
    for (Variant = 0; Variant < ARRAY_SIZE (mVariants) && Played; Variant++) {
      if (Variant != 0) {
        VariantStart (&mVariants[Variant], Rdma, Buf);
      }

      Played = VariantHoldStop (&mVariants[Variant], Rdma);
    }

    LogState ("tones end", Rdma);
  } else {
    DEBUG ((DEBUG_ERROR, "AudioSpk: amp did not power up cleanly (PUP %u ms, IRQ1_STATUS1 %x) - no unmute\n", (UINT32)Ms, V));
  }

  AmpPowerDown ();

  // ABOX teardown: stop the interface, then the DMA, then restore every saved register.
  MmioAnd32 (IFR + X_UAIF1_CTRL0, ~X_UAIF1_SPK_EN);
  MmioWrite32 (Rdma + R_CTRL0, R_CTRL0_RESET);
  for (Ms = 0; Ms < 50 && (MmioRead32 (Rdma + R_STATUS) & BIT31) != 0; Ms++) {
    gBS->Stall (1000);
  }

  MmioWrite32 (IFR + X_ROUTE_CTRL0, SavedRoute);
  MmioWrite32 (Rdma + R_BUF_STR, 0);
  MmioWrite32 (Rdma + R_BUF_END, 0);
  MmioWrite32 (Rdma + R_BUF_OFFSET, 0);
  MmioWrite32 (Rdma + R_STR_POINT, 0);
  MmioWrite32 (Rdma + R_VOL_FACTOR, 0);
  MmioWrite32 (IFR + X_SBANK_RDMA0, SavedSbank);
  MmioWrite32 (IFR + X_UAIF1_CTRL0, SavedCtrl0);
  MmioWrite32 (IFR + X_UAIF1_CTRL1, SavedCtrl1);
  MmioWrite32 (IFR + X_QCHANNEL_DISABLE, SavedQch);
  MmioWrite32 (IFR + X_SYSPOWER_CTRL, SavedSyspower);
  MmioWrite32 ((UINTN)EXYNOS_ABOX_SYSREG_BASE + S_MISC_CON, SavedMisc);
  MmioWrite32 ((UINTN)EXYNOS_PINCTRL_AUD_BASE + G_GPB1_CON, SavedGpb1);
  (VOID)SetDiv (CMU + C_DIV_UAIF1, SavedDiv & C_DIV_MASK);
  LogState ("after teardown", Rdma);
  if ((MmioRead32 (Rdma + R_STATUS) & BIT31) == 0) {
    gBS->FreePages (Buf, Pages);
  } else {
    DEBUG ((DEBUG_ERROR, "AudioSpk: RDMA0 still in progress after disable - buffer pages kept\n"));
  }

  DEBUG ((DEBUG_WARN, "AudioSpk: done - tone %a (amp I2C errors %u); amps go back into reset next\n",
          Played ? "variants PLAYED (mic decides which one sounded)" : "NOT played", (UINT32)mAmpErrors));
  return Played;
}
