/** @file
  Beyond0LtePcieDxe — bring the S10e's PCIe root port 0 (the WLAN bus) up in
  UEFI: clocks, PHY, PERST#, link training, iATU windows, and the endpoint's
  BAR0 + COMMAND. This is the firmware half of M12/D4b (design:
  docs/exynos9820-port/M12-WIFI-DESIGN.md §3); rung R1 is the VID/DID print
  below. The state this driver latches (link, iATU, BAR0, COMMAND) is what the
  Windows WLAN miniport will inherit through ExitBootServices, exactly the
  role-latch contract already decided for xHCI in D3.

  It deliberately does NOT register an EFI PCI root bridge: the single 4 KiB
  CFG0 iATU window cannot back a 2-bus ECAM contract (design §2), and the
  Windows half binds an ACPI-described device instead.

  FILE_GUID 11111111-2222-3333-4444-555555555549 is this module's BUILD
  IDENTITY, not a hardware read. Nothing here is validated on an S10e yet.
**/

#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Platform/Exynos9820.h>
#include "PcieInternal.h"

PCIE_CTX  mPcie;

STATIC
BOOLEAN
MapWindows (
  VOID
  )
// Every window below is published Device (uncached, identity) by
// PlatformMemoryMapLib; this only fills the pointer table. A zero base/size
// would mean an unverified constant — none are, all VERIFIED.
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

  return TRUE;
}

EFI_STATUS
EFIAPI
Beyond0LtePcieEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32     VidDid;
  UINT32     Vid;
  UINT32     Did;
  EFI_STATUS Status;

  if (!MapWindows ()) {
    return EFI_UNSUPPORTED;
  }

  //
  // Power-on sequence, kernel order (v0.c:2447 ff + notes W8):
  // clocks -> PMU PHY control -> coherency -> WLAN_EN high + 100 ms ->
  // PERST low -> PHY pwrdn-clear + history arm -> link up.
  //
  ClocksOn ();
  PmuPhyControlOn ();
  CacheCoherencyEnable ();

  PerstSet (FALSE);                       // hold the endpoint in reset
  WlanEnOn ();                            // gpg0-5 high (v0.c:715/776)
  gBS->Stall (100 * 10);                  // 100 ms, v0.c:718 mdelay(100)

  PowerOnPrep ();

  Status = EstablishLink ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
      "[PCIe] R1 FAIL: link never came up (check P4 clocks, P2 power rail)\n"));
    return EFI_SUCCESS;   // never break the boot for the WLAN stretch goal
  }

  //
  // Endpoint bring-up (sysfs case-9 pattern, v0.c:935-960, plus the pieces
  // that test path skipped: BusMaster and INTx). BAR0 = the MEM iATU window;
  // the 4375's BAR0 is the backplane window the WLAN driver will use.
  //
  EpCfgWrite ((UINT32)EXYNOS_PCIE0_MEM_BASE, EP_CFG_BASE_ADDRESS_0);
  EpCfgWrite (EP_CFG_COMMAND_MEM_EN | EP_CFG_COMMAND_BUS_MASTER_EN,
    EP_CFG_COMMAND);

  VidDid = EpCfgRead (0x000);
  Vid    = VidDid & 0xFFFF;
  Did    = VidDid >> 16;

  DEBUG ((DEBUG_INFO,
    "[PCIe] R1: endpoint %04x:%04x (expected 14e4:4475, UNVERIFIED until now)\n",
    Vid, Did));

  if (Vid != BCM4375_PCI_VENDOR_ID) {
    DEBUG ((DEBUG_ERROR,
      "[PCIe] R1: vendor %x is not Broadcom - config path suspect\n", Vid));
  }

  //
  // Latched for the Windows half: link up, iATU MEM+CFG0, EP BAR0 assigned,
  // MEM+BME on. The ELBI/DBI/PHY windows stay OUT of anything Windows is
  // given, so nothing in the OS path re-touches the controller (D3 contract).
  //
  return EFI_SUCCESS;
}
