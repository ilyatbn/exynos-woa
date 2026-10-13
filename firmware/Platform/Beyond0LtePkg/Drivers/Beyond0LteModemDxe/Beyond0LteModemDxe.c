/** @file
  Beyond0LteModemDxe — D9 WWAN firmware half, first slice: OBSERVE the
  Exynos 9820's integrated CP (Samsung project S500AP) and its AP<->CP
  mailbox. Prints the state this firmware inherited from sboot (design rung
  R1's pre-read, risk P2) and the mailbox contents (rung R2's observation
  half). Every window and bit is a VERIFIED read — notes/modem-wwan.md
  T1/T6/T7 and the WWAN block of Exynos9820.h.

  BEYOND0LTE_MODEM_INIT defaults to 0: the AP2CP_CFG=1 write (what Android's
  own modem_ctrl_s5000ap.c:333-342 does in s5000ap_on) is compiled OUT until
  the boot stream lands and the tester opts in — DECISION, see
  docs/exynos9820-port/D9-MODEM-DESIGN.md section 7. Flipping it to 1 is
  rung R1 proper.

  NOTHING HERE IS VALIDATED ON AN S10e.
**/

#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Platform/Exynos9820.h>

#define BEYOND0LTE_MODEM_INIT  0

//
// MCU_IPC register offsets inside the mailbox window — VERIFIED,
// mcu_ipc/regs-mcu_ipc.h:18-32 (notes/modem-wwan.md T6).
// Value slots: ISSR0 ap2cp_msg, ISSR1 cp2ap_msg, ISSR2 united_status(ap),
// ISSR3 united_status(cp) — slot map from the mif,*_mbx_* DT ids (T1/T6).
//
#define MBOX_INTSR0  0x14
#define MBOX_ISSR0   0x80

//
// SBI "united status" field — VERIFIED from the dtbo mif_pdata property
// sbi_cp_status_mask 0xf @ pos 0x1 (notes/modem-wwan.md T1).
//
#define SBI_CP_STATUS_MASK  0xF
#define SBI_CP_STATUS_POS   1

EFI_STATUS
EFIAPI
Beyond0LteModemEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Cfg;
  UINT32  Status;
  UINT32  CpIn;
  UINT32  Issr[4];
  UINT32  Intsr;
  UINTN   Try;

  //
  // R1 pre-read: what did sboot leave? Android brings the CP up via the
  // kernel (SMC + AP2CP_CFG) and cbd downloads its image (T4/T7), so
  // "all zero" here is the expected state; a set CP_STATUS would mean
  // sboot pre-booted the CP and our sequencing shrinks (risk P2).
  // PMU window is mapped by PlatformMemoryMapLib ("PMU").
  //
  Cfg    = MmioRead32 (EXYNOS_PMU_BASE + EXYNOS_PMU_CP_CONFIGURATION);
  Status = MmioRead32 (EXYNOS_PMU_BASE + EXYNOS_PMU_CP_STATUS);
  CpIn   = MmioRead32 (EXYNOS_PMU_BASE + EXYNOS_PMU_CP_IN);
  DEBUG ((DEBUG_INFO,
    "[CP] R1 pre: CONFIGURATION=%x STATUS=%x IN=%x\n",
    Cfg, Status, CpIn));

#if BEYOND0LTE_MODEM_INIT
  //
  // s5000ap_on first step (modem_ctrl_s5000ap.c:333-342): write 1 to
  // AP2CP_CFG, read back, expect 1. Compiled out by default (DECISION).
  //
  MmioWrite32 (EXYNOS_AP2CP_CFG_BASE, 1);
  Cfg = MmioRead32 (EXYNOS_AP2CP_CFG_BASE);
  DEBUG ((DEBUG_INFO, "[CP] R1 ap2cp_cfg readback=%x\n", Cfg));
#endif

  //
  // R2 observation: mailbox contents (window mapped as "MBOX_CP"). With the
  // CP off these are expected to read 0 — Android itself zeroes them at
  // modem_on (init_mailbox_regs, T7 step 1); a nonzero ISSR3 cp_status
  // would mean a live CP already talking.
  //
  for (Try = 0; Try < 4; Try++) {
    Issr[Try] = MmioRead32 (EXYNOS_MBOX_CP_BASE + MBOX_ISSR0 + (Try * 4));
  }

  Intsr = MmioRead32 (EXYNOS_MBOX_CP_BASE + MBOX_INTSR0);
  DEBUG ((DEBUG_INFO,
    "[MBOX] R2: issr0..3=%x %x %x %x intsr0=%x\n",
    Issr[0], Issr[1], Issr[2], Issr[3], Intsr));
  DEBUG ((DEBUG_INFO, "[CP] R1: cp_status=%x\n",
    (Issr[3] >> SBI_CP_STATUS_POS) & SBI_CP_STATUS_MASK));

  return EFI_SUCCESS;
}
