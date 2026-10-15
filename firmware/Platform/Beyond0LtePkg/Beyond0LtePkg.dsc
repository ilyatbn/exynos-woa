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
#  DROPPED FROM THE 9810 DSC — as of the header being written, each one of these
#  had NO 9820 equivalent in firmware/Platform/Beyond0LtePkg or firmware/Silicon/
#  Exynos9820Pkg, and none had been silently substituted with a 9810 module.
#  Existence was checked per path (Test-Path), not by directory listing. The
#  9820 tree then had no Drivers/ directory, no PlatformBootManagerLib instance
#  and no reset-system library. Entries below are annotated PORTED once a later
#  task added the equivalent, so this list is the ORIGINAL state, not the current
#  one — read the per-entry note for what exists now.
#
#    Platform/Star2LtePkg/Drivers/SimpleFramebufferDxe/SimpleFramebufferDxe.inf
#        PORTED after this header was written:
#        Platform/Beyond0LtePkg/Drivers/SimpleFramebufferDxe/ now exists and is
#        placed in [Components.common] below. EXYNOS_FB_BASE is now 0xCA000000,
#        read from the device's own kernel command line (s3cfb.bootloaderfb), so
#        the panel is drawn and PlatformMemoryMapLib.c publishes the carveout as
#        device memory.
#
#    Platform/Star2LtePkg/Drivers/Star2LteAcpiPlatformDxe/Star2LteAcpiPlatformDxe.inf
#        PORTED after this header was written ->
#        Platform/Beyond0LtePkg/Drivers/Beyond0LteAcpiPlatformDxe/, placed in
#        [Components.common] below, so the tables are now actually installed.
#        Two of its channels are compiled out rather than retargeted: the PRAM
#        breadcrumb (0xFED14000 is inside bank0 and inside the vendor's
#        debug_snapshot.log_bcm on this SoC - notes/s10e-live-reads.md §7.3) and
#        the GTDT memory-mapped counter (the 26 MHz frame and the MCT read base
#        are 9810 values), so Gtdt.aslc keeps its all-ones placeholders.
#
#    Platform/Star2LtePkg/Drivers/Star2LteRamManagerDxe/Star2LteRamManagerDxe.inf
#        PORTED after this header was written ->
#        Platform/Beyond0LtePkg/Drivers/Beyond0LteRamManagerDxe/, listed in
#        [Components.common] below. Not a rename: the 9810's hardcoded bank-1
#        ranges are 9810 DRAM-shape facts, so the ranges come from the
#        EXYNOS_DRAM_BANK3/4 macros (verified base AND size) and banks whose
#        extent was never read are skipped, not guessed.
#
#    Platform/Star2LtePkg/Drivers/Star2LteUfsHcDxe/Star2LteUfsHcDxe.inf
#        PORTED after this header was written ->
#        Platform/Beyond0LtePkg/Drivers/Beyond0LteUfsHcDxe/, placed in
#        [Components.common] below, so UfsPassThruDxe has its protocol producer
#        again. Not a rename: every UFS window is remapped to the EXYNOS_UFS_*
#        macros (9820 ufs@13D60000, Exynos9820.h:268-278) and the 0xFED1xxxx
#        PRAM/diag telemetry channel is compiled out, not retargeted.
#
#    Platform/Star2LtePkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
#        Not carried, and not needed: the library CLASS is resolved to ArmPkg's
#        generic instance (see the [LibraryClasses.common] line below), which is
#        what BdsDxe requires. What the 9810's platform instance added was the
#        platform BDS hook and its USB-debug modes, neither of which is on the
#        phase-one path (Windows boots and sees disk/RAM/CPU).
#
#    Platform/Star2LtePkg/Library/Star2LteResetSystemLib/Star2LteResetSystemLib.inf
#        PORTED after this header was written ->
#        Platform/Beyond0LtePkg/Library/Beyond0LteResetSystemLib/, which is the
#        ResetSystemLib instance below, so ResetSystemRuntimeDxe is back in the
#        build. Not a rename: only the architected PSCI path is carried; the
#        PMU SWRESET/PS_HOLD fallback is compiled out (the 9820 PMU base is a
#        read, the intra-block offsets are not) and the 0xFED14000 breadcrumb
#        likewise. See notes/m8-reset-lib.md.
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

  # --- Serial console ---
  # Boot bring-up (notes/m8-uefi-image.md B54): nobody can read the UART on the S10e,
  # so the DEBUG log is rendered on the panel instead. To restore the UART, swap back to:
  #   SerialPortLib|Silicon/Exynos9820Pkg/Library/Exynos9820SerialPortLib/Exynos9820SerialPortLib.inf
  SerialPortLib|Platform/Beyond0LtePkg/Library/Beyond0LteFbSerialPortLib/Beyond0LteFbSerialPortLib.inf

  # --- ARM architecture ---
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  ArmMmuLib|UefiCpuPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  ArmHvcLib|ArmPkg/Library/ArmHvcLib/ArmHvcLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf
  # VIRTUAL generic timer (CNTV_*, CNTVCT_EL0). sboot enters us at EL1 under Samsung's
  # hypervisor (boot #10/#11 readout, notes/m8-uefi-image.md B47), where EL1 access to the
  # PHYSICAL timer/counter can be trapped by CNTHCTL_EL2; the virtual ones never are. The
  # 9820 prior art builds with USE_PHYSICAL_TIMER = FALSE (edk2-exynos exynos9820.dsc ->
  # ArmGenericTimerVirtCounterLib). TimerDxe registers all four timer PPIs either way.
  # Pre-emptive (B60): the physical timer has not been observed to fault here.
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerVirtCounterLib/ArmGenericTimerVirtCounterLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  # edk2 master unified ARM exception handling into UefiCpuPkg and folded
  # ArmGicLib into ArmGicDxe; DefaultExceptionHandlerLib is no longer separate.
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf

  # --- ArmPlatformPkg: our device memory map lives here ---
  ArmPlatformLib|Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf

  # --- Generic services ---
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  #
  # D1 USB host: UefiUsbLib is the shared class-driver helper for UsbBusDxe /
  # UsbKbDxe / UsbMouseDxe; NonDiscoverableDeviceRegistrationLib is what
  # Beyond0LteUsbHostDxe calls to publish the MMIO xHCI (see
  # notes/d1b-usb-vbus-hcd.md Q3 — NXP Layerscape shape).
  #
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf
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
  # The 9810 line here (Star2LteResetSystemLib, PMU-based at 0x14060000) is
  # PORTED after that header was written -> Platform/Beyond0LtePkg/Library/
  # Beyond0LteResetSystemLib/, so ResetSystemLib has an instance again. It is
  # not a rename: the 9820 PMU base is 0x15860000 (Exynos9820.h:185, a read) but
  # the intra-block SWRESET/PS_HOLD offsets were never read, so that fallback is
  # compiled out (BEYOND0LTE_PMU_RESET 0) and only the architected PSCI path is
  # live; the 0xFED14000 breadcrumb is compiled out for the same reason it is
  # unusable here (notes/m8-reset-lib.md).
  ResetSystemLib|Platform/Beyond0LtePkg/Library/Beyond0LteResetSystemLib/Beyond0LteResetSystemLib.inf
  # Virtual RTC for bring-up (no Exynos RTC driver yet). Time won't persist.
  RealTimeClockLib|EmbeddedPkg/Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.inf

  # --- BDS / boot manager (launches Windows) ---
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  # The 9810's PLATFORM PlatformBootManagerLib is not carried; this build
  # resolves the library class to ArmPkg's generic instance above (line 194).
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
  # SEC runs with the MMU OFF, where every data access is Device memory. The
  # optimized AArch64 SetMem (BaseMemoryLibOptDxe/AArch64/SetMem.S:125-145) zeroes
  # blocks > 96 B with `dc zva`, and DC ZVA to Device memory is an alignment
  # fault. ArmConfigureMmu zeroes the 4 KB root translation table before it turns
  # the MMU on, which is exactly where boot #11 died (notes/m8-uefi-image.md B47/B48:
  # checkpoint after the memory map drawn, the one after the MMU-on not). The plain
  # C library is what ArmVirtPkg uses for SEC (ArmVirt.dsc.inc:236-238).
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
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
  #
  # m14 run 16: with PSCI hidden from Windows its restart goes through EFI ResetSystem, and a DEBUG print from
  # the runtime code went through Beyond0LteFbSerialPortLib to the PHYSICAL boot-log ring 0x8FF00000 after
  # SetVirtualAddressMap -> bugcheck 0x1E C0000005 at 0x8FF00000. Runtime drivers print nothing.
  #
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
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
  # ERROR|WARN only (was 0x8000004F = +INFO|LOAD): the on-panel log (B54) is read from photos, and
  # the hundreds of Loading driver / InstallProtocolInterface lines buried the UFS errors (B73).
  # B77: +INFO (0x40) back - BDS / ScsiDisk progress is INFO; LOAD (0x04) stays off.
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000042
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
  #
  #     2026-10-10 (notes/m8-uefi-image.md B29) - SIZE NOW 0x3AB00000 = EXYNOS_DRAM_BANK0_SIZE
  #     (VERIFIED, live DTB memory@80000000). PeilessSec (ModuleEntryPoint.S:37-89) puts its
  #     stack, HOBs and the whole UEFI region at the TOP of Base+Size. With 0x50000000 that was
  #     0xC8000000..0xD0000000 - bank1, which PlatformMemoryMapLib does NOT map (MMU on -> the
  #     next stack access faults) and which the 9810 found not usable by UEFI at boot
  #     (Beyond0LteUfsHcDxe.c A105). Boot #5 reached the FD and looped (B28). Now the region is
  #     0xB2B00000..0xBAB00000: mapped bank0 DRAM, above every bank0 reservation read
  #     (camera ends 0xA7400000). The paragraph above is the history of the old value.
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x000000003AB00000
  gExynos9820TokenSpaceGuid.PcdSystemMemoryBase|0x0000000080000000
  gExynos9820TokenSpaceGuid.PcdSystemMemorySize|0x000000003AB00000

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
  #     2026-10-10 (notes/m8-uefi-image.md B23/B25): MOVED to 0x90000000, the PRIOR-ART
  #     value - edk2-exynos configs/exynos9820.conf FD_BASE=0x90000000 (FD_SIZE 0x700000)
  #     is what their shim copies to and what boots EDK2 on the 9820. Not observed on
  #     this phone yet. 0x90000000..0x90700000 is above the kernel window, above el2_code
  #     (ends 0x87200000), below mm_debug 0x91200000, inside bank0. The 0x83300000
  #     history below is kept as the record of why the base moved twice.
  #
  #     DECISION, not an observed address: 0x83300000 (derived from reads, never
  #     booted). It REPLACES the old 0x80080000, whose only justification was
  #     "the DT's chosen node also puts the Android initrd at 0x84000000..0x841fffff,
  #     which is why the firmware loads at 0x80080000" - and that bound is the
  #     DTS KERNEL DEFAULT, not what sboot does (the live chosen node put this
  #     boot's initrd at 0xD5000000..0xD5A67000).
  #
  #     2026-10-10 (notes/K10-rkp.md T18): that initrd bound is the DTS KERNEL
  #     DEFAULT. The live chosen node sboot wrote for the TWRP boot is
  #     0xD5000000..0xD5A67000, so the justification below is void and 0x80080000
  #     is now an unobserved guess, not a decision derived from a read.
  #
  #     2026-10-10 (notes/k4b-drop-address.md K01-K03): the drop HAS been read.
  #     sboot loaded this boot's Image at 0x80118000 (Kernel code 0x80198000 in
  #     /proc/iomem minus the Image header's text_offset 0x80000), and the kernel
  #     window reaches 0x832D0000 (= 0x80198000 + image_size 0x3138000, exactly
  #     the Kernel data end + 1). DECISION (not a read): the FD now runs at
  #     0x83300000 + 0x700000 = 0x83A00000 - above the whole kernel window, below
  #     every reservation that was read (mm_debug 0x91200000, secure_camera
  #     0x96000000, camera 0x99000000, el2_code 0x87000000), inside bank0
  #     (0x80000000 + 0x3AB00000, ends 0xBAB00000), and disjoint from the shim's
  #     own loaded range, which removes the CONFIRMED self-overwrite the 0x80080000
  #     choice had. Still unobserved: nothing has booted at it.
  #
  #     (Superseded: 0x90000000 was first rejected as a 9810-only value; the 9820
  #     prior art uses the same address, which is why it is now adopted.)
  #
  #     0x700000 (7 MB) is the 9810's FV size, carried as a build parameter. ---
  gArmTokenSpaceGuid.PcdFdBaseAddress|0x0000000090000000
  gArmTokenSpaceGuid.PcdFdSize|0x00700000
  gArmTokenSpaceGuid.PcdFvBaseAddress|0x0000000090000000
  gArmTokenSpaceGuid.PcdFvSize|0x00700000

  # --- VFP / NEON: PcdVFPEnabled was removed upstream (handled via build flags). ---
  # gArmTokenSpaceGuid.PcdVFPEnabled|1

  # --- UEFI variable store: memory-backed (non-persistent) for bring-up.
  #     Windows sets BootOrder/BootXXXX here. Persisting them to a real flash
  #     partition is future work (see docs/08-edk2-internals.md). ---
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE

  # --- BDS: no countdown (2026-10-14, user: clean boot screen). ArmPkg's PlatformBootManagerWaitCallback drew a
  #     white progress bar + "Press ESCAPE for boot options" for these seconds; 0 skips the wait (and 3 s per boot).
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|0

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
  # GIC-400 = GICv2 (DT: arm,cortex-a15-gic, GICD/GICC/GICH/GICV). In this edk2,
  # ArmGicDxe.inf is the GICv3 driver (ENTRY_POINT GicV3DxeInitialize: ICC_SRE_EL1,
  # redistributors) and v2 is ArmGicV2Dxe.inf. Building the v3 one reset the phone
  # inside its entry, with no EL1 exception (notes/m8-uefi-image.md B58/B59).
  ArmPkg/Drivers/ArmGicDxe/ArmGicV2Dxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  MdeModulePkg/Universal/Metronome/Metronome.inf
  # ResetSystemRuntimeDxe is back: it needs a ResetSystemLib instance and
  # Beyond0LteResetSystemLib now provides one (PSCI only — the PMU fallback is
  # compiled out because the 9820 intra-block layout is unverified, so whether
  # reset actually works depends on this SoC's monitor implementing PSCI).
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
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
  #     Beyond0LteSimpleFramebufferDxe is live: EXYNOS_FB_BASE is 0xCA000000, read from
  #     the device's own kernel command line, so the screen is drawn and the map
  #     publishes the carveout as device memory. ---
  Platform/Beyond0LtePkg/Drivers/SimpleFramebufferDxe/SimpleFramebufferDxe.inf
  # HiiDatabaseDxe FIRST — produces the 3 HII protocols (Database/String/
  # ConfigRouting) that BdsDxe's UefiHiiServicesLib-injected DEPEX requires.
  # Without it BdsDxe never dispatches and the Bds arch protocol is missing.
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  # GraphicsConsoleDxe removed 2026-10-14 (user: clean boot screen): it cleared the panel and rendered BDS text
  # ("Press ESCAPE for boot options", the firmware version) over the boot screen. ConOut is then serial only (the RAM
  # log); Windows Boot Manager draws through the GOP itself.
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf

  # --- ACPI (FADT/MADT/GTDT/PPTT + DSDT) and the installer that publishes them.
  #     Beyond0LteAcpiPlatformDxe is the ported 9810 installer; without it the
  #     tables ship but Windows never receives the MADT/DSDT, i.e. the port's
  #     bar ("Windows sees CPU and RAM") cannot be met. ---
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  Platform/Beyond0LtePkg/AcpiTables/AcpiTables.inf
  Platform/Beyond0LtePkg/Drivers/Beyond0LteAcpiPlatformDxe/Beyond0LteAcpiPlatformDxe.inf
  # W19: RAM boot log -> UEFILOG.TXT on the ESP (ReadyToBoot + exception handler)
  Platform/Beyond0LtePkg/Drivers/Beyond0LteBootLogDxe/Beyond0LteBootLogDxe.inf
  # M12: ACPM IPC -> S2MPS19 touch rails LDO19M/LDO27M on at ReadyToBoot (notes/m12-acpm.md)
  Platform/Beyond0LtePkg/Drivers/Beyond0LteTouchPowerDxe/Beyond0LteTouchPowerDxe.inf
  # D8a: read the CS47L93 codec / CS35L40 amp IDs into UEFILOG, buses restored (notes/d8-audio-m1.md)
  Platform/Beyond0LtePkg/Drivers/Beyond0LteAudioProbeDxe/Beyond0LteAudioProbeDxe.inf
  # D4c: BT UART role latch + R1 loopback / R2 HCI version into UEFILOG (off unless BEYOND0LTE_BT_UART_PROBE)
  Platform/Beyond0LtePkg/Drivers/Beyond0LteBtUartDxe/Beyond0LteBtUartDxe.inf
  # M16 G3: read-only Mali-G76 probe (PMU G3D status first, GPU_ID only if on) into UEFILOG (notes/gpu-m16-g2g3.md)
  Platform/Beyond0LtePkg/Drivers/Beyond0LteGpuProbeDxe/Beyond0LteGpuProbeDxe.inf

  # --- RAM discovery: Beyond0LteRamManagerDxe is the ported 9810 manager. It
  #     publishes the two verified high banks as GCD SystemMemory, which is a
  #     different thing from the page-table entries in PlatformMemoryMapLib
  #     (see notes/m8-ram-manager.md). ---
  #     DISABLED: UEFI uses bank0 only (notes/m8-uefi-image.md B62/B63). Boot #16 reset (no EL1 exception)
  #     as Beyond0LteRamManagerDxe started on the high banks; the 9810 port never gave UEFI its high
  #     banks either (Star2LteRamManagerDxe adds 32-bit bank-1 ranges only) and its UFS work found RAM
  #     outside bank0 not usable by UEFI at boot (Beyond0LteUfsHcDxe.c A105). Re-enable only with an
  #     on-device read that EL1 may touch 0x880000000+ before the OS.
  # Platform/Beyond0LtePkg/Drivers/Beyond0LteRamManagerDxe/Beyond0LteRamManagerDxe.inf

  # --- Storage + filesystem (Windows ESP on UFS).
  #     Beyond0LteUfsHcDxe is the ported 9810 UFS host controller: it PRODUCES
  #     EDKII_UFS_HOST_CONTROLLER_PROTOCOL, which UfsPassThruDxe binds, so the
  #     chain below it can enumerate. Nothing in it is validated on an S10e.
  #     Every path listed here exists in the checkout. ---
  Platform/Beyond0LtePkg/Drivers/Beyond0LteUfsHcDxe/Beyond0LteUfsHcDxe.inf
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf

  # --- USB host (D1, DRIVERS-MILESTONES.md). Beyond0LteUsbHostDxe does the
  #     9820 PHY/DWC3 bring-up and registers the xHCI window as a
  #     non-discoverable MMIO device; NonDiscoverablePciDeviceDxe synthesizes
  #     the PciIo; stock XhciDxe + UsbBusDxe (hub enumeration is INSIDE
  #     UsbBusDxe in current edk2 — there is no UsbHubDxe module) + UsbKbDxe
  #     + UsbMouseDxe do the rest. Nothing validated on device. ---
  Platform/Beyond0LtePkg/Drivers/Beyond0LteUsbHostDxe/Beyond0LteUsbHostDxe.inf
  MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf

  # --- PCIe root port 0 (M12 WiFi milestone 1, notes/m12-wifi-m1.md).
  #     Beyond0LtePcieDxe trains the link to the BCM4375 WLAN endpoint,
  #     programs iATU MEM+CFG0, sizes/assigns the EP BARs and prints the
  #     VID/DID + chip ID. Gated by BEYOND0LTE_PCIE_WLAN and the 0x68 progress
  #     guard; it registers no PCI root bridge by design (M12-WIFI-DESIGN.md
  #     section 2). Not validated on device. ---
  Platform/Beyond0LtePkg/Drivers/Beyond0LtePcieDxe/Beyond0LtePcieDxe.inf
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
