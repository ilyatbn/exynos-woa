# M15 - performance: clocks, storage, display (2026-10-11)

User (boot #58, 6 cores + 5.2 GB): "everything is still kinda slow tbh maybe because of no gpu?" ... "any chance
you can get some basic graphics acceleration going? ... switch the screen orientation to landscape ... dynamic with
a tool from within windows".

## GPU verdict (drivers branch notes/d5-gpu-display.md, the user's research agent)
No Mali-G76 WDDM exists anywhere; options are a display-only WDDM (DECON) or a multi-person-month panfrost port.
Shipped = GOP framebuffer + inbox BasicDisplay + WARP (CPU rendering). => speed has to come from elsewhere first.

## Suspects for "slow", to MEASURE before changing anything
1. CPU clocks: sboot may leave the clusters at a boot frequency; Linux raises them via ACPM DVFS (channel 5).
   drivers branch notes/cpu-driver-9820.md F2/F3/F9/F11: domains idx 0 MIF, 2 LITTLE (442 MHz-1.95 GHz), 3 MID
   (-2.4 GHz), 4 BIG (-2.08 GHz build cap); FREQ_REQ 0 / FREQ_GET 1, reply cmd[1] = kHz.
2. Storage: Exynos9820Ufs is queue depth 1 and bounces every transfer.

## Progress log
- Boot #59 (built): Beyond0LteTouchPowerDxe also does a READ-ONLY FREQ_GET for idx 0/2/3/4 on ACPM channel 5
  (live DT acpm_dvfs = 5) and logs "TouchPower: DVFS <name> (idx n) = <kHz>". FVMAIN_COMPACT auto-sized.
