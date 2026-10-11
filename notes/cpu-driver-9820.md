# CPU driver research — Exynos 9820, all core types, from the Android 12 ODIN dump

Goal: gather the **factual** data a Windows CPU driver for the 9820 needs —
per-cluster clock trees, OPP (frequency/voltage) tables, regulator supplies,
power domains, ACM behaviour for the Samsung-custom cores — from Samsung's
own shipped artefacts (the Android 12 ODIN dump), not from guesses.

Ground rules apply: facts are reads with a source; *(inference)* is mine.
Nothing here is "done" until it is cross-checked against the dump itself.

## What is already established (from earlier passes, cited)

- Topology (`notes/dock-desktop.md` §16, two independent reads):
  8 CPUs, **two MPIDR clusters 6+2** — `cpu@0000`–`cpu@0005` (Aff1=0),
  `cpu@0100`–`cpu@0101` (Aff1=1); **three efficiency classes**:
  `arm,ananke` (Cortex-A55 class, MIDR 0x41/0xd05), `arm,prometheus`
  (0x41/0xd0a), `arm,cheetah` (Samsung 0x53/0x003). `enable-method = "psci"`.
- ACPI side is published: MADT GICC ×8, DSDT `_MAT` ×8, PPTT = package →
  3 efficiency classes → 8 cores (`notes/m8-acpi.md`, `Madt.aslc`, `Pptt.aslc`).
- CMU block addresses from Samsung's own calibration table
  (`tasks/P03-cmu-bank-hazard.md`): CMU_PERIC0 0x10400000, MIF0..3
  0x1bc00000–0x1bf00000, etc. **The CPU-cluster CMU banks are NOT yet in
  that list** — closing that is a goal of this task.
- The 9810's `ArmPlatformGetCorePosition` division is wrong for 6+2
  (`notes/dock-desktop.md` §15) — the explicit {6,2} table is what the
  platform lib uses.

## The dump (FACT, this session)

`/home/ilya/Downloads/firmware/` holds the complete SM-G970F Android 12
ODIN set — genuine POSIX tars despite the `.md5` suffix (downloader
artefact); members are lz4-compressed (`boot.img.lz4`, `sboot.bin.lz4`, …):

| tar | size | contents seen at head |
|---|---|---|
| `AP_G970FXXUHHXJ6_CL25257816_QB93696697_REV01_user_low_ship_meta_OS12.tar.md5` | 6 382 602 430 B | `boot.img.lz4` first member |
| `BL_G970FXXUHHXJ6_...tar.md5` | 4 935 857 B | `sboot.bin.lz4` |
| `CP_G970FXXUHHXJ1_...tar.md5` | 27 996 349 B | `modem.bin.lz4` |
| `CSC_OXM_G970FOXMHHXJ6_...tar.md5` | 508 262 584 B | `BEYOND0LTE_EUR_OPEN.pit` |
| `HOME_CSC_OXM_...tar.md5` | 508 231 869 B | `cache.img.lz4` |

Build `G970FXXUHHXJ6` (CL25257816) — newer than the GHWC1 build recorded in
`notes/m8-uefi-image.md` §3a; same CL number interestingly (25257816).
*(inference)* same kernel build lineage, so the boot.img DTB is the
authoritative shipped device tree for this exact phone.

## Questions the dump answers (each = a read, not a guess)

- **Q1 CPU-cluster CMU banks**: the `clock-controller@…` nodes' `reg` windows
  for the three cluster CMUs (core0/1/2) → DSDT/driver constants.
- **Q2 OPP tables**: every `operating-points-v2` table with freq/uV, and
  which cpu nodes consume it → the driver's per-cluster frequency ladder.
- **Q3 Voltage rails**: `cpu-supply` phandles → S2MPS15/S2MP200 regulator
  nodes (buck/ldo numbers) → the DVFS voltage path over HSI2C.
- **Q4 Power domains**: `power-domain` phandles → pd nodes + their
  SYSREG/PMU register windows → cluster on/off sequencing.
- **Q5 ACM**: does the shipped DT carry ACM (Automatic Clock Management)
  nodes for the cheetah/M4 cluster? Their `reg` decides whether a
  single-write frequency request register exists (CPPC-shaped control) or
  whether the driver must sequence PLL+mux itself.
- **Q6 Idle**: `idle-states`/PSCI idle entries per cluster → what the
  Windows idle path may use (WFI vs retention).
- **Q7 Thermal**: TMU nodes + trip points in the DTB (vendor thermal-engine
  XMLs live in super.img/vendor — secondary, needs the 6 GB member).
- **Q8 Kernel binary cross-check**: the shipped `kernel` (Image) strings —
  cpufreq/CMU/ACM driver names, `exynos,` compat strings — to confirm the
  DT reads match the driver that actually runs on this build.

## Tooling prepared (this session)

`tools/dump-tools/` — pure-Python, no root needed (host has no dtc and no
sudo):
- `fdt.py` — FDT parser + DTS emitter + phandle map (Devicetree Spec FDT
  layout read directly; `test_fdt.py` builds a synthetic 9820-shaped tree
  and asserts the full query stack — passing).
- `sbootimg.py` — Samsung boot.img (opaque header + ANDROID! v0 with
  dt_size at 0x28, same container `tools/twrp-winre/bootimg.py` round-trips),
  DTBO container parser, and **stream-selective** AP-tar member extraction
  (the 6.3 GB tar is never fully unpacked).
- `dtsift.py` — CLI: `cpus` (MPIDR/compatible/clocks/opp/power-domain/supply),
  `opp` (every operating-points-v2 table + consumers), `clocks`, `nodes`,
  `phandle`, `decompile`.
- venv at `tools/dump-tools/.venv` with `capstone` (aarch64 disasm),
  `lz4` (member decompression), `pyelftools`.

## Progress log

- 2026-10-11: dump located and identified (table above); tool suite written,
  synthetic round-trip test green. Next: stream `boot.img.lz4` +
  `dtbo.img.lz4` out of the AP tar, unpack, decompile the DTB, answer Q1–Q6.
- 2026-10-11 (extraction): AP tar members = `boot.img.lz4 recovery.img.lz4
  dt.img.lz4 dtbo.img.lz4 system.img.lz4 vendor.img.lz4 vbmeta.img.lz4
  userdata.img.lz4 dqmdbg.img.lz4 meta-data/ meta-data/fota.zip`.
  **The base DTB is `dt.img.lz4`** (not boot.img — boot.img carries no FDT;
  its header `dt_size` word is 1, a Samsung quirk). `dtbo.img` = 256 B
  header + **7 complete overlay DTs**, one per SKU; overlay 0 is
  `compatible = "samsung,BEYOND0LTE EUR OPEN 17"` — our phone.
  BL tar = sboot/param/up_param/**cm.bin**/keystorage/uh/vbmeta; CP = modem.
- 2026-10-11 (format, FACT): the shipped `dt.img` FDT is a **Samsung
  downstream dtc variant**: header `version = 40`, `size_dt_strings = 0`,
  and header word 3 is the **strings-block offset** (struct block runs to
  it; no reservation map). `fdt.py` now auto-detects this. The synthetic
  test caught two real parser bugs (root-node reuse, annotation of small
  integers as phandles — `REF_PROPS` whitelist added).

## F1. CPU topology — CONFIRMED from the shipped base DTB (Q: topology)

FACT (`dumps/hhxj6/base.dts`, `/cpus`): eight `cpu@` nodes exactly as
`notes/dock-desktop.md` §16 — `cpu@0000`–`0005` then `cpu@0100`–`0101`;
compatibles `arm,ananke` (0-3), `arm,prometheus` (4-5), `arm,cheetah`
(6-7); `enable-method = "psci"` on all. `cpu-map`: cluster0 = coregroup0
(4 ananke) + coregroup1 (2 prometheus), cluster1 = coregroup0 (2 cheetah).
Root `compatible = "samsung,armv8", "samsung,exynos9820"`.

## F2. DVFS architecture — the CPU clusters are driven by ACPM IPC (Q1/Q2/Q5)

**This is the central finding and it changes the Windows driver design.**
FACT from the shipped base DTB + shipped kernel strings:

- `/exynos-dm@17000000` `compatible = "samsung,exynos-dvfs-manager"`,
  `reg = <0x0 0x17000000 0x0>`, `acpm-ipc-channel = <0x1>`; `dm_domains`
  has `cpufreq_cl0/1/2` with `cal_id = 0xb040002/0xb040003/0xb040004`,
  `dm_type_name = "dm_cpu_cl0/1/2"`, plus `devfreq_mif` (cal_id
  0xb040000), `devfreq_int` (0xb040001), cam/iva/gpu domains.
- `/cpufreq/domain@0/1/2` `device_type = "cpufreq-domain"` — the cpufreq
  ladder (F3 below). Cooling: BIG zone → `domain@2`, MID → `domain@1`,
  LITTLE → `domain@0` (`cooling-device` cells).
- ACPM nodes: `acpm` (ch 0x4, `reg 0x15830000+0x1000` "timer_apm",
  `fvmap_offset 0xa400`), `acpm_ipc` (`reg = 0x15900000+0x1000` and
  `0x2039000+0x30000`, `dump-base 0x203c000+0x2d000`), `acpm_dvfs`
  (ch 0x5), `acpm_tmu` (ch 0x9), `exynos-sci` (ch 0xa).
- Kernel strings: `exynos_acme:` cpufreq driver ("Can't find device type
  cpufreq-domain", "failed to scale frequency of domain%d (%d -> %d)",
  "CPUFREQ of domain%d cal-id : %#x", "CPUFREQ of domain%d : L%2d %7d kHz"
  — the OPP list is **fetched from the ACPM at runtime**, not from DT);
  `ACPM-MFD` speedy tx/rx; `acpm_ipc_send_data_sync` (kallsyms present);
  `ECT DVFS [%s] not found` (the `mif-skew` `ect-name = "dvfs_cpucl2"`).
- **No `operating-points-v2` anywhere in the shipped base DT or the
  beyond0lte overlay** — consistent: voltages/OPPs live in the ACPM
  firmware's cal tables keyed by `cal_id`.

*(inference)* the Windows CPU driver does NOT need to sequence CMU PLLs or
PMIC rails for CPU DVFS: it must speak the **ACPM IPC mailbox protocol**
at 0x15900000 (channel 1 = dvfs-manager, 5 = acpm_dvfs) and reference the
same cal_ids. The ACPM firmware itself performs the v/f sequencing.

## F3. CPU frequency ladders as shipped in the DT (Q2, partial — the clamp layer)

FACT (`/cpufreq` in base.dts; kHz; decoded by dtsift):

| domain | cpus | cal-id | min | max | OPP steps in table |
|---|---|---|---|---|---|
| domain@0 LITTLE (ananke) | 0-3 | 0xb040002 | 442 000 | **1 950 000** | 13: 442k,546k,650k,806k,949k,1053k,1157k,1300k,1456k,1586k,1742k,1846k,1950k |
| domain@1 MID (prometheus) | 4-5 | 0xb040003 | 507 000 | **2 400 000** | 17: 650k…2400k (mif-perf: 2400k/2314k require MIF ≥ 2 093 000) |
| domain@2 BIG (cheetah) | 6-7 | 0xb040004 | 520 000 | **2 080 000** (`pm_qos-booting` = 2 080 000) | 23: 520k…2 912 000 (top 8 steps pair with MIF ≥ 2 093 000) |

- `sibling-cpus` = "0-3"/"4-5"/"6-7" — the three efficiency classes,
  matching PPTT.
- domain@1 `ank-perf` table = per-MID-frequency clamp on the LITTLE
  cluster (inter-domain constraint); domain@2 `mif-skew` uses ECT
  `dvfs_cpucl2` (hardware skew between MIF and the BIG cluster).
- *(inference)* the BIG table lists OPPs to 2.912 GHz (silicon capability
  shared across SKUs) but this build clamps at 2.08 GHz via `max-freq`;
  the authoritative per-SKU ladder is the ACPM cal table — read it via the
  same IPC the kernel uses (next task) or from the ACPM firmware blob.

## F4. Idle states (Q6) — PSCI standby, per-class latencies

FACT (`/cpus/idle-states`): `entry-method = "arm,psci"`; one state per
class, all `compatible = "exynos,idle-state"`, name "c2",
`arm,psci-suspend-param = <0x10000>` (PSCI Standby type):

| state | entry-lat | exit-lat | min-residency |
|---|---|---|---|
| ananke-cpu-sleep | 70 µs | 160 µs | 2000 µs |
| prometheus-cpu-sleep | 150 µs | 190 µs | 2500 µs |
| cheetah-cpu-sleep | 235 µs | 220 µs | 3500 µs |

## F5. Energy data — Samsung EAS constants (feeds PPTT performance ranks)

FACT (`/cpus/energy-data`): capacity-mips / power-coefficient:
ananke 272/160, prometheus 531/320, cheetah 785/760 (plus `-s` variants
215/130, 346/259, 365/506; capacity-weight/energy-weight 1/1, 2/1, 1/3).
*(inference)* these rank the three classes for Windows' PPTT
performance-register values: LITTLE < MID < BIG holds by DMIPS.

## F6. Thermal zones (Q7, partial)

FACT (`/thermal-zones`): zones `BIG` (BIG_THERMAL), `MID` (MID_THERMAL),
`LITTLE` (LITTLE_THERMAL), `G3D` (GPU); polling 50 ms passive / 1000 ms;
governor `power_allocator`; BIG trips (milli-°C): cold 20 000, switch-on
55 000, **control-temp 83 000 (passive)**, alert0 95 000, alert1 100 000,
alert2 105 000, alert3 110 000, hot 115 000; hysteresis 5 000 (cold 20 000).
Cooling maps bind the cpufreq domains (F3). TMU sensors are reached via
`acpm_tmu` (ch 0x9) — thermal reads are ALSO ACPM-IPC, not direct MMIO.

## F7. Kernel binary facts (Q8)

FACT: `boot.img` kernel field = raw ARM64 Image, 44 380 172 B,
`ARM64 Image magic` at 0x38, **`text_offset = 0x80000`**, `image_size
(memory) = 0x327b000`. This closes the `notes/m8-uefi-image.md` §2c
question: the drop address is DRAM base + 0x80000 = **0x80080000**, the
comment's 0x80100000 is wrong.
FACT: kallsyms present (`acpm_ipc_send_data_sync` found as a symbol) →
symbolize + disassemble the ACPM IPC path with capstone next.

## Next reads (order)

1. kallsyms extraction from `dumps/hhxj6/kernel` → symbol map.
2. Disassemble `acpm_ipc_send_data_sync` + `exynos_acme` target path →
   mailbox register offsets, message header format, DVFS command opcodes,
   cal-table fetch command → the protocol the Windows driver implements.
3. Locate the ACPM firmware image (candidates: `cm.bin` in BL tar,
   vendor.img `/vendor/firmware`) → its cal tables = the OPP+voltage
   ground truth per SKU.
4. Cross-check against LineageOS 9820 kernel source (raw.githubusercontent
   reachable) for the same drivers' C code.
