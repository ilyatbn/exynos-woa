# GPU: freedesktop.org panfrost group repos (research)

Task: enumerate + clone every project of https://gitlab.freedesktop.org/panfrost into
`C:\Users\ilya\code\mali\<project>` (clones stay OUT of this repo), study them for a Windows
Mali-G76 (Bifrost) driver, turn findings into GPU milestone tasks. No phone/adb, no builds.

## Progress log

- 2026-10-12 — GitLab API reachable without auth (no Anubis challenge for `/api/v4`):
  `GET /api/v4/groups/panfrost/projects?include_subgroups=true&per_page=100` returned **22
  projects** (page 2 empty); `GET /api/v4/groups/panfrost/subgroups` returned `[]` (no subgroups).
  Projects: devboard-images, vk-gl-cts, panfrost-hwc-helper, hwc-helper (fork of
  Fahien/panfrost-hwc-helper), csf.rs, valhall-docs, pandecode-standalone, bifrostc, bifrost.rs,
  shader-db (fork of mesa/shader-db), nondrm, midgard-performance-analyze, mali_kbase, linux,
  future-linux-panfrost (API `empty_repo: true`), panfrost (deprecated), panfrost.freedesktop.org,
  mali-isa-docs, linux-panfrost (deprecated), mesa (fork of mesa/mesa), ShaderProgramDisassembler,
  panloader.
- 2026-10-12 — 19 small repos cloned with full history into `C:\Users\ilya\code\mali\` (no errors);
  `future-linux-panfrost` clones as an **empty repository** (git warning, matches API `empty_repo`).
  mesa, linux, vk-gl-cts: shallow (`--depth 1`) clone started.
- 2026-10-12 — prior project state read: `notes/gpu-panfrost.md` (M16 opened 2026-10-11, nothing
  researched), `notes/d5-gpu-display.md` (G76 = Bifrost v7 per docs.mesa3d.org; `mali@18500000`
  reg size 0x5000, `pd_embedded_g3d`, gpu_max_clock 702000 from the LineageOS
  `exynos9820-mali_common.dtsi`; D5b(d) correction: panfrost userspace pairs with the panfrost DRM
  kernel driver, kbase pairs with the closed DDK), MILESTONES.md M16 table (7 rows),
  DRIVERS-MILESTONES.md D5/D5b rows. `rg -i "g3d|gpu|mali|1850" Exynos9820.h` → **no GPU
  constant in the verified platform header** — 0x18500000 is still a DT-only (LineageOS dtsi) value.
  `notes/m12-acpm.md` has a working ACPM IPC protocol description (used for PMIC regs); no G3D
  power/clock notes exist yet (only a thermal zone name `G3D` in `notes/cpu-driver-9820.md:192`).
  Core count discrepancy: task prompt says "MP12", d5 note says MP11 for S10/S10e (a phone-shop
  source) — UNVERIFIED; closes by reading GPU `SHADER_PRESENT` on the device.
- 2026-10-12 — big clones finished. `mesa` (fork) default branch HEAD = 2021-01-16 `6add056`
  ("Random work-in-progress branches", 41 branches; README points to upstream). `vk-gl-cts`
  HEAD 2025-01-17 `a998848`. `linux` fork: **default branch `master` is 2019-01-19 `2339e91d0`
  (v5.0-rc merge) and its checkout failed on Windows** (`invalid path
  drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c`, `include/soc/arc/aux.h` — `aux` is a reserved
  DOS name). The fork has 86 branches (`git ls-remote --heads`): panfrost-4.19/5.0, mt8192*,
  panthor-v3..v5, many `tyr*` (Rust CSF driver) branches, `drm-misc-next`. Fixed by fetching
  `drm-misc-next` at depth 1 (HEAD 2026-04-08 `b20a9b5f9`) and a **non-cone sparse checkout** of
  `drivers/gpu/drm/{panfrost,panthor,tyr,scheduler}/`, `include/uapi/drm/`, `Documentation/gpu/`,
  `Documentation/devicetree/bindings/gpu/`, `arch/arm64/boot/dts/exynos/`,
  `drivers/iommu/io-pgtable-arm.c`, `include/linux/io-pgtable.h` (plus every `Makefile`/`README`
  via the pattern). Pitfall: in Git Bash, `git sparse-checkout set '/x/'` gets MSYS-mangled to
  `C:/Program Files/Git/x/` → 0 % checkout; needs `MSYS_NO_PATHCONV=1`.
- 2026-10-12 — because the group's `mesa` fork is stale (2021), an **extra** shallow clone of
  upstream `https://gitlab.freedesktop.org/mesa/mesa.git` → `C:\Users\ilya\code\mali\mesa-upstream`
  (not a panfrost-group project; labelled as such in the README).

### Small-repo survey (from each clone; `git log -1`, `du -sh --exclude=.git`)

- `nondrm` (2019-03-24, 22 commits): **glue from the panfrost userspace to the legacy kbase
  ioctls** — `pan_nondrm.c` uses `KBASE_IOCTL_MEM_ALLOC/MEM_IMPORT/JOB_SUBMIT/GET_GPUPROPS/
  VERSION_CHECK (11.11)/SET_FLAGS/STICKY_RESOURCE_MAP/HWCNT_*` (pan_nondrm.c:38-509). This
  **partially contradicts** the d5 D5b(d) "panfrost <-> kbase is not a thing": it *was* done in
  2019 for Midgard; it is dead code for today's mesa (GPLv2, kept out of mesa per README).
- `mali_kbase` (2019-01-20): Arm kbase `r27p0-01rel0` (`driver/.../midgard/Kbuild:24`) for
  trying panfrost before the DRM driver existed. `mali_kbase_gpu_id.h:110`:
  `GPU_ID2_PRODUCT_TNOX = GPU_ID2_MODEL_MAKE(7, 1)`; `mali_kbase_hw.c:59,191` tNOx features /
  `tNOx_r0p0` issues. Has `mali_midg_regmap.h` (full JM-era register map).
- `linux-panfrost` ("Oolong", 2018-05-29): older out-of-tree kbase fork, Midgard/Rockchip.
- `mali-isa-docs` (2020-05-05): RE ISA docs — `Bifrost.adoc`, `Midgard.md`, `Utgard-*.md`.
- `bifrostc` (2020-11-20) / `bifrost.rs` (2020-11-13): experimental Bifrost compilers;
  `bifrostc/ISA.xml` + `bifrost.rs/{Scheduler,Units}.md` describe clause/tuple/FMA+ADD scheduling.
- `ShaderProgramDisassembler` (2019-01-21): Midgard/Bifrost disassembler (C, meson).
- `pandecode-standalone` (2021-01-18, 2 commits): pandecode lifted out of mesa
  (`decode.c`, `panfrost-job.h`, `midgard.xml`, `bifrost/`) — a self-contained job-descriptor
  decoder; small enough to port to a Windows tool.
- `panloader` (2023-08-11, 1113 commits): `panwrap` LD_PRELOAD tracer of the **blob's kbase
  ioctls** + `include/mali-ioctl.h` (kbase UAPI as RE'd).
- `csf.rs` (2023-04-10): CSF firmware notes (`mali_csffw.bin` header magic `0xC3F13A6E`,
  Cortex-M7+) + `regmap.txt` — **Valhall-CSF only, not applicable to G76 (job manager)**.
- `valhall-docs` (2022-01-27): Valhall (G78) ISA — not our arch, useful for contrast only.
- `hwc-helper` / `panfrost-hwc-helper` (2022): perf-counter XML offset tools.
- `midgard-performance-analyze` (2019): Midgard counters — not our arch.
- `devboard-images` (2026-07-29): debos images for Khadas VIM3 (Amlogic, **Mali-G52 = Bifrost**)
  and Rock2 Square — test-board recipes, nothing to port.
- `panfrost` (2018-09-03, DEPRECATED): the original out-of-mesa Midgard driver (`pan_slowfb.c`,
  `pan_nondrm.c`).
- `panfrost.freedesktop.org` (2021-07-30): website shell, `public/` only.
- `shader-db` (fork, 2019-10-30): shader corpus for compiler stats.
- `future-linux-panfrost`: empty repository.

### drm/panfrost (linux fork, branch `drm-misc-next` 2026-04-08) — kernel side, 7 733 lines total

Paths relative to `C:\Users\ilya\code\mali\linux\drivers\gpu\drm\panfrost\`.
- **GPU identity.** `panfrost_gpu.c:226` `GPU_MODEL(g76, 0x7001)`; `panfrost_gpu.c:312-314`
  `features.id = GPU_ID >> 16`, revision = low 16 bits; `panfrost_device.h:231-237` model compare
  masks ids with `0xf00f` when `id & 0xf000` (so any `0x7?01`-style product id, e.g. `0x7211`,
  matches `0x7001`). kbase cross-check: `mali_kbase/.../mali_kbase_gpu_id.h:110`
  `GPU_ID2_PRODUCT_TNOX = GPU_ID2_MODEL_MAKE(7, 1)` (arch_major 7, product_major 1). So the
  **G76 internal codename is tNOx** ("Gondul" from the task prompt: not found in any clone yet —
  checked below). **Expected 9820 GPU_ID>>16 = 0x7?01 pattern — UNVERIFIED** until read on device.
- **G76 features** `panfrost_features.h:85-97`: JOBCHAIN_DISAMBIGUATION, PWRON_DURING_PWROFF_TRANS,
  XAFFINITY, THREAD_GROUP_SPLIT, FLUSH_REDUCTION, PROTECTED_MODE, PROTECTED_DEBUG_MODE,
  COHERENCY_REG, **AARCH64_MMU**, TLS_HASHING, IDVS_GROUP_SIZE, 3BIT_EXT_RW_L2_MMU_CONFIG.
  **G76 issues** `panfrost_issues.h:259`: `hw_issues_g76 0` (no per-model errata in panfrost —
  but kbase has `base_hw_issues_tNOx_r0p0` incl. `TNOX_1194`, `mali_base_hwconfig_issues.h:1160-1183`).
- **Compatibles** `panfrost_drv.c:1155-1178`: generic `arm,mali-bifrost` (default_data) + Amlogic /
  MediaTek / Renesas / Allwinner SoC entries; **no Exynos or any known G76 SoC entry**. Samsung's
  DT uses `arm,mali` (kbase binding), not `arm,mali-bifrost`.
- **Register map** `panfrost_regs.h` (derived from Arm `mali_midg_regmap.h`): GPU_ID 0x000,
  L2/CORE/TILER/MEM/MMU_FEATURES 0x004-0x014, AS_PRESENT 0x018, JS_PRESENT 0x01C,
  GPU_INT_{RAWSTAT,CLEAR,MASK,STAT} 0x20-0x2C, GPU_CMD 0x30 (SOFT_RESET 1, HARD_RESET 2,
  CLEAN_CACHES 7, CLEAN_INV_CACHES 8), GPU_STATUS 0x34, GPU_PWR_KEY 0x50 (unlock 0x2968A819),
  SHADER/TILER/L2_PRESENT 0x100/0x110/0x120, *_READY 0x140/0x150/0x160, *_PWRON
  0x180/0x190/0x1A0, *_PWROFF 0x1C0/0x1D0/0x1E0, *_PWRTRANS 0x200/0x210/0x220,
  COHERENCY_FEATURES/ENABLE 0x300/0x304, STACK_* 0xE00-0xE44, JM/SHADER/TILER/L2_MMU_CONFIG
  0xF00-0xF0C. Job control: JOB_INT_* 0x1000-0x1014; slots at `JS_BASE 0x1800 + n*0x80`
  (HEAD/TAIL/AFFINITY/CONFIG/XAFFINITY/COMMAND/STATUS + *_NEXT at +0x40..+0x70). MMU:
  MMU_INT_* 0x2000-0x200C; AS n at `0x2400 + n*0x40` (TRANSTAB 0x00, MEMATTR 0x08, LOCKADDR 0x10,
  COMMAND 0x18, FAULTSTATUS 0x1C, FAULTADDRESS 0x20, STATUS 0x28, TRANSCFG 0x30, FAULTEXTRA 0x38).
  All of it fits in the DT `reg = <0x18500000 0x5000>` window (highest used offset 0x2400+15*0x40
  < 0x5000).
- **Reset** `panfrost_gpu.c:62-104`: INT_MASK=0, clear RESET_COMPLETED, GPU_CMD=SOFT_RESET, poll
  `GPU_INT_RAWSTAT & BIT(8)` (10 µs step, 10 ms), fall back to HARD_RESET; then clear all, unmask
  FAULT|MULTIPLE_FAULT|PERFCNT|CLEAN_CACHES.
- **Power-on** `panfrost_gpu.c:452-481` (after the platform has the power domain + clocks up):
  quirks (`panfrost_gpu_init_quirks`, :117-176 — SHADER_CONFIG: TLS_HASH_ENABLE for G76;
  JM_CONFIG: IDVS group size 0xF<<16 for G76), then `L2_PWRON_LO = l2_present & core_mask` → poll
  L2_READY; `SHADER_PWRON_LO = shader_present & core_mask` → poll SHADER_READY (20 ms);
  `TILER_PWRON_LO = tiler_present` → poll TILER_READY (1 ms). Power-off is the reverse with
  *_PWRTRANS polling (:483-506). `panfrost_get_core_mask` (:428-450): only the **first core
  group** is used if `l2_present != 1`.
- **MMU** `panfrost_mmu.c`: page-table format is `ARM_MALI_LPAE` by default, `ARM_64_LPAE_S1`
  (standard AArch64 4K stage-1 = Linux `io-pgtable-arm.c`) only with `GPU_QUIRK_FORCE_AARCH64_PGTABLE`
  (:786-795) — G76 *has* AARCH64_MMU so either works. AArch64 path (:221-243): TRANSTAB = ttbr,
  MEMATTR = MAIR translated by `mair_to_memattr` (:30-68), TRANSCFG = PTW_MEMATTR_WB | PTW_RA |
  ADRMODE_AARCH64_4K (6) | INA_BITS(55-ias) [| PTW_SH_OS if coherent]. Enable (:177-189):
  FLUSH_MEM, write TRANSTAB/MEMATTR/TRANSCFG, `AS_COMMAND_UPDATE`. Lock region (:103-137):
  naturally aligned pow2 region, `width = log2-1`, min 32 KiB. Map/unmap uses 4K or 2M blocks
  (:362-380) then `AS_COMMAND_FLUSH_PT`. Faults: grow-on-GPU-fault heap in 2 MiB chunks (:585-613).
- **Job submit** `panfrost_job.c:202-270`: JS0 = fragment, JS1 = vertex/tiler, JS2 = compute
  (:111-118, compute slot "not exposed to userspace yet"). Write JS_HEAD_NEXT = GPU VA of the
  first job descriptor, JS_AFFINITY_NEXT = shader_present (:133-147), JS_CONFIG_NEXT = AS number |
  THREAD_PRI(8) | START/END_FLUSH_CLEAN_INVALIDATE | JOB_CHAIN_FLAG(seqno&1) | ENABLE_FLUSH_REDUCTION,
  JS_FLUSH_ID_NEXT, then `JS_COMMAND_NEXT = START`. Completion via JOB_INT (done bit j, error
  bit j+16).

### Identity correction — G76 is **"Norr"/tNOx**, not "Gondul"

- `valhall-docs/Appendix.md:11-17` codename table: G72 Heimdall (6.1), G51 Sigurd (7.0),
  **G76 Norr (major 7, minor 1)**, **G52 Gondul (7.2)**, G31 Dvalin (7.3), G77 Trym (9.0).
  The "Minor" column there is the *product major*; consistent with kbase `TNOX = MODEL_MAKE(7,1)`.
- `mesa-upstream/src/panfrost/model/pan_model.c:83`
  `BIFROST_MODEL(PAN_PROD_ID(7, 2, 1), "G76", ...)` — arch_major 7, **arch_minor 2**,
  product_major 1; `MODEL_TB_SIZES(16384, 8192)`, `MODEL_RATES(2, 2, 24)`.
  `model/pan_model.h:25-41`: GPU_ID bits [31:28] arch_major, [27:24] arch_minor, [23:20] arch_rev,
  [19:16] product_major, [15:12] ver_major, [11:4] ver_minor, [3:0] status.
  ⇒ **expected 9820 `GPU_ID[31:16]` = `0x72?1`** (arch_rev nibble unknown) — UNVERIFIED, one MMIO
  read closes it. Compiler cmdline table: `compiler/bifrost/cmdline.c:79` `{"G76", 7, 1}`.
- `grep -ri gondul` over every clone hits only `valhall-docs/Appendix.md`.

### Mesa userspace (upstream shallow clone 2026-10-12 `1b75efe`) — Windows-relevant structure

- **OS seam = `src/panfrost/lib/kmod/`** (5 092 lines): `pan_kmod.h:453-550` `struct pan_kmod_ops`
  — dev_create(fd)/destroy, query_user_va_range, bo_alloc/free/import/export/mmap/wait,
  make_(un)evictable, vm_create/destroy/**vm_bind**/vm_query_state, query_timestamp, bo_set_label,
  perf_*. Backends picked by DRM driver name (`pan_kmod.c:18-37`): `panfrost` (JM, 1 609 lines),
  `panthor` (CSF), `panfrost_virtio_gpu` (virtio native context, `kmod/virtio/panfrost_proto.h`).
  A Windows/D3DKMT backend would be a fourth entry here.
- **But job submission bypasses kmod**: `DRM_IOCTL_PANFROST_SUBMIT` is called from
  `lib/kmod/panfrost_kmod.c:350`, `vulkan/jm/panvk_vX_gpu_queue.c:81,125`, and the gallium driver
  uses `DRM_IOCTL_PANFROST_JM_CTX_CREATE/DESTROY` + `drmSyncobjWait` directly
  (`gallium/drivers/panfrost/pan_jm.c:81-188,1125-1161`). Porting = kmod backend **plus** the
  submit/syncobj call sites.
- **No D3D UMD in Mesa today**: `src/gallium/frontends/` = dri hgl lavapipe mediafoundation
  rusticl teflon torx va **wgl** — no `d3d10umd` (only a stale `gallium-d3d10-dll-name` option,
  `meson.options:157-161`). The 2021 group fork has none either (clover dri glx hgl lavapipe nine
  omx osmesa va vdpau wgl xa xvmc). ⇒ the M16 row's "Mesa's Windows D3D UMD frontend" premise is
  **false for current Mesa**; the Windows-facing frontend that exists is **WGL (OpenGL ICD)**.
  DWM needs D3D (d5 note D5b(d)), so a Mesa-based path gives GL/VK/CL apps, not DWM, unless a D3D
  layer is added (e.g. a D3D11-on-Vulkan translation over PanVK — not researched here).
- Winsys: `src/gallium/winsys/panfrost/drm/panfrost_drm_winsys.c` (DRM fd only).

### Job descriptors / pandecode / compiler (mesa-upstream)

- Bifrost descriptor XML: `src/panfrost/genxml/v6.xml` (1 368 lines, G71/G72) and **`v7.xml`**
  (1 450 lines, G31/G51/G52/**G76**). `v7.xml:37-49` Job Type enum: 1 Null, **2 Write value**,
  3 Cache flush, 4 Compute, 5 Vertex, 7 Tiler, 9 Fragment, 10 Indexed Vertex. `v7.xml:1334-1350`
  **Job Header** (align 64): w0 exception status, w1 first incomplete task, w2-3 fault pointer,
  w4 bit0 Is64b (default true) / bits1-7 Type / bit8 Barrier / bit9 Invalidate Cache /
  bits16-31 Index, w5 Dependency 1/2 (16 b each), w6-7 Next (GPU VA, 0 ends chain).
  `v7.xml:1368-1395` **Write Value Job** = header + payload at +32: Address (64 b), Type
  (3 Zero, 6 Immediate 32, 7 Immediate 64, 2 System Timestamp, 1 Cycle Counter), Immediate Value.
  ⇒ the **smallest real job**: no shader, no tiler, no framebuffer — writes a value to a GPU VA.
  Mesa itself emits one (`lib/pan_jc.h:231-241`, ZERO type, to clear tiler state on v4/v5);
  job index assigned `++job_index` starting at 1 (`pan_jc.h:158-165`).
  Also `Cache Flush Job` (`v7.xml:1384-1398`) and `Fragment Job` (bounds + framebuffer ptr,
  `:1352-1366`) — fragment = the "clear" job once a framebuffer descriptor is built.
- pandecode: `genxml/decode.c` (785) + `decode_jm.c` (687; write-value case :408-409, :614) —
  dumps a job chain from GPU memory; also standalone copy in group repo `pandecode-standalone/`.
- Bifrost compiler: `src/panfrost/compiler/bifrost/` (+ shared NIR passes in `compiler/`; whole
  `compiler/` tree ≈125 k lines incl. midgard + kraid) — `bifrost_compile.c`, `bi_ra.c`,
  `bifrost/ISA.xml`, `bifrost/bi_pack.c`, `bifrost/disassemble.c`; standalone driver
  `compiler/bifrost/cmdline.c`. `bi_quirks.h:29-41`: quirks only for G71/G72/G31 — **G76 (arch
  7.2) gets none**. `compiler/bifrost/README.md`: register conventions (blend R0-R15/R48,
  fragment preload R0-R7/R57-R63). OpenCL C → Bifrost precompiled kernels: `src/panfrost/clc/`
  + `libpan/*.cl` (copy, indirect dispatch, query pool).
- Support status `docs/drivers/panfrost.rst:46-47`: G31/G51/G52/G76 = Bifrost (v7); conformance
  footnote only for **G52** (GLES 3.1, :89). **Mesa CI has no G76 job** (`src/panfrost/ci/`: g52,
  g57, g610, g72, g720, g925, t7xx/t8xx only). G52 is the closest tested part — **same arch
  7.2** (`pan_model.c:84` `PAN_PROD_ID(7,2,2)`), so G52 results are the best proxy for G76.
- `src/panfrost/drm-shim/panfrost_noop.c`: userspace fake of the panfrost DRM ioctls
  (`DRM_PANFROST_SUBMIT` → noop, :151) — lets the whole Mesa stack run/compile shaders with no
  GPU; candidate for exercising the compiler on Windows before a KMD exists.
- Slot choice for a write-value job: Mesa adds it to the vertex/tiler chain
  (`gallium/drivers/panfrost/pan_jm.c:1094`) which is submitted **without** `PANFROST_JD_REQ_FS`
  (`pan_jm.c:202` vs fragment `:210`) → kernel puts it on **JS1** (`panfrost_job.c:111-118`).

### Firmware, bindings, non-Linux templates

- **Bifrost job-manager GPUs need no GPU firmware**: `grep -rn firmware
  linux/drivers/gpu/drm/panfrost/*.c` → no hits; `request_firmware` exists only in
  `panthor/panthor_fw.c` (CSF, Valhall G610+). `csf.rs/README.md`: the CSF MCU firmware
  (`mali_csffw.bin`) "Replaces job manager". ⇒ answers D5b(b): the G76 has **no firmware
  blob to locate**; the d5 guess of a "PPU firmware" (`notes/d5-gpu-display.md` D5b(b)) is
  contradicted by every open driver for this generation (still UNVERIFIED that Samsung's kbase
  r10p0 does not load one — closes with `grep request_firmware` in that tree).
- DT binding `Documentation/devicetree/bindings/gpu/arm,mali-bifrost.yaml:18-37`: SoCs listed =
  Allwinner H616, Amlogic G12A, MediaTek MT8183/8186/8365, Realtek RTD1619, Renesas RZ/*,
  Rockchip PX30/RK3562/RK3568/RK3576 — all G31/G52/G57-class. **No G76 SoC is in mainline DT**;
  interrupts `job, mmu, gpu[, event]` (:55-68), clocks 1-3, power-domains 1-5. The
  `GPU_MODEL(g76, 0x7001)` entry exists but **no upstream board exercises it**.
  `arch/arm64/boot/dts/exynos/`: only `exynos9810*` (no 9820, no GPU node in 9810 either).
- **No non-Linux port of panfrost exists in any clone** (`grep -ril "freebsd|bare.metal|windows"`
  over the panfrost dirs: only false positives; `_WIN32` appears only in `panvk_instance.c:130`
  platform ifdefs). Closest templates for a Windows driver, in order:
  1. `drm/panfrost` itself (7.7 k lines, GPL-2.0) — the hardware sequences (reset, power,
     MMU, job slots, IRQ decode) are small and self-contained; the rest is DRM/GEM/scheduler glue.
  2. `tyr` (Rust, 652 lines, `linux/drivers/gpu/drm/tyr/{gpu,regs}.rs`) — minimal bring-up
     (`gpu.rs:211` `l2_power_on`), CSF-targeted, shows the *minimum* needed to probe a Mali.
  3. `nondrm/pan_nondrm.c` (516 lines) — proof that panfrost userspace has run over a
     non-DRM kernel interface (kbase ioctls), i.e. the shape of a "Windows kmod" shim.
  4. `mali_kbase` / `linux-panfrost` (Arm kbase r27p0 / older) — `mali_kbase_mmu_mode_aarch64.c`,
     `backend/gpu/mali_kbase_jm_hw.c`, `mali_kbase_pm_driver.c`: Arm's own JM sequences, with
     the tNOx errata list.

## What we can reuse for Windows

1. **Register map + bring-up sequences**: `panfrost_regs.h` + `panfrost_gpu.c` reset/quirks/
   power-on + `panfrost_mmu.c` AS programming → transcribe (clean-room-ish, GPL-2.0 — see risk 5)
   into a UEFI probe driver first, later into the DXGK KMD.
2. **Page tables**: G76 has `HW_FEATURE_AARCH64_MMU` → standard AArch64 4K stage-1 descriptors
   (same format as the CPU's), TRANSCFG ADRMODE_AARCH64_4K. No Mali-specific LPAE needed.
3. **Job descriptors**: `genxml/v7.xml` is the authoritative layout for every Bifrost job type;
   Write-Value → Fragment(clear) → Compute is a natural test ladder needing 0 / 0 / 1 shader.
4. **pandecode** (`genxml/decode*.c`, or `pandecode-standalone`) to dump/verify hand-built chains
   on the PC before they touch the GPU.
5. **Compiler**: Mesa's Bifrost backend (NIR → Bifrost binary, `compiler/bifrost/cmdline.c`) can
   produce shader binaries offline on the PC; `drm-shim` runs the whole stack without a GPU.
6. **UMD seam**: `pan_kmod_ops` (+ the few direct `DRM_IOCTL_PANFROST_*`/syncobj call sites) is
   where a Windows D3DKMT backend plugs in; Mesa's existing Windows frontend is **WGL** (GL), and
   PanVK gives Vulkan.

## Risks

1. **G76 is untested upstream**: no mainline SoC, no Mesa CI job; G52 (same arch 7.2) is the
   proxy. Unknown G76-specific breakage is likely.
2. **G3D power/clock is Samsung-proprietary**: panfrost assumes genpd/clk/regulator from the
   platform; on 9820 the domain is `pd_embedded_g3d` and DVFS is ACPM/cal-if (not in any clone).
   Touching GPU MMIO with the domain off = bus hang/SError. **No GPU register may be read until
   the domain is proven on** (pmu status register, UNVERIFIED offset).
3. **Coherency/cacheability**: panfrost picks MEMATTR from MAIR and `coherent_walk`; whether the
   9820 GPU is IO-coherent (COHERENCY_FEATURES/ENABLE) is UNVERIFIED — wrong choice = stale data,
   not a crash, so validate with read-back.
4. **No D3D path in Mesa**: DWM acceleration (the user-visible goal) needs a D3D UMD; Mesa only
   offers GL (WGL)/VK/CL. A Mesa-based KMD+UMD gives app-level GL/VK, not DWM.
5. **Licensing**: drm/panfrost and kbase are GPL-2.0; Mesa's panfrost is MIT. A Windows KMD
   derived from drm/panfrost source is a GPL question for the owner (same flag as d5 addendum).
6. **Core groups**: panfrost uses only the first core group if `L2_PRESENT != 1`
   (`panfrost_gpu.c:428-450`); if the 9820 has >1 L2 slice the KMD must do the same or do better.

## Clone index (full table with URLs/sizes: `C:\Users\ilya\code\mali\README.md`, not in this repo)

22 group projects + 1 extra, all cloned, 0 clone failures. Shallow (`--depth 1`): `linux`
(drm-misc-next, sparse), `mesa`, `vk-gl-cts`, `mesa-upstream` (extra, upstream Mesa). Full
history: the other 19 (one of them, `future-linux-panfrost`, is empty). Largest: vk-gl-cts 1.7 G,
mesa-upstream 549 M, linux 522 M, mesa 172 M (du incl. `.git`).

- 2026-10-12 — README written; research complete; next: commit notes, then MILESTONES rows.
