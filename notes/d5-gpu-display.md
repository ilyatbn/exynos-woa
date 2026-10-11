# D5 — GPU / display on the Exynos 9820: research notes

Plan-of-record task list: [`docs/exynos9820-port/DRIVERS-MILESTONES.md`](../docs/exynos9820-port/DRIVERS-MILESTONES.md) §D5.
Findings appended as discovered; facts carry source URLs; inference marked; dead
ends recorded.

## What this repo already has (do not re-research)

- `SimpleFramebufferDxe` in Beyond0LtePkg: GOP over the sboot-lit framebuffer at
  `EXYNOS_FB_BASE 0xCA000000` (1440x3040) — UEFI already draws.
- The boot shim paints DECON directly (`0x1281`->`decon_f 0x19030000` enable, from
  edk2-exynos `PlatformUtils.c`) — prior art for DECON register access.
- Combo PHY facts (`has_combo_phy=1`, `displayport@130B0000`) in
  `notes/dock-desktop.md` §11 — D6 (external display) owns the DP-alt-mode half.
- MILESTONES M8 open task: "Fix the framebuffer RAM/DEV overlap" (`EXYNOS_FB_BASE`
  published MMAP_DEV inside a bank0 RAM gap) — interacts with any fb work.

## Questions the research must answer

1. **Mali-G76 (Bifrost, 11-core G76?) on Windows ARM64** — does ANY WDDM driver
   exist or has anyone run a Mali GPU under Windows on ARM (Mu-Silicium status,
   edk2-exynos, WoA community)? ARM's own Windows Mali driver availability
   (NDA/partner-only? the "Mali Display Processor driver for Windows" is DP, not
   GPU — confirm the distinction).
2. **What do the Exynos Linux ports use for GPU** — panfrost vs lima vs Samsung
   blob for the 9820's Mali-G76; what kernel/driver does
   `chiffathefox/exynos-9820-mainline-linux` / postmarketOS ship; is the G76
   Bifrost generation panfrost-supported and to what degree (features/bugs)?
3. **What does Mu-Silicium do for display under Windows on Samsung targets** —
   SimpleFbDxe-style framebuffer only? Any DECON/DSI driver work? What does
   Status.md say for Display on beyond1lte/starlte?
4. **Windows basic display on a framebuffer** — what is the realistic Windows
   half: a WDDM "Indirect Display" or basic fb driver? What do Snapdragon WoA
   ports without GPU drivers show on screen (Mu on Lumia 950 = ?)? Is there a
   maintained simplefb->WDDM shim (e.g. the "BasicRender"/framebuffer WDDM
   samples)?
5. **Verdict inputs** — cost/benefit: is any GPU acceleration reachable on this
   SoC under Windows at all, or is the honest deliverable "2D framebuffer
   display, no acceleration"?

## Progress log

### D1 — Mu-Silicium status rows + Update Catalog probe

(fact) Mu-Silicium `Status.md` (github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md,
fetched 2026-10-10): every Samsung Exynos device with a Windows OS-status table shows
**Display ✅ / GPU ❌**:
- `beyond1lte` (S10, Exynos 9820): UEFI Display ✅, Windows Boot ❌, state *Inactive*.
  No OS-status table at all (Windows doesn't boot there yet). "Only 23H2 Windows Builds
  and Lower are able to Boot" note sits on the Exynos 9820 section.
- `starlte` (S9, Exynos 9810): UEFI Display ✅, Windows Boot ✅ but "Only Windows PE boots";
  OS status Display ✅, GPU ❌.
- `x1s` (S20 5G, Exynos 990/Mali-G77): Windows Boot "WinPE Only" ✅; OS status Display ✅, GPU ❌.
- `gtaxllte` (Tab A 2016, Exynos 7870/Mali-T830): Windows boots; Display ✅, GPU ❌.
- `e1s` (S24, Exynos 2400/Xclipse 940 RDNA3): UEFI Display ✅, Windows Boot ❌.
So Mu-Silicium ships working *UEFI display* on Mali-based Samsung SoCs and zero GPU under
Windows — display works without any GPU driver. (inference: that display must be GOP/framebuffer.)

(fact) Microsoft Update Catalog search `Mali WDDM ARM64` → "We did not find any results"
(https://www.catalog.update.microsoft.com/Search.aspx?q=Mali+WDDM+ARM64). No Mali WDDM
driver is distributed via Windows Update for ARM64.

(fact) Catalog search `Mali` alone → 73 hits, **all** "Windows Malicious Software Removal
Tool" (https://www.catalog.update.microsoft.com/Search.aspx?q=Mali). i.e. the Update
Catalog contains **zero** ARM-Mali GPU drivers for any Windows edition/arch — no vendor has
ever shipped a Mali display/GPU driver through Windows Update. (inference: consistent with
no Mali WDDM ever existing publicly.)

(fact) Mu-Silicium repo tree (api.github.com/repos/Project-Silicium/Mu-Silicium/git/trees/main?recursive=1,
3083 paths, not truncated): the only display-related entries are
`Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/{SimpleFb.c,SimpleFbDxe.inf}`,
`Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib/*`, and Qualcomm-only
`QcomPkg/Drivers/DisplayReEnablerDxe` + `Include/Protocol/EFIDisplay{Pwr,Utils}.h`.
**No DECON/DSI/LCD driver code exists in the tree** — Samsung display = SimpleFbDxe over a
bootloader-lit framebuffer (same shape as our SimpleFramebufferDxe).

(fact) Mu `SimpleFbDxe/SimpleFb.c` (raw.githubusercontent.com/Project-Silicium/Mu-Silicium/main/Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/SimpleFb.c):
locates a "Display Reserved"/"Display_Reserved" memory region from the memory map, takes
width/height/depth from PCDs (`PcdFrameBufferWidth/Height/ColorDepth`, min 640x475),
installs a single-mode GOP (`MaxMode = 1`, `FrameBufferSetMode` = no-op `EFI_SUCCESS`) with
Blt via `FrameBufferBltLib`. Zero display-controller programming — the panel must already be
lit by the Android bootloader. This is exactly the model our repo already follows.

(fact) Snapdragon GPU history: the full Wikipedia list of Snapdragon SoCs
(https://en.wikipedia.org/wiki/List_of_Qualcomm_Snapdragon_processors, fetched 2026-10-10)
contains **zero** "Mali" mentions; every 3D-capable Snapdragon uses Adreno (pre-Adreno parts
say "Stargate 3D"/"Defender 3D" — Imageon-derived, still not Mali; some low-ends "No 3D").
Compute/PC parts: 8cx = Adreno 680, 8cx Gen 2 = Adreno 690, 8cx Gen 3 = Adreno 695,
Microsoft SQ1 = Adreno 685. (fact) No Snapdragon has ever shipped with a Mali GPU, so no
Windows-on-Snapdragon device has ever needed a Mali WDDM.

### D2 — Q2: Linux side (panfrost / 9820 ports)

(fact) Mesa panfrost status table (https://docs.mesa3d.org/drivers/panfrost.html, fetched
2026-10-10): **Mali-G76 is Bifrost v7** (3rd-gen), same row as G31/G51/G52. Supported:
GLES 3.1 (conformance footnote shown for G52), OpenGL 3.1, **Vulkan 1.3 (PanVK)**, OpenCL 3.0
(via Rusticl). Midgard v4/v5 have no Vulkan. Utgard (Mali-4xx) uses Lima, not panfrost.
Also: "Mali GPUs are 3D-only and do not include a display controller, Mesa uses **kmsro** to
support display controllers paired with Mali GPUs" — i.e. on Linux the G76 needs the SoC's
own DRM display driver (exynos DECON/DSI) + kmsro glue; panfrost never drives the panel.

(fact) `chiffathefox/exynos-9820-mainline-linux` (github.com/chiffathefox/exynos-9820-mainline-linux,
fork of torvalds/linux, active — pushed 2026-09-27): branches `exynos9820-7.1-rc2` /
`exynos9820-7.2-rc2` carry Exynos 9820 bring-up on ~Linux 7.2-rc2: clk driver blocks
(MIF/CORE/BUSC/PERI*/FSYS/**DPU**), "clk: exynos9820: add an **empty G3D block**" (GPU clocks
stubbed out), samsung power domains, TMU thermal. **No exynos9820 DTS exists in the tree yet**
(checked `arch/arm64/boot/dts/exynos` on both the default branch and `exynos9820-7.2-rc2`:
files exist for 7870/7885/850/8895/9810/990/2200/autov9 — none for 9820). So mainline 9820
support is SoC-clocks/thermal stage; GPU/display enablement not started. (inference: the
"empty G3D block" shows GPU is deliberately deferred.)

(dead end) postmarketOS: pmaports (gitlab.com/postmarketOS/pmaports, API 2026-10-10) has **no
beyond0lte/beyond1lte/exynos9820 device package** in device/{main,testing,community,archived}
— Samsung entries are only old Utgard/Exynos-3..7 era devices. The pmOS wiki page for the S10
is bot-walled (Anubis) and could not be read; but with no pmaports package there is no
maintained pmOS port for the 9820 today.

(fact) LineageOS Samsung kernel `LineageOS/android_kernel_samsung_exynos9820` (branch
lineage-23.2, pushed 2026-09-20): GPU is **ARM's open-source Bifrost kbase DDK r10p0** at
`drivers/gpu/arm/b_r10p0/` (mali_kbase_*), plus gator profiling. DT node:
`arch/arm64/boot/dts/exynos/exynos9820-mali_common.dtsi`:
`mali: mali@18500000 { compatible = "arm,mali"; reg = <0x0 0x18500000 0x5000>; interrupts
JOB/MMU/GPU; samsung,power-domain = <&pd_embedded_g3d>; gpu_max_clock = <702000>; ... }`
(Samsung vendor binding `arm,mali`, not upstream `arm,mali-bifrost`; DVFS/thermal/TMU
properties are Samsung-specific). Userspace is the matching closed Mali blob (binder r10p0).
Useful constant: **G76 MMIO base 0x18500000** (matches this repo's Exynos9820.h if checked).

### D3 — Q4: Windows display over a plain framebuffer

(fact) Microsoft Learn, "Microsoft Basic Display Driver"
(https://learn.microsoft.com/en-us/windows-hardware/drivers/display/microsoft-basic-display-driver):
*BasicDisplay.sys* is an **in-box generic WDDM display driver** shipped with Windows; "its
primary purpose is to enable Windows to write to the display controller's linear frame
buffer"; "supports UEFI Graphics Output Protocol (GOP)"; "On UEFI platforms, BasicDisplay
inherits the linear frame buffer that is set during boot. In this case, **no mode or
resolution changes are possible**." It is "always used with *BasicRender*", the system module
exposing **WARP** (software rasterizer) as the D3D device — i.e. D3D9/10/11 API surface
exists, rendered on CPU. (fact) Since Win11 both run from DriverStore.

(fact) ARM64 proof it works on ARM: worproject/RPi-Windows-Drivers README
(https://github.com/worproject/RPi-Windows-Drivers) — Windows on ARM64 on Raspberry Pi 3/4:
"Basic Display Adapter (frame buffer) | **MSBDD (Inbox)** | **Working**" on both ARM64 tables,
while the VC4 GPU driver (`roskmd.sys`) is "Not working … unfinished". So a full Windows
ARM64 desktop runs on the inbox fb driver with no GPU driver at all. (The RPi DSI 7" panel
row: "Display: firmware-controlled … only the display works" — same model as DECON-lit fb.)

(fact) Mu-Silicium corroborates on Samsung targets: starlte/x1s/gtaxllte Windows OS-status
tables show Display ✅ / GPU ❌ (see D1) — their SimpleFbDxe GOP + inbox BasicDisplay is the
working display path today. (inference) Our SimpleFramebufferDxe at 0xCA000000 is the exact
equivalent; Windows should light up the S10e panel the same way once boot completes.

(fact) HLK has a "UEFI GOP mode test"
(https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/6afc8979-df62-4d86-8f6a-99f05bbdc7f3)
and WDDM docs require drivers to preserve the GOP-set mode on UEFI systems
(https://learn.microsoft.com/en-us/windows-hardware/drivers/display/plug-and-play--pnp--start-and-stop-cases)
— GOP-inherited framebuffer is a first-class Windows concept, not a hack.

(fact) Indirect Display Driver (IDD) model: "provides a simple user-mode driver model to
support monitors that aren't connected to traditional GPU display outputs"
(https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview);
sample "IddSample" via learn gallery
(https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/indirect-display-driver-sample/).
IDD works on ARM64: Virtual Display Driver README comparison table has an "ARM64 Support"
column and notes "ARM64 Support in Windows 11 24H2 or later may require test signing"
(https://github.com/VirtualDrivers/Virtual-Display-Driver). But IDD creates a *virtual
monitor* fed from another adapter's desktop — it does not own a physical panel; using it to
push frames to DECON would mean an IDD+capture blitter on top of whatever already owns the
fb. Not the primary path.

(fact) The old `display/BasicRender` folder is gone from microsoft/Windows-driver-samples
(contents API 2026-10-10: no `display` dir) — BasicRender itself is the *in-box* WARP
exposer paired with BasicDisplay (see Learn page above), so no sample is needed.

(dead end) No maintained third-party "simplefb→WDDM" shim found on GitHub — none exists
because Windows already ships one in-box (MSBDD). Closest hits were MicaNT (hobby NT-like
x86 OS, blits to GOP fb — not Windows) and IDD clones.

(fact) Contrast case — Lumia 950 (Snapdragon 808/Adreno 418) under Windows ARM64 has real
GPU acceleration because Windows Phone 10 already shipped an Adreno **WDDM** driver that
LumiaWOA reuses: WOA-Project/Lumia-Drivers releases mention "Qualcomm Adreno GPU Driver
UMD component registration", and 2020 announcements "GPU Acceleration is now available.
Adreno(TM)" (https://github.com/WOA-Project/Lumia-Drivers/releases,
https://t.me/s/LumiaWOA_Announcements?before=413, howtogeek/liliputing coverage).
(inference) The reason Qualcomm phones got GPU under WoA and Samsung phones never will:
WP10 was WDDM-native; Android's Mali DDK has no Windows counterpart to port from.

### D4 — Q1 remainder: ARM Windows-driver dead end, stated precisely

(dead end) Could not confirm any public ARM download named "Mali Display Processor driver
for Windows": exact-phrase web search returns nothing; developer.arm.com download pages are
JS-only (fetches return empty/timeout). What IS documented on developer.arm.com (search
snippets 2026-10-10):
- "Mali GPU User-Space Binary Drivers — for GNU/Linux and Android"
  (https://developer.arm.com/downloads/-/Mali%20GPUs%20User-Space%20Binary%20Drivers)
- "Mali Display Drivers — source code for Mali GPUs **Linux X11** Display Drivers"
  (https://developer.arm.com/downloads/-/mali-drivers/display-drivers) — "display" here =
  X11 DDX glue for Mali GPU on Linux, not a Windows display driver.
- "Mali-DP ADF Kernel Device Drivers — open source drivers … on any **Linux or Android**"
  (https://developer.arm.com/tools-and-software/graphics-and-gaming/mali-drivers/dp-adf-kernel)
  — Mali-DP is the Mali-D51/D71/DP650 *display composition* IP; the 9820 does not contain
  Mali-DP (its display is Samsung DECON), so even if a Windows Mali-DP driver existed it
  would be irrelevant to us. (inference, high confidence)
(fact) The GPU/WDDM distinction the task asked about holds: ARM has never offered a Windows
GPU driver for any Mali; all Mali GPU drivers are Linux/Android (DDK) or open-source Mesa
(panfrost).

### D5 — Q5: verdict inputs

Cost of an accelerated Mali path (facts → inference):
- (fact) Mali-G76 = Bifrost v7; the only driver knowledge base is panfrost (reverse-engineered
  ISA, officially blessed by Arm in 2020: "Arm Officially Supports Panfrost … in partnership
  with Collabora", https://www.cnx-software.com/2020-09-18/arm-officially-supports-panfrost-open-source-mali-gpu-driver-development/)
  plus ARM's closed DDK (Linux/Android only) and its open-source kbase kernel module
  (b_r10p0 in the LineageOS tree, see D2).
- (fact) panfrost took Collabora ~2016→2020+ to reach GLES 3.1 conformance on Linux
  (Phoronix progress timeline: https://www.phoronix.com/news/Panfrost-Mali-October-2018 etc.).
- (inference) A "Mali-G76 WDDM" would need: a WDDM miniport (kbase DRM→WDDM rewrite), an
  ARM64 user-mode D3D11/12 driver compiling to Bifrost v7 ISA, and Dxgk integration — i.e.
  porting the entire Mesa+DRM stack to a different OS model. Nobody has ever done this for
  any Mali on any Windows; Arm itself hasn't, despite shipping Windows-relevant IP elsewhere.
  Realistic cost: multi-year specialist team; not a milestone-sized task.
- (fact) G76 core count on 9820: MP11 (S10/S10e; S10+ is MP12) — naming per
  https://www.smartprix.com/bytes/how-to-choose-smartphone-processor-chipset/ (low stakes for
  the driver question either way).

Cost of the framebuffer path (facts):
- (fact) We already have GOP (SimpleFramebufferDxe, 1440x3040 @ 0xCA000000); Windows ARM64
  in-box BasicDisplay+BasicRender turns any GOP fb into a working desktop with WARP — proven
  on ARM64 by RPi-Windows ("MSBDD (Inbox) Working", GPU driver "not working", desktop still
  fine: https://github.com/worproject/RPi-Windows-Drivers) and by Mu-Silicium Samsung ports
  (Display ✅ / GPU ❌ on starlte/x1s/gtaxllte).
- (fact) Limitations are explicit: "no mode or resolution changes are possible" over inherited
  GOP fb (Learn BasicDisplay page); no hardware 3D; D3D via WARP (CPU) only; HLK certification
  would fail ("Verify Post Device Supports Display And Render",
  https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/a635f1f2-5a43-4264-a667-ce6cd1a43338)
  — irrelevant for a hobby port.
- (inference) Remaining real work for D5 is *display plumbing*, not drivers: keep DECON +
  panel + fb refresh alive across sboot→UEFI→Windows handoff, fix the fb RAM/DEV overlap
  (M8 open task), and get mode/rotation sane at 1440x3040 portrait.

## Verdict recommendation

**D5's honest deliverable is 2D framebuffer display (GOP + in-box BasicDisplay/WARP), with
GPU acceleration explicitly declared out of scope — `NotImplemented`, not `Guided`.**

Evidence chain:
1. No Mali WDDM exists, has ever existed, or is distributed anywhere (Update Catalog: zero
   Mali hits; Arm's downloads: Linux/Android only; Mu-Silicium: GPU ❌ on every Samsung
   device; no Snapdragon ever used Mali so no Windows driver ecosystem could piggyback).
2. The framebuffer path is proven working on Windows ARM64 twice over (RPi-Windows MSBDD,
   Mu-Silicium starlte/x1s Display ✅) and our repo already has the required UEFI half.
3. A Bifrost WDDM is a multi-year reverse-engineering port with no upstream to contribute
   to and no community precedent; the mainline Linux effort itself hasn't even started GPU
   enablement ("empty G3D block", D2).
4. Usable OOBE/desktop does **not** require a vendor GPU driver on ARM64: BasicDisplay is a
   WDDM driver in-box, DWM composites, BasicRender exposes WARP for D3D9/10/11. Slow, but
   visible and interactive — exactly what M8/M9 milestones need to progress.

Suggested D5 scope: (a) validate Windows boots to desktop over SimpleFramebufferDxe once M8
completes; (b) close the fb RAM/DEV overlap; (c) DECON resume/blank + backlight control from
UEFI; (d) document "no GPU acceleration" as a permanent known limitation with this note as
the justification. IDD/BasicRender experiments are optional garnish, not the deliverable.

## Sources

- https://github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md
- https://github.com/Project-Silicium/Mu-Silicium (repo tree via api.github.com)
- https://raw.githubusercontent.com/Project-Silicium/Mu-Silicium/main/Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/SimpleFb.c
- https://www.catalog.update.microsoft.com/Search.aspx?q=Mali+WDDM+ARM64
- https://www.catalog.update.microsoft.com/Search.aspx?q=Mali
- https://en.wikipedia.org/wiki/List_of_Qualcomm_Snapdragon_processors
- https://docs.mesa3d.org/drivers/panfrost.html
- https://github.com/chiffathefox/exynos-9820-mainline-linux (branches/commits via api.github.com)
- https://gitlab.com/postmarketOS/pmaports (tree API)
- https://github.com/LineageOS/android_kernel_samsung_exynos9820 (tree + dtsi raw)
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/microsoft-basic-display-driver
- https://support.microsoft.com/en-us/windows/hardware/display-graphics/microsoft-basic-display-adapter-in-windows
- https://github.com/worproject/RPi-Windows-Drivers
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview
- https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/indirect-display-driver-sample/
- https://github.com/VirtualDrivers/Virtual-Display-Driver
- https://github.com/WOA-Project/Lumia-Drivers/releases
- https://t.me/s/LumiaWOA_Announcements?before=413
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/plug-and-play--pnp--start-and-stop-cases
- https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/a635f1f2-5a43-4264-a667-ce6cd1a43338
- https://developer.arm.com/downloads/-/Mali%20GPUs%20User-Space%20Binary%20Drivers
- https://developer.arm.com/downloads/-/mali-drivers/display-drivers
- https://developer.arm.com/tools-and-software/graphics-and-gaming/mali-drivers/dp-adf-kernel
- https://www.cnx-software.com/2020-09-18/arm-officially-supports-panfrost-open-source-mali-gpu-driver-development/
- https://www.phoronix.com/news/Panfrost-Mali-October-2018
- https://www.smartprix.com/bytes/how-to-choose-smartphone-processor-chipset/

---

## Addendum (2026-10-10, after the D5 verdict): the S9 WDDM report + libmali-rockchip

User report: the person behind the S9 WoA project **has WDDM running** on their
device, code not released. Treat as UNVERIFIED prior art that changes the D5
"permanently out of scope" framing to "out of scope today, prior art exists".
(Open question to the user: full GPU WDDM, or a display-only WDDM miniport? That
distinction decides everything below.)

### What tsukumijima/libmali-rockchip actually is (fetched)

- https://github.com/tsukumijima/libmali-rockchip — a **mirror of Arm's closed
  userspace Mali DDK** (`lib/` = prebuilt aarch64 **ELF** .so blobs, Vulkan ICD
  data, `firmware/g610/` = GPU-internal firmware binaries) packaged for
  Rockchip Linux; itself a mirror of https://github.com/JeffyCN/mirrors/tree/libmali.
- License: Arm EULA `LES-PRE-20769` — redistribution allowed **with the licence
  text**, and an explicit **no reverse-engineering / no decompiling** clause.
- *(fact)* It works on Linux only because the **kernel side (kbase) is open
  source** there: closed userspace DDK + open kbase module is the standard Arm
  delivery model.
- *(inference, high confidence)* Useless **as binaries** for Windows: ELF .so
  cannot load as a PE DLL, and the DDK's syscall surface is Linux kbase
  ioctls/mmap. Its value to us is the **model**, not the bits.

### What the 9820 already gives us that Rockchip has to scrounge for

- *(fact, from D2 above)* The LineageOS exynos9820 kernel ships **Arm's
  open-source Bifrost kbase `b_r10p0` in full source** (`mali@18500000`,
  max 702 MHz). The kernel-side driver — the part a WDDM port needs — is
  already ours, in source, for this exact GPU.
- *(fact)* G76 = Bifrost v7, panfrost-supported (GLES 3.1 / Vulkan via PanVK,
  docs.mesa3d.org) — an **open** userspace stack exists, and Mesa builds for
  Windows ARM64 (softpipe/llvmpipe at least).
- *(open)* The G76 GPU-internal firmware .bin (the analogue of libmali's
  `firmware/g610/`): on Android it lives in the vendor image or embedded in
  the kbase module — closable with a device/vendor read.

### The three ladder rungs (recorded so the next session does not re-derive them)

1. **Display-only WDDM miniport for DECON** (no 3D): modeset, power management,
   multi-monitor — and the enabler for D6 DP alt mode regardless of GPU.
   Medium effort, no closed code involved. Possibly exactly what "got WDDM
   running" means on the S9.
2. **Full acceleration via open source only**: port kbase r10p0 (Linux DRM ->
   DXGK) + run **panfrost/Mesa** userspace on Windows ARM64. Both sides open,
   no EULA problem. Multi-month; the glue (DRM->WDDM buffer/submit path) is
   the hard part.
3. **Closed DDK userspace port**: DEAD END — Arm EULA forbids RE, and there is
   no Windows build to obtain. Do not plan around it.

### Legal flag (must record, not decide)

kbase is GPL2 (Arm's DDK releases are); a Windows kernel-mode derivative is a
distribution question the project owner must decide before shipping, even
though reading it for a clean-room DXGK port is fine. Mu-Silicium's EDK2 half
is BSD for the same reason.

### D5b(a) inventory — kbase source in OUR trees (2026-10-10, mechanical counts)

- *(fact)* The 9820 LineageOS kernel carries **seven DDK branches** under
  `drivers/gpu/arm/`: `b_r10p0, b_r12p0, b_r14p0, b_r16p0, b_r19p0, b_r26p0,
  bv_r32p1` (plus `exynos/`). Sizes: `b_r10p0` = 234 C files / ~2.5 MB source;
  `bv_r32p1` = 308 C files / ~4.0 MB. The newest (`bv_r32p1`) is the unified
  Midgard+Bifrost kbase core — same architecture family the S9+ (9810) tree
  uses (`b_r16p0..bv_r38p1` there).
- *(fact)* `b_r10p0/platform/exynos/` (29 files) is Samsung's own GPU platform
  glue for this SoC: `gpu_dvfs_*`, `gpu_control`, `gpu_balance`,
  `gpu_integration_callbacks` — i.e. the clock/power/VM bring-up sequence for
  the 9820 GPU is documented in source, which is exactly what a DXGK port needs
  beyond the kbase core.
- *(fact)* `limadriver/mali-kernel` (user link) = Mali **r3p2** (2010-2013,
  Utgard/Mali400-era GP/PP driver, GPL2, Arm's first public kernel drop —
  `.version` = `r3p2-01rel3`). Two generations older than the S9+'s Midgard,
  three older than our Bifrost: **no architectural transfer value**; it is a
  historical archive collected by the Lima community.
- *(inference)* What that archive + libmali-rockchip together confirm: Arm's
  consistent split — **kernel side published (GPL2 kbase since the r3p2 era),
  userspace GL/Vulkan always closed binary**. The most plausible reading of the
  S9+ "WDDM, code not released": kernel half = a kbase→WDDM port (could be
  released, GPL), userspace half = a port of Arm's proprietary Midgard DDK
  binary (cannot be released) — which is exactly why it stays closed.
- *(fact)* Userspace quality matters for the ladder: S9+ Midgard's open driver
  is **Lima** (GL 2.1-era, no Vulkan); our G76 Bifrost's open driver is
  **panfrost** (GLES 3.1 / Vulkan 1.3). If anyone ever does the open rung-2
  path, the 9820 is the BETTER target of the two, not the worse.

### D5b(d) — panfrost-on-Windows sizing (2026-10-10)

- *(fact)* Mesa builds on Windows with MSVC for **AARCH64** explicitly
  (docs.mesa3d.org/install.html: "Visual Studio 2022 ... (ARM, AARCH64,
  ARM64EC, X86, X64)").
- *(fact)* Every Mesa hardware driver's kernel interface is a **DRM winsys**
  (gitlab tree `src/gallium/winsys/`: panfrost, lima, freedreno, ... all
  fd/libdrm-based). There is no Windows winsys for panfrost in tree today.
- *(correction to my earlier ladder)* Driver pairing is strict:
  **panfrost userspace <-> panfrost DRM kernel driver** (mainline,
  `drivers/gpu/drm/panfrost/`), while **kbase <-> the closed Arm DDK
  userspace**. "Port kbase + run panfrost" is NOT a thing — I had conflated
  them. The coherent open rung-2 is: port the **panfrost DRM kernel driver**
  to DXGK *and* port panfrost's winsys from libdrm/fd to a Windows device
  handle.
- *(recollection, needs verification)* The mechanism "Mesa userspace on
  Windows over a vendor WDDM kernel driver" existed for Adreno (turnip/freedreno
  on WoA via D3DKMT against Qualcomm's WDDM); today's Mesa main shows only
  `tu_drm.c` in `src/freedreno/vulkan/`, so the Windows backend may have been
  dropped or lives in forks — flagged, not relied on.
- *(verdict, inference)* Rung 2 = two coordinated ports (kernel: panfrost DRM
  -> DXGK, incl. replacing GEM/scheduler with DXGK allocation + present
  machinery; userspace: panfrost winsys -> Win32). Multi-person-month. The
  kernel-side material for the ALTERNATIVE closed pairing (kbase r10p0..r32p1
  in our kernel) is useless without the closed DDK userspace. So the open
  ladder's real cost center is the DXGK port, whichever kernel driver is
  chosen.
- *(fact, for the record)* DWM acceleration needs D3D9Ex/D3D11 via the WDDM
  DDI — Mesa gives GL/Vulkan, so even rung-2 needs a D3D story for the
  "GPU-accelerated DWM" screenshot specifically (DXVK-style translation is
  not how the Windows DDI works; the ICD itself must speak D3D). This raises
  the bar on what the S9+ screenshot implies — and makes a display-only WDDM
  + WARP reading (which we ALREADY get from in-box BasicDisplay) the
  conservative interpretation.

### D5b(b) — G76 firmware blob: status (2026-10-10)

- *(fact)* Mechanical tree scan of the LineageOS 9820 kernel: **no firmware
  `.bin` under `drivers/gpu/arm/b_r10p0/`** (the `csf/…firmware…` files belong
  to `bv_r32p1` and are Valhall CSF code, not our G76's JM-era PPU firmware).
- *(inference, high confidence)* Bifrost r10p0 kbase loads PPU/shader firmware
  via `request_firmware` from the Android vendor partition — the blobs live in
  `/vendor/firmware/` (Samsung naming is `mali_*` / `mali_ppu_firmware_*`).
- *(pending, one cheap read — do NOT run while the boot stream owns the phone)*
  `adb shell ls /vendor/firmware | grep -i mali` closes this; the files can
  then be pulled read-only for a future port. Recorded as the exact next step
  rather than guessed at.

## 2026-10-11 — external report reopens the verdict
User relays: ntdev (original exynos-woa creator) posted that he has **GPU-accelerated DWM** (desktop
transparency) on Exynos, not yet released. Unverified by us (no link, no code), but it falsifies "permanently
NotImplemented" as a feasibility claim: the absence of a shipped Mali WDDM only means one has to be written or
ported. Reopened as a research task after M16 USB; first step = find his post/details.
