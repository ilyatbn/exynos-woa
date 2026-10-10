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

#include "../../Library/Beyond0LteFbSerialPortLib/Beyond0LteBootLog.h"

#define BEYOND0LTE_AUTO_RECOVERY  0

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

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

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

  Status = FlushBootLog ("exception");
  DEBUG ((DEBUG_ERROR, "BootLog: exception flush -> %r\n", Status));

 #if BEYOND0LTE_AUTO_RECOVERY
  #error "reboot-to-recovery needs the verified PMU reboot-reason/SWRESET sequence (W19)"
 #endif
  CpuDeadLoop ();
}

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

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (!EFI_ERROR (Status)) {
    Status = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, BootLogExceptionHandler);
    DEBUG ((DEBUG_ERROR, "BootLog: sync exception handler %r\n", Status));
    Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, BootLogExceptionHandler);
  }

  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot, NULL, &gEfiEventReadyToBootGuid, &Event);
}
