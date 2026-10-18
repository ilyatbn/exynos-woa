# Windows internals not looked at (security)

Rule (user, 2026-10-16; AGENTS.md section 11): when the easiest way to solve a problem is to look inside Windows
itself (disassembling or reading the internals of Microsoft binaries, checking Windows system files, undocumented
kernel structures), don't do it. Record it here instead and solve the problem from our own side (our
instrumentation, documented sources, behavioural tests).

Entry format: date, then "The best way to get the answer to this problem would have been to look at XXXX to solve the
problem of YYYY, but due to security concerns I chose not to do that", then what was done instead.

## Entries

- 2026-10-16: The best way to get the answer to this problem would have been to look at the code paths in dxgi.dll
  around BindBackBuffersToDwm / DWM redirection (when a UMD returns DXGI_STATUS_NO_REDIRECTION) to solve the problem
  of composition swap chains missing their tracked fence, but due to security concerns I chose not to do that.
  Instead: DXGI ETW events (tools/phone/wddm2/etwswap.ps1) showed which redirection events fire on WARP and not on
  our driver. That led to Mesa 0072, D3D10UMD_REDIRECT.
- 2026-10-16: The best way to get the answer to this problem would have been to look at how dxgkrnl / dwmcore decide
  STATUS_ACCESS_DENIED on DWM's Render journal entry (and how dwmcore handles 0x8898008D) to solve the problem of DWM
  losing its devices with redirected surfaces, but due to security concerns I chose not to do that. Instead: logging
  wrappers on every runtime callback in our malikm shim (Mesa 0073), plus our KMD's PRAM DDI trace
  (wddm2dbg.sh kmdtrace). Still open.
- 2026-10-16: The best way to get the answer to this problem would have been to look at Edge's (msedge.dll) GPU
  process code at the skia_output_surface_impl.cc CHECK to solve the problem of the GPU process dying under
  allowES3OnFL10_0, but due to security concerns I chose not to do that. Instead: the public Chromium sources, read
  online by a research agent, plus our UMD's own log of the GPU process (browsertest.ps1 -UmdLog).
- 2026-10-17: The best way to get the answer to this problem would have been to look at the Windows kernel/HAL
  secondary-processor start and per-CPU state code (KeRestoreProcessorState and the processor feature/errata paths) to
  solve the problem of the Mongoose (M4) cores hanging the system once Windows runs work on them, but due to security
  concerns I chose not to do that. Instead: the owner asked for a guide to do that research himself
  (docs/exynos9820-port/mongoose-research-guide.md: public symbols + WinDbg observation, ACPI dumps, safe experiments,
  a hand-back template), built from our own notes (m14-*), and the firmware side acts on what he reports.

## 2026-10-18 - M4 (big core) bring-up: leaked NT 5.1 kernel source suggested
The best way to get the answer to this problem would have been to look at the NT 5.1 kernel source
(github.com/vipinkiruba/kernel-NT5.1, suggested by the user) to solve the problem of the M4 cores resetting the
phone when Windows starts them, but due to security concerns I chose not to do that. Additionally it is leaked
proprietary source (licence/IP taint) and x86-only NT 5.1 with no ARM64, PSCI, GICv2 or big.LITTLE code, and the
M4 reset is external to Windows (notes/m14-smp.md: KeBugCheck / HAL reboot / WHEA never hit -> EL3 / uH).
Instead: our own instrumentation - the PROBE-mask CPU_ON test that records MIDR / ID registers / GIC interface /
clock of an M4 without offering it to Windows (notes/cpu-ram-plan-20261012.md plan #4), and KD over USB (M17).
