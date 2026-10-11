/** @file
  Beyond0LtePcieDxe — bring the S10e's PCIe root port 0 (the WLAN bus) up in
  UEFI: clocks, PHY, PERST#, link training, iATU windows, and the endpoint's
  BARs + COMMAND. This is the firmware half of M12/D4b (design:
  docs/exynos9820-port/M12-WIFI-DESIGN.md §3); WiFi milestone 1 (rung R1) is the
  VID/DID + chip-ID print below (notes/m12-wifi-m1.md). The state this driver
  latches (link, iATU, BARs, COMMAND) is what the Windows probe driver
  (drivers/Exynos9820Wlan) inherits through ExitBootServices, exactly the
  role-latch contract already decided for xHCI in D3.

  It deliberately does NOT register an EFI PCI root bridge: the single CFG0
  iATU window cannot back a 2-bus ECAM contract (design §2), and the Windows
  half binds an ACPI-described device instead.

  Safety (same self-healing shape as Beyond0LteUsbHostDxe's 0x67 guard):
   - BEYOND0LTE_PCIE_WLAN 0 compiles the bring-up out of the boot path.
   - Progress page 0x8FE0F000 (WINPRAM, uncached, survives a warm reset):
     [0] = previous boot's last code, [1] = this boot's code. [1] = 0x68 while the
     bring-up runs; the CMU_FSYS1 / ELBI / PHY windows are clocked by ACPM-owned
     gates, so the first access may hang the bus instead of returning a value.
     If that happens the watchdog resets, the next boot sees [0] == 0x68 and
     skips WLAN — the boot after that tries again.
   - [3] (0x8FE0F00C) = 'WLAN' only after link-up + a Broadcom VID read; it is
     zeroed in SEC (ArmPlatformLib.c) and again here, and DSDT WLAN._STA is
     present only on that value, so Windows never sees a dead controller.
   - Link-up failure is cheap: PCIE_LINK_MAX_TRIES tries, each logged, total
     bounded (PcieHw.h), and the boot continues either way.

  FILE_GUID 11111111-2222-3333-4444-555555555553 is this module's BUILD
  IDENTITY, not a hardware read. Nothing here is validated on an S10e yet.
**/

#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Platform/Exynos9820.h>
#include "PcieInternal.h"

// 1 = run the PCIe0/WLAN bring-up (WiFi milestone 1 test image), 0 = module is a no-op.
#define BEYOND0LTE_PCIE_WLAN  1

#define PCIE_PROGRESS_TRYING  0x68u
#define PCIE_STATUS_READY     0x4E414C57u   // 'WLAN' - DSDT WLAN._STA returns 0x0F only on this value
STATIC volatile UINT32  *CONST  mWlanProgress = (volatile UINT32 *)(UINTN)0x8FE0F000ULL;

PCIE_CTX  mPcie;

STATIC
VOID
MapWindows (
  VOID
  )
// Every window below is published Device (uncached, identity) by
// PlatformMemoryMapLib; this only fills the pointer table.
{
  mPcie.Elbi        = (volatile UINT32 *)(UINTN)EXYNOS_PCIE0_ELBI_BASE;
  mPcie.Phy         = (volatile UINT32 *)(UINTN)EXYNOS_PCIE0_PHY_BASE;
  mPcie.Pcs         = (volatile UINT32 *)(UINTN)EXYNOS_PCIE0_PCS_BASE;
  mPcie.SysReg      = (volatile UINT32 *)(UINTN)EXYNOS_PCIE0_SYSREG_BASE;
  mPcie.Dbi         = (volatile UINT32 *)(UINTN)EXYNOS_PCIE0_DBI_BASE;
  mPcie.Ia          = (volatile UINT32 *)(UINTN)EXYNOS_PCIE0_IA_BASE;
  mPcie.Cfg         = (volatile UINT32 *)(UINTN)EXYNOS_PCIE0_CFG_BASE;
  mPcie.Cmu         = (volatile UINT32 *)(UINTN)EXYNOS_CMU_FSYS1_BASE;
  mPcie.Pmu         = (volatile UINT32 *)(UINTN)EXYNOS_PMU_BASE;
  mPcie.GpioPeric0  = (volatile UINT32 *)(UINTN)EXYNOS_PERIC0_GPIO_BASE;
  mPcie.GpioFsys1   = (volatile UINT32 *)(UINTN)EXYNOS_FSYS1_GPIO_BASE;
  mPcie.SysRegFsys1 = (volatile UINT32 *)(UINTN)EXYNOS_SYSREG_FSYS1_BASE;
}

STATIC
EFI_STATUS
PcieWlanBringUp (
  VOID
  )
{
  UINT32      VidDid;
  UINT32      Vid;
  UINT32      Did;
  UINT32      ChipId;
  EFI_STATUS  Status;

  MapWindows ();

  //
  // First access to the ACPM-gated CMU_FSYS1 block is ONE read, printed before
  // anything is written: if the boot dies right after this line, the gate read
  // itself hung (the 0x68 guard then skips WLAN on the next boot).
  //
  DEBUG ((DEBUG_INFO, "[PCIe] WLAN bring-up: reading CMU_FSYS1 PCIe PHY_REFCLK gate\n"));
  DEBUG ((DEBUG_INFO, "[PCIe] CMU_FSYS1+0x%x = 0x%x (before)\n", EXYNOS_FSYS1_GATE_PCIE_PHY_REFCLK,
    MmioRead32 (EXYNOS_CMU_FSYS1_BASE + EXYNOS_FSYS1_GATE_PCIE_PHY_REFCLK)));

  //
  // Power-on sequence, kernel order (v0.c:2447 ff + notes W8):
  // clocks -> PMU PHY control -> coherency -> WLAN_EN high + 100 ms ->
  // PERST low -> PHY pwrdn-clear + history arm -> link up.
  //
  ClocksOn ();
  DEBUG ((DEBUG_INFO, "[PCIe] clocks on: PHY_REFCLK gate 0x%x, QCH_PCIE_PHY 0x%x\n",
    MmioRead32 (EXYNOS_CMU_FSYS1_BASE + EXYNOS_FSYS1_GATE_PCIE_PHY_REFCLK),
    MmioRead32 (EXYNOS_CMU_FSYS1_BASE + EXYNOS_FSYS1_QCH_PCIE_PHY)));
  PmuPhyControlOn ();
  CacheCoherencyEnable ();

  PerstSet (FALSE);                       // hold the endpoint in reset
  WlanEnOn ();                            // gpg0-5 high (v0.c:715/776)
  gBS->Stall (100 * 1000);                // 100 ms (Stall takes microseconds), v0.c:718 mdelay(100)
  DEBUG ((DEBUG_INFO, "[PCIe] WLAN_EN high + 100 ms; ELBI RDLH 0x%x (first ELBI read)\n",
    ElbiRead (PCIE_ELBI_RDLH_LINKUP)));

  PowerOnPrep ();

  Status = EstablishLink ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
      "[PCIe] R1 FAIL: link never came up (check P4 clocks, P2 power rail)\n"));
    return Status;
  }

  VidDid = EpCfgRead (EP_CFG_VENDOR_DEVICE);
  Vid    = VidDid & 0xFFFF;
  Did    = VidDid >> 16;
  DEBUG ((DEBUG_INFO,
    "[PCIe] R1: endpoint %04x:%04x (expected 14e4:4475, UNVERIFIED until now)\n", Vid, Did));

  if (Vid != BCM4375_PCI_VENDOR_ID) {
    DEBUG ((DEBUG_ERROR,
      "[PCIe] R1: vendor %x is not Broadcom - config path suspect, EP left alone\n", Vid));
    return EFI_DEVICE_ERROR;
  }

  //
  // Endpoint bring-up: size + assign every memory BAR inside the MEM iATU
  // window (BAR0 = backplane register window, BAR2 = TCM on Broadcom FullMAC
  // PCIe parts - the Windows half reads both back from config space), then
  // MEM decode on. Bus mastering stays OFF: nothing needs DMA until a later
  // rung downloads firmware, and the Windows driver owns that decision.
  //
  EpAssignBars ();
  EpCfgWrite (EP_CFG_COMMAND_MEM_EN, EP_CFG_COMMAND);

  if (mPcie.BarSize[0] >= PCIE_BCM_BAR0_MIN_SIZE) {
    ChipId = MmioRead32 ((UINTN)mPcie.BarAddr[0] + PCIE_BCM_BAR0_CCCORE_OFF + PCIE_BCM_CC_CHIPID);
    DEBUG ((DEBUG_INFO, "[PCIe] R1: chipcommon chipid 0x%x (id 0x%x rev %d, expected id 0x%x)\n",
      ChipId, ChipId & 0xFFFF, (ChipId >> 16) & 0xF, BCM4375_CHIP_ID));
  } else {
    DEBUG ((DEBUG_ERROR, "[PCIe] R1: BAR0 size 0x%x < 0x%x - chipid read skipped\n",
      mPcie.BarSize[0], PCIE_BCM_BAR0_MIN_SIZE));
  }

  //
  // Latched for the Windows half: link up, iATU MEM+CFG0, EP BARs assigned,
  // MEM decode on. The ELBI/DBI/PHY windows stay OUT of anything Windows is
  // given, so nothing in the OS path re-touches the controller (D3 contract).
  //
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Beyond0LtePcieEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Saved;

  mWlanProgress[3] = 0;
  if (!BEYOND0LTE_PCIE_WLAN) {
    DEBUG ((DEBUG_WARN, "[PCIe] WLAN: disabled (BEYOND0LTE_PCIE_WLAN 0)\n"));
    return EFI_UNSUPPORTED;
  }

  if (mWlanProgress[0] == PCIE_PROGRESS_TRYING) {
    DEBUG ((DEBUG_ERROR, "[PCIe] WLAN: previous boot hung in PCIe bring-up (progress 0x68) - skipping it\n"));
    return EFI_UNSUPPORTED;
  }

  Saved            = mWlanProgress[1];
  mWlanProgress[1] = PCIE_PROGRESS_TRYING;
  Status           = PcieWlanBringUp ();
  mWlanProgress[1] = Saved;
  if (!EFI_ERROR (Status)) {
    mWlanProgress[3] = PCIE_STATUS_READY;
  }

  DEBUG ((DEBUG_INFO, "[PCIe] WLAN: bring-up %r, ACPI WLAN %a to Windows\n", Status,
    (mWlanProgress[3] == PCIE_STATUS_READY) ? "visible" : "hidden"));
  //
  // Never break the boot for the WLAN stretch goal: the image stays loaded only
  // on success (nothing in it is resident anyway).
  //
  return Status;
}
