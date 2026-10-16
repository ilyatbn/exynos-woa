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
