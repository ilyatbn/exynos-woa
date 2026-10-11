/** @file
  Exynos9820.h — central SoC register-block map for the Galaxy S10e
  (`beyond0lte`, SM-G970F), Exynos 9820.

  The single place the repo keeps MMIO base/size pairs and the framebuffer
  geometry, mirroring firmware/Silicon/Exynos9810Pkg/Include/Platform/
  Exynos9810.h for the Galaxy S9+. Both the UEFI memory map (PlatformMemoryMapLib)
  and the platform drivers include this, so there are no scattered magic numbers.

  NOTHING HERE IS VALIDATED ON HARDWARE. This header is the port's floor: every
  value is a documented read, not an observed one. Under the repo's golden rule a
  stage stays Guided until it has been run end to end on an S10e.

  VALUES SOURCED FROM (see notes/dock-desktop.md §5 and §13 in this repo):
    the shipping 9820 device tree — arch/arm64/boot/dts/exynos/exynos9820.dts
      from CruelKernel/samsung-exynos9820, branch cruel-HVJ5-v5.3, which lists
      G970F among its supported models. Numeric phandles show it is the
      decompiled DT of a shipping build, i.e. what the phone actually runs.
    the live S10e itself — /proc/device-tree/ and /sys/devices/platform/ node
      NAMES. Property *contents* are SELinux-blocked to an unprivileged shell,
      but node names carry the unit address, which is what makes this readable.
    the device's own /vendor/etc/fstab.exynos9820, which names
      /dev/block/platform/13d60000.ufs — an independent confirmation of the
      UFS HCI base.

  Convention: a base of 0 means "unknown / not configured". The memory map lib
  skips zero-based device windows so a placeholder never maps PA 0 by accident.

  Lines marked UNVERIFIED are NOT reads. They are either the 9810 value carried
  over so the tree still compiles, or a value whose address is known but whose
  extent or meaning is not. Do not treat them as facts.
**/

#ifndef EXYNOS9820_H_
#define EXYNOS9820_H_

//
// ---------------------------------------------------------------------------
// DRAM  [bank0 VERIFIED from the vendor DT; the rest are node names only]
//
// The live S10e publishes five memory nodes:
//   memory@80000000  memory@C0000000  memory@E1900000
//   memory@880000000 memory@900000000
// The vendor DT gives bank0 exactly:
//   memory@80000000  reg = <0x0 0x80000000 0x80000000>   (2 GiB)
// The LIVE DTB does not. Read in a TWRP root shell on this phone (2026-10-10,
// notes/K10-rkp.md T18):
//   memory@80000000  reg = <0x0 0x80000000 0x3AB00000>   (937.5 MiB, ends 0xBAB00000)
//   memory@C0000000  reg = <0x0 0xC0000000 0x20000000>
//   memory@E1900000  reg = <0x0 0xE1900000 0x1E700000>
//   memory@880000000 reg = <0x8 0x80000000 0x80000000>
// So bank0 is NOT 2 GiB on this phone, and it ends at 0xBAB00000 — 0x1D00000
// BELOW the 9810's 0xBC800000. Anything that assumed "DRAM ends at 0xBC800000"
// is still wrong, but in the other direction than first believed: the UFS
// driver's carried DMA window 0x90000000..0xBC800000 (still marked UNVERIFIED)
// overruns the live bank0 end and is NOT a legal 9820 window. See EXYNOS_UFS_*
// below and the DMA window in drivers/Exynos9820Ufs/.
//
// For UEFI bring-up we still map ONLY bank0: it is the one bank whose extent
// is a read, and it is large enough to host the firmware. The other four must
// be published once their sizes are read from the DT (contents are blocked on
// the phone; the vendor DT has them).
//
// UPDATE (2026-10-10): in a TWRP ROOT shell the contents are NOT blocked - every
// size below is now a live read, see notes/K10-rkp.md T18.
// ---------------------------------------------------------------------------
//
// The SoC-level DT declares three 2 GiB banks; the live phone splits the low one
// at 0xC0000000 and 0xE1900000, the same split points the 9810's map used.
//   memory@80000000  reg = <0x0 0x80000000 0x80000000>
//   memory@880000000 reg = <0x8 0x80000000 0x80000000>
//   memory@900000000 reg = <0x9 0x00000000 0x80000000>
//
// Bank0 is NOT 2 GiB of usable RAM. The board file this phone includes
// (exynos9820-rmem_s5100_dual.dtsi) reserves a contiguous 729 MB in the middle
// of it, from 0xD0000000 to 0xFD900000, for the modem/CP stack:
//
//   modem_spare     0xD0000000 + 0x1600000    modem_if      0xF0000000 + 0xB900000
//   modem_if2       0xD1600000 + 0x4200000    cp_ram_logging 0xFB900000 + 0x2000000
//   cp2cp_shm       0xD5800000 + 0x4000000    s5100_msi      0xD9800000 + 0x1000
//
// Handing that to Windows as conventional RAM is a fault waiting to happen, so
// the map publishes only up to the first reserved region until the split sizes
// are confirmed. The DT's chosen node also puts the Android initrd at
// 0x84000000..0x841fffff, which is why the firmware loads at 0x80080000.
//
// CORRECTION (2026-10-10, notes/K10-rkp.md T16-T18, read in a TWRP root shell on
// this phone): 0x84000000..0x841FFFFF is the KERNEL DEFAULT from the SoC DTS, not
// what sboot does. The live chosen node of the currently booted (TWRP) kernel
// says linux,initrd-start = 0xD5000000, linux,initrd-end = 0xD5A67000. The live
// reserved-memory node also carries the sboot-added children the static DTS has
// none of — el2_code 0x87000000+0x200000 (owner el2,uh), el2_earlymem
// 0x97E000000+0x200000, kaslr 0x80100000+0x100000 (owner kernel-kaslr), sec_debug_magic
// 0x80000000+0x1000, camera 0x99000000+0xE400000, secure_camera 0x96000000+0x2200000,
// mm_debug 0x91200000+0x100000, modem_if 0xF0000000+0xB900000, modem_spare
// 0xD0000000+0x1600000, seclog_mem 0xC0000000+0x80000, cp_ram_logging 0xFB900000+0x2000000.
// So 0x80080000 sits inside NO reserved node, but the justification for it is now
// known to rest on a kernel default; it stays UNVERIFIED as an observed address.
//
// UPDATE (2026-10-10, notes/k4b-drop-address.md K01-K03): the drop address IS a
// read now. sboot loaded this boot's Image at 0x80118000 (Kernel code 0x80198000
// in /proc/iomem, minus the Image header's text_offset 0x80000), and the kernel
// reaches 0x832D0000 (= 0x80198000 + image_size 0x3138000, exactly the Kernel
// data end 0x832CFFFF + 1). The FD range 0x80080000..0x80780000 therefore
// OVERLAPS the loaded-image window and two sboot-owned nodes (sec_debug_magic
// 0x80000000+0x1000 and kaslr 0x80100000+0x100000), even though the single
// address 0x80080000 is inside no reserved node.
// ---------------------------------------------------------------------------
//
#define EXYNOS_DRAM_BASE          0x0000000080000000ULL   // VERIFIED
#define EXYNOS_DRAM_BANK0_SIZE    0x000000003AB00000ULL   // VERIFIED (live DTB reg, TWRP root read; the vendor DT says 0x80000000)
#define EXYNOS_DRAM_SIZE          0x0000000050000000ULL   // CONSERVATIVE: stops at the first reserved region

#define EXYNOS_DRAM_BANK1_BASE    0x00000000C0000000ULL   // VERIFIED (node name)
#define EXYNOS_DRAM_BANK1_SIZE    0x0000000020000000ULL   // VERIFIED (live DTB reg; the 0x21900000 candidate was wrong)
#define EXYNOS_DRAM_BANK2_BASE    0x00000000E1900000ULL   // VERIFIED (node name)
#define EXYNOS_DRAM_BANK2_SIZE    0x000000001E700000ULL   // VERIFIED (live DTB reg; the 0x1E700000 candidate was right)
#define EXYNOS_DRAM_BANK3_BASE    0x0000000880000000ULL   // VERIFIED (node name + vendor DT reg)
#define EXYNOS_DRAM_BANK3_SIZE    0x0000000080000000ULL   // VERIFIED (vendor DT reg)
#define EXYNOS_DRAM_BANK4_BASE    0x0000000900000000ULL   // VERIFIED (node name + vendor DT reg)
#define EXYNOS_DRAM_BANK4_SIZE    0x0000000080000000ULL   // VERIFIED (vendor DT reg)

//
// ---------------------------------------------------------------------------
// RAM reservations that are OWNED BY THE MONITOR, not scratch  [ALL VERIFIED]
//
// All four are children of the live reserved-memory node, read in a TWRP root
// shell (notes/K10-rkp.md T16/T17, notes/s10e-dtb-mmio.md S01). Their `compatible`
// strings name the owner: sec_debug_magic and kaslr are written by sboot
// ("kernel-kaslr"), el2_code / el2_earlymem are the uH hypervisor's own code and
// heap ("el2,uh"), and uH maps them read-only/unmapped in stage-2. Handing them
// to Windows as conventional RAM lets the OS overwrite live EL2 code.
//
// Deliberately NOT in this list: the ION heaps (camera 0x99000000+0xE400000,
// secure_camera 0x96000000+0x2200000, ion_video_stream, ion_crypto) and the
// modem/CP blocks (modem_spare, modem_if, cp_ram_logging). They are Android
// driver scratch, not monitor-owned state, and no read says what would break if
// Windows used them - that stays an open question, not a decision dressed as a
// fact.
// ---------------------------------------------------------------------------
//
#define EXYNOS_RS_MAGIC_BASE      0x0000000080000000ULL   // VERIFIED (sec_debug_magic)
#define EXYNOS_RS_MAGIC_SIZE      0x0000000000001000ULL   // VERIFIED
#define EXYNOS_RS_KASLR_BASE      0x0000000080100000ULL   // VERIFIED (kaslr, owner kernel-kaslr)
#define EXYNOS_RS_KASLR_SIZE      0x0000000000100000ULL   // VERIFIED
#define EXYNOS_RS_EL2_CODE_BASE   0x0000000087000000ULL   // VERIFIED (el2_code, owner el2,uh)
#define EXYNOS_RS_EL2_CODE_SIZE   0x0000000000200000ULL   // VERIFIED
#define EXYNOS_RS_EL2_EARLY_BASE  0x000000097E000000ULL   // VERIFIED (el2_earlymem, owner el2,uh)
#define EXYNOS_RS_EL2_EARLY_SIZE  0x0000000002000000ULL   // VERIFIED 2026-10-11 live reserved-memory/el2_earlymem reg (was 0x200000 here - 16x short)
//
// m13 (notes/m13-ram.md), live /proc/device-tree/reserved-memory/*/reg read as root in TWRP 2026-10-11:
//
#define EXYNOS_RS_RDX_BOOTDEV_BASE    0x0000000880000000ULL   // VERIFIED (sec_rdx_bootdev)
#define EXYNOS_RS_RDX_BOOTDEV_SIZE    0x000000000AD00000ULL   // VERIFIED
#define EXYNOS_RS_SECURE_CAMERA_BASE  0x0000000096000000ULL   // VERIFIED (secure_camera)
#define EXYNOS_RS_SECURE_CAMERA_SIZE  0x0000000002200000ULL   // VERIFIED

//
// Reserved inside bank0 — published so the memory map can subtract them rather
// than discover the collision at runtime. VERIFIED from the board file.
//
#define EXYNOS_RSVD_MODEM_SPARE_BASE  0x00000000D0000000ULL
#define EXYNOS_RSVD_MODEM_SPARE_SIZE  0x0000000001600000ULL
#define EXYNOS_RSVD_MODEM_IF2_BASE    0x00000000D1600000ULL
#define EXYNOS_RSVD_MODEM_IF2_SIZE    0x0000000004200000ULL
#define EXYNOS_RSVD_CP2CP_BASE        0x00000000D5800000ULL
#define EXYNOS_RSVD_CP2CP_SIZE        0x0000000004000000ULL
#define EXYNOS_RSVD_S5100_MSI_BASE    0x00000000D9800000ULL
#define EXYNOS_RSVD_S5100_MSI_SIZE    0x000000000001000ULL
#define EXYNOS_RSVD_MODEM_IF_BASE     0x00000000F0000000ULL
#define EXYNOS_RSVD_MODEM_IF_SIZE     0x000000000B900000ULL
#define EXYNOS_RSVD_CP_RAM_BASE       0x00000000FB900000ULL
#define EXYNOS_RSVD_CP_RAM_SIZE       0x0000000002000000ULL

//
// Read on-device as root in TWRP (2026-10-09) from /proc/device-tree/reserved-memory/*/reg
// — the values the browser model had only claimed (notes/vendor-dt-reserved.md) and the
// ones the unprivileged shell could not read. debug_snapshot's children occupy 0xFD900000
// .. 0x100000000 contiguously (header, log_kernel, log_platform, log_sfr, log_cachedump,
// log_etm, log_bcm, log_llc, log_dbgc, log_kevents), which is why the 9810's 0xFED14000
// breadcrumb is inside a vendor log region and unusable. Full table: notes/s10e-root-reads.md.
//
#define EXYNOS_RSVD_E2_CODE_BASE      0x0000000087000000ULL   // VERIFIED (live DT)
#define EXYNOS_RSVD_E2_CODE_SIZE      0x0000000000002000ULL   // VERIFIED
#define EXYNOS_RSVD_KASLR_BASE        0x0000000080001000ULL   // VERIFIED
#define EXYNOS_RSVD_KASLR_SIZE        0x0000000000001000ULL   // VERIFIED
#define EXYNOS_RSVD_SEC_MAGIC_BASE    0x0000000080000000ULL   // VERIFIED
#define EXYNOS_RSVD_SEC_MAGIC_SIZE    0x0000000000001000ULL   // VERIFIED
#define EXYNOS_RSVD_MM_DEBUG_BASE     0x0000000091200000ULL   // VERIFIED
#define EXYNOS_RSVD_MM_DEBUG_SIZE     0x0000000000001000ULL   // VERIFIED
#define EXYNOS_RSVD_SECLOG_BASE       0x00000000C0000000ULL   // VERIFIED
#define EXYNOS_RSVD_SECLOG_SIZE       0x0000000000080000ULL   // VERIFIED
#define EXYNOS_RSVD_CAMERA_BASE       0x0000000099000000ULL   // VERIFIED
#define EXYNOS_RSVD_CAMERA_SIZE       0x000000000E400000ULL   // VERIFIED
#define EXYNOS_RSVD_CAMERA_CTG_BASE   0x0000000098200000ULL   // VERIFIED
#define EXYNOS_RSVD_CAMERA_CTG_SIZE   0x0000000000E00000ULL   // VERIFIED
#define EXYNOS_RSVD_SECURE_CAM_BASE   0x0000000096000000ULL   // VERIFIED
#define EXYNOS_RSVD_SECURE_CAM_SIZE   0x0000000002200000ULL   // VERIFIED
#define EXYNOS_RSVD_E2_EARLY_BASE     0x000000097E000000ULL   // VERIFIED
#define EXYNOS_RSVD_E2_EARLY_SIZE     0x0000000002000000ULL   // VERIFIED
#define EXYNOS_RSVD_SEC_RDX_BASE      0x0000000880000000ULL   // VERIFIED
#define EXYNOS_RSVD_SEC_RDX_SIZE      0x000000000AD00000ULL   // VERIFIED

//
// ---------------------------------------------------------------------------
// Debug UART (Samsung/Exynos UART — NOT 16550/PL011 compatible)
//
// VERIFIED on-device: uart@10440000 is the first PERIC0 UART, same base as on
// the S9+. The vendor DT declares the window as 0x100 (not the 9810's 0x1000)
// and fifo-size = 0x100, so the smaller size is used here rather than the
// 9810's. Many more UARTs exist (0x10450000 … 0x15D60000); 0x10440000 is the
// debug-console candidate, as on the 9810.
// ---------------------------------------------------------------------------
//
#define EXYNOS_UART_BASE          0x0000000010440000ULL   // VERIFIED (uart@10440000)
#define EXYNOS_UART_SIZE          0x0000000000000100ULL   // VERIFIED (vendor DT reg)

//
// ---------------------------------------------------------------------------
// GICv2 interrupt controller  [VERIFIED — IDENTICAL to the 9810]
//
// The 9820's node is interrupt-controller@10100000, compatible
// "arm,cortex-a15-gic", "arm,cortex-a9-gic" (GICv2, same architecture as the
// 9810's arm,gic-400). Its reg list, in DT order GICD, GICC, GICH, GICV:
//   <0x0 0x10101000 0x1000  0x0 0x10102000 0x1000
//    0x0 0x10104000 0x2000  0x0 0x10106000 0x2000>
// So the GIC does NOT move between these two SoCs — the 9810's MADT work
// transfers unchanged, which is the single largest simplification in this port.
//
// Do not confuse this with abox-gic@18cf0000 (reg-names "gicd","gicc"), which
// is Samsung's ABOX GIC interface, a different thing entirely.
// ---------------------------------------------------------------------------
//
#define EXYNOS_GICD_BASE          0x0000000010101000ULL   // VERIFIED (distributor)
#define EXYNOS_GICD_SIZE          0x0000000000001000ULL   // VERIFIED
#define EXYNOS_GICC_BASE          0x0000000010102000ULL   // VERIFIED (CPU interface)
#define EXYNOS_GICC_SIZE          0x0000000000001000ULL   // VERIFIED (vendor DT)
#define EXYNOS_GICH_BASE          0x0000000010104000ULL   // VERIFIED (hypervisor)
#define EXYNOS_GICH_SIZE          0x0000000000002000ULL   // VERIFIED
#define EXYNOS_GICV_BASE          0x0000000010106000ULL   // VERIFIED (virtual CPU)
#define EXYNOS_GICV_SIZE          0x0000000000002000ULL   // VERIFIED

//
// The exynos-pm node also publishes a small latch page inside the GICD window:
//   reg-names = "gpio_alive_base", "gicd_ispendrn_base"
//   reg       =  <0x0 0x15850000 0x1000  0x0 0x10101200 0x100>
// It is inside EXYNOS_GICD_BASE's page, so it needs no separate mapping, but
// anything that carves the GICD window must not treat +0x200..+0x300 as free.
//
#define EXYNOS_GICD_ISPENDRN_BASE 0x0000000010101200ULL   // VERIFIED (exynos-pm reg)
#define EXYNOS_GICD_ISPENDRN_SIZE 0x0000000000000100ULL   // VERIFIED

//
// ---------------------------------------------------------------------------
// Architected generic timer  [UNVERIFIED on this SoC]
//
// On the S9+ the stock bootloader does NOT program CNTFRQ_EL0 and the real
// counter frequency is 26 MHz, so the 9810 header hardcodes it. Whether the
// S10e's sboot behaves the same way has not been established: the value below
// is the 9810's, kept so the tree compiles, and the first on-device read of
// CNTFRQ_EL0 (or of the kernel's own ticks) decides it.
// ---------------------------------------------------------------------------
//
#define EXYNOS_ARCH_TIMER_FREQ_HZ  26000000u   // VERIFIED on-device: CNTFRQ_EL0 = 0x018CBA80 = 26000000 read by the boot shim (notes/m8-uefi-image.md B47, T05)

//
// ---------------------------------------------------------------------------
// Power management unit (PMU) + chip ID  [PMU MOVES, chip ID does not]
//
// VERIFIED: the 9820's PMU is system-controller@15860000, compatible
// "samsung,exynos9820-pmu","syscon" — the 9810's 0x14060000 is NOT present here.
// The size below is the 9810's; the vendor DT's window for this node was not
// captured, so treat the extent as unverified even though the base is a read.
//
// The PMU is what the reset-to-recovery path and the watchdog mask live in, so
// a wrong base is not cosmetic: it is the difference between landing in TWRP
// and resetting into nothing.
// ---------------------------------------------------------------------------
//
#define EXYNOS_PMU_BASE           0x0000000015860000ULL   // VERIFIED (system-controller@15860000)
#define EXYNOS_PMU_SIZE           0x0000000000010000ULL   // VERIFIED (live DTB: system-controller reg = 0x15860000 + 0x10000, notes/s10e-dtb-mmio.md S02)
//
// Reboot reason + software reset - VERIFIED from Samsung's own binaries (notes/s10e-reboot-wdt-constants.md):
// kernel sec_reboot writes INFORM3 = 0x12345670 | mode (recovery = 4) then exynos_reboot writes SWRESET
// PMU+0x3A00 = 0x2; sboot s5p_check_reboot_mode decodes INFORM3 (PINFORM, 0x1586080C) the same way and
// itself resets with 2 -> 0x15863A00. The 9810 values (SWRESET 0x400 = 1) are WRONG on this SoC.
//
#define EXYNOS_PMU_INFORM2_OFFSET      0x0808u        // VERIFIED (sboot register dump name, kernel sec_reboot)
#define EXYNOS_PMU_INFORM3_OFFSET      0x080Cu        // VERIFIED (sboot PINFORM, kernel sec_reboot 0x8e14978)
#define EXYNOS_PMU_RSTSTAT_OFFSET      0x0404u        // VERIFIED (sboot RSTSTAT, kernel exynos_reboot 0x86391e4)
#define EXYNOS_PMU_SWRESET_OFFSET      0x3A00u        // VERIFIED (kernel exynos_reboot 0x8639204, sboot 0xa5200)
#define EXYNOS_PMU_SWRESET_VALUE       0x00000002u    // VERIFIED (same two sites)
#define EXYNOS_PMU_PS_HOLD_CONTROL_OFFSET  0x030Cu     // VERIFIED (kernel sec_power_off 0x8e144b0 clears bit 8, "set PS_HOLD low")

//
// Watchdog cl0 (EXYNOS_WDT_BASE, samsung,exynos9-v1-wdt) - VERIFIED from the stock kernel's
// s3c2410_wdt (notes/s10e-reboot-wdt-constants.md): registers, the v1 variant's only PMU setup
// (NONCPU_INT_EN 0x1244 bit 2, set through the PMU's atomic-set alias +0xC000 = write the bit
// index), and WTCON/count for 82 s by the driver's formula at fin_pll 26 MHz (DT/binary reading,
// not measured on the device).
//
#define EXYNOS_WDT_WTCON_OFFSET            0x00u
#define EXYNOS_WDT_WTDAT_OFFSET            0x04u
#define EXYNOS_WDT_WTCNT_OFFSET            0x08u
#define EXYNOS_WDT_WTCLRINT_OFFSET         0x0Cu
#define EXYNOS_PMU_WDT_NONCPU_INT_EN_OFFSET  0x1244u
#define EXYNOS_PMU_WDT_NONCPU_INT_EN_BIT     2u
#define EXYNOS_PMU_ATOMIC_SET_ALIAS        0xC000u   // exynos_pmu_update: write bit index to (off | 0xC000)
#define EXYNOS_WDT_WTCON_82S               0x0000FF39u   // prescaler 0xFF (max), DIV128, RSTEN, EN: 26 MHz/128/256 = 793.46 Hz
#define EXYNOS_WDT_COUNT_82S               0x0000FE27u   // 82 s x 793.46 Hz = 65063 (max ~82.6 s; the user asked for 90)
#define EXYNOS_REBOOT_REASON_BASE      0x12345670u    // VERIFIED (kernel sec_reboot, sboot s5p_check_reboot_mode)
#define EXYNOS_REBOOT_MODE_RECOVERY    4u             // VERIFIED (sec_reboot "recovery" -> +4; sboot recovery REBOOT_MODE 4)
#define EXYNOS_CHIPID_BASE        0x0000000010000000ULL   // VERIFIED (chipid@10000000)
#define EXYNOS_CHIPID_SIZE        0x0000000000000100ULL   // VERIFIED

//
// ---------------------------------------------------------------------------
// SPEEDY  [MOVES, and there are two]
//
// VERIFIED node names: speedy@15970000 and speedy1@15980000. The 9810 had one
// at 0x141C0000. Sizes read 2026-10-10 from the live DTB in a TWRP root shell:
// both are 0x2000, so the two entries the map used to skip are now published.
// ---------------------------------------------------------------------------
//
#define EXYNOS_SPEEDY_BASE        0x0000000015970000ULL   // VERIFIED (speedy@15970000)
#define EXYNOS_SPEEDY_SIZE        0x0000000000002000ULL   // VERIFIED (live DTB reg, TWRP root read - notes/s10e-dtb-mmio.md S02)
#define EXYNOS_SPEEDY1_BASE       0x0000000015980000ULL   // VERIFIED (speedy1@15980000)
#define EXYNOS_SPEEDY1_SIZE       0x0000000000002000ULL   // VERIFIED (live DTB reg, TWRP root read - notes/s10e-dtb-mmio.md S02)

//
// ---------------------------------------------------------------------------
// ACPM IPC (APM core that owns the S2MPS19 PMIC on the 9820)  [notes/m12-acpm.md]
//
// VERIFIED 2026-10-11 from the live device tree (TWRP root read, archive
// s10e-artifacts\dt-acpm-20261011.tar, notes/m12-touch.md): node acpm_ipc,
// compatible "samsung,exynos-acpm-ipc",
//   reg = <0x15900000 0x1000>   mailbox registers (kernel acpm_ipc.c:791 resource 0, "intr")
//         <0x02039000 0x30000>  APM SRAM          (kernel acpm_ipc.c:796 resource 1, "sram_base")
//   initdata-base = <0x7000>    struct acpm_framework offset in that SRAM (acpm_ipc.c:801-808)
// TWRP /proc/iomem independently lists "02039000-02068fff : /acpm_ipc".
// ---------------------------------------------------------------------------
//
#define EXYNOS_ACPM_IPC_MBOX_BASE        0x0000000015900000ULL   // VERIFIED (live DT acpm_ipc reg[0])
#define EXYNOS_ACPM_IPC_MBOX_SIZE        0x0000000000001000ULL   // VERIFIED (live DT acpm_ipc reg[0])
#define EXYNOS_ACPM_SRAM_BASE            0x0000000002039000ULL   // VERIFIED (live DT acpm_ipc reg[1], /proc/iomem)
#define EXYNOS_ACPM_SRAM_SIZE            0x0000000000030000ULL   // VERIFIED (live DT acpm_ipc reg[1], /proc/iomem)
#define EXYNOS_ACPM_INITDATA_OFFSET      0x00007000u             // VERIFIED (live DT acpm_ipc initdata-base)

//
// ---------------------------------------------------------------------------
// Clock management (CMU)  [TOP same base, the banks are a different set]
//
// VERIFIED from the vendor DT:
//   cmu      reg = <0x0 0x1a200000 0x8000  0x0 0x1bc00000 0x8000
//                    0x0 0x1bd00000 0x8000  0x0 0x1be00000 0x8000
//                    0x0 0x1bf00000 0x8000>
//   cmu_ewf  reg = <0x0 0x1a240000 0x1000>
//
// So 0x1A240000 is still inside the 0x1A200000 bank, and the 9810's CMU_FSYS0
// at 0x11000000 does not exist on this SoC at all. RESOLVED 2026-10-09: the
// DT's five-window `cmu` node is NOT the whole CMU set. Samsung's own
// calibration table (drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c, the
// `cmucal_sfr_block_list[]` of 34 entries) gives every CMU block:
//
//   CMU_PERIC0 0x10400000 +0x8000     CMU_FSYS0A 0x10a00000 +0x8000
//   CMU_FSYS0  0x13000000 +0x8000     CMU_PERIC1 0x10800000 +0x8000
//   CMU_BUSC   0x1a200000 +0x8000     CMU_CMU    0x1a240000 +0x8000
//   CMU_MIF/MIF1/MIF2/MIF3 = 0x1bc00000/0x1bd00000/0x1be00000/0x1bf00000
//
// i.e. the four 0x1BC..0x1BF banks the DT lists are the MEDIA IP CMUs, and
// PERIC0/FSYS0A live at their own addresses - PERIC0 at the SAME base the
// 9810 used, with a full block of 0x8000 (the 9810 mapped only 0x4000, which
// is cal-if's `dbg_offset`).
//
// Containment note: GPIO_PERIC0 (0x10430000) sits inside CMU_PERIC0's 0x8000
// block. Both are mapped DEVICE/UNCACHEABLE, so the overlap is attribute-
// consistent; it is not the UFS-style overlap the lib folds.
// ---------------------------------------------------------------------------
//
#define EXYNOS_CMU_TOP_BASE       0x000000001A240000ULL   // VERIFIED (cmu_ewf)
#define EXYNOS_CMU_TOP_SIZE       0x0000000000001000ULL   // VERIFIED (vendor DT)
#define EXYNOS_CMU_BANK0_BASE     0x000000001A200000ULL   // VERIFIED (cmu reg #1)
#define EXYNOS_CMU_BANK0_SIZE     0x0000000000008000ULL   // VERIFIED
#define EXYNOS_CMU_BANK1_BASE     0x000000001BC00000ULL   // VERIFIED (cmu reg #2)
#define EXYNOS_CMU_BANK1_SIZE     0x0000000000008000ULL   // VERIFIED
#define EXYNOS_CMU_BANK2_BASE     0x000000001BD00000ULL   // VERIFIED (cmu reg #3)
#define EXYNOS_CMU_BANK2_SIZE     0x0000000000008000ULL   // VERIFIED
#define EXYNOS_CMU_BANK3_BASE     0x000000001BE00000ULL   // VERIFIED (cmu reg #4)
#define EXYNOS_CMU_BANK3_SIZE     0x0000000000008000ULL   // VERIFIED
#define EXYNOS_CMU_BANK4_BASE     0x000000001BF00000ULL   // VERIFIED (cmu reg #5)
#define EXYNOS_CMU_BANK4_SIZE     0x0000000000008000ULL   // VERIFIED

#define EXYNOS_CMU_FSYS0A_BASE    0x0000000010A00000ULL   // cal-if SFR_BLOCK(CMU_FSYS0A)
#define EXYNOS_CMU_FSYS0A_SIZE    0x0000000000008000ULL   // cal-if SFR_BLOCK size
#define EXYNOS_CMU_PERIC0_BASE    0x0000000010400000ULL   // cal-if SFR_BLOCK(CMU_PERIC0)
#define EXYNOS_CMU_PERIC0_SIZE    0x0000000000008000ULL   // cal-if SFR_BLOCK size

//
// ---------------------------------------------------------------------------
// UFS storage (JEDEC UFSHCI host + Samsung UFS PHY)  [ALL VERIFIED — MOVES]
//
// VERIFIED twice: the vendor DT's ufs@13D60000 reg list, and the device's own
// fstab, which names /dev/block/platform/13d60000.ufs/by-name/keydata. The
// live phone shows the platform node 13d60000.ufs with a host0 child.
//
// The 9810's whole 0x111… region is gone. The intra-window vendor-specific
// offset is the SAME (HCI + 0x1100 = 0x13D61100), which is why
// drivers/Exynos9820Ufs/ keeps UFS_VENDOR_OFFSET unchanged.
//
// The UNIPRO window must keep its FULL 0x8000: the same debug-APB flow-control
// writes that cost a device cycle on the 9810 land at +0x7888, i.e. page 7.
// ---------------------------------------------------------------------------
//
#define EXYNOS_UFS_HCI_BASE       0x0000000013D60000ULL   // VERIFIED (ufs@13D60000)
#define EXYNOS_UFS_HCI_SIZE       0x0000000000002000ULL   // VERIFIED
#define EXYNOS_UFS_VS_BASE        0x0000000013D61100ULL   // VERIFIED (vendor-specific HCI)
#define EXYNOS_UFS_UNIPRO_BASE    0x0000000013D50000ULL   // VERIFIED (0x8000 — map it all)
#define EXYNOS_UFS_UNIPRO_SIZE    0x0000000000008000ULL   // VERIFIED
#define EXYNOS_UFS_PMA_BASE       0x0000000013D64000ULL   // VERIFIED (ufs-phy, PMA/analog)
#define EXYNOS_UFS_PMA_SIZE       0x0000000000000800ULL   // VERIFIED (vendor DT reg)
#define EXYNOS_UFS_UFSP_BASE      0x0000000013D70000ULL   // VERIFIED (FMP/UFSP protector)
#define EXYNOS_UFS_UFSP_SIZE      0x0000000000001000ULL   // VERIFIED
#define EXYNOS_UFS_CPORT_BASE     0x0000000013D68000ULL   // VERIFIED (cport)
#define EXYNOS_UFS_IOCOH_BASE     0x0000000013C10700ULL   // VERIFIED (io-coherency)

//
// ---------------------------------------------------------------------------
// USB 3.0 DRD (DWC3 — exposes a standard xHCI register interface) + USB PHY
//
// VERIFIED on-device and in the vendor DT:
//   usb@10C00000   compatible "samsung,exynos-dwusb", reg <0x0 0x10c00000 0x10000>
//                  child dwc3, compatible "synopsys,dwc3", same reg
//   phy@10B00000   compatible "samsung,exynos-usbdrd-phy"
//                  reg = <0x0 0x10b00000 0x200   0x0 0x10ae0000 0x2800
//                         0x0 0x10af0000 0x800>
//
// Three facts that change the 9810's model:
//   1. The DWC3 core window is at the SAME address and size as on the 9810, so
//      the xHCI MMIO Windows' inbox USBXHCI.sys binds to does not move.
//   2. The PHY is ONE node with THREE register windows, not three separate
//      nodes: main 0x10B00000+0x200, combo 0x10AE0000+0x2800, PCS 0x10AF0000+0x800.
//      The node declares has_combo_phy = 1 and has_other_phy = 0.
//   3. Both usb@10C00000 and phy@10B00000 carry status = "disabled" in the
//      shipping build, and the board-level patch sets dr_mode = "otg" on the
//      dwc3 child. The device tree itself therefore proves the port is on the
//      device side under Android — a dock needs "host".
//
// phy_version = 0x301 and sub_phy_version = 0x400: the PHY generation differs
// from the 9810's, so the 9810's PHY init sequence cannot be pasted here.
// ---------------------------------------------------------------------------
//
#define EXYNOS_USBDRD_BASE        0x0000000010C00000ULL   // VERIFIED (usb@10C00000)
#define EXYNOS_USBDRD_SIZE        0x0000000000010000ULL   // VERIFIED
#define EXYNOS_USBDRD_PHY_BASE    0x0000000010B00000ULL   // VERIFIED (phy@10B00000 reg #1)
#define EXYNOS_USBDRD_PHY_SIZE    0x0000000000000200ULL   // VERIFIED
#define EXYNOS_USB_COMBO_PHY_BASE 0x0000000010AE0000ULL   // VERIFIED (phy reg #2)
#define EXYNOS_USB_COMBO_PHY_SIZE 0x0000000000002800ULL   // VERIFIED
#define EXYNOS_USB_PCS_BASE       0x0000000010AF0000ULL   // VERIFIED (phy reg #3)
#define EXYNOS_USB_PCS_SIZE       0x0000000000000800ULL   // VERIFIED
#define EXYNOS_USBDRD_GSIV        273                     // VERIFIED (SPI 0xf1 = 241 + 32, vendor DT dwc3 child)

//
// ---------------------------------------------------------------------------
// HSI2C #45 — the bus carrying the MAX77705 interface PMIC (charger client
// 0x69, CCIC client 0x25), which is the ONLY source of the 5 V VBUS the USB
// port supplies in host role (the SoC has no VBUS pad: is_not_vbus_pad=1).
//
// VERIFIED twice: the downstream DT node `hsi2c_45: hsi2c@15D60000`
// (exynos9820.dts:3418, LineageOS android_kernel_samsung_exynos9820) and the
// live device, where `15d60000` appears under /sys/devices/platform/
// (notes/s10e-live-reads.md §2) — so the block is registered, clocked and
// pin-muxed by the bootloader already. Its clock gates are ACPM-owned
// vclk/QCH entries, NOT CMU MMIO (notes/d1b-usb-vbus-hcd.md Q1) — same
// "audit and leave alone" strategy as the USB block.
// ---------------------------------------------------------------------------
//
#define EXYNOS_PMIC_HSI2C_BASE    0x0000000015D60000ULL   // VERIFIED (hsi2c_45, DT + live device node)
#define EXYNOS_PMIC_HSI2C_SIZE    0x0000000000001000ULL   // VERIFIED (DT reg size)

// USI_CMGP3 mode register (hsi2c_45 is the USI_CMGP3 USI block). VERIFIED from the live DT (TWRP,
// 2026-10-11): __symbols__/usi_03_cmgp -> /usi@15C22030, compatible "samsung,exynos-usi-v2",
// usi_v2_mode "i2c", reg <0x15C22030 0x4>. M16 run 4: at UEFI handoff the USI was held in reset
// (USI_CON 1) and every I2C register read 0 - the bootloader never configured this bus.
// I2C mode value 0x4: same exynos-usi-v2 encoding our Windows HSI2C driver writes to usi@1042101C
// (drivers/Exynos9820Hsi2c/exynos9820hsi2c.h EXYNOS_SYSREG_USI_MODE_I2C), validated on the touch bus.
#define EXYNOS_CMGP_SYSREG_USI_PAGE      0x0000000015C22000ULL   // VERIFIED (page holding usi@15C22000..15C22034)
#define EXYNOS_CMGP_SYSREG_USI_PAGE_SIZE 0x0000000000001000ULL
#define EXYNOS_CMGP_USI03_MODE           0x0000000015C22030ULL   // VERIFIED (usi_03_cmgp)
#define EXYNOS_USI_V2_MODE_I2C           0x00000004u             // VERIFIED on usi@1042101C (touch bus)

// CMGP pin controller (hsi2c_45 pins gpm12-0 = SCL, gpm13-0 = SDA). VERIFIED:
//   live DT alias pinctrl2 -> /pinctrl@15C30000, reg <0x15C30000 0x1000>, 32 gpm banks;
//   hsi2c45-bus: samsung,pins "gpm12-0" "gpm13-0", samsung,pin-function <2> (I2C);
//   Samsung 9820 kernel (LineageOS lineage-18.1) drivers/pinctrl/samsung/pinctrl-exynos-arm64.c
//   exynos9820_pin_banks2 ("CMGP data"): gpm12 at 0x180, gpm13 at 0x1A0, exynos9810_bank_type_alive
//   (CON +0x0 4-bit field per pin, DAT +0x4, PUD +0x8, DRV +0xC). NOTE the 9810 table has them at
//   0x140/0x160 - not interchangeable.
#define EXYNOS_CMGP_GPIO_BASE            0x0000000015C30000ULL   // VERIFIED
#define EXYNOS_CMGP_GPIO_SIZE            0x0000000000001000ULL   // VERIFIED
#define EXYNOS_CMGP_GPM12_CON_OFFSET     0x180u                  // VERIFIED (kernel bank table)
#define EXYNOS_CMGP_GPM13_CON_OFFSET     0x1A0u                  // VERIFIED (kernel bank table)
#define EXYNOS_GPIO_FUNC_HSI2C45         0x2u                    // VERIFIED (DT samsung,pin-function)

//
// ---------------------------------------------------------------------------
// Touch: the HSI2C bus the panel IC hangs off, and the panel itself.
//
// VERIFIED on the live phone: touchscreen@48 is a CHILD of hsi2c@104B0000 —
// the same bus window and the same slave address as on the S9+. Enumerating
// every hsi2c@… node on the phone shows the touch IC under this one and no
// other, so the touch path's hardware identity does not move at all.
//
// What DOES move is the interrupt number: the 9820's hsi2c@104B0000 declares
// interrupts = <0x0 0x184 0x0>, i.e. SPI 388, where the 9810's was SPI 404.
// With the repo's convention (GSIV = SPI + 32) that is GSIV 420, not 436.
// The bus clock is declared as default-clk = <0xbebc200> = 200 MHz, which
// matches the 9810 driver's EXYNOS_HSI2C_SOURCE_CLOCK_HZ.
// ---------------------------------------------------------------------------
//
#define EXYNOS_TOUCH_HSI2C_BASE   0x00000000104B0000ULL   // VERIFIED (hsi2c@104B0000)
#define EXYNOS_TOUCH_HSI2C_SIZE   0x0000000000001000ULL   // VERIFIED
#define EXYNOS_TOUCH_I2C_ADDR     0x48                    // VERIFIED (touchscreen@48)
#define EXYNOS_TOUCH_HSI2C_GSIV   420                     // VERIFIED (SPI 388 + 32)
#define EXYNOS_HSI2C_SOURCE_CLOCK_HZ 200000000u           // VERIFIED (default-clk)
//
// The touch bus is USI03's USI block: hsi2c@104B0000 clocks = 509 DOUT_CLK_PERIC0_USI03_USI and
// 493 GATE_USI03_USI (dt-bindings/clock/exynos9820.h). Its mode register is usi@1042101C
// (label usi_3, usi_v2_mode "i2c") in the PERIC0 USI sysreg page. notes/m12-touch.md.
//
#define EXYNOS_PERIC0_USI_SYSREG_BASE   0x0000000010421000ULL   // VERIFIED (usi@10421000..usi@10421050 reg)
#define EXYNOS_USI03_MODE_OFFSET        0x1Cu                   // VERIFIED (usi@1042101C, label usi_3)

//
// The panel blob on THIS model is tsp_sec/y761_beyond0_phole.bin — a y761,
// the same family the Windows driver is already named after. The 9820 kernel's
// source directory is sec_ts/y771 and its default blob is y771_beyond2.bin;
// the S10 (beyond1lte) used y771_beyond1_phole.bin. So "y771" names the kernel
// driver family, not this phone's panel IC.
//
// The node also declares, per model: sec,project_name = BEYOND0G970,
// sec,afe_base = 0x0205 (the S10's was 0x0206), sec,tclm_level = 2,
// sec,max_coords = 4096 x 4096, sync-reportrate-120, support_dex_mode and
// support_ear_detect_mode. There is NO support_fod on this node, so the
// fingerprint-on-glass complication the 9820 .dtsi family carries does not
// apply to the donation device.
//
// The GPIO pair the bus uses is unchanged: gpio_scl/gpio_sda point at
// phandle 0x74 (gpp1) pins 4 and 5, exactly as on the 9810.
//
#define EXYNOS_TOUCH_GPIO_SCL_PIN 4                       // VERIFIED (gpp1 pin 4)
#define EXYNOS_TOUCH_GPIO_SDA_PIN 5                       // VERIFIED (gpp1 pin 5)

//
// ---------------------------------------------------------------------------
// GPIO  [PERIC0 unchanged, ALIVE moves]
//
// VERIFIED: pinctrl@10430000, compatible "samsung,exynos9820-pinctrl", reg
// <0x0 0x10430000 0x1000>, interrupts SPI 0x17c = 380 (GSIV 412).
// The ALIVE GPIO is not a node of its own: exynos-pm names it via reg-names,
// giving 0x15850000 where the 9810 had 0x14050000.
// ---------------------------------------------------------------------------
//
#define EXYNOS_PERIC0_GPIO_BASE   0x0000000010430000ULL   // VERIFIED (pinctrl@10430000)
#define EXYNOS_PERIC0_GPIO_SIZE   0x0000000000001000ULL   // VERIFIED
#define EXYNOS_ALIVE_GPIO_BASE    0x0000000015850000ULL   // VERIFIED (exynos-pm gpio_alive_base)
#define EXYNOS_ALIVE_GPIO_SIZE    0x0000000000001000ULL   // VERIFIED

//
// ALIVE GPIO bank + EINT register map (D8: hardware buttons, audio IRQs).
//
// VERIFIED from the vendor pinctrl driver (LineageOS exynos9820 kernel,
// drivers/pinctrl/samsung/pinctrl-exynos-arm64.c exynos9820_pin_banks0 +
// pinctrl-exynos.h offsets), cross-checked against the shipped DTB — see
// notes/d8-audio.md §3. Bank type exynos9810_bank_type_alive: per bank
// CON @+0x00 (4 bits/pin), DAT @+0x04, PUD @+0x08 (4 bits/pin), PDN @+0x0C.
// The 9820 has NO leading etc1 bank (the 9810 did) — gpa banks start at 0.
//
#define EXYNOS_ALIVE_GPA0_OFF     0x000                   // VERIFIED (exynos9820_pin_banks0)
#define EXYNOS_ALIVE_GPA1_OFF     0x020                   // VERIFIED
#define EXYNOS_ALIVE_GPA2_OFF     0x040                   // VERIFIED
#define EXYNOS_ALIVE_GPA3_OFF     0x060                   // VERIFIED
#define EXYNOS_ALIVE_GPA4_OFF     0x080                   // VERIFIED (2 pins)
#define EXYNOS_GPIO_BANK_DAT      0x004                   // VERIFIED (bank_type reg_offset[1])
#define EXYNOS_GPIO_BANK_PUD      0x008                   // VERIFIED (bank_type reg_offset[2])

// Wakeup-EINT block (the shipped pinctrl node's child is compatible
// "samsung,exynos7-wakeup-eint" -> exynos7_wkup_irq_chip offsets).
// EINT_CON: one 32-bit word per bank at 0x700 + 4*bank, 4-bit field per pin
// (trigger encoding LOW=0 HIGH=1 FALLING=2 RISING=3 BOTH=4).
// FLTCON: 0x800 + 8*bank, 8-bit field per pin, FLTCON0 covers pins 0-3 and
// FLTCON1 (+4) pins 4-7; EN=(1<<7) SEL=(1<<6) WIDTH=0x3F (the vendor sets
// EN|SEL width 0 for the gpa banks — same value the validated 9810 touch
// driver writes). MASK: 0x900 + 4*bank, 1 bit/pin (1 = masked). PENDING:
// 0xA00 + 4*bank, 1 bit/pin, write-1-clears.
//
#define EXYNOS_EINT_CON_BASE      0x700                   // VERIFIED (EXYNOS7_WKUP_ECON_OFFSET)
#define EXYNOS_EINT_FLTCON_BASE   0x800                   // VERIFIED (EXYNOS_GPIO_EFLTCON_OFFSET)
#define EXYNOS_EINT_MASK_BASE     0x900                   // VERIFIED (EXYNOS7_WKUP_EMASK_OFFSET)
#define EXYNOS_EINT_PENDING_BASE  0xA00                   // VERIFIED (EXYNOS7_WKUP_EPEND_OFFSET)

// Hardware keys (shipped overlay ov00.dts gpio_keys + vendor input dtsi):
// all four are EINT-function GPIOs with external pull-up (pressed = low),
// debounce 10 ms; POWER and BIXBY are the only wakeup sources. SPI numbers
// VERIFIED from include/dt-bindings/interrupt-controller/exynos9820.h
// (INTREQ__ALIVE_EINTn = 5 + n); GSIV = SPI + 32 (repo rule).
//
#define EXYNOS_KEY_VOLUP_PIN      3                      // VERIFIED (gpa0-3)
#define EXYNOS_KEY_VOLDN_PIN      4                      // VERIFIED (gpa0-4)
#define EXYNOS_KEY_BIXBY_PIN      6                      // VERIFIED (gpa0-6, "KEY_WINK")
#define EXYNOS_KEY_POWER_PIN      4                      // VERIFIED (gpa2-4)
#define EXYNOS_KEY_VOLUP_GSIV     40                     // VERIFIED (SPI 8 + 32)
#define EXYNOS_KEY_VOLDN_GSIV     41                     // VERIFIED (SPI 9 + 32)
#define EXYNOS_KEY_BIXBY_GSIV     43                     // VERIFIED (SPI 11 + 32)
#define EXYNOS_KEY_POWER_GSIV     57                     // VERIFIED (SPI 25 + 32)
#define EXYNOS_CODEC_IRQ_GSIV     55                     // VERIFIED (gpa2-2 = SPI 23 + 32; CS47L93 irq line, D8-audio)

//
// ---------------------------------------------------------------------------
// ABOX — the Exynos 9820 audio subsystem (DSP + I2S + DMA), D8 audio.
//
// VERIFIED from the shipped base DTB (dumps/hhxj6/base.dts:5312-5343):
// abox@18c50000 reg = ifr 0x18C50000+0x10000, sysreg 0x18C10000+0x10000,
// sram 0x18D00000+0x59000, timer 0x18CE0000+0x10000; its GIC child
// abox-gic@18cf0000 = gicd 0x18CF1000+0x1000, gicc 0x18CF2000+0x1004;
// DMA goes through sysmmu@18E00000. Two CA32 ("Calliope") DSP cores —
// audio PCM is graph-driven inside that firmware; there is NO
// firmware-free mode in the vendor driver (notes/d8-audio-linux.md Q2).
// Recorded now for the D8 rung-1/2 ladder; nothing here is initialised by
// the firmware today.
// ---------------------------------------------------------------------------
//
#define EXYNOS_ABOX_IFR_BASE      0x0000000018C50000ULL   // VERIFIED (abox@18c50000 reg[0])
#define EXYNOS_ABOX_IFR_SIZE      0x0000000000010000ULL   // VERIFIED
#define EXYNOS_ABOX_SYSREG_BASE   0x0000000018C10000ULL   // VERIFIED (reg[1] "sysreg")
#define EXYNOS_ABOX_SYSREG_SIZE   0x0000000000010000ULL   // VERIFIED
#define EXYNOS_ABOX_SRAM_BASE     0x0000000018D00000ULL   // VERIFIED (reg[2] "sram")
#define EXYNOS_ABOX_SRAM_SIZE     0x0000000000059000ULL   // VERIFIED
#define EXYNOS_ABOX_TIMER_BASE    0x0000000018CE0000ULL   // VERIFIED (reg[3] "timer")
#define EXYNOS_ABOX_TIMER_SIZE    0x0000000000010000ULL   // VERIFIED
#define EXYNOS_ABOX_GICD_BASE     0x0000000018CF1000ULL   // VERIFIED (abox-gic child)
#define EXYNOS_ABOX_GICD_SIZE     0x0000000000001000ULL   // VERIFIED
#define EXYNOS_ABOX_GICC_BASE     0x0000000018CF2000ULL   // VERIFIED
#define EXYNOS_ABOX_GICC_SIZE     0x0000000000001004ULL   // VERIFIED
#define EXYNOS_ABOX_SYSMMU_BASE   0x0000000018E00000ULL   // VERIFIED (sysmmu@18E00000, iommus phandle)
#define EXYNOS_ABOX_SYSMMU_SIZE   0x0000000000009000ULL   // VERIFIED (sysmmu@18E00000 reg = +0x9000, base.dts:2692)

//
// ---------------------------------------------------------------------------
// Display: DECON (display controller) + DSIM (MIPI DSI master)  [MOVES, and splits]
//
// VERIFIED node names: decon_f@0x19030000 and decon_t@0x19050000 (the 9810 had
// one DECON at 0x16030000), dsim at 0x19080000. decon_f declares reg size 0x10000
// and interrupts SPI 0xb7, 0xb8, 0xbd, 0x197 (GSIVs 215, 216, 221, 439).
//
// As on the 9810, these are NOT initialised for bring-up: the firmware reuses
// whatever sboot already lit. They exist for a future native display driver,
// and for the external-monitor path (DP Alt Mode out of the phone's own
// connector — see notes/dock-desktop.md §11).
// ---------------------------------------------------------------------------
//
#define EXYNOS_DECON_F_BASE       0x0000000019030000ULL   // VERIFIED (decon_f@0x19030000)
#define EXYNOS_DECON_F_SIZE       0x0000000000010000ULL   // VERIFIED
#define EXYNOS_DECON_T_BASE       0x0000000019050000ULL   // VERIFIED (decon_t@0x19050000)
#define EXYNOS_DECON_T_SIZE       0x0000000000010000ULL   // VERIFIED (live DTB reg, notes/s10e-dtb-mmio.md S02)
#define EXYNOS_DSIM_BASE          0x0000000019080000ULL   // VERIFIED (dsim@0x19080000)
#define EXYNOS_DSIM_SIZE          0x0000000000010000ULL   // CONSERVATIVE: the live DTB declares THREE ranges - 0x19080000+0x300, 0x19161000+0x800, 0x19160000+0x100 - so 0x10000 is a superset of the first and the extent discrepancy stays open (notes/s10e-dtb-mmio.md S02)

//
// ---------------------------------------------------------------------------
// Watchdog (Samsung s3c2410-style)  [SAME base as the 9810]
//
// VERIFIED: watchdog_cl0@10050000 and watchdog_cl2@10060000, at the same
// addresses the 9810 uses. This is what lets the storage driver's hardware-
// watchdog servicer and the firmware's EBS arm transfer as-is.
// ---------------------------------------------------------------------------
//
#define EXYNOS_WDT_BASE           0x0000000010050000ULL   // VERIFIED (watchdog_cl0@10050000)
#define EXYNOS_WDT_SIZE           0x0000000000001000ULL   // VERIFIED (live DTB: watchdog_cl0 and watchdog_cl2 both +0x1000, notes/s10e-dtb-mmio.md S02)

//
// ---------------------------------------------------------------------------
// Framebuffer  [BASE NOW READ]
//
// The S10e's panel is 1440 x 3040, not the S9+'s 1440 x 2960. The base is
// VERIFIED on the device from its own kernel command line
// (/proc/device-tree/chosen/bootargs): s3cfb.bootloaderfb=0xca000000 — the
// bootloader's scanout, which is what the firmware draws into. The 9810's
// 0xCC000000 was never right here.
//
// No DT node reserves it (every reg property at depth <= 3 was scanned), so it
// is ordinary bank0 DRAM the vendor happens to use as its framebuffer: the map
// must publish it as reserved, not as RAM, or Windows would claim the page.
// Region: 0xCA000000 .. 0xCA964AFF at 1080 x 2280 x 4 = 9849600 bytes.
//
// GEOMETRY CORRECTED 2026-10-10 (notes/m8-uefi-image.md B44): the S10e panel is
// s6e3fa7_beyond0, resolution = <1080 2280> (stock G970F DTBO,
// fragment@lcd/__overlay__/s6e3fa7_beyond0). 1440 x 3040 was the S10/S10+ panel
// (s6e3ha9) and mis-strided every framebuffer diagnostic; the device photo of
// boot #10 (24 cells per line, 8-cell shift per line) matches 1080 x 32 bpp.
// ---------------------------------------------------------------------------
//
#define EXYNOS_FB_BASE            0x00000000CA000000ULL   // VERIFIED (s3cfb.bootloaderfb, live cmdline)
#define EXYNOS_FB_WIDTH           1080                    // VERIFIED (DTBO s6e3fa7_beyond0 resolution)
#define EXYNOS_FB_HEIGHT          2280                    // VERIFIED (same node)
#define EXYNOS_FB_BYTES_PER_PIXEL 4                       // consistent with the boot #10 photo (B44); format still unread
#define EXYNOS_FB_STRIDE_PIXELS   EXYNOS_FB_WIDTH         // width*4, as on the 9810
#define EXYNOS_FB_SIZE            ((UINT64)EXYNOS_FB_STRIDE_PIXELS * \
                                   EXYNOS_FB_HEIGHT * EXYNOS_FB_BYTES_PER_PIXEL)

//
// ---------------------------------------------------------------------------
// IRQ numbers the ACPI tables will need (SPI + 32 = GSIV, the repo's rule)
//
//   DWC3 core        (usb@10C00000/dwc3)   SPI 0xf1 = 241   GSIV 273
//   USB PHY (two)    (phy@10B00000)        SPI 0xf5, 0xf6   GSIV 277, 278
//   HSI2C #10        (hsi2c@104B0000)      SPI 0x184 = 388  GSIV 420
//   pinctrl PERIC0   (pinctrl@10430000)    SPI 0x17c = 380  GSIV 412
//   decon_f          (decon_f@0x19030000)  SPI 0xb7,0xb8,0xbd,0x197
//                                          GSIV 215, 216, 221, 439
// ---------------------------------------------------------------------------
//

#endif // EXYNOS9820_H_
