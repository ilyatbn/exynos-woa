# Beyond1Lte boot shim

This is a minimal ARM64 Linux-Image-compatible shim for the Exynos Galaxy S10
(`beyond1lte`). It is separate from the diagnostic-heavy S10e shim.

## Behaviour

1. sboot enters at `_head`; x0 is the boot DTB pointer.
2. The shim preserves x0 unchanged and sets x1 to the appended FD payload.
3. It clears stale `TPIDRRO_EL0` state (the EDK2 PeilessSec path treats this
   register as a TransferList pointer).
4. It clean-invalidates data/unified caches by set/way and invalidates I-cache
   to avoid stale code after a warm reset.
5. It relocates the appended FD to the configured `UEFI_BASE`, using
   overlap-safe copy logic, then branches to the FD entry point.

It deliberately does not write to a fixed DRAM progress page, framebuffer,
DECON, PMU, watchdog, PMIC, GPIO or USB controller. It has no S10e RAM-map or
framebuffer assumptions.

## Build on Linux

Requires clang, ld.lld, llvm-objcopy and Python 3:

```sh
UEFI_BASE=0x90000000 UEFI_SIZE=0x700000 ./build.sh
```

The values above are **smoke-test values inherited from the S10e/Exynos9820
prior art**, not validated regular-S10 FD placement. The real values must be
taken from the matching `Beyond1LtePkg.fdf` once that file exists. The build
script requires both variables and checks the embedded Linux Image header,
tag, FD base/size and 16-byte payload alignment.

## Current limitations

- This shim is cross-compiled and structurally verified only; it has not been
  run on a regular S10.
- It preserves the DTB pointer in x0, but the current EDK2 SEC/DXE path does
  not yet consume it. A safe FDT handoff/HOB is still required.
- There is no S10 DSC/FDF or boot-image packer yet. Do not flash this shim.
- Warm-reset cache maintenance is based on the existing Exynos9820 shim's
  observed need; actual S10 reset behaviour remains untested.
