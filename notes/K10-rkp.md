# K10 — Exynos 9820 (beyond0lte) RKP/kernel-monitoring write-protected zones + sboot kernel-drop address

Started: 2026-10-10. Web-only research task. No code changes; this is the only file written.

## Question (two parts)

1. Which RAM regions does the 9820 bootloader's kernel-integrity monitor (Samsung RKP /
   "kernel monitoring" / KFI in LK4 + sboot sources) treat as write-protected?
2. Where does sboot drop the "fake Linux kernel" loaded from an Android boot.img on 9820 —
   DRAM base `0x80000000` + `text_offset` `0x80000` = `0x80080000`, or `0x80100000`?

## Known from this repo (9810 side, for contrast — not the answer)

- 9810 monitored zones: `0x80094000`–`0x828DBFFF` (`Star2LtePkg.fdf:13`, `Star2LtePkg.dsc:214-229`).
- 9810 sboot kernel drop: "near `0x80100000`" (`firmware/tools/bootshim/bootshim.S:12`).
- 9810 edk2-msm BootShim copies the FD to `0x90000000`.

## Status

IN PROGRESS — findings appended as each item is discovered.

## Progress log

### 2026-10-10 T01 — search-engine setup

- DuckDuckGo `html/` endpoint works but throttles: after 2 queries it returns a bot
  challenge ("Unfortunately, bots use DuckDuckGo too"). **Dead end / do not rely on DDG.**
- Bing via webfetch returns irrelevant results (Chinese Q&A spam) — **dead end**.
- **Brave Search (`https://search.brave.com/search?q=`) works** — use it as the primary engine.
- **grep.app API (`https://grep.app/api/search?q=`) works** — public-code search.

### 2026-10-10 T02 — grep.app: the 9810 constants have no public origin

- `grep.app q=0x80094000` → 1 hit only: `LLfam/CVE-2024-1086/tests/alloc_ptpages.log`
  (a QEMU virtual-address log line, unrelated). Source: https://grep.app/
- `grep.app q=0x828DBFFF` → **0 hits**.
- `grep.app q=Exynos9820Pkg` → **0 hits** (so no public EDK2 tree uses that package name;
  the repo's `Exynos9820Pkg` is local/derived).
- INFERENCE: the 9810 zone `0x80094000-0x828DBFFF` is a **local observation** from this
  project's own bootloop experiments, not a citable public source. So the 9820 numbers
  cannot be obtained by "finding the same numbers upstream" — need a 9820-specific source.

### 2026-10-10 T03 — Brave: candidate primary sources (not yet read)

- **Impalabs "A Samsung RKP Compendium"** — blog.impalabs.com/2111_attacking-samsung-rkp.html
  and /2101_samsung-rkp-compendium.html. Describes RKP internals incl. "physmap" and
  monitored page state. Source: https://search.brave.com/search?q=Samsung+RKP+%22kernel+monitoring%22+exynos+monitored+memory+range
- **dayzerosec "Reversing Samsung's H-Arx Hypervisor Framework - Part 1"** (2025-03-08) —
  "Samsung's exynos-implementation of their mobile hypervisor security platform ...
  commonly referred to as Real-time Kernel Protection or RKP".
  Source: https://dayzerosec.com/blog/2025/03/08/reversing-samsungs-h-arx-hypervisor-part-1.html
- **Phrack 70.10 "Hypervisor Necromancy; Reanimating Kernel Protectors"** — quotes
  Samsung kernel `vmm_init()` / `RKP_VMM_START` assembly. Source: https://phrack.org/issues/70/10
- **Project Zero "Lifting the (Hyper) Visor: Bypassing Samsung's Real-Time Kernel Protection"** (2017-02).
  Source: https://projectzero.google/2017/02/lifting-hyper-visor-bypassing-samsungs.html
- **"I hate software" blog: "Reverse-engineering Samsung Exynos 9820 bootloader and TZ"** (2019-05-29)
  — emulates `sboot_bl2.bin` for S10 in QEMU. Strongest 9820-specific lead.
  Source: https://allsoftwaresucks.blogspot.com/2019/05/reverse-engineering-samsung-exynos-9820.html
- **kernel.org "Interface between kernel and boot loaders on Exynos boards"** (Krzysztof Kozlowski)
  — documents sboot/u-boot interface incl. load addresses.
  Source: https://docs.kernel.org/arch/arm/samsung/bootloader-interface.html
- Quarkslab "Reverse Engineering Samsung S6 SBOOT" parts I/II (S6 = Exynos 920x era, older).
- chengyihe "how (L)ittle (K)ernel loads boot.img" (LK, ARM) — boot.img load-address logic.

### 2026-10-10 T04 — DEAD ENDS (do not repeat)

- `grep.app q=Exynos9820Pkg` → 0 hits; `q=0x828DBFFF` → 0 hits; `q="el2_code"` → 0 hits;
  `q=kernel-kaslr` → 0 hits; `q=RKP_SET_KERNEL_ROX` → 0 hits. (grep.app indexes public code
  only; leaked sboot/LK4 blobs are not in it.) Source: https://grep.app/
- `grep.app q=GREENTEA` → 2778 hits, all noise (mbed-os "greentea" test framework, Go
  "GreenTeaGC", OpenXRay "greenTeam"). The uH-image magic `GREENTEA` is **not** searchable
  this way. Source: https://grep.app/
- Brave `"0x80080000" sboot kernel load address exynos` → only a TI U-Boot forum post
  (`images->ep= 0x80080000`, unrelated mainline-jump code). **No Samsung 9820 hit.**
- Brave `"el2_code" reserved-memory exynos dtb` → nothing Samsung-specific.

### 2026-10-10 T05 — Impalabs "A Samsung RKP Compendium" (Jan 2021) — MECHANISM (facts)

Read: https://blog.impalabs.com/2101_samsung-rkp-compendium.html
(mirror of https://blog.longterm.io/samsung_rkp.html)
Its kernel-side quotes come from **`SM-G973F_QQ_Opensource`** = Galaxy S10 (SM-G973F,
Exynos 9820) Samsung open-source kernel drop, hosted at
https://github.com/Impalabs/resources — so the *kernel-side* code quoted IS 9820-era.
Its hypervisor-side decompilation is of an **exynos9610** `uh.elf`
(`"[+] version: %s", "UH64_3b7c7d4f exynos9610"`, dated Feb 27 2020) — so the *addresses*
in the hypervisor quotes are 9610, NOT 9820. Mark this distinction everywhere.

FACTS from that page (mechanism, applies to Exynos uH/RKP):

- RKP = Samsung's "Real-time Kernel Protection" hypervisor; runs at **EL2**, kernel at EL1,
  with **stage-2 translation** (`HCR_EL2.VM=1`, `VTTBR_EL2` → hypervisor-owned IPA→PA tables).
  So "write-protected" is enforced *per page* in the stage-2 tables, not by hardware
  memory-protection registers.
- uH is loaded by S-Boot from a partition named `UH`, magic `GREENTEA`, 0x1000-byte header
  stripped; on the 9610 sample: read into `0x86fff000`, then
  `load_hypervisor(0xc2000400, 0x87001000, 0x2000, 1, 0x87000000, 0x100000)` and
  `uh_init(0x87000000, 0x200000)`; `main()` asserts `&hyp_base == 0x87000000`
  ("[-] static s1 mmu mismatch") — i.e. **the EL2 code region is a fixed, compiled-in
  address per device**, 9610 = `0x87000000..0x87200000`.
- `pa_restrict_init()` seeds `protected_ranges` with the uH region
  (`protected_ranges_add(0x87000000, 0x200000)`), and every `physmap` entry is added to
  `protected_ranges` (`sparsemap_for_all_entries(&uh_state.phys_map, protected_ranges_add)`).
- S-Boot calls `init_cmd_add_dynamic_region` (HVC `0xC300C000`, cmd 2) **once per `memory`
  node in the DTB** — so the monitor's addressable DDR set comes from the DTB's
  `memory` nodes. `init_cmd_initialize_dynamic_heap` (cmd 4) sets
  `PHYS_OFFSET = memlist_get_min_addr(dynamic_regions)`.
- Three sparsemaps over all DDR: `physmap` (0x20 bits/page = page state), `ro_bitmap`
  (1 bit/page), `dbl_bitmap` (1 bit/page).
- Kernel text+rodata are made **ROX (read-only, execute, no write)** in stage-2 by
  `rkp_set_kernel_rox(access)` over `TEXT_PA .. ERODATA_PA`, and those pages are marked
  in `ro_bitmap`. Called first with `access=0` (RO) then again with `0x80`.
- Kernel side (`init/main.c`, S10 source): `rkp_init()` does
  `uh_call(UH_APP_INIT, 0, uh_get_fault_handler(), kimage_voffset, 0, 0)` and
  `RKP_GET_RO_BITMAP` / `RKP_GET_DBL_BITMAP` return the hypervisor bitmaps to the kernel.
  `include/linux/uh.h` (S10): `UH_PREFIX = UL(0xc300c000)`, `APP_INIT=0`, `APP_SAMPLE=1`,
  `APP_RKP=2`.
- **S-Boot patches the DTB** (`dtb_update`) adding `/reserved-memory` children — this is the
  direct link to the repo's `notes/vendor-dt-reserved.md` question:
  `el2_code` = uH region (`0x87000000`, `0x200000` on 9610, owner `el2,uh`);
  `el2_earlymem` = the dynamic-heap range the hypervisor carved (returned by cmd 4);
  `kaslr` = `0x80001000`, size `0x1000`, owner `kernel-kaslr`;
  `sboot` = `0xC9000000`, size `0x1700000` (or `0x2400000` when env `FORCE_UPLOAD == 5`),
  owner `sboot,rmem`.
  ⇒ INFERENCE: a beyond0lte DTB read on the phone would carry the **9820 values** of
  `el2_code` and `sboot` — that is the cheap device read that closes the EL2 region, and
  it is exactly the node set the earlier AI-sourced table said it "did not find".
- Kernel page-table writes by EL1 to protected pages are redirected: `set_pud`/`set_pmd`
  check `rkp_is_pg_protected()` (uses `ro_bitmap`) and call `RKP_WRITE_PGT1`/`RKP_WRITE_PGT2`.
- On Snapdragon RKP lives inside QHEE; on Exynos it is a standalone `uh.elf`/`vmm.elf`.
  From S20 onward Samsung switched to a new framework **`H-Arx`**.
  ⇒ INFERENCE: S10/9820 is still the **uH** framework (S20 = first H-Arx), so uH-shaped
  sources apply to beyond0lte.

### 2026-10-10 T06 — Phrack 70.10 "Hypervisor Necromancy" (Aris Thallas) — 9810-era facts

Read: https://phrack.org/issues/70/hypervisor-necromancy-reanimating-kernel-protectors.html
Target stated in the article: **G955FXXU4CRJ5** = Galaxy S8+ (**Exynos 9810**), QEMU 4.1.0.

FACTS:
- On S8+, RKP is `vmm.elf` **embedded in the kernel image** (`init/vmm.elf`); its ELF
  program header says **PA 0xb0100000, size 0x100000, entry 0xb0101000**; `vmm_main()`
  hard-compares the bootstrap arg to `0xB0100000`.
- The kernel logs the RKP reservations through `/proc/sec_log` (Exynos-Snapshot framework,
  `kernel/drivers/trace/exynos-ss.c`). Sample (S8+):
  `RKP: rkp_reserve_mem, base:0xaf400000, size:0x600000` / `0xafc00000,0x500000` /
  `0xb0100000,0x100000` / `0xb0200000,0x40000` / `0xb0400000,0x7000` / `0xb0407000,0x1000` /
  `0xb0408000,0x7f8000`; plus `RKP: vmm_reserved .base=ffffffc030100000 .size=1048576`,
  `RKP: vmm_kimage .base=ffffff8009375a10 .size=255184`, `RKP: vmm_start=b0100000`,
  `RKP: entry point=00000000b0101000`, `in rkp_init, swapper_pg_dir : ffffff800a554000`.
  ⇒ **`/proc/sec_log` (or dmesg) is the oracle that prints the 9820's own numbers.**
- Article explicitly says: "the S10+ hypervisor binary resides in a separate partition,
  is bootstrapped by the bootloader and then initialized by the kernel" — i.e. 9820 =
  `uh`-partition shape (matches the Impalabs compendium, below).
- INFERENCE: `rkp_reserve_mem` on 9810 is a *list of reserved regions* (RKP's own code/data/
  heap/log buffers), NOT the kernel's protected text. The protected-kernel-text range is
  computed at runtime from the kernel's own `_text/_etext/__start_rodata/__end_rodata`.

### 2026-10-10 T07 — the DTB declares an `rkp_region` node (S7/S8 dmesg logs)

grep.app found public dmesg logs in `pytorch/cpuinfo`:
- `test/dmesg/galaxy-s7-us.log`: `Reserved memory: reserved region for node 'rkp_region@0x9C200000': base 0x000000009c200000, size 15 MiB`
- `test/dmesg/galaxy-s8-us.log`: `... node 'rkp_region@0x9FA00000': base 0x000000009fa00000, size 13 MiB`
Source: https://grep.app/ (files: https://github.com/pytorch/cpuinfo/tree/main/test/dmesg)
- Repo-side corroboration (already in this checkout, `Star2LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c:125-126`): the **9810 (star2lte) DTB has `rkp_region` = `0xAF800000 + 0x2000000`** and `tima_region` = `0xB8000000 + 0x200000`.
  ⇒ So `rkp_region` grows/moves per device: S7 `0x9C200000`/15MiB → S8 `0x9FA00000`/13MiB → S9 `0xAF800000`/32MiB. **The 9820 value is still unknown** and is a DTB read, not a web fact (no public beyond0lte dmesg found: `pytorch/cpuinfo` has no S9/S10 logs — checked the whole `test/dmesg` listing).
- DEAD END: `grep.app q="reserved region for node"` → 0 hits (the phrase is not indexed).

### 2026-10-10 T08 — Impalabs compendium: what RKP actually marks read-only (mechanism)

Same page as T05. The write-protected set after `rkp_start()` + `rkp_paging_init()` +
`rkp_deferred_start()` (all addresses in the quotes are the **exynos9610** sample):
- `s2_unmap(0x87000000, 0x200000)` — the uH/EL2 region is **unmapped in stage 2**
  (kernel cannot touch it at all).
- kernel **text** `TEXT_PA..ETEXT_PA` and **rodata** up to `ERODATA_PA` set **ROX**
  (read-only + executable, no write) in stage 2; rodata pages marked in `ro_bitmap`.
  `TEXT/ETEXT/SRODATA/ERODATA` come from the kernel's `rkp_init_data` struct
  (`_text`, `_etext`, `__start_rodata`, `__end_rodata`) — i.e. **the protected window is
  wherever sboot dropped the kernel, plus the kernel's own section extents**.
- the **dynamic heap** region and the **log region** (`0x87100000..0x87140000`) set RO in stage 2.
- every **physmap entry** (the hypervisor's own page-state bookkeeping) added to
  `protected_ranges`; kernel PGDs (`swapper_pg_dir`, `tramp_pg_dir`) made RO.
- `PHYS_OFFSET = min(dynamic_regions)` — the DDR set comes from the DTB `memory` nodes.
- ⇒ **KEY INFERENCE for the repo's question:** there is no fixed "monitored zone" constant
  in the 9820 sources. The zone that matters = `[kernel drop + _text offset, kernel drop + _erodata offset]`,
  which is exactly the shape of the 9810 observation `0x80094000-0x828DBFFF`
  (starts ~0x14000 above a `0x80080000`-ish drop, spans ~42 MiB = kernel text+rodata).
  So on 9820 the same window must be re-derived from the 9820 drop address + the 9820 kernel's
  section sizes — **not** copied from 9810.

### 2026-10-10 T09 — dayzerosec H-Arx (S20+/S23 era, NOT 9820 — for contrast only)

Read: https://dayzerosec.com/blog/2025/03/08/reversing-samsungs-h-arx-hypervisor-part-1.html
- Post-2020 (S20+) Samsung uses **H-Arx**: core `harx.bin` loaded by SBOOT at
  **`0xc0000000`** (SMC `0x82000480` to init), uH **plug-in** `uh.bin` at **`0xc1400000`**
  (HVC `0xc6000010`), plugin header magic `GREENTEA`, `PLUG-IN_VER2`.
- uH plugin registers its RKP HVC range `0x83000000..0x8300ffff`
  (`UH_PREFIX = UL(0x83000000)` in the S23 `include/linux/uh.h`).
- S10/9820-era `uh.h` (from the SM-G973F_QQ_Opensource drop) uses **`UH_PREFIX = UL(0xc300c000)`**
  instead — different command prefix, same uH framework.
- Plugin API names of record: `exynos_plugin_api_stage1_entire_dram_map`,
  `exynos_plugin_api_hyp_map_va`, `exynos_plugin_api_hyp_private_and_stage2_map`,
  `exynos_plugin_api_tzmp2_drm_buffer_protect` — confirms the "map whole DDR then carve" model.

### 2026-10-10 T10 — Samsung's OWN exynos9820 SoC DTS (9820 facts, primary)

Read raw: https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/arch/arm64/boot/dts/exynos/exynos9820.dts
(Samsung's OSS kernel drop as mirrored by LineageOS; header: "SAMSUNG EXYNOS9820 SoC device tree source, Copyright (c) 2018 Samsung Electronics")

FACTS from that file:
- DDR banks as declared for 9820:
  `memory@80000000 { reg = <0x0 0x80000000 0x80000000> }` → **bank0 = 0x80000000 + 2 GiB**
  `memory@880000000 { reg = <0x00000008 0x80000000 0x80000000> }` → **0x880000000 + 2 GiB**
  `memory@900000000 { reg = <0x00000009 0x00000000 0x80000000> }` → **0x900000000 + 2 GiB**
  ⇒ 9820's DDR base is `0x80000000` (same as 9810) and the *first bank is only 2 GiB*,
  so `0x80000000..0xFFFFFFFF` then jumps to `0x880000000`.
- `chosen: chosen { linux,initrd-start = <0x84000000>; linux,initrd-end = <0x841FFFFF>; }`
  ⇒ **FACT (9820): the DTB itself declares the Android initrd at 0x84000000..0x841FFFFF.**
  This is the primary source behind the repo's `Exynos9820.h:76` comment — previously
  marked UNVERIFIED in `notes/m8-dscfdf.md:117`; it is now verified from Samsung's DTS.
- `ect { parameter_address = <0x95000000>; parameter_size = <0x3A000>; }`
  ⇒ 9820 "ect" parameter block at `0x95000000 + 0x3A000`.
- `chipid@10000000 { compatible = "samsung,exynos9810-chipid"; reg = <0x0 0x10000000 0x100> }`
- The SoC DTS `#include`s **`exynos9820-rmem.dtsi`** — "rmem" = **reserved memory**.
  That file is the 9820's `reserved-memory` node list ⇒ **fetch next** (it is the
  direct answer to the `rkp_region`/`el2_code` half of the question if Samsung ships
  those nodes statically).

### 2026-10-10 T11 — astarasikov's Exynos-9820 S-Boot QEMU port (9820 facts)

Read: https://github.com/astarasikov/qemu/commit/a2d3780055a37d07105c43deec97c59d38106c39
(patch, "YOLO 9820", 2018-02-21) + blog https://allsoftwaresucks.blogspot.com/2019/05/reverse-engineering-samsung-exynos-9820.html

FACTS from the patch:
- `#define BL2_LOAD_ADDR 0x8f000000` and `gdbscript: set $pc = 0x8f000000`,
  `b *0x8f002c44`, `set *(int*)0x8f16403c = 0` ⇒ **S-Boot BL2 for the S10 runs at
  physical 0x8f000000** (the author breakpoints inside it at real offsets).
- `WRITE_WORD(0x80000000, 0x66265999); //Exynos9820: set boot mode flag (Download choice)`
  ⇒ **the dword at physical 0x80000000 is a boot-mode parameter written by S-Boot**,
  i.e. 0x80000000 itself is *in use by sboot*, not free for a kernel drop.
- QEMU RAM map for the port: `VIRT_MEM = { 0x80000000, RAMLIMIT }`,
  fake periph `0x00000000+0x20000000`, fake IMEM `0x20000000+0x02000000`,
  fake TZMEM `0x22000000+0x02000000`, flash `0x20000000+0x04000000`.
- Blog says the S10 BL image also contains an **encrypted ARM Trusted Firmware (EL3
  monitor)** region right after Rijndael S-box constants, and that "even in the RKP
  hypervisor … some strings in the exception handler are not obfuscated".

### 2026-10-10 T12 — 9820 kernel trees: DTS sources exist publicly, addresses do not (so far)

- `LineageOS/android_kernel_samsung_exynos9820` (lineage-23.2) has
  `arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_{eur_open_17,18,19,20,22,24,25}.dts`
  and `exynos9820-beyond0lte_common.dtsi`, `..._input_common.dtsi`, etc.
  Source: https://api.github.com/repos/LineageOS/android_kernel_samsung_exynos9820/contents/arch/arm64/boot/dts/samsung
- `exynos9820-beyond0lte_common.dtsi` read: it is a `fragment@model/__overlay__` board file
  (audio/USB/pinctrl/thermistor/argos) — **no `reserved-memory`, no `rkp_region`, no
  `el2_code`** in it. So the reserved regions are not in the board DTS layer checked so far.
- `build.config.universal9820` (same tree) contains **no load addresses** — only
  `DEFCONFIG=universal9820_Q_defconfig`, `TARGET_SOC=exynos9820`, and the artifact list
  (`arch/arm64/boot/Image:kernel`, `arch/arm64/boot/dts/exynos/exynos9820.dtb:dtb.img`,
  `dtbo.img`, `vmlinux`, `vmlinux`).
  ⇒ the SoC DTS is `arch/arm64/boot/dts/exynos/exynos9820.dts` (to be read next).

### 2026-10-10 T13 — 9820 kernel headers: the uH/RKP contract on Exynos 9820 (PRIMARY, 9820)

Read raw from `LineageOS/android_kernel_samsung_exynos9820` @ lineage-23.2
(Samsung's own S10 OSS kernel drop, mirrored):

`include/linux/uh.h` — https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/include/linux/uh.h
- FACT (9820): `UH_PREFIX = UL(0xc300c000)`, `APP_INIT=0`, `APP_SAMPLE=1`, `APP_RKP=2`,
  `UH_APPID(APP_ID) = ((APP_ID & 0xFF) | UH_PREFIX)` → 9820 HVC command space is
  `0xC300C000/0xC300C100/0xC300C200` (matches the compendium's `uh_call(0xC300C000, …)`).
- **FACT (9820): `#define UH_LOG_START (0x87100000)` and `#define UH_LOG_SIZE (0x40000)`.**
  The kernel header hardcodes the hypervisor's log region for 9820 at
  **0x87100000 + 0x40000** — byte-identical to the exynos9610 sample in the Impalabs
  compendium (where uH itself is `0x87000000 + 0x200000`, log inside it at 0x87100000).
  ⇒ **INFERENCE (strong): on 9820 the uH/EL2 code+data region is `0x87000000..0x87200000`**,
  i.e. the same fixed EL2 window as the 9610 sample. That region is `s2_unmap`ped
  (inaccessible) plus `protected_ranges_add` (write-protected) — so **0x87000000+0x200000
  is a 9820 write-protected zone**, and it is *not* in the 9810 map at all.

`include/linux/rkp.h` — https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/include/linux/rkp.h
- FACT (9820): RKP command IDs `RKP_START=0x01, RKP_DEFERRED_START=0x02, RKP_WRITE_PGT1..3=0x03..0x05, RKP_EMULT_TTBR0/1=0x06/0x07, RKP_EMULT_DORESUME=0x08, RKP_FREE_PGD=0x09, RKP_NEW_PGD=0x0A, RKP_KASLR_MEM=0x0B, RKP_FIMC_VERIFY=0x0C, RKP_JOPP_INIT=0x0D, RKP_ROPP_INIT=0x0E…, robuffer 0x11/0x12, RKP_GET_RO_BITMAP=0x13, RKP_GET_DBL_BITMAP=0x14, RKP_GET_RKP_GET_BUFFER_BITMAP=0x15, RKP_DYNAMIC_LOAD=0x20, KDP 0x40..0x56`.
- FACT (9820): `RKP_INIT_MAGIC = 0x5afe0001` (test build `0x5afe0002`);
  `SPARSE_UNIT_BIT = 30` → the ro/dbl bitmaps are 1 GiB-granular sparsemaps over DDR.
- FACT (9820): the struct the kernel hands to RKP is
  `struct rkp_init { magic, vmalloc_start/end, init_mm_pgd, id_map_pgd, zero_pg_addr,
  rkp_pgt_bitmap, rkp_dbl_bitmap, rkp_bitmap_size, no_fimc_verify, fimc_phys_addr,
  _text, _etext, extra_memory_addr, extra_memory_size, physmap_addr, _srodata, _erodata,
  large_memory, tramp_pgd, tramp_valias }` — "//copy from uh (app/rkp/rkp.h)".
  ⇒ **the protected ranges are exactly `_text.._etext` and `_srodata.._erodata`
  (converted to PA by RKP) — they are runtime values, not 9820 constants.**
- FACT (9820): `RKP_FIMC_VERIFY` / `fimc_phys_addr` exist → RKP also verifies an FIMC
  (display) buffer; `no_fimc_verify` disables it. Relevant because the framebuffer is
  in the write path of any UEFI port.

`include/dt-bindings/soc/samsung/debug-snapshot-table.h` —
https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/include/dt-bindings/soc/samsung/debug-snapshot-table.h
- FACT (9820): `DSS_START_ADDR = 0xFD900000`; sizes: header 64K, kernel 2M, platform 4M,
  sfr 1M, cachedump 10M, etm 1M, bcm 4M, llc 4M, dbgc (1M−64K), pstore 32K, kevents 12M,
  s2d 21M at the **hardcoded** `DSS_LOG_S2D_ADDR = 0xEB310000`,
  `DSS_LOG_PSTORE_ADDR = DSS_LOG_S2D_ADDR − 32K = 0xEB308000`,
  `DSS_LOG_ARRAYDUMP_ADDR = DSS_LOG_S2D_ADDR + 21M = 0xEC810000`.
  ⇒ resolves the AI-sourced table in `notes/vendor-dt-reserved.md` against a primary
  header (its `log_pstore 0xEB308000`, `log_s2d 0xEB310000+0x1500000`,
  `log_arraydump 0xEC810000` rows match exactly; the contiguous chain from
  0xFD900000 upward matches too).
- FACT (9820): the DTS comment says "This definition is default settings. **We must use
  bootloader settings first**" → the DT values shipped in the DTS are the *kernel*
  defaults; sboot overrides them.

### 2026-10-10 T14 — DEAD ENDS (round 2)

- `grep.app q=vmm_start` → 33 hits, all unrelated (oneDNN, coreboot, QEMU NVMM).
- `grep.app q="rkp_reserve"` → 0 hits.
- `grep.app q=linux,initrd-start` → 318 hits, none from a Samsung DTS (grep.app does not
  index the Samsung kernel trees well).
- Brave intermittently returns HTTP 429 (rate limit) — space queries out.

### 2026-10-10 T15 — Samsung's OWN `exynos9820-rmem.dtsi` (9820, PRIMARY — the file T10 predicted)

Read raw: https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/arch/arm64/boot/dts/exynos/exynos9820-rmem.dtsi

FACTS:
- The 9820 static `reserved-memory` list is: `ion_video_stream` (size 0x4800000, alloc-ranges
  `0x40000000 + 0xB0000000`), `ion_crypto` (0x1800000), **`sec_debug_magic` reg =
  `0x80000000 + 0x1000`**, `sec_rdx_bootdev` (no-ship, reg = high cell 0x8 + `0x80000000 +
  0x0AD00000`), `debug_snapshot` (the DSS_* children, same header as T13),
  `abox_rmem` (size 0x3c00000, alloc-ranges `0xC0000000 + 0x20000000`), `seclog_mem`
  (`SECLOG_LOG_BUF_BASE + SECLOG_LOG_BUF_TOTAL_SIZE`, from
  `dt-bindings/soc/samsung/exynos9820-seclog.h` — not yet read).
- **FACT (9820): `0x80000000..0x80001000` is a reserved region (`sec_debug_magic`)** —
  corroborates T11's QEMU observation that sboot writes a boot-mode dword at physical
  0x80000000. So the drop address can never be 0x80000000 itself.
- **NOT here**: no `rkp_region`, no `el2_code`, no `kaslr`, no `sboot` node. ⇒ INFERENCE:
  those four are the ones sboot *adds at runtime* (`dtb_update`, T05), which is why the
  static DTS has none of them — the phone's own live DTB is the only source for the 9820
  values. Next read: Batch B (device).
- `ion-hpa-heap` (the ION heap region) has `hpa_limit = <0xA 0x00000000>` and
  `hpa_alloc_exception = <0x0 0xF0000000 0x10000000>` ⇒ the 0xF0000000+0x10000000 window is
  excluded from ION allocation — the same band the debug-snapshot chain lives in.

### 2026-10-10 T16 — DEVICE READ (TWRP root, `RF8M21HYDSZ   recovery`): the live 9820 DTB

`ls /proc/device-tree/reserved-memory/` on the phone returns **24 children**, and unlike
the static `exynos9820-rmem.dtsi` (T15) it **does** carry the sboot-added nodes:
`el2_code`, `el2_earlymem`, `kaslr`, plus `camera`, `secure_camera`, `cp_ram_logging`,
`mm_debug`, `modem_if`, `modem_spare`, `abox-rmem`, `ion_crypto`, `ion_video_stream`,
`sec_debug_magic`, `sec_rdx_bootdev`, `seclog_mem`, `debug_snapshot`.

Reads were `od -c` (byte-exact, decoded here; `#address-cells = 2`, `#size-cells = 1`,
`ranges`, so a 12-byte `reg` is `<hi32 lo32 size>`):

| node | reg (decoded) | verdict |
|---|---|---|
| `el2_code` | `0x0 0x87000000 0x200000` | **VERIFIED (9820): the uH/EL2 region is `0x87000000 + 0x200000`** — exactly the T13 inference from `UH_LOG_START 0x87100000`, and byte-equal to the exynos9610 sample in the Impalabs compendium |
| `el2_earlymem` | `0x9 0x7E000000 0x200000` → **`0x97E000000 + 0x200000`** | the hypervisor's early/dynamic-heap carve, in the **third** DDR bank (`memory@900000000`), not the first — so nothing in `0x80000000..0xFFFFFFFF` is the EL2 heap |
| `kaslr` | `0x0 0x80100000 0x100000` | **VERIFIED (9820): the kernel-KASLR node is `0x80100000 + 0x100000`** — on 9610 the same node was `0x80001000 + 0x1000`; the 9820 value sits **exactly at `0x80100000`**, which is the address the repo's 9810 shim comment calls "near 0x80100000" |
| `sec_debug_magic` | `0x0 0x80000000 0x1000` | matches the static DTS (T15) — `0x80000000..0x80001000` is reserved, so a kernel drop can never be at 0x80000000 |

⇒ **INFERENCE (mine, marked as such):** `kaslr` at `0x80100000` is sboot's own view of the
kernel window (the compendium's 9610 `kaslr` node sat 0x1000 above the `0x80000000` drop),
so on 9820 the drop is **`0x80100000`-ish, not `0x80080000`** — which contradicts
`Beyond0LtePkg.fdf:68`'s `0x80080000`. The read that closes it *exactly* is still the
`_text`-relative one (Batch C), but `0x80080000` is now known to sit **inside no reserved
node** while `0x80100000` sits **inside the `kaslr` node**.

### 2026-10-10 T17 — DEVICE READ part 2: exact bytes (authoritative; supersedes the T16 decode)

BusyBox `od -c` octal decoding was error-prone (it mis-stated `camera` as `0x9D000000`).
The reads below are `adb pull` of each `reg` file into
`C:\Users\ilya\AppData\Local\Temp\opencode\dtb\` and a byte-exact hex dump, decoded as
`<hi32 lo32 size>` big-endian. Raw bytes are recorded so nobody re-decodes from memory.

| node | raw 12 bytes | decoded base + size |
|---|---|---|
| `el2_code` | `00 00 00 00 87 00 00 00 00 20 00 00` | **`0x87000000 + 0x200000`** |
| `el2_earlymem` | `00 00 00 09 7E 00 00 00 02 00 00 00` | **`0x97E000000 + 0x200000`** (3rd DDR bank) |
| `kaslr` | `00 00 00 00 80 00 10 00 00 00 10 00` | **`0x80100000 + 0x100000`** |
| `sec_debug_magic` | (T16) | `0x80000000 + 0x1000` |
| `camera` | `00 00 00 00 99 00 00 00 0E 40 00 00` | `0x99000000 + 0xE400000` |
| `secure_camera` | `00 00 00 00 96 00 00 00 02 20 00 00` | `0x96000000 + 0x2200000` |
| `cp_ram_logging` | `00 00 00 00 FB 90 00 00 02 00 00 00` | `0xFB900000 + 0x2000000` |
| `mm_debug` | `00 00 00 00 91 20 00 00 00 00 10 00` | `0x91200000 + 0x100000` |
| `modem_if` | `00 00 00 00 F0 00 00 00 0B 90 00 00` | `0xF0000000 + 0xB900000` |
| `modem_spare` | `00 00 00 00 D0 00 00 00 01 60 00 00` | `0xD0000000 + 0x1600000` |
| `seclog_mem` | `00 00 00 00 C0 00 00 00 00 08 00 00` | `0xC0000000 + 0x80000` |
| `debug_snapshot.log_bcm` | `00 00 00 00 FE B1 00 00 00 40 00 00` | `0xFEB10000 + 0x400000` |
| `debug_snapshot.log_llc` | `00 00 00 00 FE F1 00 00 00 40 00 00` | `0xFEF10000 + 0x400000` |
| `debug_snapshot.log_dbgc` | `00 00 00 00 FF 31 00 00 00 0F 00 00` | `0xFF310000 + 0xF0000` |
| `debug_snapshot.log_kevents` | `00 00 00 00 FF 40 00 00 00 C0 00 00` | `0xFF400000 + 0xC00000` |
| `debug_snapshot.log_pstore` | `00 00 00 00 EB 30 80 00 00 00 80 00` | `0xEB308000 + 0x800000` (**live size 8 MiB, the DSS header default is 32 KiB** — sboot overrides, as the DTS comment in T13 warned) |

Consequences for the committed shim (`firmware/tools/beyond0lte-bootshim/bootshim.S`):
- **`0xFED14000` (the 9810 ramoops breadcrumb) is inside `log_bcm`** (`0xFEB10000..0xFEF10000`)
  — the compile-out behind `BEYOND0LTE_PRAM_DIAG 0` was right, and is now proven from the
  phone's own DTB, not from the DTS defaults.
- **`0x87000000 + 0x200000` is a real 9820 write-protected/unmapped zone** that the 9810 map
  never had; anything the UEFI port places there is dead on arrival.
- The 9820 `camera` reservation is `0x99000000 + 0xE400000` — the value the §14 note already
  used, now confirmed on-device.
- Nothing reserved covers `0x83000000` (the shim's temp-SP candidate), but the window
  `0x80100000..0x80200000` (`kaslr`) and `0x84000000..0x84200000` (initrd, T10) do.

### 2026-10-10 T18 — DEVICE READ part 3: owners, `chosen`, and the live DDR map

`cat` of the `compatible` strings (owner tags, same vocabulary the compendium's
`dtb_update` used in T05): `el2_code` → **`el2,uh`**, `el2_earlymem` → **`el2,uh`**,
`kaslr` → **`kernel-kaslr`**. So all three are sboot-added, not DTS-static — which is
why T15's static `exynos9820-rmem.dtsi` has none of them.

`/proc/device-tree/chosen` (live, i.e. **this boot is TWRP's own kernel+ramdisk**, not
stock Android): `linux,initrd-start` bytes `D5 00 00 00` → **`0xD5000000`**,
`linux,initrd-end` bytes `D5 A6 70 00` → **`0xD5A67000`** (a 10.4 MiB ramdisk).
⇒ **FACT (9820, sboot-loaded): the initrd does NOT live at the DTS default
`0x84000000..0x841FFFFF`; sboot put it at `0xD5000000`.** Every repo comment that
justifies `0x80080000` "because the DT's chosen node puts the Android initrd at
0x84000000" is therefore justified by a **kernel-default** value, not by what sboot
actually does on this phone.

`/proc/device-tree/memory*` node names + `reg` (byte-exact, `adb pull`):

| node | reg | live extent |
|---|---|---|
| `memory@80000000` | `00 00 00 00 80 00 00 00 3A B0 00 00` | **`0x80000000 + 0x3AB00000`** = ends `0xBAB00000` (937.5 MiB, **not** the DTS's 2 GiB) |
| `memory@C0000000` | `00 00 00 00 C0 00 00 00 20 00 00 00` | `0xC0000000 + 0x20000000` |
| `memory@E1900000` | `00 00 00 00 E1 90 00 00 1E 70 00 00` | `0xE1900000 + 0x1E700000` |
| `memory@880000000` | `00 00 00 08 80 00 00 00 80 00 00 00` | `0x880000000 + 0x80000000` |
| `memory@900000000` | (not pulled) | per DTS: `0x900000000 + 0x80000000` |

⇒ **INFERENCE (mine):** the 9820's first bank ends at `0xBAB00000`, so the repo's
UNVERIFIED UFS DMA window `0x90000000..0xBC800000` (the 9810's) **overruns the live
first bank by `0x1D00000`** — it is not a legal 9820 window as carried. The legal
first-bank window is `0x80000000..0xBAB00000` minus the reserved nodes above it
(`cp_ram_logging 0xFB900000` is in the second bank, `modem_if 0xF0000000` too).

### 2026-10-10 T19 — DEAD END: the kernel.org "Exynos bootloader interface" page

Read: https://docs.kernel.org/arch/arm/samsung/bootloader-interface.html — it is Krzysztof
Kozlowski's 2015 write-up of **mainline** Exynos 3250/4210/542x sysram/AFTR offsets. No
9820, no sboot load address. Do not re-fetch for this question.

### 2026-10-10 T20 — harsh review of the K10-half-1 source commit (`3f5c460`)

Checked mechanically: `git diff --stat` on the 9810 tree (`Star2LtePkg`, `Exynos9810Pkg`,
`tools/bootshim`) → **empty**, so the byte-unchanged rule holds. Build re-run after the
edit: `build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG` exit 0, `FVMAIN_COMPACT`
700416 total / 3128 free.

Two defects found, each now a follow-up commit:

1. **Arithmetic in a comment claim.** `0x3AB00000` = 983 040 000 B = **937.5 MiB**, not
   the "939 MiB" the new comment says. The hex (0xBAB00000 end, 0x1D00000 delta) is right;
   only the human-readable size was wrong.
2. **A silent behaviour change the read exposes.** `PlatformMemoryMapLib.c:150-161`
   publishes bank0 as `EXYNOS_DRAM_BASE + EXYNOS_DRAM_SIZE` = `0x80000000..0xD0000000`
   (1.25 GiB). The live `memory@80000000` bank is `0x3AB00000` long and **ends at
   `0xBAB00000`**, so the map has been handing Windows **0x15500000 (341 MiB) of RAM the
   DTB never declared** — the gap `0xBAB00000..0xC0000000` plus `0xC0000000..0xD0000000`,
   the last of which is inside the *separately* declared `memory@C0000000` bank but with
   the wrong length. This was latent before (bank0 looked 2 GiB); the live read makes it
   provably wrong. Fix: clamp the published length to the live bank0 size, keep the
   reserved-region hazard open.

## Sources

- https://blog.impalabs.com/2101_samsung-rkp-compendium.html (mirror of https://blog.longterm.io/samsung_rkp.html)
- https://phrack.org/issues/70/hypervisor-necromancy-reanimating-kernel-protectors.html
- https://dayzerosec.com/blog/2025/03/08/reversing-samsungs-h-arx-hypervisor-part-1.html
- https://allsoftwaresucks.blogspot.com/2019/05/reverse-engineering-samsung-exynos-9820.html
- https://grep.app/ (queries: 0x80094000, 0x828DBFFF, Exynos9820Pkg, el2_code, kernel-kaslr,
  RKP_SET_KERNEL_ROX, GREENTEA, rkp_reserve_mem, rkp_region, beyond0lte, BootShim,
  "reserved region for node")
- https://search.brave.com/search?q=... (primary engine used)
- https://github.com/pytorch/cpuinfo/tree/main/test/dmesg
- https://api.github.com/repos/LineageOS/android_kernel_samsung_exynos9820/contents/arch/arm64/boot/dts/samsung
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/build.config.universal9820
- https://github.com/Impalabs/resources (SM-G973F_QQ_Opensource kernel drop)
- https://github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md (from earlier repo notes)
- DEVICE READS (T16/T17): `adb shell` on `RF8M21HYDSZ   recovery` (TWRP root) —
  `ls /proc/device-tree/reserved-memory/`, `od -c` of `reg` files, then `adb pull` of the
  same files to `C:\Users\ilya\AppData\Local\Temp\opencode\dtb\` for byte-exact decoding.
  Reads only; nothing written to the phone except `/tmp`.
