# GPU on Windows: prior art (S9+ WDDM report, Mali WDDM anywhere, minimal accelerated path)

Web research only (no device, no builds). Started 2026-10-12. Every claim carries a URL + access
date (all accesses 2026-10-12 unless stated). *(inference)* marks reasoning, not sources.

Known before this note (do not repeat): `notes/d5-gpu-display.md` (Update Catalog has no Mali
WDDM; BasicDisplay+BasicRender/WARP; S9 WDDM report addendum; ntdev relay 2026-10-11),
`notes/gpu-panfrost.md`, `notes/gpu-panfrost-repos.md` (Mesa main has no d3d10umd frontend; WGL
only; no non-Linux panfrost port).

## Progress log

- 2026-10-12 — file created; read the three existing notes + M16 G1-G12 ladder (MILESTONES.md:378-389).

- 2026-10-12 — search "ntdev Windows Galaxy S9 Exynos 9810 GPU": S9+ project = github.com/ntdevlabs/exynos9810-woa
  (README: UFS/HSI2C/touch, no GPU/WDDM mention). Search also surfaced **gahingwoo/wddm-mali-g52** (Mali-G52
  WDDM on RK3576, panfrost + Mesa D3D10 frontend) — the single most relevant prior art; reading it next.

- 2026-10-12 — wddm-mali-g52 inspected via GitHub API + raw README/docs/PLAN.md/docs/M5-PLAN.md (F2 detail).
  Repo created 2026-10-01, last push 2026-10-03; 19 Mesa patches; drivers/{m1probe,malikm,malidod,maliwddm}.

- 2026-10-12 — verified Mesa d3d10umd removal + viogpu3d MR via GitLab API; read virtio-win PR 943 comments
  and UTM's Triton blog post (F3, F4).

- 2026-10-12 — X is not fetchable directly (402/451); **api.fxtwitter.com works** (status + `/2/profile/NTDEV_/statuses`
  first page only; cursor pagination 404s). Pulled NTDEV's posts 2026-09-30..10-11 and viewed the 3 GPU images (F1b).

- 2026-10-12 — transparency-under-WARP: only Learn acrylic doc ('low-end hardware'); no BasicRender statement (F5); F1b inference corrected.

- 2026-10-12 — GitHub repo search + RK3588/Arm searches (F6). GitHub unauthenticated search rate-limited after ~8 queries.

- 2026-10-12 — Learn: DWM default-adapter rule, WDDM 1.2 DDI table, FL table (F7).

- 2026-10-12 — d3d10umd READMEs + Triton source tree inspected in osy/virtio-win-mesa@neptune (F8).

- 2026-10-12 — gahingwoo M5.2-PLAN + step-c2 result read (WDDM 1.3 aperture-segment design; FL10_0 hardware device on 22621).

- 2026-10-12 — Reddit/XDA search for S9+ GPU discussion: nothing indexed. Wrote M16 section + Sources.

## Findings

### F1. The S9+ project and its author
- *(fact)* https://github.com/ntdevlabs/exynos9810-woa (accessed 2026-10-12): "Windows 11 on the Samsung
  Galaxy S9+ (SM-G965F, Exynos 9810)", org `ntdevlabs`; README lists UFS, HSI2C, touch drivers, UEFI,
  installer; licences BSD-2-Clause-Patent / GPL-2.0 (touch) / MS-PL (HSI2C); **no GPU/WDDM/display driver
  mentioned**. Forks exist (JoshAtticus/exynos9810-woa says "S9 and S9+", Ben-bit-code208/exynos9810-woa).
- *(fact)* NTDEV is the same handle known for tiny11 (guru3d article "Modder NTDev ... Tiny11",
  https://www.guru3d.com/story/modder-ntdev-presents-windows-11-running-on-graphics-card-vram-with-tiny11-project/,
  accessed 2026-10-12). *(inference)* ntdevlabs = NTDEV's org; matches the relay in d5-gpu-display.md:429.

### F1b. NTDEV's own GPU posts (the screenshot the user saw) — timeline from api.fxtwitter.com
Source for all rows: https://api.fxtwitter.com/2/profile/NTDEV_/statuses and
https://api.fxtwitter.com/NTDEV_/status/<id> (accessed 2026-10-12); canonical URLs https://x.com/NTDEV_/status/<id>.
Author: NTDEV (@NTDEV_, x id 3527880615, "Trying stuff so you don't have to.", linktr.ee/ntdev).

| date (UTC) | id | text (verbatim) |
|---|---|---|
| 2026-08-09 | 2086482825088270662 | "I can declare: WINDOWS 10 ARM64 HAS SUCCESSFULLY BOOTED ON A SAMSUNG GALAXY S9+!" |
| 2026-09-30 | 2105413206965629030 | "IT'S OUT! The Galaxy S9+ Windows installer is now public, free and open source..." |
| 2026-09-30 | 2105413219569615290 | "...if you want to help with the next part (the GPU and USB): patreon/ko-fi/paypal" |
| 2026-10-01 | 2105611856589299880 | "Another major achievement: USB now works!" |
| 2026-10-01 18:24 | **2105725603458773371** | "**Le (GPU) triangle**" (photo) |
| 2026-10-04 | 2106534996865335617 | "All 8 cores (including the proprietary Mongoose cores) work!" |
| 2026-10-05 19:16 | **2107188152498610225** | "**First GPU-accelerated DWM pixels on the S9+**" (photo) |
| 2026-10-08 | 2108194728957411837 | "The S9 now has Wi-Fi!" |
| 2026-10-09 00:57 | **2108361056884044221** | "**Well, from few lines to an almost complete desktop! We have transparencyyyy✨**" (quotes the DWM-pixels post; photo) |
| 2026-10-11 | 2109227842055684385 | "Looks like Windows 11 was made to run on an S10!" (quoting a post: "Desktop on a Galaxy S10e (exynos 9820). @NTDEV_ you made this happen.") |

What the three photos show (viewed 2026-10-12; media https://pbs.twimg.com/media/HTkKRjVWIAAhBTC.jpg,
HT48dDdWAAAZJqc.jpg, HUJnNMwXYAAbK9K.jpg):
- triangle: portrait phone, full-screen light-blue clear + a vertical yellow bar, an RGB-interpolated triangle
  in the middle — the classic D3D11 "hello triangle" vertex-colour shader. *(inference)* a hardware render
  test app presenting straight to the panel, not a DWM-composited window.
- DWM pixels: landscape, black screen, only two white L-shaped corner strokes. *(inference)* first frames
  DWM composited through the new driver (partial chrome), i.e. DWM was using the GPU adapter by 10-05.
- transparency: full Windows 11 desktop, **"Windows 11 IoT Enterprise, Build 22621.ni_release.220506-1250,
  Test Mode"** watermark, dated 10/8/2026 5:54 PM; taskbar is a dark translucent band with the wallpaper
  bloom visible through it.
  *(correction, see F5)* No source found that Windows disables transparency under Basic Display/BasicRender;
  so the photo **alone** does not prove hardware acceleration. The evidence for real GPU work is NTDEV's own
  wording ("GPU-accelerated DWM pixels", "(GPU) triangle") and the "Rendering issue" reply below.
- *(fact, https://api.fxtwitter.com/2/conversation/2108361056884044221, accessed 2026-10-12)* Reply by @NTDEV_
  2026-10-09 07:17 to "How the hell you removed start menu": "**Rendering issue**". *(inference)* the missing
  Start/taskbar-icons in the transparency photo are GPU-driver rendering bugs — consistent with a young
  hardware UMD, not with WARP (which renders the shell correctly). No reply discloses the stack (Mesa/panfrost
  or otherwise); 2105725603458773371 and 2107188152498610225 replies contain no technical info.
- *(fact)* No code: ntdevlabs/exynos9810-woa README (F1) has no GPU component; NTDEV's posts give no repo.
- *(inference, medium confidence)* Method: the S9+ GPU is Mali-G72 (Bifrost v6, panfrost-supported). The dates
  (triangle 10-01, DWM 10-05) run in lock-step with gahingwoo/wddm-mali-g52 (M4 triangle 10-02, hardware UMD
  10-03), and that repo is public GPL with the full recipe; NTDEV's own README credits the
  "Linux kernel and postmarketOS Exynos 9810 communities". The most likely stack is the same
  **Mesa 26.2 d3d10umd + panfrost + custom WDDM KMD** design. Not confirmed — ask NTDEV.

### F2. Mali WDDM prior art: gahingwoo/wddm-mali-g52 (RK3576, Mali-G52 Bifrost JM)
- *(fact)* https://github.com/gahingwoo/wddm-mali-g52 (accessed 2026-10-12): "WDDM driver for the Arm
  Mali-G52 in the Rockchip RK3576 (work in progress)", GPL-2.0, ~153 commits, last activity ~2026-10-03.
  Claims: KMD (own job submission), WDDM 2.0 display-only driver (M5.1 done), UMD = **Mesa panfrost gallium
  + Bifrost compiler under Mesa's D3D10 frontend** with a D3D11 compatibility layer; M1 Mali executes jobs
  under Windows, M2 Mesa 26.2 builds for Windows ARM64, M3 panfrost clears RT, M4 first D3D11 triangle on
  Mali (byte-identical to softpipe), M5.2 partial (jobs via WDDM; D3D11 triangle via HW UMD); **GPU desktop
  composition (DWM) not yet achieved**. Target platform woa-rk3576 (ArmSoM CM5-IO).
- *(fact, api.github.com/repos/gahingwoo/wddm-mali-g52, accessed 2026-10-12)* created 2026-10-01T08:43Z,
  pushed 2026-10-03T10:29Z, 1 star. Tree: `drivers/{common,m1probe,malikm,malidod,maliwddm}`,
  `mesa-patches/0001..0019`, `win32-compat/` (incl. `malikm_drm.c` 43 KB = libdrm/panfrost-ioctl shim,
  `xf86drm.h`, `pthread.h`), `tools/{etw,m1,windows}`, `tests/`, `ci/`.
- *(fact, raw README.md)* "No Windows driver for any Mali GPU exists as far as we can find"; woa-rk3576
  draws the desktop "through WARP into the framebuffer the firmware leaves behind". M4 image:
  docs/results/m4/first-triangle-on-mali-windows.png.
- *(fact, docs/PLAN.md, https://github.com/gahingwoo/wddm-mali-g52/blob/main/docs/PLAN.md)* Architecture:
  "D3D11 runtime, feature level 10_0 / UMD Mesa d3d10umd (DXBC SM4 -> TGSI) / Mesa Panfrost (TGSI -> NIR ->
  Bifrost compiler) / pan_kmod backend new: WDDM instead of Linux DRM / KMD full WDDM driver". M0 finding 1:
  "**Mesa removed d3d10umd on 2026-09-30** (commit `1f9af627b1`, MR 44717) ... Mesa 26.2.0 is the last
  release that has it." Finding 3: `pan_kmod_ops` ~20 ops, Linux panfrost backend ~1000 lines.
  Prior-art section cites **Triton** (UTM, 2026, D3D11 WDDM driver for QEMU guests, forwards D3D to host),
  VirtualBox WDDM, Venus Windows driver.
- *(fact, PLAN.md "Why the driver has to drive the display as well")* "A render-only Mali next to
  BasicDisplay would not speed up the desktop. DWM composites on the display adapter, so it would keep using
  WARP." Hybrid mode needs two full WDDM drivers with cross-adapter support; BasicDisplay is neither.
  *(their reasoning; not independently verified here — see F5.)*
- *(fact, PLAN.md "M1 status")* M1 recipe on RK3576: firmware powers PD_GPU + clock and publishes
  `ACPI\RKCP7402`; KMDF `m1probe` soft-resets, powers L2/tiler/shaders, AS0 `TRANSTAB = L0 | 0x7`,
  `MEMATTR = 0x888d88`, `TRANSCFG = 0`, leaf PTE `pa | 0x2C5`, table `pa | 3`, JS0 WRITE_VALUE
  0xC0FFEE42 — worked on Windows 11 23H2 with tables above 4 GB. Also tried first from Linux userspace via
  /dev/mem with panfrost unbound.
- *(fact, docs/M5-PLAN.md)* GpuMmu model maps onto Mali LPAE (CPU-updated page tables,
  `DXGK_PAGETABLEUPDATE_CPU_VIRTUAL`); G52 MMU_FEATURES 0x2823 = 35 VA bits, plan = 3 levels to VidMm +
  private L0; software paging node (Mali has no DMA engine); open question 1: "Does DWM accept a UMD with
  only the D3D10/10.1 DDI?" — CI showed the D3D11 runtime creating an FL10_0 device on d3d10umd.
  M5.1 display-only `malidod` (copies dirty rects like BasicDisplay) done 2026-10-02.
- *(fact, commit log 2026-10-03)* "UMD: Panfrost on the runtime device's kernel callbacks ... after Mesa MR
  24223 (viogpu3d's d3d10umd)": d3d10umd binds to runtime callbacks pfnAllocateCb/pfnLockCb/pfnRenderCb/
  pfnEscapeCb, waits via pfnSignalSynchronizationObject2Cb; present via DXGI pfnPresentCb + KMD CPU blt.
  Last commit: GPU read translation faults (0x4002c3) from GEM_CLOSE freeing WDDM allocations before jobs
  finished — fixed by deferring frees ("Not yet verified on hardware").
- *(fact, docs/M5.2-PLAN.md, https://github.com/gahingwoo/wddm-mali-g52/blob/main/docs/M5.2-PLAN.md, accessed
  2026-10-12)* They **dropped WDDM 2.0/GpuMmu for WDDM 1.3, copying viogpu3d's shape**: caps `DXGKDDI_WDDMv1_3`,
  one node (job manager), one **aperture segment**; `DxgkDdiCreateAllocation` reserves a fixed Mali GPU VA
  (returned in allocation private data) and `BuildPagingBuffer(MAP/UNMAP_APERTURE_SEGMENT)` maps the MDL pages
  into the Mali MMU, so addresses never move and `Patch` is a no-op; `Render` copies commands into the DMA
  buffer, `SubmitCommand` starts job chains, job IRQ -> `DXGK_INTERRUPT_DMA_COMPLETED`; no preemption.
  Reasoning recorded there: WinUI3 glitches in viogpu3d's known issues imply DWM composites on a d3d10umd UMD.
- *(fact, docs/results/m5.2-step-c2/README.md)* 2026-10-03, WinPE 22621: INF names `maliumd.dll`
  (= Mesa `libgallium_d3d10.dll`, d3d10umd + panfrost); `D3D11CreateDevice` on the adapter → "device created
  (hardware), feature level 0xa000" (= FL 10_0); Mali is DXGI adapter 0 ahead of Basic Render; 17 jobs,
  0 faults. Open: whether Win11 loads a WDDM 1.3 full driver outside WinPE (their Q2) — WinPE 22621 did.

### F3. Mesa d3d10umd: real, a WDDM D3D10 UMD on gallium — removed from Mesa main 2026-09-30
- *(fact)* Mesa MR 44717 "remove d3d10umd frontend", **merged 2026-09-30T19:15Z**
  (https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44717, GitLab API, accessed 2026-10-12).
  Rationale quoted: "one of the last holdouts that generate TGSI shaders ... added in 2021 ... implementing an
  API that's only available on a proprietary platform". Alternative considered: !44715 "remove TGSI from
  Gallium interface" (draft). This explains why our 2026-10-12 shallow clone of mesa main (notes/
  gpu-panfrost-repos.md:166) has no `frontends/d3d10umd` — it was removed 12 days earlier, it did exist.
- *(fact, gahingwoo PLAN.md M0 finding 1)* Mesa **26.2.0 is the last release with d3d10umd**; pin it or vendor
  the frontend. *(inference)* For us: pin Mesa 26.2.x tags, not main.
- *(fact)* Mesa MR 24223 "Draft: virgl/win: Virtio GPU 3D acceleration for windows" (opened 2023-07-18, never
  merged; https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/24223): d3d10umd + WGL on virgl inside a
  WDDM (viogpu3d) driver; known issues include WinUI3 glitches and "No preemption ... VioGpu3D disables
  preemption systemwide". Sibling KMD PR https://github.com/virtio-win/kvm-guest-drivers-windows/pull/943
  (WebFetch summary, accessed 2026-10-12: WDDM 1.3, render+display, d3d10umd built with
  `-Dgallium-d3d10umd=true`).
- *(fact, PR 943 comment by anonymix007, 2026-03-22, via WebFetch summary)* "**DWM seems mostly fine with just
  D3D10**" (context: whether a D3D12 UMD is needed). Second-hand but from someone who ran d3d10umd as the
  display adapter's UMD. *(not verified by us; summarizer quote)*

### F4. Triton (UTM, 2026-07-24): D3D11 DDI UMD for Windows 11 ARM64 guests
- *(fact)* https://blog.getutm.app/2026/introducing-triton-directx-11-driver-for-qemu/ (accessed 2026-10-12):
  UMD implements the D3D11 DDI and translates DDI calls **back to D3D11 API calls** serialized to the host
  (Neptune/VirtIO); KMD based on anonymix007's Venus Windows driver; DWM works on "shared textures ... that
  backbuffer is shared with the DWM process"; "DXGI is a system component which talks to the UMD";
  VirtualBox's DDI prototype list and DXBC signature algorithm filled gaps "not readily available from MSDN".
  Repos: UMD https://github.com/osy/virtio-win-mesa/tree/neptune, KMD
  https://github.com/osy/kvm-guest-drivers-windows/tree/neptune. *(inference)* It is the reference for "a
  D3D11 DDI UMD whose backend is another API" — Triton's backend is host D3D11, not Vulkan, but the
  DDI->API layer is reusable in shape: a DDI->Vulkan backend over PanVK is the same pattern.

### F5. Transparency / acrylic under Basic Display + WARP — what is and is not documented
- *(fact)* Microsoft Learn, Acrylic material (https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic,
  page dated 2026-10-02, accessed 2026-10-12): acrylic appears as a solid colour "When the user turns off
  Transparency effects", "When Battery Saver mode is activated", "**When the app runs on low-end hardware**";
  "Rendering acrylic surfaces is GPU-intensive". "Low-end hardware" is not defined there.
- *(dead end)* Searches for an official statement that DWM turns transparency off on BasicDisplay/BasicRender
  (WARP) found none (queries 2026-10-12: "Basic Display Adapter transparency effects", "virtual machine no 3D
  acceleration transparency mica"); results were only VirtualBox 3D-acceleration glitch threads
  (https://forums.virtualbox.org/viewtopic.php?f=6&t=108585). *(inference)* DWM can run its full effect
  pipeline on WARP (WARP is a complete D3D10.1/11 device), so translucency in a photo does not by itself
  distinguish WARP from a hardware GPU. **Cheap empirical closer for us:** on our S10e Windows (BasicDisplay +
  WARP today), check Settings > Personalization > Colors > Transparency effects and whether the taskbar is
  translucent; that answers it for our exact build without any source.

### F6. Other Mali-on-Windows efforts (searched 2026-10-12)
- *(fact)* GitHub repository search (api.github.com/search/repositories, accessed 2026-10-12): `mali wddm` /
  `wddm mali` → **only gahingwoo/wddm-mali-g52**; `panfrost windows`, `panfrost wddm`, `mali dxgkrnl`,
  `mali windows driver kmdf` → 0 results; `d3d10umd` → only YWFhYmJi/d3d10umd-winddk-headers ("Headers for
  gallium d3d10umd statetracker", pushed 2026-09-05).
- *(fact)* `exynos windows` → ntdevlabs/exynos9810-woa (74★), Windows-on-Exynos/exynos_ufs_driver_woa ("Exynos
  UFS driver for Windows 10/11 (ARM64)", pushed 2026-08-26), AistopGit/Exynos7870-Windows-Drivers and
  AistopGit/dwcmshc (Exynos 7870). None mention a GPU.
- *(fact)* RK3588/Mali-G610: Microsoft Q&A thread reports the G610 does not appear in Device Manager under
  Windows 11 on RK3588 (https://learn.microsoft.com/en-us/answers/questions/2356332/i-want-to-know-something-about-the-rk3588-soc,
  via search summary, accessed 2026-10-12). Radxa forum, mIIsko 2022-05-04: "No windows drivers for ARM MALI
  GPU" (https://forum.radxa.com/t/windows-10-arm-should-happen/9825, accessed 2026-10-12).
- *(fact)* gahingwoo README (F2): woa-rk3576 "draws its desktop on the CPU today ... DWM renders through WARP".
- *(not found)* No evidence Arm ever shipped a Windows Mali driver (IoT or otherwise): Arm's DDK page
  (https://developer.arm.com/ip-products/graphics-and-multimedia/mali-gpus/mali-driver-development-kit) only
  redirects; the user-space binary driver page lists Linux/Android builds
  (https://developer.arm.com/downloads/-/Mali%20GPUs%20User-Space%20Binary%20Drivers, search snippet). Consistent with
  d5-gpu-display.md (Update Catalog: zero Mali WDDM hits).

### F7. What Windows needs for an accelerated desktop (documented facts)
- *(fact)* "**DWM always runs on the default adapter**, it will choose Microsoft Basic Display Adapter to compose
  the desktop when a WDDM-compliant graphics driver is not available" and "Microsoft Basic Display Adapter is a
  software rasterizer that uses the CPU" — Compatibility Cookbook, Desktop Window Manager is always on
  (https://learn.microsoft.com/en-us/windows/compatibility/desktop-window-manager-is-always-on, accessed 2026-10-12).
  *(inference)* Confirms gahingwoo's PLAN.md: a **render-only** Mali KMD next to BasicDisplay leaves DWM on
  WARP; the GPU KMD must own the display (be the default/POST adapter) — i.e. a full (display + render) WDDM
  miniport, not render-only. Our G10 row says "render-only, no display" → must change for the DWM goal.
- *(fact)* Direct3D software requirements (WDDM 1.2 certification table,
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/software-requirements, updated 2025-07-18,
  accessed 2026-10-12): D3D10-class hardware "Required: D3D9 - UMD DDI / D3D10 - UMD DDI / D3D11.1 - UMD DDI".
  This is a **certification** rule (HLK), not a loader rule; test-signed drivers are not bound by it.
- *(fact)* Feature-level table (https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro,
  accessed 2026-10-12): FL 10_0 = SM 4.0, max texture 8192, MRT 8; "Extended Formats (BGRA, and so on)" is
  **Optional at 10_0/10_1** and "Yes" at 9_x and 11_0+. *(inference)* DWM/Direct2D create devices with BGRA
  support, so a d3d10umd-based UMD must advertise B8G8R8A8 render-target/shader-resource support (gahingwoo
  already swizzles to B,G,R,X for present, F2 commit log).
- *(fact, second-hand)* "DWM seems mostly fine with just D3D10" (anonymix007, virtio-win PR 943, F3) and
  gahingwoo M5-PLAN open question 1 "Does DWM accept a UMD with only the D3D10/10.1 DDI?" (unanswered as of
  2026-10-03). NTDEV's DWM-on-GPU result (F1b) would answer it if his stack is d3d10umd (unconfirmed).
- *(fact, Wikipedia via search summary, https://en.wikipedia.org/wiki/Desktop_Window_Manager, accessed 2026-10-12)*
  Windows 7 DWM moved to Direct3D 10.1 with 10level9 for D3D9 hardware. *(inference)* Since Win8, DWM is a
  D3D11 client that accepts down-level feature levels; a FL10_0 device suffices in practice (supported by the
  two second-hand data points above, not by a Microsoft statement).

### F8. D3D UMD on top of something else — what exists
- *(fact)* Mesa d3d10umd READMEs (read from https://github.com/osy/virtio-win-mesa/tree/neptune/src/gallium/frontends/d3d10umd
  and .../targets/d3d10umd/README.md, raw, accessed 2026-10-12; identical files existed in Mesa until MR 44717):
  "a Gallium state tracker for D3D10 UMD DDI. It still uses TGSI, not NIR. Currently it only supports SW
  rasterizers"; the DLL "implements D3D10's software rendering interface, like WARP"; can be renamed into a
  system D3D10 UMD (e.g. `vm3dum_10.dll`, `nvwgf2um.dll`); builds with Windows SDK/DDK 10.0.26100.0 (MSVC or
  mingw). *(fact)* gahingwoo (F2) and viogpu3d (F3) are the two projects that turned it into a **hardware**
  UMD bound to a WDDM KMD via runtime callbacks (gahingwoo mesa-patches 0012/0014/0018/0019).
- *(fact)* Triton UMD source = `src/virtio/neptune/triton/` in https://github.com/osy/virtio-win-mesa (branch
  `neptune`, git tree API accessed 2026-10-12): `tritonDDI.c` (D3D10/11 DDI), `tritonDDI12.c` (**D3D12 DDI**),
  `tritonDxbc.c`, `tritonPresent*.c`, `tools/make-arm64x.bat` (ARM64X build). `triton.h`: "Copyright 2026 Turing
  Software LLC, SPDX MIT"; each DDI handle's state wraps an **inner `ID3D11Device`/`ID3D11DeviceContext`** (the
  Neptune host-forwarding device). *(inference)* Triton is a generic **D3D11-DDI -> D3D11-API** adapter: with
  a Windows build of DXVK (D3D11 API -> Vulkan) as the inner device and PanVK as the Vulkan driver, it would be
  a "D3D11 UMD on Vulkan" — nobody has published that combination (no search hit), and it inherits DXVK's
  Vulkan feature requirements (unchecked for PanVK on Bifrost v7).
- *(fact)* DXVK is an API-level DLL replacement (d3d9/10/11.dll on Vulkan), "Windows is not officially
  supported, though generally works" (https://github.com/doitsujin/dxvk/wiki/Windows, via search summary,
  accessed 2026-10-12). *(inference)* As an app-local DLL it can never accelerate **DWM** (DWM loads the system
  D3D11 runtime, which loads the adapter's UMD named in the INF); it only helps individual apps.
- *(not found)* No published WDDM UMD that translates D3D DDI directly to Vulkan (searches 2026-10-12:
  "WDDM user mode driver D3D11 DDI ... Vulkan", GitHub `d3d11 ddi vulkan` was rate-limited — re-run with auth).

## What this means for our GPU milestone M16

All of this section is *(inference)* built on the facts above.

0. *(fact, local Mesa clone `C:\Users\ilya\code\mali\mesa-upstream` @1b75efe, `docs/drivers/panfrost.rst:40-50`)*
   "G71, G72 — Bifrost (v6)"; "**G31, G51, G52, G76 — Bifrost (v7)**", GLES 3.1 / Vulkan 1.3 columns. So
   gahingwoo's G52 is the **same architecture revision as our G76** (v7) — closer than the S9's G72 (v6).
   *(correction flag for another note, not edited here)* `notes/d5-gpu-display.md:375-376` calls the S9+ GPU
   "Midgard" with Lima as its open driver; the G72 is Bifrost v6 and panfrost-supported (Lima is Utgard).

1. **The S9+ result is real GPU work, method undisclosed.** NTDEV posted a GPU triangle (10-01), "first
   GPU-accelerated DWM pixels" (10-05) and a translucent desktop with "Rendering issue"s (10-09) on the
   Mali-G72; no code. The public, near-identical recipe is gahingwoo/wddm-mali-g52 (G52 = Bifrost JM like our
   G76 and the S9's G72). Treat **wddm-mali-g52 as our reference implementation**, not a blank page.
2. **The UMD question in G12 is answered:** Mesa **26.2.x d3d10umd + panfrost gallium** is a working hardware
   D3D10/11 (FL10_0) UMD on Bifrost under a custom WDDM KMD (gahingwoo step c2), and viogpu3d + the anonymix007
   quote + NTDEV's photos all point to DWM accepting a D3D10-DDI-only UMD (still unmeasured by anyone in
   public). d3d10umd left Mesa main on 2026-09-30, so pin the 26.2 branch (or vendor the frontend). The
   D3DKMT pan_kmod/libdrm shim is `win32-compat/malikm_drm.c` (43 KB) there.
3. **G10 "render-only" is the wrong target for DWM.** DWM runs on the default adapter (Learn compat cookbook);
   a render-only Mali beside BasicDisplay leaves DWM on WARP. The KMD must be a **full display+render WDDM
   driver** that inherits the GOP framebuffer (display half = what gahingwoo `malidod` does: one mode, copy
   dirty rects, no DECON programming at first).
4. **WDDM 1.3 + one aperture segment** (viogpu3d shape) beats WDDM 2.0/GpuMmu for a first driver: fixed Mali
   VA per allocation, map MDL pages into the Mali MMU on MAP_APERTURE_SEGMENT, no preemption. Proven to load on
   22621 WinPE.
5. **A KMDF-first rung is cheaper than DXGK-first.** gahingwoo M1 used a plain KMDF driver (`m1probe`, then
   `malikm` with private IOCTLs) on an ACPI device. For us: UEFI G3-G8, then KMDF, then DXGK.

Concrete suggested changes to the G1-G12 ladder (MILESTONES.md:378-389) — suggestions only, not applied:
- **G0 (new, research):** clone gahingwoo/wddm-mali-g52 (+ osy/virtio-win-mesa@neptune for d3d10umd/Triton
  reference) next to the other clones in `C:\Users\ilya\code\mali\`; inventory its 19 mesa-patches and
  `malikm_drm.c` for G52/RK3576-specific vs Bifrost-generic parts; record licences (repo GPL-2.0, Mesa MIT).
- **G9b (new):** KMDF `m1probe`-style driver on the G9 ACPI device: power-up, AS0, WRITE_VALUE job under
  Windows via private IOCTL (gahingwoo M1). Separates GPU bring-up risk from the DXGK learning curve.
- **G10 (change):** from "render-only skeleton" to **full WDDM 1.3 KMD whose display half inherits the GOP
  framebuffer** (malidod-style); first step = adapter starts with a softpipe d3d10umd UMD and the desktop still
  works.
- **G11 (change):** aperture-segment memory model + fixed per-allocation Mali VA (their M5.2-PLAN),
  DMA_COMPLETED on the job IRQ, no preemption; validate with a D3DKMT WRITE_VALUE test (their step b).
- **G12 (change, decided):** Mesa **26.2** d3d10umd + panfrost (port gahingwoo patches 0001-0019, retarget
  G52 -> G76): software-driver DLL over a private KMD path first (their M3 clear, M4 triangle), then the adapter's
  INF UMD (their c2), then runtime callbacks + DXGI present (their c/d).
- **G13 (new):** DWM on the Mali — Transparency effects on, GPU job counters vs WARP CPU time (their M5.5).
- **G14 (optional):** Triton-style D3D11 DDI over DXVK + PanVK, only if d3d10umd/TGSI turns out to be a dead end.
- **Cheap check now (no GPU work):** on our S10e Windows (BasicDisplay + WARP) note whether "Transparency
  effects" is on and the taskbar is translucent (F5) — calibrates how much the S9+ photo proves.
- **Ask NTDEV** (X @NTDEV_) whether his stack is d3d10umd + panfrost and whether he will publish; his
  2026-10-11 quote-post of the S10e desktop (id 2109227842055684385) shows he follows this port.

## Sources (all accessed 2026-10-12)
- https://github.com/ntdevlabs/exynos9810-woa
- https://api.fxtwitter.com/2/profile/NTDEV_/statuses ; https://api.fxtwitter.com/NTDEV_/status/2105725603458773371 ;
  .../2107188152498610225 ; .../2108361056884044221 ; https://api.fxtwitter.com/2/conversation/2108361056884044221 ;
  https://cdn.syndication.twimg.com/tweet-result?id=2086482825088270662 ; media pbs.twimg.com/media/HTkKRjVWIAAhBTC.jpg,
  HT48dDdWAAAZJqc.jpg, HUJnNMwXYAAbK9K.jpg
- https://www.guru3d.com/story/modder-ntdev-presents-windows-11-running-on-graphics-card-vram-with-tiny11-project/
- https://github.com/gahingwoo/wddm-mali-g52 (README, docs/PLAN.md, docs/M5-PLAN.md, docs/M5.2-PLAN.md,
  docs/results/m5.2-step-c2/README.md, commits via api.github.com)
- https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44717 ; .../24223 ; .../44715
- https://github.com/virtio-win/kvm-guest-drivers-windows/pull/943
- https://blog.getutm.app/2026/introducing-triton-directx-11-driver-for-qemu/
- https://github.com/osy/virtio-win-mesa/tree/neptune (src/virtio/neptune/triton, src/gallium/frontends/d3d10umd)
- https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic
- https://learn.microsoft.com/en-us/windows/compatibility/desktop-window-manager-is-always-on
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/software-requirements
- https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro
- https://forum.radxa.com/t/windows-10-arm-should-happen/9825
- https://learn.microsoft.com/en-us/answers/questions/2356332/i-want-to-know-something-about-the-rk3588-soc
- https://forums.virtualbox.org/viewtopic.php?f=6&t=108585
- https://github.com/doitsujin/dxvk/wiki/Windows
- https://en.wikipedia.org/wiki/Desktop_Window_Manager
