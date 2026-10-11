/** @file
  PcieInternal.h — shared prototypes and mapped-window context for
  Beyond0LtePcieDxe. Windows stay identity-mapped (the platform memory map
  publishes every window as Device — see PlatformMemoryMapLib).
**/

#ifndef _BEYOND0LTE_PCIE_INTERNAL_H
#define _BEYOND0LTE_PCIE_INTERNAL_H

#include <Uefi.h>
#include "PcieHw.h"

typedef struct {
  volatile UINT32  *Elbi;    // 0x13ED0000
  volatile UINT32  *Phy;     // 0x13EF0000
  volatile UINT32  *Pcs;     // 0x13EE0000
  volatile UINT32  *SysReg;  // 0x13C11044 (pcie_0 "sysreg" window)
  volatile UINT32  *Dbi;     // 0x13F00000
  volatile UINT32  *Ia;      // 0x13EC0000
  volatile UINT32  *Cfg;     // 0x14FFE000 (CFG0 iATU target window)
  volatile UINT32  *Cmu;     // 0x13C00000 (CMU_FSYS1)
  volatile UINT32  *Pmu;     // 0x15860000
  volatile UINT32  *GpioPeric0; // 0x10430000
  volatile UINT32  *GpioFsys1;  // 0x13C40000
  volatile UINT32  *SysRegFsys1; // 0x13C10000
} PCIE_CTX;

extern PCIE_CTX  mPcie;

//
// PciePhy.c — the pci-exynos9820_cal.c ch0 path
//
VOID  PhyCheckRxElecidle (IN UINT32  Val);      // IGNORE=1 / ENABLE=0
VOID  PhyAllPwrdnClear (VOID);
VOID  PhyConfig (VOID);
VOID  PhyRunIaSequencer (VOID);                 // assert_phy_reset's I/A program

//
// PcieLink.c
//
VOID   ClocksOn (VOID);
VOID   PmuPhyControlOn (VOID);
VOID   PowerOnPrep (VOID);
VOID   CacheCoherencyEnable (VOID);
VOID   WlanEnOn (VOID);                         // gpg0-5 high
VOID   PerstSet (IN BOOLEAN  High);             // gpf2-2
VOID   ElbiWrite (IN UINT32  Val, IN UINT32  Off);
UINT32 ElbiRead (IN UINT32  Off);
VOID   DbiWrite (IN UINT32  Val, IN UINT32  Off);
UINT32 DbiRead (IN UINT32  Off);
VOID   DbiRmw (IN UINT32  Set, IN UINT32  Clear, IN UINT32  Off);
VOID   SetupRc (VOID);                          // dw_pcie_setup + exynos_pcie_setup_rc
EFI_STATUS  ProgramAtu (VOID);                  // region1 CFG0 (bus1 dev0), region0 MEM
UINT32 EpCfgRead (IN UINT32  Where);
VOID   EpCfgWrite (IN UINT32  Val, IN UINT32  Where);
EFI_STATUS  EstablishLink (VOID);               // the full establish_link transliteration

#endif // _BEYOND0LTE_PCIE_INTERNAL_H
