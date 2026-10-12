# MaliG76Wddm (M16 G10 + G11)

**Origin.** Derived from gahingwoo's
[wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) (commit `16f7e7c6`),
GPL-2.0, Copyright gahingwoo: `drivers/maliwddm` (the WDDM 1.3 full adapter),
`drivers/malidod` (the display-only driver), `drivers/common/maligpu` (the
Mali job-manager layer) and `include/maliwddm_abi.h` (unchanged). There they
drive the RK3576's Mali-G52 and HDMI output; this copy is retargeted to the
Galaxy S10e's Mali-G76 (Exynos 9820) and its internal panel. The licence text
is `LICENSE` in this directory (the same text as upstream); this subtree stays
GPL-2.0. Credit is also in the repository's `NOTICE`.

**Modes** (`HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters`,
read when the driver loads; the INF resets both to 0):

| GpuEnable | FullAdapter | What loads |
|---|---|---|
| 0 | 0 | display-only (KMDOD): GOP framebuffer, CPU copies, no GPU access - default |
| 0 | 1 | full WDDM 1.3 adapter, GPU half off (GPU interrupt outputs masked only) |
| 1 | any | full WDDM 1.3 adapter + GPU half (aperture segment, Mali MMU, job chains) |

The full adapter needs a UMD name in its class key (the ESP script writes
`d3d10warp.dll`, the original's steps a/b); with it the desktop is expected to
go black (the original's installed-Windows run 6) until the Mesa UMD (G12).

**What changed** (notes/gpu-wddm-mali-g52.md R1-R19, notes/gpu-m16-g10.md):
binds `ACPI\SAMS9828` (DSDT `GPU0`) only; checks the `_CRS` contract (GPU
block, PMU page, 3 interrupts); "powered" = PMU+0x2104 and PMU+0x2384 bit0
both 1, re-checked before every GPU access path and in the ISR; GPU_ID gate
for the G76; panfrost's G76 init quirks (SHADER_CONFIG TLS hash); one core
group only; page tables and HighestAcceptableAddress below the GPU's PA bits;
the display child is the internal panel; the framebuffer dxgkrnl reports is
compared with the GOP values. Single job chain at a time, as the original.

**Rescue.** The firmware hides `GPU0` (image built with
`BEYOND0LTE_GPU_POWER_ON 0`, or `\S10EDIAG\nogpu.flg` on the ESP); then this
driver never loads and Basic Display keeps the screen. No NTFS writes needed.

**Not validated on hardware.** Build: `build-package.ps1 -Out <dir>`.
