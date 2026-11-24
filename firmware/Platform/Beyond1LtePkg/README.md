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

## Inherited driver code that is not safe to copy verbatim

The S10e package is a valuable source reference, but several modules have
active side effects that must be reviewed before reuse:

- `Beyond0LteUfsHcDxe.c` calls secure-monitor FMP/SMU commands during UFS
  initialisation, including disk-key store/set operations using a static
  embedded key. These calls are not all behind the diagnostics flag. They can
  change secure storage state and are not to be copied into the S10 target.
- `Beyond0LteUsbHostDxe.c` enables host bring-up by default in its current
  source and can configure MAX77705 CCIC state and switch on the VBUS boost.
  The S10 DTS confirms the MAX77705 family, but that does not validate the
  existing VBUS sequence for every S10 board revision.
- `ArmPlatformLib.c` draws an early splash to the S10e framebuffer, writes a
  fixed progress page at 0x8FE0F000 and arms the watchdog. None of those
  addresses/side effects is approved for the S10 target.
- `PlatformMemoryMapLib.c` and `Beyond0LteRamManagerDxe` publish S10e live
  RAM banks and reserved-memory carveouts.
- `SimpleFramebufferDxe` and the framebuffer serial library assume the
  S10e framebuffer base and 1080x2280 geometry.
- `Beyond0LteTouchPowerDxe` performs ACPM PMIC rail writes; validate the S10
  board's touch-rail configuration before enabling those writes.

The S10 target will reuse only code whose register interface and side effects
have been independently classified. The initial S10 UFS path must not set a
new disk key or alter FMP/SMU state speculatively; USB host/VBUS must remain
disabled until the Type-C/PMIC path is proven.

## Current inherited-driver audit

The existing S10e platform is not a safe drop-in for the S10:

- The static S10e memory map and reserved regions differ from the generic
  Exynos 9820 DTS and may differ again from the regular S10's sboot-patched
  map. No S10 FDT handoff pointer is established yet.
- The existing UFS HC driver executes active FMP/SMU secure-monitor calls
  during controller initialisation, including disk-key store/set calls with
  an embedded key. Those are not diagnostics-only. The S10 port will not
  reuse that sequence.
- The S10e USB host module currently enables host mode and Windows exposure
  by default; the PMIC module can reconfigure MAX77705 and enable VBUS boost.
  S10 host/VBUS remains disabled until the selected board revision and
  Type-C role policy are established.
- The existing early splash, progress page, watchdog, memory-map, GOP and
  touch-power paths all contain S10e-specific addresses or writes. Each needs
  an explicit S10 adaptation or must be left out of the first S10 build.

See `tasks/B1.3-beyond1lte-driver-audit.md` for the source-level audit and
acceptance checklist.

## Boot handoff lead

The 9820 bootshim receives the FDT pointer in x0 and writes it to
0x8FE0F020 with an `FDT0` tag at 0x8FE0F028. That address is part of the
S10e-specific progress/WINPRAM page. Existing DXE code does not consume it.
The S10 target needs a safe early-SEC handoff for this pointer before a
runtime FDT memory-map parser can be used. The repo's `sbootimg.py` and
`fdt.py` tools are available for offline stock-image/DTB analysis, but the
existing dump artifacts are for the S10e.
