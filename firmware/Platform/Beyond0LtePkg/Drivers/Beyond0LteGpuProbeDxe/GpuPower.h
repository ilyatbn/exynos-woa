/** @file
  M16 G9: G3D power-on (GpuPower.c). notes/gpu-m16-g3don.md.
**/

#ifndef GPU_POWER_H_
#define GPU_POWER_H_

/**
  Power the G3D domain on per pmucal embedded_g3d_on + DTZPC restore SMC, then ACPM FREQ_REQ 260 MHz.
  Does nothing if EMBEDDED_G3D_STATUS (PMU+0x2104) bit0 is already set at entry (the vendor's own test).

  @retval TRUE   the domain was OFF and is now ON (the ACPM request may still have failed - logged)
  @retval FALSE  untouched (already on), or a step failed and the domain was powered back off
**/
BOOLEAN
GpuPowerOn (
  VOID
  );

#endif // GPU_POWER_H_
