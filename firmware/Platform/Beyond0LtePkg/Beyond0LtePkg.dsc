## @file
#  Platform description (DSC) for Beyond0LtePkg — Galaxy S10e / Exynos 9820
#  (beyond0lte, SM-G970F).
#
#  Ported from firmware/Platform/Star2LtePkg/Star2LtePkg.dsc, the validated Exynos
#  9810 package. Same shape: a combined SEC+PEI (ArmPlatformPkg/PeilessSec, which
#  replaces the removed ArmPlatformPkg/PrePi) hands off straight to the DXE core,
#  then BDS.
#
#  PLATFORM_GUID IS DETERMINISTIC, NOT RANDOM: 11111111-2222-3333-4444-555555555502
#  is the 9810 platform's stem (…555555555500) with the low pair set to 02 — the
#  9820 generation digit this tree already uses (Exynos9820Pkg.dec …5520,
#  AcpiTables …5521, Exynos9820SerialPortLib …5522, PlatformMemoryMapLib …5531).
#  Grepped against every .inf/.dec/.dsc GUID in the repo: …5502 is unused, so it
#  cannot collide.
#
#  #############################################################################
#  DROPPED FROM THE 9810 DSC — every one of these has NO 9820 equivalent in
#  firmware/Platform/Beyond0LtePkg or firmware/Silicon/Exynos9820Pkg, and none
#  was silently substituted with a 9810 module. Existence was checked per path
#  (Test-Path), not by directory listing. The 9820 tree has no Drivers/
#  directory at all, no PlatformBootManagerLib and no reset-system library.
#
#    Platform/Star2LtePkg/Drivers/SimpleFramebufferDxe/SimpleFramebufferDxe.inf
#        PORTED after this header was written:
#        Platform/Beyond0LtePkg/Drivers/SimpleFramebufferDxe/ now exists and is
#        placed in [Components.common] below. It returns EFI_NOT_READY while
#        EXYNOS_FB_BASE is 0 (never read on this SoC), so the panel stays dark
#        but the boot is not blocked. Consistent with PlatformMemoryMapLib.c,
#        which skips the framebuffer carveout for the same reason.
#
#    Platform/Star2LtePkg/Drivers/Star2LteAcpiPlatformDxe/Star2LteAcpiPlatformDxe.inf
#        No Beyond0LteAcpiPlatformDxe. AcpiTables.inf DOES exist for the 9820 and
#        still ships as an FFS file, but nothing installs it via
#        EFI_ACPI_TABLE_PROTOCOL. The 9810 driver also patched the GTDT at install
#        time (26 MHz frame + the Samsung MCT read base); with it gone, Gtdt.aslc
#        keeps its 0xFFFFFFFFFFFFFFFF placeholders.
#
#    Platform/Star2LtePkg/Drivers/Star2LteRamManagerDxe/Star2LteRamManagerDxe.inf
#        None. Multi-region HOB publication of the extra DRAM banks is gone, so
#        only the single PcdSystemMemoryBase/Size window reaches DXE.
#
#    Platform/Star2LtePkg/Drivers/Star2LteUfsHcDxe/Star2LteUfsHcDxe.inf
#        None. The EDKII_UFS_HOST_CONTROLLER_PROTOCOL producer is gone, so
#        UfsPassThruDxe binds nothing and the storage chain below it enumerates
#        no volumes. Not renameable anyway: the 9810 driver's UFS HCI is the
#        0x111… region, the 9820's is ufs@13D60000 (Exynos9820.h).
#
#    Platform/Star2LtePkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
#        None. The platform BDS hook is gone (and with it the 9810's USB-debug
#        modes). UefiBootManagerLib, the edk2 backing lib, still resolves.
#
#    Platform/Star2LtePkg/Library/Star2LteResetSystemLib/Star2LteResetSystemLib.inf
#        None. ResetSystemLib therefore has NO instance. The 9810 library is PMU
#        based and the 9820 PMU moved (system-controller@15860000, not the 9810's
#        0x14060000), so reusing it unchanged would have pointed at a register
#        block that does not exist on this SoC.
#
#    Also dropped: the STAR2LTE_USB_DEBUG_MODE and STAR2LTE_RAM_STAGE DEFINEs and
#    the two -D… CC_FLAGS lines that feed them. Grep over firmware/ shows USB_DEBUG
#    _MODE is read only by Star2LtePkg's PlatformMemoryMapLib.c, PlatformBootManager
#    Lib.c, UsbDebug.c/.h and UsbDebugHid.c — all dropped above — and RAM_STAGE is
#    read by no C source in this checkout at all. The 9820 PlatformMemoryMapLib.c
#    reads neither.
#
#  PORTED (rename only; both .inf paths verified present):
#    Platform/Star2LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf
#      -> Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf
#    Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib/Exynos9810SerialPortLib.inf
#      -> Silicon/Exynos9820Pkg/Library/Exynos9820SerialPortLib/Exynos9820SerialPortLib.inf
#    Platform/Star2LtePkg/AcpiTables/AcpiTables.inf
#      -> Platform/Beyond0LtePkg/AcpiTables/AcpiTables.inf
#
#  One edk2 path was CORRECTED, not substituted: the 9810 DSC's
#  ArmPkg/Library/ArmTransferListLib/ArmTransferListLib.inf does not exist in this
#  checkout; the same library lives at MdePkg/Library/ArmTransferListLib/.
#  #############################################################################
#
#  NOTE ON VERSIONS (kept from the 9810): library .inf paths track a specific edk2
#  checkout. If the build complains a path moved, find the current location in your
#  edk2 tree (paths drift between releases). Build ONE component at a time, starting
#  with the SerialPortLib, then PeilessSec, then DxeMain.
#
#  NOTHING IN THIS FILE IS VALIDATED ON AN S10e. It is the port's floor: the reads
#  it cites come from the shipping 9820 device tree and from live node-name reads
#  on the phone (Exynos9820.h, notes/dock-desktop.md §5/§13/§15/§17).
##

[Defines]
  PLATFORM_NAME                  = Beyond0LtePkg
  PLATFORM_GUID                  = 11111111-2222-3333-4444-555555555502
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x0001001B
  OUTPUT_DIRECTORY               = Build/Beyond0LtePkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/Beyond0LtePkg/Beyond0LtePkg.fdf

#
# The 9810's STAR2LTE_USB_DEBUG_MODE and STAR2LTE_RAM_STAGE DEFINEs are dropped
# here — see the DROPPED block above: they feed only 9810 sources.
#

[BuildOptions]
  GCC:*_*_AARCH64_PLATFORM_FLAGS = -march=armv8-a
  # CRITICAL for AArch64: force 4KB PE section alignment. Without this the
  # default /ALIGN:32 produces a TE StrippedSize that is NOT page-aligned, which
  # breaks every PC-relative adrp/ldr access to .data after TE stripping (we hit
  # this exactly: mSystemMemoryEnd read as 0 -> SEC stack = 0x1 -> CEntryPoint
  # prologue faulted). Matches ArmVirtPkg/ArmVirt.dsc.inc.
  GCC:*_*_AARCH64_DLINK_FLAGS    = -z common-page-size=0x1000
  CLANGPDB:*_*_*_DLINK_FLAGS     = /ALIGN:0x1000
  # CLANGPDB/CLANGDWARF CC_FLAGS from the 9810 DSC carried only the two -D
  # switches for the dropped STAR2LTE_* knobs; nothing in the 9820 tree reads
  # them, so the lines are gone rather than replaced with empty ones.

################################################################################
#
# Library classes — shared by all module types
#
################################################################################
[LibraryClasses.common]
  # --- MdePkg base ---
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf

  # --- Stack check (compiler-inserted canaries; Null for bring-up) ---
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf

  # --- Debug / status code ---
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf

  # --- Serial console: our Exynos UART (Milestone 1) ---
  SerialPortLib|Silicon/Exynos9820Pkg/Library/Exynos9820SerialPortLib/Exynos9820SerialPortLib.inf

  # --- ARM architecture ---
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  ArmMmuLib|UefiCpuPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  ArmHvcLib|ArmPkg/Library/ArmHvcLib/ArmHvcLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  # edk2 master unified ARM exception handling into UefiCpuPkg and folded
  # ArmGicLib into ArmGicDxe; DefaultExceptionHandlerLib is no longer separate.
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf

  # --- ArmPlatformPkg: our device memory map lives here ---
  ArmPlatformLib|Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf

  # --- Generic services ---
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf

  # --- SCSI/UFS storage stack ---
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf
  # GptLib is an implicit library class of PartitionDxe in this checkout (the
  # 9810 DSC predates the split); without an instance the build stops at error 4000.
  GptLib|MdeModulePkg/Library/GptLib/GptLib.inf

  # --- BDS front-end ---
  # BdsDxe requires PlatformBootManagerLib. The 9810's is a 6000-line port that
  # has not been carried over; edk2's ArmPkg generic instance is used instead,
  # which is the same choice every other AARCH64 phone port makes.
  PlatformBootManagerLib|ArmPkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf

  # --- FV / decompression ---
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  # LzmaDecompressLib has NO instance in this edk2 checkout: neither
  # LzmaCustomDecompressLib.inf nor LzmaArchCustomDecompressLib.inf declare that
  # LIBRARY_CLASS (both are LIBRARY_CLASS = NULL). Nothing in this build asks for
  # the class, so the line the 9810 DSC carried is dropped rather than pointed at
  # a lib that cannot satisfy it.
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf

  # --- Misc null/instances ---
  DebugAgentLib|MdeModulePkg/Library/DebugAgentLibNull/DebugAgentLibNull.inf
  PciExpressLib|MdePkg/Library/BasePciExpressLib/BasePciExpressLib.inf
  PciLib|MdePkg/Library/BasePciLibPciExpress/BasePciLibPciExpress.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf

  # --- Reset + RTC ---
  # The 9810 line here (Star2LteResetSystemLib, PMU-based at 0x14060000) is DROPPED:
  # there is no 9820 equivalent and the 9820 PMU is at 0x15860000, so the library
  # would address a register block that does not exist on this SoC. ResetSystemLib
  # therefore has no instance in this build.
  # Virtual RTC for bring-up (no Exynos RTC driver yet). Time won't persist.
  RealTimeClockLib|EmbeddedPkg/Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.inf

  # --- BDS / boot manager (launches Windows) ---
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  # PlatformBootManagerLib DROPPED — no 9820 equivalent (see the DROPPED block).
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf

  # --- UEFI variable services backing libs ---
  AuthVariableLib|MdeModulePkg/Library/AuthVariableLibNull/AuthVariableLibNull.inf
  VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf

[LibraryClasses.common.SEC]
  # PeilessSec (combined SEC+PEI) flow — replaces the removed ArmPlatformPkg/PrePi
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  MemoryInitPeiLib|ArmPlatformPkg/MemoryInitPei/MemoryInitPeiLib.inf
  PlatformPeiLib|ArmPlatformPkg/PlatformPei/PlatformPeiLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  # --- PeilessSec extras (new requirements vs the old PrePi) ---
  PeilessSecMeasureLib|SecurityPkg/Library/PeilessSecMeasureLib/PeilessSecMeasureLibNull.inf
  # The 9810 DSC spelled this ArmPkg/Library/…; in THIS checkout the library lives
  # under MdePkg. Same module, drifted path — see the NOTE ON VERSIONS above.
  ArmTransferListLib|MdePkg/Library/ArmTransferListLib/ArmTransferListLib.inf
  # SEC-safe ExtractGuidedSectionLib (the DXE instance rejects MODULE_TYPE=SEC).
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf

[LibraryClasses.common.DXE_CORE]
  HobLib|MdePkg/Library/DxeCoreHobLib/DxeCoreHobLib.inf
  MemoryAllocationLib|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf

[LibraryClasses.common.DXE_DRIVER]
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf

[LibraryClasses.common.UEFI_DRIVER, LibraryClasses.common.UEFI_APPLICATION]
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLibRuntimeDxe.inf

################################################################################
#
# Feature + fixed PCDs
#
################################################################################
[PcdsFeatureFlag.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdHiiOsRuntimeSupport|FALSE

[PcdsFixedAtBuild.common]
  # --- Debug mask (errors+warnings+info+load). Trim for RELEASE. ---
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8000004F
  # PcdDebugPropertyMask: 0x0E = PRINT|CODE|CLEAR_MEMORY, but ASSERT (0x01) and
  # ASSERT_DEADLOOP (0x20) DISABLED. On this chainloaded device with no draining
  # UART, a DEBUG-build ASSERT(FALSE) -> CpuDeadLoop() hard-hangs the boot (the
  # 9810 hit it in EDK2's DumpQueryResponseResult default case during UFS device
  # init). Making ASSERT a no-op keeps any stray EDK2 assertion deeper in
  # enumeration from wedging bring-up. Kept from the 9810: it is an edk2 fact.
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x0E

  # --- BDS terminal type ---
  # ArmPkg's PlatformBootManagerLib (the instance this build uses) carries a
  # static_assert that PcdDefaultTerminalType == 4 (TTYTERM); edk2's default is
  # 0, so without this line the AARCH64 BDS front-end does not compile.
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|4

  # --- UFS device-init completion poll budget (UfsFinishDeviceInitialization
  #     fDeviceInit ReadFlag loop). Default 600000 iterations; during Exynos UFS
  #     bring-up a not-yet-clearing flag would make that loop run effectively
  #     forever, so cap it small to fail fast and reach the on-screen status panel.
  #     TODO: restore default once UFS is stable. ---
  gEfiMdeModulePkgTokenSpaceGuid.PcdUfsInitialCompletionTimeout|2000

  # --- System DRAM.
  #
  #     Base 0x80000000 is EXYNOS_DRAM_BASE (Exynos9820.h:79, VERIFIED: the vendor
  #     DT's memory@80000000 node, reg = <0x0 0x80000000 0x80000000>).
  #
  #     Size 0x50000000 is EXYNOS_DRAM_SIZE, which is NOT a read: it is bank0's
  #     front up to the FIRST reserved region the board file
  #     (exynos9820-rmem_s5100_dual.dtsi) declares, EXYNOS_RSVD_MODEM_SPARE_BASE
  #     0xD0000000 (Exynos9820.h:96) minus the base. That file reserves a
  #     contiguous 729 MB, 0xD0000000..0xFD900000, for the modem/CP stack
  #     (Exynos9820.h:96-107, notes/dock-desktop.md §17); publishing all of bank0
  #     would hand that to Windows as conventional RAM.
  #
  #     The 9810's window (0x90000000..0xBC800000) and its whole ext72/ext99
  #     experiment log are NOT carried over: they are 9810 observations about the
  #     9810's RKP kernel zone (0x80094000-0x828DBFFF), and no equivalent read
  #     exists for the 9820. The 9820's split points (0xC0000000, 0xE1900000) and
  #     the high 2 GiB banks (0x880000000, 0x900000000) are reads of the BASES
  #     only — sizes unread (Exynos9820.h:84-90) — so they stay out of this
  #     single Base/Size pair until a multi-region HOB publisher exists.
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x0000000080000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x0000000050000000
  gExynos9820TokenSpaceGuid.PcdSystemMemoryBase|0x0000000080000000
  gExynos9820TokenSpaceGuid.PcdSystemMemorySize|0x0000000050000000

  # --- Debug UART base (Milestone 1). VERIFIED on the live S10e and in the vendor
  #     DT: uart@10440000, the first PERIC0 UART — same base as the S9+, but the
  #     9820's window is 0x100, not the 9810's 0x1000 (Exynos9820.h:120-121). ---
  gExynos9820TokenSpaceGuid.PcdSerialRegisterBase|0x0000000010440000

  # --- GICv2, VERIFIED — IDENTICAL to the 9810. The 9820's node is
  #     interrupt-controller@10100000, compatible "arm,cortex-a15-gic",
  #     "arm,cortex-a9-gic" (GICv2, same architecture as the 9810's arm,gic-400).
  #     GICv2 uses PcdGicInterruptInterfaceBase (GICC), NOT redistributors.
  #     GICD 0x10101000/0x1000, GICC 0x10102000/0x1000 (Exynos9820.h:139-142).
  #     Do not confuse with abox-gic@18cf0000, Samsung's ABOX interface. ---
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x0000000010101000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0x0000000010102000

  # --- CPU topology. NOT the 9810's 4x A55 + 4x M3: the 9820 is TWO MPIDR
  #     clusters sized 6 + 2 (ananke + prometheus, then cheetah) with THREE
  #     efficiency classes inside them (notes/dock-desktop.md §16, Exynos9820Pkg.dec
  #     PcdClusterCount). Anything deriving a per-cluster core count by dividing
  #     CoreCount/ClusterCount is wrong here — 8/2 = 4 would put the two BIG cores
  #     in the MID cores' slots; Beyond0LtePkg's ArmPlatformLib.c uses the explicit
  #     mCoresPerCluster[] = { 6, 2 } instead of the division. ---
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|2
  # Primary (boot) core MpId. Almost certainly LITTLE cluster0/core0 == 0x0, as on
  # the 9810, but UNVERIFIED here: the 9810 confirmed it with a Milestone-1 UART
  # log (MPIDR_EL1); nothing has been read on an S10e (ArmPlatformLib.c).
  # NOTE: edk2 master moved this PCD to gArmTokenSpaceGuid.
  gArmTokenSpaceGuid.PcdArmPrimaryCore|0x0

  # PeilessSec stack + UEFI region sizing (replaces the removed ArmPlatformStackLib).
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x00010000
  gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize|0x08000000

  # --- Architected generic timer GSIVs. The 9810 DSC called these "VERIFIED from
  #     DT"; that claim does NOT transfer. Gtdt.aslc (9820) marks 29/30/27/26 as
  #     the architectural default, UNVERIFIED on this SoC, and the DT mapping
  #     reminder there is GIC_PPI n -> GSIV = 16 + n (PPIs 13/14/11/10). ---
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26
  # CRITICAL (runtime): stock sboot does NOT set CNTFRQ_EL0 on the 9810 and the
  # real rate is 26 MHz; whether the S10e's sboot behaves the same way has NOT
  # been established, so EXYNOS_ARCH_TIMER_FREQ_HZ (Exynos9820.h:169) is the 9810
  # value carried and marked UNVERIFIED. edk2 master REMOVED PcdArmArchTimerFreqInHz
  # (timer libs now read CNTFRQ_EL0 directly). On the 9810 CNTFRQ_EL0 read 0 and was
  # RO at EL1, handled by a workspace-local fallback in
  # ArmPkg/Library/ArmGenericTimerPhyCounterLib. Re-check that fallback on the S10e
  # before trusting any timer period here.
  # gArmTokenSpaceGuid.PcdArmArchTimerFreqInHz|26000000   # PCD removed upstream

  # --- Firmware volume placement. MUST match Beyond0LtePkg.fdf.
  #
  #     0x80080000 is the load address Exynos9820.h states for this SoC
  #     ("the DT's chosen node also puts the Android initrd at 0x84000000..0x841fffff,
  #     which is why the firmware loads at 0x80080000"). The initrd bounds are the
  #     read; 0x80080000 is the decision that follows from them — UNVERIFIED as an
  #     observed address, nothing has booted on an S10e.
  #
  #     The 9810's 0x90000000 is deliberately NOT carried over: its justification
  #     (clean RAM above the RKP-monitored kernel zones, 0x80080000 bootlooping) is
  #     a 9810 device observation with no 9820 equivalent read.
  #
  #     0x700000 (7 MB) is the 9810's FV size, carried as a build parameter. ---
  gArmTokenSpaceGuid.PcdFdBaseAddress|0x0000000080080000
  gArmTokenSpaceGuid.PcdFdSize|0x00700000
  gArmTokenSpaceGuid.PcdFvBaseAddress|0x0000000080080000
  gArmTokenSpaceGuid.PcdFvSize|0x00700000

  # --- VFP / NEON: PcdVFPEnabled was removed upstream (handled via build flags). ---
  # gArmTokenSpaceGuid.PcdVFPEnabled|1

  # --- UEFI variable store: memory-backed (non-persistent) for bring-up.
  #     Windows sets BootOrder/BootXXXX here. Persisting them to a real flash
  #     partition is future work (see docs/08-edk2-internals.md). ---
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE

  # --- BDS: short timeout; show the boot menu only briefly. ---
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|3

################################################################################
#
# Components — build order: SEC(PeilessSec) -> DXE core -> DXE/BDS drivers
#
################################################################################
[Components.common]
  #
  # Milestone 1 — build/verify the UART library in isolation first:
  #   build -p Platform/Beyond0LtePkg/Beyond0LtePkg.dsc -m \
  #     Silicon/Exynos9820Pkg/Library/Exynos9820SerialPortLib/Exynos9820SerialPortLib.inf
  #
  Silicon/Exynos9820Pkg/Library/Exynos9820SerialPortLib/Exynos9820SerialPortLib.inf

  #
  # Milestone 2+ — full boot chain. These mirror Beyond0LtePkg.fdf. The DSC must
  # list every module the FDF places, so keep the two in sync.
  #
  # --- SEC / PEI (PeilessSec replaces the removed ArmPlatformPkg/PrePi) ---
  ArmPlatformPkg/PeilessSec/PeilessSec.inf {
    <LibraryClasses>
      #
      # The DXE firmware volume (FVMAIN) is stored LZMA-compressed inside
      # FVMAIN_COMPACT (GUIDED section EE4E5898-... = LZMA_CUSTOM_DECOMPRESS_GUID).
      # PeilessSec's DecompressFirstFv -> FfsProcessFvFile extracts it via
      # ExtractGuidedSectionGetInfo/Decode, which only works if a handler for that
      # GUID has been REGISTERED. LzmaCustomDecompressLib registers it from its
      # CONSTRUCTOR (LIBRARY_CLASS = NULL), so it must be linked into THIS module
      # to have ProcessLibraryConstructorList() run that constructor. Without it,
      # DecompressFirstFv returns an error and SecMain's ASSERT_EFI_ERROR
      # CpuDeadLoops.
      #
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }

  # --- DXE core + PCD ---
  MdeModulePkg/Core/Dxe/DxeMain.inf
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  # --- Architectural protocols ---
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  MdeModulePkg/Universal/Metronome/Metronome.inf
  # ResetSystemRuntimeDxe DROPPED: it needs a ResetSystemLib instance and the
  # 9820 has none (the 9810's is PMU-based at 0x14060000, this SoC's PMU is
  # 0x15860000 and the intra-block layout is unverified). Reset-to-recovery is
  # therefore not offered by this build until that library is ported.
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  # The DXE core requires the full set of architectural protocols before it will
  # hand off to BDS (CoreAllEfiServicesAvailable ASSERTs otherwise). These three
  # were the ones missing on the 9810: Watchdog (generic software timer, no SoC HW
  # WDT so no Exynos PMU/RKP conflict), Capsule (stub), Monotonic Counter.
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf

  # --- Variable services (memory-backed; PcdEmuVariableNvModeEnable=TRUE) ---
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf

  # --- Console: serial + splitter + the 9820 first-light framebuffer driver.
  #     Beyond0LteSimpleFramebufferDxe returns EFI_NOT_READY while EXYNOS_FB_BASE
  #     is still 0 (the carveout has never been measured on an S10e), so the
  #     screen stays dark without failing the boot. ---
  Platform/Beyond0LtePkg/Drivers/SimpleFramebufferDxe/SimpleFramebufferDxe.inf
  # HiiDatabaseDxe FIRST — produces the 3 HII protocols (Database/String/
  # ConfigRouting) that BdsDxe's UefiHiiServicesLib-injected DEPEX requires.
  # Without it BdsDxe never dispatches and the Bds arch protocol is missing.
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf

  # --- ACPI (FADT/MADT/GTDT/PPTT + DSDT).
  #     Star2LteAcpiPlatformDxe is DROPPED: no Beyond0LteAcpiPlatformDxe exists, so
  #     these tables ship but are never installed (see the DROPPED block). ---
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  Platform/Beyond0LtePkg/AcpiTables/AcpiTables.inf

  # --- RAM discovery: Star2LteRamManagerDxe DROPPED (no 9820 equivalent). ---

  # --- Storage + filesystem (Windows ESP on UFS).
  #     Star2LteUfsHcDxe is DROPPED (no 9820 equivalent), so the host-controller
  #     protocol below has no producer and this chain enumerates nothing yet.
  #     Every path listed here exists in the checkout. ---
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  # EnglishDxe produces gEfiUnicodeCollation2Protocol, which EnhancedFatDxe
  # REQUIRES to start (Fat.inf lists it ## TO_START). Without a collation
  # provider, FatDriverBindingStart fails and Fat mounts ZERO volumes (fs=0),
  # so the Windows ESP never appears.
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf

  # --- BDS: finds + launches the Windows Boot Manager ---
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
  # Optional (not required to launch bootmgfw.efi). Re-add together with the
  # ShellPkg library tail once the minimal FD boots on device.
  # MdeModulePkg/Application/UiApp/UiApp.inf
  # ShellPkg/Application/Shell/Shell.inf
