# Beyond1LtePkg — Samsung Galaxy S10 (Exynos 9820)

This directory is the regular S10 target being derived from the existing
`Beyond0LtePkg` work in this same repository. The S10e package remains intact
as a reference; it is not treated as the target.

## Current state

**Research/configuration skeleton only — not a buildable or flashable platform.**

`Include/Platform/Beyond1Lte.h` records board facts that are explicitly
identified by the `beyond1lte` vendor DTS. Shared Exynos 9820 MMIO definitions
remain in `Silicon/Exynos9820Pkg`, but the S10e memory map, framebuffer and
UFS DMA window are not inherited.

The readiness markers in the header intentionally remain zero. A future DSC
must refuse to enable the corresponding driver paths until their readiness
conditions are satisfied.

## Hard blockers before a bootable DSC

1. Source or discover the regular S10's actual DRAM bank map and reserved
   memory after Samsung bootloader patching. Generic SoC DTS memory nodes and
   the S10e live map are not a substitute.
2. Establish the S10 UFS DMA/FMP aperture and reconcile controller register
   extents with the vendor DTS.
3. Obtain the S10 bootloader framebuffer base, stride, pixel format, buffer
   count and selected panel revision before publishing GOP or drawing a splash.
4. Confirm the device's hardware revision and selected MAX77705 DTBO before
   enabling USB-C role/VBUS control.
5. Decode the Samsung GPIO binding/pinctrl electrical semantics before
   implementing volume, power or Bixby input.

No device writes, flash operations, or hardware initialisation are enabled by
this skeleton.
