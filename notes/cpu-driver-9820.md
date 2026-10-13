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
  (live MIDR read says 0x41/0xd0a; the shipped kernel's MIDR-shaped
  constants say 0xd09/A75 — unresolved conflict, see `notes/psci-parking.md`),
  `arm,cheetah` (Samsung 0x53/0x003). `enable-method = "psci"`.
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

**CORRECTED 2026-10-13 by the D11 pass (`notes/thermal.md` T1)**: the shipped
`base.dtb` has **five** zones, not four — `ISP` is the fifth — and the cooling
map is not all cpufreq: `G3D` cools `/mali@18500000` and `ISP` cools
`/fimc_is@17190000`. `LITTLE` and `ISP` have `polling-delay = 0` and no
governor, so the AP framework never polls them. There is **no `critical` trip
anywhere** in the DT; the 115 000 trips are type `hot`. The MMIO path also
exists (`CURRENT_TEMP1_0 0x84` + one-point trim) even though the AP reads via
ACPM IPC.

## F7. Kernel binary facts (Q8)

FACT: `boot.img` kernel field = raw ARM64 Image, 44 380 172 B,
`ARM64 Image magic` at 0x38, **`text_offset = 0x80000`**, `image_size
(memory) = 0x327b000`. This closes the `notes/m8-uefi-image.md` §2c
question: the drop address is DRAM base + 0x80000 = **0x80080000**, the
comment's 0x80100000 is wrong.
FACT: VA base of the shipped Image = **0xffffff8008000000** (VA_BITS=39,
PAGE_OFFSET 0xffffff8000000000 + text_offset): a u64 run at file 0x11c3370
carries VA = file + 0xffffff8008000000 exactly.

**CORRECTION (harsh review of my own earlier claim):** kallsyms is NOT
present. `acpm_ipc_send_data_sync` appears exactly once, inside the
.rodata string pool (bare NUL-terminated, no length prefix, mixed with
format strings) — it is a `__func__`-style print tag, not a symbol. The
kallsyms extractor found only an 894-entry false-positive run. Symbolizing
the shipped kernel must therefore go through **string xrefs** (ADRP+ADD
scan) + capstone, not a symbol table.

## F8. ACPM firmware location (PIT read, this session)

FACT (`BEYOND0LTE_EUR_OPEN.pit`, parsed with the validated layout from
`notes/pit-geometry.md`): dedicated partitions —
`ACPM` LBA 129 len 35 (×4096 = 143 360 B, flash file `acpm.bin`),
`ECT` LBA 109 len 20 (`ect.bin` — the skew/channel table behind
`ect-name = "dvfs_cpucl2"`), `DTB` LBA 23168 (`dt.img` — confirms the
base DTB is its own partition), `DTBO` LBA 25216.
FACT: this G970FXXUHHXJ6 BL tar ships `sboot/param/up_param/cm/keystorage/
uh/vbmeta` — **no `acpm.bin`** (Samsung ships it only when changed; the
phone's ACPM partition holds the image from an earlier package). `cm.bin`
= "CryptoManagerV50" (TEE), NOT the ACPM — checked, no ladder constants.

*(inference)* the Windows driver does not need the cal table offline: the
kernel enumerates each domain's ladder from the ACPM at init (the
`CPUFREQ of domain%d : L%2d %7d kHz` prints), so the driver can issue the
same query at load time. The protocol is the fact to extract — next,
string-xref disassembly of the shipped kernel.

## F9. The ACPM DVFS protocol — complete, end-to-end (the driver spec)

Cross-referenced: shipped DT (F2) + shipped kernel strings (F7) + the
matching-generation LineageOS 9820 source (same `dm_cpu_cl0/1/2` names,
same `exynos_acme` prints — the source and the dump agree on every string
we can check in both). Source files saved in `dumps/hhxj6/src/`.

**Call chain (Android):** cpufreq `exynos-acme.c` → `cal_dfs_set_rate
(cal_id, kHz)` → `IS_ACPM_VCLK` (cal_id type nibble `0x0B`, subtype `0x04`:
`ACPM_VCLK_TYPE = 0x0B040000`, `GET_IDX = id & 0xFFFF`) →
`exynos_acpm_set_rate(idx, kHz)` → `acpm_ipc_send_data_lazy(ch5)`.
CPU cluster fvmap indices: **cl0 (LITTLE) = 2, cl1 (MID) = 3, cl2 (BIG) = 4**
(cal_ids 0xb040002/3/4 from the shipped DT).

**MMIO windows (shipped DT `acpm_ipc` node):**
- mailbox/intr block: `0x15900000 + 0x1000`. Registers (from `acpm_ipc.h`):
  `INTGR0 0x08, INTCR0 0x0C, INTMR0 0x10, INTSR0 0x14, INTMSR0 0x18,
  INTGR1 0x1C, INTCR1 0x20, INTMR1 0x24, INTSR1 0x28, INTMSR1 0x2C,
  SR0..SR3 0x80..0x8C`.
- ACPM SRAM window: `0x2039000 + 0x30000`; framework block at offset
  `initdata-base` (shipped DT value = **0x7000** big-endian cell — the
  LineageOS dts confirms `<0x7000>`; my emitter first misread the cells as
  the string `"p"`, fixed in `fdt.py`); `dump-base 0x203c000 +0x2d000`.
- `acpm` node: `0x15830000 + 0x1000` ("timer_apm"), `fvmap_offset = 0xa400`.

**Channel setup (from `channel_init()`):** the `acpm_framework` struct at
SRAM+0x7000 gives `ipc_ap_max` and `ipc_channels` (SRAM offset of an
`ipc_channel[]` array: `{id, field, owner, type, channel_info ch, ap_poll}`).
AP-side TX ring = the channel's `rx_base/rx_front/rx_rear` (ACPM's receive
side); AP-side RX ring = `tx_base/tx_front/tx_rear`. Ring entry size
`q_elem_size` (16 B for cmd[4]), depth `q_len`; front/rear are u32 MMIO
cells inside SRAM, indices mod `q_len`.

**Message (u32[4]):** `cmd[0] = domain idx` (+ protocol bits: `SEQ_NUM`
6 bits at **bit 16**, `ID` 3 bits at 26, `OWN` 31, `RSP` 30, `INDIRECTION`
29 — the AP driver only stamps `++seq (1..63) << 16` into cmd[0]);
`cmd[1] = kHz`; `cmd[2] = opcode`: **FREQ_REQ 0, FREQ_GET 1, MARGIN_REQ 2,
COLDTEMP_REQ 3** (+ `DATA_INIT`/`SET_INIT_FREQ 3`); `cmd[3] = 0`.

**Send (from `__acpm_ipc_send_data`):** wait until `tx_front+1 != tx_rear`
(ring not full) → write 16 B entry at `tx_base + size*front` → `front =
(front+1) % len`, write it → doorbell `INTGR0 = (1 << ch) << 16`.
**Response:** ACPM writes the rx ring and raises `INTSR1` bit ch
(interrupt 0 = polling-capable; `INTMR1` bit set = polling mode — the
kernel supports pure polling, which is what a Windows driver should use).
Match response by seq bits (`(word >> 16) & 0x3f`), dequeue, clear
`INTCR1 = 1 << ch`. `FREQ_GET` returns the current kHz in `cmd[1]`.
Timeouts: 15 ms poll (`IPC_TIMEOUT`), 5 retries; a hard failure triggers
`s3c2410wdt_set_emergency_reset` — the ACPM is load-bearing; a Windows
driver must never leave a request unanswered/mis-paired.

**OPP ladder source (`fvmap.c`):** the rate/volt table is **plain memory
in ACPM SRAM**, right after the DVFS plugin image (`fvmap_offset 0xa400`
from the plugin's `base_addr`). Layout (`fvmap.h`): `fvmap_header[idx]`
(24 B: dvfs_type, num_of_lv, num_of_members, num_of_pll/mux/div,
gearratio u16, init_lv, num_of_gate, block_addr[3] u16, o_members,
o_ratevolt, o_tables — offsets relative to the fvmap base) →
`rate_volt[num_of_lv] = {u32 rate kHz, u32 volt uV}` → `dvfs_table`
(u8 params per level×member: PLL/mux/div register values) → `pll_header`
(`addr` in ACPM view, CMU base 0x90000000: `member_addr = plls->addr -
0x90000000`). Voltage step: `STEP_UV 6250` (S2MP200-correct 6.25 mV).

**Consequence for the Windows driver (design, *(inference)* from the above):**
1. Map `0x15900000+0x1000` and `0x2039000+0x30000`.
2. Walk framework@SRAM+0x7000 → channels → ch5 rings; set ch5 to polling.
3. Read the fvmap (SRAM + plugin base + 0xa400) for idx 2/3/4 → the exact
   per-SKU OPP+voltage ladder — no offline acpm.bin needed.
4. Frequency set = one 16 B message + doorbell + poll `INTSR1`; get =
   FREQ_GET. Thermal caps = clamp the ladder (the DT trip points, F6);
   MIF coordination = dm constraint tables (F3) or just pin MIF high.
5. Do NOT touch CMU/PMIC directly for CPU DVFS — the ACPM owns that
   sequencing (that is what makes "all core types" one uniform protocol:
   idx 2/3/4 covers ananke/prometheus/cheetah identically).

## F10. What the dump cannot give offline, and the read that closes it

FACT: `acpm.bin` is NOT in this G970FXXUHHXJ6 package (PIT has the ACPM
partition, LBA 129 ×4096 len 35, but the BL tar omits the file — Samsung
ships it only when changed). The live OPP table therefore lives only in
the phone's ACPM SRAM/partition.

Closing reads (pick one, both safe/reads-only, from TWRP root on Windows):
- **pstore**: the stock kernel prints the whole ladder at boot
  (`CPUFREQ of domain%d : L%2d %7d kHz`, `fvmap` `lv : [...] volt = ... uV`
  prints). `cat /sys/fs/pstore/console-ramoops*` (or `lpkmsg`), grep
  `CPUFREQ of domain`.
- or `dd if=/dev/block/platform/13d60000.ufs/by-name/ACPM bs=4096 count=35`
  — 143 KB, parse the fvmap layout above. (PIT device-type field is 8 for
  ACPM/ECT/DTB/DTBO/VENDOR; the by-name symlink removes any LUN arithmetic.)

## F11. Protocol VERIFIED in the shipped kernel binary (string-xref disassembly)

`xref.py` anchored on the `.rodata` string `"[%s] ipc_timeout!!!\n"`
(file 0x17d0d25) → ADRP+ADD xref at file 0x5ab7e0 → the enclosing function
is the shipped `__acpm_ipc_send_data`. Its disassembly confirms F9 point
for point, from the dump itself:

- channel struct stride `#0xe8` (232 B) — `umaddl x28, w0, w8, x27`;
- seq: `cmp w9, #0x40` + `csinc` = wrap at 64→1; `ubfiz w8, w8, #0x10, #6`
  = **seq inserted at bits 21:16**; `orr` into `cmd[0]`, `str` back ✓;
- `str wzr, [x8, #4/#8/#0xc]` = `cmd[1..3] = 0` after the ring copy ✓;
- ring-full spin with `yield`, deadline `0xe4e1c0` ns = **15 ms**
  (`IPC_TIMEOUT`) ✓;
- `bl memcpy_align_4` (0x5aaaa0) for the 16 B ring entry ✓;
- doorbell: `dsb st` → `str w23, [x8]` (front++) → `mov w10, #0x10000;
  lsl w8, w10, w8` = **(1<<ch)<<16** → `ldr x9, [x9, #0x20]` (acpm_ipc
  global → `intr`) → `add x9, x9, #8` = **INTGR0 at intr+0x08** → `str w8,
  [x9]` ✓ — exactly `apm_interrupt_gen()`;
- indirection test `tbnz w8, #0x1d` = bit 29 ✓.

FACT: VA base of the shipped Image re-confirmed: every ADRP target decodes
as file+0xffffff8008000000 (e.g. rodata page 0xffffff80097d0000).

So the Windows driver spec (F9) is no longer source-inferred: the exact
build running on this phone implements it.

## Next reads (order)

1. pstore/ACPM-partition read from the phone (Windows adb, TWRP root) →
   the shipped OPP+voltage ladder per cluster (closes F10).
2. ~~Optional disassembly confirmation~~ — **done, F11**: the shipped
   kernel implements the F9 protocol exactly.
3. Draft the Windows driver design doc (KMDF, polling-mode IPC, PPTT
   performance values from F5) as the next task — tracked as D7 in
   `DRIVERS-MILESTONES.md`.
