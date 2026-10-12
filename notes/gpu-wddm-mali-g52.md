# wddm-mali-g52 inventory + RK3576/G52 -> Exynos9820/G76 retarget list

Task (2026-10-12): full inventory of gahingwoo/wddm-mali-g52 (local clone
`C:\Users\ilya\code\mali\wddm-mali-g52`, HEAD `16f7e7c673618363297096fa5072068185f08e47`,
2026-10-03 23:29 +1300, 153 commits) and a precise list of what changes for our S10e (Mali-G76 MP12,
Exynos 9820). Read-only: no third-party script or binary was run. Citations `path:line` are relative to
the clone root unless prefixed `mesa:` (= `C:\Users\ilya\code\mali\mesa-26.2`) or `ours:` (this repo).

## Progress log
- 2026-10-12 — clone listed: 163 files; drivers/{common,m1probe,malikm,malidod,maliwddm}, include/,
  mesa-patches/0001-0019, win32-compat/ (malikm_drm.c 1429 lines), tests/, tools/, docs/, .github/workflows/{ci,mesa}.yml.
- 2026-10-12 — README.md + docs/PLAN.md read (hardware table README.md:39-47; M1 recipe PLAN.md:99-117;
  MMU format "Mali LPAE ... TRANSCFG = 0" PLAN.md:111-113; licence PLAN.md:73-74, README.md:51-54).
- 2026-10-12 — drivers/common/maligpu.{h,c} read (shared hardware layer, 426+197 lines). Facts:
  RK PMU probe `PMU_PA 0x27380000`, ACK0 0x120 / STATUS0 0x230 (maligpu.h:16-18), "on" = STATUS0 bit25==0 &&
  ACK0 bit0==0 (maligpu.c:25-39); GPU_ID hard check `>>16 != 0x7402` (maligpu.c:152); quirks = TILER_CONFIG
  read-back + JM_CONFIG IDVS 0xF<<16 "for a G52: no shader quirks" (maligpu.c:95-98); L2->shader->tiler PWRON
  with *_LO only (maligpu.c:100-107); AS0 in **Mali LPAE (legacy) format**: TRANSTAB=L0|0x7, MEMATTR_LO
  0x00888d88, TRANSCFG=0 (maligpu.c:119-129); PTE leaf 0x2C5, table 0x3, XN bit 54 (maligpu.h:125-127); page
  tables MmNonCached contiguous below 40 bits (maligpu.c:188-203); VA 32 MB..4 GB, exec within 16 MB windows
  (maligpu.h:119-122); lock+FLUSH_PT per map (maligpu.c:273-293); job: AS0, THREAD_PRI 8, START/END
  FLUSH_CLEAN_INVALIDATE, affinity = full SHADER_PRESENT (maligpu.c:358-371); JOB_INT_MASK 0x70007 (slots 0-2,
  maligpu.c:140); one ISR for all three lines (maligpu.c:400-426).
- 2026-10-12 — drivers/malikm (KMDF, M3/M4) read: binds `ACPI\RKCP7402` (device.c:3-6), device
  `\\.\MaliG52` (malikm_ioctl.h:37-39), IOCTL_MALIKM_DRM/MAP/UNMAP/VERSION on FILE_DEVICE 0x8A52
  (malikm_ioctl.h:41-45) carrying the Linux panfrost uAPI 1.1 structs (malikm_ioctl.h:2-21,49-50);
  everything in caller context (device.c:125-201); MMIO = first memory resource, up to 3 interrupts
  (device.c:43-60); worker thread runs one job at a time, IRQ event + 1 ms raw poll, 2 s timeout -> hard stop
  + MkGpuBringup (gpu.c:55-110); slot = FS ? 0 : 1 (gpu.c:60); chain flag alternates by Seq&1 (gpu.c:69);
  BOs = MmAllocatePagesForMdlEx MmWriteCombined "GPU is not coherent" (uapi.c:113-115); HEAP BOs allocated
  whole, rounded to 2 MiB, no grow-on-fault (uapi.c:99-105); GET_PARAM straight from registers, SELECTED_
  COHERENCY = 31 (none) (uapi.c:411-452); one AS for all clients (malikm.h:7-9).
- 2026-10-12 — drivers/maliwddm read (adapter.c 587, memory.c 299, command.c 623, display.c 468, maliwddm.h 209,
  include/maliwddm_abi.h 95). Facts:
  - WDDM **1.3** (`DXGKDDI_INTERFACE_VERSION_WDDM1_3`, adapter.c:523; caps WDDMVersion 1_3 adapter.c:259),
    full adapter (`MW_RENDER_ONLY 0`, maliwddm.h:35-37) because render-only on an ACPI GPU was refused as a POST
    device with STATUS_GRAPHICS_INVALID_DRIVER_MODEL (maliwddm.h:24-34). Shape follows viogpu3d (maliwddm.h:9-10).
  - DDI table: adapter.c:519-586 (full list in the inventory section). No-op/stub: SetPowerState returns success
    without touching hardware (adapter.c:201-209), ControlInterrupt (no vsync, command.c:583-591), GetScanLine
    (command.c:593-598), SetVidPnSourceAddress records nothing - flips not handled (display.c:462-468), Preempt
    (command.c:486-492), CancelCommand, QueryEngineStatus always responsive (command.c:569-581), RenderKm ->
    NOT_SUPPORTED (command.c:604-610), SetPointerShape NOT_SUPPORTED (adapter.c:444-449).
  - Caps: HighestAcceptableAddress 40 bits, MaxAllocationListSlotId 256, MultiEngineAware+PreemptionAware,
    1 node (adapter.c:251-288); INF disables preemption system-wide (`EnablePreemption 0`, maliwddm.inf).
  - Memory: one **aperture segment**, 1 GiB, nominal base MK_VA_START, CacheCoherent FALSE (memory.c:14-40;
    maliwddm.h:20-21); GPU VA reserved at CreateAllocation, fixed for life (memory.c:63-70); pages mapped into the
    Mali MMU on MAP_APERTURE_SEGMENT, unmapped on UNMAP (memory.c:234-280); every other paging op NOT_SUPPORTED;
    UMD learns GPU VA by escape MW_ESCAPE_ALLOC_INFO (memory.c:284-299). Every DMA buffer patches every
    allocation so VidMm makes them resident (command.c:74-87).
  - UMD<->KMD: allocation private data `MW_ALLOCATION_INFO` (maliwddm_abi.h:25-36); command buffer = array of
    `MW_COMMAND{Type=JOB, Slot 0/1, Jc}` max 32 per render (maliwddm_abi.h:40-50); escapes ALLOC_INFO=1,
    GPU_INFO=2 (the 45 DRM_PANFROST_PARAM values), STATS=3 (maliwddm_abi.h:52-95; adapter.c:402-435).
  - Submission: Render copies MW_COMMANDs into DMA-buffer private data (command.c:89-121); SubmitCommand queues
    {fence, commands} in a 16-deep ring at DIRQL via DxgkCbSynchronizeExecution (command.c:365-384,453-484);
    **one job chain on the GPU at a time** (command.c:284-303); DMA_COMPLETED reported when a submission's last
    chain ends (command.c:269-280); a fault drops the rest of the submission and re-runs MkGpuBringup
    (command.c:335-344); 2 s timeout -> hard stop (command.c:321-325).
  - Interrupts **and** a 1 ms polling KTIMER/DPC while work is in flight, because "whether dxgkrnl connects the
    Mali's interrupts on this ACPI device is not yet known" (command.c:9-13,20-21,406-435).
  - Presents: Blt/ColorFill done **on the CPU** at SubmitCommand, 32-bit pixels, into the POST framebuffer for
    PRIMARY allocations (command.c:123-250,462-466); flips not handled (command.c:128).
  - Display (from malidod): DxgkCbAcquirePostDisplayOwnership, map the GOP fb write-combined, one source/one
    target, A8R8G8B8/X8R8G8B8 only (display.c:11-14,56-75); child typed **HDMI**, always connected, no EDID
    (display.c:99-123); signal timing synthesised as width+280/height+45 @60 Hz because a full driver may not
    leave frequencies unspecified (display.c:28-49); hand-back via StopDeviceAndReleasePostDisplayOwnership
    (display.c:87-96); SystemDisplayEnable/Write for bugcheck screens (display.c:431-460).
  - INF: Class=Display, `ACPI\RKCP7402`, `UserModeDriverName = maliumd.dll` x4, demand start (maliwddm.inf).
- 2026-10-12 — drivers/malidod (KMDOD, WIN8 interface, DxgkInitializeDisplayOnlyDriver; malidod.c:842,873) and
  drivers/m1probe (KMDF 1.33, 280 lines) read. m1probe = the self-contained write-value step: RK PMU check
  (m1probe.c:115-126), GPU_ID 0x7402 check (m1probe.c:131), soft reset, L2/tiler/shader PWRON, 5 contiguous
  MmNonCached pages = L0..L3 + data (m1probe.c:154-172), Mali-LPAE entries (m1probe.c:174-179), WRITE_VALUE job
  descriptor words (m1probe.c:182-186: `data[4]=1|(2<<1)|(1<<16)` is_64b/type 2/index 1, target at words 8-9,
  `data[10]=6` immediate32, value word 12 = 0xC0FFEE42), AS0 programming (m1probe.c:190-198), **JS0** start
  with affinity = SHADER_PRESENT and JS_CONFIG = pri 8 | flush start/end (m1probe.c:57-58,200-208), poll
  JOB_INT_RAWSTAT 0x10001 (m1probe.c:210). malidod binds `ACPI\RKCP7403` (DSP0) in its INF.
- 2026-10-12 — build system read: .github/workflows/ci.yml - all 4 drivers via `msbuild <drv>.vcxproj
  /p:Platform=ARM64 /p:WindowsTargetPlatformVersion=10.0.26100.0` on windows-2022 (ci.yml:11,31-38), toolset
  WindowsKernelModeDriver10.0, KMDF 1.33 (malikm/m1probe .vcxproj:30-38), WDM + displib.lib for malidod/maliwddm
  (.vcxproj:32,49-51); test-signed with a self-signed cert, inf2cat /os:10_RS3_ARM64 (ci.yml:40-69). User-mode
  tests (m3test/m5test/etwrec) with cl on windows-11-arm (ci.yml:78-112); host simulation of malikm under
  ASan/UBSan on Linux (ci.yml:121-127, tests/malikm-host). Mesa: .github/workflows/mesa.yml - tag
  **mesa-26.2.0** from a GitHub mirror (mesa.yml:20-24), patches applied with `git apply` in name order
  (mesa.yml:187-198), win32-compat + malikm_ioctl.h + maliwddm_abi.h copied into mesa/include/win32-compat
  (mesa.yml:191-193); **meson + ninja, clang-cl (MSVC ABI) from VS's Tools\Llvm\ARM64** (mesa.yml:202-209),
  `-Dgallium-drivers=softpipe,panfrost -Dgallium-d3d10umd=true -Dmesa-clc=system -Dllvm=disabled`
  `-DHAVE_FUNC_ATTRIBUTE_PACKED` (mesa.yml:210-222); libpan OpenCL kernels precompiled to SPIR-V on a **Linux**
  job with LLVM 18 (mesa.yml:107-157) and copied in by the `ci/clc-stubs/mesa_clc` python stand-in
  (ci/clc-stubs/mesa_clc:1-18); smoke tests: softpipe triangle, panfrost fallback, `MALIKM_FAKE` in-process
  emulated driver + pandecode trace (mesa.yml:236-320). Output = `libgallium_d3d10.dll` (renamed maliumd.dll).
- 2026-10-12 — mesa-patches/0001-0019 read (raw `git diff` files, no commit messages; per-patch table in the
  inventory section). **Applicability check, mechanical:** all 19 apply in order, cleanly, to our local
  `mesa-26.2` clone (branch 26.2, HEAD `9a0169f2` 2026-10-01 "docs: add sha sum for 26.2.4", VERSION 26.2.4) -
  method: `GIT_INDEX_FILE=<scratch> git read-tree HEAD; git apply --cached` per patch (no working-tree or
  real-index change). **Gotcha:** the clone has `core.autocrlf=true`, so the .patch files are CRLF on disk and
  every patch FAILS until converted to LF (`tr -d '\r'`); their CI applies on Linux/LF (mesa.yml:57-60).
  They pin tag `mesa-26.2.0` (mesa.yml:21); 26.2.4 also works for patch application (build not tried).
- 2026-10-12 — win32-compat read (README 15 lines; malikm_drm.c 1429 lines). Three backends behind one libdrm
  API: (1) `\.\MaliG52` DeviceIoControl to malikm (malikm_drm.c:67-105,139-180,192-285); (2) **maliwddm** via
  D3DKMT thunks resolved from win32u at run time, or - inside the hardware UMD - the runtime's
  D3DDDI_DEVICECALLBACKS after `malikm_bind_d3dddi` (malikm_drm.c:556-627,973-1015); mapping table at
  malikm_drm.c:556-570: CREATE_BO=CreateAllocation+ALLOC_INFO escape, mmap=Lock held to GEM_CLOSE,
  SUBMIT=one MW_CMD_JOB via Render with BOs as allocation list, **WAIT_BO and every syncobj wait = WaitForIdle**
  (malikm_drm.c:1094-1097,1156-1158), syncobjs are counters only (malikm_drm.c:1151-1167), GEM_CLOSE defers
  destroy to the next idle (malikm_drm.c:615-629,1126-1150); present = pfnPresentCb from a locked linear
  X8R8G8B8 surface (malikm_drm.c:922-970); (3) `MALIKM_FAKE` in-process emulation whose GET_PARAM table is the
  **RK3576 G52** (PROD_ID 0x7402, SHADER_PRESENT 0x7, MMU_FEATURES 0x2823, L2_FEATURES 0x07120206, ...)
  (malikm_drm.c:1221-1290). Not implemented: PRIME, syncobj fds/timelines, anon/file mmaps -> ENOSYS
  (win32-compat/README.md:13-15). Other headers: pthread.h, unistd.h, poll.h, sys/mman.h, xf86drm.h, ... (POSIX
  surface for Panfrost, compile-only).
- 2026-10-12 — docs read: M5.2-PLAN.md (viogpu3d precedent + WDDM 1.3 aperture design, M5.2-PLAN.md:7-49; steps
  a-e table M5.2-PLAN.md:71-79; open questions :84-93), results/m5.2-step-c2/README.md (hardware UMD triangle
  PASS in **WinPE 22621**, FL 0xa000 = 10_0, 17 jobs 0 faults; c2 README:3-20), results/m5.2-board-run6 (full
  adapter on **installed** Windows with d3d10warp as UMD: no bugcheck, **local screen and RDP black**, DWM's
  device died after an unknown escape c000000d; Render/Present/SubmitCommand never called; board-run6:1-26),
  results/m5.2-pe-whea README (shutdown WHEA was RK USB-C, not the GPU driver; separate finding: 2 job read
  faults 0x4002c3 per present.exe from immediate GEM_CLOSE destroy, fixed in 44de51c "not yet verified on
  hardware"). git log: last commit 16f7e7c 2026-10-03; step d user-mode half (DXGI pfnPresentCb -> KMD CPU blt)
  landed in 1a32790/50c947f with no result file -> **unverified**; step e (DWM) never attempted (M5-PLAN.md:95
  "M5.5 DWM on the Mali" still a plan). Every GPU-through-WDDM result (M5.2 b, c1, c2) is WinPE-only; M5.1 (malidod, display only) passed on installed Windows 11 23H2 (results/m5.1-board-2026-10-02.md:3-5).
- 2026-10-12 — housekeeping: this file was first written in the main checkout; moved into the session worktree
  `.claude/worktrees/agent-a2e4467b6b5f2b792` after fast-forwarding its branch to main `7862134` (which adds
  G3 `Beyond0LteGpuProbeDxe` + notes/gpu-m16-g2g3.md). Commits go on the worktree branch, not on main.
- 2026-10-12 — G76 facts gathered for the retarget: drm/panfrost (local `linux` clone b20a9b5f9 2026-04-08)
  `GPU_MODEL(g76, 0x7001)` / `g52, 0x7002` (panfrost_gpu.c:226-227), model match masks `id & 0xf00f` for
  Bifrost (panfrost_device.h:231-237); hw_features_g76 = g52's set **+ TLS_HASHING + 3BIT_EXT_RW_L2_MMU_CONFIG**
  (panfrost_features.h:73-97); hw_issues_g76 = hw_issues_g52 = 0 (panfrost_issues.h:248,259); TLS_HASHING ->
  `SC_TLS_HASH_ENABLE` BIT(17) in GPU_SHADER_CONFIG 0xF04 (panfrost_gpu.c:141-145; panfrost_regs.h:180,211);
  3BIT_EXT_RW_L2_MMU_CONFIG is declared (panfrost_features.h:25), listed in two models' feature masks (:97 g76, :110) and the
  GPU_L2_MMU_CONFIG 0xF0C fields are defined (panfrost_regs.h:182-203), but **no drm/panfrost .c file writes it** (grep);
  page-table format = ARM_MALI_LPAE (legacy) for every SoC except MediaTek MT8188/8192/8370 which force AArch64
  (panfrost_mmu.c:786-795; panfrost_drv.c:1126-1150) -> Mali-LPAE as in maligpu.c is the mainline default for G76
  too; coherency from DT `dma-coherent` (panfrost_drv.c:977) else COHERENCY_NONE (panfrost_gpu.c:276-277).
  Mesa 26.2: `PAN_PROD_ID(7,2,1) "G76"` TB 16384/8192 vs RK3576's `PAN_PROD_ID(7,4,2) "G52 r1"` TB 8192/8192
  (mesa:src/panfrost/model/pan_model.c:80-82); PAN_PROD_ID = arch_major<<16|arch_minor<<8|prod_major
  (pan_model.h:39-40), GPU_ID fields 31:28/27:24/23:20/19:16 (pan_model.h:25-28) -> G76 GPU_ID[31:16] = 0x72?1;
  bifrost compiler quirks are 0 for both arch 7.2 and 7.4 (mesa:src/panfrost/compiler/bifrost/bi_quirks.h:28-43);
  no other prod-id special case touches 0x7xxx in gallium/panfrost (grep: pan_screen.c:144 is T760,
  pan_blend_cso.c:114/pan_shader.c:176 are `< 0x700` Midgard tests).
- 2026-10-12 — our side: DSDT convention GSIV = SPI + 32, Level/ActiveHigh (ours:firmware/Platform/Beyond0LtePkg/
  AcpiTables/Dsdt.asl:244,319-322), _HIDs EXYN9820/SAMS9820/SCSY0761 (Dsdt.asl:219,251,269); GOP fb
  0xCA000000 1080x2280x4 (ours:Exynos9820.h:865-879); `EXYNOS_G3D_BASE 0x18500000` and `EXYNOS_CMU_G3D_BASE
  0x18400000` are **SOURCE-ONLY** in Exynos9820.h:774-776, PMU base VERIFIED (Exynos9820.h:280);
  G2/G3 power facts from notes/gpu-m16-g2g3.md F2-F7 (pmucal on-sequence, DTZPC SMC, ACPM DVFS ch 5 cmd 10).
  Licensing precedent: GPL-2.0-only subtree `drivers/S6SY761Touch/` in a BSD-2-Clause-Patent repo
  (ours:README.md:23-30,212-217; CONTRIBUTING.md:32-39).

## Inventory (deliverable 1)

All `file:line` below are in `C:\Users\ilya\code\mali\wddm-mali-g52` @16f7e7c unless prefixed.

### I1. What it is, how far it got
- Target: RK3576 Mali-G52 (Bifrost **JM**), regs `0x27800000` 128 KiB, IRQ SPI 347/348/349 = GSIV 379/380/381,
  power `PD_GPU` + RK806 DCDC5, clock `CLK_GPU` via TF-A SCMI (README.md:39-47). Board ArmSoM CM5-IO,
  firmware edk2-rk3576 (README.md:49-50).
- Milestones (README.md:23-31): M1 KMDF write-value job **done** (2026-10-01), M2 Mesa 26.2 d3d10umd+softpipe+
  panfrost built for Windows ARM64 in CI, M3 first clear, M4 first triangle byte-identical to softpipe
  (2026-10-02), M5.1 display-only driver, M5.2 (b) job through WDDM, (c1) triangle through WDDM, (c2) Mesa
  loaded by the D3D runtime as the adapter's UMD on a hardware device (2026-10-03,
  results/m5.2-step-c2/README.md:3-20).
- **Not done:** step (d) present/flip to the screen from a hardware device - code landed (commits 1a32790,
  50c947f) but no result file; step (e)/M5.5 **DWM on the Mali never attempted** (M5.2-PLAN.md:78-79,
  M5-PLAN.md:95). Every GPU-through-WDDM pass (M5.2 b/c1/c2) is **WinPE 22621** (M5.1 display-only passed on installed 23H2,
  results/m5.1-board-2026-10-02.md:3-5); on the installed Windows the full adapter
  gave a **black screen** (results/m5.2-board-run6-2026-10-02.md:1-26). Local clone HEAD is 2026-10-03.
- Usability verdict (INFERENCE): a working, small, well-commented reference for (1) Mali JM bring-up on
  Windows (m1probe, maligpu), (2) a WDDM 1.3 aperture-segment KMD that dxgkrnl accepts, (3) the 19 Mesa
  patches + libdrm shim that make d3d10umd+panfrost a hardware UMD. It is **not** a desktop driver yet: no
  flips, no vsync, CPU blits for every present, one job chain at a time, WaitForIdle for every BO wait, no power
  management.

### I2. Architecture (layers)
```
D3D11 runtime (FL10_0) -> maliumd.dll = Mesa libgallium_d3d10.dll (d3d10umd + panfrost gallium + mesa-patches)
  panfrost pan_kmod "panfrost" backend, unchanged (uAPI 1.1) -> win32-compat/malikm_drm.c libdrm shim
     backend A: \\.\MaliG52 IOCTLs -> drivers/malikm (KMDF, M3/M4)
     backend B: D3DKMT / runtime D3DDDI callbacks -> drivers/maliwddm (WDDM 1.3, M5.2)
     backend C: MALIKM_FAKE in-process emulation (CI)
KMD: drivers/maliwddm = display half (from drivers/malidod) + GPU half (drivers/common/maligpu, shared w/ malikm)
bring-up only: drivers/m1probe (KMDF, one WRITE_VALUE job, self-contained)
```
(maliwddm.h:1-17; malikm.h:1-10; win32-compat/README.md:6-12; malikm_drm.c:556-570; malikm_ioctl.h:1-21.)

### I3. DXGK DDIs (drivers/maliwddm; DriverEntry adapter.c:519-586; WDDM 1.3 interface adapter.c:523)
- Implemented with real work: AddDevice, StartDevice (display + GPU init; the GPU is optional, the adapter
  starts without it, adapter.c:128-160), StopDevice, QueryAdapterInfo (DRIVERCAPS, QUERYSEGMENT3,
  HISTORYBUFFERPRECISION=64, 64BITONLYCAPS=0, DISPLAY_DRIVERCAPS_EXTENSION=0; adapter.c:315-362), Escape
  (adapter.c:402-435), InterruptRoutine/DpcRoutine (adapter.c:229-243), CreateAllocation/DestroyAllocation/
  DescribeAllocation/GetStandardAllocationDriverData/OpenAllocation/CloseAllocation/BuildPagingBuffer
  (memory.c:103-280), CreateDevice/DestroyDevice/CreateContext/DestroyContext/Render/Present/Patch/
  SubmitCommand/QueryCurrentFence/ResetFromTimeout/ResetEngine/CollectDbgInfo/GetNodeMetadata
  (command.c:25-567), the VidPN set QueryChildRelations, QueryChildStatus, IsSupportedVidPn,
  RecommendFunctionalVidPn, EnumVidPnCofuncModality, SetVidPnSourceVisibility, CommitVidPn,
  UpdateActiveVidPnPresentPath, RecommendMonitorModes, QueryVidPnHWCapability,
  StopDeviceAndReleasePostDisplayOwnership, SystemDisplayEnable/Write (display.c:87-460).
- Stubs: SetPowerState (success, touches no hardware, adapter.c:201-209), ResetDevice/Unload/QueryInterface/
  DispatchIoRequest (adapter.c:192-225), SetPointerPosition ok / SetPointerShape NOT_SUPPORTED
  (adapter.c:437-449), QueryDeviceDescriptor (no EDID, display.c:144-151), SetVidPnSourceAddress (does
  nothing, display.c:462-468), ControlInterrupt/GetScanLine (no vsync, command.c:583-598), PreemptCommand/
  CancelCommand/QueryEngineStatus/QueryDependentEngineGroup (command.c:486-492,569-581,617-623), RenderKm
  NOT_SUPPORTED (command.c:604-610), RestartFromTimeout (command.c:526-530).
- Every DDI is wrapped by a registry trace (Enter_/Seen_/Order_/Fail_ values, flushed to disk per write,
  adapter.c:8-88,451-517) - the method that found every M5.1/M5.2 failure (adapter.c:3-4). Reusable as is.
- Lessons encoded as comments (all on 22621): render-only on an ACPI GPU is refused as a POST device
  (maliwddm.h:24-34); a full driver must give real target frequencies (display.c:28-33); the WDDM 1.2
  mandatory render caps (adapter.c:267-282); the per-engine TDR DDIs must be non-NULL
  (command.c:557-559,612-616).

### I4. Memory / segment model
- One **aperture** segment, 1 GiB, CPU-visible, `CacheCoherent = FALSE`, paging buffer 10 pages in it
  (memory.c:14-40; maliwddm.h:20-21). VidMm owns residency; the Mali MMU *is* the aperture (memory.c:1-9).
- GPU VA per allocation reserved at CreateAllocation from a bitmap allocator over 32 MiB..4 GiB, executable
  allocations inside one 16 MiB window (the shader PC is 24 bits) (memory.c:63-70; maligpu.h:118-122;
  maligpu.c:295-320). Fixed for life, so `Patch` has nothing to do (command.c:252-265; maliwddm_abi.h:8-13).
- MAP/UNMAP_APERTURE_SEGMENT map the MDL's PFNs into AS0 (memory.c:242-276); every other paging op
  NOT_SUPPORTED (memory.c:277-279). Residency is forced by listing every allocation as a patch location
  (command.c:74-87; lesson from WinPE runs 13-14).
- One address space AS0 for every process (maligpu.h:149-152; malikm.h:7-9) - no per-process isolation.
- Page tables: Mali-LPAE, 4 levels, uncached contiguous pages below 40 bits; PTE 0x2C5 = block | ATTRINDX 1 |
  R | W | outer shareable, XN bit 54; TRANSTAB = L0 | 0x7, MEMATTR 0x00888d88, TRANSCFG 0
  (maligpu.c:115-130,188-203; maligpu.h:124-127). Map = write PTEs, DSB, LOCK region + FLUSH_PT
  (maligpu.c:273-293,322-342).

### I5. Job submission
- The UMD puts `MW_COMMAND{JOB, slot, jc}` (<= 32) in the command buffer and the BOs in the allocation list
  (maliwddm_abi.h:40-50; malikm_drm.c:1098-1125). Render copies them into the DMA buffer's private data; the
  DMA buffer itself holds only a magic word (command.c:64-121). SubmitCommand -> 16-deep ring at DIRQL through
  DxgkCbSynchronizeExecution (command.c:365-384,453-484). Kick starts **one chain at a time** via the JS_*_NEXT
  registers, chain flag alternating (command.c:284-303; maligpu.c:358-371). Completion -> DMA_COMPLETED with
  the submission fence + DPC (command.c:269-280,347-357). Fault/timeout (2 s) -> hard stop, drop the rest of
  the submission, MkGpuBringup (command.c:305-345). TDR: ResetFromTimeout/ResetEngine = full re-bringup
  (command.c:503-567). Preemption off system-wide by the INF (maliwddm.inf `EnablePreemption 0`).
- Slot policy: fragment (`PANFROST_JD_REQ_FS`) -> JS0, everything else -> JS1 (malikm_drm.c:1115;
  malikm/gpu.c:60). M1's write-value used JS0 (m1probe.c:200-208).

### I6. UMD <-> KMD
- Allocation private data `MW_ALLOCATION_INFO{Version=1, Flags NOEXEC|PRIMARY, Size, W, H, Pitch, Format}`
  (maliwddm_abi.h:25-36). Escapes (private data starts with a code): 1 ALLOC_INFO (allocation handle ->
  GpuVa, Size), 2 GPU_INFO (45 DRM_PANFROST_PARAM values + valid mask), 3 STATS (counters)
  (maliwddm_abi.h:52-95; adapter.c:290-313,402-435; memory.c:284-299). Panfrost's GET_PARAM is answered from
  GPU_INFO, so Mesa sees exactly the registers the KMD read (malikm_drm.c:1028-1034).
- Inside the hardware UMD the shim switches from its own D3DKMT device to the runtime's callbacks
  (`malikm_bind_d3dddi`, needs pfnEscapeCb/AllocateCb/RenderCb/CreateContextCb; malikm_drm.c:973-1015;
  patch 0018). Sync: syncobjs are counters; WAIT_BO / syncobj waits = WaitForIdle or the event of the last
  submission (malikm_drm.c:556-570,871-920,1094-1097,1151-1167).

### I7. GOP framebuffer kept
- `DxgkCbAcquirePostDisplayOwnership` at StartDevice, PhysicAddress mapped write-combined, non-32-bpp refused
  (display.c:11-14,56-75); one source / one target / one mode = the firmware's (display.c:16-26,156-340);
  presents (Blt/ColorFill) are CPU copies into it at SubmitCommand (command.c:188-250,462-466); hand-back to
  BasicDisplay in StopDeviceAndReleasePostDisplayOwnership (display.c:87-96); bugcheck screen via
  SystemDisplayEnable/Write (display.c:431-460). Flips and scan-out reprogramming (RK VOP2) never implemented
  (display.c:3-8,462-468; PLAN.md:49-54).

### I8. Interrupts vs polling
- One ISR claims all three lines (maligpu.c:396-426; adapter.c:229-237). Because "whether dxgkrnl connects
  the Mali's interrupts on this ACPI device is not yet known", a 1 ms KTIMER DPC also polls while work is in
  flight (command.c:9-13,406-435). malikm: IRQ event + 1 ms tick (malikm/gpu.c:71-74). Whether the IRQs
  actually fired on the board is not stated in the result files read here (m5test prints `Irqs`; UNVERIFIED).

### I9. Mesa patches (mesa-patches/, applied in name order to tag mesa-26.2.0; mesa.yml:21,194-198)
| # | files | what it does |
|---|---|---|
| 0001 | d3d10umd/Query.cpp | `(void)pQuery` - release-build unused-variable error (/we4189) |
| 0002 | meson.build | d3d10umd needs `HAVE_GFX_COMPUTE` (it emits TGSI via ureg) |
| 0003 | bifrost.h, bi_pack.c, valhall.h, pan_nir_lower_tex.c, genxml/decode.h, pan_samples.c | explicit GCC-layout packers for the Bifrost instruction bitfield structs so MSVC-ABI builds encode correctly; no statement expressions (checked by tests/bifrost-pack/check.c, mesa.yml:130-137) |
| 0004 | compiler/pan_nir.h | `pan_bi_tex_flags` bool bitfields -> unsigned (stays 32 bits under the MSVC ABI) |
| 0005 | util/u_atomic.h | clang-cl uses the GCC `__atomic` builtins |
| 0006 | compiler/pan_compiler.h | `-Wpadded` error only off MSVC (ENUM_PACKED is empty there) |
| 0007 | bi_spill_ssa.c, bifrost_compile.c, pan_trace.c, os_mman.h, u_math.c | alloca header, field-by-field key build, os_mmap -> shim, no ARM64_FPCR under clang-cl |
| 0008 | include/meson.build | adds `include/win32-compat` for panfrost on Windows |
| 0009 | renderonly.h, pan_blend_cso.h, pan_device.h, pan_fb_preload.h, pan_mempool.c, pan_mod_conv_cso.h, pan_resource.c, pan_perf.h | winsys_handle carries a HANDLE on Windows; pthread includes |
| 0010 | clc/pan_compile.c | read SPIR-V in binary mode, without mmap |
| 0011 | pan_fb_preload.c, bi_opcodes.h.py, bifrost.h, compiler.h, valhall.h, midgard.h, pan_compiler.h, pan_blend.h | enum bitfields -> unsigned (MSVC enum bitfields are signed) |
| 0012 | gallium/auxiliary/meson.build, targets/d3d10umd/{d3d10_gdi.c,meson.build} | build renderonly + link panfrost into libgallium_d3d10; `GALLIUM_DRIVER=panfrost` path in d3d10_create_screen |
| 0013 | renderonly.c | HANDLE -> int for drmPrimeFDToHandle |
| 0014 | d3d10_gdi.c | open the Mali via `malikm_open`, driconf for panfrost, glsl type singleton, `flush_frontbuffer` = map/de-tile -> GDI StretchDIBits |
| 0015 | panfrost/lib/kmod/meson.build | links `win32-compat/malikm_drm.c` into pankmod |
| 0016 | pan_fb_preload.c, pan_compiler.h | more typedef-enum bitfields -> unsigned |
| 0017 | d3d10umd/OutputMerger.cpp, State.h | no depth/stencil test without a DSV (panfrost dropped every fragment) - a real d3d10umd bug, not Windows-specific |
| 0018 | d3d10umd/{Adapter,Device}.cpp, State.h, d3d10_gdi.c | screen per **device**, created after `d3d10_bind_runtime(hRTAdapter, hRTDevice, KT callbacks, DXGI callbacks)` |
| 0019 | d3d10_gdi.c | present through DXGI `pfnPresentCb` on the hardware runtime (the KMD CPU-blts it); GDI otherwise |

File lists from `grep '^diff --git'` per patch (2026-10-12). Nothing in any patch is G52- or RK3576-specific
(reading: the only Mali names are comments pointing at wddm-mali-g52). They apply cleanly to 26.2.4 after
CRLF -> LF (progress log).

### I10. win32-compat
See the progress-log entry (malikm_drm.c, three backends; README.md:13-15 lists what is not implemented). Its
only hardware-specific content is the `MALIKM_FAKE` GET_PARAM table (malikm_drm.c:1265-1290) and the device
name `\\.\MaliG52` (malikm_ioctl.h:37-39).

### I11. Build / toolchain
- KMDs: MSBuild + WDK **10.0.26100.0**, `WindowsKernelModeDriver10.0`, ARM64, KMDF 1.33 for the KMDF drivers,
  WDM + displib.lib for the DXGK drivers, test-signed (ci.yml:11,31-69; drivers/*/*.vcxproj:27-51). Plain
  vcxproj files, no custom build steps beyond `MW_BUILD=$(GITHUB_RUN_NUMBER)` (maliwddm.vcxproj:47).
- UMD: Mesa via **meson + ninja with clang-cl** (MSVC ABI, from VS `Tools\Llvm\ARM64`) on a native
  windows-11-arm runner, `-Dllvm=disabled`, `-Dmesa-clc=system` served by a Python stub that copies libpan
  SPIR-V precompiled on **Linux with LLVM 18** (mesa.yml:107-157,175-226; ci/clc-stubs/mesa_clc:1-18). Needs
  winflexbison and pip meson/ninja/mako/pyyaml/packaging (mesa.yml:173-183).
- Host tests: malikm against a simulated G52 under ASan/UBSan on Linux (tests/malikm-host; ci.yml:121-127).

### I12. Test procedure (board)
- M1: `tools/windows/m1-test.cmd` installs m1probe, reads `Result`/`Step` from the device's registry key
  (m1probe.c:9-12). M3/M4: `m3-test.cmd` (malikm + m3test + triangle with GALLIUM_DRIVER=panfrost),
  `m4-trace.cmd` (pandecode trace). M5: a **WinPE stick**: `m5-pe-test.cmd` (+ `m5-pe-autorun.cmd`) loads the
  drivers, runs m5test (D3DKMT write-value + STATS) and triangle/present as software and hardware devices,
  saves registry traces and an ETW DxgKrnl recording (etwrec.c) to the stick after every step
  (m5-pe-test.cmd:1-20). Installed-Windows scripts + `m5-wddm-rescue.cmd` (disables the service offline from
  WinPE when the screen goes black).
- Pass criteria are pixel checks: centre/corner colours of the triangle (results/m5.2-step-c2/README.md:11-14).

### I13. Known gaps (from their docs/commits, plus reading)
1. Present = GPU render -> CPU de-tile/swizzle readback (patch 0014 `pipe_texture_map`) -> DXGI present ->
   KMD CPU blt into the GOP fb (command.c:188-250): two full-frame CPU copies per present, no flip.
2. One job chain in flight; every BO wait = whole-GPU idle (I5, I6).
3. No GPU power management / DVFS; SetPowerState is a no-op (adapter.c:201-209); the firmware must leave GPU
   power and clock on (PLAN.md:70-72,117).
4. Heap BOs allocated whole, no grow-on-fault (malikm/uapi.c:99-105; malikm_drm.c:1039-1040).
5. Single shared GPU address space, no per-process isolation (I4).
6. Interrupt delivery unconfirmed -> polling timer (I8).
7. DWM untested; installed-Windows full adapter = black screen with a WARP UMD (I1).
8. Step (d) and the deferred-destroy fix 44de51c are not verified on hardware (commit 44de51c body).
9. Mesa pinned to the last d3d10umd release (PLAN.md:58-61).

### I14. Licence implications for us (INFERENCE, not legal advice)
- Everything in wddm-mali-g52 is GPL-2.0 (README.md:51-54; `SPDX-License-Identifier: GPL-2.0` at the top of
  every .c/.h/.cpp and tools/m1 script, e.g. maligpu.c:1, maliwddm.h:1; the .cmd scripts and tools/etw/*.py carry
  no SPDX line and fall under the repo LICENSE) except code taken from Mesa (MIT). Reusing maligpu / maliwddm /
  m1probe makes those drivers **GPL-2.0** in their own subtree with their own LICENSE, exactly like
  `drivers/S6SY761Touch/` (ours:README.md:23-30,212-217; ours:CONTRIBUTING.md:36-39). Our BSD-2-Clause-Patent
  firmware is unaffected as long as no GPL code is copied into `firmware/`.
- **UEFI caution:** G4-G6 in `Beyond0LteGpuProbeDxe` must not copy maligpu.c / m1probe.c code (nor drm/panfrost,
  also GPL-2.0); they keep writing register sequences from cited hardware facts (offsets, values), as G3 does.
- UMD: `malikm_drm.c` (GPL-2.0) is linked into libgallium_d3d10.dll (patch 0015), so a shipped maliumd.dll is
  a GPL-2.0 combined work (Mesa's MIT is GPL-compatible) - ship its source. The 19 patch files sit in a
  GPL-2.0 repo but modify MIT files; upstreaming any of them would need the author's MIT relicence (ask).
- viogpu3d (BSD-3) influence is declared as design only (maliwddm.h:9-10).
- A NOTICE entry is required for any reuse (ours:NOTICE lists every reused project).

### I13 (continued)
10. **Panfrost is opt-in per process:** d3d10umd picks the gallium driver from `GALLIUM_DRIVER` (default
    llvmpipe/softpipe; mesa:src/gallium/targets/d3d10umd/d3d10_gdi.c:70-76) and patch 0012 adds the
    `"panfrost"` branch only; every test sets `GALLIUM_DRIVER=panfrost` in its script
    (tools/windows/m5-pe-test.cmd:87). DWM sets no such variable, so on the Mali adapter DWM would get
    **softpipe** - a Mesa change (panfrost whenever `d3d10_bind_runtime` bound a maliwddm runtime) is needed
    before any DWM test means anything (INFERENCE from the code; not tried by them).

## RK3576/G52 -> Exynos 9820/G76 retarget list (deliverable 2)

Legend: **src** = where wddm-mali-g52 encodes it; **new** = the S10e value and its source; UNVERIFIED = no
device read yet (the read that closes it is named). "SOURCE-ONLY" = from stock DT/kernel source, not read live.

| # | What | src (wddm-mali-g52) | RK3576 / G52 value | S10e / G76 value (source) | Change |
|---|---|---|---|---|---|
| R1 | GPU MMIO base/size | not hard-coded: first memory resource of the ACPI device (malikm/device.c:43-48; maliwddm/adapter.c:106-126); README.md:42 | 0x27800000, 128 KiB | **0x18500000, 0x5000** (stock DT `mali@18500000` reg 0x5000, notes/gpu-odin-rom.md:253-255; ours:Exynos9820.h:776 SOURCE-ONLY until G1/G3 live read) | DSDT only. Highest offset maligpu touches is AS0+0x34 = 0x2434 and JS2 = 0x1900 window (maligpu.h:75-112), all < 0x5000. The GPU block must be the **first** Memory32Fixed in `_CRS` (adapter.c:118) |
| R2 | Interrupts | up to 3 interrupt resources, one ISR, any order (malikm/device.c:49-59; maligpu.c:398-399) | GSIV 379/380/381 | **job GSIV 320, mmu 319, gpu 321** (SPI 288/287/289 + 32; gpu-odin-rom.md:38-39,253; +32 convention ours:Dsdt.asl:319-322). Trigger: DT flags cell 0 (= kernel default), Level/ActiveHigh by our DSDT convention - UNVERIFIED for these lines | DSDT only; G10 must check `Irqs` != 0 before dropping the 1 ms poll (command.c:406-435) |
| R3 | ACPI hardware ID | `ACPI\RKCP7402` in every INF + comments (maliwddm.inf; malikm/device.c:3-6; m1probe.c:3); malidod DSP0 `RKCP7403` | RKCP7402 | new _HID in our namespace style (`EXYN9820`/`SAMS9820` precedents, ours:Dsdt.asl:219,251) - a naming decision for G9, not a hardware constant | INF `[Standard.NTARM64]` lines; malidod/DSP0 pairing not needed (render-only was refused anyway, maliwddm.h:24-34) |
| R4 | "GPU is powered" check before the first GPU access | `PMU_PA 0x27380000`, STATUS0 0x230 bit25 == 0 && ACK0 0x120 bit0 == 0 (maligpu.h:16-18; maligpu.c:25-39; m1probe.c:15-17,115-126) | RK PMU, bit **clear** = on | PMU 0x15860000 (VERIFIED ours:Exynos9820.h:280) **+0x2104 EMBEDDED_G3D_STATUS bit0 == 1 and +0x2384 G3D_STATUS bit0 == 1**; then CMU_G3D 0x18400000 +0x2004 gate not forced off (MANUAL bit20 = 1 && CG_VAL bit21 = 0 means off) (notes/gpu-m16-g2g3.md F3, F5, F6; SOURCE-ONLY, G3 boot pending) | Rewrite `PmuSaysGpuIsOn` (polarity inverted, two bits, two blocks). CMU_G3D may only be read once both PMU bits are set (F3) |
| R5 | GPU_ID gate | `(GPU_ID >> 16) != 0x7402` -> refuse (maligpu.c:152; m1probe.c:131); fake table PROD_ID 0x7402 (malikm_drm.c:1271) | 0x7402 = PAN_PROD_ID(7,4,2) "G52 r1" (mesa:pan_model.c:82) | **0x72?1** = PAN_PROD_ID(7,2,1) "G76" (mesa:pan_model.c:80; field layout pan_model.h:25-28,39-40); drm `GPU_MODEL(g76, 0x7001)` under mask 0xf00f (panfrost_gpu.c:226; panfrost_device.h:231-237). arch_rev nibble UNVERIFIED (G3 log) | compare `((id >> 16) & 0xF0FF) == 0x7201` |
| R6 | Init quirks | TILER_CONFIG read-back + `JM_CONFIG = 0xF << 16`, "for a G52: no shader quirks" (maligpu.c:95-98) | none else | G76 = G52 features **+ TLS_HASHING + 3BIT_EXT_RW_L2_MMU_CONFIG** (drm panfrost_features.h:73-97), issues 0 for both (panfrost_issues.h:248,259) -> add **GPU_SHADER_CONFIG 0xF04 = SC_TLS_HASH_ENABLE BIT(17)** (panfrost_gpu.c:141-145; panfrost_regs.h:180,211). 3BIT_EXT_RW_L2_MMU_CONFIG: no drm/panfrost .c writes GPU_L2_MMU_CONFIG 0xF0C (fields only defined, panfrost_regs.h:182-203) - whether the vendor kbase programs L2_MMU_CONFIG on 9820 is UNVERIFIED (read kbase r32p1 `kbase_hw_set_issues`/L2_MMU_CONFIG path) | add one register write + define |
| R7 | Core power-up | `L2/SHADER/TILER_PWRON_LO = *_PRESENT`, poll READY_LO == PRESENT (maligpu.c:100-107) | SHADER_PRESENT 0x7, L2 0x1 (malikm_drm.c:1273-1275) | MP12 -> 12 bits set in SHADER_PRESENT (layout UNVERIFIED; G76 MP11 vs MP12 also open, MILESTONES G3); L2_PRESENT UNVERIFIED (panfrost powers only core group 0 when L2_PRESENT != 1) | none if L2_PRESENT == 1 and SHADER_PRESENT fits 32 bits; else restrict to core group 0 like panfrost |
| R8 | Job affinity | `JS_AFFINITY_NEXT = SHADER_PRESENT` (maligpu.c:366-367; m1probe.c:205) | 0x7 | full mask if one core group (G3 read) | as R7 |
| R9 | Job IRQ mask | `JOB_INT_MASK 0x00070007`, slots 0-2 (maligpu.c:140,169) | JS_PRESENT 0x7 (malikm_drm.c:1277) | JS_PRESENT UNVERIFIED (G3 log); only JS0/JS1 are ever used (command.c:107) | none expected |
| R10 | Page-table format + AS config | Mali-LPAE, TRANSTAB = L0 \| 0x7, MEMATTR_LO 0x00888d88, TRANSCFG 0, PTE 0x2C5/0x3, XN bit 54 (maligpu.c:115-130; maligpu.h:124-127; PLAN.md:111-113) | proven on G52 under Windows (M1, PLAN.md:94-98) | G76 has AARCH64_MMU too, but drm/panfrost uses **ARM_MALI_LPAE for every SoC except MT8188/8192/8370** (panfrost_mmu.c:786-795; panfrost_drv.c:1126-1150) -> legacy LPAE is the mainline default on G76 as well | none in the KMD. **Our MILESTONES G5 plans AArch64-4K/ADRMODE 6** - align G5 with Mali-LPAE (the format the reused KMD uses) or carry both; decide at G5 |
| R11 | Physical-address reach | page tables, BOs and `HighestAcceptableAddress` limited to 40 bits (maligpu.c:194; malikm/uapi.c:111; adapter.c:260) | G52 MMU_FEATURES 0x2823 (malikm_drm.c:1282) | G76 MMU_FEATURES PA bits UNVERIFIED (G3 logs MMU_FEATURES); S10e DRAM PAs are far below 2^40 (bank list in ours:Exynos9820.h, high banks off for Windows today, MILESTONES:262) | none unless PA bits < 36 |
| R12 | Coherency | non-coherent: WC BOs, `CacheCoherent = FALSE`, SELECTED_COHERENCY 31 = none, flush clean+invalidate at job start and end (malikm/uapi.c:113-115,442; memory.c:31-33; adapter.c:306; maligpu.h:90-92; maligpu.c:360-362) | none | stock DT has **no `system-coherency`** -> vendor kbase stays non-coherent (gpu-odin-rom.md:168-171; kbase-side default UNVERIFIED); COHERENCY_FEATURES read at G3 | none expected |
| R13 | GPU power + clock owner | **firmware** powers PD_GPU, sets CLK_GPU = GPLL/6 = 198 MHz, ungates, then the driver only checks (PLAN.md:70-72,99-117); SetPowerState is a no-op (adapter.c:201-209) | edk2-rk3576 does it with plain MMIO; Linux uses SCMI (PLAN.md:70-72) | **UEFI must leave G3D on**: pmucal `embedded_g3d_on` (6 MMIO steps: PLL_G3D CON2/CON0 in CMU_CMU, G3D_CONFIGURATION 0x2380, EMBEDDED_G3D_OUT 0x2120 via the 0xC000 set-alias, EMBEDDED_G3D_CONFIGURATION 0x2100) **+ SMC 0x82000410 DTZPC restore (need_smc 0x18410204)** + **ACPM DVFS IPC ch 5 cmd 10 (`cal_dfs_set_rate(0xb04000a, Hz)`)** for clock *and voltage* (gpu-m16-g2g3.md F2-F5); start clock 260 MHz, OPPs 702..156 MHz (gpu-odin-rom.md:44-47,261). Whether sboot already leaves G3D on: UNVERIFIED (G3 first log line). | New UEFI work (G4a below). KMD unchanged (never touches power/clock) - but nothing under Windows throttles the GPU: pick a fixed **260 MHz** first (Samsung's own start clock); higher OPPs only with a thermal path |
| R14 | Display child | one `D3DKMDT_VOT_HDMI` child, always connected, no EDID (display.c:99-123); timing synthesised W+280/H+45 @ 60 Hz (display.c:28-49) | HDMI 1920x1080 | internal DSI panel 1080x2280 (s6e3fa7, ours:Exynos9820.h:865-879); report `D3DKMDT_VOT_INTERNAL` (INFERENCE: type is cosmetic to dxgkrnl, UNVERIFIED); timing stays synthetic (nothing is programmed) | 1-line type change; keep the synthetic timing |
| R15 | Firmware framebuffer | from `DxgkCbAcquirePostDisplayOwnership`, 32 bpp only (display.c:11-14,56-75) | VOP2 fb | GOP fb **0xCA000000, 1080x2280x4** (ours:Exynos9820.h:865-879), reserved in the UEFI map; BasicDisplay already drives it (MILESTONES G9 row); GOP pixel format -> D3DDDIFMT mapping UNVERIFIED (must be A8R8G8B8/X8R8G8B8 or StartDevice fails, display.c:63) | none if the GOP reports a BGRX 32-bpp mode |
| R16 | Names / strings | `\\Device\\MaliG52`, FILE_DEVICE 0x8A52 (malikm_ioctl.h:37-41), "Mali-G52 job manager" (command.c:553), INF strings (maliwddm.inf), MALIKM_FAKE params (malikm_drm.c:1265-1290) | G52 | G76 | cosmetic; keep the ABI names until it works, refresh the FAKE table from the G3 log |
| R17 | WRITE_VALUE bring-up job | JS0, `data[4] = 1 \| 2<<1 \| 1<<16`, target words 8-9, `data[10] = 6` (immediate32), value word 12 (m1probe.c:182-186,200-208) | worked on G52 | same descriptor format on all Bifrost v7 (genxml v7 already cited by MILESTONES G6); our G6 picks **JS1** (panfrost's slot for non-fragment); JS_FEATURES per slot UNVERIFIED (G3 can log GPU_JS_FEATURES(n), maligpu.h:42) | none, or JS1 |
| R18 | Mesa side | patches 0001-0019 + shim, GET_PARAM from the KMD (malikm_drm.c:1028-1034) | G52 r1 table row, TB 8192/8192 | G76 row exists in 26.2 (mesa:pan_model.c:80, TB 16384/8192), compiler quirks 0 (bi_quirks.h:28-43) - **no Mesa change for G76**; G76 has no Mesa CI coverage (notes/gpu-panfrost-repos.md) | none; plus gap 10 (panfrost as default on the maliwddm runtime) |
| R19 | Test vehicle | WinPE 22621 stick + scripts (I12) | WinPE boots from USB on RK | S10e: no WinPE path yet; Windows boots from UFS (sda) via our UEFI; a black-screen full adapter needs an offline service disable (their `m5-wddm-rescue.cmd`) - for us = mount the Windows partition from TWRP or a WinRE path (UNVERIFIED which works) | must exist before installing any DXGK driver (G10) |

## Biggest blocker for us

**Getting G3D powered and clocked before Windows, with nothing under Windows ever changing it.** On RK3576
the firmware does it with ~10 plain MMIO writes (PLAN.md:99-110); on the 9820 it is the pmucal MMIO sequence
**plus a secure-world SMC (DTZPC restore) plus an ACPM IPC request for clock and voltage** (gpu-m16-g2g3.md F2-F6),
none of which our UEFI does today (G3's power-on flag is an UNVERIFIED stub). Every rung from the first GPU
register access on depends on it; once UEFI hands over a powered GPU at a fixed OPP, the wddm-mali-g52 KMD
needs only the R4-R6 edits (PMU check, GPU_ID, TLS-hash quirk) plus DSDT/INF data. Second blocker: the
recovery path for a black screen when a full DXGK adapter replaces BasicDisplay (R19; their run 6). Third:
DWM has never run on any Mali + d3d10umd stack (I13 items 1, 7, 10).

## Recommended ladder changes (applied to MILESTONES M16 in the next commit)
- G4a (new, UEFI): power-on G3D (pmucal + DTZPC SMC + ACPM DVFS to 260 MHz) - replaces the stub.
- G5: page-table format decision -> Mali-LPAE (what the reused KMD uses; mainline default).
- G9: DSDT values from R1-R3.
- G9b (new): m1probe retargeted (R4, R5, R6, R17) = KMDF write-value under Windows.
- G10: full WDDM 1.3 maliwddm (display half keeps the GOP fb, R14-R16), not render-only; rescue path first (R19).
- G11: maliwddm GPU half: aperture segment + fixed VA, D3DKMT write-value (their step b, tools/windows/m5test.c).
- G12: pin Mesa 26.2 d3d10umd + panfrost with patches 0001-0019 (+ CRLF note) and the shim; softpipe UMD first,
  then panfrost through the runtime callbacks (their c1/c2).
- G13 (new): DWM on the Mali - panfrost default on the maliwddm runtime (gap 10), present path (their step d),
  then DWM composition measured.

## Review of 3e73db6 (adversarial, 2026-10-12)
Checked mechanically:
- Register reach: every offset defined in maligpu.h is <= 0x2434 (AS0 + TRANSCFG_HI) except the RK PMU PA
  (`grep 0x....` on maligpu.h) -> fits the G76's 0x5000 window (R1 holds).
- "No patch is G52/RK specific": `grep -i '7402|g52|rk3576|rockchip' mesa-patches/*` hits only comments naming
  wddm-mali-g52 and the `\.\MaliG52` string (0008:11, 0012:36, 0014:33,118,135, 0015:8) - holds.
- Cited lines re-read with sed: malikm_drm.c GET_PARAM/CREATE_BO/WAIT_BO/SUBMIT/GEM_CLOSE/SYNCOBJ/fake table
  (several were off by 4-10 lines in the first draft and were corrected before the commit), panfrost_gpu.c
  141-145/226-227/276-277, panfrost_mmu.c 786-795, panfrost_drv.c 977, pan_model.h 25-28/39-40, d3d10_gdi.c 70-76,
  m5-pe-test.cmd:87, gpu-odin-rom.md 38-39/44-47/168-171/253-261, MILESTONES:262.
Found and fixed (this follow-up commit):
1. "Every hardware pass after M4 is WinPE" was false: M5.1 (malidod) passed on installed Windows 11 23H2
   (results/m5.1-board-2026-10-02.md:3-5). Reworded to "every GPU-through-WDDM pass".
2. "3BIT_EXT_RW_L2_MMU_CONFIG used nowhere (only features.h:25)" was imprecise: it is also in the g76 mask (:97)
   and :110, and the L2_MMU_CONFIG fields are defined (panfrost_regs.h:182-203); what holds is that no .c writes
   the register.
3. "SPDX GPL-2.0 at the top of each source" overstated: the 15 .cmd scripts and tools/etw/*.py have no SPDX line
   (loop over `git ls-files`); they fall under the repo LICENSE.
4. "Repo quiet since 2026-10-03" is only what the local clone shows -> "Local clone HEAD is 2026-10-03".
Left as is (marked): interrupt delivery on their board UNVERIFIED (I8); GOP pixel-format mapping UNVERIFIED (R15).
