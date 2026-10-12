# GPU facts from the stock Odin ROM (G970FXXSGHWC1, Android 12)

Task: mine the stock Samsung firmware package for Mali-G76 / G3D facts for M16 (G1-G12).
Source: `C:\Users\ilya\code\Dumps\` (read-only originals). Extracted to `C:\Users\ilya\code\Dumps\extracted\`
(outside the repo). No device access, nothing flashed, no extracted binary executed.

## Progress log

- P1. `AP_G970FXXSGHWC1_CL25257816_QB62768393_REV01_user_low_ship_meta_OS12.tar` contents (`tar -tvf`):
  boot.img.lz4 (22.6 MB), recovery.img.lz4, dt.img.lz4 (108 KB), dtbo.img.lz4 (415 KB),
  system.img.lz4 (3.8 GB), vendor.img.lz4 (639 MB), vbmeta, userdata, dqmdbg, meta-data/fota.zip.
  **No super.img** (S10e launched on Android 9, no dynamic partitions) -> system/vendor are standalone
  sparse images, no lpunpack needed. No vendor_boot. BL tar: sboot.bin, param, up_param, cm, keystorage, uh, vbmeta.
- P2. Tooling: venv `scratchpad/venv` (pip `lz4` 4.4.5); streamed `boot.img.lz4 dt.img.lz4 dtbo.img.lz4
  vendor.img.lz4 system.img.lz4` out of the AP tar (script `scratchpad/scripts/untar_lz4.py`) into
  `Dumps/extracted/`: boot.img 57,671,680 B; dt.img 8,388,608; dtbo.img 8,388,608; vendor.img 985,121,096
  (sparse); system.img 5,711,914,152 (sparse).
- P3. boot.img (`tools/dump-tools/sbootimg.py` `BootImage.parse`): plain `ANDROID!` v0 at offset 0, name
  `SRPRI28A016KU`, kernel 44,380,172 B **uncompressed** Image, ramdisk size 0, dt_size word = 1 (Samsung quirk,
  as `notes/cpu-driver-9820.md` already found). The DTB lives in `dt.img`.
- P4. `dt.img` and `dtbo.img` are standard Android DT tables (magic `d7b7ab1e` at offset 0, big-endian;
  dt.img 1 entry, dtbo.img 7 entries). Decompiled with `fdt.py` (`split_blobs`+`to_dts`, script
  `scratchpad/scripts/dts_all.py`) to `Dumps/extracted/dts/dt.img.0.dts` (base, root compatible
  `samsung,armv8`) and `dtbo.img.{0..6}.dts` (overlay 0 = `samsung,BEYOND0LTE EUR OPEN 17`, 1..6 = rev 18,19,20,22,24,25).
  NOTE: `fdt.py` annotates small integers in `interrupts` as phandles (e.g. `&isp-alert0` = 0x120) - raw
  cells re-read with `scratchpad/scripts/rawprops.py` for every value quoted below.
- P5. Base DT GPU nodes read (findings F1-F4 below). Overlays: the only GPU hit in `dtbo.img.0.dts` is a
  `gpu { gpu,label = "GPU fault"; threshold_count 0x14; threshold_time 0x4b0 }` (dtbo.img.0.dts:2021,
  a Samsung fault-counter, not a mali override) -> **the overlay does not touch `mali@18500000`.**

## Findings

### F1. `mali@18500000` (dt.img.0.dts:4623-4679; raw cells via rawprops.py)

```
compatible = "arm,mali";
reg = <0x0 0x18500000 0x5000>;
interrupts = <0x0 0x120 0x0   0x0 0x11f 0x0   0x0 0x121 0x0>;
interrupt-names = "JOB", "MMU", "GPU";
g3d_cmu_cal_id = <0xb04000a>;          /* ACPM/CAL DVFS id; same id in exynos-dm dvfs_gpu (:8707) and
                                          exynos_perf_ncmemcpy cal-id-g3d (:4683) */
samsung,power-domain = <0x32>;          /* -> /pd-embedded_g3d@15862100 */
g3d_genpd_name = "pd-embedded_g3d";
governor = "interactive"; interactive_info = <260000 94 0>;
gpu_dvfs_table_size = <9 8>;
gpu_pmu_status_reg_offset = <0x2104>;   gpu_pmu_status_local_pwr_mask = <0x1>;
gpu_max_clock = gpu_max_clock_limit = 702000;  gpu_min_clock = 156000;  gpu_dvfs_start_clock = 260000;
gpu_default_voltage = 800000 (0xc3500 uV);     gpu_voltage_offset_margin = 37500 (0x927c);
gpu_temp_throttling = <650000 572000 455000 325000 260000 156000>;
gpu_runtime_pm_delay_time = 50; gpu_dvfs_polling_time = 30; gpu_power_coeff = 625; gpu_bts_support = 1;
gpu_pmqos_mif_max_clock = 2094000 (0x1ff3b0); gpu_boost_egl_min_lock = 1872000 (0x1c9080);
gpu_vk_boost_max_lock = 325000; gpu_vk_boost_mif_min_lock = 1014000; gpu_ess_id_type = 0xe;
```

- IRQs (GIC SPI, 3-cell, flags cell 0): **JOB = SPI 0x120 = 288 (GSIV 320), MMU = SPI 0x11f = 287 (GSIV 319),
  GPU = SPI 0x121 = 289 (GSIV 321).** Order in DT is JOB, MMU, GPU.
- **No `clocks`/`clock-names`, no `*-supply`, no `operating-points-v2`** in the node: clock + voltage are
  set by Samsung's platform layer through CAL id `0xb04000a` (ACPM DVFS, like the CPU domains in
  `notes/cpu-driver-9820.md` F2) and the regulator is not a DT-visible Linux regulator.
- MMIO window 0x5000 (20 KiB) - not the 0x4000 of upstream panfrost bindings; G76 register map is in
  the first 0x4000 (gpu/job/mmu), the extra page is unused by panfrost (UNVERIFIED what Samsung uses it for).

### F2. GPU DVFS table (9 rows x 8 cols, kHz)

Column meaning not labelled in the DT; from the values: clock, min_threshold %, max_threshold %,
down_staycount, MIF min kHz, CPU little-cluster lock, CPU mid lock, CPU big max (column semantics
UNVERIFIED - read the Samsung `gpu_dvfs_table` parser, e.g. LineageOS `gpu_dvfs_handler`/`gpexbe` to close).

| GPU kHz | min% | max% | stay | MIF kHz | col6 | col7 | col8 |
|---|---|---|---|---|---|---|---|
| 702000 | 78 | 100 | 9 | 2093000 | 1456000 | 0 | 1820000 |
| 650000 | 78 | 98 | 5 | 2093000 | 1456000 | 0 | 2080000 |
| 572000 | 78 | 98 | 5 | 1794000 | 0 | 0 | 0 |
| 433000 | 78 | 95 | 1 | 1352000 | 0 | 0 | 0 |
| 377000 | 78 | 90 | 1 | 1352000 | 0 | 0 | 0 |
| 325000 | 78 | 85 | 1 | 1014000 | 0 | 0 | 0 |
| 260000 | 78 | 85 | 1 | 676000 | 0 | 0 | 0 |
| 200000 | 78 | 85 | 1 | 676000 | 0 | 0 | 0 |
| 156000 | 0 | 85 | 1 | 676000 | 0 | 0 | 0 |

No voltages in the DT: per-OPP voltage comes from ECT/ASV through ACPM (`ect { parameter_address = <0x95000000>;
parameter_size = <0x3a000> }`, dt.img.0.dts:4685). `gpu_sustainable_info = <433000 95 949000 1690000 520000>`.

### F3. G3D power domain (dt.img.0.dts:3517-3525, :4331-4338, :5290-5302)

```
pd-embedded_g3d@15862100 { compatible = "samsung,exynos-pd"; reg = <0x0 0x15862100 0x20>;
                           cal_id = <0xb1380007>; need_smc = <0x18410204>; status = "okay"; };
pmucal dbg: pd-embedded-g3d { id = <0x7>; emul_offset = <0x210c>; latency_offset = <0x108>; }
system-controller@15860000 { compatible = "samsung,exynos9820-pmu", "syscon"; reg = 0x15860000 + 0x10000 }
```

- PMU G3D block = PMU_BASE (0x15860000, already VERIFIED in Exynos9820.h:280) + 0x2100:
  **G3D_CONFIGURATION = 0x15862100, G3D_STATUS = 0x15862104** (the mali node's own
  `gpu_pmu_status_reg_offset = 0x2104` with `local_pwr_mask = 0x1` names the same register: status bit 0
  = powered). G3D_OPTION/EMUL at +0x210c. Value to write to CONFIGURATION for on/off is not in the DT
  (generic exynos-pd: 0xF on / 0x0 off - UNVERIFIED for 9820, cal-if pmucal sequence `cal_id 0xb1380007`).
- `need_smc = 0x18410204`: 14 of the 15 `need_smc` values (16 pd nodes; pd-fsys0a has none) end in `0x...0204` (g2d 0x18a10204, mfc 0x18820204; pd-aud is the exception, 0x18c62000; that it is a block SYSREG/TZPC register is UNVERIFIED;
  dpu 0x191b0204 ...). In Samsung's exynos-pd.c (from memory, UNVERIFIED - not re-read) this address is handed to EL3 by SMC after power-on to
  restore secure (TZPC) state of the block -> **G3D likely needs an SMC after power-up** before non-secure
  access (UNVERIFIED: the SMC id + whether the GPU regs fault without it; read exynos-pd.c `need_smc`).
- GPU DVFS domain in exynos-dm: `dvfs_gpu { dm-index 0xd; available = "false"; cal_id 0xb04000a; dm_type_name "dm_gpu" }`
  (dt.img.0.dts:8707-8712) - gpu DVFS is *not* routed through exynos-dm, Samsung's mali platform calls CAL directly.

### F4. G3D thermal (dt.img.0.dts:9244-9264, :9550-9625)

TMU `G3D@100A0000` (`samsung,exynos9810-tmu`, reg 0x100a0000+0x800, id 3, SPI 0x1df=479); thermal-zone
`G3D_THERMAL` trips gpu-cold/switch-on/control-temp/alert0-3/hot, cooling-device = mali node.
ION carve-out `ion_gpu_buffer` (secure, protection_id 9) for protected content (dt.img.0.dts:2527).

### F5. Shipped kernel: version, kbase, IKCONFIG (boot.img kernel)

- `Dumps/extracted/boot/kernel.img` @0x109000f: `Linux version 4.14.113-25257816 (dpi@21DKG923) (... clang 6.0.1 ...)
  #1 SMP PREEMPT Thu Mar 2 09:32:04 KST 2023`.
- **Kernel Mali driver = `drivers/gpu/arm/bv_r32p1/`** (kernel.img @0x17e8c11 `drivers/gpu/arm/bv_r32p1/mali_kbase_ccswe.c`,
  and ~dozens more `bv_r32p1/mali_kbase_*.c` paths) - Samsung's late (Android 12) update moved the 9820 from
  `b_r10p0` (LineageOS/older stock) to the **r32p1** Bifrost/Valhall kbase.
- IKCONFIG embedded (`IKCFG_ST` @0x109d4f0, `IKCFG_ED` @0x10a6d1a; gzip payload 17421560..17460506) ->
  `Dumps/extracted/boot/kernel.config`. GPU block (kernel.config:3368-3420):
  `CONFIG_MALI_DDK_VERSION=y`, choice `CONFIG_MALI_BIFROST_R32P1=y` (R10P0/R12P0/R14P0/R16P0/R19P0_Q/R26P0 all
  "not set" -> the Samsung tree carries 7 kbase versions), `CONFIG_MALI_MIDGARD=y`, `CONFIG_MALI_EXPERT=y`,
  `CONFIG_MALI_REAL_HW=y`, `CONFIG_MALI_DEVFREQ` not set, `CONFIG_MALI_DVFS=y`, `CONFIG_MALI_PLATFORM_NAME="exynos"`,
  `CONFIG_EXYNOS_SOC_NAME="9820"`, `CONFIG_MALI_EXYNOS_{CLOCK,DVFS,PM,RTPM,QOS,THERMAL,BTS_MO,DEBUG,DEVICETREE,CL_BOOST}=y`,
  `CONFIG_MALI_EXYNOS_IFPO` not set, `CONFIG_MALI_EXYNOS_LLC` not set, both `..._SECURE_RENDERING_{LEGACY,ARM}` not set,
  `CONFIG_MALI_EXYNOS_SECURE_SMC_NOTIFY_GPU` not set, `CONFIG_MALI_ARBITER_SUPPORT` not set,
  `CONFIG_MALI_DMA_BUF_LEGACY_COMPAT=y`. `CONFIG_SOC_EXYNOS9820=y` (:434).
- **kbase version string: `K:r32p1-01bet2(GPL)`** (kernel.img @0x17ea842 - the `MALI_RELEASE_NAME` the driver
  reports in `KBASE_IOCTL_VERSION`/`gpuinfo`). Probe log format `GPU identified as 0x%x arch %d.%d.%d r%dp%d status %d`
  (@0x17e9cd6). Product table includes `Mali-G76` (@0x17eaf46). `tGOx` (G76 codename) @0x166ac20/0x166ac8c.
- Samsung platform layer = **`gpex` (frontend) / `gpexbe` (backend)**, source path
  `drivers/gpu/arm/bv_r32p1/../exynos/frontend/gpu_dvfs_governor.c` (@0x17ef3c8). Region 0x17ee870..0x17f00eb
  holds all `[G3D]` strings (dump in `scratchpad/g3dstr.txt`). Load-bearing ones:
  - DT parser keys (@0x17ee881..0x17eeba0): `g3d_cmu_cal_id g3d_genpd_name gpu_max_clock gpu_min_clock
    gpu_pmu_status_reg_offset gpu_pmu_status_local_pwr_mask gpu_dvfs_table_size gpu_dvfs_table gpu_cl_pmqos_table ...`.
  - **DVFS table column labels** (@0x17eeb12): `[G3D] up [%d] down [%d] staycnt [%d] mif [%d] lit [%d] mid [%d] big [%d]`
    -> the 8 columns of F2 are clock, then up-threshold, down-threshold, staycount, MIF-min, little-CPU, mid-CPU,
    big-CPU (label order = printf order; that the printf walks the columns in order is UNVERIFIED but
    consistent with the values: 78/100 = thresholds, 9/5/1 = staycount, 2093000.. = MIF kHz).
  - Clock/voltage via CAL: `set_clock_using_calapi` (@0x17ef070), `Failed to get G3D ASV table from CAL IF`
    (@0x17eef66), `can't set clock and voltage in the power-off state!` -> **GPU clock+voltage are one
    CAL-DFS call (`cal_dfs_set_rate(0xb04000a, kHz)` in Samsung trees) which goes through ACPM**; no CMU writes
    from the GPU driver itself.
  - Power: `gpexbe_pm_pd_control` (@0x17efbf4), `g3d pm_domain is null` / `gpu_get_pm_domain` -> power is
    the genpd `pd-embedded_g3d` (exynos-pd -> cal-if pmucal), runtime PM with autosuspend
    (`power autosuspend prepare`, `gpu_runtime_pm_delay_time` = 50 ms).
  - Secure world: `gpexbe_smc_protection_enable/disable` "Enter/Exit Secure World by GPU" (@0x17ef8c0..)
    - only for protected (DRM) content mode, not needed for normal rendering.
- cal-if SFR name tables for the G3D block (@0x17b3523..0x17b36a8, used by cmucal/pmucal debug):
  `PLL_CON0_PLL_G3D PLL_CON2_PLL_G3D EMBEDDED_G3D_OUT EMBEDDED_G3D_CONFIGURATION EMBEDDED_G3D_STATUS
  CLK_CON_DIV_DIV_CLK_G3D_BUSP/BUSD CLK_CON_MUX_MUX_CLK_G3D_BUSD PLL_CON0_MUX_CLKCMU_EMBEDDED_G3D_USER
  PLL_CON0_MUX_CLKCMU_G3D_BUS_USER PLL_CON0_MUX_CLKCMU_G3D_SWITCH_USER G3D_SHORTSTOP G3D_CLKDIV2PH
  G3D_EMBEDDED_CMU_G3D_CONTROLLER_OPTION G3D_CMU_G3D_CONTROLLER_OPTION SYSREG_G3D_BUS_COMPONENT_DRCG_EN`,
  `blkpwr_embedded_g3d` (@0x17b57de), `EXT_REGULATOR_G3D_DURATION` (@0x17b5c83), QCH list incl. 4 ASB/QE/PPMU
  G3D0-3 ports (= 4 ACE ports to the bus, `LHS_ACE_D0..D3_G3D`) and `D_TZPC_G3D_QCH` (G3D has its own TZPC ->
  consistent with the `need_smc` in F3). Two CMUs: `EMBEDDED_CMU_G3D` (the GPU PLL) and `CMU_G3D` (bus).
  Their numeric SFR offsets are in pointer tables, not recovered (see P7).
- `CSFFW` timeline strings exist (@0x1212aeb) only because r32p1 shares tl code with CSF GPUs; the G76 is a
  Job-Manager GPU (`mali_kbase_context_jm.c`, `backend/gpu/mali_kbase_jm_hw.c`, `mmu/backend/mali_kbase_mmu_jm.c`
  are the compiled backends) -> **no GPU firmware is loaded by this kernel**: no `mali_csffw.bin`, and the only
  `request_firmware` users nearby are NPU/abox/SCore/ccic, none in the gpu/arm path.
- P6. Platform-region strings dumped (above). `Windows/dxgk/wddm/panfrost` -> 0 hits in the kernel.
- P7. kallsyms: `CONFIG_KALLSYMS=y`, `CONFIG_KALLSYMS_ALL=y`, `CONFIG_KALLSYMS_BASE_RELATIVE=y` (kernel.config:198-201)
  but a scan for the 256-entry `kallsyms_token_index` (u16, [0]=0, increasing) found **no candidate**
  (`scratchpad/scripts/kallsyms_rel.py`) - matches `tools/dump-tools/kallsyms.py` header (no table in the shipped
  HHXJ6 kernel either). Samsung strips/relocates it (RKP/`CONFIG_UH_RKP=y`). Symbol-level facts therefore come
  from strings + `__ksymtab_strings` only.
- P8. kbase DT-property strings (@0x17ea612..0x17ed5b3): `quirks_sc quirks_tiler quirks_mmu quirks_gpu`,
  `system-coherency` ("Using coherency mode %u set from dtb"), `idvs-group-size`. **None of these
  properties is present in `mali@18500000`** (F1 full property list) -> Samsung runs the G76 with the kbase
  defaults: no DT quirk overrides, no `system-coherency` (kbase r32p1 `kbase_device_coherency_init` then keeps
  `COHERENCY_NONE` - i.e. **non-coherent / ACE-Lite not used**; this default is from ARM's kbase source, not
  re-read here: UNVERIFIED until checked in a bv_r32p1 tree), default IDVS group size.
  `&kbdev->fw_load_lock` (@0x17ecf15) is the generic CSF-shared field, not a JM firmware load.
- P9. vendor: `sparse_img.py` -> `Dumps/extracted/vendor.raw` (25 chunks -> 1,153,433,600 B, ext4). The installed
  7-Zip (Universal Extractor's 9.13 beta) cannot open ext4 -> pip `ext4` in the venv + walker
  `scratchpad/scripts/ext4x.py` (listing `Dumps/extracted/vendor.list.txt`, 1735 entries, 0 read errors;
  GPU-ish files copied to `Dumps/extracted/vendor/`).

### F6. vendor GPU stack (vendor.list.txt)

| path (under /vendor) | size | note |
|---|---|---|
| lib64/egl/libGLES_mali.so | 41,320,968 | the whole DDK: EGL + GLES1/2/3 + **OpenCL** (`lib64/libOpenCL.so{,.1,.1.1}` are symlinks to it) + Vulkan entry |
| lib/egl/libGLES_mali.so | 30,152,440 | 32-bit |
| lib64/hw/vulkan.mali.so | 10,448 | tiny Vulkan HAL shim -> loads libGLES_mali.so |
| lib64/libLLVM_android_mali.so | 19,004,232 | Mali offline/online compiler LLVM (RenderScript `bcc_mali`) |
| lib64/libbcc_mali.so, bin/bcc_mali, lib64/libmalicore.bc, libclcore.bc | | RenderScript-on-Mali |
| lib64/libgpudataproducer.so | 1,533,984 | Android GPU inspector counters producer |
| lib64/mali_symlink.so | 10,408 | |
| lib64/hw/gralloc.exynos9820.so | 78,984 | gralloc |
| etc/permissions/android.hardware.vulkan.{compute,level,version}.xml, opengles.aep.xml, *deqp.level.xml | | feature levels |

**GPU firmware: none.** Every `*.bin` under `/vendor/firmware` is audio/camera/NPU/IVA/MFC/Wi-Fi/NFC/SCore
(listing above, no `mali`/`gpu`/`g3d`/`csf` name). The only GPU-named blobs are **compute kernels for
Samsung's own frameworks**, not GPU firmware: `/vendor/eden/gpu/eden_kernel_{32,64}.bin` (Eden NN runtime
OpenCL kernels), `/vendor/etc/snap_gpu_kernel_64.bin` (19 MB, SNAP), `/vendor/etc/fruc_kernels.exynos9820.bin`.
Consistent with F5 (JM GPU, no `request_firmware` in kbase).

### F7. DDK (user-mode) version - `/vendor/lib64/egl/libGLES_mali.so`

- `U:r32p1-01bet2 ` @0x74c8f (UMD counterpart of the kernel's `K:r32p1-01bet2(GPL)` -> **UMD and KMD are the
  same DDK drop, r32p1-01bet2**).
- `r32p1-01bet2-mbs2v39_0` @0x445328, followed by `131801e953429f661ecce1d5e1d2b3ef` (build hash) - full build id.
- EGL version string `1.4 Bifrost-"r32p1-01bet2"` @0x14474a; `OpenGL ES 3.2` @0x18cc14; device node `/dev/mali` @0x14c5b9.
- Source tree path baked in: `vendor/samsung_slsi/MaliG76_r32p1_MK_S/...` (e.g. @0x2e2e1 `cdbg/src/mali_cdbg_control.c`)
  - a G76-specific build of r32p1 for Android S ("MK_S").
- Product-revision table entries `tGOx.r0p0` @0x4a257 (G76), plus tSIx/tHEx/tNAx/tBAx/tTUx/tVAx -> generic DDK.
- Vulkan: `vulkan.mali.so` (10,448 B) is a stub `mali_vk_vendor_stub` that loads `$ORIGIN/../egl/libGLES_mali.so`;
  `android.hardware.vulkan.version` = 4198400 = 0x401000 = **Vulkan 1.1.0**, `vulkan.level` = 1.
- OpenCL: `libOpenCL.so*` -> `libGLES_mali.so` (single binary); `cl_arm_*` extensions present.
- No `dxgk`/`d3dkmt`/`wddm` strings (the only "Windows" hits are LLVM's COFF/EH text, @0x91458, 0xa76d4).

### F8. vendor init / props (Dumps/extracted/vendor/...)

- `ueventd.rc:1` `/dev/mali0 0666 system system` - the kbase char device.
- `etc/init/init.exynos9820.rc:705-711` "# GPU DVFS": sysfs `/sys/devices/platform/18500000.mali/{cl_boost_disable,dvfs_min_lock,dvfs_max_lock}`
  (confirms the platform-device name `18500000.mali`); `:106,:111` `/dev/gpu_freq_min` (EPIC PM-QoS node).
- `build.prop:105` `ro.opengles.version=196610` (0x30002 = GLES 3.2); `:113-114` `ro.hardware.vulkan=mali`, `ro.hardware.egl=mali`;
  `:115` `graphics.gpu.profiler.support=true`;
  **`:185-186` `ro.gfx.driver.0=com.samsung.gamedriver.ex9820`, `ro.gfx.driver.1=com.samsung.pregpudriver.ex9820`**
  - Android "updatable graphics driver" packages (Samsung "GPU Driver Updates" / game driver): the GLES/Vulkan
  UMD can be shipped as an APK outside /vendor (newer DDK than r32p1 possible on a live phone).
- No `/vendor/etc/*.xml` GPU tunables beyond `etc/permissions` feature XMLs (F6).
- P10. system: `sparse_img.py` -> `Dumps/extracted/system.raw` (97 chunks -> 5,976,883,200 B, ext4); walked with
  ext4x.py -> `Dumps/extracted/system.list.txt` (7003 entries, 0 errors). No GPU-named `*.bin`/firmware anywhere in
  system either (grep of the listing for `.bin`/`firmware` x `gpu|mali|g3d|csf` = 0 hits).

### F9. system partition: updatable driver stubs, Windows-related apps (surprises)

- `/system/priv-app/GameDriver-EX9820/GameDriver-EX9820.apk` (21,252 B, package `com.samsung.gamedriver.ex9820`,
  label "Samsung EX9820 GameDriver", versionName `12.0.00.0`, build `NorrSGameDriver-1200000-12.0.00`) and
  `/system/priv-app/DevGPUDriver-EX9820/DevGPUDriver-EX9820.apk` (21,100 B, `com.samsung.pregpudriver.ex9820`,
  "Samsung EX9820 Dev GPUDriver"). Both are **empty placeholders** (zip contents: manifest, empty
  `assets/sphal_libraries.txt` + `whitelist.txt`, a 1.3 KB classes.dex, **no lib/arm64-v8a/*.so**) - real
  driver payloads come later from the Galaxy Store, so the shipped UMD stays r32p1-01bet2.
- `/system/priv-app/GpuWatchApp` (+ `libgpustat.so`, `gpustat_launcher`) - Samsung GPU overlay/counters.
- `/system/priv-app/LinkToWindowsService/LinkToWindowsService.apk` (Microsoft Phone Link) and
  `/system/priv-app/DeXonPC/DeXonPC.apk` (DeX for PC) - app-level Windows integration only;
  nothing kernel/driver-side (no dxgk/wddm strings in kernel or libGLES_mali.so, P6/F7).
- P11. `Dumps/twrp_device_tree/kernel` (TWRP's prebuilt): `Linux version 4.14.113-g55f042627 (runner@fv-az572-167) ... clang 12.0.7`
  @0xc90027, also `drivers/gpu/arm/bv_r32p1/` (@0x1352269) and `K:r32p1-01bet2(GPL)` (@0x1352692) -> TWRP runs the same
  kbase drop. `BoardConfig.mk`: `TARGET_BOARD_PLATFORM_GPU := mali-g76`.

## Facts that close UNVERIFIED items in M16 G1-G3

Source class: **stock ROM DT/kernel (G970FXXSGHWC1)**, not a live-device read. The phone's currently
installed Android build is not known to be HWC1, so the live `/proc/device-tree` read in G1 stays the
final confirmation - but the values below are what Samsung's own kernel binds to on this SKU.

| Row | Item | Fact | Citation |
|---|---|---|---|
| G1 | mali node name | `mali@18500000`, `compatible = "arm,mali"`, `reg = <0 0x18500000 0x5000>` | dt.img.0.dts:4623-4625 |
| G1 | interrupt-names | `"JOB", "MMU", "GPU"` = SPI 288 / 287 / 289 (GSIV 320 / 319 / 321), flags cell 0 | dt.img.0.dts:4626-4627 (raw cells) |
| G1 | `samsung,power-domain` target | phandle 0x32 -> `/pd-embedded_g3d@15862100` | dt.img.0.dts:4629, :3517-3524 |
| G1 | pd node name | `pd-embedded_g3d@15862100` (`samsung,exynos-pd`, cal_id 0xb1380007, need_smc 0x18410204) | dt.img.0.dts:3517-3522 |
| G1 | "no G76 firmware" | **closed for the stock kernel**: kbase `bv_r32p1` JM backend only, no firmware name in the gpu/arm string region, no GPU blob in vendor/system | F5, F6, P10 |
| G1 | kbase version (CORRECTION) | stock Android 12 kernel = **`bv_r32p1`, `K:r32p1-01bet2(GPL)`**, not `b_r10p0` (that is the LineageOS/older tree) | kernel.img @0x17ea842, kernel.config:3377 |
| G2 | PMU G3D offsets | CONFIGURATION = PMU+0x2100 (0x15862100), STATUS = PMU+0x2104 (0x15862104), powered = bit 0 (`gpu_pmu_status_local_pwr_mask = 1`) | dt.img.0.dts:3519, :4638-4639 |
| G2 | clock/voltage path | **no `clocks`/regulator in DT; GPU clock+voltage set via CAL-DFS id `0xb04000a` (ACPM)**, `set_clock_using_calapi`, ASV table from CAL IF | dt.img.0.dts:4628; kernel.img @0x17ef070, @0x17eef66 |
| G2 | ACPM vs direct PMU | power-on = genpd `exynos-pd` (pmucal sequence, cal_id 0xb1380007) + `need_smc` 0x18410204 (target register UNVERIFIED; a `D_TZPC_G3D` QCH exists, kernel.img @0x17b71f3) -> *direct PMU write possible but a secure SMC follows*; DVFS = ACPM | dt.img.0.dts:3517-3522; kernel.img @0x17b71f3 |
| G2 | OPP table | 702/650/572/433/377/325/260/200/156 MHz; boot/start 260 MHz; default voltage 800 mV | F2 |
| G3 | MP count hint | none in DT (no core mask property); G3 SHADER_PRESENT read remains the closer | F1 |
| G5 | coherency | DT has **no `system-coherency`** -> kbase default (non-coherent, UNVERIFIED in kbase source) | F1, P8 |

Still UNVERIFIED (not in the ROM data): the CONFIGURATION value that powers G3D on (generic exynos-pd uses 0xF),
the pmucal sequence contents (pointer tables in the kernel, no kallsyms), the `need_smc` SMC id, CMU_G3D /
EMBEDDED_CMU_G3D base addresses (not DT nodes), whether sboot leaves G3D powered.

## Review of e7913cb (adversarial, AGENTS.md §4)

Checked mechanically: every hex->decimal conversion re-computed with `python -c "print(0x..)"`; every
`dt.img.0.dts:<line>` citation re-read with `sed -n`; `Exynos9820.h:280` PMU base re-grepped.
Found and fixed in the follow-up commit:
- `0x69b68` is **433000**, not 432000 (DVFS row 4, `gpu_sustainable_info[0]`, the G2 OPP list) - a 433 MHz OPP.
- `gpu_pmqos_mif_max_clock = 0x1ff3b0` is **2094000**, not 2093000 (the table's MIF column 0x1fefc8 *is* 2093000).
Everything else verified: IRQ cells 0x120/0x11f/0x121 = 288/287/289; ect node starts at dts:4685; pmucal dbg
node at :4331-4338; dm_gpu at :8707-8712.

## Review of 4e7bff1 `M16 G1/G2 rows` (adversarial)

- Flagged: the MILESTONES G2 text and notes F3/G2-table called `need_smc` 0x18410204 "D_TZPC_G3D" as fact, and F3
  claimed *every* pd has a `<SYSREG>+0x204` value. `grep -n need_smc dt.img.0.dts` shows 15 values, pd-aud's is
  0x18c62000 (not +0x204), and nothing in the ROM names the register at 0x18410204. Both reworded to UNVERIFIED
  in the follow-up commit.
- Verified: G1 row figures match F1 (SPI 288/287/289, reg 0x5000), kbase string at kernel.img @0x17ea842.
