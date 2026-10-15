# D10 video (MFC) - working notes on main (2026-10-15)

Source material lives on origin/drivers only (commit 2722372): docs/exynos9820-port/D10-VIDEO-MFC-DESIGN.md,
notes/video-mfc.md, tasks/V01-mfc-live-reads.md, tasks/V02-mfc-r1-observe.md, plus an Exynos9820.h "Video codec /
MFC (D10)" block and 5 PlatformMemoryMapLib entries. main had none of it when this file was started.

## Design (branch)
- Windows: AVStream (Ks.sys) minidriver exposing KS filters (decoder output NV12); Media Foundation wraps them as
  hardware MFTs. DXVA/D3D11 video is out (needs a WDDM display driver). Fallback: KMDF + proxy hardware MFT (P8).
- UEFI: Beyond0LteMfcDxe, observe-only first; R2+ behind BEYOND0LTE_MFC_INIT 0. SYSMMUs belong to Windows (in _CRS).
- DSDT: Device (VIDC). The branch used _HID SAMS9828, which on main is GPU0 -> needs a free ID (SAMS982A planned).
- Rungs: R1 version reg 0xF028 = 0x1300 (zero writes) | R2 gate 422 on, re-read | R3 RISC boots mfc_fw.bin
  (R2H 12) | R4 SYS_INIT (R2H 1) | R5 Windows binds VIDC | R6 MFTEnumEx finds it, one H.264 frame -> NV12 | R7 encode.

## Constants (branch, VERIFIED there)
- Core 0x188D0000 + 0x10000 (samsung,exynos-mfc); GSIV 370 (SPI 338); ip_ver 0x1300 at 0xF028.
- SYSMMU 0x18890000 / 0x188B0000 (+0x9000 each; secure aliases 0x188A0000 / 0x188C0000), GSIVs 375-378.
- Clock gates 422 (MFC), 423/424 (SYSMMUs) in CMU_TOP clock-controller@0x1a240000.
- pd-mfc@15862600 (reg 0x20), cal_id 0xb138000c, need_smc 0x18820204.
- Regs: RISC_ON 0x0000, RISC_BASE_ADDR 0x0054, MFC_RESET 0x1070; R2H codes 1 / 12 (mfc_regs.h).
- Firmware /vendor/firmware/mfc_fw.bin, 687,912 B (never commit).
UNVERIFIED there: which CMU bank feeds gate 422; mfc_freqs unit; live-device node/IRQ confirmation (V01).

## Open questions (branch numbering)
P2 SMC-mediated power domain (need_smc); P3 SYSMMU contract; P4 firmware must match ip_ver; P5 CMU bank of gate
422; P6 TZPC in the vendor clock-on sequence; P8 AVStream on Win11 ARM64. Branch FD had only 56 B free in
FVMAIN_COMPACT (verification build) - watch image space on main.

## Progress log
- 2026-10-15: summary taken from origin/drivers (above). Order of work: D4c Bluetooth first, then this.
- 2026-10-15 btmfc1 (MFC probe + power on, 13:34): pd-mfc ALREADY ON at UEFI time (configuration 1, status 1 -
  sboot leaves it powered), so R2's power-on was not needed. CMU_MFC readable: mfc user mux 0x10, mfc ACLK gate
  0x00200000, MFC QCH 0x003F0002. Version register 0xF028 = 0x1D0447AA, RISC_ON 0, no hang.
- The vendor driver reads MFC_REG_MFC_VERSION only AFTER the firmware's SYS_INIT (mfc_run.c:110-116, "Check MFC
  version and F/W version") - the firmware fills it. So "read 0x1300 with zero writes" was a wrong R1 expectation on
  the branch; pre-firmware the register holds a leftover. R1 really proves: domain on, CMU_MFC + MFC respond without
  a hang. Identity check moves to R3/R4 (boot mfc_fw.bin: RISC_BASE_ADDR, RISC_ON, wait R2H FW_STATUS 12 / SYS_INIT 1).
- **R3 PASSED (btmfc2 c23162d6, 14:04):** mfc_fw.bin (687,912 B, FV-embedded test build) copied to a reserved buffer
  at 0xEF700000 (< 4 GiB, 1 MiB aligned); vendor reset sequence, RISC_BASE_ADDRESS = 0xEF700000 (reads back),
  RISC_ON 1 / MFC_OFF 0 -> RISC2HOST_INT 1, RISC2HOST_CMD 12 = FW_STATUS_RET after 1 ms. After the boot
  MFC_VERSION = 0x00001300 (= DT ip_ver, the firmware fills it) and FW_VERSION = 0x45210720. => the MFC RISC boots
  from non-secure UEFI with the SYSMMUs in reset bypass (device address = physical) and no TZPC block (sboot left
  pd-mfc on and configured; no need_smc call was needed). MFC put back into reset afterwards; Windows booted fine.
- Next: R4 SYS_INIT (mfc_cmd_sys_init), then the Windows side.
- **R4 PASSED (btmfc3 2e5a27a4, 14:16 boot):** after FW_STATUS_RET: CONTEXT_MEM_ADDR = 0xEFB00000 (buffer + 4 MiB),
  CONTEXT_MEM_SIZE = 0x8000, H2R cmd 1 + H2R int -> R2H cmd 1 = SYS_INIT_RET after 1 ms; FW_VERSION 45210720 stays.
  => MFC firmware boots AND initialises from non-secure UEFI (physical addressing, SYSMMUs in reset bypass).
  (The first btmfc3 boot at 14:14 came back to TWRP after ~50 s with no UEFILOG written and no explanation - the
  identical image then ran cleanly; logged as an intermittent early-boot reset, second unexplained one today after
  the verbose8 UEFI hang.)
- Windows plan: V-W1 KMDF driver Exynos9820Mfc on DSDT VIDC (_HID SAMS982A, MFC window + GSIV 370) that loads
  %12%\mfc_fw.bin (package copy from the phone, never committed) into a physically contiguous non-cached buffer
  below 4 GiB, boots it (R3) and SYS_INITs it (R4) with the R2H interrupt; V-W2 decode one H.264 stream via private
  IOCTLs (instance open, seq header, frame decode - vendor mfc_cmd/mfc_nal flow); V-W3 Media Foundation integration.
- **V-W1 PASSED in Windows (vidc1 82abd47c + Exynos9820Mfc a196552, 14:31):** KD: "fw 687912 B at PA 0xef600000:
  R2H 12, MFC_VERSION 0x1300, FW_VERSION 0x45210720", "SYS_INIT: R2H 1", "boot 0x00000000", "interrupt test: ISR ran
  1 time(s), last R2H cmd 1, R2H int now 0". => the Windows driver boots + initialises the MFC firmware from disk
  (MmAllocateContiguousMemorySpecifyCache, non-cached, < 4 GiB, 1 MiB aligned) and the R2H interrupt (GSIV 370)
  reaches the ISR. Next: V-W2 decode (instance open / seq header / buffers / frame decode).
- **VIDC gate (boot hazard fix):** the MFC driver is now installed in Windows, but DSDT VIDC was always present
  (_STA 0x0F) - on a build/boot where pd-mfc is off the driver would touch an unpowered MFC. VIDC._STA now reads
  progress word [13] (0x8FE0F034) = 'VIDC' (0x43444956); Beyond0LteMfcDxe clears it at entry and sets it only when
  MfcPowered() (also with all MFC switches off - a read-only PMU check). Word [13] was free (SEC clears [2..4],
  BT owns [11], the death counter [12]). Build clean; device check with the next image (VIDC must stay Started).
