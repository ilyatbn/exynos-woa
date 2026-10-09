/** @file
  Runtime reset and power-off support for Exynos 9820 / Galaxy S10e
  (beyond0lte).

  Ported from Star2LtePkg/Library/Star2LteResetSystemLib (Exynos 9810 / Galaxy
  S9+). PSCI is the preferred path and is architecture-defined, so it is carried
  unchanged. The two Exynos-specific channels are NOT carried:

   - the PMU fallback (SWRESET, PS_HOLD_CONTROL) - the 9820 PMU base is a read
     (system-controller@0x15860000, EXYNOS_PMU_BASE in Exynos9820.h:185) but the
     intra-block offsets were never read on this SoC, so pointing at
     base+0x400 / base+0x330C would be the 9810's layout assumed onto a different
     block. The fallback is COMPILED OUT behind BEYOND0LTE_PMU_RESET 0 rather
     than retargeted to a guess.
   - the PRAM breadcrumb channel at 0xFED14000 - on this SoC DRAM bank0 runs
     0x80000000..0x100000000, so 0xFED14000 is inside conventional memory and
     inside the vendor's debug_snapshot.log_bcm region. Same hazard already
     recorded in Beyond0LteAcpiPlatformDxe.c, PlatformMemoryMapLib.c and
     Beyond0LteUfsHcDxe.c; the replacement address has to come from the vendor
     DT's reserved-memory node before the channel can be used at all
     (notes/s10e-live-reads.md §7.3).

  NOTHING HERE IS VALIDATED ON AN S10e. Whether this SoC's EL3 monitor
  implements PSCI_SYSTEM_RESET/RESET2/SYSTEM_OFF at all is unverified
  (Exynos9820.h keeps ArmBootArch PSCI_COMPLIANT as a 9810 value), so a
  successful build says nothing about reset working; the boot this produces is
  Guided, not Ready.
**/

#include <PiDxe.h>

#include <IndustryStandard/ArmStdSmc.h>

#include <Library/ArmLib.h>
#include <Library/ArmMonitorLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Platform/Exynos9820.h>

//
// ---------------------------------------------------------------------------
// Exynos-specific fallback channels
//
// BEYOND0LTE_PMU_RESET  - the PMU SWRESET / PS_HOLD fallback. The 9820 PMU BASE
//   is a verified read; the intra-block OFFSETS are not, so the fallback stays
//   compiled out until the 9820's system-controller layout is read.
// BEYOND0LTE_PRAM_DIAG  - the ramoops-style breadcrumb channel. 0xFED14000 is
//   inside bank0 DRAM on this SoC, so the channel has no legal address yet.
//
// Both are switches, not retargets: with them off, every reset path in this
// library is the architected PSCI one.
//
#define BEYOND0LTE_PMU_RESET  0
#define BEYOND0LTE_PRAM_DIAG  0

#if BEYOND0LTE_PMU_RESET
//
// 9810 offsets, kept for reference only. NOT 9820 reads: the 9810 PMU was at
// 0x14060000 and this SoC's system-controller is at 0x15860000, and nothing in
// this checkout has read the 9820 block's interior.
//
#define BEYOND0LTE_PMU_SWRESET_OFFSET          0x0400U
#define BEYOND0LTE_PMU_PS_HOLD_CONTROL_OFFSET  0x330CU
#define BEYOND0LTE_PMU_PS_HOLD_ENABLE          BIT8
#endif

#if BEYOND0LTE_PRAM_DIAG
//
// 9810 address, kept for reference only. 0xFED14000 is inside bank0 (0x80000000
// + 0x80000000) and inside debug_snapshot.log_bcm (0xfeb10000 + 0x400000) here,
// so this channel is compiled out until a reserved carveout is chosen.
//
#define BEYOND0LTE_PRAM_BASE        0xFED14000ULL
#define BEYOND0LTE_PRAM_MAGIC       0x43474244U
#define BEYOND0LTE_PRAM_HEADER_SIZE 12U
#define BEYOND0LTE_PRAM_CAPACITY    0x3F00U
#endif

#if BEYOND0LTE_PMU_RESET
STATIC volatile UINT32  *mSwReset;
STATIC volatile UINT32  *mPsHoldControl;
STATIC BOOLEAN          mSwResetVirtualReady;
STATIC BOOLEAN          mPsHoldVirtualReady;
#endif

#if BEYOND0LTE_PRAM_DIAG
STATIC volatile UINT8   *mPram;
STATIC BOOLEAN          mPramVirtualReady;
#endif

#if (BEYOND0LTE_PMU_RESET || BEYOND0LTE_PRAM_DIAG)
STATIC EFI_EVENT        mVirtualAddressChangeEvent;
#endif

#if (BEYOND0LTE_PMU_RESET || BEYOND0LTE_PRAM_DIAG)
STATIC
EFI_STATUS
MarkRangeRuntime (
  IN UINTN  Address,
  IN UINTN  Size
  )
{
  EFI_PHYSICAL_ADDRESS             PageAddress;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  EFI_STATUS                       Status;
  UINT64                           Attributes;
  UINT64                           Capabilities;
  UINT64                           RangeLength;

  PageAddress = Address & ~(EFI_PHYSICAL_ADDRESS)EFI_PAGE_MASK;
  RangeLength = EFI_PAGES_TO_SIZE (
                  EFI_SIZE_TO_PAGES ((Address & EFI_PAGE_MASK) + Size)
                  );
  Status      = gDS->GetMemorySpaceDescriptor (PageAddress, &Descriptor);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Descriptor.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    Status = gDS->AddMemorySpace (
                    EfiGcdMemoryTypeMemoryMappedIo,
                    PageAddress,
                    RangeLength,
                    EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Attributes = EFI_MEMORY_UC | EFI_MEMORY_RUNTIME;
  } else {
    if (Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) {
      return EFI_UNSUPPORTED;
    }

    if (((Descriptor.Attributes & EFI_CACHE_ATTRIBUTE_MASK) != 0) &&
        ((Descriptor.Attributes & EFI_CACHE_ATTRIBUTE_MASK) != EFI_MEMORY_UC))
    {
      return EFI_UNSUPPORTED;
    }

    Capabilities = Descriptor.Capabilities | EFI_MEMORY_UC | EFI_MEMORY_RUNTIME;
    if (Capabilities != Descriptor.Capabilities) {
      Status = gDS->SetMemorySpaceCapabilities (
                      PageAddress,
                      RangeLength,
                      Capabilities
                      );
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    Attributes = Descriptor.Attributes | EFI_MEMORY_UC | EFI_MEMORY_RUNTIME;
  }

  return gDS->SetMemorySpaceAttributes (
                PageAddress,
                RangeLength,
                Attributes
                );
}

STATIC
VOID
EFIAPI
VirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  VOID        *Address;

  //
  // Only the channels that are compiled in have pointers to relocate. The
  // event itself is still created when a channel exists; with both off this
  // callback does nothing.
  //
#if BEYOND0LTE_PMU_RESET
  Address = (VOID *)(UINTN)mSwReset;
  Status  = EfiConvertPointer (0, &Address);
  if (!EFI_ERROR (Status)) {
    mSwReset             = (volatile UINT32 *)Address;
    mSwResetVirtualReady = TRUE;
  }

  Address = (VOID *)(UINTN)mPsHoldControl;
  Status  = EfiConvertPointer (0, &Address);
  if (!EFI_ERROR (Status)) {
    mPsHoldControl       = (volatile UINT32 *)Address;
    mPsHoldVirtualReady  = TRUE;
  }
#endif

#if BEYOND0LTE_PRAM_DIAG
  Address = (VOID *)(UINTN)mPram;
  Status  = EfiConvertPointer (0, &Address);
  if (!EFI_ERROR (Status)) {
    mPram                = (volatile UINT8 *)Address;
    mPramVirtualReady    = TRUE;
  }
#endif

  (VOID)Event;
  (VOID)Context;
  (VOID)Status;
  (VOID)Address;
}
#endif

EFI_STATUS
EFIAPI
Beyond0LteResetSystemLibConstructor (
  IN   EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  #if (BEYOND0LTE_PMU_RESET || BEYOND0LTE_PRAM_DIAG)
  EFI_STATUS  Status;
  #endif


#if BEYOND0LTE_PMU_RESET
  //
  // EXYNOS_PMU_BASE is a verified 9820 read (Exynos9820.h:185); the offsets
  // inside the block are NOT, which is why BEYOND0LTE_PMU_RESET is 0.
  //
  mSwReset       = (volatile UINT32 *)(UINTN)(EXYNOS_PMU_BASE + BEYOND0LTE_PMU_SWRESET_OFFSET);
  mPsHoldControl = (volatile UINT32 *)(UINTN)(EXYNOS_PMU_BASE + BEYOND0LTE_PMU_PS_HOLD_CONTROL_OFFSET);

  Status = MarkRangeRuntime ((UINTN)mSwReset, sizeof (*mSwReset));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ResetSystem: SWRESET runtime mapping failed: %r\n", Status));
  }

  Status = MarkRangeRuntime ((UINTN)mPsHoldControl, sizeof (*mPsHoldControl));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ResetSystem: PS_HOLD runtime mapping failed: %r\n", Status));
  }
#endif

#if BEYOND0LTE_PRAM_DIAG
  mPram = (volatile UINT8 *)(UINTN)BEYOND0LTE_PRAM_BASE;

  Status = MarkRangeRuntime (
             (UINTN)mPram,
             BEYOND0LTE_PRAM_HEADER_SIZE + BEYOND0LTE_PRAM_CAPACITY
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ResetSystem: PRAM runtime mapping failed: %r\n", Status));
  }
#endif

#if (BEYOND0LTE_PMU_RESET || BEYOND0LTE_PRAM_DIAG)
  Status = gBS->CreateEvent (
              EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE,
              TPL_NOTIFY,
              VirtualAddressChange,
              NULL,
              &mVirtualAddressChangeEvent
              );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ResetSystem: virtual-address event failed: %r\n", Status));
  }
#endif

  //
  // Preserve the PSCI ResetSystem services regardless of the optional
  // Exynos-specific channels.
  //
  (VOID)ImageHandle;
  (VOID)SystemTable;
  return EFI_SUCCESS;
}

#if BEYOND0LTE_PRAM_DIAG
STATIC
VOID
RuntimePramByte (
  IN UINT8  Value
  )
{
  volatile UINT32  *Words;
  UINT32           Position;

  if (EfiGoneVirtual () && !mPramVirtualReady) {
    return;
  }

  Words = (volatile UINT32 *)(UINTN)mPram;
  if (Words[0] != BEYOND0LTE_PRAM_MAGIC) {
    Words[0] = BEYOND0LTE_PRAM_MAGIC;
    Words[1] = 0;
    Words[2] = 0;
  }

  Position = Words[2];
  if (Position < BEYOND0LTE_PRAM_CAPACITY) {
    mPram[BEYOND0LTE_PRAM_HEADER_SIZE + Position] = Value;
    Words[2]                                      = Position + 1U;
  }
}

STATIC
VOID
RuntimePramString (
  IN CONST CHAR8  *String
  )
{
  while (*String != '\0') {
    RuntimePramByte ((UINT8)*String++);
  }
}

STATIC
VOID
RuntimePramHex64 (
  IN UINT64  Value
  )
{
  STATIC CONST CHAR8  Hex[] = "0123456789abcdef";
  UINTN               Index;

  for (Index = 0; Index < 16; Index++) {
    RuntimePramByte ((UINT8)Hex[(Value >> (60U - (Index * 4U))) & 0xFU]);
  }
}

STATIC
VOID
RuntimePramMarker (
  IN CONST CHAR8  *Tag,
  IN UINT64       Value
  )
{
  RuntimePramString ("\n=");
  RuntimePramString (Tag);
  RuntimePramByte ((UINT8)' ');
  RuntimePramHex64 (Value);
  ArmDataSynchronizationBarrier ();
}
#else
STATIC
VOID
RuntimePramMarker (
  IN CONST CHAR8  *Tag,
  IN UINT64       Value
  )
{
  //
  // No breadcrumb channel until a reserved 9820 address is chosen (see the
  // file header). The call sites stay so the reset flow is unchanged by the
  // channel being switched back on.
  //
  (VOID)Tag;
  (VOID)Value;
}
#endif

STATIC
VOID
WaitForReset (
  VOID
  )
{
  DisableInterrupts ();
  for (;;) {
    ArmCallWFI ();
  }
}

STATIC
VOID
AttemptPsciSystemReset (
  VOID
  )
{
  ARM_MONITOR_ARGS  Args;

  Args.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_RESET;
  ArmMonitorCall (&Args);
  RuntimePramMarker ("PSR", Args.Arg0);
}

STATIC
VOID
AttemptPsciSystemReset2 (
  VOID
  )
{
  ARM_MONITOR_ARGS  Args;

  Args.Arg0 = ARM_SMC_ID_PSCI_FEATURES;
  Args.Arg1 = ARM_SMC_ID_PSCI_SYSTEM_RESET2_AARCH64;
  ArmMonitorCall (&Args);
  RuntimePramMarker ("P2F", Args.Arg0);
  if (Args.Arg0 == ARM_SMC_PSCI_RET_SUCCESS) {
    Args.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_RESET2_AARCH64;
    Args.Arg1 = 0;
    Args.Arg2 = 0;
    ArmMonitorCall (&Args);
    RuntimePramMarker ("P2R", Args.Arg0);
  }
}

STATIC
VOID
ExynosPmuReset (
  VOID
  )
{
#if BEYOND0LTE_PMU_RESET
  if (!EfiGoneVirtual () || mSwResetVirtualReady) {
    RuntimePramMarker ("SWR", BIT0);
    RuntimePramMarker ("SWA", BIT0);
    MmioWrite32 ((UINTN)mSwReset, BIT0);
    ArmDataSynchronizationBarrier ();
    ArmInstructionSynchronizationBarrier ();
  }
#endif
  //
  // With the PMU fallback compiled out this is only the "the PSCI call did not
  // return, so stop touching memory" guard that the 9810 flow already needed.
  //
  WaitForReset ();
}

STATIC
VOID
ExynosPmuPowerOff (
  VOID
  )
{
#if BEYOND0LTE_PMU_RESET
  UINT32  Value;

  if (!EfiGoneVirtual () || mPsHoldVirtualReady) {
    Value = MmioRead32 ((UINTN)mPsHoldControl);
    RuntimePramMarker ("PHB", Value);
    Value &= ~BEYOND0LTE_PMU_PS_HOLD_ENABLE;
    RuntimePramMarker ("PHA", Value);
    MmioWrite32 (
      (UINTN)mPsHoldControl,
      Value
      );
    ArmDataSynchronizationBarrier ();
    ArmInstructionSynchronizationBarrier ();
  }
#endif
  WaitForReset ();
}

VOID
EFIAPI
ResetCold (
  VOID
  )
{
  AttemptPsciSystemReset ();
  ExynosPmuReset ();
}

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  AttemptPsciSystemReset2 ();
  AttemptPsciSystemReset ();
  ExynosPmuReset ();
}

VOID
EFIAPI
ResetShutdown (
  VOID
  )
{
  ARM_MONITOR_ARGS  Args;

  Args.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_OFF;
  ArmMonitorCall (&Args);
  RuntimePramMarker ("PSO", Args.Arg0);
  ExynosPmuPowerOff ();
}

VOID
EFIAPI
ResetPlatformSpecific (
  IN UINTN  DataSize,
  IN VOID   *ResetData
  )
{
  (VOID)DataSize;
  (VOID)ResetData;
  ResetCold ();
}

VOID
EFIAPI
ResetSystem (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  UINT32  State;

  State = (UINT32)ResetType;
#if BEYOND0LTE_PMU_RESET
  if (EfiGoneVirtual ()) {
    State |= BIT8;
  }

  if (mSwResetVirtualReady) {
    State |= BIT9;
  }

  if (mPsHoldVirtualReady) {
    State |= BIT10;
  }
#endif
#if BEYOND0LTE_PRAM_DIAG
  if (mPramVirtualReady) {
    State |= BIT11;
  }
#endif

  RuntimePramMarker ("RSE", State);
  RuntimePramMarker ("RSS", ResetStatus);
  RuntimePramMarker ("RSD", DataSize);

  switch (ResetType) {
    case EfiResetWarm:
      ResetWarm ();
      break;

    case EfiResetCold:
      ResetCold ();
      break;

    case EfiResetShutdown:
      ResetShutdown ();
      break;

    case EfiResetPlatformSpecific:
      ResetPlatformSpecific (DataSize, ResetData);
      break;

    default:
      RuntimePramMarker ("UNS", ResetType);
      WaitForReset ();
  }
}
