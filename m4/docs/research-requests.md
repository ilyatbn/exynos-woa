# Research requests for a researcher agent

Written 2026-10-17 at the owner's request: questions whose answers sit inside Windows itself. The project's coding
agent may not inspect Windows binaries or internals (AGENTS.md rule 11, log in notes/skipped-windows-internals.md),
so these are handed to a researcher. Answer what you can, in the format of section 3. The Mongoose (M4 big core)
work has its own guide: [mongoose-research-guide.md](mongoose-research-guide.md) (request R13 below).

## 1. The system

- **Device:** Galaxy S10e (Exynos 9820), Windows 11 Pro ARM64, build 22621 (ni_release), test signing on, custom
  UEFI. Six usable cores (4x A55 + 2x A75); the 2 Mongoose M4 cores are parked (R13).
- **GPU:** Mali-G76 MP12.
  - **KMD:** our MaliG76Wddm, WDDM 2.0. It runs without GPU virtual addressing: allocation lists and patch lists on
    every submission, one aperture segment of 2.5 GiB.
  - **UMD:** Mesa 26.2 d3d10umd + Panfrost, implementing the D3D10/10.1/11 user-mode DDI. Apps see Direct3D 11 at
    feature level 10.1.
  - **Shim:** a layer (malikm_drm.c) maps Panfrost's Linux DRM calls onto the runtime callbacks: pfnAllocateCb,
    pfnRenderCb, pfnLockCb, pfnPresentCb (DXGI), pfnSignalSynchronizationObject2Cb with CPU events.
- **Not present:** no D3D12, no D3D9 DDI, no Vulkan, no OpenGL ICD.
- **32-bit games:** an x86 build of the UMD (maliumd32.dll, UserModeDriverNameWow) runs inside WOW64 processes,
  emulated.
- **Display:** the panel is command-mode DSI, scanned out from a firmware framebuffer. The KMD reports a software
  vsync and copies or latches frames. A UMD GPU blit writes most frames straight into the scan-out buffer.
- **Rule for answers:** the project ships native only (AGENTS.md rule 12). An answer of the form "set this env var
  or app flag" helps diagnosis, but the fix we need is something our KMD/UMD/INF does.

## 2. Requests (highest value first)

### R1. Can the UMD stop waiting for the GPU before each Present?
- **Seen:** at every application Present our UMD flushes, then waits on the CPU until the frame's GPU work is
  complete, then calls pfnPresentCb (Mesa d3d10umd DxgiFns.cpp, "the frame is finished first"). In a game this wait
  is about 10.5 ms per frame, and it serialises CPU and GPU (notes/g21-submissions.md, 21:40).
- **Question:** in this configuration (WDDM 2.0, no GPU VA, allocation lists with WriteOperation set for written
  allocations), does the OS guarantee that the consumer's reads wait for earlier submissions that wrote the presented
  allocation? Consumers here are DWM's GPU composition in another process, and our KMD's CPU copy at
  DxgkDdiPresent / SetVidPnSourceAddress time.
  - If yes: under which conditions (same context, allocation listed as written, flip vs blt model, redirected vs
    direct flip)?
  - If no: which documented mechanism should a UMD use instead (a monitored fence, pfnSignalSynchronizationObjectCb
    plus a wait in the present, a present flag)?
- **Answer needed:** yes/no plus the exact conditions; the DDI fields that matter; what dxgkrnl does with the
  allocation list of a Present.

### R2. Why did waking the display take minutes?
- **Seen (2026-10-17):** the screen timed out. Afterwards neither input nor the power button brought it back for many
  minutes, then it came back by itself. DWM was alive and responding; no TDR, no events (notes/g21-submissions.md,
  "SCREEN OUTAGE"). Mode changes (ChangeDisplaySettingsEx, not saved) had been requested while it was off; they never
  re-created DWM's primaries.
- **Question:** the sequence Windows runs for display off and display on with a WDDM 2.0 driver. Which DDIs, in what
  order: child/monitor power, SetVidPnSourceVisibility, deferred CommitVidPn of mode changes requested while off,
  SetPowerState. Also: which timeouts or waits in dxgkrnl, win32k or DWM could stall the wake for minutes, and what a
  driver must answer to avoid that.
- **Answer needed:** the DDI order with expected return values; any timeout values; what DWM waits for before its
  first frame after wake.

### R3. DWM loses its devices when surfaces are redirected
- **Seen:** with redirected swap chains, dxgkrnl answers DWM's Render with STATUS_ACCESS_DENIED, and DWM then
  removes the device (0x8898008D in dwmcore). Our KMD sees nothing failing (notes/gpu-umd-d3d11.md;
  notes/skipped-windows-internals.md 2026-10-16).
- **Question:** what makes dxgkrnl refuse a Render with STATUS_ACCESS_DENIED: which allocation, ownership or
  open-resource rule? How does dwmcore react to 0x8898008D?
- **Answer needed:** the condition; which allocation flags, lists or calls of the UMD trigger it; what a UMD must do
  differently.

### R4. Native DWM redirection for composition swap chains
- **Seen:** composition swap chains lack their tracked fence unless our UMD returns DXGI_STATUS_NO_REDIRECTION. That
  behaviour is gated by a diagnostic env var (D3D10UMD_REDIRECT), which is not a native solution (rule 12)
  (notes/gpu-umd-d3d11.md around Mesa 0072/0073; notes/skipped-windows-internals.md).
- **Question:** what DXGI (BindBackBuffersToDwm / redirection) expects from a UMD, so that redirection works
  natively: which DDIs and return values, which resource flags, and which fence objects DXGI creates or expects.
- **Answer needed:** the expected UMD behaviour, step by step.

### R5. CpuVisibleOnDemand in 32-bit processes and in DWM
- **Seen:** for allocations created from a WOW64 process, the allocation flag CpuVisibleOnDemand (0x00040000 in
  DXGK_ALLOCATIONINFOFLAGS) makes allocation creation fail with E_INVALIDARG. In 64-bit DWM the same flag works but
  composition becomes very slow (a frame every few seconds) (notes/g17-dxva-mfc.md ~742).
- **Question:** what the OS checks for this flag (process bitness, segment properties, allocation size or
  alignment), and why it slows DWM.
- **Answer needed:** the conditions; the documented or required segment and allocation properties.

### R6. Each allocation's CPU view costs 32 MB of address space in WOW64
- **Seen:** in a 32-bit process, every CPU-visible allocation's mapping consumes 32 MB of virtual address space. We
  worked around it with sub-allocation slabs (Mesa 0102, notes/g17-dxva-mfc.md).
- **Question:** where the 32 MB granularity comes from (aperture segment mapping, section views, LockCb behaviour),
  and which KMD segment or allocation settings would map smaller views natively.
- **Answer needed:** the cause, and the setting (segment flags, allocation flags, LockCb flags) that changes it.

### R7. DWM keeps one buffer per app window (~19 MB leak per session)
- **Seen:** for each app window, DWM opens one flip buffer and never destroys it (no DestroyResource reaches our
  UMD) (HANDOFF section 55, item 3).
- **Question:** what makes DWM release an opened, redirected app buffer. Which UMD calls or answers does it rely on:
  resource release, present statistics, the swap chain's destroy path?
- **Answer needed:** the release trigger, and what our UMD or KMD may be missing.

### R8. DWM / process teardown hang after taskkill
- **Seen:** killing an app sometimes leaves it unkillable, and DWM stops composing until a reboot (HANDOFF section 55,
  item 4).
- **Question:** what dxgkrnl and DWM wait for while a GPU process is torn down: pending submissions, fences,
  allocation destruction, context destroy. Which KMD DDIs must complete or answer for teardown to finish (for
  example DestroyContext / DestroyDevice ordering, outstanding fences, CollectDbgInfo)?
- **Answer needed:** the waits and their exit conditions.

### R9. Direct3D 9 games on a driver without a D3D9 DDI
- **Question:** on Windows 11 ARM64 build 22621, what does the D3D9 runtime do when the WDDM driver's UMD exports
  only the D3D10/11 entry points?
  - Is there any built-in D3D9 path other than D3D9On12 (which needs a D3D12 driver)?
  - What is the minimum a UMD must export and implement for D3D9 (OpenAdapter and the D3D9 DDI), for both 64-bit
    and 32-bit (WOW64) processes?
  - Does anything decide automatically between a native D3D9 DDI and a mapping layer (registry, INF, caps)?
- **Answer needed:** the decision logic, and the required exports and registry values.

### R10. x64 (emulated) apps on ARM64 cannot use our GPU
- **Question:** how the runtime chooses and loads a UMD into an x64-emulated process on ARM64. Is it an ARM64EC or
  ARM64X DLL, and which registry values name it (alongside UserModeDriverName / UserModeDriverNameWow)? What must
  the DLL export?
- **Answer needed:** the loader rules, the registry and INF value names, and the build requirements (ARM64X,
  exports).

### R11. Photos plays video at about 5 fps
- **Seen:** the Photos app is not CPU-bound; it waits on something every frame (notes/g17-dxva-mfc.md ~298, ~348).
  mpv runs smoothly on the same stack.
- **Question:** what Photos' video path waits on per frame (DirectComposition / Media Foundation / DXGI present
  statistics or fences), and which driver capability or answer it needs.
- **Answer needed:** the wait and what satisfies it.

### R12. Edge / Chromium GPU process on a feature-level 10.1 device
- **Seen:** the Edge GPU process dies at a CHECK in skia_output_surface_impl.cc when ES3-on-FL10_0 is forced, a
  diagnostic flag only (notes/skipped-windows-internals.md 2026-10-16).
- **Question:** what Edge/ANGLE need from a D3D11 device (feature level, formats, caps) to stay on hardware
  composition natively at FL 10.1.
- **Answer needed:** the caps list and the failing check's condition.

### R13. Mongoose (M4) cores
- See [mongoose-research-guide.md](mongoose-research-guide.md): open questions, safe experiments, hand-back
  template.

### R14. CPU frequency and temperature in Task Manager
- **Seen:** Task Manager shows a "base speed" of 1.79 GHz and a fixed current speed. The real clocks are set by our
  S10ePower driver through the firmware (ACPM), outside Windows' processor power management (notes/clocks-dvfs.md).
- **Question:** where Windows (processor power management / Task Manager) gets current and base CPU frequency on
  ARM64: ACPI _CPC / CPPC, PCC, _PSS, the firmware SMBIOS. What our ACPI tables or a driver must provide so the
  real clocks and temperatures show natively.
- **Answer needed:** the data source per field; the documented ACPI objects to add.

## 3. How to answer

One block per request, in a file called `research-answers.md` (the owner passes it back). Example:

```
## R1 - <one-line answer>
Confidence: observed (debugger/trace) | read in code | documented | inferred
Windows build checked: 22621.xxxx ARM64
Answer:
- <facts: function / DDI names, order of operations, conditions, error codes, registry values>
What our side should change:
- <the KMD / UMD / INF / ACPI change this implies, in plain terms>
Sources:
- <learn.microsoft.com URL + quoted sentence, public symbol names, trace excerpts>
Caveats / not checked:
- <...>
```

- Prefer documented sources (learn.microsoft.com, WDK headers, public symbols) and say so when an answer comes
  from them.
- State "not found" plainly. A precise "no" helps as much as a "yes".
- Do not include MAC or IP addresses (project rule).
- Do not modify the phone or the repo. Experiments on the phone go through the owner (the phone recovery rules are
  in AGENTS.md sections 8-10).
