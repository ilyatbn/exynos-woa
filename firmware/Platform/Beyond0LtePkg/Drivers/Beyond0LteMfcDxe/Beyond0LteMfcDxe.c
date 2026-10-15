/** @file
  D10 video (MFC) UEFI probe (notes/d10-mfc-main.md). Constants from Exynos9820.h.

  R1 (BEYOND0LTE_MFC_PROBE): read PMU MFC_STATUS; only if pd-mfc is on, read CMU_MFC (mux / ACLK gate / QCH) and the
  MFC version register (0xF028, expect ip_ver 0x1300). Zero writes.
  R2 (BEYOND0LTE_MFC_POWER): if pd-mfc is off, power it on the way the vendor PMU table does (MFC_CONFIGURATION bit0,
  wait MFC_STATUS bit0), then the R1 reads. The vendor also issues an SMC (DTZPC restore, need_smc 0x18820204) after
  power-on that this does NOT do - if the version reads 0 / faults, that is the reason to look at first.

  Guard: progress word [1] = 0x6C while it runs; a boot whose previous boot stopped at 0x6C skips it.
  NOTHING HERE IS VALIDATED ON AN S10e YET.
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Platform/Exynos9820.h>

#ifndef BEYOND0LTE_MFC_PROBE
#define BEYOND0LTE_MFC_PROBE  0
#endif
#ifndef BEYOND0LTE_MFC_POWER
#define BEYOND0LTE_MFC_POWER  0
#endif
//
// R3 (BEYOND0LTE_MFC_FWBOOT): boot the MFC RISC from mfc_fw.bin (embedded in the FV only by test builds: FDF
// MFC_FW_EMBED, blob copied from the phone's /vendor/firmware into a gitignored folder - never committed). Sequence =
// vendor __mfc_init_hw (mfc_run.c:30-70): reset (clear R2H/H2R/FW_VERSION + 1024 words from 0xF000, MFC_RESET 0x1FFF
// then 0), RISC_BASE_ADDRESS = firmware buffer address (SYSMMUs in their reset bypass state: device address =
// physical, so the buffer is below 4 GiB), RISC_ON 1 + MFC_OFF 0, wait R2H FW_STATUS_RET (12). Then the MFC is put
// back into reset so Windows finds it quiet.
//
#ifndef BEYOND0LTE_MFC_FWBOOT
#define BEYOND0LTE_MFC_FWBOOT  0
#endif
#define MFC_REG_RISC2HOST_INT        0x003Cu   // mfc_regs.h:25
#define MFC_REG_HOST2RISC_INT        0x0044u   // mfc_regs.h:26
#define MFC_REG_RISC_BASE_ADDRESS    0x0054u   // mfc_regs.h:27
#define MFC_REG_HOST2RISC_CMD        0x1100u   // mfc_regs.h:32
#define MFC_REG_RISC2HOST_CMD        0x1104u   // mfc_regs.h:33
#define MFC_REG_MFC_OFF              0x7120u   // mfc_regs.h:39
#define MFC_REG_FW_VERSION           0xF000u   // mfc_regs.h:42
#define MFC_REG_REG_CLEAR_BEGIN      0xF000u   // mfc_regs.h:491
#define MFC_REG_REG_CLEAR_COUNT      1024u     // mfc_regs.h:492
#define MFC_R2H_FW_STATUS_RET        12u       // mfc_regs.h:526
#define MFC_REG_CONTEXT_MEM_ADDR     0xF014u   // mfc_regs.h:45
#define MFC_REG_CONTEXT_MEM_SIZE     0xF018u   // mfc_regs.h:46
#define MFC_H2R_CMD_SYS_INIT         1u        // mfc_regs.h:498
#define MFC_R2H_SYS_INIT_RET         1u        // mfc_regs.h:517
#define MFC_DEV_CTX_SIZE             0x8000u   // mfc.c mfc_ctx_buf_size.dev_ctx = PAGE_ALIGN(0x7800)
#define MFC_CTX_OFFSET               0x400000u // common context buffer: 4 MiB into the 8 MiB firmware buffer
#define MFC_FW_BUFFER_PAGES          2048u     // 8 MiB: image (688 KiB) + room for the firmware's own working memory
#define MFC_FW_FILE_GUID  { 0x5d7c1b1e, 0x93a6, 0x4f1e, { 0x9a, 0x3c, 0x6a, 0x0e, 0x21, 0x7b, 0x4d, 0x10 } }

#define MFC_PROGRESS_TRYING  0x6Cu
#define MFC_DSDT_WORD        13u           // progress word [13] (0x8FE0F034) = 'VIDC' -> DSDT VIDC present
#define MFC_DSDT_PRESENT     0x43444956u   // 'VIDC'
STATIC volatile UINT32  *CONST  mProgress = (volatile UINT32 *)(UINTN)0x8FE0F000ULL;

STATIC
BOOLEAN
MfcPowered (
  VOID
  )
{
  return (MmioRead32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_MFC_STATUS) & 0x1) != 0;
}

STATIC
VOID
MfcReadIds (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "[MFC] CMU_MFC: mfc user mux %08x, mfc aclk gate %08x, mfc qch %08x\n",
          MmioRead32 ((UINTN)EXYNOS_CMU_MFC_BASE + EXYNOS_CMU_MFC_MFC_USER_MUX),
          MmioRead32 ((UINTN)EXYNOS_CMU_MFC_BASE + EXYNOS_CMU_MFC_MFC_ACLK),
          MmioRead32 ((UINTN)EXYNOS_CMU_MFC_BASE + EXYNOS_CMU_MFC_MFC_QCH)));
  DEBUG ((DEBUG_ERROR, "[MFC] R1: version=0x%x (ip_ver 0x%x expected), risc_on %x\n",
          MmioRead32 ((UINTN)EXYNOS_MFC_BASE + MFC_REG_MFC_VERSION), EXYNOS_MFC_IP_VER,
          MmioRead32 ((UINTN)EXYNOS_MFC_BASE + MFC_REG_RISC_ON)));
}

STATIC
VOID
MfcReset (
  VOID
  )
{
  UINTN  Mfc;
  UINTN  Index;

  Mfc = (UINTN)EXYNOS_MFC_BASE;
  MmioWrite32 (Mfc + MFC_REG_RISC2HOST_CMD, 0);
  MmioWrite32 (Mfc + MFC_REG_HOST2RISC_CMD, 0);
  MmioWrite32 (Mfc + MFC_REG_FW_VERSION, 0);
  for (Index = 0; Index < MFC_REG_REG_CLEAR_COUNT; Index++) {
    MmioWrite32 (Mfc + MFC_REG_REG_CLEAR_BEGIN + Index * 4, 0);
  }

  MmioWrite32 (Mfc + MFC_REG_MFC_RESET, 0x1FFF);
  MmioWrite32 (Mfc + MFC_REG_MFC_RESET, 0);
}

/** R3: boot the RISC from the embedded mfc_fw.bin and wait for FW_STATUS_RET (12). **/
STATIC
VOID
MfcFwBoot (
  VOID
  )
{
  STATIC CONST EFI_GUID  FwGuid = MFC_FW_FILE_GUID;
  VOID                   *Fw;
  UINTN                  FwSize;
  EFI_PHYSICAL_ADDRESS   Buf;
  UINTN                  Mfc;
  UINTN                  Wait;
  UINT32                 R2h;
  EFI_STATUS             Status;

  Fw     = NULL;
  FwSize = 0;
  Status = GetSectionFromAnyFv (&FwGuid, EFI_SECTION_RAW, 0, &Fw, &FwSize);
  if (EFI_ERROR (Status) || (FwSize == 0)) {
    DEBUG ((DEBUG_ERROR, "[MFC] R3: mfc_fw.bin not embedded in this build (%r) - skipped\n", Status));
    return;
  }

  Buf    = SIZE_4GB - 1;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, MFC_FW_BUFFER_PAGES + 256, &Buf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[MFC] R3: no buffer below 4 GiB (%r)\n", Status));
    return;
  }

  Buf = ALIGN_VALUE (Buf, SIZE_1MB);                       // 256 spare pages cover the 1 MiB alignment
  ZeroMem ((VOID *)(UINTN)Buf, EFI_PAGES_TO_SIZE (MFC_FW_BUFFER_PAGES));
  CopyMem ((VOID *)(UINTN)Buf, Fw, FwSize);
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Buf, EFI_PAGES_TO_SIZE (MFC_FW_BUFFER_PAGES));
  FreePool (Fw);

  Mfc = (UINTN)EXYNOS_MFC_BASE;
  MfcReset ();
  MmioWrite32 (Mfc + MFC_REG_RISC_BASE_ADDRESS, (UINT32)Buf);
  MmioWrite32 (Mfc + MFC_REG_RISC2HOST_CMD, 0);           // mfc_clean_dev_int_flags
  MmioWrite32 (Mfc + MFC_REG_RISC2HOST_INT, 0);
  MmioWrite32 (Mfc + MFC_REG_RISC_ON, 1);
  MmioWrite32 (Mfc + MFC_REG_MFC_OFF, 0);
  R2h = 0;
  for (Wait = 0; Wait < 2000; Wait++) {                    // up to 2 s
    if (MmioRead32 (Mfc + MFC_REG_RISC2HOST_INT) != 0) {
      R2h = MmioRead32 (Mfc + MFC_REG_RISC2HOST_CMD);
      break;
    }

    MicroSecondDelay (1000);
  }

  DEBUG ((DEBUG_ERROR, "[MFC] R3: fw %u B at %lx, base reg %x; R2H after %u ms: int %x cmd %u (FW_STATUS_RET %u expected)\n",
          (UINT32)FwSize, Buf, MmioRead32 (Mfc + MFC_REG_RISC_BASE_ADDRESS), (UINT32)Wait,
          MmioRead32 (Mfc + MFC_REG_RISC2HOST_INT), R2h, MFC_R2H_FW_STATUS_RET));
  DEBUG ((DEBUG_ERROR, "[MFC] R3: FW_VERSION %08x MFC_VERSION %08x RISC_ON %x\n", MmioRead32 (Mfc + MFC_REG_FW_VERSION),
          MmioRead32 (Mfc + MFC_REG_MFC_VERSION), MmioRead32 (Mfc + MFC_REG_RISC_ON)));

  //
  // R4: SYS_INIT (vendor mfc_cmd_sys_init + mfc_cmd_host2risc): CONTEXT_MEM_ADDR/SIZE = the common context buffer
  // (dev_ctx = PAGE_ALIGN(0x7800) = 0x8000, mfc.c mfc_ctx_buf_size), clean R2H, H2R cmd 1 + H2R int, wait R2H 1.
  //
  if (R2h == MFC_R2H_FW_STATUS_RET) {
    MmioWrite32 (Mfc + MFC_REG_RISC2HOST_CMD, 0);
    MmioWrite32 (Mfc + MFC_REG_RISC2HOST_INT, 0);
    MmioWrite32 (Mfc + MFC_REG_CONTEXT_MEM_ADDR, (UINT32)(Buf + MFC_CTX_OFFSET));
    MmioWrite32 (Mfc + MFC_REG_CONTEXT_MEM_SIZE, MFC_DEV_CTX_SIZE);
    MmioWrite32 (Mfc + MFC_REG_HOST2RISC_CMD, MFC_H2R_CMD_SYS_INIT);
    MmioWrite32 (Mfc + MFC_REG_HOST2RISC_INT, 1);
    R2h = 0;
    for (Wait = 0; Wait < 1000; Wait++) {
      if (MmioRead32 (Mfc + MFC_REG_RISC2HOST_INT) != 0) {
        R2h = MmioRead32 (Mfc + MFC_REG_RISC2HOST_CMD);
        break;
      }

      MicroSecondDelay (1000);
    }

    DEBUG ((DEBUG_ERROR, "[MFC] R4: SYS_INIT ctx %lx + %x -> R2H after %u ms: cmd %u (SYS_INIT_RET %u expected); FW_VERSION %08x\n",
            Buf + MFC_CTX_OFFSET, MFC_DEV_CTX_SIZE, (UINT32)Wait, R2h, MFC_R2H_SYS_INIT_RET,
            MmioRead32 (Mfc + MFC_REG_FW_VERSION)));
  }

  MmioWrite32 (Mfc + MFC_REG_RISC_ON, 0);                  // quiet again for Windows
  MfcReset ();
}

EFI_STATUS
EFIAPI
Beyond0LteMfcEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Saved;
  UINTN   Wait;

  //
  // DSDT VIDC._STA reads progress word [13]: 'VIDC' only when THIS boot saw pd-mfc on, so the Windows driver never
  // touches an unpowered MFC (a value surviving a warm reset is cleared here first).
  //
  mProgress[MFC_DSDT_WORD] = 0;
  if (!BEYOND0LTE_MFC_PROBE && !BEYOND0LTE_MFC_POWER && !BEYOND0LTE_MFC_FWBOOT) {
    if (MfcPowered ()) {
      mProgress[MFC_DSDT_WORD] = MFC_DSDT_PRESENT;
    }

    return EFI_SUCCESS;
  }

  if (mProgress[0] == MFC_PROGRESS_TRYING) {
    DEBUG ((DEBUG_ERROR, "[MFC] previous boot hung in the MFC probe (progress 0x6C) - skipped this boot\n"));
    return EFI_SUCCESS;
  }

  Saved        = mProgress[1];
  mProgress[1] = MFC_PROGRESS_TRYING;
  DEBUG ((DEBUG_ERROR, "[MFC] pd-mfc: configuration %08x status %08x\n",
          MmioRead32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_MFC_CONFIGURATION),
          MmioRead32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_MFC_STATUS)));

  if (!MfcPowered () && BEYOND0LTE_MFC_POWER) {
    MmioOr32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_MFC_CONFIGURATION, 0x1);
    for (Wait = 0; (Wait < 1000) && !MfcPowered (); Wait++) {
      MicroSecondDelay (100);
    }

    DEBUG ((DEBUG_ERROR, "[MFC] R2: power-on -> status %08x after %u x 100 us\n",
            MmioRead32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_MFC_STATUS), (UINT32)Wait));
  }

  if (MfcPowered ()) {
    MfcReadIds ();
    if (BEYOND0LTE_MFC_FWBOOT) {
      MfcFwBoot ();
    }
  } else {
    DEBUG ((DEBUG_ERROR, "[MFC] pd-mfc is off - no CMU_MFC / MFC reads (R2 powers it on)\n"));
  }

  if (MfcPowered ()) {
    mProgress[MFC_DSDT_WORD] = MFC_DSDT_PRESENT;
  }

  mProgress[1] = Saved;
  return EFI_SUCCESS;
}
