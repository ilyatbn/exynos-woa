/** @file
  Installs the platform ACPI tables packaged as a freeform FFS.

  Ported from Star2LtePkg/Drivers/Star2LteAcpiPlatformDxe (Exynos 9810 / Galaxy
  S9+) to Beyond0LtePkg (Exynos 9820 / Galaxy S10e, `beyond0lte`).

  NOTHING HERE IS VALIDATED ON AN S10e. The reads the 9810 flow relied on (the
  PRAM breadcrumb address, the architected-timer frequency, the MCT counter base)
  are family facts or 9810-specific facts, not 9820 observations — see the notes
  inline and notes/s10e-live-reads.md §7.3. Under the repo's golden rule the boot
  this produces is Guided until it has been run end to end on an S10e.
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/FirmwareVolume2.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/Acpi63.h>
#include <Library/CacheMaintenanceLib.h>
#include <Platform/Exynos9820.h>
#include <Guid/EventGroup.h>
#include "../../AcpiTables/Beyond0LteAcpiHeader.h"   // m14 BEYOND0LTE_SMP_PARKING
#include "Beyond0LteParking.h"

//
// m10 W17: winload's heap overwrites a callee-saved register slot on the firmware stack
// (SP 0xBAAFF760). Dump the UEFI memory map at ReadyToBoot - what bootmgr/winload start
// from - two entries per panel line, then hold 20 s for a photo. Diagnostic only.
//
#define BEYOND0LTE_MEMMAP_DUMP  0   // W18: map read (boot #29); off - it holds the boot 20 s

//
// FFS file GUID the AcpiTables module is built under. This is the GUID the
// installer reads ACPI sections by, and it MUST equal the FILE_GUID in
// Beyond0LtePkg/AcpiTables/AcpiTables.inf (11111111-2222-3333-4444-555555555521).
// It is a build identity, not a hardware read.
//
STATIC EFI_GUID  mBeyond0LteAcpiTablesFileGuid = {
  0x11111111, 0x2222, 0x3333, { 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x21 }
};

//
// ---------------------------------------------------------------------------
// PRAM breadcrumb diagnostics channel
//
// The 9810 flow wrote single-byte boot breadcrumbs to a hardcoded 0xFED14000 and
// mapped a 64 KB Device window over it. That worked because on the S9+ DRAM ENDED
// at 0xBC800000, so 0xFED14000 was above every bank.
//
// On the 9820 that assumption is FALSE: bank0 is 0x80000000 + 0x80000000, i.e. it
// runs to 0x100000000, so 0xFED14000 is INSIDE conventional DRAM, and inside the
// vendor's debug_snapshot.log_bcm region (0xfeb10000 + 0x400000) — see
// notes/s10e-live-reads.md §7.3. Writing there would scribble into memory the
// loader owns. The 0xFED13F00 scratch word the GTDT patch used is in the same
// zone and is unusable for the same reason.
//
// The breadcrumb channel therefore has to be re-picked before it can be used, and
// the only honest place to pick it from is the vendor DT's reserved-memory node
// (el2_code, el2_earlymem, debug_snapshot, seclog_mem, cp_ram_logging, kaslr,
// abox-rmem). Until that choice is made, the channel is COMPILED OUT rather than
// pointed at a guess — mirroring BEYOND0LTE_PRAM_DIAG 0 in
// PlatformMemoryMapLib/PlatformMemoryMapLib.c:48.
//
#define BEYOND0LTE_PRAM_DIAG  0

#if BEYOND0LTE_PRAM_DIAG
STATIC
VOID
AcpiPramByte (
  IN UINT8  Ch
  )
{
  volatile UINT32  *Words;
  volatile UINT8   *Bytes;
  UINT32           Position;

  //
  // 9810 address, kept for reference only. NOT a 9820 read: it is inside bank0
  // and inside debug_snapshot.log_bcm on this SoC, so this whole channel is
  // compiled out (BEYOND0LTE_PRAM_DIAG 0) until a safe address is chosen.
  //
  Words = (volatile UINT32 *)(UINTN)0xFED14000ULL;
  Bytes = (volatile UINT8  *)(UINTN)0xFED14000ULL;
  if (Words[0] != 0x43474244u) {
    Words[0] = 0x43474244u;
    Words[1] = 0;
    Words[2] = 0;
  }
  Position = Words[2];
  if (Position < 0x3F00u) {
    Bytes[12u + Position] = Ch;
    Words[2] = Position + 1u;
  }
}

STATIC
VOID
AcpiPramStr (
  IN CONST CHAR8  *Str
  )
{
  while (*Str != '\0') {
    AcpiPramByte ((UINT8)*Str++);
  }
}

STATIC
VOID
AcpiPramHex (
  IN UINT32  Val,
  IN UINTN   Nibbles
  )
{
  CONST CHAR8  *Hex;

  Hex = "0123456789abcdef";
  while (Nibbles-- > 0) {
    AcpiPramByte ((UINT8)Hex[(Val >> (Nibbles * 4u)) & 0xFu]);
  }
}

STATIC
VOID
AcpiPramSig (
  IN UINT32  Signature
  )
{
  AcpiPramByte ((UINT8)(Signature & 0xFFu));
  AcpiPramByte ((UINT8)((Signature >> 8) & 0xFFu));
  AcpiPramByte ((UINT8)((Signature >> 16) & 0xFFu));
  AcpiPramByte ((UINT8)((Signature >> 24) & 0xFFu));
}
#endif

//
// --- GTDT memory-mapped counter fix for CNTFRQ_EL0 = 0 ---------------------
//
// [9810 fact] On the S9+ EL3 never programs CNTFRQ_EL0, so it reads 0 on every
// core and Windows' architected timer has no frequency -> deterministic hang
// right after the logo. CNTPCT_EL0 (the count) DOES advance (fed by the Samsung
// MCT global counter at 26 MHz). The ACPI GTDT lets us hand the OS a memory-
// mapped counter description instead: a CntControlBase frame whose CNTFID0 =
// 26 MHz (the missing frequency), and CntReadBase pointed at the real free-
// running MCT count. This is the ARM/SBSA-blessed path for "CNTFRQ not
// correctly initialized": read the frequency from CNTControlBase.CNTFID0.
//
// [9820 UNVERIFIED] Whether the S10e's sboot behaves the same way has NOT been
// established. Exynos9820.h:169 keeps EXYNOS_ARCH_TIMER_FREQ_HZ = 26000000 as
// the 9810's value, unconfirmed on this SoC; the first on-device read of
// CNTFRQ_EL0 (or of the kernel's own ticks) decides it. Do not treat 26 MHz as
// a 9820 fact. The frequency is taken from <Platform/Exynos9820.h> rather than
// restated as a local literal.
//
// The MCT free-running count base below is the 9810's read. There is no
// Exynos9820.h constant for it, so it stays a local placeholder and is UNVERIFIED
// on the 9820 — do not trust it or invent a 9820 number for it.
//
#define BEYOND0LTE_MCT_CNTCV_PA  0x10040100ULL  // UNVERIFIED on 9820 (9810 MCT G_CNT low/high)

//
// EXPERIMENT (2026-07-07, ext39): Mu-Silicium's exynos-refactor "Allow Windows
// Boot" config leaves the GTDT counter bases all-ones (no memory-mapped counter)
// and fixes CNTFRQ_EL0=0 PURELY by patching the kernel's `mrs x8,cntfrq_el0`
// reads to return 26 MHz. We already apply those CNTFRQ register patches (static
// + dynamic T6 scanner). Handing Windows an ADDITIONAL memory-mapped counter
// frame (our synthetic CntControlBase + Samsung MCT CntReadBase) makes the kernel
// prefer the MMIO timer path; if that frame is unusable in the kernel's own VA
// map, the scheduler never ticks -> logo with NO spinning dots (exactly our
// symptom). Set to 0 to match Mu (leave GTDT all-ones, register timer only).
// Set to 1 to restore the memory-mapped counter frame patch.
//
// [9810 experiment] The symptom and the fix were observed on the S9+; the 9820
// has not been driven to the logo yet, so the same switch is carried here as the
// family's known-good setting, not as a 9820 observation.
//
#ifndef BEYOND0LTE_GTDT_MMIO_COUNTER
#define BEYOND0LTE_GTDT_MMIO_COUNTER  0
#endif

// Register offsets within an ARM CNTControlBase frame (DDI0487, memory-mapped).
#define CNT_CR     0x00u   // Counter Control Register (bit0 EN = counter enabled)
#define CNT_SR     0x04u   // Counter Status Register
#define CNT_CV_LO  0x08u   // Counter Count Value [31:0]
#define CNT_CV_HI  0x0Cu   // Counter Count Value [63:32]
#define CNT_FID0   0x20u   // Frequency mode table entry 0 = base frequency (Hz)
#define CNT_FID1   0x24u   // Frequency mode table terminator (0)

STATIC
VOID
Beyond0LtePatchGtdt (
  IN OUT EFI_ACPI_DESCRIPTION_HEADER  *Header
  )
{
  EFI_ACPI_6_3_GENERIC_TIMER_DESCRIPTION_TABLE  *Gtdt;
  EFI_STATUS                                    Status;
  EFI_PHYSICAL_ADDRESS                          FrameAddr;
  volatile UINT8                                *Frame;
  UINT8                                         *Bytes;
  UINT8                                         Sum;
  UINTN                                         Index;

  if (Header->Length < sizeof (EFI_ACPI_6_3_GENERIC_TIMER_DESCRIPTION_TABLE)) {
    return;
  }
  Gtdt = (EFI_ACPI_6_3_GENERIC_TIMER_DESCRIPTION_TABLE *)Header;

  //
  // ext39 experiment: when the memory-mapped counter is disabled, leave the
  // GTDT CntControlBase/CntReadBase as the all-ones placeholders from Gtdt.aslc
  // so Windows uses the CNTVCT_EL0/CNTFRQ_EL0 system-register timer only (the
  // Mu-Silicium "Allow Windows Boot" path). CNTFRQ_EL0=0 is fixed by the kernel
  // mrs-patch, not here. Skipping avoids handing the kernel an MMIO timer frame
  // it may be unable to use in its own VA map (logo hang, no spinner).
  //
  if (BEYOND0LTE_GTDT_MMIO_COUNTER == 0) {
    #if BEYOND0LTE_PRAM_DIAG
    AcpiPramStr ("\n=GTDTPATCH sk=1");
    #endif
    return;
  }


  // memory is carved out of usable RAM so Windows never allocates over it, and
  // it maps the base as MMIO (uncached) via the GTDT physical address.
  //
  FrameAddr = 0;
  Status    = gBS->AllocatePages (AllocateAnyPages, EfiReservedMemoryType, 1, &FrameAddr);
  #if BEYOND0LTE_PRAM_DIAG
  AcpiPramStr ("\n=GTDTPATCH al=");
  AcpiPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
  #endif
  if (EFI_ERROR (Status)) {
    #if BEYOND0LTE_PRAM_DIAG
    AcpiPramHex ((UINT32)(Status & 0xFFFFu), 4);
    #endif
    return;
  }

  Frame = (volatile UINT8 *)(UINTN)FrameAddr;
  ZeroMem ((VOID *)(UINTN)FrameAddr, SIZE_4KB);
  *(volatile UINT32 *)(Frame + CNT_CR)    = 0x1u;                      // counter enabled
  *(volatile UINT32 *)(Frame + CNT_SR)    = 0x0u;
  *(volatile UINT32 *)(Frame + CNT_CV_LO) = 0x0u;                      // count is read via CntReadBase (MCT)
  *(volatile UINT32 *)(Frame + CNT_CV_HI) = 0x0u;
  *(volatile UINT32 *)(Frame + CNT_FID0)  = EXYNOS_ARCH_TIMER_FREQ_HZ; // base frequency (UNVERIFIED on 9820)
  *(volatile UINT32 *)(Frame + CNT_FID1)  = 0x0u;                      // frequency table terminator

  //
  // Flush the frame to DRAM: Windows maps this base as uncached MMIO and must
  // observe the values we wrote through the firmware's cached mapping.
  //
  WriteBackDataCacheRange ((VOID *)(UINTN)FrameAddr, SIZE_4KB);

  Gtdt->CntControlBasePhysicalAddress = FrameAddr;
  Gtdt->CntReadBasePhysicalAddress    = BEYOND0LTE_MCT_CNTCV_PA;

  //
  // Recompute the ACPI 8-bit checksum over the whole (now-patched) table.
  //
  Gtdt->Header.Checksum = 0;
  Bytes = (UINT8 *)Header;
  Sum   = 0;
  for (Index = 0; Index < Header->Length; Index++) {
    Sum = (UINT8)(Sum + Bytes[Index]);
  }
  Gtdt->Header.Checksum = (UINT8)(0 - Sum);

  //
  // Stamp the patched values into a fixed scratch word (0xFED13F00, in the
  // mapped pram-diag zone, OUTSIDE the DBGC trace ring at 0xFED14000). The
  // survivable =EXC exception dump reads this back to confirm the patch applied
  // with the right values even after the trace ring has wrapped past this early
  // marker.
  //
  // [9810] That zone sat above every DRAM bank on the S9+. On the 9820 both
  // 0xFED13F00 and 0xFED14000 are inside bank0 and inside debug_snapshot.log_bcm,
  // so this whole block is compiled out with the PRAM channel until a safe
  // address is chosen.
  //
  #if BEYOND0LTE_PRAM_DIAG
  {
    volatile UINT32  *Scratch = (volatile UINT32 *)(UINTN)0xFED13F00ULL;
    Scratch[0] = 0x47544450u;                     // 'GTDP' magic
    Scratch[1] = (UINT32)FrameAddr;               // CntControlBase (low 32)
    Scratch[2] = (UINT32)BEYOND0LTE_MCT_CNTCV_PA; // CntReadBase (low 32)
    Scratch[3] = EXYNOS_ARCH_TIMER_FREQ_HZ;       // CNTFID0 frequency
  }

  AcpiPramStr ("cc=");
  AcpiPramHex ((UINT32)(FrameAddr >> 32), 8);
  AcpiPramHex ((UINT32)FrameAddr, 8);
  AcpiPramStr (" cr=");
  AcpiPramHex ((UINT32)BEYOND0LTE_MCT_CNTCV_PA, 8);
  AcpiPramStr (" fq=");
  AcpiPramHex (EXYNOS_ARCH_TIMER_FREQ_HZ, 8);
  #endif
}

#if BEYOND0LTE_MEMMAP_DUMP
STATIC
VOID
EFIAPI
Beyond0LteDumpMemoryMap (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  UINTN                  Count;
  UINTN                  Index;
  UINT64                 Sp;
  EFI_STATUS             Status;

  __asm__ __volatile__ ("mov %0, sp" : "=r" (Sp));
  MapSize = 0;
  Map     = NULL;
  Status  = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_ERROR, "MMAP: GetMemoryMap %r\n", Status));
    return;
  }

  MapSize += 8 * DescSize;
  Status   = gBS->AllocatePool (EfiBootServicesData, MapSize, (VOID **)&Map);
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MMAP: GetMemoryMap %r\n", Status));
    gBS->FreePool (Map);
    return;
  }

  Count = MapSize / DescSize;
  DEBUG ((DEBUG_ERROR, "\nMMAP n=%u sp=%lx (type base+pages)\n", (UINT32)Count, Sp));
  for (Index = 0; Index < Count; Index++) {
    Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescSize);
    DEBUG ((
      DEBUG_ERROR,
      "%a%x %lx+%lx",
      (Index & 1) ? "  " : "",
      (UINT32)Desc->Type,
      Desc->PhysicalStart,
      Desc->NumberOfPages
      ));
    if ((Index & 1) || (Index + 1 == Count)) {
      DEBUG ((DEBUG_ERROR, "\n"));
    }
  }

  DEBUG ((DEBUG_ERROR, "MMAP end - holding 20 s\n"));
  gBS->FreePool (Map);
  gBS->Stall (20 * 1000 * 1000);
}

#endif

EFI_STATUS
EFIAPI
Beyond0LteAcpiPlatformEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                      Status;
  EFI_ACPI_TABLE_PROTOCOL         *AcpiTable;
  EFI_HANDLE                      *FvHandles;
  UINTN                           FvCount;
  UINTN                           FvIndex;
  UINTN                           InstalledCount;

  AcpiTable      = NULL;
  FvHandles      = NULL;
  FvCount        = 0;
  InstalledCount = 0;

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  #if BEYOND0LTE_MEMMAP_DUMP
  {
    EFI_EVENT  ReadyToBoot;

    gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Beyond0LteDumpMemoryMap, NULL,
                        &gEfiEventReadyToBootGuid, &ReadyToBoot);
  }
  #endif

  Status = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiFirmwareVolume2ProtocolGuid,
                   NULL,
                   &FvCount,
                   &FvHandles
                   );
  #if BEYOND0LTE_PRAM_DIAG
  AcpiPramStr ("\n=ACPIINST A22 fvst=");
  AcpiPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
  AcpiPramHex ((UINT32)(Status & 0xFFFFu), 4);
  AcpiPramStr (" n=");
  AcpiPramHex ((UINT32)FvCount, 2);
  #endif

  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (FvIndex = 0; FvIndex < FvCount; FvIndex++) {
    EFI_FIRMWARE_VOLUME2_PROTOCOL  *Fv;
    UINTN                          SectionInstance;

    Status = gBS->HandleProtocol (FvHandles[FvIndex], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv);
    if (EFI_ERROR (Status)) {
      continue;
    }

    for (SectionInstance = 0; SectionInstance < 16; SectionInstance++) {
      VOID                         *TableBuffer;
      UINTN                        TableSize;
      UINT32                       AuthenticationStatus;
      EFI_ACPI_DESCRIPTION_HEADER  *Header;
      UINTN                        TableKey;

      TableBuffer          = NULL;
      TableSize            = 0;
      AuthenticationStatus = 0;
      Status = Fv->ReadSection (
                     Fv,
                     &mBeyond0LteAcpiTablesFileGuid,
                     EFI_SECTION_RAW,
                     SectionInstance,
                     &TableBuffer,
                     &TableSize,
                     &AuthenticationStatus
                     );
      if (EFI_ERROR (Status)) {
        if ((Status == EFI_NOT_FOUND) && (SectionInstance != 0)) {
          break;
        }
        continue;
      }

      Header = (EFI_ACPI_DESCRIPTION_HEADER *)TableBuffer;
      if ((TableSize >= sizeof (EFI_ACPI_DESCRIPTION_HEADER)) &&
          (Header->Length <= TableSize) &&
          (Header->Length >= sizeof (EFI_ACPI_DESCRIPTION_HEADER))) {
        //
        // Give Windows a timer frequency via the GTDT memory-mapped counter
        // frames (CNTFRQ_EL0 reads 0 on this SoC — a 9810 fact; UNVERIFIED on
        // the 9820, see the note above).
        //
        if (Header->Signature == EFI_ACPI_6_3_GENERIC_TIMER_DESCRIPTION_TABLE_SIGNATURE) {
          Beyond0LtePatchGtdt (Header);
        }
        #if BEYOND0LTE_SMP_PARKING
        //
        // m14: start the parked secondaries and publish their mailboxes in this
        // MADT before it is installed (the installer recomputes the checksum).
        //
        if (Header->Signature == EFI_ACPI_6_3_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE) {
          Beyond0LteParkSecondaries (Header);
        }
        #endif
        TableKey = 0;
        Status = AcpiTable->InstallAcpiTable (AcpiTable, Header, Header->Length, &TableKey);
        #if BEYOND0LTE_PRAM_DIAG
        AcpiPramStr (" s");
        AcpiPramHex ((UINT32)SectionInstance, 1);
        AcpiPramByte ('=');
        AcpiPramSig (Header->Signature);
        AcpiPramStr (" len=");
        AcpiPramHex (Header->Length, 4);
        AcpiPramStr (" st=");
        AcpiPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
        AcpiPramHex ((UINT32)(Status & 0xFFFFu), 4);
        #endif
        if (!EFI_ERROR (Status)) {
          InstalledCount++;
        }
      }

      if (TableBuffer != NULL) {
        gBS->FreePool (TableBuffer);
      }
    }
  }

  if (FvHandles != NULL) {
    gBS->FreePool (FvHandles);
  }

  #if BEYOND0LTE_PRAM_DIAG
  AcpiPramStr (" done=");
  AcpiPramHex ((UINT32)InstalledCount, 2);
  AcpiPramByte ('\n');
  #endif

  return (InstalledCount != 0) ? EFI_SUCCESS : EFI_NOT_FOUND;
}
