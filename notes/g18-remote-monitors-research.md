# G18 research: remote monitors (another device as extra/mirrored display)

Date: 2026-10-18. Research only, no phone access, no code changes.
Context: Win11 Pro ARM64 22621 on S10e; our WDDM 2.0 KMD MaliG76Wddm (render + panel display, FL 10_1 via
Mesa d3d10umd); our BCM4375 Wi-Fi (station only). Desktop Duplication suspected in system resets
(notes/g27-x64-apps.md lines ~159-217: 4 resets, all in boots where screenrec -dup had run). INFERRED = my inference.

## Progress log
- 2026-10-18: file created; starting learn.microsoft.com IddCx research.

## 1. IddCx (Indirect Display Driver) - Microsoft docs

### 1.1 What it is (docs)
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview
  - "The indirect display driver (IDD) model provides a simple user-mode driver model to support monitors that aren't
    connected to traditional GPU display outputs." Scenarios listed: "Streaming the display output over a network to a
    remote client (remote display)." and "Creating virtual monitors for applications such as virtual desktop
    environments (virtual displays)."
  - "An IDD is the third party-provided UMDF driver for the device."
  - "The IDD runs in Session 0 without any components running in the user session, so any driver instability doesn't
    affect the stability of the system as a whole."
  - "The IDD uses a user-mode model and doesn't support kernel-mode components. With this model, the driver can use any
    DirectX APIs to process the desktop image. IddCx provides the desktop image to encode in a DirectX surface."
  - "Build the IDD as a universal windows driver so you can use it on multiple Windows platforms."
  - "This sample demonstrates how to connect a monitor, how to respond to a mode set, and how to receive frames."
  -> No kernel code needed from us; the IDD is a UMDF DLL (architecture-neutral source -> ARM64 build is a target
     switch). INFERRED.

### 1.2 Version on our build (22621)
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iddcx-versions
  - Table: "22621 | SV2 | Windows 11 22H2 | 1.9 | 0x1900".
  - "1.5 | 0x1500 | Add IddCxSwapChainInSystemMemory and IddCxSwapChainReleaseAndAcquireSystemBuffer"
  - "1.4 | ... Add support for remote session ID drivers, ..., IddCxAdapterSetRenderAdapter"
  - "1.10 | 0x1A00 | Add HDR10 and SDR Wide Color Gamut (WCG) support - Windows 11 23H2"; 1.11 adds D3D12.
  -> Build the IDD against IddCx <= 1.9 (or 1.4-1.8 to be safe), not the newest WDK default; a driver declaring a
     newer IddCx than the OS has will not load. INFERRED from "The UMDF IDD declares the version of IddCx it was built
     against. The OS ensures that the correct version of IddCx is loaded" (overview page).

### 1.3 Render adapter
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/iddcx/nf-iddcx-iddcxadaptersetrenderadapter
  - "IddCxAdapterSetRenderAdapter sets the preferred render adapter on which to render the swapchains for the
    specified adapter."
  - "The OS will use the ID driver's preferred render adapter where possible. In some cases (for example, when the
    preferred render adapter is PnpStopped), the OS may choose a different adapter. The OS passes the actual render
    adapter used for a swapchain to the driver in the EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN callback."
  -> DWM composes the IDD monitor's desktop on a render adapter (on the phone: MaliG76Wddm, or WARP/Basic Render if
     the OS falls back). The IDD gets the frame as a D3D11 texture shared from that adapter. INFERRED.

### 1.4 Swapchain -> frames (docs)
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/iddcx/nc-iddcx-evt_idd_cx_monitor_assign_swapchain
  - "The OS calls EvtIddCxMonitorAssignSwapChain so that the IDD can setup its frame processing mechanism. For example,
    an IDD might create a new thread to initialize D3D devices and then loop on processing frames"
  - "IddCx will immediately bugcheck the driver if EvtIddCxMonitorAssignSwapChain reports an error code other than
    STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN." ("bugcheck the driver" = UMDF host crash/restart, not a
    system bugcheck - INFERRED from the Session-0 isolation sentence above.)
  - "if a driver encounters an error while creating the D3D device on the render adapter it could call
    IddCxAdapterSetRenderAdapter to change to another adapter (perhaps another GPU or WARP) and then return
    STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN. The OS will create a new swapchain on the new render adapter"
    -> WARP is an officially mentioned fallback render adapter: if Mali sharing misbehaves, the IDD can push
       composition of the virtual monitor to WARP (CPU) - slow but avoids our KMD/UMD. INFERRED feasibility.
  - "The resolution of the surfaces in the swapchain will always be the same resolution as the target mode set."
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/iddcx/nf-iddcx-iddcxswapchaininsystemmemory
  - "When IddCxSwapChainInSystemMemory returns TRUE in pInSystemMemory, the driver can use either
    IddCxSwapChainReleaseAndAcquireBuffer or IddCxSwapChainReleaseAndAcquireSystemBuffer" (IddCx 1.5+, so on 22621)
    -> with a system-memory swapchain the encoder gets a CPU pointer, no GPU readback needed. INFERRED.

### 1.5 Microsoft sample (github microsoft/Windows-driver-samples video/IndirectDisplay)
- README: "a sample IddCx 1.4 driver and a sample application that instantiates a software device using it."
  The driver "enumerate[s] a single monitor when its device enters the D0/started power state."
  (Driver.cpp actually has IDD_SAMPLE_MONITOR_COUNT = 3 with hard-coded EDIDs; default 1920x1080@60 for EDID-less.)
- IddSampleDriver.vcxproj: platforms x64 and **ARM64**; IDDCX_VERSION_MAJOR 1 / MINOR 4 (preprocessor shows 1.6);
  UMDF 2.25; IndirectDisplayDriver=true; DynamicLibrary. -> ARM64 is already a solution platform: build with
  msbuild /p:Platform=ARM64 (WDK + VS ARM64 tools). Both 1.4 and 1.6 are <= 1.9 (22621) -> loads on our build. INFERRED.
- Driver.cpp frame loop: Direct3DDevice::Init finds the adapter by the swapchain's RenderAdapterLuid and calls
  D3D11CreateDevice with D3D11_CREATE_DEVICE_BGRA_SUPPORT ("BGRA support is required"); SwapChainProcessor::RunCore:
  IddCxSwapChainSetDevice -> loop IddCxSwapChainReleaseAndAcquireBuffer (waits on hNextSurfaceAvailable, 16 ms) ->
  "TODO: Process the frame here ... GPU copy to another buffer surface ... GPU encode operation" ->
  IddCxSwapChainFinishedProcessingFrame. So the sample gives a working virtual monitor that discards frames; the
  streaming part is ours to add at the TODO.
- IddSampleApp/main.cpp: SwDeviceCreate(L"IddSampleDriver", L"HTREE\ROOT\0", ...) with hardware id
  "IddSampleDriver"; device lives while the app runs, SwDeviceClose removes it. (devcon/pnputil root-enum also works.)
- Signing: UMDF driver package; test-signing is on on the phone, so a test-signed catalog is enough. INFERRED
  (same as our other drivers).

### 1.6 Third-party IDD with ARM64 builds
- https://github.com/VirtualDrivers/Virtual-Display-Driver : "ARM64 Support in Windows 11 24H2 or later may require
  test signing be enabled." Uses IddCx "1.10 (latest)" -> its prebuilt ARM64 binary likely will NOT load on 22621
  (IddCx 1.9). INFERRED. Not useful prebuilt; the source shows ARM64 IDD builds are routine.

- Progress: IddCx docs + sample done (2026-10-18). Next: spacedesk, Miracast, MICE, RDP, Parsec/Sunshine.

## 2. Existing products / OS features

### 2.1 spacedesk (datronicsoft)
- https://www.spacedesk.net/download/ : "Windows 11 (ARM64) ... msi Version: 2.2.33" (download link /downloadarm64),
  next to "Windows 11 (64-bit)" (/downloadidd64) and "Windows 10 (32-bit)" (/downloadidd32). Installer name per
  search results: spacedesk_driver_Win_11_ARM64_v2233.msi.
- https://www.spacedesk.net/support-forum/topic/windows-on-arm64-support/ (2024-10-16, datronicsoft staff):
  "Spacedesk server for ARM64 is available now."
- https://manual.spacedesk.net/SystemRequirements1.html : "Windows 11 / 10 (version 1607 - 26H1)"; "spacedesk
  supplies additional Display monitors on Primary Machine. These can mirror once screen or extend the Desktop";
  the driver software handles "screen content, compressing it and transmitting it over the LAN".
- Technology: not stated in the docs. The "idd64"/"idd32" download names point to an IddCx indirect display driver
  (INFERRED); with an IDD, both extend and Windows "Duplicate" (clone) are DWM compositions into the IDD swapchain,
  so no Desktop Duplication API is needed (INFERRED - not confirmed by vendor). Receivers: Windows/Android/iOS apps
  and an HTML5 web viewer (any browser) - viewer side per spacedesk site (not re-verified here).
- Needs from us: an ARM64 IddCx-capable OS (22621 = IddCx 1.9 - fine), a render adapter DWM can compose the extra
  monitor on (MaliG76Wddm, D3D11 sharing between DWM and the UMDF host's D3D device), CPU/GPU for its encoder.
  Signing: production-signed MSI, so test-signing irrelevant. Its ARM64 package says "Windows 11"; whether it
  installs on 22621 (vs 24H2-only like VirtualDrivers' VDD) is UNVERIFIED - only a try tells.
- Effort: zero code; install MSI on the phone, viewer on PC/tablet. Cheapest PoC.

### 2.2 Miracast ("Cast", Win+K) - phone as SOURCE
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wireless-displays--miracast-
  - "The Miracast standard is based on Wi-Fi Direct technology and uses the Wi-Fi Direct connection to establish a
    peer-to-peer connection between the source device and the display."
  - "Starting in Windows 10 (WDDM 2.0), the operating system ships with a built-in Miracast stack that can work on any
    GPU. Driver developers should no longer implement a custom Miracast stack."
  -> No Miracast DDIs needed in MaliG76Wddm (the WDDM 1.3 DXGK Miracast DDIs are the deprecated custom-stack path).
     GPU side is OK in principle. INFERRED: the OS encoder is Media Foundation H.264; with no HW encoder MFT on the
     phone it must use a software encoder (performance UNVERIFIED).
  - The blocker is Wi-Fi: our BCM4375 driver is station-only -> no Wi-Fi Direct -> Win+K finds nothing.
- Miracast over Infrastructure (MS-MICE):
  - https://learn.microsoft.com/en-us/surface-hub/miracast-over-infrastructure : "No changes to current wireless
    drivers or PC hardware are required." but also "Users attempt to connect to a Miracast receiver through their
    Wi-Fi adapter. When the list of Miracast receivers is populated, Windows identifies that the receiver can support
    a connection over the infrastructure." "Open TCP port: 7250." "A Windows PC or phone can act as a Miracast over
    Infrastructure source." Source "must be connected to the same enterprise network via Ethernet or a secure Wi-Fi
    connection".
  - https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-mice/ab6341b7-4fc7-41fd-a74d-3fe023455482 :
    "A Miracast over Infrastructure session consists of three phases: device discovery, host name resolution, and
    projection." "Each Sink advertises its capabilities by transmitting Beacon and Probe Response frames that might
    include WSC IE Vendor Extension attributes".
  -> MICE only moves the STREAM to the infrastructure link; DISCOVERY is still Wi-Fi P2P beacon/probe based, so the
     source's WLAN driver must still expose Wi-Fi Direct device discovery to Windows. "No changes to drivers" means
     drivers that already have Wi-Fi Direct. INFERRED from the spec text; there is no manual-IP entry in the Windows
     Cast UI (INFERRED). => Miracast/MICE both need Wi-Fi Direct (at least P2P discovery) in our BCM4375 driver.
- Effort: Wi-Fi Direct in a WDI/NDIS driver for BCM4375 FullMAC (P2P device port, discovery, GO negotiation for plain
  Miracast) = large (weeks+); plus receiver hardware (TV/dongle or a Windows PC running "Wireless Display" app).
  Not a cheap PoC.
- Receiving side note ("Project to this PC" on another Windows PC = receiver) is the other direction and also needs
  Wi-Fi Direct on the phone source.

#### 2.2 addendum - Miracast source requirements (docs, decisive)
- https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/wireless-projection-pc-manufacturers
  - "The Microsoft Miracast stack functions upon WDDM 2.0 (Window's Display Driver Model) or greater. ... A WDDM 2.0
    or higher driver is required." -> MaliG76Wddm (WDDM 2.0) meets this line.
  - "A Miracast sender's graphics device/driver must support a hardware accelerated AVC (H264) Encoder." /
    checklist "Hardware accelerated AVC (H264) Encode | Required". -> We have NO hardware H.264 encoder (the Exynos
    MFC video codec block has no Windows driver/MFT). Second hard blocker besides Wi-Fi Direct. Whether Windows
    falls back to a software encoder when there is none is not stated; INFERRED: the "Cast" UI reports the device
    doesn't support Miracast.
  - "Wi-Fi Driver Type: Ensure that Wi-Fi adapter manufacturers provide drivers specifically designed for the Windows
    10 Wireless Driver Interface (WDI)."
- https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/wireless-projection-wifi-device-manufacturers
  - "Ensure that the Wi-Fi driver declares capability support for each of these functions through WDI: Wi-Fi Direct
    Device, Wi-Fi Direct GO, Wi-Fi Direct Client, P2P Device Discovery, Extended Channel Switch Announcement, Miracast
    Sink, P2P GO ports count, P2P Clients Port Count"; checklist "Wi-Fi Driver Type | WDI Driver model | Required".
- Verdict Miracast/MICE: needs (a) Wi-Fi Direct (WDI P2P device + discovery, GO/client for plain Miracast) in our
  BCM4375 driver and (b) a hardware H.264 encoder MFT (MFC driver). Both large. Not a PoC path. Does NOT need
  Desktop Duplication or WDDM Miracast DDIs.

### 2.3 Remote Desktop (phone = RDP host, Win11 Pro has the host)
- https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/mstsc
  - "/multimon | Configures the Remote Desktop Services session monitor layout to be identical to the current
    client-side configuration." "/span | ... spans the Remote Desktop session across multiple monitors."
  - "/shadow:<sessionID> | Specifies the ID of the session to shadow." "/control | Allows control of the session when
    shadowing."
- https://learn.microsoft.com/en-us/azure/virtual-desktop/rdp-properties : "use multimon ... 1: A remote session uses
  multiple displays." applies to "Remote PC connections".
- Graphics: policy "Use hardware graphics adapters for all Remote Desktop Services sessions" - per its policy text
  (quoted via gp.kapilarya.com / Parallels docs, not learn.microsoft.com): "If you disable or skip the configuration of
  this policy setting, all Remote Desktop Services sessions on the RD Session Host server will use the Microsoft Basic
  Render Driver as the default adapter." -> by default an RDP session does not render on MaliG76Wddm at all; no KMD/UMD
  features, no Desktop Duplication, no IddCx work from us. (INFERRED: same default on client SKUs.)
- Limitation: client Windows allows one interactive session; connecting by RDP locks/takes over the console
  session (Microsoft Q&A https://learn.microsoft.com/en-us/answers/questions/4181174/use-remote-desktop-while-also-logged-in-windows-11 :
  "By default, Windows only allows one active session at a time." - community answer, not normative). So RDP gives
  the remote device(s) the phone's desktop (multi-monitor on the client side), but NOT an extra monitor next to the
  phone panel. Shadowing (/shadow) mirrors the console but needs policy + mechanism UNVERIFIED (may use a capture
  path similar to duplication - unknown).
- Effort: zero code; enable Remote Desktop on the phone (Settings > System > Remote Desktop), connect from PC with
  mstsc /multimon. Works today if our Wi-Fi + TCP 3389 are fine. Already a viable "remote screen" today.

### 2.4 Sunshine (LizardByte) + Moonlight client
- https://github.com/LizardByte/Sunshine/releases : release v2026.516.143833: "Added Windows support for ARM64";
  assets Sunshine-Windows-ARM64-installer.msi / -lite.zip.
- README/docs (via search snippet of github.com/LizardByte/Sunshine): "Sunshine supports ARM64 on Windows; however,
  this should be considered experimental. This version does not properly support GPU scheduling and any hardware
  acceleration."
- https://docs.lizardbyte.dev/projects/sunshine/master/ : Windows minimum "Windows: 11+"; capture: "DXGI Desktop
  Duplication" (full support) and "Windows.Graphics.Capture" (partial: "Portable" variant only); software encoding
  available on all platforms.
- Needs from us: Desktop Duplication (default) or WGC (portable mode) -> both are DWM-side capture of our display;
  DD is the suspected reset trigger (g27). Software x264 on the A75/A55 CPUs at 1520x720 (INFERRED feasible at low fps).
  Mirrors the phone panel only; an EXTRA monitor needs a virtual display (IDD) as well.
- Effort: zero code, but rides the risky DD path -> second choice, try WGC mode.

### 2.5 Parsec
- support.parsec.app returned HTTP 403 to the fetcher. Search summary of Parsec hosting docs: "Hosting is currently
  only available on Windows and macOS computers" on 64-bit Windows 10/11; no ARM64 host found. Parsec hosting needs a
  hardware encoder (NVIDIA/AMD/Intel) per common knowledge (UNVERIFIED here). -> Not viable. INFERRED.

### 2.6 Duet Display / Splashtop / others
- Duet: third-party changelog (filehorse.com mirror) mentions a "dedicated hardware video encoder for Snapdragon
  Adreno GPUs on ARM64 Windows devices" -> ARM64 host exists, but tuned for Adreno/Qualcomm; on our phone it would need
  software encode or fail. Not verified on vendor site. Low priority.
- Splashtop Streamer: no ARM64 Windows streamer found (only the On-Prem app mentions "ARM support"). Not viable now.
- DisplayLink (USB, not network, but a non-DP-alt second screen):
  https://www.synaptics.com/sites/default/files/release_notes/2024-11/DisplayLink%20USB%20Graphics%20Software%20for%20Windows11.5%20M2-Release%20Notes.txt :
  "Windows 11 (64-bit of Intel/AMD and Arm64)"; "DisplayLink for Windows on Arm64 is available on all packages except
  MSI." Requires working USB host in Windows (DRIVERS-MILESTONES D1/D3 still "doing") and a non-powered setup
  (USB-powered DisplayLink adapters draw bus power - safety rule). DisplayLink's Windows driver is widely understood to
  be IddCx-based (INFERRED). Later option once D3 is validated.

- Progress: products done (2026-10-18). Next: per-path needs/effort table + recommendation.

### 1.7 Encoder for an own IDD streamer (docs)
- https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder : "The Microsoft Media Foundation H.264
  video encoder is a Media Foundation transform"; input "MFVideoFormat_I420 / IYUV / NV12 / YUY2 / YV12"; output
  "MFVideoFormat_H264"; "If a certified hardware encoder is present, it will generally be used instead of the inbox
  system encoder"; DLL "Mfh264enc.dll"; "The encoder defaults to slice encoding, to minimize latency."
  -> An inbox SOFTWARE H.264 encoder exists for our own IDD/streamer (needs BGRA->NV12 conversion first; CPU or a
     D3D11 pixel shader on Mali). Availability of Mfh264enc.dll on ARM64 22621 Pro: INFERRED yes (inbox MF); check
     with `dir C:\Windows\System32\mfh264enc.dll` on the phone.

### 1.8 Our-driver facts relevant to IddCx (from repo notes)
- notes/gpu-full-adapter-hang.md run g52full (Mesa 0037): "shared handles now real on the Mali adapter
  (GetSharedHandle ..., CreateSharedHandle ...); composition buffers B8G8R8A8 + R8G8B8A8 S_OK (out1 NT handle, out2
  object)". DWM also already opens a second HWDEVICE on the Basic Render Driver for some content.
  -> The D3D11 shared/NT-handle machinery that DWM -> IDD swapchain handoff relies on exists in our UMD (INFERRED: the
     IddCx swapchain buffers are DWM composition-type surfaces opened in the WUDFHost device). Feature level 10_0/10_1
     is enough for D3D11CreateDevice + BGRA_SUPPORT in the sample (no FL requirement stated in IddCx docs; INFERRED).
- Risk: first-ever second display path in DWM on our stack (multi-monitor composition, mode set on the IDD adapter,
  clone mode). Any hang there is a GPU/DWM hang on Mali, not isolated by UMDF. Mitigation documented by MS:
  IddCxAdapterSetRenderAdapter to WARP (section 1.4).

## 3. Per-path summary: needs from OUR drivers, effort, verdict

| Path | Extra/mirror | Needs from our KMD/UMD | Needs from Wi-Fi | Desktop Duplication? | Effort | Verdict |
|---|---|---|---|---|---|---|
| RDP (phone = host, mstsc /multimon) | remote replaces console; multi-mon on client | none (session renders on Basic Render Driver by default) | TCP only (station OK) | no | 0 code, minutes | works now as "remote screen"; not an extra monitor |
| spacedesk ARM64 MSI 2.2.33 | extend + mirror | render adapter for DWM to compose the IDD monitor (shared surfaces, now working), multi-monitor DWM | TCP/UDP LAN (station OK) | no (INFERRED: IDD-based) | 0 code, ~1 h test | best cheap PoC of a TRUE extra monitor; 22621 install UNVERIFIED |
| Own IDD (MS IddSampleDriver, ARM64) + own streamer | extend (+clone via Windows) | same as spacedesk | TCP (station OK) | no | monitor appears: 1-2 days; JPEG/TCP or MJPEG-to-browser viewer: +3-5 days; H.264 (Mfh264enc) + input back-channel: +1-2 weeks | the native long-term path; fully under our control |
| Sunshine ARM64 (experimental) + Moonlight | mirror (extend only with an IDD) | DD or WGC capture of our display | TCP/UDP | yes by default (WGC portable as alternative) | 0 code | risky: DD is the g27 reset suspect |
| Miracast / Cast (Win+K), incl. MICE | extend + mirror | WDDM 2.0 (ok) + **HW H.264 encoder (missing)** | **WDI Wi-Fi Direct (missing)**; MICE still uses P2P discovery | no | weeks-months (WDI P2P + MFC encoder driver) | not a PoC path |
| Parsec | - | HW encoder | - | - | - | no ARM64 host found |
| Duet / Splashtop | - | Duet: Adreno HW encoder focus | - | unknown | - | Duet maybe, unverified; Splashtop no ARM64 streamer |
| DisplayLink USB adapter (not network) | extend | render adapter (IDD-based, INFERRED) | - | no | 0 code once USB host (D1/D3) works | later, after D3 validation; bus-power safety |

### Recommendation (cheapest PoC the user can test on return)
1. RDP from the PC: `mstsc /v:<phone> /multimon` - zero risk to our GPU, proves the remote-screen use case today
   (but the phone panel locks). 5 minutes.
2. spacedesk ARM64 driver MSI on the phone + spacedesk viewer (PC app, Android/iOS app or the HTML5 browser viewer) -
   first real "extra monitor": appears in Display Settings, Extend/Duplicate. Watch for: MSI refusing 22621, DWM
   hangs/resets on the second display (record with the usual reset-log procedure), CPU load of its encoder.
3. If spacedesk fails or is unacceptable: build microsoft/Windows-driver-samples video/IndirectDisplay for ARM64
   (IddCx 1.4/1.6 <= 1.9 OK), test-sign, install via IddSampleApp -> verify a virtual monitor appears; then add a frame
   sink at the "TODO: Process the frame here" (copy to staging, BGRA->NV12, Mfh264enc or JPEG, send over TCP).
   Note the sample uses 3 monitors by default - set IDD_SAMPLE_MONITOR_COUNT = 1 for the PoC.
4. Avoid Sunshine/any DD-based capture until the g27 duplication reset is understood.
5. Miracast: drop for now (needs Wi-Fi Direct WDI + HW H.264 encoder).

- Progress: research complete 2026-10-18.
- 18:15 IddCx PoC test: wdt image first (first DWM second output on our stack)

## PoC log (main session)
- 2026-10-18 IddCx sample vendored as drivers/S10eIdd (MS-PL, LICENSE-MS-PL.txt; changes marked "exynos-woa": 1
  monitor instead of 3, IddSampleApp SECONDS keep-alive mode). Build: VS 18 + WDK 26100 header warnings (C6553
  wdfmemory.h, C28193 iddcx.h) under the sample's /WX + /analyze -> build-package.ps1 sets _CL_=/WX- /analyze-
  (appended after the project options; CL= comes before them and loses). Package s10e-artifacts\s10eidd-poc
  (dll 7EC9CE5E, test-signed, Inf2Cat 10_NI/GE_ARM64).
- Device test (watchdog-1 image boot-host-r2-wdt.img for safety): pnputil /add-driver, IddSampleApp 90 as a SYSTEM
  task (tools/phone/rec/iddrun.cmd): user session sees \.\DISPLAY2 2560x1440 at X=1520 (extended), adapter
  "IddSampleDriver Device" OK, monitor "Generic Monitor (S2719DGF)" (sample EDID); GDI shot of the 4080x1440 virtual
  desktop (traces/rec/idd-virtual.png): wallpaper + taskbar composed on the virtual monitor -> DWM drives a second
  (indirect) output on MaliG76Wddm. After 90 s the device left cleanly (one display, same dwm pid, no reset).
  => spacedesk-style and our own remote monitor are viable. Next: frame sink (driver -> file/shared memory ->
  S10eIddView HTTP MJPEG server) so another device on the LAN sees the virtual monitor in a browser.
