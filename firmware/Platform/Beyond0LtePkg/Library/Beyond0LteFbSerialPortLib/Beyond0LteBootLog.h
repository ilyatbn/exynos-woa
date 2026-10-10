/** @file
  Beyond0LteBootLog.h - the RAM boot log shared by Beyond0LteFbSerialPortLib (writer, every
  module), PlatformMemoryMapLib (reserves it in the UEFI memory map) and Beyond0LteBootLogDxe
  (copies it to \UEFILOG.TXT on the ESP). notes/m10-windows-deploy-plan.md W19.

  The address is a FIRMWARE CHOICE, not a hardware constant: 1 MiB of bank0 RAM just below the
  FD (0x90000000), inside the verified memory@80000000 range and clear of the monitor
  reservations (0x80000000, 0x80100000, 0x87000000+2 MiB). It is identity-mapped write-back by
  PlatformMemoryMapLib and published as EfiReservedMemoryType so Windows never reuses it.
**/

#ifndef BEYOND0LTE_BOOT_LOG_H_
#define BEYOND0LTE_BOOT_LOG_H_

#define BEYOND0LTE_BOOTLOG_BASE   0x000000008FF00000ULL
#define BEYOND0LTE_BOOTLOG_SIZE   0x0000000000100000ULL
#define BEYOND0LTE_BOOTLOG_MAGIC  0x474F4C42u           // "BLOG"

typedef struct {
  UINT32    Magic;
  UINT32    Head;           // next write offset into Data
  UINT32    Wrapped;        // 1 once Head has wrapped at least once
  UINT32    LastMmuOn;      // 1 if the last writer ran with the MMU on (DXE); SEC + this = new boot
  UINT32    BootCount;      // incremented at every reset of the log
  UINT32    Reserved[3];
} BEYOND0LTE_BOOTLOG_HEADER;

#define BEYOND0LTE_BOOTLOG_DATA_SIZE  ((UINT32)(BEYOND0LTE_BOOTLOG_SIZE - sizeof (BEYOND0LTE_BOOTLOG_HEADER)))

//
// W23: 1 MiB for the Windows UFS driver's PRAM breadcrumb ring (drivers/Exynos9820Ufs
// UFS_PRAM_PHYSICAL_BASE must equal this). Also a firmware choice in bank0 RAM, reserved the same
// way. RAM survives SWRESET and the PMIC key reset (BootCount 1 -> 2 -> 3 over boots #31-#33), so
// the next UEFI boot can copy it to \WINPRAM.BIN on the ESP.
//
#define BEYOND0LTE_WINPRAM_BASE   0x000000008FE00000ULL
#define BEYOND0LTE_WINPRAM_SIZE   0x0000000000100000ULL
#define BEYOND0LTE_WINPRAM_DUMP   0x0000000000010000ULL   // bytes copied to the ESP (ring window + latch)

#endif
