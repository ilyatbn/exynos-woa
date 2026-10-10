/** @file
  Beyond0LteUfsHcDxe - platform UFS Host Controller driver for Exynos 9820 /
  Galaxy S10e (beyond0lte).

  Ported from Star2LtePkg/Drivers/Star2LteUfsHcDxe (Exynos 9810 / Galaxy S9+).
  Every UFS window is remapped to the EXYNOS_UFS_* macros in
  Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h (all VERIFIED reads); the
  intra-window vendor offsets carry unchanged (Exynos9820.h:260-262). The 9810's
  PRAM/diag telemetry channel (0xFED1xxxx) and the framebuffer HUD are COMPILED
  OUT, not retargeted: 0xFED1xxxx is inside bank0 DRAM on this SoC and inside the
  vendor's debug_snapshot region, and the fb base is unread (EXYNOS_FB_BASE = 0)
  - see notes/s10e-live-reads.md 7.3 and notes/m8-ufs-hc.md. Clock rates, the
  PMU phy-isolation offset, the opt-suite DME value and the UFS RefClk category
  have no 9820 read and stay marked UNVERIFIED.

  NOTHING HERE IS VALIDATED ON AN S10e. The boot this produces is Guided, not
  Ready (repo golden rule 3). See Beyond0LteUfsHcDxe.inf for the same warning.

  sboot already initialized the UFS PHY/UniPro link to load the kernel, and we
  chainload from that state, so the link is expected to be UP - we do NOT re-init
  the PHY here. If a future device needs vendor PHY/UniPro setup, implement the
  optional EDKII_UFS_HC_PLATFORM_PROTOCOL (UfsPassThruDxe SOMETIMES_CONSUMES it).

  DMA COHERENCY: this SoC has no IOMMU in our path and DRAM is identity-mapped
  WRITE-BACK cacheable, while the UFS engine's DMA is assumed NON-coherent with
  the CPU D-cache. So:
   - AllocateBuffer (common buffers = the UTP descriptor/UPIU rings the device
     and CPU both poll) returns UNCACHED pages, removing the need for per-access
     maintenance on those structures.
   - Map/Unmap (streaming PRDT data buffers) perform explicit cache maintenance
     based on direction.
   - Map is identity (DeviceAddress == HostAddress) because DRAM is identity
     mapped and there is no remapping bus master unit.
**/

#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/BaseLib.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <Protocol/DevicePath.h>
#include <Protocol/HardwareInterrupt.h>
#include <Protocol/ScsiPassThruExt.h>

#include <Platform/Exynos9820.h>

//
// ---------------------------------------------------------------------------
// PRAM / diag telemetry channel
//
// The 9810 flow kept a UINT32 diag block at 0xFED13000 and a ramoops-style
// console at 0xFED14000, both read back by the BDS panel and by
// tools/diag-cycle.ps1 after a warm reboot. That was only legal because on the
// S9+ DRAM ended at 0xBC800000, so 0xFED1xxxx sat above every bank.
//
// On the 9820 that assumption is FALSE: bank0 runs 0x80000000..0x100000000, so
// 0xFED1xxxx is INSIDE conventional DRAM and inside the vendor's
// debug_snapshot.log_bcm region (0xfeb10000 + 0x400000). Writing there scribbles
// into memory the loader owns. The replacement channel has to be re-picked from
// the vendor DT's reserved-memory node before it can be used, and until that
// choice is made the whole channel is COMPILED OUT rather than pointed at a
// guess - mirroring BEYOND0LTE_PRAM_DIAG 0 in Beyond0LteAcpiPlatformDxe.c and
// PlatformMemoryMapLib.c (notes/s10e-live-reads.md 7.3).
//
#define BEYOND0LTE_UFS_DIAG  0

//
// Vendor device path so BDS ConnectController can bind UfsPassThruDxe (its
// Supported() opens BOTH gEfiDevicePathProtocolGuid and the HC protocol).
//
#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} UFSHC_DEVICE_PATH;
#pragma pack ()

// {3d2e9a14-6b7c-4f10-9a8d-2f1e0c4b5a60}
#define BEYOND0LTE_UFSHC_VENDOR_GUID \
  { 0x3d2e9a14, 0x6b7c, 0x4f10, { 0x9a, 0x8d, 0x2f, 0x1e, 0x0c, 0x4b, 0x5a, 0x60 } }

STATIC UFSHC_DEVICE_PATH  mUfsHcDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { sizeof (VENDOR_DEVICE_PATH), 0 } },
    BEYOND0LTE_UFSHC_VENDOR_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

//
// Tracks one streaming Map() so Unmap() can do the right post-DMA maintenance.
//
typedef struct {
  EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation;
  VOID                                 *HostAddress;
  UINTN                                Length;
  BOOLEAN                              Bounced;
} UFSHC_MAP_INFO;

STATIC EDKII_UFS_HOST_CONTROLLER_PROTOCOL  mUfsHc;

//
// Forward declarations for the cross-module diag counters (defined below).
//
STATIC VOID UfsHcDiagBump (IN UINTN Index);
STATIC VOID UfsHcDiagSet  (IN UINTN Index, IN UINT32 Value);
STATIC VOID UfsHcDiagOr   (IN UINTN Index, IN UINT32 Bits);

//
// Forward declaration for the framebuffer breadcrumb HUD (defined below). Lets the
// platform callbacks and the doorbell hooks drop on-screen checkpoints that survive
// a hang (the BDS diag panel only renders if enumeration RETURNS).
//
STATIC VOID Beyond0LteCrumb (IN CONST CHAR8 *Tag);

//
// PHASE TRAIL (diag[13]): each bit is OR'd in the first time that UFS init phase
// is reached, giving a compact "how far did we get + in what order" picture on the
// panel. Read as a bitmask. The phases run in roughly ascending bit order, so the
// highest set bit = the furthest phase reached before the stall.
//
#define UFS_PH_PREHCE       0x00000001u // EdkiiUfsHcPreHce callback entered
#define UFS_PH_FORCEHCS     0x00000002u // host-reset cleared FORCE_HCS to 0
#define UFS_PH_SWRST_OK     0x00000004u // host-reset SW_RST self-cleared (good)
#define UFS_PH_ISCLR        0x00000008u // host-reset cleared VS_IS idle indicator
#define UFS_PH_POSTHCE      0x00000010u // EdkiiUfsHcPostHce callback entered
#define UFS_PH_VENDOR       0x00000020u // Exynos vendor HCI setup done
#define UFS_PH_PRELINK      0x00000040u // EdkiiUfsHcPreLinkStartup entered
#define UFS_PH_PMA          0x00000080u // PMA (M-PHY) analog cal done
#define UFS_PH_PRELINKDONE  0x00000100u // PreLinkSetup returned
#define UFS_PH_DOORBELL     0x00000200u // first transfer doorbell rung
#define UFS_PH_NOP          0x00000400u // a NOP-OUT (0x00) transfer seen
#define UFS_PH_QUERY        0x00000800u // a QUERY (0x16) transfer seen
#define UFS_PH_COMMAND      0x00001000u // a SCSI COMMAND (0x01) transfer seen
#define UFS_PH_HCREAD       0x00002000u // EDK2 polled a register via UfsHcRead
#define UFS_PH_ISPROBE_PRE  0x00004000u // about to do the PostHce IS-write probe
#define UFS_PH_ISPROBE_POST 0x00008000u // PostHce IS-write probe RETURNED (no trap)
#define UFS_PH_UTRLCLR      0x00020000u // UTRLCLR (0x5C) force-clear of a stuck slot fired
#define UFS_PH_CLRSUPPRESS  0x00040000u // CYCLE 38: a UTRLCLR clear was SUPPRESSED (a SCSI command in-flight)
#define UFS_PH_RECOVER      0x00080000u // CYCLE 44: a kernel-style LINERESET recovery (re-PMC to HS) fired

#define BEYOND0LTE_SKIP_PREHCE_SW_RST  0  // A117 probe switch: 1 skips PreHce vendor LINK+UNIPRO SW_RST

//
// PMU phy-isolation control (UFS_PHY_CONTROL). From the device tree ufs-phy ->
// ufs-phy-sys reg = <0x14060724 0x4> (inside the PMU @0x14060000 window, which is
// in our MMU map). bit0 = 1 means PHY isolation bypassed / MPHY powered. sboot
// booted from UFS so this is already 1; we only READ it (a PMU WRITE from EL1 is
// trapped by RKP and resets the SoC), matching the reference which writes only if
// bit0 == 0. Captured in diag[14].
//
#define BEYOND0LTE_UFS_PHY_ISO  0ULL  // UNVERIFIED on 9820: PMU base 0x15860000 is a read, the intra-block phy-isolation offset was never read -> 0 skips the read

//
// Vendor-specific Interrupt Status (HCI_VENDOR_SPECIFIC_IS). bit20 = UFS idle
// indicator, asserted when the UFS link is reset; the reference clears it (W1C)
// during host reset. Offset within the VS block (0x11121100).
//
#define HCI_VS_IS           0x38u
#define HCI_VS_IS_IDLE      (1u << 20)


//
// *** UFS PROTECTOR (SMU) CONFIG - the media-read fix (cycle 54). *** The Exynos UFS protector
// (reg_ufsp) sits between the controller and NAND and GATES media DMA. Controller-internal commands
// (NOP/query/INQUIRY/READ-CAP) bypass it, but a real READ(10) of NAND media goes THROUGH it - which is
// why cycles 51-53 had BlockIo=1 (metadata ok) yet READ(10) LBA0 totally silent (buf=0, resp=0, DL=0).
// The reference re-runs exynos_ufs_smu_init on EVERY controller init (ufs-exynos.c L616); ufs-exynos-
// smu.h's fallback shows the exact register writes: region 0 = whole device (SBEGIN0=0, SEND0=max),
// all LUNs (SLUN0=0xFF), control SCTRL0=0xF1 (access allowed). Our HCE resets the protector, so it MUST
// be re-opened or media reads are silently dropped. *** CORRECT BASE = 0x11130000 *** (mapped via
// EXYNOS_UFS_PHY_BASE, size 0x1000 -> covers regs 0x200-0x27C). The OLD bypass wrote these to 0x11110000
// (= the UNIPRO block, WRONG) and was then deleted on a false "sboot left it open" assumption.
//
#define UFSP_BASE          EXYNOS_UFS_UFSP_BASE   // reg_ufsp (UFS protector), NOT 0x11110000 (=UNIPRO)
#define UFSP_RCTRL         0x000u
#define UFSP_RSECURITY     0x010u
#define UFSP_WSECURITY     0x110u
#define UFSP_NSSMU         (1u << 14)
#define UFSP_DESCTYPE3     0x00180000u   // UFSPRSECURITY DESCTYPE=(3<<19), bits 19,20. WOADUMP working RSECURITY=0xEFFA6492 (DESCTYPE=3); we inherit 0xEFE26492 (DESCTYPE=0). FMP media-DMA descriptor-type / AxPROT group (ufs-exynos.h CFG_AXPROT).
#define UFSP_SBEGIN0       0x200u
#define UFSP_SEND0         0x204u
#define UFSP_SLUN0         0x208u
#define UFSP_SCTRL0        0x20Cu
#define UFS_VS_BASE        EXYNOS_UFS_VS_BASE
#define UFS_VS_HCI_MISC    0x0B4u            // FORCE_HCS
#define UFS_HCI_CORECLK_EN (1u << 4)         // HCI_CORECLK_CTRL_EN
#define HCI_SMU_ABORT_MATCH_INFO 0x10Cu      // Exynos vendor HCI: SMU/protector abort match info

//
// Exynos secure-world SMC IDs for the FMP/SMU (kernel include/linux/smc.h + drivers/crypto/fmp/smu_dev.c).
// The FMP descriptor-type (UFSPRSECURITY DESCTYPE) is SECURE-ONLY - an EL1 MMIO write SErrors (cycle 81),
// so it is set via the secure monitor exactly like exynos_ufs_smu_sec_cfg(). exynos_smc() is literally
// "dsb sy; smc #0" with args in x0..x3 and the result in x0; Linux issues it from EL1, so our EL1 UEFI can.
//
#define SMC_CMD_FMP_SECURITY  0xC2001810ULL  // exynos_smc(cmd, 0, SMU_EMBEDDED, desc_type) -> set FMP DESCTYPE
#define SMC_CMD_FMP_DISK_KEY_STORED 0xC2001820ULL // A119: exynos_smc(cmd, 0, phys(key), key_size) -> stash disk key in secure FMP
#define SMC_CMD_FMP_DISK_KEY_SET 0xC2001830ULL    // A119: exynos_smc(cmd, 0, 0, 0) -> activate previously stored disk key
#define SMC_CMD_SMU           0xC2001850ULL  // exynos_smc(cmd, SMU_INIT, SMU_EMBEDDED, 0)   -> SMU init
#define SMC_CMD_FMP_SMU_RESUME 0xC2001860ULL // A113: exynos_smc(cmd, 0, SMU_EMBEDDED, 0) -> SMU/FMP resume (restore secure ctx)
#define SMC_CMD_FMP_SMU_DUMP  0xC2001870ULL  // A118: exynos_smc(cmd, 0, SMU_EMBEDDED, offset) -> SMU/FMP secure dump/status
#define SMC_CMD_UFS_LOG        0xC2001880ULL  // A120: kernel issues this after Exynos UFS host_reset/resume
#define SMU_EMBEDDED          0u             // enum smu_id
#define SMU_INIT_CMD          0u             // enum smu_command SMU_INIT
#define CFG_DESCTYPE_3        3u             // crypto/smu.h CFG_DESCTYPE (FMP enabled)

STATIC UINT8  mA119FmpDiskKey[64] = {
  0x27, 0x18, 0x28, 0x18, 0x28, 0x45, 0x90, 0x45,
  0x23, 0x53, 0x60, 0x28, 0x74, 0x71, 0x35, 0x26,
  0x62, 0x49, 0x77, 0x57, 0x24, 0x70, 0x93, 0x69,
  0x99, 0x59, 0x57, 0x49, 0x66, 0x96, 0x76, 0x27,
  0x31, 0x41, 0x59, 0x26, 0x53, 0x58, 0x97, 0x93,
  0x23, 0x84, 0x62, 0x64, 0x33, 0x83, 0x27, 0x95,
  0x02, 0x88, 0x41, 0x97, 0x16, 0x93, 0x99, 0x37,
  0x51, 0x05, 0x82, 0x09, 0x74, 0x94, 0x45, 0x92
};

STATIC
UINT64
Beyond0LteSmc (
  UINT64  Cmd,
  UINT64  Arg1,
  UINT64  Arg2,
  UINT64  Arg3
  )
{
  register UINT64  x0 __asm__ ("x0") = Cmd;
  register UINT64  x1 __asm__ ("x1") = Arg1;
  register UINT64  x2 __asm__ ("x2") = Arg2;
  register UINT64  x3 __asm__ ("x3") = Arg3;

  __asm__ __volatile__ (
    "dsb sy\n\t"
    "smc #0\n\t"
    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
    :
    : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
      "x11", "x12", "x13", "x14", "x15", "x16", "x17"
    );
  return x0;
}

#if BEYOND0LTE_UFS_DIAG
STATIC
UINT32
Beyond0LteSmcReadSfr (
  IN  UINT32   Addr,
  OUT UINT32  *Status
  )
{
  //
  // *** A115: read a secure-only SFR via the Exynos EL3 monitor's SMC_CMD_REG backdoor (replicates the kernel's
  // exynos_smc_readsfr, arch/arm64/kernel/exynos-smc.S): x0 = SMC_CMD_REG ((u32)-101 = 0xFFFFFF9B); x1 =
  // (addr>>2) | SMC_REG_CLASS_SFR_R (0x3<<30). After smc #0: x0 = status (0=ok), x2 = the SFR value. This lets
  // EL1 read registers EL3 normally guards (FMP / UFS-Protector / TZASC) to see the secure-world enforcement.
  //
  register UINT64  x0 __asm__ ("x0") = 0x00000000FFFFFF9BULL;
  register UINT64  x1 __asm__ ("x1") = (UINT64)((Addr >> 2) | 0xC0000000u);
  register UINT64  x2 __asm__ ("x2") = 0;
  register UINT64  x3 __asm__ ("x3") = 0;

  __asm__ __volatile__ (
    "dsb sy\n\t"
    "smc #0\n\t"
    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
    :
    : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
      "x11", "x12", "x13", "x14", "x15", "x16", "x17"
    );

  *Status = (UINT32)x0;
  return (UINT32)x2;
}
#endif

STATIC
VOID
Beyond0LteUfspConfig (
  VOID
  )
{
  //
  // *** A104 REVERTED (Synchronous Exception / data abort): the GPD0 VCC-enable accessed the PERIC1 pin-
  // controller @0x10830000, which is NOT in EDK2's MMU map (only the UFS/PMU windows are mapped) -> translation
  // fault at boot. The VCC theory is also weak: sboot READ the boot image from UFS NAND (so VCC was ON) and the
  // GPIO output latch persists (EDK2 never clears it). To actually test VCC would require mapping 0x10830000
  // into the platform virtual-memory map first. Removed to restore booting (A103 = WC + DESCTYPE-3).
  //

  //
  // Open UFS protector region 0 to the whole device (exynos_ufs_smu_init, ufs-exynos-smu.h). diag[15]
  // = SCTRL0 BEFORE (0x00 => was blocked/secure); diag[16] = SCTRL0 AFTER (0xF1 => write stuck = EL1 can
  // configure the SMU = fix is live; 0x00 => secure-locked, would need an SMC). The block is mapped
  // device memory (EXYNOS_UFS_PHY_BASE) so these are normal-world MMIO, exactly like the Linux writel path.
  //
  UfsHcDiagSet (15, MmioRead32 ((UINTN)UFSP_BASE + UFSP_SCTRL0));
  //
  // *** CYCLE A84: REMOVED the direct EL1 MMIO writes to the UFSP protector (SBEGIN0/SEND0/SLUN0/SCTRL0).
  // The WORKING KERNEL configures the protector ONLY via the secure SMCs (exynos_ufs_smu_sec_cfg +
  // exynos_ufs_smu_init) below - it does NO direct EL1 protector writes. Our EL1 0xF1/0xFFFFFFFF "open-all"
  // writes (here AND the A43 re-write after the SMC) may OVERRIDE the secure SMU_init's correct config and
  // turn the FMP inline-crypto ON for media reads (-> READ(10) DATA-IN never arrives, OCS 0x0F). Test: SMCs alone.
  //
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  UfsHcDiagSet (16, MmioRead32 ((UINTN)UFSP_BASE + UFSP_SCTRL0));

  //
  // *** CYCLE 82: set the FMP descriptor type (DESCTYPE=3) via the SECURE-WORLD SMC. *** Cycle 81 proved a
  // direct EL1 MMIO write to UFSPRSECURITY SErrors (it is secure-only). The working kernel sets DESCTYPE via
  // exynos_ufs_smu_sec_cfg() -> exynos_smc(SMC_CMD_FMP_SECURITY, 0, SMU_EMBEDDED, CFG_DESCTYPE_3)
  // [drivers/crypto/fmp/smu_dev.c], in the order sec_cfg THEN smu_init (ufs-exynos.c L615-616). Replicate
  // that exact secure call from EL1 (smc #0 -> EL3, identical to Linux's EL1 path).
  //
  // *** A98 DESCTYPE=0 ROOT-CAUSE TEST: SKIP the FMP DESCTYPE=3 + SMU_init SMCs. EDK2 INHERITS DESCTYPE=0 (FMP
  // OFF, RSECURITY=0xEFE26492) from sboot; the kernel forces DESCTYPE=3 ONLY because it uses FMP disk ENCRYPTION
  // (we don't). Forcing DESCTYPE=3 turns ON the FMP engine that rejects READ(10)/READ BUFFER (Type A) + SBFES's
  // every >=512B data transfer (Type B). Leave inherited DESCTYPE=0 + standard 16-byte UFSHCI PRDT.
  // Beyond0LteSmc (SMC_CMD_FMP_SECURITY, 0, SMU_EMBEDDED, CFG_DESCTYPE_3);
  // Beyond0LteSmc (SMC_CMD_SMU, SMU_INIT_CMD, SMU_EMBEDDED, 0);
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  //
  // *** CYCLE A24: re-issue + CAPTURE the secure-monitor returns (x0). The first call set the FMP DESCTYPE,
  // the second SMU init; their returns were never checked. Stash to diag slots 300/301 so the recovery (A22,
  // UfsPassThruHci.c) can pram them: 0 = success; non-zero => the FMP/SMU secure config silently FAILED, which
  // would fault cold media reads (the FMP crypto path) while cached/metadata reads pass.
  //
  {
    // *** A102: RE-ENABLE the FMP DESCTYPE-3 SMC (kernel order: FMP_SECURITY then SMU_INIT). The LIVE working
    // kernel runs DESCTYPE-3 (RSECURITY=0xEFFA6492) even for UNENCRYPTED system reads (module dump 2026-06-26),
    // DISPROVING the A98 "DESCTYPE-3 is FMP-encryption-only" assumption. A live kernel module also confirmed
    // exynos_smc(FMP_SECURITY,0,SMU_EMBEDDED,3) returns 0 (success) from EL1. Capture EDK2's return (slot 300 /
    // panel "fmpr") + RSECURITY (slot 301 / panel "smur"):
    //   smur=0xEFFA6492 => DESCTYPE-3 STUCK (RKP allows EDK2's write -> next: 128B FMP PRDT + retest read);
    //   smur=0xEFE26492 => still DESCTYPE-0 (RKP context-gates EDK2's FMP_SECURITY write = hard wall);
    //   fmpr=0 => SMC returned success (real or RKP fake-success); fmpr!=0 => monitor rejected (error code).
    //
    // *** A109 DECISIVE FMP TEST: set DESCTYPE-0 (FMP transparent) instead of -3. A106-A108 proved the fault is
    // COMMAND-CATEGORY: data-transfer cmds (READ(10)/READ(6)/READ BUFFER) SBFES at EVERY size 128..4096, while
    // PARAMETER cmds (INQUIRY 36B / MODE SENSE 192B) PASS = the FMP signature. DESCTYPE-3 makes the controller
    // expect Samsung's extended 128-byte FMP PRDT entries on the data path, but EDK2's generic driver builds
    // STANDARD 16-byte UFSHCI PRDT entries => the controller misreads the data-path descriptor => SBFES.
    // DESCTYPE-0 = standard 16B PRDT (MATCHES EDK2) + FMP transparent (our partitions are unencrypted). Unlike
    // A98's DESCTYPE-0 (which carried other broken vars), this runs with the A46 byte-gran PRDT + iocoh-OFF + WC
    // bounce all in place. EXPECT smur=0xEFE26492. If the A108 READ BUFFER probe now PASSES (r6/o1 EFIst 00) =>
    // FMP/PRDT-format mismatch CONFIRMED as the root cause + FIXED.
    UINT64  FmpRet = Beyond0LteSmc (SMC_CMD_FMP_SECURITY, 0, SMU_EMBEDDED, CFG_DESCTYPE_3);   // A112: back to DESCTYPE-3 (kernel-faithful: kernel CFG_DESCTYPE=3); now paired with iocoh ON
    UINT64  SmuRet = Beyond0LteSmc (SMC_CMD_SMU, SMU_INIT_CMD, SMU_EMBEDDED, 0);
    //
    // *** A113: also issue the SMU/FMP RESUME SMC (exynos_ufs_smu_resume -> SMC_CMD_FMP_SMU_RESUME). The kernel's
    // COLD boot does FMP_SECURITY + SMU_INIT (== my sequence, which the kernel uses successfully) - so the SMCs are
    // NOT the gap. But MY UEFI is effectively a RESUME from sboot's already-initialized FMP state (sboot read the
    // boot image through the FMP), so SMU_INIT may RE-init the protector into a state that faults the data path,
    // whereas RESUME restores the working secure context. Additive + low-risk (worst case: error return, no effect).
    // Capture the return (panel "smres"): 0 => resume OK; non-zero => monitor rejected (LDFW drmdrv_result_t).
    //
    UINT64  SmuResume = Beyond0LteSmc (SMC_CMD_FMP_SMU_RESUME, 0, SMU_EMBEDDED, 0);
    UINT64  SmuDump0   = Beyond0LteSmc (SMC_CMD_FMP_SMU_DUMP, 0, SMU_EMBEDDED, 0x00);
    UINT64  SmuDump10  = Beyond0LteSmc (SMC_CMD_FMP_SMU_DUMP, 0, SMU_EMBEDDED, 0x10);
    UINT64  DiskKeyStored;
    UINT64  DiskKeySet;
    UINT64  UfsLogRet;
    WriteBackDataCacheRange (mA119FmpDiskKey, sizeof (mA119FmpDiskKey));
    DiskKeyStored = Beyond0LteSmc (SMC_CMD_FMP_DISK_KEY_STORED, 0, (UINTN)mA119FmpDiskKey, 32);
    DiskKeySet    = Beyond0LteSmc (SMC_CMD_FMP_DISK_KEY_SET, 0, 0, 0);
    UfsLogRet     = Beyond0LteSmc (SMC_CMD_UFS_LOG, 0, 0, 0);
    UINT32  RSec   = MmioRead32 ((UINTN)UFSP_BASE + UFSP_RSECURITY);
    //
    // The SMC/MMIO calls above are LIVE (they program the protector and the FMP);
    // only their RETURNS are diag-only. The returns are read-and-discarded here so
    // the declarations stay in scope for the panel block below, which compiles
    // against exactly these names when BEYOND0LTE_UFS_DIAG is turned back on.
    //
    (VOID)FmpRet;
    (VOID)SmuRet;
    (VOID)SmuResume;
    (VOID)SmuDump0;
    (VOID)SmuDump10;
    (VOID)DiskKeyStored;
    (VOID)DiskKeySet;
    (VOID)UfsLogRet;
    (VOID)RSec;
#if BEYOND0LTE_UFS_DIAG
    ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[300] = (UINT32)FmpRet;   // panel "fmpr" = FMP_SECURITY return (0=ok)
    ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[301] = RSec;             // panel "smur" = RSECURITY: EFE26492=DESCTYPE0, EFFA6492=DESCTYPE3
    ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[302] = (UINT32)SmuResume; // A113 panel "smres" = SMU_RESUME return (0=ok)
    ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[327] = (UINT32)SmuDump0;  // A118 panel "smd0" = SMU_DUMP offset 0x00 return
    ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[328] = (UINT32)SmuDump10; // A118 panel "smd1" = SMU_DUMP offset 0x10 return
    ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[329] = (UINT32)DiskKeyStored; // A119 panel "dks" = DISK_KEY_STORED return
    ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[366] = (UINT32)DiskKeySet;    // A119 panel "dka" = DISK_KEY_SET return
    ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[367] = (UINT32)UfsLogRet;      // A120 panel "ulog" = UFS_LOG return
    //
    // *** A115: validate the EL3 secure-SFR READ backdoor. Read UFSP_RSECURITY (0x11130010, known 0xEFFA6492 from
    // the EL1 MmioRead above) THROUGH the monitor. If slot 304 == 0xEFFA6492 AND slot 303 == 0, SMC_CMD_REG works
    // from my EL1 UEFI => I can then read (and try to WRITE) secure FMP/TZASC registers EL1 cannot. Also read the
    // protector SCTRL0 (0x1113020C) / SEND0 (0x11130204) via the monitor (slots 305/306) to compare the EL3 view
    // with the EL1 reads (panel ufsc/ufse). sfrst!=0 or sfrv!=EFFA6492 => the monitor refuses SFR reads from EL1.
    //
    {
      UINT32  SfrSt = 0xFFFFFFFFu;
      ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[304] = Beyond0LteSmcReadSfr ((UINT32)(EXYNOS_UFS_UFSP_BASE + UFSP_RSECURITY), &SfrSt);  // RSECURITY via EL3
      ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[303] = SfrSt;                                      // status (0=ok)
      ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[305] = Beyond0LteSmcReadSfr ((UINT32)(EXYNOS_UFS_UFSP_BASE + UFSP_SCTRL0), &SfrSt);  // UFSP_SCTRL0 via EL3
      ((volatile UINT32 *)(UINTN)0x00000000FED13000ULL)[306] = Beyond0LteSmcReadSfr ((UINT32)(EXYNOS_UFS_UFSP_BASE + UFSP_SEND0), &SfrSt);  // UFSP_SEND0 via EL3
    }
#endif
    __asm__ __volatile__ ("dsb sy" ::: "memory");
  }

  //
  // *** CYCLE A43: RE-OPEN the SMU region to ALL sectors AFTER the secure SMU_INIT SMC. *** The cold-read
  // SBFES is LBA-dependent (low-LBA GPT reads pass, high-LBA partition reads fault, no data reaches the
  // bounce) = the classic signature of the UFS-protector region only covering LOW sectors. We already wrote
  // allow-all (SBEGIN0=0/SEND0=max/SLUN0=0xFF/SCTRL0=0xF1) ABOVE, but the secure SMC_CMD_SMU(SMU_INIT) runs
  // AFTER it and may RESET region 0 to sboot's restrictive bootloader range. Re-assert allow-all here, as
  // the LAST writer, so high-LBA media DMA is permitted (if the block is EL1-writable; if it is secure-locked
  // these are harmless no-ops and the A43 wedge dump of ufspend/ufspctrl will reveal the locked state).
  //
  //
  // *** CYCLE A84: REMOVED the A43 post-SMC direct protector re-write. Let the secure SMU_init's config
  // stand (the kernel does no EL1 protector writes). diag[17] just reads back the SMC-set region end.
  //
  UfsHcDiagSet (17, MmioRead32 ((UINTN)UFSP_BASE + UFSP_SEND0));   // post-SMC region end (SMU_init-set)

  //
  // *** CYCLE 64: ENABLE UFS DMA IO-coherency (read-modify-write). *** Cycle 63 read this sysreg and the
  // panel showed iocoh=0x00000011 -> bits 8|9 (the 0x300 coherency bits the reference sets) are CLEAR, so
  // the Exynos UFS master's DMA is NON-coherent and sboot never enabled it. The reference
  // exynos_ufs_modify_sysreg() RMWs FSYS1_SYSREG 0x11010700 |= (BIT8|BIT9) so the UFS AXI master emits
  // shareable/coherent attributes - the ONE unreplicated DMA-path item, and the 4KB multi-frame DATA-IN is
  // exactly a DMA-path stall (small single-PRD control reads pass, the bigger media read never completes:
  // OCS stuck 0x0F, resp=0, no data). The cycle-63 READ did not fault, so the region is reachable from EL1;
  // now do the WRITE (faithful to the kernel, which RMWs this from EL1). diag[14] (panel "iocoh") = the
  // value READ BACK AFTER the write: 0x00000311 => write STUCK, coherent DMA now ON (then check partitions:
  // >=1 = WIN); 0x00000011 => write silently ignored (sysreg write-protected) => coherency is TZ-gated.
  // (If it traps we hang with no panel => the sysreg is RKP/TZ write-locked.)
  //
  {
    UINT32  IoCoh;

    IoCoh = MmioRead32 (EXYNOS_UFS_IOCOH_BASE);
    // *** CYCLE A49: iocoh OFF (clear bits 8|9) to MATCH our WC (non-cacheable) common buffers. *** A46's
    // byte-gran PRDT fixed the addressing (rxmatch/vis cleared), but the controller's data/response/OCS
    // WRITE still SBFES'd with iocoh=ON (A44) under BOTH WC (A48) and cached (A47) memory. Theory: with
    // iocoh ON the UFS master issues COHERENT/shareable AXI writes, which the interconnect REJECTS when the
    // target is NOT in the coherent domain (our WC = ARM Normal NON-cacheable, and the cached buffer was not
    // guaranteed inner-shareable) -> SBFES on the WRITE; coherent READS are tolerated (so descriptor reads
    // worked). The kernel only runs iocoh=ON because it pairs it with CACHED INNER-SHAREABLE dma-coherent
    // memory. For our WC buffers the master must be NON-coherent. The A32 iocoh-OFF test failed only because
    // the PRDT was still broken then; retry now with the correct byte-gran PRDT. A22 wedge iocoh -> 0x11.
    // *** CYCLE A85: iocoh OFF (clear bits 8|9) - revert the A83 coherent experiment. A83 (iocoh ON + WB
    // cached + LOW bounce 0x97f80000) FAILED exactly like A79 (coherent at high): part=00, rd0=0. Combined
    // with A79/A51, this eliminates the ENTIRE host memory-model/coherency/address class (coherent & WC, high
    // & low - all fail). The kernel's true buffer (0x6952f000) is below EDK2's 0x90000000 RKP-safe window =
    // unreachable. Back to reliable WC + iocoh OFF (the controller's writes are directly visible, no stale cache).
    // *** A96: iocoh ON - RE-TEST the coherent model on READ(6) (the Type-B data write). Prior iocoh-ON/WB tests
    // (A44/A83) ALL ran on READ(10) = Type-A-rejected BEFORE the data write => INVALID for the data path.
    // *** A101: iocoh ON (set BIT8|BIT9) - FAITHFUL kernel coherent model. Pairs with WB inner-shareable
    // descriptors + WB inner-shareable data bounce + NO per-transfer cache maintenance (= the clean
    // dma_alloc_coherent model). A96 set iocoh ON + WB on both buffers AND the controller reached the data
    // write (so WB descriptors are fine), but A96 KEPT the pre-DMA WriteBackInvalidate on the bounce, which
    // fights coherent DMA; removing that maintenance is the one variable A96 never tried. iocoh readback
    // (diag[14]/panel "iocoh") = 0x00000311 => write stuck, coherent DMA ON.
    //
    // *** A112: iocoh ON (set FSYS1 BIT8|BIT9) - the working kernel ALWAYS runs iocoh ON; A103/A109 ran it OFF.
    // With the command-category root cause (FMP/protector data path faults PRE-DATA on read-class commands) and the
    // FMP/SMU/descriptor stack now PROVEN faithful, iocoh is the one real kernel divergence left: the FMP/data AXI
    // master likely REQUIRES coherent (shareable) transactions, and with iocoh OFF the interconnect rejects them
    // (SBFES) for the read-class data path while metadata reads (non-FMP) tolerate it. Keep the WC bounce (single
    // variable vs A103). iocoh readback panel "ioco" should now be 0x00000311. Test the READ BUFFER probe: r6/o1
    // EFIst 00 => iocoh-ON was the wall (then chase any WC-vs-coherent buffer nuance); still 07 => iocoh exonerated.
    //
    MmioWrite32 (EXYNOS_UFS_IOCOH_BASE, IoCoh | 0x00000300u);   // A112: iocoh ON (BIT8|BIT9) - kernel-faithful coherent UFS DMA
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    UfsHcDiagSet (14, MmioRead32 (EXYNOS_UFS_IOCOH_BASE));
  }
}

//
// EXYNOS VENDOR HCI SETUP. Generic UfsPassThru only programs the JEDEC UFSHCI
// registers (CAP/HCE/UTRLBA/doorbell); it never touches the Exynos vendor HCI
// block at VS base 0x11121100. DATA_REORDER alone was DEVICE-DISPROVEN (it read
// back 0xA but the first transfer still failed with UEC PA=80000010), so the
// blocker is one of the OTHER vendor steps the Samsung bring-up does and EDK2
// omits. Of those, three are plain MMIO (no external PHY cal tables needed) and
// are the plausible "link trains but cannot move data" causes:
//   * CLKSTOP_CTRL bit4 -> enable the reference-clock OUTPUT to the UFS device
//     (a device with no refclk cannot run its own PHY).
//   * CPORT setup (0x114 then 0x110) -> arm the UniPro CPort that carries UPIUs;
//     without it the link reports ready but no UPIU is delivered.
//   * GPIO_OUT -> hardware-reset the device so it re-trains cleanly from its side
//     before DME_LINKSTARTUP, instead of inheriting sboot's half-torn-down state.
// Offsets cross-checked against Linux drivers/ufs/host/ufs-exynos.c (reg_hci =
// vs_hci) and reference Exynos9820Pkg ExynosUfsLib (ufs_pre_setup/ufs_vendor_setup).
// All are inside the proven EL1-writable VS window (DATA_REORDER write landed).
// The remaining Exynos PHY (PMA) calibration — ufs_cal_pre/post_link/pmc — needs
// Samsung's external cal tables and is NOT done here.
//
#define HCI_TXPRDT_ENTRY_SIZE   0x00u
#define HCI_1US_TO_CNT_VAL      0x0Cu   // mclk ticks per 1us (= mclk MHz), masked 0x3FF
#define HCI_RXPRDT_ENTRY_SIZE   0x04u
#define HCI_UTRL_NEXUS_TYPE     0x40u
#define HCI_UTMRL_NEXUS_TYPE    0x44u
#define HCI_SW_RST              0x50u   // vendor SW reset: bit0 LINK, bit1 UNIPRO
#define HCI_SW_RST_MASK         0x3u    // (UFS_UNIPRO_SW_RST | UFS_LINK_SW_RST)
#define HCI_DATA_REORDER        0x60u
#define HCI_GPIO_OUT            0x70u   // device HW reset line (bit0)
#define HCI_AXIDMA_BURST_LEN    0x6Cu
#define HCI_ERROR_EN_DL_LAYER   0x7Cu   // DFES data-link error-enable
#define HCI_ERROR_EN_N_LAYER    0x80u   // DFES network error-enable
#define HCI_ERROR_EN_T_LAYER    0x84u   // DFES transport error-enable
#define HCI_V2P1_CTRL           0x8Cu   // bit16 IA_TICK_SEL (interrupt-aggr tick = 1us_to_cnt)
#define HCI_CLKSTOP_CTRL        0xB0u   // bit4: refclk OUTPUT to device (clear = enable)
#define HCI_CLK_STOP_ALL        0x17u   // CLK_STOP_ALL: REFCLKOUT|REFCLK|UNIPRO_MCLK|UNIPRO_PCLK (b4,b2,b1,b0), NOT MPHY_APB(b3)
#define HCI_FORCE_HCS           0xB4u   // per-clock auto-stop ENABLEs; bit4=CORECLK_STOP_EN
#define HCI_FORCE_HCS_ONDEMAND  0x00000DE0u // WOADUMP working value: UNIPRO_M/PCLK(b5,b6)+REFCLK(b7)+MPHY_APB(b10)+b8/b11 STOP_EN (clock-on-demand auto-gate), CORECLK(b4)=0 (core always on)
#define HCI_UFS_ACG_DISABLE     0xFCu   // bit0: disable HW auto clock-gating (HWACG)
#define HCI_IOP_ACG_DISABLE     0x100u  // bit0=DISABLE_EN: SET = disable IOP auto clock-gating (A35: force IOP clock on)
#define HCI_MPHY_REFCLK_SEL     0x108u  // bit0: select MPHY refclk (fork select_refclk(true))
#define HCI_CPORT_CTRL_A        0x110u  // CPort connection setup (write 0x1)
#define HCI_CPORT_CTRL_B        0x114u  // CPort connection setup (write 0x22)

//
// DFES (Device Fatal Error Stop) error-enable values, faithful to the fork's
// exynos_ufs_config_intr(DFES_DEF_*_ERRS, layer). Each = DFES_ERR_EN(bit31) | the
// layer's default error mask. The fork configures DL/N/T (not PA/DME) from
// link_startup_notify. These are the last unmatched vendor writes in the init sequence.
//
#define HCI_DFES_DL_VAL         0x80002020u  // bit31 | RX_BUF_OF(b5) | PA_INIT(b13)
#define HCI_DFES_N_VAL          0x80000007u  // bit31 | b0 | b1 | b2
#define HCI_DFES_T_VAL          0x80000017u  // bit31 | b0 | b1 | b2 | b4
#define HCI_MPHY_REFCLK_SEL_BIT 0x00000001u

//
// init_host data-path completion bits (fork ufs-exynos.c exynos_ufs_init_host). These
// are the gap that explains "NOP retires but the first data-bearing query does not":
//   PRDT_PREFECT_EN (TXPRDT bit31)  — PRDT prefetch for the data/response path.
//   WLU_EN          (AXIDMA  bit31) — AXI-DMA write-LU enable: the DMA write-back of a
//                                     transfer's response/data. Without it a data-bearing
//                                     transfer's completion never fully registers, so the
//                                     controller never auto-retires its UTRLDBR doorbell
//                                     bit (a NOP has no data segment, so it sails through).
//   BURST_LEN(3)    = (3<<27)|3 = 0x18000003.   IA_TICK_SEL (V2P1 bit16).
//
#define HCI_PRDT_PREFECT_EN     0x80000000u
#define HCI_PRDT_SET_SIZE12     0x0000000Cu // 12 DWORDs/entry = standard 4 + 8-DWORD FMP crypto descriptor
#define HCI_PRDT_SET_SIZE_STD   0x00000004u // CYCLE 49: 4 DWORDs/entry = EDK2's standard 16B PRDT (no FMP desc)
#define HCI_WLU_EN              0x80000000u
#define HCI_BURST_LEN3          0x18000003u
//
// *** CYCLE A88: SUB-CACHE-LINE AXI DMA BURST. *** A86/A87 proved the host data-in fault is SIZE-thresholded
// (~64B), NOT media/FMP/cmd-support: every CONFIRMED success transfers <64B (INQUIRY 36B, REQ SENSE 18B,
// READ CAP 8B - one partial burst) while every failure needs >=64B (READ 4096B, READ BUFFER >=64B). The
// kernel's BURST_LEN(3) = 2^3 beats * 8B AXI = a 64-BYTE (one cache-line) burst. The kernel tolerates it only
// because its DMA buffer is IOMMU/cache-coherent; our non-coherent WC bounce faults (SBFES/OCS 0x07) on the
// full cache-line burst, so sub-64B (partial-burst) transfers pass and >=64B fail. Drop the burst below a
// cache line = BURST_LEN(1) = (1<<27)|1 = 0x08000001, so a 4096B read is many small sub-cache-line bursts
// (each like the working INQUIRY) instead of 64B cache-line bursts. If READ(10)/READ BUFFER now return 0x00,
// the cache-line burst was the wall.
//
#define HCI_BURST_LEN_A88       0x08000001u
#define HCI_IA_TICK_SEL         0x00010000u
#define HCI_1US_CNT        0x000000A6u   // UNVERIFIED on 9820: 9810 mclk ~166 MHz ticks/us; the 9820 mclk rate was never read

//
// Interrupt-aggregation control (standard UFSHCI reg at EXYNOS_UFS_HCI_BASE + 0x4C).
// We now DISABLE aggregation (write 0 = ufshcd_disable_intr_aggr). With aggregation
// ENABLED (0x81000F01) the controller batched completions and only committed them \u2014
// retiring the UTRLDBR bits \u2014 on an aggregated-interrupt ACK via an IS write, which
// is impossible here (IS @ 0x20 writes hard-hang our chainloaded EL1 context, device-
// confirmed 5x). Disabling aggregation makes each completion retire immediately with
// no batch-commit ack needed \u2014 the only completion path open to a poll-only driver
// that cannot touch IS. (The kernel's own make_hba_operational does exactly this in
// the !is_intr_aggr_allowed branch.)
//
#define UFS_HC_UTRIACR_OFF      0x4Cu
#define UFS_HC_UTRIACR_CFG      0x00000000u

//
// Interrupt Enable (standard UFSHCI reg at EXYNOS_UFS_HCI_BASE + 0x24). The kernel
// enables UFSHCD_ENABLE_INTRS before any transfer (ufshcd_make_hba_operational);
// generic EDK2 is poll-based and NEVER writes IE, leaving the controller's
// completion/doorbell-retire FSM ungated. We enable ONLY UTP_TRANSFER_REQ_COMPL
// (bit0) — the transfer-completion interrupt that the doorbell-retire path keys off
// — not the error/UIC bits (IS.UE is already asserted and the UFS GIC SPI is not
// wired in our EDK2, so we avoid any chance of an asserted-line storm). IE is a mask
// register with no W1C side effects, so writing it is safe (unlike IS @ 0x20).
//
#define UFS_HC_IE_OFF           0x24u
// *** CYCLE A41: match the WORKING KERNEL's full IE mask (ground-truth UFSREGDUMP: IE=0x00030ef5), was 0x1
// (UTRCS only). The instrumented LineageOS kernel runs with IE = UTRCS(b0)|UE(b2)|UIC link/pwr(b4-7)|UTMRCS(b9)|
// UCCS(b10)|DFES(b11)|HCFES(b16)|SBFES(b17). This controller's completion/transfer FSM is gated on IE (see note
// above), so enabling only b0 leaves the data-in/transport path mis-armed (cold DATA-IN rejected: rxmatch=1,
// which the kernel - with the full mask - never gets). SAFE in our polling driver: no UFS IRQ handler is
// registered, so the GIC never delivers/storms the unhandled IRQ (the prior 36M-storm was WITH a handler).
#define UFS_HC_IE_CFG           0x00030ef5u
#define UFS_HC_IS_OFF           0x20u
#define UFS_HC_IS_UTRCS         0x00000001u

//
// UIC error-code registers (standard UFSHCI, at EXYNOS_UFS_HCI_BASE + offset). Each is
// READ-TO-CLEAR: a non-zero value (bit31 = error valid, low bits = layer error code) is
// latched on a UIC error and cleared when read. Polling them during the stuck doorbell
// thus distinguishes a one-time latched error (seen once) from a CONTINUOUSLY re-
// asserting PHY failure (seen on many polls). PA=PHY-adapter, DL=data-link, TR=transport.
//
#define UFS_HC_UECPA_OFF        0x38u
#define UFS_HC_UECDL_OFF        0x3Cu
#define UFS_HC_UECT_OFF         0x44u

//
// EXYNOS VENDOR HOST RESET (faithful port of Linux exynos_ufs_host_reset /
// ExynosUfsLib ufs_pre_setup), the root-cause fix for "transfers never truly
// retire, doorbell never clears, and no software poke can clear it". sboot hands
// the controller off in a half-torn-down state; EDK2's HCE re-enable does NOT cold-
// reset the vendor LINK/UNIPRO block, so the controller's native completion +
// doorbell-clear never works. The reference sequence (which we now replicate
// exactly) is: (0) ensure PHY isolation is bypassed; (1) clear FORCE_HCS to 0 so
// ALL HCI clocks run during the reset (clearing only bit4 left UNIPRO/LINK clocks
// gated, which is why our earlier reset left swrst=1 stuck); (2) assert SW_RST=3
// and poll to 0; (3) clear the VS_IS idle indicator. Done at EdkiiUfsHcPreHce,
// before HCE. Breadcrumbs: phase trail bits in diag[13]; diag[9]=SW_RST residual
// (0=good); diag[14]=phy-iso readback; diag[15]=FORCE_HCS before clear;
// diag[16]=VS_IS before clear.
//
#if !BEYOND0LTE_SKIP_PREHCE_SW_RST
STATIC
VOID
Beyond0LteHostReset (
  VOID
  )
{
  UINT32  PhyIso;
  UINT32  Force;
  UINT32  VsIs;
  UINT32  Rst;
  UINT32  Tries;

  UfsHcDiagOr (13, UFS_PH_PREHCE);

  //
  // (0) Report PHY isolation state (PMU UFS_PHY_CONTROL bit0). Read-only: a PMU
  // write from EL1 is RKP-trapped, and sboot already bypassed isolation (bit0=1)
  // since it booted from UFS, so the reference's "write if bit0==0" never fires.
  //
  PhyIso = 0;
  if (BEYOND0LTE_UFS_PHY_ISO != 0) {
    PhyIso = MmioRead32 ((UINTN)BEYOND0LTE_UFS_PHY_ISO);
  }
  UfsHcDiagSet (14, PhyIso);

  //
  // *** CYCLE A23: UFS IO COHERENCY (the DT marks the UFS node `dma-coherent`). *** The kernel's
  // exynos_ufs_init_system -> exynos_ufs_modify_sysreg SETS bits 8|9 of SYSREG_FSYS 0x11010700 (DT child
  // "ufs-io-coherency": reg=<0x11010700 0x4>, mask=bits=BIT8|BIT9) so the UFS AXI master issues cache-
  // coherent (shareable) transactions through the CCI. Our driver never set it; a host-side
  // SYSTEM_BUS_FATAL_ERROR that recurs ONLY on cold NAND-fetch reads (control cmds + cached boot sectors
  // succeed) fits the interconnect rejecting the non-coherent transaction on the cold-fetch path. Match the
  // kernel - 0x11010700 is in the Periphs device map (EL1-writable, normal-world like the Linux writel).
  //
  {
    volatile UINT32  *IoCoh = (volatile UINT32 *)(UINTN)EXYNOS_UFS_IOCOH_BASE;
    *IoCoh = (*IoCoh & ~0x00000300u);   // *** A103: iocoh OFF (revert A101) - DESCTYPE-3 + WC non-coherent model
    __asm__ __volatile__ ("dsb sy" ::: "memory");
  }

  //
  // (1) Clear FORCE_HCS to 0 so every HCI clock (core/UNIPRO/MPHY) runs during the
  // reset. (Matches the reference's "if ((FORCE_HCS>>4)&0xF) FORCE_HCS=0".)
  //
  Force = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_FORCE_HCS);
  UfsHcDiagSet (15, Force);
  if (((Force >> 4) & 0xFu) != 0) {
    MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_FORCE_HCS, 0);
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    UfsHcDiagOr (13, UFS_PH_FORCEHCS);
  }

  //
  // (2) Assert the vendor LINK+UNIPRO software reset, then wait for it to self-clear.
  //
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_SW_RST, HCI_SW_RST_MASK);
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  Rst = HCI_SW_RST_MASK;
  for (Tries = 0; Tries < 100000u; Tries++) {
    Rst = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_SW_RST) & HCI_SW_RST_MASK;
    if (Rst == 0) {
      break;
    }
  }

  UfsHcDiagSet (9, Rst);
  if (Rst == 0) {
    UfsHcDiagOr (13, UFS_PH_SWRST_OK);
  }

  //
  // (3) Clear the VS_IS idle indicator (bit20, W1C) asserted by the link reset.
  //
  VsIs = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_VS_IS);
  UfsHcDiagSet (16, VsIs);
  if ((VsIs & HCI_VS_IS_IDLE) != 0) {
    MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_VS_IS, VsIs);
    UfsHcDiagOr (13, UFS_PH_ISCLR);
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
}
#endif

STATIC
VOID
Beyond0LteHciVendorSetup (
  VOID
  )
{
  UINT32  Clk;

  //
  // (0) *** CYCLE 79: FORCE_HCS = 0xDE0 (clock-on-demand), the WORKING-KERNEL value. ***
  // The WOADUMP register dump from the instrumented Linux proves the working driver runs with
  // FORCE_HCS(0xB4)=0x00000DE0 and CLKSTOP_CTRL(0xB0)=0x08. The FORCE_HCS *_STOP_EN bits ENABLE the
  // per-clock ON-DEMAND gating controller: UNIPRO_PCLK(b6)/MCLK(b5)/REFCLK(b7)/MPHY_APB(b10) auto-gate
  // when idle BUT UN-GATE on demand during a transfer; CORECLK(b4)=0 keeps the core clock always on.
  // A PRIOR cycle WRONGLY set FORCE_HCS=0 believing "0 forces all clocks on" - but 0 DISABLES the
  // on-demand controllers, leaving UNIPRO PCLK/MCLK STUCK gated (our panel: CLKSTOP=0x0B, fhcs=0 at the
  // stuck READ). With those clocks gated mid-transfer the sustained 4KB media read stalls while a small
  // INQUIRY finishes first. The doorbell-stuck that originally motivated FORCE_HCS=0 was really the
  // clock-period (cycle 51) + PRDT/FMP (cycle 78) bugs, now fixed. Restore 0xDE0 so the clocks un-gate
  // on demand for the READ. HWACG stays disabled (ACG_DISABLE=1, matches working acg=1).
  //
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_FORCE_HCS, HCI_FORCE_HCS_ONDEMAND);
  MmioOr32    ((UINTN)UFS_VS_BASE + HCI_UFS_ACG_DISABLE, 0x1u);
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  //
  // (0b) CONFIGURE INTERRUPT AGGREGATION (UTRIACR @ standard HCI base + 0x4C) — the
  // ROOT-CAUSE FIX for "transfer completes (OCS=0, response written) but the UTRLDBR
  // doorbell never clears under polling". The Exynos controller USES interrupt
  // aggregation (Linux ufs-exynos.c sets hba->caps |= UFSHCD_CAP_INTR_AGGR); the
  // kernel's ufshcd_init runs ufshcd_config_intr_aggr(nutrs-1, INT_AGGR_DEF_TO) which
  // writes UTRIACR = INT_AGGR_ENABLE(b31) | INT_AGGR_PARAM_WRITE(b24) | THLD(nutrs-1) |
  // TIMEOUT(1). Completion retirement on this controller is coupled to the aggregation
  // block 'firing' (counter threshold or the timeout); generic EDK2 UfsPassThru NEVER
  // touches 0x4C, so after the HCE reset the block is left in a state where completed
  // transfers never retire to the doorbell when EDK2 polls (it also never enables IE or
  // services interrupts). NUTRS = (CAP & 0x1F)+1 = 16 (CAP=0x1303FF0F) -> THLD=15=0xF,
  // TIMEOUT=1 (~40us). Value 0x81000F01 == ufshcd_config_intr_aggr(15, 1). This is a
  // STANDARD HCI register (not IS/0x20), so the write is safe like UTRLBA/RSR.
  //
  MmioWrite32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRIACR_OFF, UFS_HC_UTRIACR_CFG);
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  //
  // (0c) ENABLE the transfer-completion interrupt (IE @ 0x24 bit0). On this Exynos
  // controller the doorbell never retires under EDK2 polling even though the transfer
  // completes (OCS=0, response written) and IS.UTRCS gets set — the completion FSM is
  // gated on IE.UTP_TRANSFER_REQ_COMPL, which the Linux driver enables but generic
  // EDK2 (poll-based) never does. IE is a plain mask register (no W1C side effects),
  // so this write is safe; EDK2 never touches 0x24 so it persists through all transfers.
  //
  MmioOr32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_IE_OFF, UFS_HC_IE_CFG);
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  //
  // (1) Un-gate the UniPro + reference clocks NOW: clear CLK_STOP_ALL (REFCLKOUT b4,
  // REFCLK b2, UNIPRO_MCLK b1, UNIPRO_PCLK b0 = 0x17). Leaves MPHY_APBCLK_STOP (b3)
  // for the PMA helper. (Was: cleared only bit4, leaving UNIPRO/REFCLK gated.)
  //
  Clk = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL);
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL, Clk & ~HCI_CLK_STOP_ALL);

  //
  // (2) *** CYCLE A53 REMOVED: CPort "connection setup" MMIO writes to reg_hci 0x110/0x114. *** The
  // comprehensive kernel re-audit found the working LineageOS driver NEVER writes reg_hci 0x110/0x114 -
  // those offsets are UNDOCUMENTED in the kernel's reg_hci map (they sit between SMU_ABORT_MATCH_INFO @0x10C
  // and DBR_DUPLICATION_INFO @0x120). The real Samsung/DWC bring-up arms the UniPro CPort via DME UIC
  // attributes (T_CPORTFLAGS=0x6 / T_CPORTMODE=1, ufshcd-dwc.c), NOT MMIO. Our standard DME_LINKSTARTUP
  // already arms the CPort (UPIUs ARE delivered - the device enumerates + INQUIRY/READ-CAP work), so these
  // MMIO writes were extraneous AND poked 0x1/0x22 into undocumented registers adjacent to the SMU/DBR
  // block - a plausible corruptor of the media DMA/SMU path (the >=512B data-write SBFES). Removed.
  //

  //
  // (3) Data-path setup (faithful to fork exynos_ufs_init_host). The bit31 enables
  // here are the gap that explains why a NOP retires but the first data-bearing query
  // does not: TXPRDT gets PRDT_PREFECT_EN, and AXIDMA gets WLU_EN (AXI-DMA write-LU
  // enable) so the controller can complete the DMA write-back of a transfer's response/
  // data and then auto-retire its UTRLDBR doorbell bit. (We previously wrote bare 0xC /
  // 0xF, missing both bit31s.) Also the interrupt-aggregation tick (V2P1 IA_TICK_SEL +
  // 1us_to_cnt = mclk MHz) and IOP HWACG-disable, as the fork does.
  //
  //
  // *** CYCLE 78: PRDT entry size = 12 DWORDs (48B) - THE FMP FIX, proven by the working-kernel register
  // dump (WOADUMP via instrumented Linux: HCI_VS TXPRDT=0x8000000C / RXPRDT=0x0000000C). The Exynos
  // controller's FMP engine strides 48B/entry on MEDIA (LBA) reads. Cycle 58 instead shrank the controller
  // to 16B to "match" EDK2's 16B entry, but that STILL silently swallowed READ(10) (resp=0, no data),
  // because size=4 does NOT disable the FMP path - it just makes the engine read a malformed 16B entry.
  // The real fix (what the working driver does): make EDK2 build 48B entries (UTP_TR_PRD is now 12 DWORDs,
  // the 8 FMP DWORDs zeroed = crypto bypass), tell the controller 48B here, AND scale the byte-gran PRDT
  // length by 48 (UTP_TR_PRD_SIZE). All three are now consistent at 48B. INQUIRY (no FMP) unaffected.
  //
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_TXPRDT_ENTRY_SIZE,
               HCI_PRDT_PREFECT_EN | HCI_PRDT_SET_SIZE12);  // 0x8000000C
  //
  // *** CYCLE A78: RX PRDT PREFETCH OFF, to MATCH the working kernel. Ground-truth WOAXFER dump (working
  // debug-kernel 4KB READ(10), tools/kmod/woaxfer-kernel-working-read.txt) shows the kernel writes
  // txprdt=0x8000000C (prefetch ON) but rxprdt=0x0000000C (prefetch OFF) - ASYMMETRIC. Cycle A31 had set
  // RX prefetch ON (0x8000000C), DIVERGING from the kernel; with the FMP/DESCTYPE-3 128-byte PRDT entry the
  // upfront RX prefetch faults the media DATA-IN (cold READ data never lands, OCS 0x0F). Restore the
  // kernel's exact rxprdt = 0x0000000C (no prefetch, 12-DWORD size). INQUIRY (no FMP) was unaffected either way.
  //
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_RXPRDT_ENTRY_SIZE,
               HCI_PRDT_SET_SIZE12);                          // A78: 0x0000000C (no prefetch) = MATCH working-kernel rxprdt
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_DATA_REORDER,      0x0000000Au);
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_AXIDMA_BURST_LEN,
               HCI_WLU_EN | HCI_BURST_LEN3);                 // A100: restore kernel BURST_LEN(3)=0x98000003 - READ(6) data write was NEVER tested with this (A88 ran on Type-A READ10)

  //
  // *** CYCLE 74: IMMEDIATE readback of the data-path vendor regs (BEFORE the device reset). *** The
  // existing D[8] readback (taken AFTER the device-reset GPIO pulse) shows DATA_REORDER=0x16, but we
  // WRITE 0xA. If these immediate reads come back 0xA / 0x98000003 / 0x80000004 / 0x4 (i.e. they STICK at
  // write time), then the device reset / link startup is REVERTING our vendor data-path config -> streaming
  // DMA left unconfigured = the broken-drain root (next: re-write after the reset). If they are ALREADY
  // wrong here, the writes are being rejected/protected (RKP-style) = a deeper wall.
  //
  UfsHcDiagSet (43, MmioRead32 ((UINTN)UFS_VS_BASE + HCI_DATA_REORDER));       // rordI (expect 0xA)
  UfsHcDiagSet (44, MmioRead32 ((UINTN)UFS_VS_BASE + HCI_AXIDMA_BURST_LEN));   // axI   (A88: expect 0x88000001)
  UfsHcDiagSet (45, MmioRead32 ((UINTN)UFS_VS_BASE + HCI_TXPRDT_ENTRY_SIZE));  // txI   (expect 0x8000000C)
  UfsHcDiagSet (46, MmioRead32 ((UINTN)UFS_VS_BASE + HCI_RXPRDT_ENTRY_SIZE));  // rxI   (expect 0xC)
  MmioOr32    ((UINTN)UFS_VS_BASE + HCI_V2P1_CTRL, HCI_IA_TICK_SEL);
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_1US_TO_CNT_VAL, HCI_1US_CNT);
  //
  // *** CYCLE A35: FORCE the IOP clock ON through transfers (SET HCI_IOP_ACG_DISABLE bit0 =
  // disable IOP auto-clock-gating), mirroring how the HWACG-disable write above forces HWACG off.
  // Every prior cycle CLEARED this bit (IOP auto-gating ENABLED) - the ONE clock knob that diverged
  // from the working kernel (which never writes 0x100, leaving the IOP clock un-gated) and that the
  // "clock-gating both states" sweeps never isolated (it was constant-cleared every cycle). The
  // cold-read SBFES is a host-side AXI fatal that fires ONLY when the device pauses for the NAND
  // fetch (warm reads have no idle gap and never fault); an IOP clock that auto-gates during that
  // idle and mis-resumes the data-DMA on wake is a clean mechanism for it. Forcing the clock on is
  // strictly safe (more power, no functional risk) and a clean binary test: cold read works =>
  // IOP-gating was the cause; still wedges => IOP-gating ruled out, advance to polling->interrupt.
  //
  // *** CYCLE A35 REVERTED in A42: ground-truth UFSREGDUMP from the WORKING kernel shows iopacg=0
  // (HCI_IOP_ACG_DISABLE_EN CLEARED => IOP auto-clock-gating ENABLED) and it reads every LBA fine, so
  // forcing the IOP clock on was an unneeded divergence. Match the kernel: CLEAR bit0 (kernel does
  // exactly `reg & ~HCI_IOP_ACG_DISABLE_EN`).
  MmioAnd32   ((UINTN)UFS_VS_BASE + HCI_IOP_ACG_DISABLE, ~0x1u);

  //
  // (3b REVERTED: DFES error-enable (0x7C/0x80/0x84) + MPHY_REFCLK_SEL (0x108).
  // Adding these regressed the link on device \u2014 DevPresent 1->0, HCS 0xF->0x8, no
  // transfers at all. The fork applies them during a from-scratch re-link
  // (link_startup_notify); we applied them to sboot's already-trained LIVE link in
  // PostHce, which tore it down. Switching the MPHY refclk source and/or arming
  // device-fatal-error-stop on a live link breaks it. Not used here.)
  //

  //
  // (4) Mark all transfer/task-management list slots as SCSI nexus (all tags).
  //
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_UTRL_NEXUS_TYPE,  0xFFFFFFFFu);
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_UTMRL_NEXUS_TYPE, 0xFFFFFFFFu);

  __asm__ __volatile__ ("dsb sy" ::: "memory");

  //
  // (5) *** Hardware-reset the UFS device via the vendor GPIO (CONFIRMED REQUIRED, cycle 55). ***
  // assert (0), settle, deassert (1), then let it re-init before UfsPassThru issues DME_LINKSTARTUP.
  // Matches Linux exynos_ufs_dev_hw_reset / ufs_device_reset. CYCLE 55 REMOVED this pulse and the link
  // DIED (DevPresent=0, UEC PA=0x80000010 line-reset, never detected): without a device reset the
  // device stays at sboot's HS-G3 while the controller re-links at PWM-G1 -> mismatch -> PA line-reset.
  // So the device MUST be reset to drop to its PWM-G1 power-on default for link startup. (The reset is
  // therefore NOT the cause of the media-read silence; that is chased via the HS-PMC bisect below.)
  //
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_GPIO_OUT, 0x00000000u);
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  gBS->Stall (5000);                                   // 5 ms asserted
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_GPIO_OUT, 0x00000001u);
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  gBS->Stall (10000);                                  // 10 ms to recover

  //
  // DIAG: [8] = DATA_REORDER readback (expect 0xA); [9] = GPIO_OUT readback
  // (expect 0x1 = device reset deasserted) — confirms the VS writes land.
  //
  UfsHcDiagSet (8, MmioRead32 ((UINTN)UFS_VS_BASE + HCI_DATA_REORDER));
  UfsHcDiagSet (9, MmioRead32 ((UINTN)UFS_VS_BASE + HCI_GPIO_OUT));
}

//
// PRE-LINK UNIPRO + MPHY CALIBRATION — authoritative exynos7_ufs_pre_link port
// (mainline drivers/ufs/host/ufs-exynos.c; the 9810 binds "samsung,exynos7-ufs").
// The link trains at boot config but the FIRST data burst line-resets
// (UECPA=0x80000010) because HCE wiped the UniPro/MPHY init that sboot applied.
// We re-apply it here in PreLinkStartup (after HCE, before DME_LINKSTARTUP), in
// mainline order:
//   (1) config_unipro : PA_DBG_CLK_PERIOD = DIV_ROUND_UP(1e9, mclk) (~7) + opt-suite1.
//   (2) exynos7 pre_link : OV_TM-gated MPHY attrs (0x297/0x362/0x363) then the
//       PA_DBG line-reset handshake (TXPHY_CFGUPDT, SKIP_RESET_PHY, SKIP_LINE_RESET,
//       LINE_RESET_REQ) bracketed by the mandatory Stall(1)/Stall(1600).
// KEY FIX vs. the prior attempt: the MPHY attribute writes are silently dropped
// unless PA_DBG_OV_TM (0x9540) is set to 1 first. Every offset/value below is
// verified against mainline; opt_suite1_val = 0x30103 is exynos7_uic_attr.
//
#define UFS_UIC_DME_GET       0x01u
#define UFS_UIC_DME_SET       0x02u
#define UFS_UIC_DME_PEER_SET  0x04u
#define UFS_UIC_DME_PEER_GET  0x05u
//
// Exynos 9810 init_cfg attribute IDs + sel encodings (Samsung universal9810
// ufs-cal-9810.c). The whole digital head is DME_SET; the PCS attrs use lane
// selectors RX_LANE_0=4 / TX_LANE_0=0; the three period attrs take the mclk
// period = 1e9 / mclk. CYCLE 51: this is now only the FALLBACK - PreLinkSetup
// READS the device's own PA_DBG_CLK_PERIOD (the period sboot programmed for the
// UFS clock we INHERIT, and sboot's UFS works) and uses that. The old hardcoded
// 6 assumed Linux's 166.5 MHz, but the device reports 10 (~100 MHz); telling the
// PCS 6 when the clock is ~100 MHz is a ~40% timing error that only corrupts
// SUSTAINED reads (short metadata bursts survive). 10 = the proven sboot value.
//
#define UFS_MCLK_PRD          10u   // UNVERIFIED on 9820: proven sboot mclk period on the 9810; only the fallback, the device's own PA_DBG_CLK_PERIOD is read first
#define PCS_RX_LANE0          4u
#define PCS_TX_LANE0          0u
#define PA_DBG_CLK_PERIOD     0x9514u    // UNIPRO_DBG_PRD (mclk period)
#define PCS_COMN              0x0200u    // PHY_PCS_COMN config gate (0x40 open / 0x00 close)
#define PCS_RX_PRD            0x0012u    // PHY_PCS_RX_PRD (per RX lane)
#define PCS_TX_PRD            0x00AAu    // PHY_PCS_TX_PRD (per TX lane)
#define UNIPRO_DBG_AUTOMODE   0x9536u
#define PA_DBG_OPTION_SUITE   0x9564u
#define BEYOND0LTE_UFS_OPT_SUITE  0x2E820183u  // UNVERIFIED on 9820: 9810 DME PA_DBG_OPTION_SUITE value, never re-read here

//
// Minimal DME_GET/DME_SET helper (keeps the calibration compact for the FV
// budget). Returns the attribute value (valid after DME_GET).
//
STATIC
UINT32
Beyond0LteDme (
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *DrvIf,
  IN UINT8                           Opcode,
  IN UINT32                          Mib,
  IN UINT32                          Val
  )
{
  EDKII_UIC_COMMAND  Cmd;

  ZeroMem (&Cmd, sizeof (Cmd));
  Cmd.Opcode = Opcode;
  Cmd.Arg1   = Mib;
  Cmd.Arg3   = Val;
  DrvIf->UfsExecUicCommand (DrvIf, &Cmd);
  return Cmd.Arg3;
}

//
// ANALOG M-PHY (PMA) CALIBRATION — the genuinely missing piece. The DME/UniPro
// digital cal above reaches LinkRdy=1, but the first real data burst still
// line-resets (UECPA code 0x10) because the analog M-PHY front-end (PLL/bias/CDR)
// is never tuned. Generic UfsPassThru does no PMA writes, and sboot's PMA cal is
// gone after the HCE re-enable. We port the exynos9810 ufs_cal_pre_link init_cfg
// analog tail (Samsung universal9810 ufs-cal-9810.c) verbatim.
//
// CRITICAL: the real PMA base is 0x11124000 (the ufs-phy DT node), NOT 0x11130000
// (which is the FMP protector) — the platform header had these mislabeled.
// Per Linux phy_pma_writel, each PMA access must un-gate the MPHY APB clock
// (clear MPHY_APBCLK_STOP in HCI_CLKSTOP_CTRL @ VS+0xB0) and restore it after,
// or the write does not stick. PHY_PMA_COMN regs are lane-independent (written
// once); PHY_PMA_TRSV regs are per-lane with stride 0x140.
//
#define UFS_PMA_BASE          EXYNOS_UFS_PMA_BASE
#define UFS_MPHY_APBCLK_STOP  (1u << 3)   // HCI_CLKSTOP_CTRL bit3
#define UFS_PMA_TRSV_STRIDE   0x140u

//
// UNIPRO debug-APB block (DT "ufs" reg #2 = 0x11110000, size 0x8000). The cal's
// UNIPRO_DBG_APB rows are direct byte-offset MMIO here (ref unipro_writel =
// writel(val, reg_unipro + addr); plain, no clock ungating unlike the PMA path).
//
#define UFS_UNIPRO_BASE       EXYNOS_UFS_UNIPRO_BASE

//
// HS data-lane count for the power-mode change. CYCLE 28 ran this at 1 (single
// lane + lane 1 squelched) and it line-reset IDENTICALLY to x2 (gear=03030101,
// uPA=80000010) -> LANE COUNT EXONERATED, and with it the analog front-end (the
// failure is gear- AND lane-independent). 2 = the reference/target dual-lane
// config for CYCLE 29 (which adds the missing hardware DL flow-control timers).
// Set back to 1 to re-enter the single-lane path (lane1_sq_off) if ever needed.
//
#define UFS_HS_LANES          2u

//
// CYCLE 34 ISOLATION KNOB: 1 = do the HS power-mode change (the normal path); 0 = SKIP it and run
// all I/O at the PWM-G1 link-startup default. After exonerating gear/lane/TActivate/DL-timers and
// proving the transfer is structurally perfect yet NO data flows on the LBA read, this bisects
// HS/PMC-specific (PWM read works) vs mode-independent (PWM read also line-resets). The PWM PMA cal
// (calib_pwm in Beyond0LtePmaInit) + DL-timer DME sets (PreLinkSetup) remain in effect either way.
//
// CYCLE 35: PMC re-ENABLED (=1, back to HS-G3 - the target mode). Cycle 34 proved the failure is
// mode-independent (PWM line-reset too), and an exhaustive ref diff found NO missing/wrong init,
// cal, vendor-HCI, clock, timer, nexus or PRDT register. So this cycle is DIAGNOSTIC: it stops the
// stall buffer-read from clobbering D[11]/D[12] so the PMC's PA_TActivate calib (D[11], step 2c)
// and setst (D[12], step 6) finally render - revealing whether cycle 31's "TActivate didn't help"
// used SANE values (HwCap sane) or garbage (HwCap=0 => DME_GET 0x8F failed => never truly tested).
//
// *** CYCLE 56: PMC DISABLED (=0) - RE-TEST at PWM-G1 WITH the cycle-51 clock-period fix in place. ***
// The ONLY difference between the working reads (INQUIRY/READ-CAP/TUR = SHORT single-frame DATA-IN) and
// the silent READ(10) (4KB = LONG multi-frame DATA-IN) is the HS sustained burst. Cycle 34's PWM test
// was BEFORE the clock fix, so its "PWM line-resets too" was the clock bug (now fixed) - this re-test is
// genuinely new. The PRD/clock fix applies at PWM too (set in PreLinkSetup, gear-independent). If the
// 4KB READ now COMPLETES at PWM-G1 => HS sustained multi-frame burst was the wall (and we likely BOOT,
// slowly); if still SILENT => mode-independent => device-media-hold (next: bActiveICCLevel + SSU active
// + media-ready wait via an EDK2 UfsPassThru patch). setst (D[12]) will show ~0x22 (Slow/PWM) not 0x11.
//
// *** CYCLE 56 RESULT + CYCLE 57: PMC RE-ENABLED (=1, HS). *** Cycle 56 at PWM gave partitions=0 AND a
// PA LINE-RESET (uPA=0x80000010) on the read (HS was SILENT instead) => the media read fails at BOTH
// modes = MODE-INDEPENDENT, HS-burst REFUTED. Both failure modes fit a NAND-ACCESS STALL-RESUME: a real
// media read makes the device fetch from NAND, the M-PHY STALLs during that idle, and on resume the host
// RX can't re-acquire. The fix (applied below after the AFC lock) is the pre_h8_exit RX-resume re-cal.
// HS is the cleaner baseline (control works, only the media read fails silently), so run at HS.
//
#define UFS_DO_PMC            1u

//
// CYCLE A2: do ONE explicit hibern8 enter+exit after the PMC/AFC-lock so the M-PHY re-adapts its RX into
// the kernel's steady-state (pre_h8_exit) cal. Everything static matches the working kernel yet the first
// sustained partition read PA-line-resets; the kernel only reaches that RX state by transitioning THROUGH a
// real hibern8 exit (a static poke of the same register values was proven inert, cy57). 0 = skip (A1 path).
//
#define UFS_DO_H8_SETTLE      1u

STATIC
VOID
Beyond0LtePmaWr (
  IN UINT32  Off,
  IN UINT32  Val
  )
{
  UINT32  Clk;

  Clk = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL);
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL, Clk & ~UFS_MPHY_APBCLK_STOP);
  MmioWrite32 ((UINTN)UFS_PMA_BASE + Off, Val);
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL, Clk | UFS_MPHY_APBCLK_STOP);
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

#if UFS_DO_PMC
//
// PMA read helper - only referenced by the HS AFC-lock wait (step 5 of the power-mode change),
// so compile it in only when the PMC path is built (else -Wunused-function -Werror).
//
STATIC
UINT32
Beyond0LtePmaRd (
  IN UINT32  Off
  )
{
  UINT32  Clk;
  UINT32  Val;

  Clk = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL);
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL, Clk & ~UFS_MPHY_APBCLK_STOP);
  Val = MmioRead32 ((UINTN)UFS_PMA_BASE + Off);
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL, Clk | UFS_MPHY_APBCLK_STOP);
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  return Val;
}

//
// CYCLE A9: append text to the persistent-RAM console at 0xFED14000 (Linux ramoops
// persistent_ram_buffer: sig 'DBGC' = 0x43474244 @0, start @4, size @8, data @12,
// cap 0x3F00 - the SAME buffer the BdsLib BDS_DPUT breadcrumb uses). After a warm
// reboot into TWRP this surfaces as /sys/fs/pstore/pmsg-ramoops-0, so the PMADUMP is
// read with `tools/diag-cycle.ps1 -Read` - no camera or serial jig needed.
//
#if BEYOND0LTE_UFS_DIAG
STATIC
VOID
Beyond0LtePramByte (
  IN UINT8  Ch
  )
{
  volatile UINT32  *Pw = (volatile UINT32 *)(UINTN)0xFED14000ULL;
  volatile UINT8   *Pb = (volatile UINT8  *)(UINTN)0xFED14000ULL;
  UINT32           Sz;

  if (Pw[0] != 0x43474244u) {
    Pw[0] = 0x43474244u;
    Pw[1] = 0;
    Pw[2] = 0;
  }
  Sz = Pw[2];
  if (Sz < 0x3F00u) {
    Pb[12u + Sz] = Ch;
    Pw[2] = Sz + 1u;
  }
}

STATIC
VOID
Beyond0LtePramStr (
  IN CONST CHAR8  *Str
  )
{
  while (*Str != '\0') {
    Beyond0LtePramByte ((UINT8)*Str++);
  }
}

STATIC
VOID
Beyond0LtePramHex8 (
  IN UINT8  Val8
  )
{
  CONST CHAR8  *Hex = "0123456789abcdef";

  Beyond0LtePramByte ((UINT8)Hex[(Val8 >> 4) & 0xFu]);
  Beyond0LtePramByte ((UINT8)Hex[Val8 & 0xFu]);
}
#else
STATIC
VOID
Beyond0LtePramByte (
  IN UINT8  Ch
  )
{
  (VOID)Ch;
}

STATIC
VOID
Beyond0LtePramStr (
  IN CONST CHAR8  *Str
  )
{
  (VOID)Str;
}

STATIC
VOID
Beyond0LtePramHex8 (
  IN UINT8  Val8
  )
{
  (VOID)Val8;
}
#endif
#endif

STATIC
VOID
Beyond0LtePmaInit (
  VOID
  )
{
  UINT32  Lane;
  UINT32  T;

  //
  // exynos9810 init_cfg analog PMA tail (ufs-cal-9810.c). CRITICAL ORDERING: the
  // kernel's ufs_cal_config_uic applies the table ROW-MAJOR — each PHY_PMA_COMN
  // row is written ONCE (lane-independent), each PHY_PMA_TRSV row is written to
  // ALL active lanes, and only AFTER the entire table is the 0x8C PLL-cal latch
  // (0xC0 -> 0x00) + 200us settle performed. The earlier lane-major loop latched
  // the PLL and waited AFTER lane 0 but BEFORE lane 1's TRSV writes, so lane 1's
  // analog front-end was programmed after the PLL had already settled and never
  // re-settled -> lane 1 ran with a marginal eye. A 2-lane sustained device->host
  // burst (512B+ block read) then lost signal integrity on lane 1 -> generic PA
  // line-reset (UECPA=0x80000010), while the tiny INQUIRY/READ-CAP (which still
  // produced BlockIo=1) were short enough to squeak through. Match the reference.
  //

  // PHY_PMA_COMN head — PLL/bias enable (written once, lane-independent).
  Beyond0LtePmaWr (0x8C, 0x80);
  Beyond0LtePmaWr (0x74, 0x10);

  //
  // PHY_PMA_TRSV rows applied to BOTH lanes (stride 0x140) BEFORE the latch. The
  // first seven are the init_cfg data-rate-INDEPENDENT defaults; the last five are
  // the calib_of_pwm RECEIVER tuning (equalizer/CDR/slicer) for PWM link startup.
  // They are transient: the HS cal in PostLinkStartup overwrites
  // 0xC8/0xF0/0x120/0x128/0x134 on both lanes before the power-mode change.
  //
  for (Lane = 0; Lane < 2; Lane++) {
    T = Lane * UFS_PMA_TRSV_STRIDE;

    Beyond0LtePmaWr (0x110 + T, 0xB5);
    Beyond0LtePmaWr (0x134 + T, 0x43);
    Beyond0LtePmaWr (0x16C + T, 0x20);
    Beyond0LtePmaWr (0x178 + T, 0xC0);
    Beyond0LtePmaWr (0x0E0 + T, 0x12);
    Beyond0LtePmaWr (0x164 + T, 0x58);
    Beyond0LtePmaWr (0x1B0 + T, 0x18);

    Beyond0LtePmaWr (0x0C8 + T, 0x40);
    Beyond0LtePmaWr (0x0F0 + T, 0x77);
    Beyond0LtePmaWr (0x120 + T, 0x80);
    Beyond0LtePmaWr (0x128 + T, 0x00);
    Beyond0LtePmaWr (0x12C + T, 0x00);
  }

  // PHY_PMA_COMN tail — PLL cal strobe + release (once), THEN the init_cfg
  // COMMON_WAIT 0xC8 = udelay(200) settle, only after every lane is programmed.
  Beyond0LtePmaWr (0x8C, 0xC0);
  Beyond0LtePmaWr (0x8C, 0x00);
  gBS->Stall (200);
}

STATIC
VOID
Beyond0LtePreLinkSetup (
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *DrvIf
  )
{
  UINT32  Lane;
  UINT32  ClkPrd;

  if ((DrvIf == NULL) || (DrvIf->UfsExecUicCommand == NULL)) {
    return;
  }

  //
  // *** CYCLE A26: arm the DFES (Device Fatal Error Stop) error-enable for the UniPro DL/N/T layers
  // HERE (PreLinkStartup = the kernel's link_startup_notify PRE timing, before DME_LINKSTARTUP) -
  // NOT at PostHce on the already-live link (cycle 3b did that and tore the link down). The DL value
  // 0x80002020 enables RX_BUF_OF (RX buffer overflow, b5): the prime suspect for the cold-NAND-read
  // failure - the device's delayed data overruns the host RX, and with DFES OFF it does not latch in
  // UECDL (=0, as observed) but escalates to a host SYSTEM_BUS_FATAL. The working kernel arms these
  // (exynos_ufs_config_intr) and reads fine. We omit the MPHY_REFCLK_SEL switch (the live-link killer).
  //
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_ERROR_EN_DL_LAYER, HCI_DFES_DL_VAL);
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_ERROR_EN_N_LAYER,  HCI_DFES_N_VAL);
  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_ERROR_EN_T_LAYER,  HCI_DFES_T_VAL);
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  //
  // *** CYCLE 51 CLOCK-PERIOD FIX (the strongest lead found in the whole effort). *** The mclk
  // period (ns) the PCS/UniPro symbol+timeout logic needs = 1e9 / mclk_rate, and we INHERIT
  // sboot's UFS clock (we never touch the CMU). sboot left its own period in PA_DBG_CLK_PERIOD,
  // and sboot's UFS WORKS (it loads the boot image from UFS), so that value is the PROVEN-correct
  // period for the clock we run on. The old code hardcoded 6 (1e9/166.5MHz, Linux's value) and
  // OVERWROTE sboot's value - but the device reports 10 (~100 MHz). Telling the PCS the period is
  // 6 when the real clock is ~100 MHz is a ~40% timing error: short metadata bursts survive, but a
  // SUSTAINED 4 KB read drifts / trips a too-short timeout window -> generic PA line-reset. This
  // matches the exact symptom (INQUIRY/READ-CAP ok, LBA-0 media read line-resets). Read the
  // device's own period and program THAT into all three PRD attrs (0x9514, 0x12 RX, 0xAA TX).
  //
  ClkPrd = Beyond0LteDme (DrvIf, UFS_UIC_DME_GET, PA_DBG_CLK_PERIOD << 16, 0) & 0xFFu;
  if ((ClkPrd < 4u) || (ClkPrd > 16u)) {
    ClkPrd = UFS_MCLK_PRD;   // sanity fallback (10 = proven sboot value)
  }

  //
  //
  // *** CYCLE 78: REVERT cycle 68's 1US_TO_CNT change. *** Cycle 68 set 1US_TO_CNT = 1000/ClkPrd = 100,
  // assuming the HCI mclk equals the PCS/UniPro clock (PA_DBG_CLK_PERIOD = 10 ns). The working-kernel
  // register dump (WOADUMP via instrumented Linux) proves they are DIFFERENT clocks: HCI 1US_TO_CNT =
  // 0xA6 (166 => mclk ~166 MHz) even though the PCS period is 10 ns. So the hardcoded 0xA6 (already
  // written in the vendor setup) is correct; do NOT derive it from ClkPrd. The cycle-51 PCS clock-period
  // fix (PRD attrs 0x9514/0x12/0xAA from ClkPrd) is SEPARATE and stays. Re-assert 0xA6 here defensively.
  //
  {
    UINT32  UsCnt = HCI_1US_CNT;   // 0xA6 = 166, per working-kernel WOADUMP (NOT 1000/ClkPrd)

    MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_1US_TO_CNT_VAL, UsCnt & 0x3FFu);

    // DIAG[10] (panel "CLKPRD"): [31:24]=cycle tag 0xA2; [15:8]=1US_TO_CNT; [7:0]=mclk period. Panel
    // "use=A200A60A" = cycle-A2. Everything static matches the working kernel; A2 forces ONE real hibern8
    // enter+exit after the PMC so the M-PHY re-adapts its RX into the kernel's steady-state (pre_h8_exit)
    // cal - the only thing the kernel does at runtime that we don't. WIN = uPA=0 / FAT mounts / FileSystems>=1.
    // Panel h8e/h8x = hibern8 enter/exit IS status (0x40 UHES / 0x20 UHXS / 0x10 UPMS seen); hsS = Rate (2=B).
    // *** A4 = A2/A3 behaviour UNCHANGED; summary now shows Wcmd/resp/OCS to PIN the read failure. A3 PROVED:
    // hibern8 enter+exit COMPLETE (h8e=..40/h8x=..20), PA line-reset GONE, uDL=uTR=0, yet reads fail + list
    // HALTS (HCS UTRLRDY=0). resp=0 => device SILENT on media read (device-side); resp!=0 => sense/reject. ***
    // A5: + panel hcs0 = HCS UTRLRDY at PartReadProbe START -> locate WHERE the list halts (enum vs probe).
    // A6: BOOST host PA_TActivate +0x10 (RX re-activate margin for the high-latency read resume). tact2 = readback.
    // *** A7: WORKING-KERNEL PMA-DUMP MATCH. *** Booted my own instrumented LineageOS 4.9.218 kernel on-device;
    // it READ the disk fine (mounted FAT sda18 + EXT4 sda3/19/20/21/22) and dumped the M-PHY RX regs to
    // /proc/last_kmsg. The working READ state rests at COMN 0x004=0x00 and per-lane TRSV 0x0F0=0x7F (0x0C4=0xD9 /
    // 0x0E8=0x77 match). Our pre_h8_exit left 0x004=0x3F / 0x0F0=0xFF; 0x0F0 bit7=1 FREEZES the CDR so the RX
    // cannot re-adapt across a NAND read's idle-then-burst (control reads work, sustained media reads fail). A7
    // restores the kernel's clean resting state after the h8 settle + the line-reset recovery. WIN=FileSystems>=1.
    // *** A8 REFUTED, A9 = RAM READBACK. *** A7/A8 PHY resting-state theory wrong (reads still silent). A9 mirrors
    // the live PMA RX dump into the persistent-RAM console (0xFED14000 -> pmsg-ramoops-0) so the FULL dump is read
    // from TWRP via `tools/diag-cycle.ps1 -Read` (no camera/jig) and diffed vs the working-kernel PMADUMP. The A8
    // A10 (tag 0xAA): PHY EXONERATED (A9 PMADUMP == working kernel byte-for-byte). Refocus on transport: the BdsLib
    // LBA-sweep -> pram maps WHERE the read wedges. Auto-reboot-to-recovery RE-ENABLED with the PROVEN m1_power.c
    // scheme (PMU INFORM3 0x1406080C = SEC reason 0x12345674 recovery, NOT A8's wrong SYSIP_DAT0/0xF). No PHY change.
    // A11 (tag 0xAB): A10 proved the failure is a HOST UTP LIST-HALT (UTRLRDY=0, every LBA fails). A11 mirrors the
    // stuck-read state (D[] diag) to pram to ID which fatal bit halts the list, then restart it like the kernel.
    // A12 (tag 0xAC): A11 showed NO fatal error / device just SILENT. A12 tries the kernel UTRL restart (RUN_STOP
    // reg 0x60/0x70) + retry in BdsLib to see if un-wedging the list lets a read through (non-determinism => retry).
    // A13 (tag 0xAD): A12 PROVED IT - clear fatal IS (SBFES 0x20000) + restart UTRLRSR resumes the halted list and
    // the read SUCCEEDS. A13 grafts that recovery into UfsPassThruDxe UfsExecScsiCmds (retry 8x on doorbell timeout)
    // so it applies to the real boot reads (ScsiDisk mounts + bootmgfw). EXPECT FileSystems>=1 / boot.
    // A14 (tag 0xAE): A13's re-wait of the aborted slot was insufficient; A14 RE-ISSUES a fresh transfer (stop slot +
    // reset OCS + re-ring) after the recovery, matching the A12 fresh-command that works. WIN sweep reads = OK.
    // A15 (tag 0xAF): A14 still left UTRLRDY=0 after UTRLRSR=1 - missing the SETTLE A12 had (gBS->Stall 2ms). A15
    // adds MicroSecondDelay(3000) after the restart so the HC clears the halt before re-issuing. WIN = boot/FS>=1.
    // A16 (tag 0xB0): A13-A15 (re-ring SAME descriptor) all re-fault. A16 recovers the HC + RECURSIVELY re-runs
    // UfsExecScsiCmds so a FRESH descriptor is built (the only thing A12 proved works), depth-limited 8x. WIN=boot.
    // A17 (tag 0xB1): A16 FIXED reads (sweep all OK, real data). A17 counts SBFES recoveries (D[81]) to gauge how
    // often the fault fires (informs whether a 382MB boot.wim read is fast enough or needs the DMA-root fix).
    // A28 (tag 0xB3): A27's full reset cleared the SBFES + re-linked (A22b hcs=0x0f) but at PWM-G1 (UPMCRS=0),
    // where media reads PA-line-reset (fs=00). A28 re-invokes EdkiiUfsHcPostLinkStartup after UfsControllerInit
    // so the device returns to HS-G3 before the retry. WIN = fs>=03 restored + readOk climbs past the first cold read.
    // A29 (tag 0xB4): A28 returned to HS (A22b hcs=0x10f) but STILL fs=00 - the full re-init mid-enum breaks the
    // ScsiDisk stack + the cold retry stays cold (device cache cleared). RECOVERY IS A DEAD END. Reverted the
    // recovery to the A21 list-reset (fs=03 baseline). The cold-read SBFES must be PREVENTED, not recovered.
    // A31 (tag 0xB4): RX-PRDT prefetch is a hardware no-op (wedge rxprdt stays 0x0C). A32 (tag 0xB5): IO-coherency
    // OFF (non-coherent DMA) - the idle coherent CCI master may mishandle the delayed cold DATA-IN; wedge iocoh->0x011.
    UfsHcDiagSet (10, 0xB5000000u | ((UsCnt & 0xFFu) << 8) | (ClkPrd & 0xFFu));
  }
  Beyond0LteCrumb ("CLKPRD");

  //
  // Exynos 9810 ufs_cal_pre_link init_cfg DIGITAL head (verbatim from Samsung
  // universal9810 ufs-cal-9810.c). This SoC's PCS programming is ENTIRELY
  // different from exynos7's OV_TM/PA_DBG dance (which we had wrongly applied).
  // Everything here is a UIC DME_SET (ufs_lld_dme_set -> ufshcd_dme_set). The
  // two PCS_COMN(0x200) writes bracket the per-lane PCS attribute programming;
  // PCS attrs select the lane via RX_LANE_0=4 / TX_LANE_0=0 in the MIB selector.
  //
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (PA_DBG_CLK_PERIOD << 16),                 ClkPrd);        // UNIPRO_DBG_PRD (device-measured mclk period)
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (PCS_COMN         << 16),                 0x40);          // PHY_PCS_COMN open
  for (Lane = 0; Lane < 2; Lane++) {
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (PCS_RX_PRD << 16) | (PCS_RX_LANE0 + Lane), ClkPrd);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (PCS_TX_PRD << 16) | (PCS_TX_LANE0 + Lane), ClkPrd);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x5Cu      << 16) | (PCS_RX_LANE0 + Lane), 0x38);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x0Fu      << 16) | (PCS_RX_LANE0 + Lane), 0x00);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x65u      << 16) | (PCS_RX_LANE0 + Lane), 0x01);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x69u      << 16) | (PCS_RX_LANE0 + Lane), 0x01);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x21u      << 16) | (PCS_RX_LANE0 + Lane), 0x00);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x22u      << 16) | (PCS_RX_LANE0 + Lane), 0x00);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x84u      << 16) | (PCS_RX_LANE0 + Lane), 0x01);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x04u      << 16) | (PCS_TX_LANE0 + Lane), 0x01);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x8Fu      << 16) | (PCS_TX_LANE0 + Lane), 0x3E);
  }
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (PCS_COMN            << 16),               0x00);          // PHY_PCS_COMN close
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (UNIPRO_DBG_AUTOMODE << 16),               0x4E20);        // UNIPRO_DBG_MIB
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (PA_DBG_OPTION_SUITE << 16),               BEYOND0LTE_UFS_OPT_SUITE);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x155Eu             << 16),               0x00);          // UNIPRO_STD_MIB
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x3000u             << 16),               0x00);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x3001u             << 16),               0x01);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x4021u             << 16),               0x01);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x4020u             << 16),               0x01);

  //
  // init_cfg ANALOG tail (PMA writes to 0x11124000) + udelay(200).
  //
  Beyond0LtePmaInit ();
  UfsHcDiagOr (13, UFS_PH_PMA);
  Beyond0LteCrumb ("PMA-DONE");

  //
  // post_init_cfg (ufs_cal_post_link, ufs-cal-9810.c). Samsung runs this AFTER
  // DME_LINKSTARTUP but BEFORE the first NOP. EDK2's PostLinkStartup callback
  // fires only AFTER the NOP (which is what's hanging), so we fold it into
  // pre-link. The PHY_PCS_RX RX-tuning (0x35/0x73/0x41/0x42, sel RX_LANE_0+lane)
  // is what lets the host RECEIVE the device's NOP-IN response; without it the
  // transfer doorbell never clears (our exact symptom). 0x15A4 = PA_SaveConfigTime
  // (bracketed by the 0x9529 dbg-mode enable/disable).
  //
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x9529u  << 16),                     0x01);  // dbg-mode on
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x15A4u  << 16),                     0xFA);  // PA_SaveConfigTime
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x9529u  << 16),                     0x00);  // dbg-mode off
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (PCS_COMN << 16),                     0x40);  // PHY_PCS_COMN open
  for (Lane = 0; Lane < 2; Lane++) {
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x35u << 16) | (PCS_RX_LANE0 + Lane), 0x05);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x73u << 16) | (PCS_RX_LANE0 + Lane), 0x01);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x41u << 16) | (PCS_RX_LANE0 + Lane), 0x02);
    Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x42u << 16) | (PCS_RX_LANE0 + Lane), 0xAC);
  }
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (PCS_COMN << 16),                     0x00);  // PHY_PCS_COMN close

  //
  // (lane1_sq_off REMOVED: it disabled host lane #1's squelch, valid only for 1-lane operation.
  // We now drive a 2-lane HS power-mode change in PostLinkStartup, so lane #1 must stay active.)
  //

  //
  // calib_of_pwm UniPro DL/PA timing (ufs-cal-9810.c; identical values in calib_of_hs => MODE-
  // INDEPENDENT, spec-recommended). We never set these, so the data-link layer runs on power-on
  // default flow-control timers. A short INQUIRY(36B)/READ-CAP(8B) finishes before they expire,
  // but a sustained device->host block READ (512B+) outlasts them -> the DL layer times out and
  // RESETS THE LINE, which surfaces as the generic PA error (UECPA=0x10) that halts the UTP list.
  // 0x2041 DL_FC0ProtectionTimeOutVal, 0x2042 DL_TC0ReplayTimeOutVal, 0x2043 DL_AFC0ReqTimeOutVal;
  // 0x15B0-2 PA_PWRModeUserData0-2 (carry the same to the device on a PMC; harmless without one).
  //
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x2041u << 16), 8064);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x2042u << 16), 28224);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x2043u << 16), 20160);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x15B0u << 16), 12000);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x15B1u << 16), 32000);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, (0x15B2u << 16), 16000);

  // DIAG[11]: UNIPRO_DBG_PRD readback (expect 6). DIAG[12]: opt-suite (expect 0x2E820183).
  UfsHcDiagSet (11, Beyond0LteDme (DrvIf, UFS_UIC_DME_GET, PA_DBG_CLK_PERIOD   << 16, 0));
  UfsHcDiagSet (12, Beyond0LteDme (DrvIf, UFS_UIC_DME_GET, PA_DBG_OPTION_SUITE << 16, 0));
}

//
// CYCLE 44: kernel-style LINERESET recovery state. The Linux UFS error handler treats
// uPA=0x80000010 (UFSHCD_UIC_PA_GENERIC_ERROR = "the LINERESET indication") as NON-FATAL: it
// restores the HS power mode (re-runs the PMC) and retries, with no host reset. EDK2 does none of
// this. mSavedDrvIf = the driver interface captured at PostLinkStartup (gives the doorbell hook
// access to DME/UIC commands for the re-PMC); mInRecovery guards re-entrancy while the recovery
// issues UIC commands; mRecoverCount = bounded count of restores performed; mRePmcResult = the last
// restore's PA_PWRMode/UPMS/AFC status (for the panel).
//
#if UFS_DO_PMC
STATIC EDKII_UFS_HC_DRIVER_INTERFACE  *mSavedDrvIf   = NULL;   // only the HS-PMC recovery path uses it
#endif
STATIC BOOLEAN                         mInRecovery   = FALSE;
STATIC UINT32                          mRecoverCount = 0;
STATIC UINT32                          mRePmcResult  = 0;
STATIC UINT32                          mReissueMask  = 0;
STATIC UINT32                          mReissueDoneMask = 0;

/**
  UFS HC platform callback. UfsPassThru invokes this at defined init phases.
  - EdkiiUfsHcPreHce (before HCE enable): vendor LINK+UNIPRO software reset so the
    controller's native doorbell auto-clear works (reference exynos_ufs_host_reset).
  - EdkiiUfsHcPostHce (after HCE enable): program the Exynos vendor HCI registers
    (refclk-out, CPort, DATA_REORDER, device reset) and attempt the UFSP bypass.
  - EdkiiUfsHcPreLinkStartup (just before DME_LINKSTARTUP): re-apply the UniPro
    clock period (PA_DBG_CLK_PERIOD) that the HCE reset wiped — the likely fix
    for the first-transfer PA line-reset (UECPA=0x80000010).
**/
EFI_STATUS
EFIAPI
Beyond0LteUfsHcCallback (
  IN     EFI_HANDLE                            ControllerHandle,
  IN     EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                                  *CallbackData
  )
{
  switch (CallbackPhase) {
    case EdkiiUfsHcPreHce:
      //
      // A114 (REVERTED): moving Beyond0LteUfspConfig() here (pre-HCE, kernel .init timing) did NOT fix the
      // read-class fault (DESCTYPE-3 set pre-HCE, smur=0xEFFA6492, still SBFES) AND regressed iocoh (the FSYS1
      // sysreg write does not stick this early in my sequence -> ioco read back 0x11 instead of 0x311). So the
      // pre-vs-post-HCE ORDERING is exonerated; iocoh must be written post-HCE to stick. Back to PostHce (A112).
      //
      //
      // *** A116 (option B): snapshot S-Boot's UFS handoff state at our VERY FIRST entry, BEFORE Beyond0LteHostReset /
      // HCE tears it down. S-Boot read the boot image FROM UFS, so the controller + FMP are in a WORKING state right
      // now. If HCE=1 + UTRLBA!=0 + UTRLRSR=1 + HCS shows link-up, S-Boot left a usable controller we could INHERIT
      // (minimal-init) instead of full-reset. Read-only std HCI (base 0x11120000) + UFSP RSECURITY. Panel sb* fields.
      //
#if BEYOND0LTE_UFS_DIAG
      {
        volatile UINT32  *Dh = (volatile UINT32 *)(UINTN)0x00000000FED13000ULL;
        Dh[307] = MmioRead32 ((EXYNOS_UFS_HCI_BASE + 0x34ULL));   // HCE     (1 = S-Boot left controller enabled)
        Dh[308] = MmioRead32 ((EXYNOS_UFS_HCI_BASE + 0x30ULL));   // HCS     (DP/UTRLRDY/UCRDY/UPMCRS = link+device state)
        Dh[309] = MmioRead32 ((EXYNOS_UFS_HCI_BASE + 0x50ULL));   // UTRLBA  (!=0 = S-Boot set up a transfer request list)
        Dh[317] = MmioRead32 ((EXYNOS_UFS_HCI_BASE + 0x60ULL));   // UTRLRSR (1 = list running)
        Dh[318] = MmioRead32 ((EXYNOS_UFS_UFSP_BASE + UFSP_RSECURITY));   // UFSP RSECURITY (S-Boot FMP DESCTYPE: EFFA6492=3, EFE26492=0)
      }
#endif
      #if BEYOND0LTE_SKIP_PREHCE_SW_RST
      UfsHcDiagOr (13, UFS_PH_PREHCE);
      UfsHcDiagSet (319, 0xA1170001u);                  // A117: skipped vendor LINK+UNIPRO SW_RST at PreHce
      #else
        UfsHcDiagSet (319, 0xA1180000u);                  // A118: normal PreHce vendor reset restored
        Beyond0LteHostReset ();
      #endif
      Beyond0LteCrumb ("PREHCE");
      break;

    case EdkiiUfsHcPostHce:
      UfsHcDiagOr (13, UFS_PH_POSTHCE);
      Beyond0LteHciVendorSetup ();
      Beyond0LteUfspConfig ();
      //
      // (UFS interrupt registration DISABLED: the IS-ack-from-ISR approach is dead
      // \u2014 clearing IS.UTRCS hangs even in interrupt context, and an enabled+unacked
      // level IRQ storms ~36M times and starves EDK2's polling loop. We instead make
      // the controller AUTO-RETIRE the doorbell via the init_host data-path fixes
      // (WLU_EN / PRDT_PREFECT_EN above), the kernel's actual mechanism, needing no
      // interrupt at all. Beyond0LteRegisterUfsInterrupt is retained but not called.)
      //
      UfsHcDiagOr (13, UFS_PH_VENDOR);
      Beyond0LteCrumb ("POSTHCE");
      break;

    case EdkiiUfsHcPreLinkStartup:
      UfsHcDiagOr (13, UFS_PH_PRELINK);
      Beyond0LtePreLinkSetup ((EDKII_UFS_HC_DRIVER_INTERFACE *)CallbackData);
      UfsHcDiagOr (13, UFS_PH_PRELINKDONE);
      Beyond0LteCrumb ("PRELINK");
      break;

    case EdkiiUfsHcPostLinkStartup:
    {
      //
      // *** HS POWER-MODE CHANGE (the real fix). *** EDK2 invokes this AFTER device-init but
      // BEFORE any SCSI block read (UfsPassThru.c:973) - exactly where the kernel switches from
      // the PWM-G1 link-startup default to High-Speed. PWM-G1 sustained reads are NOT viable on
      // this HW (a 512B+ burst always raises a generic PA line-reset that halts the UTP list);
      // full calib_of_pwm did not help. The kernel runs all I/O at HS, so we replicate its PMC.
      // Target (CYCLE 28) = HS-G3 x1 Rate A: gear G3 is the kernel's I/O mode and is exonerated
      // (G1 x2 and G3 x2 both line-reset); x1 narrows to a single lane to bisect lane-skew /
      // lane-1 defects from a deeper cause. Sequence = ufs_cal_pre_pmc(calib_of_hs_rate_a)
      // -> set PA attrs -> DME_SET PA_PWRMode=Fast/Fast (trigger) -> wait IS.UPMS -> post AFC.
      //
      EDKII_UFS_HC_DRIVER_INTERFACE  *PostDrvIf = (EDKII_UFS_HC_DRIVER_INTERFACE *)CallbackData;
#if UFS_DO_PMC
      UINT32  HsLane;
      UINT32  HsT;
      UINT32  Upms;
      UINT32  Spin;
      UINT32  PwrNew;
      UINT32  HcsNew;
      UINT32  AfcLock = 0;

      //
      // CYCLE 44: save the driver interface so the doorbell hook's LINERESET recovery can later
      // issue DME/UIC commands to restore the HS power mode after a generic PA line-reset.
      //
      mSavedDrvIf = PostDrvIf;

      //
      // (1) pre_pmc HS MPHY analog tuning (calib_of_hs_rate_a, Gear 3 = the kernel's I/O mode),
      // applied to the UFS_HS_LANES active lane(s). PMD_HS: 0xC8=0xBC,0xF0=0x7F,0x120=0xC0;
      // PMD_HS_G3_L2: 0x128=0x00,0x134=0x63. Overwrites the PWM values from init_cfg.
      //
      // *** CYCLE 28 SINGLE-LANE (x1) BISECTION ***: the untested variable is the LANE COUNT.
      // UFS_HS_LANES=1 tunes only lane 0 and squelches lane 1 (1b), removing lane-to-lane
      // de-skew and any lane-1 analog defect. A x1 burst is ~2x LONGER, so: x1 WORKS => the
      // 2-lane path (skew / lane-1 cal) was the killer; x1 STILL line-resets (despite the
      // longer burst) => NOT lane-related -> the analog front-end is exonerated and the cause
      // is elsewhere (data path / protocol / a deeper PHY-state event).
      //
      for (HsLane = 0; HsLane < UFS_HS_LANES; HsLane++) {
        HsT = HsLane * UFS_PMA_TRSV_STRIDE;
        Beyond0LtePmaWr (0x0C8 + HsT, 0xBC);
        Beyond0LtePmaWr (0x0F0 + HsT, 0x7F);
        Beyond0LtePmaWr (0x120 + HsT, 0xC0);
        Beyond0LtePmaWr (0x128 + HsT, 0x00);   // G3
        Beyond0LtePmaWr (0x134 + HsT, 0x63);   // G3
      }
      Beyond0LteCrumb ("HS-PMA");

      //
      // (1b) With lane 1 inactive, turn OFF its squelch/power (ufs_cal_pre_pmc lane1_sq_off,
      // applied by the kernel when available=2 & target=1): PHY_PMA_TRSV lane1 0x0C4=0x19,
      // 0x0E8=0xFF (offsets +0x140 = lane 1).
      //
#if (UFS_HS_LANES < 2)
      Beyond0LtePmaWr (0x0C4 + UFS_PMA_TRSV_STRIDE, 0x19);
      Beyond0LtePmaWr (0x0E8 + UFS_PMA_TRSV_STRIDE, 0xFF);
#endif

      //
      // (2) Re-assert the DL/PA flow-control timers + PWRModeUserData (consumed by the PMC), in
      // case link startup reset them, then program the target mode's PA attributes.
      //
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x2041u << 16, 8064);
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x2042u << 16, 28224);
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x2043u << 16, 20160);
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x15B0u << 16, 12000);
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x15B1u << 16, 32000);
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x15B2u << 16, 16000);

      //
      // (2b) *** CYCLE 29: the missing HARDWARE DL flow-control + PA timers. *** calib_of_hs_rate_a
      // writes these SAME six values a SECOND time as UNIPRO_DBG_APB rows (ref unipro_writel ->
      // direct MMIO at reg_unipro+addr), NOT just as the DME attrs above. On Exynos the DME_SET
      // configures the UniPro stack's ATTRIBUTE (capability/negotiation) while the APB write sets
      // the ACTUAL hardware timers. We only did the DME_SET, so the hardware DL flow-control ran at
      // power-on defaults: a short INQUIRY/READ-CAP finishes before they matter, but a sustained
      // device->host block read outlasts a mis-set DL_FC0Protection/AFC0Req window or overruns the
      // RX buffer -> the link RESETS (generic PA error uPA=0x80000010). With analog now EXONERATED
      // (gear- & lane-independent), this hardware-timer gap is the leading non-PHY cause. 0x7888/
      // 0x788C/0x7890 = DL FC0/TC0/AFC0 timeouts; 0x78B8/0x78BC/0x78C0 = PA_PWRModeUserData0-2;
      // mirrors the 0x2041-3 / 0x15B0-2 DME sets above. (reg_unipro = 0x11110000, plain MMIO.)
      //
      MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x7888u, 8064);
      MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x788Cu, 28224);
      MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x7890u, 20160);
      MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x78B8u, 12000);
      MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x78BCu, 32000);
      MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x78C0u, 16000);

      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x1560u << 16, UFS_HS_LANES); // PA_ActiveTxDataLanes
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x1580u << 16, UFS_HS_LANES); // PA_ActiveRxDataLanes
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x1569u << 16, 1);     // PA_TxTermination (HS)
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x1584u << 16, 1);     // PA_RxTermination (HS)
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x156Au << 16, 2);     // PA_HSSeries = Rate B (cyA0: LIVE kernel dmesg shows HS-series(2)=Rate B; we ran Rate A=1 => the sustained-burst line-reset)
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x1568u << 16, 3);     // PA_TxGear = 3
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x1583u << 16, 3);     // PA_RxGear = 3

      //
      // (2c) *** CYCLE 31: PA_TActivate STALL-resume margin. *** A READ(10) makes the device do a
      // NAND-access idle before it streams the 4KB; the HS M-PHY enters STALL during that idle and
      // the host RX must re-activate on burst resume. Cycle-26 measured local PA_TActivate=2 which is
      // BELOW the HW minimum cap (hw_cap=3) -> the RX activate window is shorter than the HW even
      // supports -> the resume burst misses the data -> generic PA line-reset (fits metadata-ok /
      // LBA-data-fail: INQUIRY/READ-CAP are device-generated with no NAND idle, so they never STALL).
      // Set host PA_TActivate >= hw_cap and the DEVICE's higher (vendor recommendation: activate-time
      // host < device), and bump host PA_Hibern8Time, BEFORE the PMC trigger so the power-mode change
      // applies them to the HS link. (Supersedes the old post-PMC 5b block whose tact>=hw_cap
      // condition never fired = a no-op. Cycle-30 proved the transfer is structurally perfect, so the
      // failure is this physical STALL-resume, not the descriptor.)
      //
      {
        //
        // CYCLE 41 FIX: match the Samsung reference (ufs_cal_calib_hibern8_values) EXACTLY. The OLD
        // code OVERWROTE the host's PA_TActivate with HwCap+1 (the hardware MINIMUM), which can be
        // LOWER than the value link-training negotiated. That advertises to the device that our RX
        // can resume a burst after only HwCap+1 - but after the device's long NAND-access idle our RX
        // needs its full negotiated activate time, so the resume burst is MISSED -> PA line-reset. The
        // reference LEAVES the host PA_TActivate at its negotiated value and only raises the DEVICE's
        // to (host_negotiated + 1) (vendor rule activate host < device), plus raises host Hibern8Time.
        //
        UINT32  HwCap    = Beyond0LteDme (PostDrvIf, UFS_UIC_DME_GET, (0x8Fu << 16) | PCS_RX_LANE0, 0) & 0xFFu;
        UINT32  HostTact = Beyond0LteDme (PostDrvIf, UFS_UIC_DME_GET, 0x15A8u << 16, 0) & 0xFFu;  // negotiated host PA_TActivate
        UINT32  Hib8     = Beyond0LteDme (PostDrvIf, UFS_UIC_DME_GET, 0x15A7u << 16, 0) & 0xFFu;
        //
        // *** CYCLE A72: REVERT the A6 +0x10 boost. The working-kernel woaall dump shows PA_TActivate
        // host=2 / device=3 (LOW, negotiated). My A6 boost to (max(neg,hw_min)+0x10 ~0x12) DIVERGES from
        // the kernel and may MIS-TIME the device's burst RESUME after the NAND-fetch idle relative to the
        // host RX activate window -> the HS silence. Match ufs_cal_calib_hibern8_values EXACTLY: LEAVE host
        // PA_TActivate at its negotiated value, raise ONLY the device to host+1 (vendor rule host < device),
        // and host Hibern8Time + 1.
        //
        Beyond0LteDme (PostDrvIf, UFS_UIC_DME_PEER_SET, 0x15A8u << 16, HostTact + 1);   // device PA_TActivate = host + 1 (kernel 3)
        Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET,      0x15A7u << 16, Hib8 + 1);       // host PA_Hibern8Time + 1
#if BEYOND0LTE_UFS_DIAG
        *(volatile UINT32 *)(UINTN)0xFED13434u =
          (Beyond0LteDme (PostDrvIf, UFS_UIC_DME_GET, 0x15A8u << 16, 0) & 0xFFu) | (HostTact << 8) | (HwCap << 16);
#endif

        // diag[11] (panel "tact"): [23:16]=hw_cap, [15:8]=device TActivate(=host+1), [7:0]=host TActivate (negotiated).
        UfsHcDiagSet (11, (HwCap << 16) | ((HostTact + 1u) << 8) | HostTact);
      }

      //
      // (3) Trigger the power-mode change: PA_PWRMode = (RX<<4)|TX = Fast(1)/Fast(1) = 0x11.
      //
      Beyond0LteCrumb ("PMC-GO");
      Beyond0LteDme (PostDrvIf, UFS_UIC_DME_SET, 0x1571u << 16, 0x11);

      //
      // (4) Wait for the power-mode-change completion (IS bit4 UPMS), bounded ~1s. IS is
      // read-only-safe here (only WRITES wedge); we never clear it.
      //
      Upms = 0;
      for (Spin = 0; Spin < 100000; Spin++) {
        Upms = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x20u) & 0x10u;
        if (Upms != 0) {
          break;
        }
        gBS->Stall (10);
      }

      //
      // (5) post_calib_of_hs_rate_a CDR-AFC LOCK WAIT (ufs_cal_wait_cdr_afc_check): POLL TRSV
      // 0x1fc (per lane) until bit 0x40 (AFC/CDR locked) is set; while not yet locked, poke 0xF0
      // (0x7F then 0xFF) to re-trigger AFC and retry (<=100 x ~41us). My earlier bare WRITE of
      // 0x1fc=0x40 skipped this, so the HS clock-data recovery was never confirmed locked and a
      // sustained read drifted -> generic PA line-reset. AfcLock bit per lane surfaced in setst.
      //
      for (HsLane = 0; HsLane < UFS_HS_LANES; HsLane++) {
        HsT = HsLane * UFS_PMA_TRSV_STRIDE;
        for (Spin = 0; Spin < 100; Spin++) {
          gBS->Stall (40);
          if ((Beyond0LtePmaRd (0x1FC + HsT) & 0x40u) == 0x40u) {
            AfcLock |= (1u << HsLane);
            break;
          }
          gBS->Stall (1);
          Beyond0LtePmaWr (0x0F0 + HsT, 0x7F);
          Beyond0LtePmaWr (0x0F0 + HsT, 0xFF);
        }
      }
      Beyond0LteCrumb ("AFC-LOCK");

#if UFS_DO_H8_SETTLE
      //
      // *** CYCLE A2: explicit HIBERN8 enter+exit "RX settle". *** The link is idle here (EDK2 has not yet
      // started its NOP/reads), so we transition the M-PHY through ONE real hibern8 cycle - applying the
      // reference post_h8_enter cal (while in H8) then pre_h8_exit cal (before exit) - and return to ACTIVE.
      // EDK2 never sees H8, but the RX has re-run its adaptation exactly as the working kernel's first
      // auto-hibern8 exit does (the static poke at cy57 was inert because the PHY only re-adapts THROUGH the
      // h8-exit state machine). Best-effort with timeouts: a stuck transition can never hang the boot, and
      // the existing LINERESET recovery is the safety net. IS bits: UHES=0x40 (enter), UHXS=0x20 (exit),
      // UPMS=0x10. Diag -> D[264] (panel h8e) = enter IS|spins, D[265] (panel h8x) = exit IS|spins.
      //
      {
        UINT32  H8Is;
        UINT32  H8Spin;
        UINT32  H8Lane;
        UINT32  H8T;

        // (a) HIBERN8 ENTER (UIC 0x17): Beyond0LteDme issues it + waits UCCS; then poll IS for UHES|UPMS.
        Beyond0LteDme (PostDrvIf, 0x17u, 0, 0);
        H8Is = 0;
        for (H8Spin = 0; H8Spin < 60000; H8Spin++) {
          H8Is = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x20u);
          if ((H8Is & 0x50u) != 0u) {
            break;
          }
          gBS->Stall (10);
        }
#if BEYOND0LTE_UFS_DIAG
        *(volatile UINT32 *)(UINTN)0xFED13420u = (H8Is & 0xFFu) | (H8Spin << 8);
#endif

        // (b) post_h8_enter cal (while IN H8): per-active-lane TRSV 0x0C4=0x99 / 0x0E8=0x7F / 0x0F0=0x7F + COMN 0x004=0x02.
        for (H8Lane = 0; H8Lane < UFS_HS_LANES; H8Lane++) {
          H8T = H8Lane * UFS_PMA_TRSV_STRIDE;
          Beyond0LtePmaWr (0x0C4 + H8T, 0x99);
          Beyond0LtePmaWr (0x0E8 + H8T, 0x7F);
          Beyond0LtePmaWr (0x0F0 + H8T, 0x7F);
        }
        Beyond0LtePmaWr (0x004, 0x02);

        // (c) pre_h8_exit cal (still IN H8, before issuing exit): COMN 0x004=0x3F, TRSV 0x0C4=0xD9 / 0x0E8=0x77, settle, 0x0F0=0xFF.
        Beyond0LtePmaWr (0x004, 0x3F);
        for (H8Lane = 0; H8Lane < UFS_HS_LANES; H8Lane++) {
          H8T = H8Lane * UFS_PMA_TRSV_STRIDE;
          Beyond0LtePmaWr (0x0C4 + H8T, 0xD9);
          Beyond0LtePmaWr (0x0E8 + H8T, 0x77);
        }
        gBS->Stall (10);
        for (H8Lane = 0; H8Lane < UFS_HS_LANES; H8Lane++) {
          H8T = H8Lane * UFS_PMA_TRSV_STRIDE;
          Beyond0LtePmaWr (0x0F0 + H8T, 0xFF);
        }

        // (d) HIBERN8 EXIT (UIC 0x18): issue + wait UCCS; then poll IS for UHXS|UPMS. The RX re-adapts on exit.
        Beyond0LteDme (PostDrvIf, 0x18u, 0, 0);
        H8Is = 0;
        for (H8Spin = 0; H8Spin < 60000; H8Spin++) {
          H8Is = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x20u);
          if ((H8Is & 0x30u) != 0u) {
            break;
          }
          gBS->Stall (10);
        }
#if BEYOND0LTE_UFS_DIAG
        *(volatile UINT32 *)(UINTN)0xFED13424u = (H8Is & 0xFFu) | (H8Spin << 8);
#endif
      }
      Beyond0LteCrumb ("H8-SETTLE");
#endif

      //
      // *** CYCLE A7: restore the working kernel's CLEAN post-PMC RX RESTING state (captured from an on-device
      // debug-kernel PMA dump that READ the disk successfully): COMN 0x004=0x00 and per-active-lane TRSV 0x0F0=0x7F.
      // The h8 settle's pre_h8_exit leaves 0x004=0x3F / 0x0F0=0xFF, but 0x0F0 bit7=1 FREEZES the CDR. The kernel
      // reads with 0x0F0=0x7F (bit7=0) so the RX re-adapts across the idle-then-burst of a real NAND read. 0x0C4=0xD9
      // / 0x0E8=0x77 (set by the settle) already match the dump and are kept.
      //
      Beyond0LtePmaWr (0x004, 0x00);
      for (HsLane = 0; HsLane < UFS_HS_LANES; HsLane++) {
        Beyond0LtePmaWr (0x0F0 + HsLane * UFS_PMA_TRSV_STRIDE, 0x7F);
      }
      Beyond0LteCrumb ("RX-REST");

      //
      // *** CYCLE A8: live PMA RX dump. *** Read the same M-PHY RX regs the working-kernel pma_dump_regs()
      // captured and stream them to the serial console (DebugLib -> Exynos UART) for a direct diff, and pack the
      // resting regs into D[70]/D[71] for the panel (visible without a serial jig). State here = post-PMC, i.e.
      // exactly the working-kernel dump's capture point. Each Beyond0LtePmaRd re-ungates the MPHY APB clock.
      //
#if BEYOND0LTE_UFS_DIAG
      {
        STATIC CONST UINT16  PmaDumpRegs[] = {
          0x004, 0x010, 0x014, 0x018, 0x0C4, 0x0E8, 0x0F0,
          0x110, 0x120, 0x134, 0x164, 0x16C, 0x178, 0x1B0, 0x1FC
        };
        UINTN            DumpIdx;
        volatile UINT32  *Dpanel = (volatile UINT32 *)(UINTN)0xFED13000ULL;

        DEBUG ((DEBUG_ERROR, "BEYOND0LTE PMADUMP (post-PMC, cycle A9):\n"));
        Beyond0LtePramStr ("\n=PMADUMP A9 post-PMC=\n");
        for (DumpIdx = 0; DumpIdx < ARRAY_SIZE (PmaDumpRegs); DumpIdx++) {
          UINT8  L0 = (UINT8)(Beyond0LtePmaRd (PmaDumpRegs[DumpIdx]) & 0xFFu);
          UINT8  L1 = (UINT8)(Beyond0LtePmaRd (PmaDumpRegs[DumpIdx] + UFS_PMA_TRSV_STRIDE) & 0xFFu);

          DEBUG ((DEBUG_ERROR, "PMADUMP: reg=0x%03x l0=0x%02x l1=0x%02x\n", PmaDumpRegs[DumpIdx], L0, L1));

          // Mirror the same line to persistent RAM (pmsg-ramoops-0): "r=01fc l0=ff l1=ff".
          Beyond0LtePramStr ("r=");
          Beyond0LtePramHex8 ((UINT8)(PmaDumpRegs[DumpIdx] >> 8));
          Beyond0LtePramHex8 ((UINT8)PmaDumpRegs[DumpIdx]);
          Beyond0LtePramStr (" l0=");
          Beyond0LtePramHex8 (L0);
          Beyond0LtePramStr (" l1=");
          Beyond0LtePramHex8 (L1);
          Beyond0LtePramByte ((UINT8)'\n');
        }
        //
        // Panel pack (want rest ~ 00 7F D9 77): D[70]=[0x004 | 0x0F0_l0 | 0x0C4_l0 | 0x0E8_l0];
        // D[71]=[0x010_l0 | 0x010_l1 | 0x014_l0 | 0x014_l1] (the per-lane adapted CDR regs).
        //
        Dpanel[70] = ((Beyond0LtePmaRd (0x004) & 0xFFu) << 24) |
                     ((Beyond0LtePmaRd (0x0F0) & 0xFFu) << 16) |
                     ((Beyond0LtePmaRd (0x0C4) & 0xFFu) << 8)  |
                      (Beyond0LtePmaRd (0x0E8) & 0xFFu);
        Dpanel[71] = ((Beyond0LtePmaRd (0x010) & 0xFFu) << 24) |
                     ((Beyond0LtePmaRd (0x010 + UFS_PMA_TRSV_STRIDE) & 0xFFu) << 16) |
                     ((Beyond0LtePmaRd (0x014) & 0xFFu) << 8) |
                      (Beyond0LtePmaRd (0x014 + UFS_PMA_TRSV_STRIDE) & 0xFFu);
      }
#endif

      //
      // (CYCLE 57 pre_h8_exit RX-resume re-cal REMOVED: applying it statically here was proven INERT -
      // cycle 57 panel was byte-identical to cycle 54 (uPA=0, resp=0, READ silent). The media-read failure
      // is NOT the host RX resume state; cycle 58 pivots to the FMP/PRDT-entry-size path instead.)
      //

      //
      // (6) Result into diag[12] (panel "setst"): [31:24]=PA_PWRMode after PMC (0x11=Fast=HS
      // SUCCESS; 0x55=unchanged=PMC did not take); [23]=UPMS seen; [15:0]=HCS (UPMCRS=bits[10:8],
      // 1=PWR_LOCAL success; DP=bit0, UTRLRDY=bit1).
      //
      PwrNew = Beyond0LteDme (PostDrvIf, UFS_UIC_DME_GET, 0x1571u << 16, 0);
      HcsNew = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x30u);
      UfsHcDiagSet (
        12,
        ((PwrNew & 0xFFu) << 24) | ((Upms != 0) ? 0x00800000u : 0u) |
        ((AfcLock & 0x3u) << 16) | (HcsNew & 0xFFFFu)
        );
      //
      // *** CYCLE A1: read back the NEGOTIATED PA_HSSeries after the PMC -> D[262] (panel "hsS"). 2 = Rate B
      // (cyA0 requested B; if hsS != 2 the PMC fell back to Rate A => Rate B was never actually tested).
      //
#if BEYOND0LTE_UFS_DIAG
      *(volatile UINT32 *)(UINTN)0xFED13418u = Beyond0LteDme (PostDrvIf, UFS_UIC_DME_GET, 0x156Au << 16, 0);
#endif
#else
      //
      // CYCLE 34 ISOLATION (UFS_DO_PMC==0): the HS power-mode change above is COMPILED OUT - the
      // partition read runs at the PWM-G1 link-startup default. diag[12] (panel "setst") = live
      // PA_PWRMode<<24: expect 0x22xxxxxx (Slow/PWM) vs the HS 0x11. WIN (uPA=0 / partitions>=1 /
      // buf shows data) => the failure is HS/PMC-specific; STILL line-resets => mode-independent.
      //
      UfsHcDiagSet (12, (Beyond0LteDme (PostDrvIf, UFS_UIC_DME_GET, 0x1571u << 16, 0) & 0xFFu) << 24);
#endif
      //
      // *** CYCLE 75: RE-APPLY the Exynos vendor DATA-PATH config. *** Cycle 74 proved the device-reset
      // GPIO pulse in Beyond0LteHciVendorSetup REVERTS our writes (DATA_REORDER 0xA -> 0x16, immediate=0xA).
      // Re-write the data-path registers NOW - after the device reset, link startup AND the HS PMC, right
      // before UfsPassThru issues the first transfers - so the streaming RX->memory DMA runs with the
      // CORRECT config (the suspected broken-drain root). These are data-path (not PHY/link) regs, so they
      // do not disturb the trained HS link. D[8] (panel REORDER) is refreshed here: 0xA now (was 0x16) =>
      // the re-apply stuck. WIN = rd0=00000000 / partitions>=1 (the 4KB read finally completes).
      //
      MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_TXPRDT_ENTRY_SIZE, HCI_PRDT_PREFECT_EN | HCI_PRDT_SET_SIZE12);
      MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_RXPRDT_ENTRY_SIZE, HCI_PRDT_PREFECT_EN | HCI_PRDT_SET_SIZE12);  // A31: RX prefetch
      MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_DATA_REORDER,      0x0000000Au);
      MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_AXIDMA_BURST_LEN,  HCI_WLU_EN | HCI_BURST_LEN3);  // A100: restore kernel BURST_LEN(3)
      // *** CYCLE 90 / A25 REVERTED in A42 (ground-truth kernel match). *** The working LineageOS kernel
      // runs the ENTIRE read path with FORCE_HCS = 0xDE0 (full clock-on-demand) + CLKSTOP = 0x08 (MPHY_APB
      // auto-gated) and reads ALL LBAs fine - so forcing UNIPRO (cyc90 => 0xD80) and MPHY_APB (A25 => 0x980)
      // always-on was a WORKAROUND for the old IE=0x1: this controller's completion FSM is IE-gated, and with
      // only UTRCS armed the on-demand clock resume after the cold-fetch idle lost the completion, so I
      // pinned the clocks on. A41 now programs the kernel's FULL IE=0x30ef5, so the on-demand path should
      // self-complete. Stop rewriting FORCE_HCS on the read path (it stays 0xDE0 from PostHce, like the
      // kernel); still clear only the UNIPRO bits of CLKSTOP so it settles to the kernel's 0x08 (MPHY_APB b3
      // left GATED). The A25 MPHY_APB un-gate was a hardware no-op anyway (clkstop always re-read 0x08).
      {
        UINT32  ClkKeep = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL);
        MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL, ClkKeep & ~HCI_CLK_STOP_ALL);
      }
      __asm__ __volatile__ ("dsb sy" ::: "memory");
      __asm__ __volatile__ ("dsb sy" ::: "memory");
      //
      // *** CYCLE 93: DISABLE AUTO-HIBERNATE (AH8). *** Classic "first reads work, post-idle reads wedge"
      // cause: the link auto-hibernates after the back-to-back GPT reads (AHIT idle timer); the next data
      // read finds the link in H8 and the completion is lost (Device Error). EDK2 never touches AHIT
      // (UFSHCI std reg 0x18) so sboot's value persists. Capture it (D[16], panel ahitWas) then write 0.
      //
      UfsHcDiagSet (16, MmioRead32 ((EXYNOS_UFS_HCI_BASE + 0x18ULL)));   // AHIT before
      MmioWrite32 ((EXYNOS_UFS_HCI_BASE + 0x18ULL), 0x00000000u);        // disable auto-hibernate
      __asm__ __volatile__ ("dsb sy" ::: "memory");
      //
      // *** CYCLE 80: RE-OPEN the UFS PROTECTOR (SMU) right before the first SCSI READ. *** The SMU is
      // configured at PostHce, but - like the data-path vendor regs (cycle 74/75) - it may be reset by the
      // device-reset / link-startup before READ(10). D[46] (panel x46) = SCTRL0 captured BEFORE this
      // re-apply: 0xF1 => protector stayed OPEN since PostHce (=> SMU is NOT the block, pivot to UNIPRO/
      // transfer capture); 0x00 => it was RESET (=> this post-link re-open is the media-read fix). Then
      // re-open region 0 = whole device (SBEGIN0=0, SEND0=max), all LUNs (SLUN0=0xFF), allow (SCTRL0=0xF1).
      //
      UfsHcDiagSet (46, MmioRead32 ((UINTN)UFSP_BASE + UFSP_SCTRL0));
      MmioWrite32 ((UINTN)UFSP_BASE + UFSP_SBEGIN0, 0x00000000u);
      MmioWrite32 ((UINTN)UFSP_BASE + UFSP_SEND0,   0xFFFFFFFFu);
      MmioWrite32 ((UINTN)UFSP_BASE + UFSP_SLUN0,   0x000000FFu);
      MmioWrite32 ((UINTN)UFSP_BASE + UFSP_SCTRL0,  0x000000F1u);
      // *** A98: re-assert SKIPPED (DESCTYPE=0 root-cause test). Was: re-assert FMP DESCTYPE=3 via secure SMC.
      // Beyond0LteSmc (SMC_CMD_FMP_SECURITY, 0, SMU_EMBEDDED, CFG_DESCTYPE_3);
      __asm__ __volatile__ ("dsb sy" ::: "memory");
      UfsHcDiagSet (46, MmioRead32 ((UINTN)UFSP_BASE + UFSP_RSECURITY)); // x46 = RSECURITY at read time: EFFA6492 => DESCTYPE=3 set by SMC; EFE26492 => SMC did not take
      UfsHcDiagSet (8, MmioRead32 ((UINTN)UFS_VS_BASE + HCI_DATA_REORDER));
      //
      // *** CYCLE 94: DUMP + RE-OPEN ALL 8 SMU REGIONS. *** Low-LBA reads (GPT 0-5) work but high-LBA/
      // data reads HANG (OCS stuck 0xF, no UEC err) = textbook range-based protection. Region 0 is open
      // (cycle 80) but sboot may bound SEND0 to metadata only and set regions 1-7 to deny the rest.
      // Capture originals (SEND0, region-1 range/ctrl, OR of SCTRL1..7 -> D[17..21]) BEFORE widening,
      // then open EVERY region to whole-device-allow (SBEGIN=0, SEND=max, SLUN=0xFF, SCTRL=0xF1).
      //
      {
        UINT32  OrCtrl;
        UINTN   RgN;

        OrCtrl = 0;
        UfsHcDiagSet (17, MmioRead32 ((UINTN)UFSP_BASE + 0x204u));            // SEND0 (region0 end LBA)
        UfsHcDiagSet (18, MmioRead32 ((UINTN)UFSP_BASE + 0x210u));            // SBEGIN1
        UfsHcDiagSet (19, MmioRead32 ((UINTN)UFSP_BASE + 0x214u));            // SEND1
        UfsHcDiagSet (20, MmioRead32 ((UINTN)UFSP_BASE + 0x21Cu));            // SCTRL1
        for (RgN = 1; RgN < 8; RgN++) {
          OrCtrl |= MmioRead32 ((UINTN)UFSP_BASE + 0x20Cu + RgN * 0x10u);     // OR SCTRL1..7
        }
        UfsHcDiagSet (21, OrCtrl);
        for (RgN = 0; RgN < 8; RgN++) {
          MmioWrite32 ((UINTN)UFSP_BASE + 0x200u + RgN * 0x10u, 0x00000000u); // SBEGINn = 0
          MmioWrite32 ((UINTN)UFSP_BASE + 0x204u + RgN * 0x10u, 0xFFFFFFFFu); // SENDn   = max
          MmioWrite32 ((UINTN)UFSP_BASE + 0x208u + RgN * 0x10u, 0x000000FFu); // SLUNn   = all LUNs
          MmioWrite32 ((UINTN)UFSP_BASE + 0x20Cu + RgN * 0x10u, 0x000000F1u); // SCTRLn  = allow
        }
        __asm__ __volatile__ ("dsb sy" ::: "memory");
      }
      Beyond0LteCrumb ("POSTLINK");
      break;
    }

    default:
      break;
  }

  return EFI_SUCCESS;
}

//
// UFS HC PLATFORM PROTOCOL — kept so we can supply RefClkFreq and, if needed,
// Pre/PostLinkStartup vendor hooks later. On-device diagnosis showed sboot hands
// off with the UFS controller DISABLED (HCE=0): with SkipHceReenable=TRUE the
// HCE-ready wait timed out (~6M polls). So the controller MUST be enabled +
// linked by UfsPassThru — we do NOT skip. Both flags FALSE = let UfsPassThru run
// its normal enable + DME_LINKSTARTUP (which reaches device detection + ring
// alloc + the first transfer). The remaining failure (UECPA on the first UTP
// transfer) is being chased via DMA-buffer coherency first, then MPHY init.
//
STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL  mUfsHcPlatform = {
  EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION,   // Version (3)
  NULL,                                     // OverrideHcInfo (CAP/VER read fine)
  Beyond0LteUfsHcCallback,                    // Callback — UFSP bypass at PostHce
  EdkiiUfsCardRefClkFreq26Mhz,              // RefClkFreq CATEGORY: the 9810's 26 MHz; UNVERIFIED on 9820 (a legal enum value, not an address)
  FALSE,                                    // SkipHceReenable — controller needs enabling
  FALSE                                     // SkipLinkStartup — needs link startup
};

//
// DIAG: cross-module counter block at a fixed address in the mapped pram window
// (0xFED10000..0xFED20000 Device). PlatformBootManagerLib reads these to see how
// far UfsPassThru.Start() gets into our host controller before failing — the
// ConnectController EFI_NOT_FOUND swallows the real Start() error, so this is how
// we localize it. Layout (UINT32 each):
//   [0] GetMmioBar calls   [1] AllocateBuffer calls   [2] Map calls
//   [3] Read calls         [4] Write calls            [5] AllocBuf UC/WC/cached (0/1/2)
//   [6] HCS snapshot at first AllocateBuffer (link state when rings allocated)
//   [7] UFSPSCTRL0 readback after UFSP bypass (TZ-locked -> reads 0)
//   [8] HCI_DATA_REORDER readback after Exynos vendor setup (expect 0xA)
//   [9] HCI_GPIO_OUT readback after device reset deassert (expect 0x1)
//   [10] PA_DBG_CLK_PERIOD reset-default (DME_GET before our set)
//   [11] PA_DBG_CLK_PERIOD readback after DME_SET (expect 7)
//   [12] PA_DBG_CLK_PERIOD DME_SET status (0 = accepted)
//
#if BEYOND0LTE_UFS_DIAG
#define UFSHC_DIAG_BASE  0x00000000FED13000ULL

STATIC
VOID
UfsHcDiagBump (
  IN UINTN  Index
  )
{
  volatile UINT32  *D = (volatile UINT32 *)(UINTN)UFSHC_DIAG_BASE;
  D[Index] = D[Index] + 1u;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
UfsHcDiagSet (
  IN UINTN   Index,
  IN UINT32  Value
  )
{
  volatile UINT32  *D = (volatile UINT32 *)(UINTN)UFSHC_DIAG_BASE;
  D[Index] = Value;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
UfsHcDiagOr (
  IN UINTN   Index,
  IN UINT32  Bits
  )
{
  volatile UINT32  *D = (volatile UINT32 *)(UINTN)UFSHC_DIAG_BASE;
  D[Index] = D[Index] | Bits;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}
#else
STATIC
VOID
UfsHcDiagBump (
  IN UINTN  Index
  )
{
  (VOID)Index;
}

STATIC
VOID
UfsHcDiagSet (
  IN UINTN   Index,
  IN UINT32  Value
  )
{
  (VOID)Index;
  (VOID)Value;
}

STATIC
VOID
UfsHcDiagOr (
  IN UINTN   Index,
  IN UINT32  Bits
  )
{
  (VOID)Index;
  (VOID)Bits;
}
#endif

//
// ============================================================================
// FRAMEBUFFER BREADCRUMB HUD (self-contained; survives a hang).
//
// The diag block (0xFED13000) is only rendered by the BDS panel, which runs
// AFTER ConnectAllControllers returns. If UFS enumeration HANGS inside EDK2's
// UfsPassThru (e.g. wedged after a doorbell event), BDS is never reached and the
// screen shows nothing about how far we got. This HUD paints text straight to the
// command-mode panel scanout (0xCC000000, 1440x2960 BGRX) and pushes ONE frame
// via the DECON SW-trigger (0x16030000) - the exact mechanism the working
// first-light uses. Because the panel is COMMAND-MODE, the last pushed frame
// stays latched on the glass through a CPU hang, so the final breadcrumb reveals
// the last checkpoint reached. Called only at BOUNDED checkpoints (platform
// callbacks, each transfer doorbell ring, each observed doorbell retire) - NEVER
// in the per-poll path - so the ~35 ms present cost is paid a few dozen times.
// ============================================================================
//
#ifndef BEYOND0LTE_UFS_SCREEN_HUD
#define BEYOND0LTE_UFS_SCREEN_HUD 0
#endif

#if BEYOND0LTE_UFS_DIAG
#define BEYOND0LTE_STAGE_FNPTR     0x00000000FED13100ULL
#endif
#define BEYOND0LTE_STAGE_UFS_READ  0xA2700000u
#define BEYOND0LTE_STAGE_FAT_READ  0xA2710000u
#define BEYOND0LTE_STAGE_BS_CALL   0xA2720000u

#if BEYOND0LTE_UFS_SCREEN_HUD
#define HUD_FB_BASE       EXYNOS_FB_BASE       // UNVERIFIED on 9820 (EXYNOS_FB_BASE is 0 until read); block compiled out by BEYOND0LTE_UFS_SCREEN_HUD 0
#define HUD_FB_WIDTH      1440u
#define HUD_FB_MARGIN     96u
#define HUD_FG            0xFF33FF66u   // opaque green (stands out from console white)
#define HUD_BG            0xFF000000u   // opaque black
#define HUD_SCALE         2u
#define HUD_GLYPH_W       8u
#define HUD_GLYPH_H       16u
// CYCLE 8B (display-only): was 1500u ("mid-screen"), where the green RING-CMD HUD
// overdrew the BDS storage report's des0/x46/g48 + gMsk/gLBA/gNE/geC/gcC/gSt GPT-diag
// lines, making the decisive GPT-failure fields unreadable on the panel. Moved into
// the TOP black margin (above the report's first visible "UFS HC" line), so the HUD
// no longer covers any report text. Low Y is always in framebuffer bounds (HudFbLine
// is unbounded); going below the report would risk an out-of-bounds FB write if the
// panel is shorter than 2960. No UFS/GPT behavior changes.
#define HUD_ROW0          300u          // first HUD line Y (top margin, above the report)
#define HUD_LINEH         36u           // line pitch in pixels (glyph 16*scale=32 + gap)

#define HUD_DECON_BASE    EXYNOS_DECON_F_BASE  // 9820 decon_f window; block compiled out by BEYOND0LTE_UFS_SCREEN_HUD 0
#define HUD_DECON_SHADOW  0x0060u
#define HUD_DECON_TRIG    0x0070u
#define HUD_SHADOW_GLOBAL (1u << 31)
#define HUD_SHADOW_WINS   0x3Fu
#define HUD_TRIG_SW_EN    (1u << 8)
#define HUD_TRIG_HW_PULSE (1u << 0)

//
// 8x16 font, ASCII 0x20..0x5F (64 glyphs: space, symbols, digits, UPPERCASE);
// bit7 = leftmost pixel. Copied from Beyond0LteFbText.c (lowercase omitted to save
// space). index = ch - 0x20; chars outside 0x20..0x5F render as space.
//
STATIC CONST UINT8  mHudFont[64][16] = {
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },                                          /* ' ' */
  { 0, 0, 0x18, 0x3C, 0x3C, 0x3C, 0x18, 0x18, 0x18, 0, 0x18, 0x18, 0, 0, 0, 0 },               /* ! */
  { 0, 0x66, 0x66, 0x66, 0x24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },                              /* " */
  { 0, 0, 0, 0x6C, 0x6C, 0xFE, 0x6C, 0x6C, 0x6C, 0xFE, 0x6C, 0x6C, 0, 0, 0, 0 },               /* # */
  { 0x18, 0x18, 0x7C, 0xC6, 0xC2, 0xC0, 0x7C, 0x06, 0x86, 0xC6, 0x7C, 0x18, 0x18, 0, 0, 0 },   /* $ */
  { 0, 0, 0, 0, 0xC2, 0xC6, 0x0C, 0x18, 0x30, 0x60, 0xC6, 0x86, 0, 0, 0, 0 },                  /* % */
  { 0, 0, 0x38, 0x6C, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0xCC, 0xCC, 0x76, 0, 0, 0, 0 },            /* & */
  { 0, 0x30, 0x30, 0x30, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },                              /* ' */
  { 0, 0, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x30, 0x30, 0x18, 0x0C, 0, 0, 0, 0, 0 },               /* ( */
  { 0, 0, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0, 0, 0, 0, 0 },               /* ) */
  { 0, 0, 0, 0, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0, 0, 0, 0, 0, 0, 0 },                           /* * */
  { 0, 0, 0, 0, 0x18, 0x18, 0x7E, 0x18, 0x18, 0, 0, 0, 0, 0, 0, 0 },                           /* + */
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x18, 0x18, 0x18, 0x30, 0, 0, 0 },                              /* , */
  { 0, 0, 0, 0, 0, 0, 0xFE, 0, 0, 0, 0, 0, 0, 0, 0, 0 },                                       /* - */
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x18, 0x18, 0, 0, 0, 0 },                                    /* . */
  { 0, 0, 0x02, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0, 0, 0, 0, 0, 0 },                  /* / */
  { 0, 0, 0x38, 0x6C, 0xC6, 0xC6, 0xD6, 0xD6, 0xC6, 0xC6, 0x6C, 0x38, 0, 0, 0, 0 },            /* 0 */
  { 0, 0, 0x18, 0x38, 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0, 0, 0, 0 },            /* 1 */
  { 0, 0, 0x7C, 0xC6, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0xC6, 0xFE, 0, 0, 0, 0 },            /* 2 */
  { 0, 0, 0x7C, 0xC6, 0x06, 0x06, 0x3C, 0x06, 0x06, 0x06, 0xC6, 0x7C, 0, 0, 0, 0 },            /* 3 */
  { 0, 0, 0x0C, 0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x0C, 0x0C, 0x1E, 0, 0, 0, 0 },            /* 4 */
  { 0, 0, 0xFE, 0xC0, 0xC0, 0xC0, 0xFC, 0x06, 0x06, 0x06, 0xC6, 0x7C, 0, 0, 0, 0 },            /* 5 */
  { 0, 0, 0x38, 0x60, 0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0, 0, 0, 0 },            /* 6 */
  { 0, 0, 0xFE, 0xC6, 0x06, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x30, 0, 0, 0, 0 },            /* 7 */
  { 0, 0, 0x7C, 0xC6, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0, 0, 0, 0 },            /* 8 */
  { 0, 0, 0x7C, 0xC6, 0xC6, 0xC6, 0x7E, 0x06, 0x06, 0x06, 0x0C, 0x78, 0, 0, 0, 0 },            /* 9 */
  { 0, 0, 0, 0, 0x18, 0x18, 0, 0, 0, 0x18, 0x18, 0, 0, 0, 0, 0 },                              /* : */
  { 0, 0, 0, 0, 0x18, 0x18, 0, 0, 0, 0x18, 0x18, 0x30, 0, 0, 0, 0 },                           /* ; */
  { 0, 0, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x06, 0, 0, 0, 0, 0 },               /* < */
  { 0, 0, 0, 0, 0, 0x7E, 0, 0x7E, 0, 0, 0, 0, 0, 0, 0, 0 },                                    /* = */
  { 0, 0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x60, 0, 0, 0, 0, 0 },               /* > */
  { 0, 0, 0x7C, 0xC6, 0xC6, 0x0C, 0x18, 0x18, 0x18, 0, 0x18, 0x18, 0, 0, 0, 0 },               /* ? */
  { 0, 0, 0x7C, 0xC6, 0xC6, 0xDE, 0xDE, 0xDE, 0xDC, 0xC0, 0x7C, 0, 0, 0, 0, 0 },               /* @ */
  { 0, 0, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0xC6, 0, 0, 0, 0 },            /* A */
  { 0, 0, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x66, 0x66, 0xFC, 0, 0, 0, 0 },            /* B */
  { 0, 0, 0x3C, 0x66, 0xC2, 0xC0, 0xC0, 0xC0, 0xC0, 0xC2, 0x66, 0x3C, 0, 0, 0, 0 },            /* C */
  { 0, 0, 0xF8, 0x6C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0, 0, 0, 0 },            /* D */
  { 0, 0, 0xFE, 0x66, 0x62, 0x68, 0x78, 0x68, 0x60, 0x62, 0x66, 0xFE, 0, 0, 0, 0 },            /* E */
  { 0, 0, 0xFE, 0x66, 0x62, 0x68, 0x78, 0x68, 0x60, 0x60, 0x60, 0xF0, 0, 0, 0, 0 },            /* F */
  { 0, 0, 0x3C, 0x66, 0xC2, 0xC0, 0xC0, 0xDE, 0xC6, 0xC6, 0x66, 0x3A, 0, 0, 0, 0 },            /* G */
  { 0, 0, 0xC6, 0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0, 0, 0, 0 },            /* H */
  { 0, 0, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0, 0, 0, 0 },            /* I */
  { 0, 0, 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0xCC, 0x78, 0, 0, 0, 0 },            /* J */
  { 0, 0, 0xE6, 0x66, 0x66, 0x6C, 0x78, 0x78, 0x6C, 0x66, 0x66, 0xE6, 0, 0, 0, 0 },            /* K */
  { 0, 0, 0xF0, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x62, 0x66, 0xFE, 0, 0, 0, 0 },            /* L */
  { 0, 0, 0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0, 0, 0, 0 },            /* M */
  { 0, 0, 0xC6, 0xE6, 0xF6, 0xFE, 0xDE, 0xCE, 0xC6, 0xC6, 0xC6, 0xC6, 0, 0, 0, 0 },            /* N */
  { 0, 0, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0, 0, 0, 0 },            /* O */
  { 0, 0, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x60, 0xF0, 0, 0, 0, 0 },            /* P */
  { 0, 0, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xD6, 0xDE, 0x7C, 0x0C, 0x0E, 0, 0, 0 },         /* Q */
  { 0, 0, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x66, 0xE6, 0, 0, 0, 0 },            /* R */
  { 0, 0, 0x7C, 0xC6, 0xC6, 0x60, 0x38, 0x0C, 0x06, 0xC6, 0xC6, 0x7C, 0, 0, 0, 0 },            /* S */
  { 0, 0, 0x7E, 0x7E, 0x5A, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0, 0, 0, 0 },            /* T */
  { 0, 0, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0, 0, 0, 0 },            /* U */
  { 0, 0, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x10, 0, 0, 0, 0 },            /* V */
  { 0, 0, 0xC6, 0xC6, 0xC6, 0xC6, 0xD6, 0xD6, 0xD6, 0xFE, 0xEE, 0x6C, 0, 0, 0, 0 },            /* W */
  { 0, 0, 0xC6, 0xC6, 0x6C, 0x7C, 0x38, 0x38, 0x7C, 0x6C, 0xC6, 0xC6, 0, 0, 0, 0 },            /* X */
  { 0, 0, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x3C, 0, 0, 0, 0 },            /* Y */
  { 0, 0, 0xFE, 0xC6, 0x86, 0x0C, 0x18, 0x30, 0x60, 0xC2, 0xC6, 0xFE, 0, 0, 0, 0 },            /* Z */
  { 0, 0, 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0, 0, 0, 0 },            /* [ */
  { 0, 0, 0x80, 0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0, 0, 0, 0, 0, 0 },                  /* \ */
  { 0, 0, 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0, 0, 0, 0 },            /* ] */
  { 0x10, 0x38, 0x6C, 0xC6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },                              /* ^ */
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0 },                                       /* _ */
};

//
// Paint one line of ASCII text to the panel scanout at PixelRow (clears the band
// first). DXE/MMU-on; the FB at 0xCC000000 is Device-mapped (per first-light), so
// physical stores reach DRAM where DECON's DMA reads them. No boot services used.
//
STATIC
VOID
HudFbLine (
  IN UINT32       PixelRow,
  IN CONST CHAR8  *Str
  )
{
  volatile UINT32  *Fb = (volatile UINT32 *)(UINTN)HUD_FB_BASE;
  UINTN            PenX;
  UINTN            Row;
  UINTN            Col;
  UINTN            Sx;
  UINTN            Sy;
  UINT8            Bits;
  UINT8            Ch;
  UINT32           Color;

  for (Row = 0; Row < HUD_GLYPH_H * HUD_SCALE; Row++) {
    for (Col = 0; Col < HUD_FB_WIDTH; Col++) {
      Fb[(PixelRow + Row) * HUD_FB_WIDTH + Col] = HUD_BG;
    }
  }

  PenX = HUD_FB_MARGIN;
  while ((*Str != '\0') && (PenX + HUD_GLYPH_W * HUD_SCALE <= HUD_FB_WIDTH)) {
    Ch = (UINT8)*Str++;
    if ((Ch < 0x20) || (Ch > 0x5F)) {
      Ch = 0x20;
    }

    for (Row = 0; Row < HUD_GLYPH_H; Row++) {
      Bits = mHudFont[Ch - 0x20][Row];
      for (Col = 0; Col < HUD_GLYPH_W; Col++) {
        Color = ((Bits & (0x80u >> Col)) != 0) ? HUD_FG : HUD_BG;
        for (Sy = 0; Sy < HUD_SCALE; Sy++) {
          for (Sx = 0; Sx < HUD_SCALE; Sx++) {
            Fb[(PixelRow + Row * HUD_SCALE + Sy) * HUD_FB_WIDTH + (PenX + Col * HUD_SCALE + Sx)] = Color;
          }
        }
      }
    }

    PenX += HUD_GLYPH_W * HUD_SCALE;
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

//
// Push one fresh frame to the command-mode panel via the DECON SW-trigger 0->1
// edge (same sequence as the BDS first-light present). gBS->Stall is guarded so a
// very-early call is still safe (just no inter-write settle).
//
STATIC
VOID
HudPresent (
  VOID
  )
{
  volatile UINT32  *Trig = (volatile UINT32 *)(UINTN)(HUD_DECON_BASE + HUD_DECON_TRIG);
  volatile UINT32  *Shad = (volatile UINT32 *)(UINTN)(HUD_DECON_BASE + HUD_DECON_SHADOW);
  UINT32           Base;

  __asm__ __volatile__ ("dsb sy" ::: "memory");
  Base   = *Trig & ~HUD_TRIG_HW_PULSE;
  *Shad  = HUD_SHADOW_GLOBAL | HUD_SHADOW_WINS;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  if (gBS != NULL) { gBS->Stall (500); }
  *Trig  = Base | HUD_TRIG_SW_EN;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  if (gBS != NULL) { gBS->Stall (500); }
  *Trig  = Base | HUD_TRIG_SW_EN | HUD_TRIG_HW_PULSE;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  if (gBS != NULL) { gBS->Stall (6000); }
  *Trig  = Base;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

//
// Format Val as 8 uppercase hex digits into Buf[0..8] (NUL-terminated).
//
STATIC
VOID
HudHex8 (
  OUT CHAR8   *Buf,
  IN  UINT32  Val
  )
{
  UINTN  I;

  for (I = 0; I < 8; I++) {
    UINT8  Nyb = (UINT8)((Val >> ((7 - I) * 4)) & 0xFu);
    Buf[I] = (CHAR8)((Nyb < 10) ? ('0' + Nyb) : ('A' + Nyb - 10));
  }

  Buf[8] = '\0';
}

//
// Append Src to Dst (max Dst capacity Max incl. NUL). Tiny, no library dep.
//
STATIC
VOID
HudCat (
  IN OUT CHAR8        *Dst,
  IN     CONST CHAR8  *Src,
  IN     UINTN        Max
  )
{
  UINTN  D = 0;

  while ((Dst[D] != '\0') && (D < Max)) {
    D++;
  }

  while ((*Src != '\0') && (D < (Max - 1))) {
    Dst[D++] = *Src++;
  }

  Dst[D] = '\0';
}

STATIC UINT32  mHudSeq = 0;

//
// Drop a breadcrumb: paint 3 status lines (tag+seq, xfers+live-doorbell, phase+OCS)
// to the panel and push one frame. The seq counter monotonically increases so a
// frozen frame is unambiguously "the last checkpoint reached." Bounded-call only.
//
STATIC
VOID
Beyond0LteCrumb (
  IN CONST CHAR8  *Tag
  )
{
  volatile UINT32  *D = (volatile UINT32 *)(UINTN)UFSHC_DIAG_BASE;
  CHAR8            Line[80];
  CHAR8            Hex[9];

  mHudSeq++;

  Line[0] = '\0';
  HudCat (Line, "UFS> ", sizeof (Line));
  HudCat (Line, Tag, sizeof (Line));
  HudCat (Line, "  SEQ ", sizeof (Line));
  HudHex8 (Hex, mHudSeq);
  HudCat (Line, Hex, sizeof (Line));
  HudFbLine (HUD_ROW0, Line);

  Line[0] = '\0';
  HudCat (Line, "XF ", sizeof (Line));
  HudHex8 (Hex, D[6]);
  HudCat (Line, Hex, sizeof (Line));
  HudCat (Line, "  DB ", sizeof (Line));
  HudHex8 (Hex, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x58u));   // UTRLDBR (UFS_HC_UTRLDBR_OFF, def'd below)
  HudCat (Line, Hex, sizeof (Line));
  HudFbLine (HUD_ROW0 + HUD_LINEH, Line);

  Line[0] = '\0';
  HudCat (Line, "PH ", sizeof (Line));
  HudHex8 (Hex, D[13]);
  HudCat (Line, Hex, sizeof (Line));
  HudCat (Line, "  OCS ", sizeof (Line));
  HudHex8 (Hex, D[7]);
  HudCat (Line, Hex, sizeof (Line));
  HudFbLine (HUD_ROW0 + 2 * HUD_LINEH, Line);

  HudPresent ();
}

//
// STAGE breadcrumb published as a FUNCTION POINTER at a fixed DRAM address (0xFED13100)
// so the in-box EDK2 UfsPassThru (a separate module) can drop on-screen stage markers
// INSIDE its own dev-command flow - the only way to localize a non-MMIO hang in EDK2's
// post-completion code (UfsSendDmRequestRetry: Flush/Unmap/FreeBuffer + the query-response
// parse) that our register hooks cannot see. Paints "STAGE <code>" + a SEQ counter so the
// LAST stage frozen on the command-mode panel names the exact line EDK2 died on. Global +
// EFIAPI for a stable cross-module ABI; not static so its address survives.
//
VOID
EFIAPI
Beyond0LteStage (
  IN UINT32  Code
  )
{
  volatile UINT32  *D = (volatile UINT32 *)(UINTN)UFSHC_DIAG_BASE;
  CHAR8            Line[64];
  CHAR8            Hex[9];

  mHudSeq++;

  if ((Code & 0xFFFF0000u) == BEYOND0LTE_STAGE_UFS_READ) {
    Line[0] = '\0';
    HudCat (Line, "UFS> READ ", sizeof (Line));
    HudCat (Line, ((Code & 1u) != 0) ? "DONE" : "RING", sizeof (Line));
    HudCat (Line, " SEQ ", sizeof (Line));
    HudHex8 (Hex, D[400]);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0, Line);

    Line[0] = '\0';
    HudCat (Line, "LBA ", sizeof (Line));
    HudHex8 (Hex, D[402]);
    HudCat (Line, Hex, sizeof (Line));
    HudCat (Line, " CNT ", sizeof (Line));
    HudHex8 (Hex, D[403]);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0 + HUD_LINEH, Line);

    Line[0] = '\0';
    HudCat (Line, "DB ", sizeof (Line));
    HudHex8 (Hex, D[405]);
    HudCat (Line, Hex, sizeof (Line));
    HudCat (Line, " OCS ", sizeof (Line));
    HudHex8 (Hex, (D[407] >> 8) & 0xFFu);
    HudCat (Line, Hex, sizeof (Line));
    HudCat (Line, " ST ", sizeof (Line));
    HudHex8 (Hex, (D[407] >> 16) & 0xFFFFu);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0 + 2 * HUD_LINEH, Line);

    Line[0] = '\0';
    HudCat (Line, "FILE ", sizeof (Line));
    HudCat (Line, (CONST CHAR8 *)(VOID *)&D[416], sizeof (Line));
    HudFbLine (HUD_ROW0 + 3 * HUD_LINEH, Line);

    Line[0] = '\0';
    HudCat (Line, "FOFF ", sizeof (Line));
    HudHex8 (Hex, D[412]);
    HudCat (Line, Hex, sizeof (Line));
    HudCat (Line, " FREQ ", sizeof (Line));
    HudHex8 (Hex, D[413]);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0 + 4 * HUD_LINEH, Line);

    HudPresent ();
    return;
  }

  if ((Code & 0xFFFF0000u) == BEYOND0LTE_STAGE_FAT_READ) {
    Line[0] = '\0';
    HudCat (Line, "FAT> READ ", sizeof (Line));
    HudCat (Line, ((Code & 1u) != 0) ? "DONE" : "RING", sizeof (Line));
    HudCat (Line, " SEQ ", sizeof (Line));
    HudHex8 (Hex, D[410]);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0, Line);

    Line[0] = '\0';
    HudCat (Line, (CONST CHAR8 *)(VOID *)&D[416], sizeof (Line));
    HudFbLine (HUD_ROW0 + HUD_LINEH, Line);

    Line[0] = '\0';
    HudCat (Line, "OFF ", sizeof (Line));
    HudHex8 (Hex, D[412]);
    HudCat (Line, Hex, sizeof (Line));
    HudCat (Line, " REQ ", sizeof (Line));
    HudHex8 (Hex, D[413]);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0 + 2 * HUD_LINEH, Line);

    Line[0] = '\0';
    HudCat (Line, "GOT ", sizeof (Line));
    HudHex8 (Hex, D[414]);
    HudCat (Line, Hex, sizeof (Line));
    HudCat (Line, " ST ", sizeof (Line));
    HudHex8 (Hex, D[411]);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0 + 3 * HUD_LINEH, Line);

    HudPresent ();
    return;
  }

  if ((Code & 0xFFFF0000u) == BEYOND0LTE_STAGE_BS_CALL) {
    Line[0] = '\0';
    HudCat (Line, "BS> ", sizeof (Line));
    HudCat (Line, (CONST CHAR8 *)(VOID *)&D[436], sizeof (Line));
    HudCat (Line, ((Code & 1u) != 0) ? " DONE" : " RING", sizeof (Line));
    HudCat (Line, " SEQ ", sizeof (Line));
    HudHex8 (Hex, D[430]);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0, Line);

    Line[0] = '\0';
    HudCat (Line, "ST ", sizeof (Line));
    HudHex8 (Hex, D[431]);
    HudCat (Line, Hex, sizeof (Line));
    HudCat (Line, " A ", sizeof (Line));
    HudHex8 (Hex, D[432]);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0 + HUD_LINEH, Line);

    Line[0] = '\0';
    HudCat (Line, "B ", sizeof (Line));
    HudHex8 (Hex, D[433]);
    HudCat (Line, Hex, sizeof (Line));
    HudCat (Line, " C ", sizeof (Line));
    HudHex8 (Hex, D[434]);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0 + 2 * HUD_LINEH, Line);

    Line[0] = '\0';
    HudCat (Line, "FILE ", sizeof (Line));
    HudCat (Line, (CONST CHAR8 *)(VOID *)&D[416], sizeof (Line));
    HudFbLine (HUD_ROW0 + 3 * HUD_LINEH, Line);

    Line[0] = '\0';
    HudCat (Line, "FOFF ", sizeof (Line));
    HudHex8 (Hex, D[412]);
    HudCat (Line, Hex, sizeof (Line));
    HudCat (Line, " FREQ ", sizeof (Line));
    HudHex8 (Hex, D[413]);
    HudCat (Line, Hex, sizeof (Line));
    HudFbLine (HUD_ROW0 + 4 * HUD_LINEH, Line);

    HudPresent ();
    return;
  }

  Line[0] = '\0';
  HudCat (Line, "STAGE ", sizeof (Line));
  HudHex8 (Hex, Code);
  HudCat (Line, Hex, sizeof (Line));
  HudCat (Line, "  SEQ ", sizeof (Line));
  HudHex8 (Hex, mHudSeq);
  HudCat (Line, Hex, sizeof (Line));
  HudFbLine (HUD_ROW0, Line);

  Line[0] = '\0';
  HudCat (Line, "XF ", sizeof (Line));
  HudHex8 (Hex, D[6]);
  HudCat (Line, Hex, sizeof (Line));
  HudCat (Line, "  DB ", sizeof (Line));
  HudHex8 (Hex, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x58u));
  HudCat (Line, Hex, sizeof (Line));
  HudFbLine (HUD_ROW0 + HUD_LINEH, Line);

  HudPresent ();
}

#else

STATIC
VOID
Beyond0LteCrumb (
  IN CONST CHAR8  *Tag
  )
{
  (VOID)Tag;
}

VOID
EFIAPI
Beyond0LteStage (
  IN UINT32  Code
  )
{
  (VOID)Code;
}

#endif

/**
  Return the MMIO base of the UFS host controller.
**/
EFI_STATUS
EFIAPI
UfsHcGetMmioBar (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT UINTN                               *MmioBar
  )
{
  UfsHcDiagBump (0);

  if (MmioBar == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *MmioBar = (UINTN)EXYNOS_UFS_HCI_BASE;
  return EFI_SUCCESS;
}


#define UFS_COMMON_EXTRA_PAGES 17u  // B83: AllocateBuffer slack so a UCD holds 511 x 128 B PRDT entries past EDK2's own layout

/**
  Allocate common-buffer pages for the UTP descriptor/UPIU rings. Returns UNCACHED
  memory so the non-coherent UFS DMA engine and the CPU see a consistent view
  without per-access cache maintenance.
**/
EFI_STATUS
EFIAPI
UfsHcAllocateBuffer (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN     EFI_ALLOCATE_TYPE                   Type,
  IN     EFI_MEMORY_TYPE                     MemoryType,
  IN     UINTN                               Pages,
  OUT    VOID                                **HostAddress,
  IN     UINT64                              Attributes
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Address;

  if (HostAddress == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // DIAG: record reaching ring allocation + the UFS link state (HCS) right now.
  // If this fires, UfsPassThru.Start() got past controller-init/link-startup.
  //
  #if BEYOND0LTE_UFS_DIAG
  if (((volatile UINT32 *)(UINTN)UFSHC_DIAG_BASE)[1] == 0) {
    UfsHcDiagSet (6, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x30));
  }
  #endif
  UfsHcDiagBump (1);

  //
  // A51 (low-addr cap) reverted - it was a no-op (the >=512B data-write SBFES is address-independent).
  //
  Pages  += UFS_COMMON_EXTRA_PAGES;   // B83: room for the 128 B-stride split PRDT (freed symmetrically)
  //
  // W2: bank1/bank2 are now RAM (notes/m10-windows-deploy-plan.md) but UFS DMA is only proven into
  // bank0 (boot #25), so the UTP rings / UCDs stay below bank0's end.
  //
  Address = EXYNOS_DRAM_BASE + EXYNOS_DRAM_BANK0_SIZE - 1;
  Status  = gBS->AllocatePages (AllocateMaxAddress, MemoryType, Pages, &Address);
  if (EFI_ERROR (Status)) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // *** CYCLE A48: revert A47 (cached) -> back to EFI_MEMORY_WC (Normal Non-Cacheable) for the common buffer
  // (UTP descriptors + Response UPIU). A47 cached + A46 byte-gran still SBFES'd on the controller's WRITE; the
  // generic EDK2 UFS driver does NO per-transfer cache maintenance on common buffers (assumes DMA-coherent),
  // so cached is the wrong model. WC is coherent-by-non-cacheable AND allows the unaligned UPIU field access
  // (unlike UC = Device memory, which alignment-faulted - DEVICE-CONFIRMED). Restore the native WC buffer.
  //
  {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDesc;
    Status = gDS->GetMemorySpaceDescriptor (Address, &GcdDesc);
    if (!EFI_ERROR (Status)) {
      Status = gDS->SetMemorySpaceCapabilities (
                      Address,
                      EFI_PAGES_TO_SIZE (Pages),
                      GcdDesc.Capabilities | EFI_MEMORY_WC
                      );
    }
  }
  Status = gDS->SetMemorySpaceAttributes (
                  Address,
                  EFI_PAGES_TO_SIZE (Pages),
                  EFI_MEMORY_WC
                  );
  UfsHcDiagSet (5, 1);
  ZeroMem ((VOID *)(UINTN)Address, EFI_PAGES_TO_SIZE (Pages));
  *HostAddress = (VOID *)(UINTN)Address;
  return EFI_SUCCESS;
}

/**
  Free common-buffer pages, restoring write-back caching first.
**/
EFI_STATUS
EFIAPI
UfsHcFreeBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  UINTN                               Pages,
  IN  VOID                                *HostAddress
  )
{
  Pages += UFS_COMMON_EXTRA_PAGES;   // B83: matches UfsHcAllocateBuffer
  gDS->SetMemorySpaceAttributes (
         (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress,
         EFI_PAGES_TO_SIZE (Pages),
         EFI_MEMORY_WB
         );
  gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress, Pages);
  return EFI_SUCCESS;
}

//
// DMA BOUNCE BUFFER (Write-Combining). The common buffer (UTP descriptors) is WC and the
// controller accesses it fine, but EDK2's streaming DATA buffers are the CALLER's CACHED
// memory. A tiny INQUIRY/READ-CAP drains fine, but a 4KB HS-G3 block read appears to outrun a
// cached-memory DMA drain -> RX FIFO backpressure -> the device line-resets the link
// (UECPA=0x10, no DL errors, size-dependent, gear/mode/CDR-independent - matches exactly).
// Route streaming reads/writes through a persistent WC bounce buffer (proven DMA-coherent like
// the descriptors) + memcpy, falling back to identity if it does not fit / is busy.
//
#define UFS_BOUNCE_BYTES  (512u * 1024u)
STATIC VOID     *mBounceBuf  = NULL;
STATIC UINTN     mBounceSize = 0;
STATIC BOOLEAN   mBounceBusy = FALSE;
STATIC UINT32    mFallData   = 0;       // cycle 96: count of Read/Write data-buffer fall-throughs to cached path
STATIC UINT32    mBusyReset  = 0;       // cycle 97: count of stale mBounceBusy leaks cleared at Map entry
STATIC BOOLEAN   mBufProbed  = FALSE;   // cycle 84: one-shot first-media-read content probe done
STATIC BOOLEAN   mGptProbed  = FALSE;   // cycle 87: one-shot GPT-header ("EFI PART") seen

STATIC
VOID *
Beyond0LteAllocWcPages (
  IN UINTN  Pages
  )
{
  EFI_STATUS                       Status;
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDesc;

  //
  // A105 (REVERTED - crashed): relocating the bounce to the 0xC0000000 DRAM bank raised a
  // Synchronous Exception (data abort) - the OTHER System-RAM banks the booted kernel shows
  // (0xC0000000+, 0xE1900000+, high 0x880000000+) are NOT non-secure-accessible to UEFI at
  // boot; sboot/TZASC hands UEFI only the 0x80000000-0xBC800000 bank. So the data bounce MUST
  // live in this first bank and the large-data-in SBFES here cannot be dodged by re-banking.
  // Restored the A103 WC alloc below.
  //
  // A51 (low-addr cap) was a NO-OP: capping the bounce to ~0x9FF7xxxx did not change the >=512B data-write
  // SBFES, so the fault is address-independent. Reverted to MAX_ADDRESS (the stable default).
  //
  Address = EXYNOS_DRAM_BASE + EXYNOS_DRAM_BANK0_SIZE - 1;   // W2: bank0 only (bank1/bank2 are RAM now, UFS DMA proven into bank0 only)
  Status  = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Pages, &Address);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  //
  // *** CYCLE A48: revert A45 (cached) -> back to EFI_MEMORY_WC (Normal Non-Cacheable) for the DATA bounce.
  // With A46's correct byte-gran PRDT the controller now REACHES the data write, but the CACHED bounce (A45)
  // + cached descriptors (A47) still SBFES'd on the controller's WRITE - the generic EDK2 UFS driver does NO
  // per-transfer cache clean/invalidate on its common/stream buffers (it assumes they are DMA-coherent), so
  // cached memory is the wrong model for it. WC = coherent-by-non-cacheable AND allows the unaligned UPIU
  // access. Restore the native WC streaming buffer (the real bug was the byte-gran PRDT, now fixed in A46).
  //
  if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor (Address, &GcdDesc))) {
    gDS->SetMemorySpaceCapabilities (Address, EFI_PAGES_TO_SIZE (Pages), GcdDesc.Capabilities | EFI_MEMORY_WC);
  }

  gDS->SetMemorySpaceAttributes (Address, EFI_PAGES_TO_SIZE (Pages), EFI_MEMORY_WC);  // A103: WC non-cacheable data bounce (revert A101 WB) - DESCTYPE-3 + WC model
  ZeroMem ((VOID *)(UINTN)Address, EFI_PAGES_TO_SIZE (Pages));
  return (VOID *)(UINTN)Address;
}

/**
  Map a streaming buffer for UFS DMA. Identity mapping (no remap unit); apply
  cache maintenance so the non-coherent engine sees / produces correct data.
**/
EFI_STATUS
EFIAPI
UfsHcMap (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN     EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN     VOID                                 *HostAddress,
  IN OUT UINTN                                *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS                 *DeviceAddress,
  OUT    VOID                                 **Mapping
  )
{
  UFSHC_MAP_INFO  *MapInfo;

  if ((HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Operation >= EdkiiUfsHcOperationMaximum) {
    return EFI_INVALID_PARAMETER;
  }

  UfsHcDiagBump (2);

  MapInfo = AllocatePool (sizeof (UFSHC_MAP_INFO));
  if (MapInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  MapInfo->Operation   = Operation;
  MapInfo->HostAddress = HostAddress;
  MapInfo->Length      = *NumberOfBytes;
  MapInfo->Bounced     = FALSE;

  //
  // Bounce streaming data through the WC buffer (lazy-allocated) when it fits and is free: the
  // WC region is DMA-coherent like the descriptors, avoiding the cached-memory drain that
  // appears to backpressure a 4KB HS burst. BusMasterRead = device reads host mem (stage now);
  // BusMasterWrite = device writes host mem (copy out in Unmap). Over-size / busy / CommonBuffer
  // fall through to the identity + cache-maintenance path below.
  //
  if (mBounceBuf == NULL) {
    mBounceBuf = Beyond0LteAllocWcPages (EFI_SIZE_TO_PAGES (UFS_BOUNCE_BYTES));
    if (mBounceBuf != NULL) {
      mBounceSize = UFS_BOUNCE_BYTES;
    }
  }

  UfsHcDiagSet (27, (UINT32)(UINTN)mBounceBuf);   // cycle 97: bounce buffer addr (0 => WC alloc failed)
  {
    STATIC BOOLEAN  mBounceLogged = FALSE;

    if (!mBounceLogged && (mBounceBuf != NULL)) {
      mBounceLogged = TRUE;
      DEBUG ((DEBUG_ERROR, "UfsHc: bounce buffer 0x%lx size 0x%x (B79)\n", (UINT64)(UINTN)mBounceBuf, (UINT32)mBounceSize));
    }
  }

#if UFS_DO_PMC
  //
  // CYCLE A19: one-shot pram dump of the WC bounce buffer addr/size, so from TWRP we can see whether the
  // bounce allocated (BNC=0 => alloc failed => ALL data reads take the cached path that backpressures the
  // HS burst under Fat load = the read-halt root).
  //
  {
    STATIC BOOLEAN  mBncReported = FALSE;
    if (!mBncReported) {
      mBncReported = TRUE;
      Beyond0LtePramStr ("\nA19 BNC=");
      Beyond0LtePramHex8 ((UINT8)((UINT32)(UINTN)mBounceBuf >> 24));
      Beyond0LtePramHex8 ((UINT8)((UINT32)(UINTN)mBounceBuf >> 16));
      Beyond0LtePramHex8 ((UINT8)((UINT32)(UINTN)mBounceBuf >> 8));
      Beyond0LtePramHex8 ((UINT8)((UINT32)(UINTN)mBounceBuf));
      Beyond0LtePramStr (" sz=");
      Beyond0LtePramHex8 ((UINT8)(mBounceSize >> 16));
      Beyond0LtePramHex8 ((UINT8)(mBounceSize >> 8));
      Beyond0LtePramHex8 ((UINT8)mBounceSize);
      Beyond0LtePramByte ((UINT8)'\n');
    }
  }
#endif

  //
  // *** CYCLE 97 FIX: UEFI boot-time BlockIo is synchronous + non-reentrant, so a still-set mBounceBusy at
  // Map entry for a Read/Write DATA buffer can ONLY be a STALE LEAK from a prior transfer whose Unmap was
  // skipped (a failed/timed-out command). cy96 saw fall=271 = the bounce bypassed en masse: ONE leaked
  // Unmap permanently forces every later read onto the cached path that stalls the HS burst (OCS=0xF) =
  // the cumulative wedge. Clear the stale flag here so the coherent WC bounce is ALWAYS reused.
  //
  if (mBounceBusy &&
      ((Operation == EdkiiUfsHcOperationBusMasterRead) ||
       (Operation == EdkiiUfsHcOperationBusMasterWrite)))
  {
    mBounceBusy = FALSE;
    mBusyReset++;
    UfsHcDiagSet (26, mBusyReset);
  }

  if ((mBounceBuf != NULL) && !mBounceBusy && (*NumberOfBytes <= mBounceSize) &&
      ((Operation == EdkiiUfsHcOperationBusMasterRead) ||
       (Operation == EdkiiUfsHcOperationBusMasterWrite)))
  {
    mBounceBusy      = TRUE;
    MapInfo->Bounced = TRUE;
    if (Operation == EdkiiUfsHcOperationBusMasterRead) {
      CopyMem (mBounceBuf, HostAddress, *NumberOfBytes);
    }

    //
    // *** A101: COHERENT model (iocoh ON + WB inner-shareable bounce) => NO cache maintenance. The CCI snoops
    // the CPU cache, so the controller sees the CopyMem'd data-OUT and the CPU sees the DMA'd data-IN with no
    // clean/invalidate (which would fight the coherent DMA). Keep only a dsb to order the CopyMem store before
    // the doorbell. Removing this WriteBackInvalidate is the single variable A96 (iocoh ON + WB) never tried.
    //
    __asm__ __volatile__ ("dsb sy" ::: "memory");

    *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)mBounceBuf;
    *Mapping       = MapInfo;
    return EFI_SUCCESS;
  }

  //
  // *** CYCLE 96: reached here = the WC bounce was NOT used, so this Read/Write DATA buffer takes the cached
  // identity path - the path known to backpressure/stall the HS burst (OCS=0xF) on this device. Count these
  // dangerous data-buffer fall-throughs (D[24], panel "fall") and snapshot WHY (D[25], panel "why": bit0=
  // bounce NULL, bit1=bounce BUSY, bit2=size>bounce; [31:8]=this transfer's byte length). fall>0 at BDS =>
  // the cumulative wedge is the bounce being bypassed (likely mBounceBusy stuck from a leaked Unmap) =>
  // fix = make the bounce robust. fall=0 => bounce always used => the wedge is a real controller-level stall.
  //
  if ((Operation == EdkiiUfsHcOperationBusMasterRead) ||
      (Operation == EdkiiUfsHcOperationBusMasterWrite))
  {
    mFallData++;
    UfsHcDiagSet (24, mFallData);
    UfsHcDiagSet (
      25,
      (mBounceBuf == NULL ? 1u : 0u) | (mBounceBusy ? 2u : 0u) |
      ((*NumberOfBytes > mBounceSize) ? 4u : 0u) | ((UINT32)(*NumberOfBytes & 0xFFFFFFu) << 8)
      );
#if UFS_DO_PMC
    //
    // CYCLE A19: one-shot pram dump of the FIRST data-buffer fall-through (bounce bypassed). why bits:
    // [0]=bounce NULL, [1]=bounce busy, [2]=size>bounce; len = transfer bytes. FALL present => some data
    // read used the cached path (the HS-burst backpressure source); absent => bounce always used.
    //
    {
      STATIC BOOLEAN  mFallReported = FALSE;
      if (!mFallReported) {
        mFallReported = TRUE;
        Beyond0LtePramStr ("A19 FALL why=");
        Beyond0LtePramHex8 ((UINT8)((mBounceBuf == NULL ? 1u : 0u) | (mBounceBusy ? 2u : 0u) | ((*NumberOfBytes > mBounceSize) ? 4u : 0u)));
        Beyond0LtePramStr (" len=");
        Beyond0LtePramHex8 ((UINT8)(*NumberOfBytes >> 16));
        Beyond0LtePramHex8 ((UINT8)(*NumberOfBytes >> 8));
        Beyond0LtePramHex8 ((UINT8)*NumberOfBytes);
        Beyond0LtePramByte ((UINT8)'\n');
      }
    }
#endif
  }

  //
  // Pre-DMA maintenance:
  //  - Read (CPU->device): clean so the device reads the CPU's latest bytes.
  //  - Write/CommonBuffer (device->CPU): clean+invalidate so no dirty CPU line
  //    later evicts over the device's data; Unmap invalidates again post-DMA.
  //
  if (Operation == EdkiiUfsHcOperationBusMasterRead) {
    WriteBackDataCacheRange (HostAddress, *NumberOfBytes);
  } else {
    WriteBackInvalidateDataCacheRange (HostAddress, *NumberOfBytes);
  }

  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  *Mapping       = MapInfo;
  return EFI_SUCCESS;
}

/**
  Complete a streaming Map(): for device->CPU transfers, invalidate so the CPU
  reads the freshly DMA'd data rather than stale cache lines.
**/
EFI_STATUS
EFIAPI
UfsHcUnmap (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  VOID                                *Mapping
  )
{
  UFSHC_MAP_INFO  *MapInfo;

  if (Mapping == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  MapInfo = (UFSHC_MAP_INFO *)Mapping;

  if (MapInfo->Bounced) {
    //
    // Device->CPU (BusMasterWrite = a SCSI READ): copy the freshly DMA'd data out of the WC
    // bounce buffer into the caller's buffer, then release the bounce for the next transfer.
    //
    if (MapInfo->Operation == EdkiiUfsHcOperationBusMasterWrite) {
      //
      // *** A83: WB bounce + clean-before-DMA (UfsHcMap) => the coherent DMA data is current; do NOT invalidate
      // here (would discard coherent cache-resident DMA data). dsb to order the completed DMA, then copy out.
      //
      __asm__ __volatile__ ("dsb sy" ::: "memory");
      CopyMem (MapInfo->HostAddress, mBounceBuf, MapInfo->Length);
      //
      // *** CYCLE 84: one-shot probe of the FIRST media read's data. *** Now that the transfer completes
      // (s3210OCS=0), verify VALID partition data actually landed: D[39] (panel rd0) = MBR signature @0x1FE
      // (0xAA55 = valid MBR / protective-MBR) in [31:16] | read length in [15:0]; D[40] (panel rdsns) =
      // first data dword. rd0=AA55xxxx => data arrives & the read WORKS (partitions=0 is then a parse/GPT
      // or 512B-recovery-truncation issue); rd0=0000xxxx => read completes but no MBR (zeros/FMP/cache).
      //
      if (!mBufProbed && (MapInfo->Length >= 512u)) {
        UINT8   *Bb  = (UINT8 *)mBounceBuf;
        UINT32  Sig  = (UINT32)(Bb[0x1FEu] | (Bb[0x1FFu] << 8));   // MBR/protective-MBR signature (0xAA55 if valid)
        mBufProbed = TRUE;
        UfsHcDiagSet (39, (Sig << 16) | (UINT32)(MapInfo->Length & 0xFFFFu));
        UfsHcDiagSet (40, *(UINT32 *)Bb);
        //
        // *** CYCLE 87: x46 (D[47]) = LBA0 read = MBR sig @0x1FE [31:16] | protective-MBR partition-entry
        // TYPE @0x1C2 [15:8] (0xEE = GPT protective => PartitionDxe should parse GPT) | read length/512
        // [7:0] (08 = 4KB block, 01 = 512B). AA55EE08 = a 4KB-block GPT protective MBR.
        //
        UfsHcDiagSet (47, (Sig << 16) | ((UINT32)Bb[0x1C2u] << 8) | (UINT32)((MapInfo->Length >> 9) & 0xFFu));
      }
      //
      // *** CYCLE 87: g48 (D[48]) = 2nd dword of the first read starting with "EFI " (0x20494645) = "PART"
      // (0x54524150) => the GPT HEADER (LBA1) was actually read OK; g48=0 => PartitionDxe never obtained a
      // valid GPT header (wrong LBA / block-size mismatch / that read failed).
      //
      if (!mGptProbed && (*(UINT32 *)mBounceBuf == 0x20494645u)) {
        mGptProbed = TRUE;
        UfsHcDiagSet (48, *(UINT32 *)((UINT8 *)mBounceBuf + 4u));
      }
    }

    mBounceBusy = FALSE;
  } else if (MapInfo->Operation != EdkiiUfsHcOperationBusMasterRead) {
    InvalidateDataCacheRange (MapInfo->HostAddress, MapInfo->Length);
  }

  FreePool (MapInfo);
  return EFI_SUCCESS;
}

/**
  Flush posted writes. A data synchronization barrier orders all prior MMIO and
  memory accesses before the device acts on them.
**/
EFI_STATUS
EFIAPI
UfsHcFlush (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  MemoryFence ();
  return EFI_SUCCESS;
}

/**
  Common MMIO accessor for Read()/Write().
**/
STATIC
EFI_STATUS
UfsHcMmioRw (
  IN     BOOLEAN                                   Write,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  )
{
  UINTN  Base;
  UINTN  Index;
  UINTN  Stride;
  UINTN  Addr;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Width >= EfiUfsHcWidthMaximum) {
    return EFI_INVALID_PARAMETER;
  }

  Base   = (UINTN)EXYNOS_UFS_HCI_BASE + (UINTN)Offset;
  Stride = (UINTN)1 << (UINTN)Width;   // 1,2,4,8 bytes

  for (Index = 0; Index < Count; Index++) {
    Addr = Base + Index * Stride;
    switch (Width) {
    case EfiUfsHcWidthUint8:
      if (Write) {
        MmioWrite8 (Addr, ((UINT8 *)Buffer)[Index]);
      } else {
        ((UINT8 *)Buffer)[Index] = MmioRead8 (Addr);
      }
      break;
    case EfiUfsHcWidthUint16:
      if (Write) {
        MmioWrite16 (Addr, ((UINT16 *)Buffer)[Index]);
      } else {
        ((UINT16 *)Buffer)[Index] = MmioRead16 (Addr);
      }
      break;
    case EfiUfsHcWidthUint32:
      if (Write) {
        MmioWrite32 (Addr, ((UINT32 *)Buffer)[Index]);
      } else {
        ((UINT32 *)Buffer)[Index] = MmioRead32 (Addr);
      }
      break;
    case EfiUfsHcWidthUint64:
      if (Write) {
        MmioWrite64 (Addr, ((UINT64 *)Buffer)[Index]);
      } else {
        ((UINT64 *)Buffer)[Index] = MmioRead64 (Addr);
      }
      break;
    default:
      return EFI_INVALID_PARAMETER;
    }
  }

  return EFI_SUCCESS;
}

//
// EXYNOS NEXUS-TYPE FIX (the transport-layer blocker, not a PHY/cal issue). The
// Exynos UFS controller has a vendor HCI_UTRL_NEXUS_TYPE register (VS+0x40) that
// must match each transfer's UPIU type BEFORE its doorbell is rung:
//   SCSI Command -> 0xFFFFFFFF (all slots "command nexus")
//   NOP / Query  -> 0x00000000
// per the Samsung reference (ExynosUfsLib __utp_send). This is normally a
// per-request hook (exynos_ufs_set_nexus_t_xfer_req); generic UfsPassThru has no
// such hook, so with our static 0xFFFFFFFF the very FIRST transfer — a NOP OUT —
// is misclassified as a SCSI command, the controller never completes it, and the
// doorbell stays set with NO error raised (exactly the UTRLDBR=1 / 6M-poll hang).
// We intercept the transfer doorbell write (UTRLDBR, HCI offset 0x58) and set the
// nexus type from the in-flight UTRD's Command UPIU transaction type. UFS boot is
// serialized (one transfer in flight at a time), so writing the whole register
// from the single rung slot is correct — matching the reference.
//
#define UFS_HC_UTRLBA_OFF    0x50u
#define UFS_HC_UTRLBAU_OFF   0x54u
#define UFS_HC_UTRLDBR_OFF   0x58u
#define UFS_HC_UTRLCLR_OFF   0x5Cu // UTP Transfer Request List CLEAR. Per-slot force-clear
                                  // of a doorbell that did not auto-retire. Exynos sets
                                  // UFSHCD_QUIRK_BROKEN_REQ_LIST_CLR which only INVERTS the
                                  // polarity (it is NOT unusable): the kernel's
                                  // ufshcd_utrl_clear writes (1<<pos) here (vs ~(1<<pos)
                                  // normally), then waits UTRLDBR[pos]->0. This is the
                                  // kernel's escape hatch (ufshcd_clear_cmd) for exactly a
                                  // stuck doorbell, and is a DIFFERENT register than IS
                                  // (0x20, which wedges) and run-stop (0x60, a no-op here).
#define UFS_HC_UTRLRSR_OFF   0x60u // UTP Transfer Request List Run-Stop. bit0=run.
                                  // Toggling 1->0->1 was tried to clear completed doorbell
                                  // bits but is a NO-OP on this controller (x1.4M polls).
#define UFS_HC_UTRLRSR_RUN   0x1u // bit0 = list running
#define UFS_HC_NEXUS_DONE    0x140u // Exynos vendor reg in the standard HCI 0x200
                                    // window; reference sets bit0 AFTER each
                                    // NOP/QUERY completion to release the UTP
                                    // engine for the next transfer.
#define UTP_TRD_SIZE         32u
#define UTP_TRD_OCS_DW2      8u    // DW2: Ocs:8 | Rsvd:24 (Overall Command Status)
#define UTP_TRD_OCS_INIT     0x0Fu // EDK2 UFS_HC_TRD_OCS_INIT_VALUE: OCS the host
                                   // writes before ringing; the controller
                                   // overwrites it on completion. OCS != 0x0F
                                   // therefore means "this slot has completed".
#define UTP_TRD_UCDBA_DW4    16u   // DW4: Rsvd6:7 | UcdBa:25 (addr[31:7])
#define UTP_TRD_UCDBAU_DW5   20u   // DW5: UcdBaU (addr[63:32])
#define UPIU_TXTYPE_NOP      0x00u
#define UPIU_TXTYPE_COMMAND  0x01u // NOP OUT=0x00, Command=0x01, Query Req=0x16
#define UPIU_TXTYPE_QUERY    0x16u

//
// EXYNOS PRDT_BYTE_GRAN QUIRK (Linux UFSHCD_QUIRK_PRDT_BYTE_GRAN, confirmed in
// ref/u9810-ufs/ufs-exynos.c line 561 and the Samsung ExynosUfsLib reference,
// which programs rsp_upiu_off/len and prdt_off/len in BYTES). The Exynos UTP
// engine reads the UTRD Response-UPIU and PRDT length/offset fields (DW6/DW7) in
// BYTE granularity. The 9810 tree relied on a locally patched edk2 UfsPassThruHci.c
// that emitted byte-granular fields; that patch is NOT in this checkout (edk2 is
// gitignored, notes/m8-uefi-image.md B66): stock UfsPassThruHci.c:550-553/643-647/
// 704-705 write dwords, which is exactly boot #17's "Failed to send query request,
// OCS = 4" (OCS_MISMATCH_RESP_UPIU_SIZE). So the platform-side conversion is ON: DW6/DW7
// are converted when the doorbell rings and restored on completion, and stock
// UfsPassThruDxe keeps reading dword offsets.
//
// 9820 values, VERIFIED from the S10-family kernel (LineageOS
// android_kernel_samsung_exynos9820, drivers/scsi/ufs): ufs-exynos.c:585 sets
// UFSHCD_QUIRK_PRDT_BYTE_GRAN; ufshcd.c:4424-4430 writes response/PRDT offsets and the
// response length in bytes; PRDT entries are plain 16-byte ufshcd_sg_entry
// (ufshcd.c:10184 sg_entry_size = sizeof, ufshci.h:435) - the same as EDK2's
// UTP_TR_PRD - and for CONFIG_SOC_EXYNOS9820 the PRDT length is
// segments * 16 + 16 bytes (ufshcd.c:2635-2638). The 9810 lineage's 128-byte FMP
// entry does not apply here.
//
#define BEYOND0LTE_CONVERT_UTRD_BYTE_GRAN 1
#define UTP_TRD_RESP_DW6     24u   // DW6: RuL[15:0] (len, dwords) | RuO[31:16] (off, dwords)
#define UTP_TRD_PRDT_DW7     28u   // DW7: PrdtL[15:0] (entries)   | PrdtO[31:16] (off, dwords)
// B83: one controller PRDT entry is 128 B (4 standard + 28 FMP dwords) - the 9810 measurement from the
// working Samsung kernel ("WOAXFER PrdtL=0x80/entry", this file's original constant), and the only layout
// consistent with boot #24: 1-entry reads complete (entry 0 is at offset 0 for any stride), the 4-entry
// 16 KB read never completes (the controller reads entry 1 at +128). FMP dwords zeroed = crypto bypass.
#define UTP_TR_PRD_SIZE      128u
#define UTP_PRDT_MAX_SPLIT   511u   // DW7 PrdtL is 16-bit bytes: 511 x 128 = 65408 -> ~2 MB per command at 4 KB/entry
// B81: the LineageOS 9820 kernel adds 16 B to the byte-gran PRDT length (ufshcd.c:2635-2638), but Samsung's
// own bootloader UFS code for this SoC (edk2-exynos Exynos9820Pkg/Library/ExynosUfsLib/ufs.c:451-463, LK
// <dev/ufs.h>) uses prdt_len = segments * sizeof(struct ufs_prdt) with 4 KB segments (:316-323,
// TX/RXPRDT = UFS_SG_BLOCK_SIZE_BIT :1370-1371). With +16 a 4-entry read declares a phantom 5th entry
// (boot #23: the split 16 KB READ never completed). The bootloader formula is used.
#define UTP_PRDT_9820_EXTRA  0u

//
// Saved DWORD-granularity DW6/DW7 of every slot we converted to byte granularity,
// a bitmask of which slots are currently converted, and the cached UTRL base.
// Each slot is restored from these the moment its doorbell bit clears.
//
STATIC UINT32  mTrdSaveDw6[32];
STATIC UINT32  mTrdSaveDw7[32];
STATIC UINT32  mTrdSaveMask  = 0;
STATIC UINT32  mUfsLogBudget = 40;   // B81: first 40 data-command ring/done lines only, so BDS retries do not scroll them away

//
// Boot #18/#19 (notes/m8-uefi-image.md B68-B72): READ(10) ends in CHECK CONDITION. Called from
// BOTH restore-on-completion sites BEFORE DW6 goes back to dwords, while DW6[31:16] is still the
// response UPIU's BYTE offset. Response UPIU: Status = byte 7, sense length = bytes 32-33 (BE),
// fixed-format sense from byte 34 (key = [2] & 0xF, ASC = [12], ASCQ = [13]); CDB opcode = Command
// UPIU byte 16. Prints only for SCSI status 0x02 (CHECK CONDITION).
//
//
// Exynos reports OCS 0x7 (FATAL_ERROR) whenever the DEVICE answers a SCSI command with a
// non-GOOD status, and the device then also sets the UPIU Response byte to 0x01. Samsung's
// 9820 kernel ignores both: ufshcd_transfer_rsp_status handles "case OCS_SUCCESS: case
// OCS_FATAL_ERROR:" alike and takes only the SCSI status byte from the response UPIU
// (LineageOS android_kernel_samsung_exynos9820 drivers/scsi/ufs/ufshcd.c ~5856-5880). Stock
// EDK2 fails such a command outright (UfsPassThruHci.c:1605-1610 "Target Failure", then
// Ocs != 0), so ScsiDisk never sees the CHECK CONDITION it would retry - boot #20 showed it
// on the power-on UNIT ATTENTION (key 6 / ASC 0x29). Present the completion the way the
// kernel reads it: for a SCSI Response UPIU (transaction code 0x21), OCS 0x7 -> 0, and a
// non-GOOD status clears the Response byte so status + sense reach ScsiDisk (B75).
//
STATIC
VOID
Beyond0LteOcsFatalFixup (
  IN UINTN  Trd,
  IN UINTN  SResp
  )
{
  UINT32  Dw2;

  if ((MmioRead8 (SResp) & 0x3Fu) != 0x21u) {
    return;
  }

  Dw2 = MmioRead32 (Trd + UTP_TRD_OCS_DW2);
  if ((Dw2 & 0xFFu) == 0x07u) {
    MmioWrite32 (Trd + UTP_TRD_OCS_DW2, Dw2 & ~0xFFu);
  }

  if ((MmioRead8 (SResp + 7) != 0) && (MmioRead8 (SResp + 6) != 0)) {
    MmioWrite8 (SResp + 6, 0);
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
Beyond0LteLogCheckCond (
  IN UINT32  Slot,
  IN UINTN   Trd
  )
{
  UINT64  SUcd;
  UINTN   SResp;

  SUcd  = (UINT64)(MmioRead32 (Trd + UTP_TRD_UCDBA_DW4) & 0xFFFFFF80u);
  SUcd |= (UINT64)MmioRead32 (Trd + UTP_TRD_UCDBAU_DW5) << 32;
  if (SUcd == 0) {
    return;
  }

  SResp = (UINTN)SUcd + (UINTN)(MmioRead32 (Trd + UTP_TRD_RESP_DW6) >> 16);
  {
    UINT8  DOp = MmioRead8 ((UINTN)SUcd + 16);

    if (((DOp == 0x28) || (DOp == 0x2A) || (DOp == 0x88) || (DOp == 0x8A)) && (mUfsLogBudget != 0)) {
      mUfsLogBudget--;
      DEBUG ((
        DEBUG_ERROR,
        "UfsHc: done s%u cdb 0x%02x ocs 0x%x resp 0x%x st 0x%x resid %u\n",
        Slot,
        (UINT32)DOp,
        MmioRead32 (Trd + UTP_TRD_OCS_DW2) & 0xFFu,
        (UINT32)MmioRead8 (SResp + 6),
        (UINT32)MmioRead8 (SResp + 7),
        ((UINT32)MmioRead8 (SResp + 12) << 24) | ((UINT32)MmioRead8 (SResp + 13) << 16) |
        ((UINT32)MmioRead8 (SResp + 14) << 8) | (UINT32)MmioRead8 (SResp + 15)
        ));
    }
  }
  if (MmioRead8 (SResp + 7) != 0x02) {
    Beyond0LteOcsFatalFixup (Trd, SResp);
    return;
  }

  DEBUG ((
    DEBUG_ERROR,
    "UfsHc: slot %u CDB 0x%02x CHECK COND: ocs 0x%x resp 0x%x senselen %u key 0x%x asc 0x%02x ascq 0x%02x\n",
    Slot,
    (UINT32)MmioRead8 ((UINTN)SUcd + 16),
    MmioRead32 (Trd + UTP_TRD_OCS_DW2) & 0xFFu,
    (UINT32)MmioRead8 (SResp + 6),
    ((UINT32)MmioRead8 (SResp + 32) << 8) | MmioRead8 (SResp + 33),
    (UINT32)(MmioRead8 (SResp + 34 + 2) & 0x0Fu),
    (UINT32)MmioRead8 (SResp + 34 + 12),
    (UINT32)MmioRead8 (SResp + 34 + 13)
    ));
  Beyond0LteOcsFatalFixup (Trd, SResp);
}
STATIC UINT64  mTrlBaseCache = 0;

//
// Count of transfer doorbell rings (transfers issued). Surfaced in diag[6] so the
// panel shows how far UFS enumeration progressed.
//
STATIC UINT32  mXferCount = 0;

//
// First-QUERY capture: the UCD (command UPIU) and response-UPIU addresses of the
// first QUERY transfer rung, plus its slot. Set once at the ring; then the doorbell
// poll path samples the live bytes there to see whether the controller ever writes
// the query response (response UPIU header byte0 == 0x36 = written; 0x00 = never).
// This is the decisive test of "does the query truly complete" vs "OCS=0 is stale".
//
STATIC UINT64  mQueryCmdAddr  = 0;
#if BEYOND0LTE_UFS_DIAG
STATIC UINT64  mQueryRespAddr = 0;
STATIC UINT8   mQuerySlot     = 0xFFu;
#endif

//
// UIC error-code accumulators sampled during the stuck doorbell poll. *Accum bits OR in
// every UEC bit ever seen (read-to-clear, so we must accumulate); *Count increments each
// poll a non-zero PA error is seen (high count => the PHY is CONTINUOUSLY line-resetting
// during the stall = active failure; count ~1 => latched once at link startup).
//
STATIC UINT32  mUecPaAccum = 0;
STATIC UINT32  mUecPaCount = 0;
STATIC UINT32  mUecDlAccum = 0;
STATIC UINT32  mUecTAccum  = 0;

//
// CYCLE 39 timer-free latency: cntpct/cntvct read 0 from our EL1 ctx (RKP hypervisor traps the
// generic-timer system regs), so measure the wait in EDK2 POLLS instead. mCmdRingPoll = the poll
// count snapshot at the last SCSI COMMAND doorbell; the stall path reports (mPollCount-mCmdRingPoll)
// = polls until the first PA line-reset: ~0-10 = immediate reject, thousands+ = the read waited.
//
#if BEYOND0LTE_UFS_DIAG
STATIC UINT32  mCmdRingPoll = 0;
#endif
STATIC UINT32  mCmd2PaUs    = 0;
STATIC UINT32  mClr2PaUs    = 0;

//
// UTRLCLR force-clear bookkeeping. mUtrlClrCount = how many 0x5C clear passes we ran,
// mUtrlClrLast = the last bit-mask written, mUtrlMin = the LOWEST UTRLDBR value ever seen
// after a clear pass (init 0xFFFFFFFF). If mUtrlMin ever drops below the piled-retry value
// (e.g. < 0x1F, and especially toward 0), UTRLCLR is genuinely retiring doorbell bits.
//
STATIC UINT32  mUtrlClrCount = 0;
STATIC UINT32  mUtrlClrLast  = 0;
STATIC UINT32  mUtrlMin      = 0xFFFFFFFFu;

//
// Read-poll HEARTBEAT bookkeeping. mPollCount = how many times EDK2 has read ANY HC
// register through our hook; the generic heartbeat in UfsHcRead throttles on it.
//
#if BEYOND0LTE_UFS_DIAG || BEYOND0LTE_UFS_SCREEN_HUD
STATIC UINT32  mPollCount  = 0;
#endif

//
// mHookTick: throttle for the heavy doorbell-read hook (run 1-in-64 polls). mListRearmCount:
// bounded count of UTRLRSR re-arms after a transfer-list halt. mStallLatched: freeze the
// waited-slot capture (diag[19..22]) on the FIRST stalling SCSI command so the panel shows a
// real stall instead of the post-failure poison.
//
STATIC UINT32   mHookTick       = 0;
STATIC UINT32   mListRearmCount = 0;
STATIC BOOLEAN  mStallLatched   = FALSE;


STATIC
VOID
Beyond0LteSetNexusForDoorbell (
  IN UINT32  DoorbellVal
  )
{
  UINT32  Slot;
  UINT64  TrlBase;
  UINTN   Trd;
  UINT64  Ucd;
  UINT8   TxType;
  UINT32  Nexus;

  if (DoorbellVal == 0) {
    return;
  }

  //
  // *** A1: ONE-SHOT IS.UE CLEAR at the FRESH first-doorbell moment (DEVICE-PROVEN SAFE). ***
  // Device result: at the fresh first doorbell (mXferCount==0, the NOP ring, link up, no
  // transfer ever rung), W1C of IS survived and cleared UE (0x404 -> 0x400). BUT extending
  // this to EVERY doorbell HUNG at the query's ring (mXferCount==1): by then the NOP has
  // completed, UTRCS is set with a transfer in flight, and writing IS wedges (same class as
  // all prior post-NOP IS-write hangs). So the SAFE window is ONLY the very first doorbell.
  // We keep just that one-shot UE drain (harmless, panel-reaching) and do NOT write IS on
  // later doorbells. The per-transfer completion edge must come from elsewhere (see the
  // read-poll path), not a pre-ring IS write. diag[36]=IS before, diag[37]=IS after.
  //
  if (mXferCount == 0) {
    UfsHcDiagSet (36, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_IS_OFF));
    UfsHcDiagOr  (13, UFS_PH_ISPROBE_PRE);
    MmioWrite32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_IS_OFF, 0x00000004u);  // W1C UE only
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    UfsHcDiagOr  (13, UFS_PH_ISPROBE_POST);
    UfsHcDiagSet (37, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_IS_OFF));
  }

  //
  // (REMOVED: a per-transfer MmioOr32(HCI base + 0x140, 0x1) that we had labeled
  // "nexus done." It was based on a wrong assumption: 0x140 at the VENDOR base is
  // HCI_DBR_TIMER_CONFIG (a doorbell-register timer), but we wrote it at the STANDARD
  // HCI base (0x11120140 = a reserved/crypto region), and the Samsung fork driver does
  // NO such per-transfer write. Critically it fired ONLY when the previous transfer was
  // a NOP/query, so the first NOP rang WITHOUT it but the first device-init query rang
  // WITH it — a concrete difference between the transfer that retires its doorbell and
  // the one that does not. Removing it to test whether this write is what blocks the
  // query's doorbell-retire.)
  //

  //
  // Lowest set slot bit = the transfer being rung this doorbell.
  //
  for (Slot = 0; Slot < 32; Slot++) {
    if ((DoorbellVal & (1u << Slot)) != 0) {
      break;
    }
  }

  //
  // UTP Transfer Request List base (physical == virtual; DRAM identity-mapped).
  //
  TrlBase  = (UINT64)MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRLBA_OFF);
  TrlBase |= (UINT64)MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRLBAU_OFF) << 32;
  if (TrlBase == 0) {
    return;
  }

  mTrlBaseCache = TrlBase;   // for the restore-on-completion path in UfsHcRead

  //
  // First-doorbell register snapshot: capture IS (0x20) + HCS (0x30) the instant
  // the very first transfer is rung, so the panel shows the controller's interrupt
  // and host status AT THAT MOMENT (the panel's own IS/HCS are read post-stall, much
  // later). diag[17]=IS@first-doorbell, diag[18]=HCS@first-doorbell.
  //
  UfsHcDiagOr (13, UFS_PH_DOORBELL);
  if (mXferCount == 0) {
    UfsHcDiagSet (17, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x20u));
    UfsHcDiagSet (18, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x30u));
  }

  //
  // UTRD for this slot -> UTP Command Descriptor base. EDK2 stores UcdBa =
  // (addr >> 7) in DW4 bits[31:7], so (DW4 & 0xFFFFFF80) recovers addr[31:7];
  // DW5 is addr[63:32].
  //
  Trd  = (UINTN)TrlBase + (UINTN)Slot * UTP_TRD_SIZE;
  Ucd  = (UINT64)(MmioRead32 (Trd + UTP_TRD_UCDBA_DW4) & 0xFFFFFF80u);
  Ucd |= (UINT64)MmioRead32 (Trd + UTP_TRD_UCDBAU_DW5) << 32;
  if (Ucd == 0) {
    return;
  }
#if BEYOND0LTE_CONVERT_UTRD_BYTE_GRAN
  //
  // PRDT_BYTE_GRAN: convert this slot's DW6/DW7 to Exynos byte granularity and
  // stash the original DWORD values for restore-on-completion (see note above).
  // Response length/offset and PRDT offset are dword counts (*4 -> bytes); PRDT
  // length is an entry count (*sizeof(UTP_TR_PRD) -> bytes). Each half is scaled
  // independently so no carry crosses the 16-bit field boundary.
  //
  {
    UINT32  Dw6   = MmioRead32 (Trd + UTP_TRD_RESP_DW6);
    UINT32  Dw7   = MmioRead32 (Trd + UTP_TRD_PRDT_DW7);
    UINT16  RuL   = (UINT16)(Dw6 & 0xFFFFu);   // response UPIU length (dwords)
    UINT16  RuO   = (UINT16)(Dw6 >> 16);        // response UPIU offset (dwords)
    UINT16  PrdtL = (UINT16)(Dw7 & 0xFFFFu);    // PRDT length (entry count)
    UINT16  PrdtO = (UINT16)(Dw7 >> 16);        // PRDT offset (dwords)

    mTrdSaveDw6[Slot] = Dw6;
    mTrdSaveDw7[Slot] = Dw7;
    mTrdSaveMask     |= (1u << Slot);

    //
    // Response length. With a PRDT the response region ends where the PRDT starts, so
    // keep EDK2's exact length. Without one (NOP / query), give the device the room the
    // 9820 kernel always gives it - ALIGNED_UPIU_SIZE = 1024 B (ufshcd.c:4429-4430) -
    // capped to the UCD's own 4 KB page (UfsAllocateAlignCommonBuffer allocates whole
    // pages): EDK2 sizes a READ DESCRIPTOR response to the length it ASKED for, and a
    // device that returns more trips OCS 4 (boot #19, notes/m8-uefi-image.md B71).
    //
    {
      UINT32  RuOBytes = (UINT32)RuO * 4u;
      UINT32  RuLBytes = (UINT32)RuL * 4u;

      if ((PrdtL == 0) && (RuOBytes < 0x1000u)) {
        RuLBytes = 0x1000u - RuOBytes;
        if (RuLBytes > 1024u) {
          RuLBytes = 1024u;
        }
      }

      MmioWrite32 (
        Trd + UTP_TRD_RESP_DW6,
        ((UINT32)(UINT16)RuOBytes << 16) | (UINT32)(UINT16)RuLBytes
        );
    }
    //
    // Boot #22 (notes/m8-uefi-image.md B79): 4 KB READ(10)s complete, the first 16 KB one
    // resets the phone. EDK2 puts up to 256 KB in one PRDT entry; the 9820 programs
    // PRDT_SET_SIZE(12) (= 2^12?) for TX/RX. Test: rebuild the PRDT as <= 4 KB pieces when
    // the result fits in the UCD's page (UCDs are page-aligned whole-page allocations),
    // else leave it and log. PRDT entry = DW0 addr[31:2]<<2, DW1 addr[63:32], DW3[17:0] = len-1.
    //
    {
      UINT32  NewPrdtL = PrdtL;

      if ((PrdtL != 0) && (PrdtL <= 64u)) {
        STATIC UINT32  mPrdtSplit[UTP_PRDT_MAX_SPLIT * 4];
        UINTN          Prdt   = (UINTN)Ucd + (UINTN)PrdtO * 4u;
        UINT32         Out    = 0;
        UINT32         In;
        UINT32         Dw;
        BOOLEAN        Fits   = TRUE;

        //
        // Read EDK2's 16 B entries, cut them into <= 4 KB pieces (page-aligned chunks), then
        // write them back at the controller's 128 B stride with the FMP dwords zeroed.
        // UCDs come from UfsHcAllocateBuffer with UFS_COMMON_EXTRA_PAGES of slack, so
        // PrdtO + 1024 * 128 stays inside the allocation.
        //
        for (In = 0; (In < PrdtL) && Fits; In++) {
          UINT64  Addr = ((UINT64)MmioRead32 (Prdt + In * 16u + 4u) << 32) |
                         (UINT64)(MmioRead32 (Prdt + In * 16u) & ~3u);
          UINT32  Len  = (MmioRead32 (Prdt + In * 16u + 12u) & 0x3FFFFu) + 1u;

          while (Len != 0) {
            UINT32  Chunk = 0x1000u - (UINT32)(Addr & 0xFFFu);

            if (Chunk > Len) {
              Chunk = Len;
            }

            if (Out >= UTP_PRDT_MAX_SPLIT) {
              Fits = FALSE;
              break;
            }

            mPrdtSplit[Out * 4u + 0] = (UINT32)Addr & ~3u;
            mPrdtSplit[Out * 4u + 1] = (UINT32)(Addr >> 32);
            mPrdtSplit[Out * 4u + 2] = 0;
            mPrdtSplit[Out * 4u + 3] = Chunk - 1u;
            Out++;
            Addr += Chunk;
            Len  -= Chunk;
          }
        }

        if (Fits) {
          for (In = 0; In < Out; In++) {
            for (Dw = 0; Dw < (UTP_TR_PRD_SIZE / 4u); Dw++) {
              MmioWrite32 (Prdt + In * UTP_TR_PRD_SIZE + Dw * 4u, (Dw < 4u) ? mPrdtSplit[In * 4u + Dw] : 0u);
            }
          }

          NewPrdtL = Out;
        } else {
          DEBUG ((DEBUG_ERROR, "UfsHc: PRDT split exceeds %u entries\n", UTP_PRDT_MAX_SPLIT));
        }
      }

      MmioWrite32 (
        Trd + UTP_TRD_PRDT_DW7,
        ((UINT32)(UINT16)(PrdtO * 4u) << 16) |
        (UINT32)(UINT16)((NewPrdtL != 0) ? (NewPrdtL * UTP_TR_PRD_SIZE + UTP_PRDT_9820_EXTRA) : 0)
        );
    }
    __asm__ __volatile__ ("dsb sy" ::: "memory");
  }
#endif

  //
  // Command UPIU is at the start of the UCD; byte 0 bits[5:0] = transaction type.
  //
  TxType = (UINT8)(MmioRead8 ((UINTN)Ucd) & 0x3Fu);

  //
  // Boot #20/#21 reset right after the first TEST UNIT READY's UNIT ATTENTION, with the quiet
  // log showing nothing in between (notes/m8-uefi-image.md B77). Name every SCSI command as it
  // is rung, so the last "UfsHc: ring" line before a reset is the command that triggers it:
  // CDB opcode (Command UPIU byte 16), LUN (byte 2), LBA + blocks for READ/WRITE(10) (CDB 2..5,
  // 7..8 big-endian), and the expected data length (Command UPIU bytes 12..15 BE).
  //
  if ((TxType == UPIU_TXTYPE_COMMAND) &&
      ((MmioRead8 ((UINTN)Ucd + 16) == 0x28) || (MmioRead8 ((UINTN)Ucd + 16) == 0x2A) ||
       (MmioRead8 ((UINTN)Ucd + 16) == 0x88) || (MmioRead8 ((UINTN)Ucd + 16) == 0x8A)) &&
      (mUfsLogBudget != 0))
  {
    UINT8  Op = MmioRead8 ((UINTN)Ucd + 16);
    mUfsLogBudget--;
    DEBUG ((
      DEBUG_ERROR,
      "UfsHc: ring s%u lun%u cdb 0x%02x lba 0x%08x n %u edtl %u\n",
      (UINT32)Slot,
      (UINT32)MmioRead8 ((UINTN)Ucd + 2),
      (UINT32)Op,
      ((UINT32)MmioRead8 ((UINTN)Ucd + 18) << 24) | ((UINT32)MmioRead8 ((UINTN)Ucd + 19) << 16) |
      ((UINT32)MmioRead8 ((UINTN)Ucd + 20) << 8) | (UINT32)MmioRead8 ((UINTN)Ucd + 21),
      ((UINT32)MmioRead8 ((UINTN)Ucd + 23) << 8) | (UINT32)MmioRead8 ((UINTN)Ucd + 24),
      ((UINT32)MmioRead8 ((UINTN)Ucd + 12) << 24) | ((UINT32)MmioRead8 ((UINTN)Ucd + 13) << 16) |
      ((UINT32)MmioRead8 ((UINTN)Ucd + 14) << 8) | (UINT32)MmioRead8 ((UINTN)Ucd + 15)
      ));
  }

  //
  // EXYNOS NEXUS-TYPE, EXACT KERNEL SEMANTICS (exynos_ufs_set_nexus_t_xfer_req,
  // ref/u9810-ufs/ufs-exynos.c:801): the register is initialised to 0xFFFFFFFF at HCE
  // (Beyond0LteHciVendorSetup) and thereafter READ-MODIFY-WRITTEN one slot bit at a time -
  // SET (1<<tag) for a SCSI Command, CLEAR for NOP/Query - preserving every other slot.
  // The earlier code blanket-wrote 0x00000000 on every query (wiping the init value) and
  // 0xFFFFFFFF on every command. NOP/query completed, but the first data-bearing SCSI
  // command sat UNPROCESSED (OCS stuck at 0x0F init). Matching the kernel's per-tag RMW
  // is the documented requirement for the controller to actually execute the command.
  //
  Nexus = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_UTRL_NEXUS_TYPE);
  if (TxType == UPIU_TXTYPE_COMMAND) {
    Nexus |= (1u << Slot);
  } else {
    Nexus &= ~(1u << Slot);
  }

  //
  // Phase trail: record which transaction types have been issued. NOP=first init
  // handshake, QUERY=device descriptor/flag/attr reads, COMMAND=SCSI (disk layer).
  //
  if (TxType == UPIU_TXTYPE_NOP) {
    UfsHcDiagOr (13, UFS_PH_NOP);
  } else if (TxType == UPIU_TXTYPE_QUERY) {
    UfsHcDiagOr (13, UFS_PH_QUERY);

    //
    // First QUERY: record its command + response UPIU addresses so the doorbell
    // poll path can watch whether the controller ever writes the response. The
    // response UPIU sits at UCD + RuO bytes; we just byte-converted DW6, so its
    // high 16 bits ARE the response byte offset (RuO_dword * 4).
    //
    if (mQueryCmdAddr == 0) {
      #if BEYOND0LTE_UFS_DIAG
      UINT32  ConvDw6 = MmioRead32 (Trd + UTP_TRD_RESP_DW6);
      #endif
      mQueryCmdAddr  = Ucd;
      #if BEYOND0LTE_UFS_DIAG
      mQueryRespAddr = Ucd + (UINT64)(ConvDw6 >> 16);
      mQuerySlot     = (UINT8)Slot;
      #endif
    }
  } else if (TxType == UPIU_TXTYPE_COMMAND) {
    UfsHcDiagOr (13, UFS_PH_COMMAND);
    //
    // CYCLE 39: snapshot the EDK2 poll count at the SCSI COMMAND doorbell (timer-free).
    //
    #if BEYOND0LTE_UFS_DIAG
    mCmdRingPoll = mPollCount;
    #endif
    //
    // *** CYCLE 60: capture the LIVE CDB of a READ command at RING time (freshly built by EDK2, NOT a
    // churned/abandoned stuck slot). Only for READ(10)=0x28 / READ(16)=0x88, so the last value latched is
    // the media READ (not a later INQUIRY to another LUN). D[17] (panel cdb0) = CDB[0..3] @ Ucd+16 = opcode
    // + flags + LBA[31:16]. D[18] (panel cdbL) = CDB[6..9] @ Ucd+22 = group | len_hi<<8 | len_lo<<16 |
    // control<<24, so transfer-length blocks = (cdbL>>8 & 0xFF)<<8 | (cdbL>>16 & 0xFF). A 1-block read =>
    // cdbL=0x00010000. cdbL=0 => transfer length 0 (vs EDTL 4096) = MALFORMED host-side CDB. Nonzero =>
    // the READ is valid and the device is REFUSING it (device-side).
    //
    {
      UINT8  Op = MmioRead8 ((UINTN)Ucd + 16u);
      if ((Op == 0x28u) || (Op == 0x88u)) {
        UfsHcDiagSet (17, MmioRead32 ((UINTN)Ucd + 16u));
        UfsHcDiagSet (18, MmioRead32 ((UINTN)Ucd + 22u));
        //
        // *** CYCLE 77: capture the LIVE READ's EDTL + PRDT ENTRY at RING time (freshly built by EDK2,
        // CORRECT slot - unlike the cycle-76 stuck-poll which caught a zeroed/abandoned retry). DW7 is
        // already byte-gran converted above, so PRDT = Ucd + (DW7[31:16] byte offset). des0(DW0)=data
        // addr lo (= bounce buffer, MUST be non-zero), des3(DW3) = [31:30]DAS [29:28]FAS [25:0]LENGTH.
        // D[43]=des0 (0 => the data addr never made it into the PRDT = PRDT-build bug), D[44]=EDTL @Ucd+12
        // (expect 0x00100000 = 4096 BE; 0 => EDK2 requested no data), D[45]=des3 (expect 0x00000FFF =>
        // DAS/FAS=0 bypass + len 4095; top nibble != 0 => FMP engages = bug). This is the DEFINITIVE check
        // of whether EDK2 hands the controller a valid READ descriptor.
        //
        {
          UINTN  RingPrd = (UINTN)Ucd + (UINTN)(MmioRead32 (Trd + UTP_TRD_PRDT_DW7) >> 16);

          UfsHcDiagSet (43, MmioRead32 (RingPrd + 0u));
          UfsHcDiagSet (44, MmioRead32 ((UINTN)Ucd + 12u));
          UfsHcDiagSet (45, MmioRead32 (RingPrd + 12u));
        }
      }
    }
  }

  MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_UTRL_NEXUS_TYPE, Nexus);
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  //
  // DIAG[6]: count doorbell rings (transfers issued). Lets the panel show how far
  // enumeration progressed: ~5-6 = stuck at the first device-init query; 20-40+ =
  // device init / SCSI enumeration is advancing.
  //
  mXferCount++;
  UfsHcDiagSet (6, mXferCount);

  //
  // DIAG[8] repurposed during transfers: low byte = txtype, next byte = nexus!=0.
  // (Vendor-setup REORDER readback already proved the VS block writable.)
  //
  UfsHcDiagSet (8, ((TxType == UPIU_TXTYPE_COMMAND) ? 0x100u : 0x000u) | TxType);

  //
  // BREADCRUMB: keep early NOP/query rings visible, but leave SCSI commands quiet
  // after storage is up so higher-level FAT/Boot Services overlays survive freezes.
  //
  if (TxType != UPIU_TXTYPE_COMMAND) {
    Beyond0LteCrumb (
      (TxType == UPIU_TXTYPE_NOP)   ? "RING NOP" :
      (TxType == UPIU_TXTYPE_QUERY) ? "RING QRY" : "RING ???"
      );
  }
}

#if UFS_DO_PMC
//
// *** CYCLE 44: kernel-style LINERESET (generic PA error) RECOVERY. ***
// The Linux UFS error handler (drivers/ufs/core/ufshcd.c) treats uPA=0x80000010 -
// UFSHCD_UIC_PA_GENERIC_ERROR, which its own comment calls "the LINERESET indication" - as
// NON-FATAL: it clears the error, recognizes a LINERESET may have dropped the link back to PWM,
// RESTORES the High-Speed power mode (re-runs the power-mode change), and RETRIES the command,
// WITHOUT a host reset. EDK2 has no such recovery, so our first LBA-data READ line-resets and stays
// dead while the stock kernel reads the same device fine. This routine replays the HS bring-up
// (HS M-PHY tuning -> DL/PA flow-control timers -> PA gear/lane attrs -> DME_SET PA_PWRMode=Fast ->
// wait UPMS -> AFC/CDR lock), mirroring PostLinkStartup, so the halted UTP list can be re-armed onto
// a restored HS link and the stuck READ retried. The restore status is stashed in mRePmcResult for
// the panel. Callable from the doorbell hook (re-entrancy guarded by mInRecovery).
//
STATIC
VOID
Beyond0LteRecoverHsLink (
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *DrvIf
  )
{
  UINT32  HsLane;
  UINT32  HsT;
  UINT32  Upms;
  UINT32  Spin;
  UINT32  AfcLock = 0;
  UINT32  PwrNew;

  if ((DrvIf == NULL) || (DrvIf->UfsExecUicCommand == NULL)) {
    return;
  }

  //
  // (1) Re-apply the HS M-PHY analog tuning (calib_of_hs_rate_a, Gear 3) to the active lane(s).
  // The PMA survives a UniPro LINERESET, but re-writing the same values is idempotent.
  //
  for (HsLane = 0; HsLane < UFS_HS_LANES; HsLane++) {
    HsT = HsLane * UFS_PMA_TRSV_STRIDE;
    Beyond0LtePmaWr (0x0C8 + HsT, 0xBC);
    Beyond0LtePmaWr (0x0F0 + HsT, 0x7F);
    Beyond0LtePmaWr (0x120 + HsT, 0xC0);
    Beyond0LtePmaWr (0x128 + HsT, 0x00);
    Beyond0LtePmaWr (0x134 + HsT, 0x63);
  }
#if (UFS_HS_LANES < 2)
  Beyond0LtePmaWr (0x0C4 + UFS_PMA_TRSV_STRIDE, 0x19);
  Beyond0LtePmaWr (0x0E8 + UFS_PMA_TRSV_STRIDE, 0xFF);
#endif

  //
  // (2) Re-assert the DL/PA flow-control timers (a LINERESET re-inits the UniPro layer to its
  // PWM-G1 defaults) as both the UniPro DME attributes AND the reg_unipro hardware-timer rows.
  //
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x2041u << 16, 8064);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x2042u << 16, 28224);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x2043u << 16, 20160);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x15B0u << 16, 12000);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x15B1u << 16, 32000);
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x15B2u << 16, 16000);
  MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x7888u, 8064);
  MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x788Cu, 28224);
  MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x7890u, 20160);
  MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x78B8u, 12000);
  MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x78BCu, 32000);
  MmioWrite32 ((UINTN)UFS_UNIPRO_BASE + 0x78C0u, 16000);

  //
  // (3) Re-program the target HS PA attributes, then trigger the power-mode change back to HS.
  //
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x1560u << 16, UFS_HS_LANES);  // PA_ActiveTxDataLanes
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x1580u << 16, UFS_HS_LANES);  // PA_ActiveRxDataLanes
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x1569u << 16, 1);             // PA_TxTermination (HS)
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x1584u << 16, 1);             // PA_RxTermination (HS)
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x156Au << 16, 2);             // PA_HSSeries = Rate B (cyA0: match working kernel)
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x1568u << 16, 3);             // PA_TxGear = 3
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x1583u << 16, 3);             // PA_RxGear = 3
  Beyond0LteDme (DrvIf, UFS_UIC_DME_SET, 0x1571u << 16, 0x11);          // PA_PWRMode = Fast/Fast

  //
  // (4) Wait for the power-mode-change completion (IS bit4 UPMS), bounded ~1s.
  //
  Upms = 0;
  for (Spin = 0; Spin < 100000; Spin++) {
    Upms = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x20u) & 0x10u;
    if (Upms != 0) {
      break;
    }
    gBS->Stall (10);
  }

  //
  // (5) post-PMC CDR/AFC lock wait per active lane (poke 0xF0 to re-trigger AFC).
  //
  for (HsLane = 0; HsLane < UFS_HS_LANES; HsLane++) {
    HsT = HsLane * UFS_PMA_TRSV_STRIDE;
    for (Spin = 0; Spin < 100; Spin++) {
      gBS->Stall (40);
      if ((Beyond0LtePmaRd (0x1FC + HsT) & 0x40u) == 0x40u) {
        AfcLock |= (1u << HsLane);
        break;
      }
      gBS->Stall (1);
      Beyond0LtePmaWr (0x0F0 + HsT, 0x7F);
      Beyond0LtePmaWr (0x0F0 + HsT, 0xFF);
    }
  }

  //
  // *** CYCLE 9E: pre_h8_exit RX-RESUME re-cal (ref ufs-cal-9810.c pre_h8_exit) applied at the line-reset
  // recovery so the RE-ISSUED read resumes on a freshly RX-re-cal'd PHY. cy9D proved the link trains fine
  // (HS + AFC locked both lanes) yet the first real-NAND read line-resets when the device pauses mid-burst
  // for a NAND fetch and the RX cannot re-acquire. cy57 applied these STATICALLY at init (no effect); the
  // kernel applies them AT the resume (h8-exit), which is exactly what this recovery is. COMN 0x004=0x3F
  // (once) + per-active-lane TRSV 0x0C4=0xD9 / 0x0E8=0x77 / 0x0F0=0xFF.
  //
  Beyond0LtePmaWr (0x004, 0x3F);
  for (HsLane = 0; HsLane < UFS_HS_LANES; HsLane++) {
    HsT = HsLane * UFS_PMA_TRSV_STRIDE;
    Beyond0LtePmaWr (0x0C4 + HsT, 0xD9);
    Beyond0LtePmaWr (0x0E8 + HsT, 0x77);
    Beyond0LtePmaWr (0x0F0 + HsT, 0xFF);
  }

  //
  // *** CYCLE A7: settle to the working kernel's clean READ resting state (on-device dump): COMN 0x004=0x00 and
  // per-active-lane TRSV 0x0F0=0x7F. The transient pre_h8_exit edges above (0x3F/0xFF) re-cal the RX; resting at
  // 0x0F0=0x7F (bit7=0) leaves the CDR free to re-adapt for the re-issued read instead of frozen at 0xFF.
  //
  Beyond0LtePmaWr (0x004, 0x00);
  for (HsLane = 0; HsLane < UFS_HS_LANES; HsLane++) {
    Beyond0LtePmaWr (0x0F0 + HsLane * UFS_PMA_TRSV_STRIDE, 0x7F);
  }

  //
  // Stash the restore status for the panel (diag[36] "rePMC", surfaced by the LIST-HALT RECOVERY
  // block): [23:16]=AFC lock per lane, [8]=UPMS seen, [7:0]=PA_PWRMode after restore (0x11 = HS
  // re-established = the recovery's PMC succeeded).
  //
  PwrNew       = Beyond0LteDme (DrvIf, UFS_UIC_DME_GET, 0x1571u << 16, 0) & 0xFFu;
  mRePmcResult = ((AfcLock & 0xFFu) << 16) | ((Upms != 0) ? 0x100u : 0u) | PwrNew;
}
#endif

//
// EXYNOS DOORBELL FORCE-CLEAR via UTRLCLR (0x5C). On-device proof: every query transfer
// reports OCS=0x00 in its TRD, yet the controller never auto-clears the UTRLDBR bit
// (UTRCS stuck at 1 -> no fresh completion edge; clearing UTRCS via the IS register
// wedges this controller, and the run-stop(0x60) toggle is a no-op here). The kernel's
// escape hatch for a doorbell that will not auto-clear is ufshcd_clear_cmd -> write
// (1<<slot) to UTRLCLR (the (1<<slot) polarity is REQUIRED because Exynos sets
// UFSHCD_QUIRK_BROKEN_REQ_LIST_CLR, which inverts this register; it is NOT unusable) then
// wait UTRLDBR[slot]->0. UTRLCLR is a DIFFERENT register than IS, so it is not subject to
// the IS-write wedge. This hook force-clears, for the slot EDK2 is waiting on, ONLY once
// the controller has truly finished it (OCS(DW2) != 0x0F init), so we never abort a live
// transfer. No permanent per-slot mask: a later query that reuses the slot re-arms the
// clear after it completes. If the real bit clears, EDK2 observes completion AND slot
// reuse stays valid (unlike spoofing the read value, which left the real bit stuck and
// hung deeper). diag[34]=UTRLDBR before, diag[35]=UTRLDBR after, diag[38]=last (1<<slot).
//
STATIC
VOID
Beyond0LteFixupDoorbellRead (
  IN OUT UINT32  *DoorbellPtr
  )
{
  UINT32   Real;
  UINT32   Diag;
  UINT32   Slot;

  if (mTrlBaseCache == 0) {
    return;
  }

  //
  // CYCLE 44: re-entrancy guard. The LINERESET recovery below issues DME/UIC commands via the
  // driver interface, which read HC registers through this same protocol path; never recurse into
  // the stall/recovery logic while a recovery is mid-flight.
  //
  if (mInRecovery) {
    return;
  }

  //
  // THROTTLE: this hook scans up to 32 TRD slots + reads UEC/clock/UPIU state on EVERY
  // doorbell poll (~50 MMIO ops), which dominated the multi-minute polls. It is OBSERVE-ONLY
  // (never writes *DoorbellPtr), so running it 1-in-64 polls still retires a completed
  // doorbell within ~64 fast polls (sub-millisecond) while the other 63 return immediately.
  // (Exynos command/query doorbells never auto-clear - proven by the UTRLCLR saga - so no
  // byte-gran-restore race: a slot's DW6/DW7 are restored in the same pass we retire it.)
  //
  mHookTick++;
  if ((mHookTick & 0x3Fu) != 0) {
    return;
  }

  Real  = *DoorbellPtr;
  Diag  = 0;

  //
  // Pack OCS of slots 0..3 for the panel ([31:24]s3 .. [7:0]s0), and restore byte-gran
  // DW6/DW7 for any converted slot whose REAL doorbell bit has cleared (genuine
  // completion) so EDK2's response parse lands on the right addresses.
  //
  for (Slot = 0; Slot < 32; Slot++) {
    UINTN   Trd = (UINTN)mTrlBaseCache + (UINTN)Slot * UTP_TRD_SIZE;

    if (Slot < 4) {
      Diag |= ((MmioRead32 (Trd + UTP_TRD_OCS_DW2) & 0xFFu) << (Slot * 8));
    }

    if (((Real & (1u << Slot)) == 0) && ((mTrdSaveMask & (1u << Slot)) != 0)) {
      Beyond0LteLogCheckCond ((UINT32)Slot, Trd);
      MmioWrite32 (Trd + UTP_TRD_RESP_DW6, mTrdSaveDw6[Slot]);
      MmioWrite32 (Trd + UTP_TRD_PRDT_DW7, mTrdSaveDw7[Slot]);
      mTrdSaveMask &= ~(1u << Slot);
    }

    //
    // CYCLE 47: did a SHRUNK (512B) re-issued READ actually COMPLETE? A re-issued slot whose OCS
    // left 0x0F (init) means the controller finished it = the device ANSWERED the small read =>
    // SIZE was the wall. Accumulate (the throttle could otherwise miss a brief completion):
    // mReissueDoneMask[15:0] = completed re-issued slots, [23:16] = OCS of the first (0x00=success).
    //
    if ((mReissueMask & (1u << Slot)) != 0) {
      UINT32  Rocs = MmioRead32 (Trd + UTP_TRD_OCS_DW2) & 0xFFu;
      if (Rocs != UTP_TRD_OCS_INIT) {
        if ((mReissueDoneMask & 0xFFFFu) == 0) {
          mReissueDoneMask |= (Rocs << 16);
        }
        mReissueDoneMask |= (1u << Slot);
      }
    }
  }

  //
  // UTRLCLR no-op-vs-works DECIDER (observe-only, render-guaranteed, cannot hang).
  // Cycle 4 cleared ONE bit (slot 0) and its immediate re-read still showed 0x1F - but
  // that is inconclusive: EDK2 retries the query onto slots 1..4 (UTRLDBR piles to 0x1F),
  // so it WAITS on the HIGHEST set bit while we cleared the LOWEST, and the re-read may be
  // too early to see an async clear. This build settles it WITHOUT any chance of hanging:
  //   (1) every poll, clear EVERY completed (OCS != 0x0F) set bit, one (1<<slot) write per
  //       bit (exact kernel ufshcd_utrl_clear semantics, Exynos inverted polarity);
  //   (2) track mUtrlMin = lowest UTRLDBR ever seen after a clear pass;
  //   (3) OBSERVE-ONLY: NEVER modify *DoorbellPtr. EDK2 sees only the REAL register, so if
  //       UTRLCLR truly retires bits EDK2 advances on its own; if not, EDK2 fail-fasts
  //       (150 ms cap) and BDS renders. No fake values => no slot-reuse wedge (the likely
  //       cycle-1 hang cause); writes proven safe in cycle 3 => this build CANNOT hang.
  // READ: utrlA(=mUtrlMin) < 0x1F  => UTRLCLR WORKS (retires bits) -> next build drives it
  //       to completion; utrlA == 0x1F (unchanged) => UTRLCLR is a genuine NO-OP on this
  //       controller (like run-stop) => no register-level doorbell clear exists => A2.
  //
  if (Real != 0) {
    UINT32  CompletedMask;
    UINT32  InFlightCmd;
    UINT32  NexusReg;
    UINT32  Slot2;

    if (mUtrlClrCount == 0) {
      UfsHcDiagSet (34, Real);   // utrlB: doorbell at the first clear pass (expect 0x1F)
    }

    CompletedMask = 0;
    InFlightCmd   = 0;
    NexusReg      = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_UTRL_NEXUS_TYPE);
    for (Slot2 = 0; Slot2 < 16; Slot2++) {
      if ((Real & (1u << Slot2)) != 0) {
        UINTN  T = (UINTN)mTrlBaseCache + (UINTN)Slot2 * UTP_TRD_SIZE;
        if ((MmioRead32 (T + UTP_TRD_OCS_DW2) & 0xFFu) != UTP_TRD_OCS_INIT) {
          CompletedMask |= (1u << Slot2);
        } else if ((NexusReg & (1u << Slot2)) != 0) {
          InFlightCmd |= (1u << Slot2);   // OCS=0x0F + command nexus => a SCSI COMMAND is in-flight
        }
      }
    }

    //
    // CYCLE 38: do NOT fire UTRLCLR (0x5C) while a SCSI COMMAND is in-flight. Our force-clear is the
    // ONLY non-reference register write we make during the device's NAND-access idle, and the prime
    // suspect for the PA line-reset on the first real LBA read (TActivate/config/mode all refuted).
    // Completed-slot doorbells are still retired between commands (no command in-flight), so the
    // enumeration up to the READ is unaffected; only the in-flight READ window goes quiet.
    //
    if (InFlightCmd != 0) {
      UfsHcDiagOr (13, UFS_PH_CLRSUPPRESS);
    }

    if ((CompletedMask != 0) && (InFlightCmd == 0)) {
      UINT32  After;

      mUtrlClrLast = CompletedMask;
      mUtrlClrCount++;
      UfsHcDiagOr (13, UFS_PH_UTRLCLR);

      //
      // One (1<<slot) write per completed bit (exact kernel semantics), then barrier+re-read.
      //
      for (Slot2 = 0; Slot2 < 16; Slot2++) {
        if ((CompletedMask & (1u << Slot2)) != 0) {
          MmioWrite32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRLCLR_OFF, (1u << Slot2));
        }
      }
      __asm__ __volatile__ ("dsb sy" ::: "memory");

      After = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRLDBR_OFF);
      if (After < mUtrlMin) {
        mUtrlMin = After;
        //
        // BREADCRUMB: a doorbell bit ACTUALLY retired (the live register dropped below the
        // piled value). This is the smoking gun that UTRLCLR genuinely works on this
        // controller. Bounded: mUtrlMin only ever decreases, so this fires <= 16 times.
        //
        Beyond0LteCrumb ("DB-RETIRE");
      }

      UfsHcDiagSet (35, mUtrlMin);   // utrlA: LOWEST doorbell seen after any clear pass
      UfsHcDiagSet (38, (mUtrlClrLast & 0xFFFFu) | (mUtrlClrCount << 16));

      //
      // Restore byte-gran DW6/DW7 for any slot that ACTUALLY retired (so EDK2's eventual
      // real-register read parses the right addresses). Observe-only: *DoorbellPtr untouched.
      //
      for (Slot2 = 0; Slot2 < 16; Slot2++) {
        if (((After & (1u << Slot2)) == 0) && ((mTrdSaveMask & (1u << Slot2)) != 0)) {
          UINTN  T = (UINTN)mTrlBaseCache + (UINTN)Slot2 * UTP_TRD_SIZE;
          Beyond0LteLogCheckCond ((UINT32)Slot2, T);
          MmioWrite32 (T + UTP_TRD_RESP_DW6, mTrdSaveDw6[Slot2]);
          MmioWrite32 (T + UTP_TRD_PRDT_DW7, mTrdSaveDw7[Slot2]);
          mTrdSaveMask &= ~(1u << Slot2);
        }
      }
    } else {
      UfsHcDiagSet (35, mUtrlMin);
    }
  }

  UfsHcDiagSet (7, Diag);

  //
  // (IS-ack removed: writing IS @ 0x20 from this hook HARD-HANGS the controller at
  // "Starting DXE core" \u2014 device-confirmed 5x, including with IE=1 and full kernel
  // config replicated. The IS register is physically undriveable from our chainloaded
  // EL1 context, almost certainly an RKP/EL2 trap on 0x20 writes. Stuck-slot detection
  // is still computed above for diag[7]; we no longer attempt any IS write.)
  //

  //
  // CLOCK-STATE CAPTURE at the stuck poll (read-only; VS-block reads are safe). Tests
  // the clock-gating root-cause model: if the doorbell is stuck because a UFS clock
  // auto-gated when the controller idled, these show it. diag[25]=CLKSTOP_CTRL (stop
  // bits b0 UNIPRO_PCLK..b4 REFCLKOUT; a set bit = that clock GATED), diag[26]=
  // FORCE_HCS (our force-on config; expect 0 if it stuck), diag[27]=UFS_ACG_DISABLE.
  //
  if (Real != 0) {
    UfsHcDiagSet (25, MmioRead32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL));
    UfsHcDiagSet (26, MmioRead32 ((UINTN)UFS_VS_BASE + HCI_FORCE_HCS));
    UfsHcDiagSet (27, MmioRead32 ((UINTN)UFS_VS_BASE + HCI_UFS_ACG_DISABLE));
    UfsHcDiagSet (28, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRIACR_OFF));
    UfsHcDiagSet (29, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_IE_OFF));

    //
    // *** CYCLE 66: force-ungate the UNIPRO data-path clocks during the stuck transfer wait. *** The
    // capture above reads CLKSTOP_CTRL=0x0B at stuck = UNIPRO_PCLK(b0)+UNIPRO_MCLK(b1) GATED (b3 MPHY_APB
    // is expected, managed by the PMA helper). We clear these at setup, so the controller RE-GATED the
    // UniPro clocks during the media read's NAND latency - which would FREEZE the data phase exactly as
    // observed (tiny internal commands complete before gating; a 4KB media read sits long enough to gate).
    // Re-clear the stop bits (0x17, leaving MPHY_APB b3) on every stuck poll to hold the data clocks on
    // through the transfer. Turning a clock ON is the same safe write we already do at setup. WIN signal:
    // the cycle-65 init READ probe (rd0) flips from EFI_TIMEOUT (rdsns hi=0x12) to GOOD (rd0=00000000),
    // and/or partitions>=1; the next diag25 capture should then read ~0x08 (UniPro clocks held ungated).
    //
    {
      UINT32  ClkNow = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL);
      if ((ClkNow & HCI_CLK_STOP_ALL) != 0) {
        MmioWrite32 ((UINTN)UFS_VS_BASE + HCI_CLKSTOP_CTRL, ClkNow & ~HCI_CLK_STOP_ALL);
        __asm__ __volatile__ ("dsb sy" ::: "memory");
      }
    }

    //
    // LIVE UIC ERROR-CODE CAPTURE (read-to-clear; read-only, no HC state change beyond
    // clearing the latched error code, which an ISR does routinely). Tells us whether the
    // PA line-reset error is latched ONCE (count ~1) or re-asserts CONTINUOUSLY during the
    // stall (high count = active PHY failure = incomplete HS calibration). diag[30]=PA bits
    // accumulated, diag[31]=PA non-zero poll count, diag[32]=DL accumulated, diag[33]=TR.
    //
    {
      UINT32  Pa = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UECPA_OFF);
      UINT32  Dl = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UECDL_OFF);
      UINT32  Tr = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UECT_OFF);

      mUecPaAccum |= Pa;
      mUecDlAccum |= Dl;
      mUecTAccum  |= Tr;
      if (Pa != 0) {
        mUecPaCount++;
      }
      //
      // CYCLE 40: capture the SPECIFIC UniPro error that triggers the line-reset. The Exynos UNIPRO
      // block latches the error LAYER (reg_unipro+0xC0: which of PA/DL/N/T/DME) and CODE (+0xC4) -
      // far more specific than the vague "uPA generic 0x10". Both are within our 0x8000 UFS_UNIPRO_BASE
      // map. Keep the FIRST non-zero of each (robust to read-to-clear / timing vs the UECPA latch).
      //
      //
      // CYCLE 43: did the LINK do a HIBERN8 / power-mode transition DURING the read? We do NOT handle
      // hibern8 (the ref has hibern8_notify/prepare PMA tweaks we lack). Read the UNIPRO DME indication
      // RESULT regs (reg_unipro = UFS_UNIPRO_BASE, all within our 0x8000 map): if a hibern8 enter/exit
      // fired mid-read and we never applied the h8-exit PMA cal, the resume fails -> PA line-reset.
      // Non-zero h8ent/h8exit at the stall = THE mechanism (=> implement hibern8 PMA handling).
      //
      mCmd2PaUs = MmioRead32 ((UINTN)UFS_UNIPRO_BASE + 0x78ECu);   // UNIP_DME_PWR_IND_RESULT
      mClr2PaUs = MmioRead32 ((UINTN)UFS_UNIPRO_BASE + 0x00D0u);   // UNIP_DME_DL_FRAME_IND

      UfsHcDiagSet (30, mUecPaAccum);
      UfsHcDiagSet (31, mUecPaCount);
      UfsHcDiagSet (32, mUecDlAccum);
      UfsHcDiagSet (33, mUecTAccum);
      UfsHcDiagSet (34, MmioRead32 ((UINTN)UFS_UNIPRO_BASE + 0x7868u));  // CYCLE43: HIBERN8_ENTER_IND_RESULT
      UfsHcDiagSet (35, MmioRead32 ((UINTN)UFS_UNIPRO_BASE + 0x7878u));  // CYCLE43: HIBERN8_EXIT_IND_RESULT
      UfsHcDiagSet (36, mCmd2PaUs);   // CYCLE43: PWR_IND_RESULT
      UfsHcDiagSet (37, mClr2PaUs);   // CYCLE43: DL_FRAME_IND
      //
      // *** CYCLE 9C: one-shot capture of the FIRST UTP-list-HALT's nature to UNCONTENDED slots (the hook's
      // D[30-35] get clobbered by later polls/completion). D[259]=0xFED1340C = mUecPaAccum (80000010 => PA
      // LINERESET on the data burst); D[260]=0xFED13410 = HIBERN8_ENTER_IND (0x7868); D[261]=0xFED13414 =
      // HIBERN8_EXIT_IND (0x7878). h8e/h8x != 0 => hibern8 fired mid-read => the GPT->FAT idle gap hibernated
      // the link and read 8's h8-exit line-resets (no h8-exit PMA cal) => implement dynamic h8-exit cal.
      //
      {
        STATIC BOOLEAN  mFirstStallCaptured = FALSE;
        if (!mFirstStallCaptured &&
            ((MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x30u) & 0x2u) == 0))
        {
          mFirstStallCaptured                    = TRUE;
#if BEYOND0LTE_UFS_DIAG
          *(volatile UINT32 *)(UINTN)0xFED1340Cu = mUecPaAccum;   // D[259] panel uPA
          *(volatile UINT32 *)(UINTN)0xFED13410u = mUecDlAccum;   // D[260] panel dl (cy9F: UEC data-link - NAC?)
          *(volatile UINT32 *)(UINTN)0xFED13414u = mUecTAccum;    // D[261] panel t  (UEC transport)
          *(volatile UINT32 *)(UINTN)0xFED135C0u = (UINT32)Beyond0LteSmc (SMC_CMD_FMP_SMU_DUMP, 0, SMU_EMBEDDED, 0x08);  // D[368] A121 read-status at fault
          *(volatile UINT32 *)(UINTN)0xFED135C4u = (UINT32)Beyond0LteSmc (SMC_CMD_FMP_SMU_DUMP, 0, SMU_EMBEDDED, 0x108); // D[369] A121 write-status at fault
          *(volatile UINT32 *)(UINTN)0xFED135C8u = (UINT32)Beyond0LteSmc (SMC_CMD_FMP_SMU_DUMP, 0, SMU_EMBEDDED, 0x10);  // D[370] A121 read-security at fault
          *(volatile UINT32 *)(UINTN)0xFED135CCu = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_SMU_ABORT_MATCH_INFO);          // D[371] A121 abort match info
#endif
        }
      }
    }

    //
    // LIST-HALT RECOVERY: a transient Exynos PHY/UIC error during a data burst halts the UTP
    // transfer list (HCS.UTRLRDY bit1 -> 0); the controller then ignores ALL doorbells (OCS
    // frozen at 0x0F) until the list is re-armed. We just read-cleared the UEC* codes above
    // (read-to-clear). Now, if the list shows not-ready, re-assert UTRLRSR=1 (Run) to restart
    // it. UTRLRSR (0x60) writes are EL1-safe here (proven a no-op for retire, so never wedge).
    // Bounded so a genuinely dead link cannot spin forever. diag[28] (panel "iacr") repurposed
    // to UTRLRSR readback; diag[29] (panel "ie") = re-arm count.
    //
    {
      UINT32  HcsNow = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x30u);
      UfsHcDiagSet (28, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRLRSR_OFF));
      if (((HcsNow & 0x2u) == 0) && (mListRearmCount < 0x10000u)) {
#if UFS_DO_PMC
        //
        // *** CYCLE 44: kernel-style LINERESET recovery (restore HS power mode, THEN re-arm). ***
        // The list halted (UTRLRDY=0). If a generic PA LINERESET (uPA=0x80000010) caused it, the
        // link ALSO fell back to PWM, so merely re-arming would re-run the stuck READ on a PWM link
        // (which line-resets again - exactly what we have seen for 40+ cycles). Mirror the Linux err
        // handler: re-establish the HS power mode FIRST; the still-set doorbell bit then makes the
        // controller retry the READ on the restored HS link when we re-arm just below. Bounded;
        // consume the latched uPA so this fires once per line-reset event, not every poll.
        //
        if (((mUecPaAccum & 0x80000010u) == 0x80000010u) &&
            !mInRecovery && (mRecoverCount < 0x100u) && (mSavedDrvIf != NULL))
        {
          UINT32  Db;
          UINT32  Nx;
          UINT32  Reslot;

          mInRecovery = TRUE;
          Beyond0LteCrumb ("LINERESET");
          Beyond0LteRecoverHsLink (mSavedDrvIf);
          mInRecovery = FALSE;
          mRecoverCount++;
          mUecPaAccum = 0;
          UfsHcDiagOr (13, UFS_PH_RECOVER);

          //
          // *** CYCLE 45/46: the missing kernel step - RE-ISSUE the command. *** Cycle 44 PROVED the
          // re-PMC restores HS and stops the line-reset, yet the READ still returned no data/no
          // response (OCS=0x0F): merely re-arming made the controller RESUME a wait for data the
          // device - whose transfer the LINERESET aborted - never resends => deadlock. The Linux err
          // handler RETRIES by re-queuing a FRESH command (ufshcd_clear_cmd -> write (1<<slot) to
          // UTRLCLR, then re-ring). Replicate it. *** CYCLE 46: re-issue ONLY the in-flight SCSI
          // COMMAND (HCI_UTRL_NEXUS_TYPE bit SET = command; CLEAR = NOP/query). Cycle 45 re-rang ALL
          // 15 stuck OCS=0x0F slots (reiss=0xFFFE) - those extra slots are ScsiDisk's abandoned READ
          // retries (Exynos doorbells never auto-clear), and re-ringing 15 at once may wedge the
          // controller/device. Target just the READ's command slot, force-clear its doorbell (Exynos
          // (1<<slot) polarity) and re-ring so the controller re-fetches the UTRD and RE-SENDS the
          // command UPIU on the restored HS link.
          //
          MmioWrite32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRLRSR_OFF, UFS_HC_UTRLRSR_RUN);
          __asm__ __volatile__ ("dsb sy" ::: "memory");
          Db = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRLDBR_OFF);
          Nx = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_UTRL_NEXUS_TYPE);
          //
          // *** CYCLE 48: SIZE TEST, crash-hardened. *** Cycle 47 wrote the UCD/PRDT of EVERY stuck
          // slot and took a Synchronous Exception (data abort, FAR=0xBBD5C3): the 15 stuck slots
          // include EDK2's ABANDONED/churned READ retries whose UCDBA/PRDT-offset are garbage. Now
          // touch ONLY the FIRST (lowest) in-flight slot whose UCD is VALID (aligned DRAM) and whose
          // CDB opcode is really READ(10)=0x28 - the one EDK2 is actually waiting on. Shrink it to one
          // 512B block (EDTL off 12 BE->512; CDB xfer-len off 23..24 BE->1; PRDT DW3 byte-count->511),
          // re-fetch+re-send, then STOP. If the device now answers (done!=0, OCS [23:16]=00), SIZE /
          // multi-frame data-in is the wall; if still OCS=0x0F silent, size is NOT it.
          //
          for (Reslot = 0; Reslot < 32; Reslot++) {
            UINTN  RTrd = (UINTN)mTrlBaseCache + (UINTN)Reslot * UTP_TRD_SIZE;

            if (((Db & (1u << Reslot)) == 0) || ((Nx & (1u << Reslot)) == 0) ||
                ((MmioRead32 (RTrd + UTP_TRD_OCS_DW2) & 0xFFu) != UTP_TRD_OCS_INIT)) {
              continue;
            }
            {
              UINT64  RUcd = (UINT64)(MmioRead32 (RTrd + UTP_TRD_UCDBA_DW4) & 0xFFFFFF80u)
                             | ((UINT64)MmioRead32 (RTrd + UTP_TRD_UCDBAU_DW5) << 32);
              UINTN   RPrd = (UINTN)RUcd + (UINTN)(MmioRead32 (RTrd + UTP_TRD_PRDT_DW7) >> 16);

              if ((RUcd < EXYNOS_DRAM_BASE) || (RUcd >= (EXYNOS_DRAM_BASE + EXYNOS_DRAM_SIZE)) || ((RUcd & 0x7Fu) != 0) ||
                  ((UINT64)RPrd < EXYNOS_DRAM_BASE) || ((UINT64)RPrd >= (EXYNOS_DRAM_BASE + EXYNOS_DRAM_SIZE)) || ((RPrd & 0x3u) != 0) ||
                  (MmioRead8 ((UINTN)RUcd + 16u) != 0x28u)) {
                continue;   // garbage/churned UCD or not a READ(10) - never write to it (cycle-47 crash fix)
              }

              //
              // *** CYCLE 88: re-issue the stuck READ(10) at its ORIGINAL FULL SIZE (no 512B shrink). ***
              // The 128-byte PRDT (cycle 83) makes full media reads complete (OCS=0). The old 512B shrink
              // truncated the GPT partition-ENTRY-ARRAY read (~16KB, 128 entries x 128B) so its CRC32
              // failed and PartitionDxe rejected the whole GPT -> partitions=0. Re-ring the slot AS-IS so
              // the full entry array arrives and its CRC validates. (RPrd is still used by the guard above.)
              //
              MmioWrite32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRLCLR_OFF, (1u << Reslot));
              __asm__ __volatile__ ("dsb sy" ::: "memory");
              MmioWrite32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRLDBR_OFF, (1u << Reslot));
              __asm__ __volatile__ ("dsb sy" ::: "memory");
              mReissueMask |= (1u << Reslot);
              break;   // one clean shrunk re-issue is enough for the size test
            }
          }
        }
#endif
        MmioWrite32 ((UINTN)EXYNOS_UFS_HCI_BASE + UFS_HC_UTRLRSR_OFF, 0x1u);
        __asm__ __volatile__ ("dsb sy" ::: "memory");
        mListRearmCount++;
      }
      UfsHcDiagSet (29, mListRearmCount);
      UfsHcDiagSet (34, mRecoverCount);   // CYCLE 44: # of HS re-PMC recoveries fired (panel "recov")
      UfsHcDiagSet (35, mReissueMask);    // CYCLE 45: slots UTRLCLR+re-rung = command re-issued (panel "reiss")
      UfsHcDiagSet (36, mRePmcResult);    // CYCLE 44: last restore's PA_PWRMode|UPMS|AFC (panel "rePMC")
      UfsHcDiagSet (37, mReissueDoneMask);// CYCLE 47: shrunk(512B) re-issue COMPLETED? [23:16]=OCS(00=ok) [15:0]=slots (panel "done")
    }
  }

  //
  // DECISIVE LIVE WAITED-SLOT CAPTURE: decode, fresh on every poll, the UPIU state
  // of the transfer EDK2 is ACTUALLY waiting on right now (lowest set doorbell bit).
  // This supersedes the old one-shot first-query capture, which pinned to the first
  // query's UCD — a buffer EDK2 ABANDONS on retry (it churns slots, zeroes the old
  // UCD, rebuilds elsewhere), so that read returned stale zeros (Qcmd byte0 was 0x00,
  // never the 0x16 of a live query → the reading was untrustworthy). Reading the
  // currently-waited slot guarantees we sample a LIVE, still-pending transfer:
  //   diag[19] Wcmd  = waited slot CMD UPIU dword0  (byte0 = txtype: 0x16 query / 0x00 NOP)
  //   diag[20] Wresp = waited slot RESP UPIU dword0 (byte0: 0x36 query-resp or 0x20 NOP-resp
  //                    => device WROTE a response = truly completed; 0x00 => never written)
  //   diag[21] Wrsp2 = waited slot RESP UPIU dword2 (response/status area)
  //   diag[22] Wstat = [7:0] slot OCS | [15:8] cmd txtype | bit16 doorbell-still-set
  // For a still-pending slot DW6 holds the byte-gran response offset (we only restore
  // it on real completion), so RuO_bytes = (DW6>>16). Observation-only: no doorbell
  // change, no HC register write — cannot wedge the engine.
  //
  if (Real != 0) {
    UINT32  WSlot;

    //
    // LIVE controller IS/HCS at the stuck poll (read-only — cannot wedge the
    // engine). Tests the IS.UTRCS completion-edge theory: if IS bit0 (UTP
    // transfer completion) is stuck at 1 while the query doorbell never clears,
    // the controller's doorbell-clear is gated on a fresh IS.UTRCS 0->1 edge that
    // EDK2 (which never acks IS) cannot produce. HCS shows whether DevPresent/
    // LinkRdy held during the stall. diag[23]=live IS, diag[24]=live HCS.
    //
    UfsHcDiagSet (23, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x20u));
    UfsHcDiagSet (24, MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x30u));

    for (WSlot = 0; WSlot < 32; WSlot++) {
      if ((Real & (1u << WSlot)) != 0) {
        break;
      }
    }

    if (WSlot < 32) {
      UINTN   WTrd = (UINTN)mTrlBaseCache + (UINTN)WSlot * UTP_TRD_SIZE;
      UINT64  WUcd;

      WUcd  = (UINT64)(MmioRead32 (WTrd + UTP_TRD_UCDBA_DW4) & 0xFFFFFF80u);
      WUcd |= (UINT64)MmioRead32 (WTrd + UTP_TRD_UCDBAU_DW5) << 32;

      if (WUcd != 0) {
        UINT32  WCmd0  = MmioRead32 ((UINTN)WUcd);
        UINT32  WOcs   = MmioRead32 (WTrd + UTP_TRD_OCS_DW2) & 0xFFu;
        UINT32  WTx    = MmioRead8 ((UINTN)WUcd) & 0x3Fu;
        UINT32  WDw6   = MmioRead32 (WTrd + UTP_TRD_RESP_DW6);
        UINT64  WResp  = WUcd + (UINT64)(WDw6 >> 16);

        //
        // CYCLE 53/59: buffer confirmed all-zero (no data on a clean link). Pinpoint WHY the device is
        // SILENT on a READ(10) LBA0 - malformed command vs device-drop:
        //   diag[20] (panel "EDTL") = cmd UPIU Expected Data Transfer Length (WUcd+12, big-endian, so
        //                             4096 -> 0x00100000). 0 => EDK2 requested NO data = malformed cmd.
        //   diag[21] (panel "resp") = device RESPONSE UPIU dword0 at WUcd+(byte-gran DW6>>16). 0 => the
        //                             device sent NO response (truly silent); nonzero (byte0~0x21) =>
        //                             it ANSWERED with status/sense => decode the error.
        //   diag[19] Wcmd=CMD UPIU dword0 ; diag[22]=OCS.
        //   *** CYCLE 59: the ACTUAL SCSI CDB (never confirmed in 58 cycles - Wcmd is only the UPIU header).
        //   diag[17] (panel "cdb0") = CDB[0..3] @ WUcd+16 = SCSI opcode + LBA[31:16]; READ(10) => low byte
        //   0x28. diag[18] (panel "cdb8") = CDB[8..11] @ WUcd+24 = transfer-length lo + control; a 1-block
        //   read => low byte 0x01. cdb0=...28 + cdb8=...01 => well-formed READ(10) LBA0 1-block => the
        //   device is REFUSING a valid read (device-side). A weird opcode/len => a host-side CDB bug. ***
        //
        if (!mStallLatched) {
          UfsHcDiagSet (19, WCmd0);
          UfsHcDiagSet (20, MmioRead32 ((UINTN)WUcd + 12u));
          UfsHcDiagSet (21, ((WResp >= EXYNOS_DRAM_BASE) && (WResp < (EXYNOS_DRAM_BASE + EXYNOS_DRAM_SIZE))) ? MmioRead32 ((UINTN)WResp) : 0xDEAD0002u);
          UfsHcDiagSet (22, WOcs | (WTx << 8) | 0x10000u);
          // (CYCLE 60: stuck-slot CDB capture REMOVED - the lowest stuck doorbell slot can be a churned/
          // abandoned retry. The LIVE CDB is now captured at RING time in Beyond0LteSetNexusForDoorbell
          // (panel cdb0/cdbL), which is reliable. CYCLE 76->77: the stuck-slot PRDT dump was ALSO catching a
          // zeroed/abandoned slot (des0/des3/EDTL all 0); the LIVE PRDT is now captured at RING time too.)
          if ((WTx == UPIU_TXTYPE_COMMAND) && (WOcs == UTP_TRD_OCS_INIT)) {
            mStallLatched = TRUE;
          }
        }
      }
    }
  }
}

EFI_STATUS
EFIAPI
UfsHcRead (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  )
{
  EFI_STATUS  Status;

  UfsHcDiagBump (3);
  UfsHcDiagOr (13, UFS_PH_HCREAD);
  Status = UfsHcMmioRw (FALSE, Width, Offset, Count, Buffer);

  //
  // GENERIC READ HEARTBEAT (any register, VERY coarse throttle now). The 1024-read throttle
  // was a diagnostic that slowed EVERY poll to a crawl (35 ms present per 1024 reads), making
  // a working-but-slow enumeration look like an endless "RD" count. Now that the ASSERT
  // deadloop is fixed and EDK2 actually progresses, fire only every ~4M reads so a NORMAL poll
  // (sub-second, < ~0.5M reads) never triggers it and runs at full speed; only a genuinely
  // stuck multi-second poll (> 4M reads) paints, naming the wedged register. The bounded
  // per-transfer STAGE/RING crumbs remain the primary progress signal.
  //
  #if BEYOND0LTE_UFS_DIAG || BEYOND0LTE_UFS_SCREEN_HUD
  mPollCount++;
  #endif
#if BEYOND0LTE_UFS_SCREEN_HUD
  if ((mPollCount & 0x3FFFFFu) == 0) {
    CHAR8   HbLine[80];
    CHAR8   HbHex[9];
    UINT32  HbDb     = MmioRead32 ((UINTN)EXYNOS_UFS_HCI_BASE + 0x58u);
    UINT32  HbNx     = MmioRead32 ((UINTN)UFS_VS_BASE + HCI_UTRL_NEXUS_TYPE);
    UINT32  HbOcs    = 0xFFu;
    UINT32  HbTx     = 0xFFu;
    UINT32  HbD0     = 0;
    UINT32  HbPrdLo  = 0;
    UINT32  HbPrdLen = 0;
    UINT32  HbLow;

    //
    // Decode the slot EDK2 is stuck waiting on (lowest set doorbell bit): its OCS, the
    // Command UPIU transaction type (TX: 0x01 SCSI cmd / 0x16 query / 0x00 NOP), the UTRD
    // DW0 (D0: command-type/data-direction the controller sees), and the first PRDT entry
    // (PA = data-DMA base addr[31:0], PL = byte-count dword). A data-bearing SCSI command
    // stuck at OCS=0x0F points the finger precisely: PA=0/insane => bad data-buffer
    // mapping; PA sane but OCS frozen => controller refuses the data transfer
    // (nexus/data-path), not a descriptor error.
    //
    if (mTrlBaseCache != 0) {
      for (HbLow = 0; HbLow < 16; HbLow++) {
        if ((HbDb & (1u << HbLow)) != 0) {
          UINTN   HbTrd = (UINTN)mTrlBaseCache + (UINTN)HbLow * UTP_TRD_SIZE;
          UINT64  HbUcd = (UINT64)(MmioRead32 (HbTrd + UTP_TRD_UCDBA_DW4) & 0xFFFFFF80u)
                          | ((UINT64)MmioRead32 (HbTrd + UTP_TRD_UCDBAU_DW5) << 32);
          HbOcs = MmioRead32 (HbTrd + UTP_TRD_OCS_DW2) & 0xFFu;
          HbD0  = MmioRead32 (HbTrd);
          if (HbUcd != 0) {
            UINTN  HbPrd = (UINTN)HbUcd + (UINT32)(MmioRead32 (HbTrd + UTP_TRD_PRDT_DW7) >> 16);
            HbTx     = MmioRead8 ((UINTN)HbUcd) & 0x3Fu;
            HbPrdLo  = MmioRead32 (HbPrd);
            HbPrdLen = MmioRead32 (HbPrd + 12u);
          }
          break;
        }
      }
    }

    HbLine[0] = '\0';
    HudCat (HbLine, "UFS> RD ", sizeof (HbLine));
    HudHex8 (HbHex, mPollCount);
    HudCat (HbLine, HbHex, sizeof (HbLine));
    HudCat (HbLine, " OFF ", sizeof (HbLine));
    HudHex8 (HbHex, (UINT32)Offset);
    HudCat (HbLine, HbHex, sizeof (HbLine));
    HudFbLine (HUD_ROW0, HbLine);

    HbLine[0] = '\0';
    HudCat (HbLine, "DB ", sizeof (HbLine));
    HudHex8 (HbHex, HbDb);
    HudCat (HbLine, HbHex, sizeof (HbLine));
    HudCat (HbLine, " OCS ", sizeof (HbLine));
    HudHex8 (HbHex, HbOcs);
    HudCat (HbLine, HbHex, sizeof (HbLine));
    HudCat (HbLine, " TX ", sizeof (HbLine));
    HudHex8 (HbHex, HbTx);
    HudCat (HbLine, HbHex, sizeof (HbLine));
    HudFbLine (HUD_ROW0 + HUD_LINEH, HbLine);

    HbLine[0] = '\0';
    HudCat (HbLine, "D0 ", sizeof (HbLine));
    HudHex8 (HbHex, HbD0);
    HudCat (HbLine, HbHex, sizeof (HbLine));
    HudCat (HbLine, " NX ", sizeof (HbLine));
    HudHex8 (HbHex, HbNx);
    HudCat (HbLine, HbHex, sizeof (HbLine));
    HudFbLine (HUD_ROW0 + 2 * HUD_LINEH, HbLine);

    HbLine[0] = '\0';
    HudCat (HbLine, "PA ", sizeof (HbLine));
    HudHex8 (HbHex, HbPrdLo);
    HudCat (HbLine, HbHex, sizeof (HbLine));
    HudCat (HbLine, " PL ", sizeof (HbLine));
    HudHex8 (HbHex, HbPrdLen);
    HudCat (HbLine, HbHex, sizeof (HbLine));
    HudFbLine (HUD_ROW0 + 3 * HUD_LINEH, HbLine);

    HudPresent ();
  }
#endif

  //
  // When EDK2 polls the transfer doorbell, run the (non-clearing) diagnostic +
  // byte-gran restore: pack slots 0..3 OCS into diag[7] and restore DW6/DW7 for any
  // slot whose REAL bit has cleared. Does NOT modify the doorbell value (spoofing
  // completion hangs EDK2 deeper), so this stays observation-only.
  //
  if ((Offset == UFS_HC_UTRLDBR_OFF) && (Width == EfiUfsHcWidthUint32) &&
      (Count == 1) && (Buffer != NULL))
  {
    Beyond0LteFixupDoorbellRead ((UINT32 *)Buffer);
  }

  return Status;
}

EFI_STATUS
EFIAPI
UfsHcWrite (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  )
{
  UfsHcDiagBump (4);

  //
  // Exynos nexus-type fix: set HCI_UTRL_NEXUS_TYPE to match the in-flight UPIU
  // (SCSI Command -> all-1s, NOP/Query -> 0) immediately before the transfer
  // doorbell, the way the Samsung reference does per request.
  //
  if ((Offset == UFS_HC_UTRLDBR_OFF) && (Width == EfiUfsHcWidthUint32) &&
      (Count == 1) && (Buffer != NULL) && (*(UINT32 *)Buffer != 0))
  {
    Beyond0LteSetNexusForDoorbell (*(UINT32 *)Buffer);
  }

  return UfsHcMmioRw (TRUE, Width, Offset, Count, Buffer);
}

//
// (UFS COMPLETION-INTERRUPT / IS-ACK PATH REMOVED.) Earlier builds registered a GIC
// handler for the UFS IRQ (SPI 242 / INTID 274) and acked IS from the ISR — the kernel's
// interrupt model. Device result: the handler DID fire (~36M times, RKP did not mask it),
// proving the route works, BUT every write that CLEARS a set IS bit hard-hangs this
// controller in ANY context (8 attempts: 6 polling + 2 ISR), so the IS-ack path is dead.
// We instead make the controller AUTO-RETIRE the doorbell via the init_host data-path
// fixes (WLU_EN / PRDT_PREFECT_EN in Beyond0LteHciVendorSetup), which is the actual hardware
// mechanism the kernel relies on (the IS write there is only interrupt housekeeping), and
// needs no interrupt at all. The HardwareInterrupt include/INF entry are left in place for
// a possible future revisit.
//

/**
  Driver entry: publish the UFS host controller protocol + a device path on a
  fresh handle so UfsPassThruDxe binds and drives the Exynos UFS link.
**/
//
// m10 W18: split large SCSI READ/WRITE commands before they reach the PRDT.
//
// The doorbell hook rewrites every PRDT as <= 4 KB entries at the controller's 128 B stride, and
// DW7 PrdtL is a 16-bit BYTE count, so one command carries at most UTP_PRDT_MAX_SPLIT (511)
// entries, ~2 MB. Above that the hook only logged "PRDT split exceeds" and left EDK2's packed
// 16 B entries in place while DW7 still declared 128 B entries - the controller then reads
// garbage descriptors and DMAs disk data to arbitrary addresses. winload faulted right after
// BlFileLoad with a callee-saved stack slot overwritten (W16/W17). UfsPassThru has no
// EFI_BAD_BUFFER_SIZE path, so the split is done here: EXT_SCSI_PASS_THRU.PassThru of the UFS
// controller is wrapped and READ/WRITE (10/16) larger than UFS_SCSI_SPLIT_BYTES are issued as
// consecutive commands of at most that size (blocking requests only).
//
#define UFS_SCSI_SPLIT_BYTES  (1024u * 1024u)   // 256 x 4 KB entries (+1 for a misaligned start)

STATIC_ASSERT (UFS_SCSI_SPLIT_BYTES / 0x1000u + 1u <= UTP_PRDT_MAX_SPLIT, "split must fit the PRDT");

STATIC EFI_EXT_SCSI_PASS_THRU_PASSTHRU  mOrigPassThru = NULL;
STATIC VOID                             *mPassThruReg = NULL;
STATIC UINT32                           mSplitLogged  = 0;

STATIC
EFI_STATUS
EFIAPI
Beyond0LteSplitPassThru (
  IN     EFI_EXT_SCSI_PASS_THRU_PROTOCOL             *This,
  IN     UINT8                                       *Target,
  IN     UINT64                                      Lun,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet,
  IN     EFI_EVENT                                   Event OPTIONAL
  )
{
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  Sub;
  UINT8                                       SubCdb[16];
  UINT8                                       *Cdb;
  UINT8                                       Op;
  BOOLEAN                                     Is16;
  BOOLEAN                                     IsRead;
  UINT64                                      Lba;
  UINT32                                      Blocks;
  UINT32                                      Total;
  UINT32                                      BlockSize;
  UINT32                                      ChunkBlocks;
  UINT32                                      Done;
  UINT32                                      Count;
  UINT32                                      Got;
  UINT8                                       SenseLen;
  UINTN                                       Index;
  EFI_STATUS                                  Status;

  if ((Event != NULL) || (Packet == NULL) || (Packet->Cdb == NULL)) {
    return mOrigPassThru (This, Target, Lun, Packet, Event);
  }

  Cdb = (UINT8 *)Packet->Cdb;
  Op  = Cdb[0];
  if ((Op != 0x28) && (Op != 0x2A) && (Op != 0x88) && (Op != 0x8A)) {
    return mOrigPassThru (This, Target, Lun, Packet, Event);
  }

  Is16   = (BOOLEAN)((Op == 0x88) || (Op == 0x8A));
  IsRead = (BOOLEAN)((Op == 0x28) || (Op == 0x88));
  Total  = IsRead ? Packet->InTransferLength : Packet->OutTransferLength;
  if ((Total <= UFS_SCSI_SPLIT_BYTES) || (Packet->CdbLength > sizeof (SubCdb)) ||
      (Packet->CdbLength < (Is16 ? 16u : 10u)))
  {
    return mOrigPassThru (This, Target, Lun, Packet, Event);
  }

  if (Is16) {
    Lba = 0;
    for (Index = 2; Index < 10; Index++) {
      Lba = LShiftU64 (Lba, 8) | Cdb[Index];
    }

    Blocks = ((UINT32)Cdb[10] << 24) | ((UINT32)Cdb[11] << 16) | ((UINT32)Cdb[12] << 8) | Cdb[13];
  } else {
    Lba    = ((UINT32)Cdb[2] << 24) | ((UINT32)Cdb[3] << 16) | ((UINT32)Cdb[4] << 8) | Cdb[5];
    Blocks = ((UINT32)Cdb[7] << 8) | Cdb[8];
  }

  if ((Blocks == 0) || ((Total % Blocks) != 0)) {
    return mOrigPassThru (This, Target, Lun, Packet, Event);
  }

  BlockSize = Total / Blocks;
  if ((BlockSize > UFS_SCSI_SPLIT_BYTES) || ((UFS_SCSI_SPLIT_BYTES % BlockSize) != 0)) {
    return mOrigPassThru (This, Target, Lun, Packet, Event);
  }

  if (mSplitLogged < 4) {
    mSplitLogged++;
    DEBUG ((DEBUG_ERROR, "UfsHc: split op 0x%02x lba 0x%lx %u x %u B\n", Op, Lba, Blocks, BlockSize));
  }

  ChunkBlocks = UFS_SCSI_SPLIT_BYTES / BlockSize;
  SenseLen    = Packet->SenseDataLength;
  CopyMem (SubCdb, Cdb, Packet->CdbLength);
  Status = EFI_SUCCESS;
  Done   = 0;
  while (Done < Blocks) {
    Count = MIN (ChunkBlocks, Blocks - Done);
    CopyMem (&Sub, Packet, sizeof (Sub));
    Sub.Cdb             = SubCdb;
    Sub.SenseDataLength = SenseLen;
    if (Is16) {
      UINT64  L = Lba + Done;

      for (Index = 9; Index >= 2; Index--) {
        SubCdb[Index] = (UINT8)L;
        L             = RShiftU64 (L, 8);
      }

      SubCdb[10] = (UINT8)(Count >> 24);
      SubCdb[11] = (UINT8)(Count >> 16);
      SubCdb[12] = (UINT8)(Count >> 8);
      SubCdb[13] = (UINT8)Count;
    } else {
      UINT32  L = (UINT32)Lba + Done;

      SubCdb[2] = (UINT8)(L >> 24);
      SubCdb[3] = (UINT8)(L >> 16);
      SubCdb[4] = (UINT8)(L >> 8);
      SubCdb[5] = (UINT8)L;
      SubCdb[7] = (UINT8)(Count >> 8);
      SubCdb[8] = (UINT8)Count;
    }

    if (IsRead) {
      Sub.InDataBuffer     = (UINT8 *)Packet->InDataBuffer + (UINTN)Done * BlockSize;
      Sub.InTransferLength = Count * BlockSize;
    } else {
      Sub.OutDataBuffer     = (UINT8 *)Packet->OutDataBuffer + (UINTN)Done * BlockSize;
      Sub.OutTransferLength = Count * BlockSize;
    }

    Status                    = mOrigPassThru (This, Target, Lun, &Sub, NULL);
    Got                       = IsRead ? Sub.InTransferLength : Sub.OutTransferLength;
    Packet->HostAdapterStatus = Sub.HostAdapterStatus;
    Packet->TargetStatus      = Sub.TargetStatus;
    Packet->SenseDataLength   = Sub.SenseDataLength;
    if (EFI_ERROR (Status) || (Sub.HostAdapterStatus != 0) || (Sub.TargetStatus != 0) ||
        (Got != Count * BlockSize))
    {
      Got = Done * BlockSize + Got;   // bytes actually moved, reported like a short transfer
      if (IsRead) {
        Packet->InTransferLength = Got;
      } else {
        Packet->OutTransferLength = Got;
      }

      return Status;
    }

    Done += Count;
  }

  return Status;
}

STATIC
VOID
EFIAPI
Beyond0LteOnExtScsiPassThru (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *Pt;

  while (!EFI_ERROR (
            gBS->LocateHandleBuffer (
                   ByRegisterNotify,
                   &gEfiExtScsiPassThruProtocolGuid,
                   mPassThruReg,
                   &Count,
                   &Handles
                   )
            ))
  {
    for (Index = 0; Index < Count; Index++) {
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiExtScsiPassThruProtocolGuid, (VOID **)&Pt)) ||
          (Pt->PassThru == Beyond0LteSplitPassThru))
      {
        continue;
      }

      if (mOrigPassThru == NULL) {
        mOrigPassThru = Pt->PassThru;
        Pt->PassThru  = Beyond0LteSplitPassThru;
        DEBUG ((DEBUG_ERROR, "UfsHc: ExtScsiPassThru wrapped (split > %u B)\n", UFS_SCSI_SPLIT_BYTES));
      } else if (Pt->PassThru == mOrigPassThru) {
        Pt->PassThru = Beyond0LteSplitPassThru;
      }
    }

    FreePool (Handles);
  }
}

EFI_STATUS
EFIAPI
Beyond0LteUfsHcEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  if (EXYNOS_UFS_HCI_BASE == 0) {
    DEBUG ((DEBUG_ERROR, "Beyond0LteUfsHc: EXYNOS_UFS_HCI_BASE not set.\n"));
    return EFI_NOT_READY;
  }

  //
  // DIAG: zero the cross-module counter block so PlatformBootManagerLib reads a
  // clean slate of how far UfsPassThru.Start() drives our callbacks.
  //
  {
    #if BEYOND0LTE_UFS_DIAG
    volatile UINT32  *D = (volatile UINT32 *)(UINTN)UFSHC_DIAG_BASE;
    UINTN            I;
    for (I = 0; I < 39; I++) {
      D[I] = 0;
    }
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    #endif
  }

  //
  // Publish the STAGE breadcrumb function pointer at the fixed address so the in-box
  // UfsPassThru module can paint on-screen stage markers from inside its dev-command flow.
  //
#if BEYOND0LTE_UFS_DIAG
  *(volatile UINT64 *)(UINTN)BEYOND0LTE_STAGE_FNPTR = (UINT64)(UINTN)&Beyond0LteStage;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
#endif

  mUfsHc.GetUfsHcMmioBar = UfsHcGetMmioBar;
  mUfsHc.AllocateBuffer  = UfsHcAllocateBuffer;
  mUfsHc.FreeBuffer      = UfsHcFreeBuffer;
  mUfsHc.Map             = UfsHcMap;
  mUfsHc.Unmap           = UfsHcUnmap;
  mUfsHc.Flush           = UfsHcFlush;
  mUfsHc.Read            = UfsHcRead;
  mUfsHc.Write           = UfsHcWrite;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEdkiiUfsHostControllerProtocolGuid, &mUfsHc,
                  &gEfiDevicePathProtocolGuid,          &mUfsHcDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Beyond0LteUfsHc: install protocol failed (%r).\n", Status));
    return Status;
  }

  //
  // Install the UFS HC platform protocol (separate handle is fine — UfsPassThru
  // finds it via LocateProtocol). MUST exist before BDS connects the controller
  // so UfsPassThru.Start() honors SkipHceReenable/SkipLinkStartup and reuses
  // sboot's live link.
  //
  {
    EFI_HANDLE  PlatHandle = NULL;
    EFI_STATUS  PlatStatus = gBS->InstallMultipleProtocolInterfaces (
                                    &PlatHandle,
                                    &gEdkiiUfsHcPlatformProtocolGuid, &mUfsHcPlatform,
                                    NULL
                                    );
    if (EFI_ERROR (PlatStatus)) {
      DEBUG ((DEBUG_WARN, "Beyond0LteUfsHc: UFS HC platform protocol install failed (%r); "
        "UfsPassThru will reset the link.\n", PlatStatus));
    }
  }

  //
  // (UFS completion interrupt is registered from the PostHce callback, where the
  // GIC HardwareInterrupt protocol is available — not here at entry, where it isn't.)
  //

  {
    EFI_EVENT  PtEvent;

    if (!EFI_ERROR (gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Beyond0LteOnExtScsiPassThru, NULL, &PtEvent))) {
      gBS->RegisterProtocolNotify (&gEfiExtScsiPassThruProtocolGuid, PtEvent, &mPassThruReg);
    }
  }

  DEBUG ((DEBUG_INFO, "Beyond0LteUfsHc: UFS host controller protocol up at 0x%lx.\n",
    (UINT64)EXYNOS_UFS_HCI_BASE));
  return EFI_SUCCESS;
}
