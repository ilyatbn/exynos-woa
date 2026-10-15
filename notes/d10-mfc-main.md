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
