/** @file
  D8a audio milestone 2: the CS47L93 codec's own tone generator -> OUT2 mixers -> HPOUT2 -> 3.5 mm jack,
  ~2 s, from SPI register writes only (no ABOX, no AP clock, no Windows driver).

  Called by CodecProbe (Beyond0LteAudioProbeDxe.c) with the spi_2 bus up, right after the codec was taken
  out of hard reset by us and answered its ID + BOOT_DONE, i.e. every codec register is at its reset
  default. CodecProbe puts the codec back into hard reset afterwards, which wipes everything written here;
  the teardown below still runs the kernel's own power-down order first so the output stage turns off
  cleanly before the reset hits it.

  Every register, field and value is cited in notes/d8-audio-m1.md §6.1/§6.2. Sources (LineageOS
  android_kernel_samsung_exynos9820 @ lineage-22.2): [CORE] drivers/mfd/madera-core.c, [L92T]
  drivers/mfd/cs47l92-tables.c, [L92] sound/soc/codecs/cs47l92.c, [MC] sound/soc/codecs/madera.c,
  [MH] madera.h, [MR] include/linux/mfd/madera/registers.h, [BM] sound/soc/samsung/beyond_madera.c;
  [DTBO] = the phone's stock dtbo.

  Each step is proven by a register read-back logged to UEFILOG (prefix "AudioTone:") - the test runs
  unattended; the ear test is the user's, later.

  NOT VALIDATED ON HARDWARE (stage Guided).
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>

#include "AudioCodec.h"

//
// Registers [MR] (line numbers in notes §6.1).
//
#define R_SOFTWARE_RESET        0x0000u
#define R_TONE_GENERATOR_1      0x0020u   // TONE_RATE[15:11], TONE1_OVD bit4, TONE1_ENA bit0
#define   TONE1_ENA               0x0001u
#define R_CLOCK_32K_1           0x0100u   // CLK_32K_ENA bit6, CLK_32K_SRC[1:0]
#define   CLK_32K_ENA             0x0040u
#define   CLK_32K_SRC_MASK        0x0003u
#define   CLK_32K_SRC_MCLK2       0x0001u // [CORE:41] MADERA_32KZ_MCLK2
#define R_SYSTEM_CLOCK_1        0x0101u   // SYSCLK_FRAC bit15, SYSCLK_FREQ[10:8], SYSCLK_ENA bit6, SYSCLK_SRC[3:0]
#define   SYSCLK_SET_MASK         0x870Fu // FRAC | FREQ | SRC ([MC:3001-3013] madera_set_sysclk mask)
#define   SYSCLK_98MHZ_FLL1       0x0404u // FREQ 4 = MADERA_SYSCLK_98MHZ [MC:71], SRC 4 = MADERA_CLK_SRC_FLL1 [MH:57]
#define   SYSCLK_ENA              0x0040u
#define R_SAMPLE_RATE_1         0x0102u   // [4:0]
#define   SAMPLE_RATE_48K         0x0003u // madera_sr_vals[3] = 48000 [MC:3216-3241]

#define R_FLL1_CONTROL_1        0x0171u   // REFCLK_SRC[15:12] (CS47L92), HOLD bit2, ENA bit0
#define   FLL_REFCLK_SRC_MASK     0xF000u
#define   FLL_REFCLK_SRC_SHIFT    12
#define   FLL_HOLD                0x0004u
#define   FLL_ENA                 0x0001u
#define R_FLL1_CONTROL_2        0x0172u   // CTRL_UPD bit15, N[9:0]
#define   FLL_CTRL_UPD            0x8000u
#define R_FLL1_CONTROL_3        0x0173u   // THETA
#define R_FLL1_CONTROL_4        0x0174u   // LAMBDA
#define R_FLL1_CONTROL_5        0x0175u   // FB_DIV[9:0]
#define   FLL_FB_DIV_MASK         0x03FFu
#define R_FLL1_CONTROL_6        0x0176u   // REFCLK_DIV[7:6]
#define   FLL_REFCLK_DIV_MASK     0x00C0u
#define   FLL_REFCLK_DIV_SHIFT    6
#define R_FLL1_CONTROL_8        0x0178u   // CS47L92_FLL1_CONTROL_8 = fll->base + MADERA_FLL_GAIN_OFFS (gains, 16 bits)
#define R_FLL1_CONTROL_10       0x017Au   // HP[15:14], PHASEDET_ENA bit12
#define   FLL_HP_MASK             0xC000u
#define   FLL_HP_SHIFT            14
#define   FLL_PHASEDET_ENA        0x1000u
#define R_FLL1_CONTROL_11       0x017Bu   // LOCKDET_THR[4:1], LOCKDET bit0
#define   FLL_LOCKDET_THR_MASK    0x001Eu
#define   FLL_LOCKDET_THR_SHIFT   1
#define   FLL_LOCKDET             0x0001u
#define R_FLL1_DIGITAL_TEST_1   0x017Du   // SYNC_EFS_ENA bit8, CLK_VCO_FAST_SRC[1:0]
#define   FLL_DT1_MASK            0x0103u

#define R_OUTPUT_ENABLES_1      0x0400u   // EP_SEL bit15, OUT2L_ENA bit3, OUT2R_ENA bit2
#define   OUT2_ENA_BOTH           0x000Cu
#define R_RAW_OUTPUT_STATUS_1   0x0406u   // read for the log only (no field defs in [MR])
#define R_DAC_DIGITAL_VOLUME_2L 0x0419u   // VU bit9, MUTE bit8, VOL[7:0] (0x80 = 0 dB, 0.5 dB/step)
#define R_DAC_DIGITAL_VOLUME_2R 0x041Du
#define   DAC_VU                  0x0200u
#define   DAC_MUTE                0x0100u
#define   DAC_VOL_0DB             0x0080u
#define R_OUT2LMIX_INPUT_1_SRC  0x0690u
#define R_OUT2LMIX_INPUT_1_VOL  0x0691u   // VOL[7:1]: 0x20 = -32 dB .. 0x40 = 0 dB, 1 dB/step [MH:74-75,194-197]
#define R_OUT2RMIX_INPUT_1_SRC  0x0698u
#define R_OUT2RMIX_INPUT_1_VOL  0x0699u
#define   MIX_SRC_TONE1           0x0004u // madera_mixer_values[1] "Tone Generator 1" [MC:1574-1576]
#define   MIX_VOL_0DB_REG         0x0080u // field 0x40 << 1 = reset default [L92T:425]
#define R_HEADPHONE_DETECT_0    0x0299u   // HPD_OVD_ENA_SEL bit15 = HP clamp on CS47L92/93 [EXT:859-918]

#define R_IRQ1_RAW_STATUS_2     0x1881u   // FLL1_LOCK_STS1 bit8
#define   FLL1_LOCK_STS           0x0100u
#define R_IRQ1_RAW_STATUS_12    0x188Bu   // HP2R_SC_STS1 bit3, HP2L_SC_STS1 bit2
#define R_IRQ1_RAW_STATUS_13    0x188Cu   // HP2R/L_ENABLE_DONE_STS1 bits 3/2
#define R_IRQ1_RAW_STATUS_14    0x188Du   // HP2R/L_DISABLE_DONE_STS1 bits 3/2
#define   HP2_BOTH                0x000Cu

//
// Level: mixer -20 dB + DAC -12 dB = -32 dBFS (notes §6.1 "Level": the tone is assumed to be a full-scale
// square wave - UNVERIFIED - so this sits ~12 dB below the -20 dBFS the task asked for).
//
#define TONE_MIX_VOL_REG        (0x2Cu << 1)   // field 0x2C = 0x40 - 20 -> -20 dB
#define TONE_DAC_VOL            0x0068u        // 0x80 - 24 half-dB steps -> -12 dB

#define TONE_HOLD_MS            2000u
#define FLL_LOCK_MAX_MS         300u           // madera_wait_for_fll gives up after ~300 ms [MC:4187-4218]
#define OUT_UP_DELAY_US         12000u         // madera_out_ev: 6 ms per CS47L92/93 output, L + R [MC:2646-2687]
#define OUT_DOWN_DELAY_US       2000u          // POST_PMD: 1 ms per output [MC:2695-2727]

//
// cs47l92_patch [L92T:22-92], applied by madera-core right after the ID check [CORE:903-910]. Verbatim,
// incl. 0x30D1 (regmap_register_patch writes it as-is; no stride check on that path).
//
STATIC CONST UINT16  mPatch16[][2] = {
  { 0x3A2, 0x2C29 }, { 0x3A3, 0x0E00 }, { 0x281, 0x0000 }, { 0x282, 0x0000 }, { 0x4EA, 0x0100 },
  { 0x22B, 0x0000 }, { 0x4A0, 0x0080 }, { 0x4A1, 0x0000 }, { 0x4A2, 0x0000 }, { 0x180B, 0x033F },
  { 0x190B, 0x033F }, { 0x442, 0x0304 }, { 0x34C, 0x0003 }, { 0x124, 0x0C49 }, { 0x120, 0x0345 },
  { 0x120, 0x0305 }, { 0x4FA, 0x5064 }, { 0x1300, 0x050E }, { 0x1302, 0x0101 }, { 0x1380, 0x02E0 },
  { 0x1381, 0xF942 }, { 0x1382, 0x04CE }, { 0x1383, 0xFF06 }, { 0x1390, 0x0304 }, { 0x1391, 0xF8FF },
  { 0x1392, 0x04F3 }, { 0x1393, 0xFF00 }, { 0x13A0, 0x02E0 }, { 0x13A1, 0xF942 }, { 0x13A2, 0x04CE },
  { 0x13A3, 0xFF06 }, { 0x13B0, 0x0304 }, { 0x13B1, 0xF8FF }, { 0x13B2, 0x04F3 }, { 0x13B3, 0xFF00 },
  { 0x412, 0x0005 }, { 0x41A, 0x0005 }, { 0x422, 0x0005 },
};

STATIC CONST UINT32  mPatch32[][2] = {
  { 0x3030, 0x04A00C01 }, { 0x3032, 0x0225F501 }, { 0x3044, 0x04A00C00 }, { 0x3046, 0x0225FF01 },
  { 0x3080, 0x04A00C01 }, { 0x3082, 0x0226F501 }, { 0x3094, 0x04A00C00 }, { 0x3096, 0x0226FF01 },
  { 0x30D1, 0x04A10C01 }, { 0x30D2, 0x0227F501 }, { 0x30E4, 0x04A10C00 }, { 0x30E6, 0x0227FF01 },
  { 0x3120, 0x04A10C01 }, { 0x3122, 0x0228F501 }, { 0x3134, 0x04A10C00 }, { 0x3136, 0x0228FF01 },
  { 0x3170, 0x04A20C01 }, { 0x3172, 0x022B0101 }, { 0x3174, 0x0229F501 }, { 0x3184, 0x04A20C00 },
  { 0x3186, 0x022B0100 }, { 0x3188, 0x0229FF01 }, { 0x31C0, 0x04A20C01 }, { 0x31C2, 0x022B0001 },
  { 0x31C4, 0x022AF501 }, { 0x31D4, 0x04A20C00 }, { 0x31D6, 0x022B0000 }, { 0x31D8, 0x022AFF01 },
};

//
// FLLHJ configurations = madera_fllhj_apply [MC:4737-4881] evaluated for fout 49.152 MHz (SYSCLK 98.304 MHz
// / 2, [MC:4971-4976]); derivation in notes §6.1/§6.2.
//
typedef struct {
  CONST CHAR8  *Name;
  UINT16       RefSrc;     // CS47L92_FLL1_REFCLK_SRC: MADERA_FLL_SRC_MCLK1 0 / MCLK2 1 [MH:31-32]
  UINT16       N;
  UINT16       Theta;
  UINT16       Lambda;
  UINT16       FbDiv;
  UINT16       RefDiv;
  UINT16       Gains;
  UINT16       Hp;
  UINT16       LockThr;
  UINT16       FastClk;
} FLL_CFG;

// MCLK2 32768 Hz (rate UNVERIFIED; a 32000 Hz ref would lock 2.3 % low with these values).
STATIC CONST FLL_CFG  mFllMclk2 = { "MCLK2 (32 kHz)", 1, 375, 0, 1, 4, 0, 0x23F0, 0, 2, 0 };
// MCLK1 26 MHz = the AP clock-out the vendor uses [DTBO cirrus,fll1-refclk, BM:350-364]; only works if
// sboot left it running - we never switch it on.
STATIC CONST FLL_CFG  mFllMclk1 = { "MCLK1 (26 MHz)", 0, 22, 0x045A, 0x0659, 1, 1, 0x21F0, 3, 8, 3 };

STATIC
UINT16
Rd (IN UINT32 Reg)
{
  UINT16  Value;

  Value = 0;
  (VOID)CodecRead (Reg, &Value);   // a failed SPI read logs as 0 (CodecRead returns the zeroed RX bytes)
  return Value;
}

// Read-modify-write + read-back, one log line. TRUE when the masked bits read back as written, ignoring
// the bits in NoVerify (write strobes such as the DAC volume-update bit, whose read-back is unknown).
STATIC
BOOLEAN
SetEx (IN CONST CHAR8 *Name, IN UINT32 Reg, IN UINT16 Mask, IN UINT16 Value, IN UINT16 NoVerify)
{
  EFI_STATUS  Status;
  UINT16      Back;
  UINT16      Check;

  Status = CodecUpdate (Reg, Mask, Value);
  Back   = Rd (Reg);
  Check  = (UINT16)(Mask & ~NoVerify);
  DEBUG ((DEBUG_WARN, "AudioTone: %a [%x] mask %x <- %x : read %x %a\n", Name, Reg, Mask, Value & Mask, Back,
          EFI_ERROR (Status) ? "SPI ERROR" : (((Back & Check) == (Value & Check)) ? "ok" : "MISMATCH")));
  return !EFI_ERROR (Status) && ((Back & Check) == (Value & Check));
}

STATIC
BOOLEAN
Set (IN CONST CHAR8 *Name, IN UINT32 Reg, IN UINT16 Mask, IN UINT16 Value)
{
  return SetEx (Name, Reg, Mask, Value, 0);
}

STATIC
VOID
LogStatus (IN CONST CHAR8 *When)
{
  UINT16  Raw2;

  Raw2 = Rd (R_IRQ1_RAW_STATUS_2);
  DEBUG ((DEBUG_WARN, "AudioTone: status %a: FLL1 lock %u (RAW2 %x) SYSCLK1 %x OUT_EN1 %x RAW_OUT_STS1 %x TONE1 %x"
                      " HP2 SC %x EN_DONE %x DIS_DONE %x (bits 3:2 = R:L) HPD0 %x\n",
          When, (Raw2 & FLL1_LOCK_STS) ? 1 : 0, Raw2, Rd (R_SYSTEM_CLOCK_1), Rd (R_OUTPUT_ENABLES_1),
          Rd (R_RAW_OUTPUT_STATUS_1), Rd (R_TONE_GENERATOR_1), Rd (R_IRQ1_RAW_STATUS_12) & HP2_BOTH,
          Rd (R_IRQ1_RAW_STATUS_13) & HP2_BOTH, Rd (R_IRQ1_RAW_STATUS_14) & HP2_BOTH, Rd (R_HEADPHONE_DETECT_0)));
}

// cs47l92_patch with a read-back after every write. Returns FALSE only if no write stuck at all (= SPI
// writes do not work; continuing would be blind). Single mismatches are logged (self-clearing or
// write-only bits are possible) and tolerated.
STATIC
BOOLEAN
ApplyPatch (VOID)
{
  UINTN       Index;
  UINTN       Bad16;
  UINTN       Bad32;
  UINT16      Back16;
  UINT32      Back32;
  EFI_STATUS  Status;

  Bad16 = 0;
  for (Index = 0; Index < ARRAY_SIZE (mPatch16); Index++) {
    Status = CodecWrite (mPatch16[Index][0], mPatch16[Index][1]);
    Back16 = Rd (mPatch16[Index][0]);
    if (EFI_ERROR (Status) || (Back16 != mPatch16[Index][1])) {
      Bad16++;
      DEBUG ((DEBUG_WARN, "AudioTone: patch16 [%x] <- %x read %x (%r)\n", mPatch16[Index][0], mPatch16[Index][1], Back16, Status));
    }
  }

  Bad32 = 0;
  for (Index = 0; Index < ARRAY_SIZE (mPatch32); Index++) {
    Back32 = 0;
    Status = CodecWrite32 (mPatch32[Index][0], mPatch32[Index][1]);
    (VOID)CodecRead32 (mPatch32[Index][0], &Back32);
    if (EFI_ERROR (Status) || (Back32 != mPatch32[Index][1])) {
      Bad32++;
      DEBUG ((DEBUG_WARN, "AudioTone: patch32 [%x] <- %x read %x (%r)\n", mPatch32[Index][0], mPatch32[Index][1], Back32, Status));
    }
  }

  DEBUG ((DEBUG_WARN, "AudioTone: cs47l92 patch applied: 16-bit %u/%u read back exact, 32-bit %u/%u\n",
          (UINT32)(ARRAY_SIZE (mPatch16) - Bad16), (UINT32)ARRAY_SIZE (mPatch16), (UINT32)(ARRAY_SIZE (mPatch32) - Bad32),
          (UINT32)ARRAY_SIZE (mPatch32)));
  return (Bad16 < ARRAY_SIZE (mPatch16)) || (Bad32 < ARRAY_SIZE (mPatch32));
}

// madera_fllhj_disable [MC:4695-4735].
STATIC
VOID
FllDisable (VOID)
{
  UINTN  Ms;

  (VOID)CodecUpdate (R_FLL1_CONTROL_11, FLL_LOCKDET, 0);
  (VOID)CodecUpdate (R_FLL1_CONTROL_1, FLL_HOLD, FLL_HOLD);
  (VOID)CodecUpdate (R_FLL1_CONTROL_1, FLL_ENA, 0);
  for (Ms = 0; (Ms < FLL_LOCK_MAX_MS) && ((Rd (R_IRQ1_RAW_STATUS_2) & FLL1_LOCK_STS) != 0); Ms++) {
    gBS->Stall (1000);
  }

  (VOID)CodecUpdate (R_FLL1_CONTROL_2, FLL_CTRL_UPD, 0);
  DEBUG ((DEBUG_WARN, "AudioTone: FLL1 disabled after %u ms: CTRL1 %x CTRL2 %x CTRL11 %x RAW2 %x\n", (UINT32)Ms,
          Rd (R_FLL1_CONTROL_1), Rd (R_FLL1_CONTROL_2), Rd (R_FLL1_CONTROL_11), Rd (R_IRQ1_RAW_STATUS_2)));
}

// madera_fllhj_enable + madera_fllhj_apply [MC:4737-4942] + madera_wait_for_fll [MC:4187-4218]
// (polled every 1 ms instead of the kernel's 75 us .. 20 ms ladder). TRUE = FLL1_LOCK_STS1 set.
STATIC
BOOLEAN
FllEnable (IN CONST FLL_CFG *Cfg)
{
  UINTN    Ms;
  BOOLEAN  Locked;

  (VOID)CodecUpdate (R_FLL1_CONTROL_1, FLL_HOLD, FLL_HOLD);       // "HOLD must be set before configuring"
  (VOID)CodecWrite (R_FLL1_CONTROL_2, Cfg->N);                    // plain write: N, CTRL_UPD cleared
  (VOID)CodecUpdate (R_FLL1_CONTROL_3, 0xFFFF, Cfg->Theta);
  (VOID)CodecUpdate (R_FLL1_CONTROL_4, 0xFFFF, Cfg->Lambda);
  (VOID)CodecUpdate (R_FLL1_CONTROL_5, FLL_FB_DIV_MASK, Cfg->FbDiv);
  (VOID)CodecUpdate (R_FLL1_CONTROL_6, FLL_REFCLK_DIV_MASK, (UINT16)(Cfg->RefDiv << FLL_REFCLK_DIV_SHIFT));
  (VOID)CodecUpdate (R_FLL1_CONTROL_8, 0xFFFF, Cfg->Gains);
  (VOID)CodecUpdate (R_FLL1_CONTROL_10, FLL_HP_MASK | FLL_PHASEDET_ENA, (UINT16)((Cfg->Hp << FLL_HP_SHIFT) | FLL_PHASEDET_ENA));
  (VOID)CodecUpdate (R_FLL1_CONTROL_11, FLL_LOCKDET_THR_MASK, (UINT16)(Cfg->LockThr << FLL_LOCKDET_THR_SHIFT));
  (VOID)CodecUpdate (R_FLL1_DIGITAL_TEST_1, FLL_DT1_MASK, Cfg->FastClk);
  (VOID)CodecUpdate (R_FLL1_CONTROL_1, FLL_REFCLK_SRC_MASK, (UINT16)(Cfg->RefSrc << FLL_REFCLK_SRC_SHIFT));
  (VOID)CodecUpdate (R_FLL1_CONTROL_1, FLL_ENA, FLL_ENA);
  (VOID)CodecUpdate (R_FLL1_CONTROL_11, FLL_LOCKDET, FLL_LOCKDET);
  (VOID)CodecUpdate (R_FLL1_CONTROL_2, FLL_CTRL_UPD, FLL_CTRL_UPD);
  (VOID)CodecUpdate (R_FLL1_CONTROL_1, FLL_HOLD, 0);              // release: lock to the reference

  DEBUG ((DEBUG_WARN, "AudioTone: FLL1 <- %a programmed: CTRL1 %x CTRL2 %x THETA %x LAMBDA %x CTRL5 %x CTRL6 %x GAIN %x"
                      " CTRL10 %x CTRL11 %x DT1 %x (want N %x TH %x LA %x FB %x REFDIV %u GAIN %x HP %u THR %u FAST %u)\n",
          Cfg->Name, Rd (R_FLL1_CONTROL_1), Rd (R_FLL1_CONTROL_2), Rd (R_FLL1_CONTROL_3), Rd (R_FLL1_CONTROL_4),
          Rd (R_FLL1_CONTROL_5), Rd (R_FLL1_CONTROL_6), Rd (R_FLL1_CONTROL_8), Rd (R_FLL1_CONTROL_10),
          Rd (R_FLL1_CONTROL_11), Rd (R_FLL1_DIGITAL_TEST_1), Cfg->N, Cfg->Theta, Cfg->Lambda, Cfg->FbDiv, Cfg->RefDiv,
          Cfg->Gains, Cfg->Hp, Cfg->LockThr, Cfg->FastClk));

  Locked = FALSE;
  for (Ms = 0; Ms <= FLL_LOCK_MAX_MS; Ms++) {
    if ((Rd (R_IRQ1_RAW_STATUS_2) & FLL1_LOCK_STS) != 0) {
      Locked = TRUE;
      break;
    }

    gBS->Stall (1000);
  }

  DEBUG ((DEBUG_WARN, "AudioTone: FLL1 <- %a %a after %u ms (IRQ1_RAW_STATUS_2 %x)\n", Cfg->Name,
          Locked ? "LOCKED" : "NO LOCK", (UINT32)Ms, Rd (R_IRQ1_RAW_STATUS_2)));
  return Locked;
}

BOOLEAN
CodecTonePlay (VOID)
{
  BOOLEAN  Locked;
  BOOLEAN  Shorted;
  BOOLEAN  Played;
  UINTN    Index;
  UINT16   Value;

  DEBUG ((DEBUG_WARN, "AudioTone: start - tone1 -> OUT2 mixers -> HPOUT2 (3.5 mm jack) for %u ms at -32 dBFS"
                      " (mixer -20 dB, DAC -12 dB)\n", TONE_HOLD_MS));
  LogStatus ("at reset");

  // 1. madera-core probe: hardware patch [CORE:903-910], 32 kHz clock from MCLK2 [CORE:917-921].
  if (!ApplyPatch ()) {
    DEBUG ((DEBUG_ERROR, "AudioTone: no patch write read back - SPI writes do not work, abort (nothing enabled)\n"));
    return FALSE;
  }

  (VOID)Set ("CLOCK_32K_1 ena + MCLK2", R_CLOCK_32K_1, CLK_32K_ENA | CLK_32K_SRC_MASK, CLK_32K_ENA | CLK_32K_SRC_MCLK2);

  // 2. Machine driver init + hw_params: SYSCLK = FLL1 98.304 MHz (not yet enabled), 48 kHz rate.
  (VOID)Set ("SAMPLE_RATE_1 48k", R_SAMPLE_RATE_1, 0x001F, SAMPLE_RATE_48K);
  (VOID)Set ("SYSTEM_CLOCK_1 98M FLL1", R_SYSTEM_CLOCK_1, SYSCLK_SET_MASK, SYSCLK_98MHZ_FLL1);

  // 3. FLL1 - MCLK2 first (no AP clock), MCLK1 only if MCLK2 does not lock.
  Locked = FllEnable (&mFllMclk2);
  if (!Locked) {
    FllDisable ();
    Locked = FllEnable (&mFllMclk1);
  }

  if (!Locked) {
    DEBUG ((DEBUG_ERROR, "AudioTone: FLL1 locked on neither MCLK2 nor MCLK1 - no SYSCLK, abort (outputs never enabled)\n"));
    FllDisable ();
    return FALSE;
  }

  // 4. DAPM "SYSCLK" supply + madera_sysclk_ev / madera_spin_sysclk [MC:174-209].
  (VOID)Set ("SYSCLK_ENA", R_SYSTEM_CLOCK_1, SYSCLK_ENA, SYSCLK_ENA);
  for (Index = 0; Index < 4; Index++) {
    (VOID)Rd (R_SOFTWARE_RESET);
  }

  gBS->Stall (300);

  // 5. Path (the user-visible controls the kernel sets before power-up): mixer sources + volumes, DAC
  //    volume unmuted (VU latches L+R), then the tone generator PGA.
  (VOID)Set ("OUT2LMIX in1 = tone1", R_OUT2LMIX_INPUT_1_SRC, 0x00FF, MIX_SRC_TONE1);
  (VOID)Set ("OUT2RMIX in1 = tone1", R_OUT2RMIX_INPUT_1_SRC, 0x00FF, MIX_SRC_TONE1);
  (VOID)Set ("OUT2LMIX in1 vol -20dB", R_OUT2LMIX_INPUT_1_VOL, 0x00FE, TONE_MIX_VOL_REG);
  (VOID)Set ("OUT2RMIX in1 vol -20dB", R_OUT2RMIX_INPUT_1_VOL, 0x00FE, TONE_MIX_VOL_REG);
  (VOID)SetEx ("DAC2L -12dB unmute", R_DAC_DIGITAL_VOLUME_2L, DAC_VU | DAC_MUTE | 0x00FF, DAC_VU | TONE_DAC_VOL, DAC_VU);
  (VOID)SetEx ("DAC2R -12dB unmute", R_DAC_DIGITAL_VOLUME_2R, DAC_VU | DAC_MUTE | 0x00FF, DAC_VU | TONE_DAC_VOL, DAC_VU);
  (VOID)Set ("TONE1_ENA (rate = SAMPLE_RATE_1)", R_TONE_GENERATOR_1, 0xFFFF, TONE1_ENA);

  // 6. OUT2L/OUT2R: madera_hp_ev POST_PMU sets the enable bits, madera_out_ev waits 6 ms per output.
  Value = Rd (R_HEADPHONE_DETECT_0);
  if ((Value & 0x8000) != 0) {
    DEBUG ((DEBUG_WARN, "AudioTone: HP clamp set (HEADPHONE_DETECT_0 %x) - the kernel would keep the outputs off\n", Value));
  }

  Played = FALSE;
  Shorted = FALSE;
  if ((Value & 0x8000) == 0) {
    (VOID)Set ("OUT2L+OUT2R ENA", R_OUTPUT_ENABLES_1, OUT2_ENA_BOTH, OUT2_ENA_BOTH);
    gBS->Stall (OUT_UP_DELAY_US);
    LogStatus ("outputs on (+12 ms)");
    Shorted = (Rd (R_IRQ1_RAW_STATUS_12) & HP2_BOTH) != 0;
    if (Shorted) {
      DEBUG ((DEBUG_ERROR, "AudioTone: HP2 short-circuit status set - outputs off at once\n"));
    } else {
      // 7. Hold. Half-way status read proves the clock + outputs stayed up while the tone played.
      for (Index = 0; Index < TONE_HOLD_MS / 100; Index++) {
        if (Index == (TONE_HOLD_MS / 200)) {
          LogStatus ("mid-tone");
        }

        gBS->Stall (100 * 1000);
      }

      Played = TRUE;
    }
  }

  // 8. Teardown in DAPM order: outputs (PRE_PMD clear + 1 ms/output), tone PGA, SYSCLK supply, then the
  //    machine driver's BIAS_OFF set_pll(0) = fllhj_disable. Controls back to their reset defaults.
  (VOID)Set ("OUT2L+OUT2R off", R_OUTPUT_ENABLES_1, OUT2_ENA_BOTH, 0);
  gBS->Stall (OUT_DOWN_DELAY_US);
  LogStatus ("outputs off (+2 ms)");
  (VOID)Set ("TONE1 off", R_TONE_GENERATOR_1, 0xFFFF, 0);
  (VOID)SetEx ("DAC2L mute (default)", R_DAC_DIGITAL_VOLUME_2L, DAC_VU | DAC_MUTE | 0x00FF, DAC_VU | DAC_MUTE | DAC_VOL_0DB, DAC_VU);
  (VOID)SetEx ("DAC2R mute (default)", R_DAC_DIGITAL_VOLUME_2R, DAC_VU | DAC_MUTE | 0x00FF, DAC_VU | DAC_MUTE | DAC_VOL_0DB, DAC_VU);
  (VOID)Set ("OUT2LMIX in1 none", R_OUT2LMIX_INPUT_1_SRC, 0x00FF, 0);
  (VOID)Set ("OUT2RMIX in1 none", R_OUT2RMIX_INPUT_1_SRC, 0x00FF, 0);
  (VOID)Set ("OUT2LMIX in1 vol 0dB (default)", R_OUT2LMIX_INPUT_1_VOL, 0x00FE, MIX_VOL_0DB_REG);
  (VOID)Set ("OUT2RMIX in1 vol 0dB (default)", R_OUT2RMIX_INPUT_1_VOL, 0x00FE, MIX_VOL_0DB_REG);
  (VOID)Set ("SYSCLK off", R_SYSTEM_CLOCK_1, SYSCLK_ENA, 0);
  FllDisable ();
  LogStatus ("after teardown");
  DEBUG ((DEBUG_WARN, "AudioTone: done - tone %a; codec goes back into hard reset next\n",
          Played ? "PLAYED (listen: ~2 s, both ears; pitch UNVERIFIED, expected ~1 kHz)" : (Shorted ? "STOPPED (HP2 short)" : "NOT PLAYED (HP clamp)")));
  return Played;
}
