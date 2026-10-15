/** @file
  Beyond0LteBootLogDxe - make every boot readable from TWRP (notes/m10-windows-deploy-plan.md W19).

  Beyond0LteFbSerialPortLib appends every DEBUG byte of every module to a RAM ring
  (Beyond0LteBootLog.h). This driver copies that ring to \UEFILOG.TXT on the ESP (the FAT volume
  holding \EFI\Microsoft\Boot\BCD, i.e. CACHE on the S10e):
    - at ReadyToBoot, i.e. just before Windows Boot Manager starts;
    - from its own synchronous-exception / SError handler, after printing a dump in the same shape
      as CpuExceptionHandlerLib's (banner, PC + backtrace with image names, registers, the code
      words around ELR, the loaded-image list, stack).
  After the exception flush it parks the CPU (BEYOND0LTE_AUTO_RECOVERY reboots into TWRP instead,
  once the reboot-reason registers are verified).

  The exception path calls file I/O from exception context. That is only sound while boot
  services exist (before ExitBootServices) and when the fault is not inside the storage stack; a
  second fault while flushing parks the CPU at once.
**/

#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/PeCoffGetEntryPointLib.h>
#include <Library/DebugLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/Cpu.h>
#include <Protocol/DebugSupport.h>
#include <Guid/EventGroup.h>
#include <Guid/DebugImageInfoTable.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Protocol/BlockIo.h>
#include <Platform/Exynos9820.h>

#include "../../Library/Beyond0LteFbSerialPortLib/Beyond0LteBootLog.h"

#define BEYOND0LTE_AUTO_RECOVERY  1   // W21: after the dump + flush, reboot into TWRP
#define BEYOND0LTE_WINDOWS_PAIR   1   // W23: the boot after a Windows run saves \WINPRAM.BIN and goes to TWRP
#ifndef BEYOND0LTE_DSS_DUMP
#define BEYOND0LTE_DSS_DUMP  0   // m14: after a Windows death (stay.flg path) save log_s2d / log_arraydump heads to the ESP
#endif
//
// m14 HW access-flag test (notes/m14-smp.md): 1 = at ExitBootServices make ntoskrnl 22621.1's HAFDBS check
// report 0, so Windows keeps TCR_EL1.HA off on every core (like the stock Linux, CONFIG_ARM64_HW_AFDBM off).
//
#ifndef BEYOND0LTE_STAY_MAX_DEATHS
#define BEYOND0LTE_STAY_MAX_DEATHS  2   // stay.flg is ignored after this many Windows deaths in a row (-> TWRP)
#endif
#ifndef BEYOND0LTE_NT_NO_HWAF
#define BEYOND0LTE_NT_NO_HWAF  0
#endif
#define BEYOND0LTE_HANDOFF_WATCHDOG  1   // W24: arm WDT cl0 (82 s) at ExitBootServices - bring-up only
//
// W37 (user: "disable the reboot watchdog (add a flag)"): 1 = the 82 s watchdog stays armed into Windows
// (unattended loop: a hang or the driver's feed cap returns the phone to TWRP); 0 = ReadyToBoot still
// re-arms it (bootmgr/winload hangs are still rescued) but ExitBootServices STOPS it, so a running Windows
// is never reset by it. The driver's WTCNT reloads do not re-enable a stopped timer (WTCON EN = 0).
//
#ifndef BEYOND0LTE_WINDOWS_WATCHDOG
#define BEYOND0LTE_WINDOWS_WATCHDOG  1   // 1 = unattended sessions only (hang -> 82 s -> TWRP); 0 for interactive use (HANDOFF §29)
#endif

STATIC
VOID
RebootToRecovery (
  VOID
  );

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  );

#define LOG_FILE_NAME  L"\\UEFILOG.TXT"

STATIC BOOLEAN  mInException = FALSE;

/** Root of the FAT volume that carries \EFI\Microsoft\Boot\BCD, or NULL. **/
STATIC
EFI_FILE_PROTOCOL *
OpenEspRoot (
  VOID
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *Probe;
  EFI_FILE_PROTOCOL                *Found;

  Found = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) {
    return NULL;
  }

  for (Index = 0; (Index < Count) && (Found == NULL); Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
        EFI_ERROR (Fs->OpenVolume (Fs, &Root)))
    {
      continue;
    }

    if (!EFI_ERROR (Root->Open (Root, &Probe, L"\\EFI\\Microsoft\\Boot\\BCD", EFI_FILE_MODE_READ, 0))) {
      Probe->Close (Probe);
      Found = Root;
    } else {
      Root->Close (Root);
    }
  }

  FreePool (Handles);
  return Found;
}

STATIC
EFI_STATUS
WriteAll (
  IN EFI_FILE_PROTOCOL  *File,
  IN CONST VOID         *Data,
  IN UINTN              Size
  )
{
  UINTN       Len;
  EFI_STATUS  Status;

  if (Size == 0) {
    return EFI_SUCCESS;
  }

  Len    = Size;
  Status = File->Write (File, &Len, (VOID *)Data);
  if (!EFI_ERROR (Status) && (Len != Size)) {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}

/** Rewrites \UEFILOG.TXT on the ESP with the whole RAM ring, oldest byte first. **/
STATIC
EFI_STATUS
FlushBootLog (
  IN CONST CHAR8  *Reason
  )
{
  BEYOND0LTE_BOOTLOG_HEADER  *Hdr;
  UINT8                      *Data;
  EFI_FILE_PROTOCOL          *Root;
  EFI_FILE_PROTOCOL          *File;
  CHAR8                      Line[160];
  UINTN                      Len;
  EFI_STATUS                 Status;

  Hdr  = (BEYOND0LTE_BOOTLOG_HEADER *)(UINTN)BEYOND0LTE_BOOTLOG_BASE;
  Data = (UINT8 *)(UINTN)(BEYOND0LTE_BOOTLOG_BASE + sizeof (BEYOND0LTE_BOOTLOG_HEADER));
  if ((Hdr->Magic != BEYOND0LTE_BOOTLOG_MAGIC) || (Hdr->Head >= BEYOND0LTE_BOOTLOG_DATA_SIZE)) {
    return EFI_NOT_FOUND;
  }

  Root = OpenEspRoot ();
  if (Root == NULL) {
    return EFI_NOT_FOUND;
  }

  if (!EFI_ERROR (Root->Open (Root, &File, LOG_FILE_NAME, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
    File->Delete (File);   // truncate: closes the handle too
  }

  Status = Root->Open (
                   Root,
                   &File,
                   LOG_FILE_NAME,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   0
                   );
  if (!EFI_ERROR (Status)) {
    Len = AsciiSPrint (
            Line,
            sizeof (Line),
            "=== Beyond0Lte UEFI boot log: boot %u, flush at %a, %u bytes%a ===\n",
            Hdr->BootCount,
            Reason,
            Hdr->Wrapped ? BEYOND0LTE_BOOTLOG_DATA_SIZE : Hdr->Head,
            Hdr->Wrapped ? " (ring wrapped, oldest lost)" : ""
            );
    Status = WriteAll (File, Line, Len);
    if (!EFI_ERROR (Status) && Hdr->Wrapped) {
      Status = WriteAll (File, Data + Hdr->Head, BEYOND0LTE_BOOTLOG_DATA_SIZE - Hdr->Head);
    }

    if (!EFI_ERROR (Status)) {
      Status = WriteAll (File, Data, Hdr->Head);
    }

    File->Flush (File);
    File->Close (File);
  }

  Root->Flush (Root);
  Root->Close (Root);
  return Status;
}

//
// W20 read test: bootmgr's code in RAM did not match the file at the crash site (the words at
// ELR were the file's bytes 0x3018 further on). Read \EFI\Boot\bootaa64.efi once in one call (the
// way LoadImage reads it) and once in 4 KB calls, and log a CRC32 per 64 KB of each, to compare
// with the file on the PC.
//
#define BEYOND0LTE_READ_TEST  0   // W20 answered: one-shot and 4 KB reads both match the PC file

#if BEYOND0LTE_READ_TEST
STATIC
VOID
ReadTestLogCrcs (
  IN CONST CHAR8  *Tag,
  IN UINT8        *Buf,
  IN UINTN        Size
  )
{
  UINTN   Off;
  UINTN   Len;
  UINT32  Crc;

  DEBUG ((DEBUG_ERROR, "RT %a size 0x%lx\n", Tag, (UINT64)Size));
  for (Off = 0; Off < Size; Off += SIZE_64KB) {
    Len = MIN (SIZE_64KB, Size - Off);
    gBS->CalculateCrc32 (Buf + Off, Len, &Crc);
    DEBUG ((DEBUG_ERROR, "RT %a %lx %08x\n", Tag, (UINT64)Off, Crc));
  }
}

STATIC
VOID
ReadTest (
  VOID
  )
{
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *File;
  UINT8              *Buf;
  UINTN              Size;
  UINTN              Len;
  UINTN              Off;
  UINT64             Pos;
  EFI_STATUS         Status;

  Root = OpenEspRoot ();
  if ((Root == NULL) ||
      EFI_ERROR (Root->Open (Root, &File, L"\\EFI\\Boot\\bootaa64.efi", EFI_FILE_MODE_READ, 0)))
  {
    DEBUG ((DEBUG_ERROR, "RT: cannot open bootaa64.efi\n"));
    return;
  }

  Pos = MAX_UINT64;
  File->SetPosition (File, Pos);   // end of file
  File->GetPosition (File, &Pos);
  Size = (UINTN)Pos;
  Buf  = AllocatePages (EFI_SIZE_TO_PAGES (Size));
  if (Buf != NULL) {
    File->SetPosition (File, 0);
    Len    = Size;
    Status = File->Read (File, &Len, Buf);
    DEBUG ((DEBUG_ERROR, "RT one-shot read %r len 0x%lx buf %p\n", Status, (UINT64)Len, Buf));
    ReadTestLogCrcs ("one", Buf, Len);

    SetMem (Buf, Size, 0xA5);
    File->SetPosition (File, 0);
    for (Off = 0; Off < Size; Off += Len) {
      Len    = MIN (SIZE_4KB, Size - Off);
      Status = File->Read (File, &Len, Buf + Off);
      if (EFI_ERROR (Status) || (Len == 0)) {
        DEBUG ((DEBUG_ERROR, "RT 4k read %r at 0x%lx\n", Status, (UINT64)Off));
        break;
      }
    }

    ReadTestLogCrcs ("4k", Buf, Size);
    FreePages (Buf, EFI_SIZE_TO_PAGES (Size));
  }

  File->Close (File);
  Root->Close (Root);
}

#endif

//
// m15 UFS speed split (notes/m15-ufs-throughput.md): time raw ReadBlocks on the whole-disk BlockIo (UEFI's own UFS
// path = Beyond0LteUfsHcDxe + UfsPassThru) before Windows starts. Same sizes as s10eperf raw/sweep. Read-only.
// UEFI fast (~hundreds of MB/s) => the controller/clocks are fine and the Windows miniport is the limiter;
// UEFI slow too => controller/PHY/clock configuration.
//
#ifndef BEYOND0LTE_UFS_SPEED_TEST
#define BEYOND0LTE_UFS_SPEED_TEST  0
#endif

#if BEYOND0LTE_UFS_SPEED_TEST
STATIC
VOID
UfsSpeedTest (
  VOID
  )
{
  EFI_HANDLE             *Handles;
  UINTN                  Count;
  UINTN                  Index;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_BLOCK_IO_PROTOCOL  *Disk;
  UINT8                  *Buf;
  UINTN                  Size;
  UINTN                  Reads;
  UINTN                  I;
  UINT64                 T0;
  UINT64                 Ns;
  EFI_STATUS             Status;

  Disk = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles))) {
    DEBUG ((DEBUG_ERROR, "UfsSpeed: no BlockIo\n"));
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo))) {
      continue;
    }

    if (!BlockIo->Media->LogicalPartition && BlockIo->Media->MediaPresent &&
        ((Disk == NULL) || (BlockIo->Media->LastBlock > Disk->Media->LastBlock)))
    {
      Disk = BlockIo;
    }
  }

  FreePool (Handles);
  if (Disk == NULL) {
    DEBUG ((DEBUG_ERROR, "UfsSpeed: no whole-disk BlockIo\n"));
    return;
  }

  DEBUG ((DEBUG_ERROR, "UfsSpeed: disk block %u last 0x%lx align %u\n", Disk->Media->BlockSize,
          Disk->Media->LastBlock, Disk->Media->IoAlign));
  Buf = AllocatePages (EFI_SIZE_TO_PAGES (SIZE_1MB));
  if (Buf == NULL) {
    return;
  }

  for (Size = SIZE_4KB; Size <= SIZE_1MB; Size *= 2) {
    Reads = (Size >= SIZE_256KB) ? 16 : 128;
    T0    = GetPerformanceCounter ();
    for (I = 0; I < Reads; I++) {
      Status = Disk->ReadBlocks (Disk, Disk->Media->MediaId, (EFI_LBA)((I * Size) / Disk->Media->BlockSize), Size, Buf);
      if (EFI_ERROR (Status)) {
        break;
      }
    }

    Ns = GetTimeInNanoSecond (GetPerformanceCounter () - T0);
    DEBUG ((DEBUG_ERROR, "UfsSpeed: %7u B x %3u: %r, %lu us per read, %lu KB/s\n", (UINT32)Size, (UINT32)I, Status,
            (I != 0) ? (Ns / 1000) / I : 0, (Ns != 0) ? ((UINT64)I * Size * 1000000ULL) / Ns : 0));
  }

  //
  // m15 u3: same 64 KiB x 128 read into three buffer locations - (a) the high allocation above, (b) a bank0
  // allocation below the pool, (c) the Windows driver's reserved UFS DMA pool past its workspace
  // (BEYOND0LTE_UFS_DMA_POOL_BASE + 0x28000, inside the 0x40000 pool, after the 0x24000 workspace). Windows reads
  // 64 KiB in ~4 ms vs UEFI ~0.73 ms; if (c) is slow the pool region itself is the limiter.
  //
  {
    EFI_PHYSICAL_ADDRESS  Low;
    UINT8                 *Bufs[3];
    CONST CHAR8           *Names[3];
    UINTN                 B;

    Low = 0xA0000000ULL;
    Bufs[0]  = Buf;
    Names[0] = "high";
    Bufs[1]  = EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, EFI_SIZE_TO_PAGES (SIZE_64KB), &Low))
               ? NULL : (UINT8 *)(UINTN)Low;
    Names[1] = "bank0";
    Bufs[2]  = (UINT8 *)(UINTN)(BEYOND0LTE_UFS_DMA_POOL_BASE + 0x28000ULL);
    Names[2] = "pool";
    for (B = 0; B < 3; B++) {
      if (Bufs[B] == NULL) {
        DEBUG ((DEBUG_ERROR, "UfsSpeed: %a buffer unavailable\n", Names[B]));
        continue;
      }

      T0 = GetPerformanceCounter ();
      for (I = 0; I < 128; I++) {
        Status = Disk->ReadBlocks (Disk, Disk->Media->MediaId, (EFI_LBA)((I * SIZE_64KB) / Disk->Media->BlockSize),
                                   SIZE_64KB, Bufs[B]);
        if (EFI_ERROR (Status)) {
          break;
        }
      }

      Ns = GetTimeInNanoSecond (GetPerformanceCounter () - T0);
      DEBUG ((DEBUG_ERROR, "UfsSpeed: 64K into %a %p x %u: %r, %lu us per read\n", Names[B], Bufs[B], (UINT32)I, Status,
              (I != 0) ? (Ns / 1000) / I : 0));
    }

    if (Bufs[1] != NULL) {
      gBS->FreePages (Low, EFI_SIZE_TO_PAGES (SIZE_64KB));
    }
  }

  FreePages (Buf, EFI_SIZE_TO_PAGES (SIZE_1MB));
}

#endif

/** Writes Size bytes of memory at Address to Name on the ESP (overwrites). Logs the result. **/
STATIC
VOID
DumpMemoryToEsp (
  IN CONST CHAR16  *Name,
  IN UINTN         Address,
  IN UINTN         Size
  )
{
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *File;
  EFI_STATUS         Status;

  Root = OpenEspRoot ();
  if (Root == NULL) {
    DEBUG ((DEBUG_ERROR, "DUMP %s: no ESP\n", Name));
    return;
  }

  if (!EFI_ERROR (Root->Open (Root, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
    File->Delete (File);
  }

  Status = Root->Open (Root, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (!EFI_ERROR (Status)) {
    Status = WriteAll (File, (VOID *)Address, Size);
    File->Flush (File);
    File->Close (File);
  }

  Root->Close (Root);
  DEBUG ((DEBUG_ERROR, "DUMP %s 0x%lx+0x%lx -> %r\n", Name, (UINT64)Address, (UINT64)Size, Status));
}

/** m14 PC sampler: save the ring the parking code kept from the previous boot (PREV) as \PCSAMP.BIN, once. **/
STATIC
VOID
DumpPcSamples (
  VOID
  )
{
  volatile UINT32  *Prev;

  Prev = (volatile UINT32 *)(UINTN)BEYOND0LTE_PCSAMP_PREV;
  if (Prev[0] != BEYOND0LTE_PCSAMP_MAGIC) {
    return;
  }

  DumpMemoryToEsp (L"\\PCSAMP.BIN", (UINTN)BEYOND0LTE_PCSAMP_PREV, (UINTN)BEYOND0LTE_PCSAMP_SIZE);
  Prev[0] = 0;
}

#if BEYOND0LTE_NT_NO_HWAF
//
// ntoskrnl.exe 10.0.22621.1 arm64, RVA 0xA7DCD0..0xA7DD1F: the HAFDBS check (unique in the file). Sections are
// page aligned, so the run starts at page offset 0xCD0 wherever winload put that page. Word at +0x14 is changed.
//
STATIC CONST UINT8  mNtHwAfSig[0x50] = {
  0x7F, 0x23, 0x03, 0xD5, 0xFD, 0x7B, 0xBE, 0xA9, 0xFD, 0x03, 0x00, 0x91, 0xBF, 0x13, 0x00, 0xB9,
  0x28, 0x07, 0x38, 0xD5, 0x0A, 0x0D, 0x00, 0x12, 0xAA, 0x13, 0x00, 0xB9, 0x02, 0x00, 0x00, 0x14,
  0xAA, 0x13, 0x40, 0xB9, 0x6A, 0x00, 0x00, 0x35, 0x00, 0x00, 0x80, 0x52, 0x08, 0x00, 0x00, 0x14,
  0x48, 0x20, 0x38, 0xD5, 0x09, 0x01, 0x59, 0xB2, 0x5F, 0x09, 0x00, 0x71, 0x43, 0x00, 0x00, 0x54,
  0x29, 0xF9, 0x57, 0x92, 0x49, 0x20, 0x18, 0xD5, 0x20, 0x00, 0x80, 0x52, 0xFD, 0x7B, 0xC2, 0xA8
};
#define NT_HWAF_PAGE_OFF    0xCD0u
#define NT_HWAF_WORD_OFF    0x14u
#define NT_HWAF_OLD         0x12000D0Au   // and w10, w8, #0xf   (w10 = HAFDBS)
#define NT_HWAF_NEW         0x5280000Au   // mov w10, #0
#define NT_HWAF_MAX_RANGES  64u

STATIC UINT64  mHwAfBase[NT_HWAF_MAX_RANGES];
STATIC UINT64  mHwAfEnd[NT_HWAF_MAX_RANGES];
STATIC UINTN   mHwAfRanges;

/** ReadyToBoot: keep the RAM ranges winload can still allocate from (adjacent ones merged). **/
STATIC
VOID
NtHwAfCollect (
  VOID
  )
{
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  UINT64                 End;

  MapSize = 0;
  Map     = NULL;
  if (gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVersion) != EFI_BUFFER_TOO_SMALL) {
    return;
  }

  MapSize += 8 * DescSize;
  Map      = AllocatePool (MapSize);
  if ((Map == NULL) || EFI_ERROR (gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion))) {
    if (Map != NULL) {
      FreePool (Map);
    }

    return;
  }

  mHwAfRanges = 0;
  for (Desc = Map; (UINT8 *)Desc < (UINT8 *)Map + MapSize; Desc = NEXT_MEMORY_DESCRIPTOR (Desc, DescSize)) {
    if ((Desc->Type != EfiConventionalMemory) && (Desc->Type != EfiLoaderCode) && (Desc->Type != EfiLoaderData) &&
        (Desc->Type != EfiBootServicesCode) && (Desc->Type != EfiBootServicesData))
    {
      continue;
    }

    End = Desc->PhysicalStart + EFI_PAGES_TO_SIZE ((UINTN)Desc->NumberOfPages);
    if ((mHwAfRanges != 0) && (mHwAfEnd[mHwAfRanges - 1] == Desc->PhysicalStart)) {
      mHwAfEnd[mHwAfRanges - 1] = End;
    } else if (mHwAfRanges < NT_HWAF_MAX_RANGES) {
      mHwAfBase[mHwAfRanges] = Desc->PhysicalStart;
      mHwAfEnd[mHwAfRanges]  = End;
      mHwAfRanges++;
    }
  }

  FreePool (Map);
  DEBUG ((DEBUG_ERROR, "HWAF: %u RAM range(s) to search at ExitBootServices\n", (UINT32)mHwAfRanges));
}

/**
  ExitBootServices: winload has loaded ntoskrnl but not run it. Find the check (page offset 0xCD0 of a page in the
  recorded ranges) and change its HAFDBS read to 0 - only when exactly one copy is found. Progress page words:
  [8] copies found, [9]/[10] PA of the changed word (lo/hi), 0 when nothing was changed.
**/
STATIC
VOID
NtHwAfApply (
  VOID
  )
{
  volatile UINT32  *Progress;
  UINT64           Page;
  UINT64           Found;
  UINTN            Range;
  UINT32           Count;
  UINT32           First;

  Progress = (volatile UINT32 *)(UINTN)0x8FE0F000ULL;
  Found    = 0;
  Count    = 0;
  CopyMem (&First, mNtHwAfSig, sizeof (First));
  for (Range = 0; Range < mHwAfRanges; Range++) {
    for (Page = ALIGN_VALUE (mHwAfBase[Range], SIZE_4KB); Page + SIZE_4KB <= mHwAfEnd[Range]; Page += SIZE_4KB) {
      if ((*(volatile UINT32 *)(UINTN)(Page + NT_HWAF_PAGE_OFF) == First) &&
          (CompareMem ((VOID *)(UINTN)(Page + NT_HWAF_PAGE_OFF), mNtHwAfSig, sizeof (mNtHwAfSig)) == 0))
      {
        Found = Page + NT_HWAF_PAGE_OFF + NT_HWAF_WORD_OFF;
        Count++;
      }
    }
  }

  Progress[8]  = Count;
  Progress[9]  = 0;
  Progress[10] = 0;
  if ((Count == 1) && (*(volatile UINT32 *)(UINTN)Found == NT_HWAF_OLD)) {
    *(volatile UINT32 *)(UINTN)Found = NT_HWAF_NEW;
    WriteBackDataCacheRange ((VOID *)(UINTN)Found, sizeof (UINT32));
    InvalidateInstructionCacheRange ((VOID *)(UINTN)Found, sizeof (UINT32));
    Progress[9]  = (UINT32)Found;
    Progress[10] = (UINT32)(Found >> 32);
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
}
#endif

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;

  ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[1] = 0x71;   // W28 progress: ReadyToBoot

  //
  // Boot #31: the first ReadyToBoot (PlatformRecovery) comes before BDS has connected the disk,
  // so no FAT volume existed yet ("Not Found"). Connect everything first.
  //
  if (!EFI_ERROR (gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles))) {
    for (Index = 0; Index < Count; Index++) {
      gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    }

    FreePool (Handles);
  }

 #if BEYOND0LTE_READ_TEST
  ReadTest ();
 #endif

 #if BEYOND0LTE_UFS_SPEED_TEST
  UfsSpeedTest ();
 #endif

  //
  // M16 G10 rescue path (notes/gpu-m16-g10.md): \S10EDIAG\nogpu.flg on the ESP (vfat, writable from TWRP)
  // hides DSDT GPU0 for this boot. GPU0._STA is 0x0F only while progress word [4] (0x8FE0F010) holds 'GPU0'
  // (Beyond0LteGpuProbeDxe); overwriting it with 'NOGP' (0x50474F4E) before ExitBootServices means Windows never
  // enumerates ACPI\SAMS9828, so no GPU driver loads and Basic Display keeps the GOP framebuffer. Only ever
  // hides, never shows; repeated ReadyToBoot signals are harmless.
  //
  {
    EFI_FILE_PROTOCOL  *EspRoot;
    EFI_FILE_PROTOCOL  *NoGpu;

    EspRoot = OpenEspRoot ();
    if (EspRoot != NULL) {
      if (!EFI_ERROR (EspRoot->Open (EspRoot, &NoGpu, L"\\S10EDIAG\\nogpu.flg", EFI_FILE_MODE_READ, 0))) {
        NoGpu->Close (NoGpu);
        ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[4] = 0x50474F4E;   // 'NOGP': GPU0._STA -> 0
        __asm__ __volatile__ ("dsb sy" ::: "memory");
        DEBUG ((DEBUG_ERROR, "BootLog: \\S10EDIAG\\nogpu.flg present - DSDT GPU0 hidden (word [4] = NOGP)\n"));
      }

      //
      // Audio rescue switch: \S10EDIAG\noaudio.flg hides DSDT AUD0 (word [5], 0x8FE0F014 = 'NOAU'). Written on
      // every boot (0 when the file is absent) because nothing else clears word [5].
      //
      if (!EFI_ERROR (EspRoot->Open (EspRoot, &NoGpu, L"\\S10EDIAG\\noaudio.flg", EFI_FILE_MODE_READ, 0))) {
        NoGpu->Close (NoGpu);
        ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[5] = 0x55414F4E;   // 'NOAU': AUD0._STA -> 0
        DEBUG ((DEBUG_ERROR, "BootLog: \\S10EDIAG\\noaudio.flg present - DSDT AUD0 hidden (word [5] = NOAU)\n"));
      } else {
        ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[5] = 0;
      }

      __asm__ __volatile__ ("dsb sy" ::: "memory");
      EspRoot->Close (EspRoot);
    }
  }

 #if BEYOND0LTE_WINDOWS_PAIR
  //
  // W23 boot pair: before handing over to Windows, leave \WINRUN.FLG on the ESP. When Windows
  // resets (BSOD auto-restart -> ResetSystem -> SWRESET) the next boot finds the flag, saves the
  // Windows driver's PRAM ring to \WINPRAM.BIN and goes to TWRP instead of looping, so every
  // Windows attempt ends in TWRP with its evidence on the ESP. Once per boot (ReadyToBoot can be
  // signalled more than once).
  //
  {
    STATIC BOOLEAN     mPairDone = FALSE;
    EFI_FILE_PROTOCOL  *Root;
    EFI_FILE_PROTOCOL  *Flag;

    if (!mPairDone) {
      mPairDone = TRUE;
      Root      = OpenEspRoot ();
      if (Root != NULL) {
        if ((((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[0] == 0x80) &&
            !EFI_ERROR (Root->Open (Root, &Flag, L"\\WINRUN.FLG", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)))
        {
          //
          // W31: the previous Windows run ended with an OS-requested reset (Beyond0LteResetSystemLib
          // stored 0x80) - e.g. Setup rebooting between passes. Keep its evidence but let it go on.
          //
          EFI_FILE_PROTOCOL  *ToRecovery;

          Flag->Close (Flag);
          //
          // 2026-10-12: \S10EDIAG\torecovery.flg = the agent's CLEAN return to TWRP (collect.cmd writes it, then
          // `shutdown /r`). A clean restart writes the registry hives and file caches to disk; the old hard reset
          // (s10ediag flushreset) lost unflushed SYSTEM-hive changes (driver installs, MaliG76Wddm Parameters).
          //
          if (!EFI_ERROR (Root->Open (Root, &ToRecovery, L"\\S10EDIAG\\torecovery.flg", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
            ToRecovery->Delete (ToRecovery);
            if (!EFI_ERROR (Root->Open (Root, &Flag, L"\\WINRUN.FLG", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
              Flag->Delete (Flag);
            }

            Root->Close (Root);
            DEBUG ((DEBUG_ERROR, "BootLog: clean Windows restart with torecovery.flg - saving its PRAM, then recovery\n"));
            DumpMemoryToEsp (L"\\WINPRAM.BIN", (UINTN)BEYOND0LTE_WINPRAM_BASE, (UINTN)BEYOND0LTE_WINPRAM_DUMP);
            DumpPcSamples ();   // m14 PC sampler ring of that run, if one was kept
            ZeroMem ((VOID *)(UINTN)BEYOND0LTE_WINPRAM_BASE, (UINTN)BEYOND0LTE_WINPRAM_DUMP);
            FlushBootLog ("after-windows");
            RebootToRecovery ();
          }

          Root->Close (Root);
          ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[12] = 0;   // clean restart: the death counter starts over
          DEBUG ((DEBUG_ERROR, "BootLog: Windows asked for this restart - saving its PRAM and continuing\n"));
          DumpMemoryToEsp (L"\\WINPRAM-RESTART.BIN", (UINTN)BEYOND0LTE_WINPRAM_BASE, (UINTN)BEYOND0LTE_WINPRAM_DUMP);
          DumpPcSamples ();   // m14 PC sampler ring of that run, if one was kept
          Root = OpenEspRoot ();
        } else if (!EFI_ERROR (Root->Open (Root, &Flag, L"\\WINRUN.FLG", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
          EFI_FILE_PROTOCOL  *Stay;
          BOOLEAN            Interactive;

          Flag->Delete (Flag);
          //
          // 2026-10-12 (user): a Windows shut down and later powered on (button or charger) landed in TWRP - a cold
          // boot loses the 0x80 restart marker, so it looked like a crash. With \S10EDIAG\stay.flg (interactive use)
          // keep the evidence but continue into Windows; the unattended loop (no stay.flg) keeps going to TWRP.
          //
          Interactive = !EFI_ERROR (Root->Open (Root, &Stay, L"\\S10EDIAG\\stay.flg", EFI_FILE_MODE_READ, 0));
          if (Interactive) {
            Stay->Close (Stay);
          }

          //
          // 2026-10-15 (user: "add some sort of safeguard in case of crashes like these to reboot to twrp"): with
          // stay.flg a Windows that dies on every boot looped forever (hwaf8: watchdog reset -> Windows -> hang ->
          // ...). Progress word [12] counts unexplained Windows deaths ('DE' << 16 | n). RAM survives the watchdog
          // and key resets but not a cold power-on, and a clean OS restart clears it, so normal interactive use
          // never trips it. The second death without a cold boot or clean restart in between goes to TWRP.
          //
          if (Interactive) {
            volatile UINT32  *Progress;
            UINT32           Deaths;

            Progress     = (volatile UINT32 *)(UINTN)0x8FE0F000ULL;
            Deaths       = ((Progress[12] >> 16) == 0x4445u) ? (Progress[12] & 0xFFFFu) + 1 : 1;
            Progress[12] = 0x44450000u | Deaths;
            if (Deaths >= BEYOND0LTE_STAY_MAX_DEATHS) {
              DEBUG ((DEBUG_ERROR, "BootLog: Windows died %u times in a row - ignoring stay.flg, going to TWRP\n", Deaths));
              Progress[12] = 0;
              Interactive  = FALSE;
            }
          }

          Root->Close (Root);
          if (Interactive) {
            DEBUG ((DEBUG_ERROR, "BootLog: previous boot ran Windows (no restart marker) - interactive (stay.flg): "
                    "saving its PRAM and continuing\n"));
            DumpMemoryToEsp (L"\\WINPRAM.BIN", (UINTN)BEYOND0LTE_WINPRAM_BASE, (UINTN)BEYOND0LTE_WINPRAM_DUMP);
            DumpPcSamples ();   // m14 PC sampler ring of that run, if one was kept
 #if BEYOND0LTE_DSS_DUMP
            //
            // m14: Samsung debug-snapshot areas a debug reset may have filled (live DT: log_s2d 0xEB310000,
            // log_arraydump 0xEC810000 - both inside bank2, mapped as RAM here). First 1 MiB of each.
            //
            DumpMemoryToEsp (L"\\S2D.BIN", (UINTN)0xEB310000ULL, SIZE_1MB);
            DumpMemoryToEsp (L"\\ARRAYDUMP.BIN", (UINTN)0xEC810000ULL, SIZE_1MB);
 #endif
            //
            // This boot continues into Windows: keep the progress page (0x8FE0F000, the last 4 KiB of the
            // dump). Its words [2..5] are this boot's 'USBH'/'WLAN'/'GPU0'/'NOAU' (set by the DXEs before
            // ReadyToBoot) and the DSDT _STA of XHC0/WLAN/GPU0/AUD0 reads them; zeroing the whole dump hid
            // XHC0, WLAN and GPU0 from Windows after every hard reset, and un-hid AUD0 despite noaudio.flg
            // ('NOAU' hides it, 0 shows it) (2026-10-13, notes/m12-wifi-wificx.md).
            //
            ZeroMem ((VOID *)(UINTN)BEYOND0LTE_WINPRAM_BASE, (UINTN)BEYOND0LTE_WINPRAM_DUMP - SIZE_4KB);
            Root = OpenEspRoot ();
          } else {
            DEBUG ((DEBUG_ERROR, "BootLog: previous boot ran Windows - saving its PRAM, then recovery\n"));
            DumpMemoryToEsp (L"\\WINPRAM.BIN", (UINTN)BEYOND0LTE_WINPRAM_BASE, (UINTN)BEYOND0LTE_WINPRAM_DUMP);
            DumpPcSamples ();   // m14 PC sampler ring of that run, if one was kept
            ZeroMem ((VOID *)(UINTN)BEYOND0LTE_WINPRAM_BASE, (UINTN)BEYOND0LTE_WINPRAM_DUMP);   // one run per dump
            FlushBootLog ("after-windows");
            RebootToRecovery ();
          }
        }

        if (Root != NULL) {
          if (!EFI_ERROR (Root->Open (Root, &Flag, L"\\WINRUN.FLG", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
            Flag->Flush (Flag);
            Flag->Close (Flag);
          }

          Root->Close (Root);
        }
      }
    }
  }
 #endif

  //
  // W35: publish the reserved UFS DMA workspace to the Windows driver (record in the PRAM ring
  // page, uncached window - see Beyond0LteBootLog.h). The driver consumes it on every boot.
  //
  {
    volatile UINT32  *Rec;

    Rec    = (volatile UINT32 *)(UINTN)BEYOND0LTE_UFS_DMA_POOL_RECORD;
    Rec[1] = (UINT32)BEYOND0LTE_UFS_DMA_POOL_BASE;
    Rec[2] = BEYOND0LTE_UFS_DMA_POOL_MAGIC ^ (UINT32)BEYOND0LTE_UFS_DMA_POOL_BASE ^
             BEYOND0LTE_UFS_DMA_POOL_WORKSPACE ^ BEYOND0LTE_UFS_DMA_POOL_GUARD;
    Rec[0] = BEYOND0LTE_UFS_DMA_POOL_MAGIC;
  }

 #if BEYOND0LTE_HANDOFF_WATCHDOG
  //
  // W25: the firmware reached the OS hand-off - cancel SEC's "recovery" dead man's switch:
  // reason back to normal, watchdog re-armed for another 82 s (re-armed again at EBS).
  //
  OnExitBootServices (NULL, NULL);
 #endif
 #if BEYOND0LTE_NT_NO_HWAF
  NtHwAfCollect ();
 #endif
  DEBUG ((DEBUG_ERROR, "BootLog: ReadyToBoot flush\n"));
  Status = FlushBootLog ("ReadyToBoot");
  DEBUG ((DEBUG_ERROR, "BootLog: %s -> %r\n", LOG_FILE_NAME, Status));
}

/** PDB name and base of the loaded image containing Pc, from the debug image info table. **/
STATIC
CONST CHAR8 *
ImageOf (
  IN  UINTN  Pc,
  OUT UINTN  *Base
  )
{
  EFI_DEBUG_IMAGE_INFO_TABLE_HEADER  *Table;
  EFI_LOADED_IMAGE_PROTOCOL          *Image;
  UINTN                              Index;
  CHAR8                              *Pdb;

  *Base = 0;
  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiDebugImageInfoTableGuid, (VOID **)&Table)) ||
      (Table == NULL) || (Table->EfiDebugImageInfoTable == NULL))
  {
    return NULL;
  }

  for (Index = 0; Index < Table->TableSize; Index++) {
    if ((Table->EfiDebugImageInfoTable[Index].NormalImage == NULL) ||
        (Table->EfiDebugImageInfoTable[Index].NormalImage->LoadedImageProtocolInstance == NULL))
    {
      continue;
    }

    Image = Table->EfiDebugImageInfoTable[Index].NormalImage->LoadedImageProtocolInstance;
    if ((Pc >= (UINTN)Image->ImageBase) && (Pc < (UINTN)Image->ImageBase + Image->ImageSize)) {
      *Base = (UINTN)Image->ImageBase;
      Pdb   = (CHAR8 *)PeCoffLoaderGetPdbPointer (Image->ImageBase);
      return (Pdb != NULL) ? Pdb : "(no pdb)";
    }
  }

  return NULL;
}

STATIC
VOID
PrintPc (
  IN CONST CHAR8  *Tag,
  IN UINTN        Pc
  )
{
  CONST CHAR8  *Name;
  UINTN        Base;

  Name = ImageOf (Pc, &Base);
  if (Name != NULL) {
    DEBUG ((DEBUG_ERROR, "%a 0x%lx = %a+0x%lx\n", Tag, (UINT64)Pc, Name, (UINT64)(Pc - Base)));
  } else {
    DEBUG ((DEBUG_ERROR, "%a 0x%lx (no UEFI image)\n", Tag, (UINT64)Pc));
  }
}

STATIC
VOID
ListImages (
  VOID
  )
{
  EFI_DEBUG_IMAGE_INFO_TABLE_HEADER  *Table;
  EFI_LOADED_IMAGE_PROTOCOL          *Image;
  UINTN                              Index;
  CHAR8                              *Pdb;

  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiDebugImageInfoTableGuid, (VOID **)&Table)) ||
      (Table == NULL) || (Table->EfiDebugImageInfoTable == NULL))
  {
    return;
  }

  DEBUG ((DEBUG_ERROR, "Loaded images:\n"));
  for (Index = 0; Index < Table->TableSize; Index++) {
    if ((Table->EfiDebugImageInfoTable[Index].NormalImage == NULL) ||
        (Table->EfiDebugImageInfoTable[Index].NormalImage->LoadedImageProtocolInstance == NULL))
    {
      continue;
    }

    Image = Table->EfiDebugImageInfoTable[Index].NormalImage->LoadedImageProtocolInstance;
    Pdb   = (CHAR8 *)PeCoffLoaderGetPdbPointer (Image->ImageBase);
    DEBUG ((
      DEBUG_ERROR,
      "  0x%lx +0x%lx %a\n",
      (UINT64)(UINTN)Image->ImageBase,
      Image->ImageSize,
      (Pdb != NULL) ? Pdb : "(no pdb)"
      ));
  }
}

#if BEYOND0LTE_AUTO_RECOVERY
/**
  Samsung's own `reboot recovery`, minus the kernel-only steps (notes/s10e-reboot-wdt-constants.md):
  INFORM3 = 0x12345670 | 4 (sboot reads it as REBOOT_MODE 4 = recovery), RSTSTAT = 0, then
  SWRESET PMU+0x3A00 = 2 - the same reset sboot itself performs.
**/
STATIC
VOID
RebootToRecovery (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "BootLog: rebooting to recovery\n"));
  MmioWrite32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_INFORM3_OFFSET, EXYNOS_REBOOT_REASON_BASE | EXYNOS_REBOOT_MODE_RECOVERY);
  MmioWrite32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_RSTSTAT_OFFSET, 0);
  __asm__ __volatile__ ("dsb sy\n isb" ::: "memory");
  MmioWrite32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_SWRESET_OFFSET, EXYNOS_PMU_SWRESET_VALUE);
  for ( ; ;) {
    __asm__ __volatile__ ("wfi");
  }
}

#endif

STATIC
VOID
EFIAPI
BootLogExceptionHandler (
  IN     EFI_EXCEPTION_TYPE  ExceptionType,
  IN OUT EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  EFI_SYSTEM_CONTEXT_AARCH64  *C;
  UINT64                      *Fp;
  UINT64                      *Sp;
  UINT32                      *Code;
  INTN                        Index;
  UINTN                       Depth;
  EFI_STATUS                  Status;

  if (mInException) {
    CpuDeadLoop ();   // fault while dumping/flushing: nothing safe is left to do
  }

  mInException = TRUE;
  C            = SystemContext.SystemContextAArch64;

  // "Exception at 0x" puts the panel console into exception mode (Beyond0LteFbSerialPortLib).
  DEBUG ((
    DEBUG_ERROR,
    "\n\n%a Exception at 0x%016lx\n",
    (ExceptionType == EXCEPT_AARCH64_SERROR) ? "SError" : "Synchronous",
    C->ELR
    ));
  DEBUG ((DEBUG_ERROR, "ESR 0x%08lx (EC 0x%02x ISS 0x%07x) FAR 0x%016lx\n", C->ESR,
          (UINT32)((C->ESR >> 26) & 0x3F), (UINT32)(C->ESR & 0x1FFFFFF), C->FAR));
  DEBUG ((DEBUG_ERROR, "SP 0x%016lx SPSR 0x%08lx\n", C->SP, C->SPSR));
  PrintPc ("PC", (UINTN)C->ELR);
  PrintPc ("LR", (UINTN)C->LR);
  Fp = (UINT64 *)(UINTN)C->FP;
  for (Depth = 0; (Fp != NULL) && (Depth < 16) && (((UINTN)Fp & 7) == 0); Depth++) {
    PrintPc ("  <-", (UINTN)Fp[1]);
    Fp = (UINT64 *)(UINTN)Fp[0];
  }

  DEBUG ((DEBUG_ERROR, " X0 %016lx  X1 %016lx  X2 %016lx  X3 %016lx\n", C->X0, C->X1, C->X2, C->X3));
  DEBUG ((DEBUG_ERROR, " X4 %016lx  X5 %016lx  X6 %016lx  X7 %016lx\n", C->X4, C->X5, C->X6, C->X7));
  DEBUG ((DEBUG_ERROR, " X8 %016lx  X9 %016lx X10 %016lx X11 %016lx\n", C->X8, C->X9, C->X10, C->X11));
  DEBUG ((DEBUG_ERROR, "X12 %016lx X13 %016lx X14 %016lx X15 %016lx\n", C->X12, C->X13, C->X14, C->X15));
  DEBUG ((DEBUG_ERROR, "X16 %016lx X17 %016lx X18 %016lx X19 %016lx\n", C->X16, C->X17, C->X18, C->X19));
  DEBUG ((DEBUG_ERROR, "X20 %016lx X21 %016lx X22 %016lx X23 %016lx\n", C->X20, C->X21, C->X22, C->X23));
  DEBUG ((DEBUG_ERROR, "X24 %016lx X25 %016lx X26 %016lx X27 %016lx\n", C->X24, C->X25, C->X26, C->X27));
  DEBUG ((DEBUG_ERROR, "X28 %016lx  FP %016lx  LR %016lx\n", C->X28, C->FP, C->LR));

  Code = (UINT32 *)(UINTN)(C->ELR & ~(UINT64)3);
  DEBUG ((DEBUG_ERROR, "Code at ELR-0x20:"));
  for (Index = -8; Index < 8; Index++) {
    DEBUG ((DEBUG_ERROR, "%a%08x", (Index == 0) ? " >" : " ", Code[Index]));
  }

  DEBUG ((DEBUG_ERROR, "\n"));
  ListImages ();

  Sp = (UINT64 *)(UINTN)(C->SP & ~(UINT64)7);
  DEBUG ((DEBUG_ERROR, "Stack:\n"));
  for (Index = -16; Index < 48; Index += 4) {
    DEBUG ((DEBUG_ERROR, "%c%lx: %016lx %016lx %016lx %016lx\n", (Index == 0) ? '>' : ' ',
            (UINT64)(UINTN)(Sp + Index), Sp[Index], Sp[Index + 1], Sp[Index + 2], Sp[Index + 3]));
  }

  //
  // W21: is the faulting image mapped where the UEFI debug table says it was loaded? Translate
  // its base and ELR through the CURRENT stage-1 tables, and dump the region to \IMGDUMP.BIN.
  //
  {
    CONST CHAR8  *Name;
    UINTN        ImgBase;
    UINT64       Ttbr0;
    UINT64       Tcr;
    UINT64       Sctlr;
    UINT64       Va[2];
    UINT64       Par;
    UINTN        Index2;

    __asm__ __volatile__ ("mrs %0, ttbr0_el1" : "=r" (Ttbr0));
    __asm__ __volatile__ ("mrs %0, tcr_el1" : "=r" (Tcr));
    __asm__ __volatile__ ("mrs %0, sctlr_el1" : "=r" (Sctlr));
    DEBUG ((DEBUG_ERROR, "TTBR0 %016lx TCR %016lx SCTLR %016lx\n", Ttbr0, Tcr, Sctlr));
    Name  = ImageOf ((UINTN)C->ELR, &ImgBase);
    Va[0] = (Name != NULL) ? ImgBase : (C->ELR & ~(UINT64)0xFFF);
    Va[1] = C->ELR;
    for (Index2 = 0; Index2 < 2; Index2++) {
      __asm__ __volatile__ ("at s1e1r, %1\n isb\n mrs %0, par_el1" : "=r" (Par) : "r" (Va[Index2]) : "memory");
      DEBUG ((DEBUG_ERROR, "AT S1E1R %016lx -> PAR %016lx\n", Va[Index2], Par));
    }

    if (Name != NULL) {
      DumpMemoryToEsp (L"\\IMGDUMP.BIN", ImgBase - SIZE_64KB, SIZE_2MB + SIZE_128KB);
    }
  }

  Status = FlushBootLog ("exception");
  DEBUG ((DEBUG_ERROR, "BootLog: exception flush -> %r\n", Status));

 #if BEYOND0LTE_AUTO_RECOVERY
  RebootToRecovery ();
 #endif
  CpuDeadLoop ();
}

#if BEYOND0LTE_HANDOFF_WATCHDOG
/**
  W24: arm watchdog cl0 for 82 s (the maximum) as boot services end. Windows does not service it, so any
  hang - or a BSOD whose restart never reaches our ResetSystem - resets the SoC with INFORM3
  "normal", and the boot pair (WINRUN.FLG) turns that next boot into "save WINPRAM.BIN, go to
  TWRP". A Windows that boots fine is also reset after 82 s while this is on (bring-up only).
  Same sequence as the stock s3c2410wdt probe + start (notes/s10e-reboot-wdt-constants.md).
**/
STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN  Wdt;

  Wdt = (UINTN)EXYNOS_WDT_BASE;
  if (Event != NULL) {
    ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[1] = 0x72;   // W28 progress: ExitBootServices (Windows owns the CPU)
 #if BEYOND0LTE_NT_NO_HWAF
    NtHwAfApply ();
 #endif
  }

  MmioWrite32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_INFORM3_OFFSET, EXYNOS_REBOOT_REASON_BASE);
  MmioWrite32 (Wdt + EXYNOS_WDT_WTCON_OFFSET, 0);
  MmioWrite32 (Wdt + EXYNOS_WDT_WTCLRINT_OFFSET, 1);
 #if !BEYOND0LTE_WINDOWS_WATCHDOG
  if (Event != NULL) {
    __asm__ __volatile__ ("dsb sy" ::: "memory");   // W37: stopped (WTCON = 0) for the life of Windows
    return;
  }
 #endif
  MmioWrite32 (
    (UINTN)EXYNOS_PMU_BASE + (EXYNOS_PMU_WDT_NONCPU_INT_EN_OFFSET | EXYNOS_PMU_ATOMIC_SET_ALIAS),
    EXYNOS_PMU_WDT_NONCPU_INT_EN_BIT
    );
  MmioWrite32 (Wdt + EXYNOS_WDT_WTDAT_OFFSET, EXYNOS_WDT_COUNT_82S);
  MmioWrite32 (Wdt + EXYNOS_WDT_WTCNT_OFFSET, EXYNOS_WDT_COUNT_82S);
  MmioWrite32 (Wdt + EXYNOS_WDT_WTCON_OFFSET, EXYNOS_WDT_WTCON_82S);
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

#endif

EFI_STATUS
EFIAPI
Beyond0LteBootLogEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_CPU_ARCH_PROTOCOL  *Cpu;
  EFI_EVENT              Event;
  EFI_STATUS             Status;

  //
  // W28 boot progress word (0x8FE0F000, written by the shim / SEC / here): [0] = the previous
  // boot's last code - for a boot that hung, where it stopped (0x5x shim, 0x60 SEC, 0x70 DXE,
  // 0x71 ReadyToBoot).
  //
  DEBUG ((
    DEBUG_ERROR,
    "BootLog: progress prev=0x%x cur=0x%x\n",
    ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[0],
    ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[1]
    ));
  ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[1] = 0x70;

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (!EFI_ERROR (Status)) {
    Status = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, BootLogExceptionHandler);
    DEBUG ((DEBUG_ERROR, "BootLog: sync exception handler %r\n", Status));
    Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, BootLogExceptionHandler);
  }

 #if BEYOND0LTE_HANDOFF_WATCHDOG
  gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, OnExitBootServices, NULL, &Event);
 #endif
  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot, NULL, &gEfiEventReadyToBootGuid, &Event);
}
