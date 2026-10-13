# Video codec (MFC) on the S10e — research + Windows driver

Task: research the Exynos 9820's **video offload engine (MFC, Multi-Format Codec)** as a driver
subsystem — what the block is on this SoC, what its constants are, and what a Windows path would
look like. The pointer that opened the topic is a third-party tree, `github.com/ianmacd/d2s`
(`drivers/`), which may also carry drivers for other open subsystems. Mode: **research**
(notes + constants + design + a written next order of work; no skeleton promised at this stage).
Started 2026-10-13. Adjacent prior topic: `notes/d5-gpu-display.md` (D5/D5b — Mali/display; the
MFC is a **separate** block from `mali@18500000`, and this file does not re-derive anything there).

## Progress log

### 2026-10-13 — T0: framing — what the plan of record already says about video (fact, cited)

- **No video-codec row existed before this session.** `DRIVERS-MILESTONES.md` has D1–D9 (D4b/D4c/D5b
  variants); the closest row is **D5/D5b GPU/display** (`mali@18500000`, kbase `b_r10p0`, no Mali
  WDDM — `notes/d5-gpu-display.md`). MFC is not mentioned anywhere in `progress.md`, `tasks/`,
  `DRIVERS-MILESTONES.md` or `MILESTONES.md` (grep `mfc|codec|v4l2|encode|decode` over the plan of
  record: zero hits in a milestone row).
- **D10 is therefore this subsystem's milestone ID**, and its device-visible actions get task IDs
  `V01…` (namespace rule: extend with a subsystem letter, not the milestone number).
- What D5 already settled that this topic must not re-litigate: **no Mali WDDM exists or ever
  shipped**, and the shipped-code display deliverable is GOP + in-box BasicDisplay. That matters
  here because Windows' *video acceleration* contract hangs off the display miniport — see U6.

### 2026-10-13 — T1: shipped-dump walk — the codec block in the vendor DTB and kernel (fact)

Method (reusable): `python3 /tmp/opencode/video-mfc/walk.py dumps/hhxj6/base.dtb` — a walker
over `tools/dump-tools/fdt.py` that matches every node whose **name or compatible** hits
`mfc|codec|video|m2m|s5p|hwfc|mmcache|sysmmu` and prints **raw big-endian cells**, resolving
each phandle-bearing cell through the tree's own phandle map (`0x2 -> /clock-controller@…`,
or `-> UNRESOLVED`). It exists because `base.dts` is misleading: `fdt.to_dts()` annotates
`interrupts`/`clocks` cells as phandles when the integer happens to collide with a phandle,
so `base.dts:4434` prints `interrupts = <0x0 &gpm28 0x0>` and `base.dts:4436` prints
`clocks = <&clock-controller@0x1a240000 &spi0-cs>` — both are **coincidental collisions**, the
second cell of each is a plain number. Trust the raw cells below, not the .dts annotations.
`base.dtb` parses as FDT **version 40**, 1117 nodes (`fdt.parse` samsung-layout branch).
  Caveat found while using it: the walker's printable-string heuristic
  (`all(0x20 <= b < 0x7f for p in parts for b in p)`) iterates the **NUL-split parts**, so it
  mis-renders short numeric properties as ASCII — e.g. it printed `iommus = "G", "H"` for
  `b'\x00\x00\x00G\x00\x00\x00H'`. **Every number quoted below comes from a direct
  `struct.unpack(">…I", value)` of the DTB property, not from the walker's rendering**; the
  walker was used only to enumerate nodes and to resolve phandles.

**1. Node identity — the codec block is present in the base DTB, with no `status` property
(i.e. enabled by the DT-spec default).**

- `/mfc@188D0000` — `compatible = "samsung,exynos-mfc"` (single string, no fallback, **no MFC
  version in the compatible**). Raw read: `fdt.parse(open("dumps/hhxj6/base.dtb","rb").read())`,
  `find("/mfc@188D0000").props["compatible"] == b"samsung,exynos-mfc\x00"`. Also at
  `dumps/hhxj6/base.dts:4431-4432`.
- **`status` property: ABSENT** on `/mfc@188D0000` — verified by enumerating all 69 property
  names of the node (the list has no `status`). Per DT spec an absent `status` means enabled;
  whether Samsung's 4.x-era MFC driver ever checks `status` is **not** established here.
- The node is **from the base DTB**, not an overlay (it is in `base.dtb` itself). No overlay
  touches it — see §1b.
- Alias: `mfc0 = "/mfc@188D0000"` — `dumps/hhxj6/base.dts:3957` (aliases node).
- Related nodes found by the same walk (names only here, details below): `/pd-mfc@15862600`,
  `/dbgdev-pd-mfc`, `/pmucal_dbg/pmucal_dbg_local/pd-mfc`, `/iommu-domain_mfc`,
  `/iommu-domain_mscl_smfc`, `/smfc@18B00000`, `/exynos-dm@17000000/dm_domains/devfreq_mfc`,
  `/exynos_devfreq/devfreq_mfc@17000090`, `/sysmmu@18890000`, `/sysmmu@188B0000`,
  `/ion-hpa-heap/ion_video_{fw,nfw,frame,scaler}`, `/reserved-memory/ion_video_stream`.
- **No node anywhere in `base.dtb` has a compatible matching `s5p` or `m2m`** — checked over all
  **153 distinct compatible strings** in the tree (`[c for c in comps if "s5p" in c or "m2m" in c]`
  → `[]`). The walk's only `codec`-compatible hits are `/pinctrl@18C60000/aud-codec-mclk{,-idle}`
  (audio MCLK pin groups, unrelated). Dead end recorded below.

**1b. Overlays — no overlay adds, changes or disables the codec.**
- `dtbo.img` (8 388 608 B) splits into **7 FDT blobs** via `fdt.split_blobs()`; each root
  `compatible` is `samsung,BEYOND0LTE EUR OPEN <17|18|19|20|22|24|25>`, `samsung,EXYNOS9820`
  (so the 7 are hw-revision variants of the same overlay set, not 7 different feature sets).
  They are **byte-identical** to `dumps/hhxj6/dts/dtb00.dtb … dtb06.dtb` (md5 of each split
  blob == md5 of the matching file; sizes 176648/176728/178028/177652/176248/176180/175164 B).
- Walked every node of all 7 blobs for name/compatible matching `mfc|codec|video|m2m|s5p`:
  **zero hits on the video codec**. The only `mfc` hit in any overlay is
  `/fragment@5/__overlay__/mfc-charger@3b` with `compatible = "idt,mfc-charger"` — an IDT
  **wireless-power (Qi) receiver IC** on i2c addr 0x3b, nothing to do with the codec
  (its properties are `battery,wireless_charger_name`, `battery,tx_mfc_iout_*`).
  All other `codec` hits are ASoC/audio (`/fragment@model/__overlay__/sound/*/codec`,
  `audio_codec_dummy` = `snd-soc-dummy`, `codec-irq`, `codec-reset` = the Cirrus cs35l40
  audio amp).
- Fragment counts per blob: 125/124/128/127/123/123/121; **no fragment has an `__overlay__`
  child named `mfc*`/`video*`/`m2m*`**, and `strings -a dumps/hhxj6/dtbo.img | grep -i mfc`
  returns only the charger strings (`battery,tx_mfc_iout_{gear,phone,phone_5v,lcd_on}`,
  `idt,mfc-charger`, `mfc-charger@3b`).
- `base.dtb` has a `__symbols__` node but **no `__fixups__` and no `__local_fixups__`**; the
  overlays' cross-references into the base are by `target-path` (e.g. `/fragment@lcd` → `/`),
  and none of them name the codec.
- **Conclusion (fact for U1): `/mfc@188D0000` comes from the base DTB, is not overridden by any
  of the 7 overlays, and carries no `status` property at all — so nothing in the shipped firmware
  disables it.** The node-name check alone was *not* enough (every fragment's `target` is the
  placeholder `<0xffffffff>` and 14 fragments per blob do set `status = "disabled"`); §1c closes
  it properly via the `__fixups__` inversion.

**1c. Overlays, closed properly — the `__fixups__` inversion (reusable method).**
The first pass over the overlays was **not** sufficient on its own: every fragment in every
overlay blob has `target = <0xffffffff>` (a placeholder — all 125/124/128/127/123/123/121
fragments, one distinct value), and 13–14 nodes per blob do carry `status = "disabled"`, so
"no overlay disables anything" could not have been concluded from node names alone. Recorded
here as a **method correction**.
- How the target is actually recoverable: the overlay's `/__fixups__` node maps
  **base `__symbols__` name → list of `path:property:offset` references**, e.g. the key `abox`
  carries `…/fragment@74:target:0 /fragment@97:target:0`. Inverting that gives, for every
  overlay, exactly which base node each fragment targets.
- Quirk to know: in **`base.dtb` the `__symbols__` values are path *strings*, not phandles**
  (749 entries; `mfc_0 = "/mfc@188D0000"`, `pd_mfc = "/pd-mfc@15862600"`,
  `sysmmu_mfc0 = "/sysmmu@18890000"`, `sysmmu_mfc1 = "/sysmmu@188B0000"`,
  `tsmux = "/tsmux@188E0000"`, `smfc = "/smfc@18B00000"`, `scaler_0 = "/scaler@18B10000"`,
  `jsqz = "/jsqz@18B30000"`). A `struct.unpack(">I", …)` on them raises
  `struct.error: unpack requires a buffer of 4 bytes` — that is the reason the naive
  symbol-resolution attempt failed, not a parse error.
- The model is confirmed mechanically: of the 749 base `__symbols__` names, the 167
  `__fixups__` keys in blob 0 are a **strict subset** (intersection = 167/167), so a symbol that
  is *not* a `__fixups__` key is *not* referenced by that overlay at all.
- **Result (fact):** across **all 7** overlay blobs, **no codec-related symbol appears as a
  `__fixups__` key at all**. Per-symbol check (blob 0, same result for 1-6):
  `mfc_0`, `pd_mfc`, `sysmmu_mfc0`, `sysmmu_mfc1`, `tsmux`, `smfc`, `scaler_0`, `jsqz` —
  **all present in base `__symbols__`, all absent from overlay `__fixups__`**. So no overlay
  references the codec for *any* property, not merely for `status`.
- All 14 `status = "disabled"` nodes in blob 0, with the base symbol each fragment targets
  (11 resolved, 3 unresolved): `fragment@42`→`serial_20`, `@43`→`serial_21`, `@51`→`usi_7`,
  `@55`→`usi_11`, `@56`→`usi_12`, `@57`→`usi_13`, `@58`→`usi_14`, `@59`→`usi_15`,
  `@84`→`hsi2c_25`, `@86`→`pcie_1`, `@116`→`hsi2c_1`; the three with **no** `:target:` fixup are
  `/fragment@97/__overlay__/ext-bin@0`, `/fragment@model/__overlay__/hs_tune` and
  `…/ss_tune` (the latter two are audio tuning nodes added by the named `fragment@model`).
  **None of them is the codec**, and none of the 11 resolved targets is codec-adjacent.
- Combined with §1b (no `__overlay__` child named `mfc*`/`video*`/`m2m*`, no codec string in
  `dtbo.img`): **the codec node is base-DTB-only and untouched by the overlays.**

**2. `reg` — decoded.** Parent bus is the **root node**: `#address-cells = <2>`,
`#size-cells = <1>` (raw: `/mfc@188D0000` is a child of `/`, and `/`'s props are
`['compatible','dtb-hw_rev','dtb-hw_rev_end','interrupt-parent','#address-cells','#size-cells']`
with `#address-cells = 00 00 00 02`, `#size-cells = 00 00 00 01`).
- `/mfc@188D0000/reg = <0x0 0x188d0000 0x10000>` → **base 0x188D0000, size 0x10000 (65 536 B)**.
  Raw hex `00000000188d000000010000`. Matches the unit-address in the node name.
- Child nodes (the mfc node itself declares `#address-cells = <0x2>`, `#size-cells = <0x1>`,
  `ranges;` — so children use the same 2/1 split):
  - `/mfc@188D0000/iommu/reg = <0x0 0x18890000 0x9000  0x0 0x188b0000 0x9000>` → 0x18890000 +36 KiB,
    0x188B0000 +36 KiB (the two SYSMMU windows, duplicated from the sysmmu nodes — see §5).
  - `/mfc@188D0000/hwfc/reg = <0x0 0x18a28000 0x100>` → **0x18A28000, 256 B**. The DT does not
    expand the acronym; the driver has `mfc_hwfc_encode`, `__mfc_hwfc_check_run`,
    `mfc_otf_set_hwfc_index` and `__mfc_otf_init_hwfc_buf` (strings 148322-148343) and the
    `mfc_otf_*` group sits next to them, so HWFC belongs to the **on-the-fly encode path** —
    reading it as "hardware flow control" is **inference**.
  - `/mfc@188D0000/mmcache/reg = <0x0 0x1a300000 0x100>` → **0x1A300000, 256 B**.
  - `/mfc@188D0000/cmu/reg = <0x0 0x1a200000 0x8000  0x0 0x1bc00000 0x8000  0x0 0x1bd00000 0x8000
    0x0 0x1be00000 0x8000  0x0 0x1bf00000 0x8000>` → five 32 KiB CMU windows: 0x1A200000,
    0x1BC00000, 0x1BD00000, 0x1BE00000, 0x1BF00000. (Which bank is MFC's is **not** stated by the
    DT; inference only — do not use as a constant.)

**3. `interrupts` — raw cells and decode.**
- `/mfc@188D0000/interrupts = <0x0 0x152 0x0>` (3 cells, raw hex `000000000000015200000000`).
  The node has **no `interrupt-parent`**, so it inherits the root's:
  `root/interrupt-parent = <1>` → phandle 1 = **`/interrupt-controller@10100000`**,
  `compatible = "arm,cortex-a15-gic", "arm,cortex-a9-gic"`, **`#interrupt-cells = <3>`**,
  `interrupt-controller;` present, `reg` = 4 windows (GICD 0x10100000/4 KiB, 0x10101000/4 KiB,
  0x10102000/8 KiB, 0x10104000/8 KiB). So the 3-cell decode is valid.
- Cell 0 = `0` = the SPI type in this tree's convention; cross-checked against the GIC's own
  `interrupts = <1 9 3844>` (cell 0 = 1, PPI 9 = the arch private timer) — i.e. cell 0 selects
  the interrupt class, 0 = SPI. **Inference from those two reads, not from a binding doc.**
- Therefore **SPI number = 0x152 = 338**, and **GSIV = 338 + 32 = 370 = 0x172** (the +32 SPI
  offset is the ARM GIC architectural rule, applied here because the parent is a GIC with
  `#interrupt-cells = 3`; the dumps do not restate the +32).
- Cell 2 = `0` = trigger/type field (no flags set).
- The two SYSMMUs feeding MFC have their own interrupts (raw): `/sysmmu@18890000/interrupts =
  <0x0 0x157 0x0  0x0 0x158 0x0>` → SPI 343 (GSIV 375) and SPI 344 (GSIV 376);
  `/sysmmu@188B0000/interrupts = <0x0 0x159 0x0  0x0 0x15a 0x0>` → SPI 345 (GSIV 377) and
  SPI 346 (GSIV 378). Both carry `sysmmu,secure-irq;` and `sysmmu,secure_base` = 0x188A0000 /
  0x188C0000 respectively.
- **No `interrupt-names` property** on `/mfc@188D0000` (absent from the 69-name property list).

**4. `clocks` / `clock-names`.**
- `/mfc@188D0000/clocks = <0x2 0x1a6>` → phandle **0x2 = `/clock-controller@0x1a240000`**,
  `compatible = "samsung,exynos9820-clock"`, `#clock-cells = <1>`, `reg = <0 0x1a240000 0x8000>`.
  So there is exactly **one** clock specifier: **clock index 0x1a6 = 422** in that provider.
- `/mfc@188D0000/clock-names = "aclk_mfc"` — one name, matching the one specifier.
- The provider is a **single flat clock controller node**, not per-bank: the DT gives no bank
  name for index 422. Which CMU bank/gate 422 is must come from the CMU cal tables
  (`dumps/hhxj6/src/9820_cmucal-sfr.c`, `cmucal.h` are in the dump — not yet read for this).
- Sibling clocks on the same provider for the MFC SYSMMUs: `/sysmmu@18890000/clocks = <0x2 0x1a7>`
  (`clock-names = "aclk"`), `/sysmmu@188B0000/clocks = <0x2 0x1a8>` (`clock-names = "aclk"`) —
  indices 423 and 424, i.e. **422/423/424 are contiguous MFC-block gates** (fact from the raw
  cells; the "contiguous ⇒ same bank" reading is inference).

**5. power-domain / pmu / syscon / iommu / memory-region phandles.**
- `/mfc@188D0000` has **no** `power-domain`, **no** `samsung,power-domain`, **no**
  `samsung,syscon-phandle`, **no** `memory-region`, and no `#iommu-cells` (it is an IOMMU **consumer**, not a provider)
  (all absent from its 69-name property list — verified by enumeration, not by grep).
- `/mfc@188D0000/iommus = <0x47 0x48>` → **phandle 0x47 = `/sysmmu@18890000`**,
  **phandle 0x48 = `/sysmmu@188B0000`**, both `compatible = "samsung,exynos-sysmmu"`,
  both `#iommu-cells = <0x0>` (so the two cells are two separate 0-arg specifiers, not
  phandle+args), both **`status` absent**.
  - `/sysmmu@18890000`: `reg = <0 0x18890000 0x9000>`, `port-name = "MFC port 0"`
    (raw bytes `4d464320706f7274203000`), `qos = <0xf>`, `samsung,power-domain = <0x6>`.
  - `/sysmmu@188B0000`: `reg = <0 0x188b0000 0x9000>`, `port-name` = raw
    `4d464320706f727420312c2057464400` = **"MFC port 1, WFD"**, `qos = <0xf>`,
    `samsung,power-domain = <0x6>`.
- Power domain: phandle **0x6 = `/pd-mfc@15862600`**, `compatible = "samsung,exynos-pd"`,
  `reg = <0 0x15862600 0x20>`, `cal_id = <0xb138000c>`, `need_smc = <0x18820204>`,
  **`status = "okay"`**. Referenced by `/sysmmu@18890000`, `/sysmmu@188B0000` and
  `/dbgdev-pd-mfc` (all via `samsung,power-domain`). So on this board the MFC's power is switched
  through the **SYSMMU** power-domain, not on the function node.
- IOMMU bus grouping: `/iommu-domain_mfc` (`compatible = "samsung,exynos-iommu-bus"`) has
  `domain-clients = <0x100 0x101>` → **0x100 = `/mfc@188D0000`** (its own phandle) and
  **0x101 = `/tsmux@188E0000`**. `/tsmux@188E0000/iommus = <0x48>` — it shares `/sysmmu@188B0000`
  with MFC. Separate group `/iommu-domain_mscl_smfc` = `<0x102 0x103 0x104>` →
  `/scaler@18B10000`, `/smfc@18B00000`, `/jsqz@18B30000`.
- Devfreq: `/exynos_devfreq/devfreq_mfc@17000090` (`compatible = "samsung,exynos-devfreq"`,
  `reg = <0 0x17000090 0x0>`, `devfreq_domain_name = "dvfs_mfc"`, `dfs_id = <0xb04000e>`,
  `freq_info = <0x20b70 0x20b70 0x20b70 0x20b70 0xa2990 0x20b70>` = 134000/134000/134000/134000/666000/134000 — the DT does not state the unit; kHz is the obvious reading but is **inference**)
  and a matching `/exynos-dm@17000000/dm_domains/devfreq_mfc`.

**6. The shipped kernel — the MFC driver is IN the Image (built-in, not a module).**
Method: `strings -a dumps/hhxj6/kernel > /tmp/opencode/video-mfc/kernel.strings.txt`
(291 408 lines), then `grep -inE 'mfc' …` → **867 matching lines**. Quoting the decisive ones
(line numbers are `grep -n` line numbers into that strings file):
- `50894: samsung,exynos-mfc` — the DT match string, present in the Image. Raw byte search:
  file offset **0x1271d70**, string length 18, **69 NUL bytes before it and a 2 501-byte zero
  run after it** (next printable run starts at 0x1272748). The surrounding .rodata is the V4L2
  subsystem (`1271484 v4l2_mem2mem.debug`, `1271500 videobuf2_v4l2.debug`,
  `1271598 videobuf2_dma_sg.debug`) — consistent with an `of_device_id` entry in the media
  part of .rodata, but the table layout is **not** proven from the dump.
- `146687: s5p-mfc`, and the V4L2 media-entity names `146692 s5p-mfc-dec`,
  `146693 s5p-mfc-enc`, `146694 s5p-mfc-dec-secure`, `146695 s5p-mfc-enc-secure`,
  `146696 s5p-mfc-enc-otf`, `146697 s5p-mfc-enc-otf-secure` (raw: `s5p-mfc` at file 0x1898c04).
- Driver source paths baked in: `147161 drivers/media/platform/exynos/mfc/mfc_mem.h`,
  `147337 drivers/media/platform/exynos/mfc/mfc_hwlock.c`,
  `147581 drivers/media/platform/exynos/mfc/mfc_watchdog.c`,
  `147689 drivers/media/platform/exynos/mfc/mfc_sync.c` → the Samsung downstream
  `drivers/media/platform/exynos/mfc/` tree. (**Contrast with mainline is NOT a dump fact** —
  mainline `s5p-mfc` living in `drivers/media/platform/samsung/s5p-mfc/` with no
  `mfc_hwlock.c`/`nal_q`/`otf` is external knowledge and belongs to U7, to be checked against a
  mainline tree).
- Whole driver is present as code: `mfc_open/mfc_release/mfc_poll/mfc_mmap/mfc_irq/
  mfc_top_half_irq/mfc_suspend/mfc_resume/mfc_runtime_suspend/mfc_runtime_resume/
  mfc_try_run/mfc_just_run/mfc_dec_* /mfc_enc_* /mfc_nal_q_* /mfc_otf_* /mfc_mmcache_enable`
  (strings lines 146796-148382).
- **Built-in vs module — three independent reads:**
  1. the driver's strings and code are inside `dumps/hhxj6/kernel` (the boot.img Image) itself;
  2. `strings -a dumps/hhxj6/kernel | grep -E '\.ko$'` → **one** hit, and it is not a module:
     `4skbuff: %s: attempt to compute crc32c without libcrc32c.ko` (an error message);
  3. the vendor image has **no loadable modules at all**: `debugfs -R 'ls /lib/modules'` and
     `'ls /lib64/modules'` → "File not found"; `/vendor_dlkm` and `/odm_dlkm` contain only
     `etc/` (`NOTICE.xml.gz`, `build.prop`).
  ⇒ **`y`, built in.** (U3 is answered from the dump side; the defconfig mirror read is still
  not done and is not needed for this conclusion.)
- `kallsyms.log` / `symbols.tsv` are a **dead end**: `cat dumps/hhxj6/kallsyms.log` =
  `# addresses at file 0x11c3370, num_syms 894` + `# _text VA 0xffffff80080a32bc -> file 0`,
  and all 894 rows of `symbols.tsv` have an **empty name column** (`cat -A` shows the line ends
  with a bare `^I$`). That matches the documented negative result in
  `tools/dump-tools/kallsyms.py:4-8` — the shipped G970FXXUHHXJ6 kernel carries no kallsyms.
- Code xref confirming the firmware path is used from kernel text:
  `xref.py dumps/hhxj6/kernel --va 0xFFFFFF80098A5E30` → `xref: adrp at file 0xaa3968 ->
  0xffffff80098a5e30` (the VA of the `mfc_fw.bin` literal at file 0x18a5e30, VA base
  `0xFFFFFF8008000000` per `xref.py:24`).
- **Bug found in the bundled tool (do not re-trip on it):** `xref.py --string 'samsung,exynos-mfc'`
  prints nothing and exits `give --string or --va`. Cause, read at `tools/dump-tools/xref.py:34-35`:
  `find_string_offsets()` rejects a match when `start == j`, i.e. whenever the needle starts at
  the beginning of the NUL-terminated string — which is the normal case. Workaround used here:
  locate the literal with a raw `blob.find()` and pass `--va`. Not fixed (out of scope for this
  task) → follow-up.
- **The driver's DT property-name table is visible in .rodata** at file
  `0x1898e1b…0x1899333`, and it covers nearly every property on the shipped DT node:
  `share_sysmmu, axid_mask, mfc_fault_num, nal_q_entry_size, nal_q_dump_size, skype, black_bar,
  color_aspect_dec, static_info_dec, color_aspect_enc, static_info_enc, hdr10_plus,
  P010_decoding, support_10bit, support_422, support_rgb, max_hdr_win, enc_param_num,
  enc_param_addr, enc_param_val, bw_enc_{h264,hevc,hevc_10bit,vp8,vp9,vp9_10bit,mpeg4},
  bw_dec_{h264,hevc,hevc_10bit,vp8,vp9,vp9_10bit,mpeg4}, num_qos_steps, max_qos_steps, max_mb,
  mfc_freq_control, mo_control, bw_control, mfc_qos_variant_%d, mfc_perf_boost_table,
  num_cluster, freq_mfc, freq_int, freq_mif, freq_cluster, qos_weight_* (11 of them),
  num_mfc_freq, mfc_freqs, max_Kbps, thrd_mb, mo_value, mo_10bit_value, mo_uhd_enc60_value,
  time_fw` — plus child-node lookups `iommu` ("failed to get iommu node"), `hwfc`
  ("failed to iomap hwfc address region"), `mmcache` ("failed to iomap mmcache address region")
  and `cmu` ("failed to iomap busc address region").
  **Notably absent: `ip_ver`.** A raw search for `b'ip_ver\x00'` hits only file 0x17a6262, which
  is inside the display driver's `chip_ver` literal (0x17a6260) — so the shipped MFC driver does
  not read `ip_ver` by that name, even though the DT supplies `ip_ver = <0x1300>`.
  Three properties on `/mfc@188D0000` have **no** literal in this cluster: `ip_ver`,
  `debug_mode` and `nal_q` (the latter two do exist elsewhere in .rodata — `debug_mode\0` at
  0x17ea46d/0x18b15ab/0x198d2ef, `nal_q\0` from 0x18a3ce9 onward — so they may be read on a
  different code path; **not established**). Also absent as standalone literals:
  `mfc_qos_table` and `mfc_qos_platdata` (the latter appears
  only inside the message `3%s: could not find mfc_qos_platdata node` at 0x18991c1) — so which
  child name the QoS lookup uses is **not established**; the DT child is `mfc_qos_table`.

**7. Firmware blob — present, named `mfc_fw.bin`, 687 912 bytes (names/sizes only).**
- `debugfs -R 'ls -l /firmware' dumps/hhxj6/vendor/vendor.raw` → inode **673**, mode `100644`,
  owner `0:2000`, size **687 912** bytes, name **`mfc_fw.bin`**.
  Path note: `vendor.raw` is the ext4 of the **vendor partition itself**, so the in-image path is
  `/firmware/mfc_fw.bin`; the mounted path on the device is `/vendor/firmware/mfc_fw.bin`
  (the `ls` of the image root shows `bin, eden, etc, firmware, lib, lib64, odm, odm_dlkm,
  openvx, overlay, recovery-from-boot.p, saiv, tee, ueventd.rc, vendor_dlkm` — i.e. the root
  **is** `/vendor`). `debugfs -R 'ls -l /vendor/firmware'` fails with "File not found by
  ext2_lookup" — recorded as a dead end so the next pass does not retry it.
- The kernel asks for exactly that name: `mfc_load_firmware` / `mfc_alloc_firmware` /
  `mfc_release_firmware` sit next to the literal `mfc_fw.bin` in .rodata (file offsets
  0x18a5dff, 0x18a5e30), with the surrounding prints
  `[F/W] Requesting F/W`, `[F/W] Couldn't find the F/W invalid path`,
  `[MEMINFO][F/W] loaded F/W Size: %zu`, `[MEMINFO][F/W] MFC firmware(%zu) is too big to be
  loaded in memory(%zu)`, and separate **normal** and **DRM** firmware buffers
  (`[F/W] Allocating normal firmware buffer failed`, `[F/W] Allocating DRM firmware buffer
  failed`, `copy firmware to secure region`). So the blob is loaded into a device buffer, not
  executed by the AP.
- Version check exists in the driver but its constants are not readable from strings:
  `6%s:%d: [F/W] MFC v%x.%x, %02xyy %02xmm %02xdd (%c)` and
  `3%s:%d: Invalid F/W version(0x%x) for MFC H/W(0x%x)` (strings lines 147680-147681).
  **The version inside the blob was NOT read** — extracting it means copying the blob, which is
  out of bounds for this task. Left open.
- No other **video-codec** blob in `/firmware`: the rest are audio DSP (`calliope_*`, `AP*_AUDIO_SLSI.bin`,
  `dax_param.bin`, `epx.bin`, `rxse*.bin`, `txse*.bin`, `vts.bin`, `fmsb.bin`), camera ISP
  (`fimc_is_{fw_mcu,lib,rta}.bin`, `setfile_*.bin`), NPU (`NPU.bin`), IVA
  (`iva30_rt-makalu.bin`), Wi-Fi/BT (`bcmdhd_*`, `bcm4375*`, `nvram.txt_*`), NFC
  (`/firmware/nfc/sec_s3nrn82_firmware.bin`), audio amps (`cs35l40-*`, `cs47l93-*`).
- **Not to be confused with the codec:** the wireless-charger driver also loads an "MFC"
  firmware — `mfc/mfc_fw_flash.bin`, `mfc/mfc_fw_flash_lsi.bin`, `/sdcard/mfc_fw_flash.bin`,
  `/spu/mfc/mfc_fw_flash_p9320.bin`, `mfc_get_firmware_version`, `mfc_firmware_verify`,
  `mfc_wpc_fw_update_work` (strings lines 165394-165743). Those belong to the IDT/Qi
  `mfc-charger` driver (`%s: MFC p9321 Charger Driver Loading`), not to the video block.

**7b. Userspace side (vendor) — the codec is wired up and shipped.**
- `/lib/omx` and `/lib64/omx` (debugfs `ls -l`) contain the Samsung OMX components:
  `libOMX.Exynos.{AVC,HEVC,MPEG4,VP8,VP9}.{Decoder,Encoder}.so`, plus
  `libOMX.Exynos.{AVC,HEVC}.WFD.Encoder.so` and `libOMX.Exynos.WMV.Decoder.so`
  (13 components per arch, ~180-283 KB each), alongside `/lib{,64}/libExynosOMX_Core.so` and
  `libExynosOMX_Resourcemanager.so`.
- `/etc/media_codecs.xml` (18 447 B, read with `debugfs -R 'cat /etc/media_codecs.xml'`) lists
  `OMX.Exynos.{mpeg4,h263,avc,vp8,hevc,vp9}.dec`, `OMX.Exynos.{avc,hevc,vp9}.dec.secure` and
  `OMX.Exynos.{AVC,H263,MPEG4,VP8,HEVC,VP9}.Encoder` — **no `enabled="false"` attribute anywhere
  in that file** (`grep -icE 'enabled'` → 0).
- `/etc/media_codecs_wfd.xml` (4 920 B) adds the WFD encoders
  (`OMX.Exynos.{AVC,HEVC}.WFD.Encoder`, `.secure` variants) — matching the DT's
  `port-name = "MFC port 1, WFD"` on `/sysmmu@188B0000`.

**8. Other dump facts bearing on usability.**
- **MFC generation:** the DT carries `ip_ver = <0x1300>` on `/mfc@188D0000` (raw `00001300`) —
  the only version-ish datum in the DT. The compatible is the bare `samsung,exynos-mfc` with **no
  generation suffix and no fallback list**, so the DT does not name an MFC core revision (e.g.
  nothing like `samsung,exynos5433-mfc`). Reading `0x1300` as "MFC v1.3" is **inference**; the
  driver's own version print is `[F/W] MFC v%x.%x` (see 7).
- Feature flags the DT turns on for this board (raw cells, `0x1` = on):
  `P010_decoding = <0x1>` (10-bit decode), `support_10bit = <0x1>`, `support_422 = <0x1>`,
  `support_rgb = <0x0>` (**off**), `max_hdr_win = <0x1>`, `hdr10_plus = <0x1 0x180709>`
  (the 2nd cell reads like a datecode 2018-07-09 — inference), `static_info_enc = <0x1 0x180314>`,
  `enc_ts_delta = <0x1 0x211208>`, `nal_q = <0x1 0x0>` (NAL queue on),
  `skype = <0x1 0x0>`, `black_bar = <0x1 0x0>`, `color_aspect_{dec,enc} = <0x1 0x0>`,
  `debug_mode = <0x1>`, `share_sysmmu = <0x0>`, `mfc_fault_num = <0x0>` (the last two are
  4-byte cell properties holding zero — raw `repr` of the DTB value is `b'\x00\x00\x00\x00'`,
  they are NOT zero-length), `mfc_freq_control = <0x1>`,
  `mo_control = <0x1>`, `bw_control = <0x1>` (DVFS + memory-overlap + bandwidth QoS all enabled).
- **Max capability datum:** `max_mb = <0x4982fc>` = 4 817 660. **Meaning and unit are not
  established by the dump** — it is not the macroblock count of any single standard resolution
  (8K 8192×4608 = 491 520 MB, 4K 4096×2304 = 36 864 MB; neither matches), so it reads more like a
  QoS/throughput bound than a resolution ceiling. **Do not use as a constant**; closing it needs
  the driver's `max_mb` consumer (a mirror read, not a dump read).
- **Clock ladder:** `num_mfc_freq = <0x5>`, `mfc_freqs = <0x20b70 0x412f8 0x61a80 0x825f0
  0xa2990>` = 134 000 / 267 000 / 400 000 / 534 000 / 666 000 (unit unstated; kHz is inference),
  `num_qos_steps = <0x9>`, `max_qos_steps = <0xa>`, and 10 `mfc_qos_variant_0..9` children under
  `/mfc@188D0000/mfc_qos_table` each with `thrd_mb, freq_mfc, freq_int, freq_mif, mo_value,
  mo_10bit_value, mo_uhd_enc60_value, time_fw`. Plus `/mfc@188D0000/mfc_perf_boost_table`
  (`num_cluster = <0x3>`, `freq_cluster = <0x1a94b0 0x1cf610 0x163780>`, `freq_mfc = <0xa2990>`,
  `freq_int = <0x825f0>`, `freq_mif = <0x1fefc8>`). All three of `mfc_perf_boost_table`,
  `freq_mfc/freq_int/freq_mif` and `num_cluster` are read by the driver (literals at Image file 0x1899042-
  0x18990ba) — so the ladder is live, not dead DT.
- **Kernel-side names that must match the DT** (strings, in the CMU/power-domain tables of the
  Image): `pd-mfc` (line 120347), `dvfs_mfc` (118839), `SYSMMU_MFC0` / `SYSMMU_MFC1`
  (120375-6), `blkpwr_mfc` (115706), `aclk_mfc` (147729, right after `mfc_pm_power_on`; raw file
  offset 0x18a26ca) — the DT supplies exactly `clock-names = "aclk_mfc"` and
  `/pd-mfc@15862600`, so the names line up.
- **CMU MFC bank SFR names in the Image** (from the cal/SFR debug table, strings lines
  114460-118560): `UMUX_CLKCMU_MFC_MFC`, `UMUX_CLKCMU_MFC_WFD`, `GATE_MFC`,
  `GATE_SYSMMU_MFCD0`, `GATE_SYSMMU_MFCD1`, `GATE_CLKCMU_MFC_MFC`, `GATE_CLKCMU_MFC_WFD`,
  `CLK_CON_DIV_DIV_CLK_MFC_BUSP`, `PLL_CON0_MUX_CLKCMU_{MFC_WFD,MFC_MFC}_USER`,
  `MFC_CMU_MFC_CONTROLLER_OPTION`, `CTRL_OPTION_CMU_MFC`, `CLKOUT_CON_BLK_MFC_CMU_MFC_CLKOUT{0,1}`,
  `SYSREG_MFC_BUS_COMPONENT_DRCG_EN`, `SYSREG_MFC_MEMCLK`, `OSCCLK_MFC`, `VCLK_BLK_MFC`,
  plus `CLK_BLK_MFC_UID_MFC_CMU_MFC_IPCLKPORT_PCLK` and ~50 `GOUT_BLK_MFC_UID_*` entries
  (SYSMMU_MFCD0/1, PPMU_MFCD0/1/2, BTM_MFCD0/1, RSTNSYNC_CLK_MFC_{BUSP,BUSD_MFC,BUSD_WFD},
  LH_ATB_MFC, XIU_D_MFC, VGEN_MFC, D_TZPC_MFC, AS_APB(_S/_NS)_SYSMMU_{S,NS}_MFCD{0,1}, WFD).
  These name the **`BLK_MFC` clock domain and a `CMU_MFC` bank** — they are the strings to match
  against when resolving clock index 422, but the index↔name mapping is **not** in the DT and is
  not established here.
- **Debug surface the driver exposes** (strings 148238-148282): `mfc_init_debugfs`,
  `>> MFC device information(common)`, `ex) echo 0xff > /d/mfc/sfr_dump (all dump mode)`,
  `ex) echo 7 > /d/mfc/perf_boost_mode (max freq)`, `1   (1 << 0): DVFS (INT/MFC/MIF)` —
  i.e. a live debugfs node `/d/mfc/` on the running Android build.
- **Neighbouring blocks in the same MMIO island** (base.dtb, all `status` absent):
  `/tsmux@188E0000` (`samsung,exynos-tsmux`, reg 0x188E0000/0x1000, `interrupts = <0 0x153 0>`
  → SPI 339 / GSIV 371, `iommus = <0x48>` — shares MFC's second SYSMMU),
  `/smfc@18B00000` (`compatible = "samsung,exynos8890-jpeg"`, `dma-coherent;`,
  reg 0x18B00000/0x1000, `interrupts = <0 0x10b 0>` → SPI 267 / GSIV 299,
  `clocks = <0x2 0x143>` (index 323), `iommus = <0x105>` → **`/sysmmu@18B90000`**,
  `smfc,int_qos_minlock = <0x825f0>`),
  `/scaler@18B10000` (`samsung,exynos5-scaler`), `/jsqz@18B30000` (`samsung,exynos-jsqz`).
  The kernel has the SMFC driver too
  (`drivers/media/platform/exynos/smfc/smfc.c`, `exynos_smfc_probe`,
  `Still MFC %02x.%02x.%04x`).
- **ION/heaps the codec uses** (base.dtb, `compatible = "exynos9820-ion"`):
  `/ion-hpa-heap/ion_video_fw` (`ion,secure;`, `ion,protection_id = <0x2>`,
  `ion,alignment = <0x10000>`), `/ion-hpa-heap/ion_video_nfw` (not secure,
  `ion,protection_id = <0x7>`), `/ion-hpa-heap/ion_video_frame` (`ion,secure;`,
  `ion,heapname = "vframe_heap"`, `protection_id = <0x5>`),
  `/ion-hpa-heap/ion_video_scaler` (`ion,secure;`, `protection_id = <0x6>`),
  `/reserved-memory/ion_video_stream` (`size = <0x4800000>` = 72 MiB,
  `alloc-ranges = <0x0 0x40000000 0xb0000000>`, `alignment = <0 0x400000>`, `ion,secure;`,
  `ion,reusable;`, `protection_id = <0x3>`). The driver's memory paths match:
  `mfc_mem_ion_alloc`, `not supported mfc mem type: %d, heapname: %s`, `mfc_get_iovmm`.
  **No `memory-region` property on `/mfc@188D0000`** — the codec gets its buffers via ION heaps,
  not via a reserved-memory phandle.

**9. `dt.img` and `cm.bin`.**
- `dumps/hhxj6/dt.img` (8 388 608 B) → `fdt.split_blobs()` yields **1** FDT blob of 239 680 B,
  md5 **4c53ff438bc5bbdc87ccda6fb65c9bd3** — **byte-identical to `dumps/hhxj6/base.dtb`**
  (`md5sum dumps/hhxj6/base.dtb` = same). So `dt.img` is just the base DTB in a Samsung
  container; it adds nothing about the codec.
- `dumps/hhxj6/cm.bin` (7 340 032 B) → `fdt.split_blobs()` finds **0** FDT blobs. Not a DT.

**Dead ends from this pass (exact queries, so they are not repeated):**
- `debugfs -R 'ls -l /vendor/firmware' dumps/hhxj6/vendor/vendor.raw` →
  `/vendor/firmware: File not found by ext2_lookup`. The ext4 root **is** `/vendor`; use
  `/firmware`.
- `debugfs -R 'ls -l /lib/modules'` and `'ls -l /lib64/modules'` → both "File not found by
  ext2_lookup". `/vendor_dlkm` and `/odm_dlkm` hold only `etc/`.
- `grep -iE 'mfc|s5p|codec' dumps/hhxj6/symbols.tsv` → **zero lines**. All 894 rows have an
  empty name column (`sed -n '1,12p' dumps/hhxj6/symbols.tsv | cat -A` → lines end `^I$`).
- `strings -a dumps/hhxj6/kernel | grep -E '\.ko$'` → one line, and it is an error message
  (`4skbuff: %s: attempt to compute crc32c without libcrc32c.ko`), not a module.
- `xref.py dumps/hhxj6/kernel --string 'samsung,exynos-mfc'` → prints nothing, exits
  `give --string or --va`. Tool bug in `find_string_offsets()` (`xref.py:34-35`, rejects
  `start == j`). Use a raw `blob.find()` + `--va` instead.
- Raw search for little-endian 8-byte absolute pointers to the string VAs
  (`struct.pack("<Q", 0xFFFFFF8009271D70)` etc.) → **0 hits** for every one of
  `samsung,exynos-mfc`, `s5p-mfc`, `mfc_fw.bin`, `aclk_mfc`. The Image is relocatable, so
  pointer-table scans do not work; only ADRP-relative xrefs do.
- `grep -cF 'ip_ver' /tmp/opencode/video-mfc/kernel.strings.txt` → 2, but both are the display
  driver's `chip_ver` (substring collision). Raw `blob.find(b'ip_ver\x00')` → single hit at
  0x17a6262, inside `chip_ver\0` at 0x17a6260. So: **no `ip_ver` read in the shipped MFC driver.**
- `blob.find(b'mfc_qos_table\x00')` and `blob.find(b'mfc_qos_platdata\x00')` → **0 hits** each.
- `grep -inE 'm2m|s5p' over base.dtb compatibles` → no `m2m`/`s5p` compatible anywhere in the
  base DTB; the only `s5p*` compatibles in the Image are unrelated
  (`samsung,s5pv210-uart`, `samsung,s5pv210-keypad`, `s5p-smem`, `samsung,s5p6440-pwm`).
- `strings -a dumps/hhxj6/dtbo.img | grep -i mfc` → only the Qi-charger strings (see 1b).

### 2026-10-13 — T2: the `ianmacd/d2s` tree — a whole Exynos 9820 vendor kernel, and the Windows contract (fact)

Clone: `git clone https://github.com/ianmacd/d2s` → **`/tmp/opencode/d2s`, kept outside the repo**
(2.1 GB; nothing from it is committed). HEAD `d1d4fb4aa` (2020-11-19, "Update custom kernel config
in line with changes to DTJ4 defconfig").

**What the tree is** (the pointer that opened D10 turned out to be much larger than a driver
folder):

- `Makefile:3-5` → **Linux 4.14.113**; `arch/arm64/configs/` has `exynos9820-d1/-d2s/-d2x_defconfig`
  (+ `_caliban`); `README_Kernel-n970f.txt:20` builds with `make exynos9820-d1_defconfig`;
  `build_n970f`/`build_n975f`/`build_n976b` → **Note 10 family on Exynos 9820/9825**.
- So: a complete **Samsung Exynos 9820 vendor kernel source** — same SoC as beyond0lte, different
  product, older release (DTJ4/2020 vs our HHXJ6/Android 12).
- Licence: `Makefile:1` `SPDX-License-Identifier: GPL-2.0`; the MFC files carry GPL-2.0 headers
  (e.g. `mfc_hwlock.c:9-13`) → **reference-only**, same posture as the other kernel mirrors.

**The MFC payload** (mirror, rung 3 — and it agrees with the shipped dump point by point):

- `arch/arm64/boot/dts/exynos/exynos9820-mfc.dtsi:20-27` — `mfc_0: mfc@188D0000`,
  `compatible = "samsung,exynos-mfc"`, `reg = <0x0 0x188D0000 0x10000>`,
  `interrupts = <0 INTREQ__MFC 0>`, `clocks = <&clock GATE_MFC>`,
  `iommus = <&sysmmu_mfc0>, <&sysmmu_mfc1>`; sub-windows `iommu 0x18890000/0x9000 +
  0x188B0000/0x9000`, `hwfc 0x18A28000/0x100`, `mmcache 0x1A300000/0x100`, five `cmu` banks
  (`:123-139`). **Every one of those matches the `base.dtb` read in T1** — including
  `ip_ver = <0x1300>` (`:30`) and the clock id: `include/dt-bindings/clock/exynos9820.h:300-306`
  gives `CLK_MFC_BASE = 420`, `GATE_MFC = 422`, and the DT cell in T1 is `0x1a6` = **422**.
- `include/dt-bindings/interrupt-controller/exynos9820.h:352` — `INTREQ__MFC = 338`, matching the
  DT cell `0x152` = 338 read in T1.
- Driver: `drivers/media/platform/exynos/mfc/` (57 files), `CONFIG_VIDEO_EXYNOS_MFC=y`
  (`exynos9820-d2s_defconfig:3077`); mainline-style `s5p-mfc` present but **off**
  (`# CONFIG_VIDEO_SAMSUNG_S5P_MFC is not set`, `:3243`); `CONFIG_VIDEO_EXYNOS_SMFC=y` (`:3246`).
- Firmware: `mfc_common.h:68` `#define MFC_FW_NAME "mfc_fw.bin"`, loaded by `request_firmware`
  (`mfc_buf.c:580`); the driver reserves `PAGE_ALIGN(0x100000)` = 1 MiB for it (`mfc.c:1778`).
  **The blob is in our dump**: `debugfs -R "stat /firmware/mfc_fw.bin" dumps/hhxj6/vendor/vendor.raw`
  → inode 673, **Size 687912** (name+size only — never copied, never committed).
  `firmware/mfc/*.ihex` in the tree are *not* the 9820's (`mfc_fw_flash*` for lsi/s2miw04-era parts).
- **Bring-up order** (`mfc_run.c:30-101`, `__mfc_init_hw`): `mfc_pm_clock_on` (`mfc_pm.c:39`) →
  `mfc_reset_mfc` (reg `MFC_REG_MFC_RESET = 0x1070`, `mfc_regs.h:30`) →
  `mfc_set_risc_base_addr` (`MFC_REG_RISC_BASE_ADDRESS = 0x0054`, `mfc_regs.h:27`) →
  `mfc_risc_on` (`mfc_hw_reg_api.h:93`; `MFC_REG_RISC_ON = 0x0000`, `mfc_regs.h:24`) → wait
  `MFC_REG_R2H_CMD_FW_STATUS_RET = 12` (`mfc_regs.h:526`) → `mfc_cmd_sys_init` → wait
  `MFC_REG_R2H_CMD_SYS_INIT_RET = 1` (`mfc_regs.h:516`) → `mfc_get_fimv_info()` ('D'/'E'/'N').
- **The identity handshake is a register read**: `mfc_run.c:110-116` compares
  `mfc_get_mfc_version()` — `MFC_READL(MFC_REG_MFC_VERSION)`, `MFC_REG_MFC_VERSION = 0xF028`,
  full 32-bit mask (`mfc_reg_api.h:50`, `mfc_regs.h:51,583`) — against `ip_ver`, and refuses to run
  on a mismatch. Version halves: `mfc_common.h:198-199` (`ip_ver >> 8`, `& 0xFF`) → **v13.0** for
  `0x1300`.
- **The SFR is readable with the clock off**: `mfc_pm.c:48-52` — "The MFC and TZPC SFR are in APB
  clock domain and it is accessible through Q-CH even if clock off". That makes a zero-write
  version read in UEFI possible, exactly like the D9 mailbox pre-read. TZPC "IP Protection" is part
  of the clock-on sequence (`mfc_pm.c:52-55`) — noted, not yet understood (P6).
- `mfc_hwlock.c` is an **AP-side software lock** (spinlock + waiting list, `mfc_init_hwlock:35-49`),
  not a hardware share with another processor: no evidence in this tree of the CP or a secure world
  owning the MFC.

### 2026-10-13 — T3: the Windows contract for a non-GPU codec — U6 CLOSED (fact, cited)

Windows has **two documented shapes** for a hardware codec that is not part of the display
adapter. Neither requires a WDDM driver — which is what makes D10 a project rather than a D5b
dependency.

1. **AVStream minidriver presenting KS codec filters** (the class-driver route):
   - "Starting in Windows 7, Windows Media Foundation represents AVStream-based media components as
     user-mode Media Foundation Transforms (MFTs). By using this feature, vendors can present
     **hardware-based decoders, encoders, and video processors as MFTs**"; the minidriver must
     "expose transform functions as **individual KS filters**" (decoder / encoder / video processor
     as three distinct filters, decoder output "must include NV12") and register them under
     `KSMFT_CATEGORY_VIDEO_DECODER` / `KSMFT_CATEGORY_OTHER`, after which `MFTEnumEx` sees them —
     `learn.microsoft.com/en-us/windows-hardware/drivers/stream/getting-started-with-hardware-codec-support-in-avstream`
   - "If you are writing a new minidriver (either for a standalone encoder or an integrated one),
     Microsoft recommends following the **AVStream** architecture because the stream class is
     obsolete"; and there is a starting point: "You may use the **AVStream Simulated Hardware
     Sample Driver (Avshws)** … a pin-centric AVStream example that implements support for DMA
     transfers" — `learn.microsoft.com/en-us/windows-hardware/drivers/stream/encoder-devices`
   - INF recipe: `include=ks.inf,kscaptur.inf`, `AddInterface=%KSCATEGORY_ENCODER%`
     (`{19689BF6-C384-48FD-AD51-90E58C79F70B}`), KsProxy CLSID
     `17CCA71B-ECD7-11D0-B908-00A0C9223196`, and one of `CLSID_IVideoEncoderProxy`
     `{B43C4EEC-8C32-4791-9102-508ADA5EE8E7}` / `CLSID_ICodecAPIProxy` `{7FF0997A-…}` /
     `CLSID_IVideoEncoderCodecAPIProxy` `{B05DABD9-…}` —
     `learn.microsoft.com/en-us/windows-hardware/drivers/stream/encoder-installation-and-registration`;
     a dedicated install page exists ("Installing an AVStream-based Hardware Codec Driver").
2. **Custom kernel driver + proxy MFT** (the Quick-Sync-style route): "Any hardware codec that is
   not based on AVStream must provide its own MFT to act as a proxy to the driver"; encoder /
   decoder / scaling must be **separate** MFTs ("A hardware MFT should never act as a multi-purpose
   transcoder"); hardware MFTs "must use the … asynchronous processing model" and "must support
   dynamic format changes", and must set `MF_TRANSFORM_ASYNC=TRUE`,
   `MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE=TRUE` and `MFT_ENUM_HARDWARE_URL_Attribute` = "the symbolic
   link for the hardware device"; a paired decoder+encoder on one chip is matched by offering a
   **custom media type at `dwTypeIndex = 0`** plus a standard format (NV12) after it —
   `learn.microsoft.com/en-us/windows/win32/medfound/hardware-mfts`.
3. **What is closed to us**: the D3D11/DXVA path. "The decoder device is the video accelerator, and
   is **implemented by the graphics driver**"
   (`learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation`)
   and D3D11 video is "Mandatory for all **WDDM 1.2** drivers"
   (`learn.microsoft.com/en-us/windows-hardware/drivers/display/d3d11-video-playback-improvements`).
   With no Mali WDDM (the D5 verdict), DXVA is out — which is precisely why shape (1)/(2) matter.

### 2026-10-13 — T4: prior art — U7 (fact + a dated negative)

- **Mainline Linux does not support this MFC generation.** The mainline binding
  `Documentation/devicetree/bindings/media/s5p-mfc.txt` lists only `samsung,mfc-v5` (Exynos4),
  `-v6` (Exynos5), `-v7` (Exynos5420), `-v8` (Exynos5800), `samsung,exynos5433-mfc` (v8) and
  `-v10` (Exynos7880). The 9820's node is `samsung,exynos-mfc` with `ip_ver = <0x1300>` →
  **no mainline match**. Mainline's driver is a different one anyway
  (`drivers/media/platform/samsung/s5p-mfc/`, confirmed present on `torvalds/linux` master via
  `s5p_mfc_dec.c`).
- The vendor driver in the clone is **not** mainline's: different directory
  (`drivers/media/platform/exynos/mfc`), different file names, and the 9820 defconfig explicitly
  turns mainline's off.
- Dead end (do not repeat): a web search for "Samsung MFC Windows driver" is useless — **MFC
  collides with Microsoft Foundation Classes**. The Update Catalog chip-by-chip scan was not
  attempted because `samsung,exynos-mfc` is a vendor-internal compatible that never becomes a
  hardware ID. Reopens if a Windows-on-Exynos effort ever publishes one.

### 2026-10-13 — T5: cross-cutting — the SPI-vs-GSIV rule proven twice, and a D4c defect it exposes

The repo's records use two conventions. `notes/modem-wwan.md:737` and `notes/m12-wifi-chip.md:78,81`
add 32 (modem 375→407, WiFi 253→285); `notes/bt-driver.md:106,187` and `Dsdt.asl:385` treat the BT
UART's DT cell `0x198` = 408 as already the GSIV and put **408** in the DSDT.

Two independent reads settle it — **the DT cell is the SPI number, GSIV = SPI + 32**:

1. The GIC driver that actually runs on this SoC family: `drivers/irqchip/irq-gic.c:1019-1026`
   (in the clone) — `*hwirq = fwspec->param[1] + 16;` then `if (!fwspec->param[0]) *hwirq += 16;`
   for type 0 = SPI.
2. Mainline's binding, `Documentation/devicetree/bindings/interrupt-controller/arm,gic.yaml`
   ("The 2nd cell contains the interrupt number **for the interrupt type**. SPI interrupts are in
   the range [0-987]"), fetched 2026-10-13.

Consequences:

- **MFC: SPI 338 → GSIV 370** (0x172) — as T1 stated.
- **D4c defect**: the BT UART's DT cell is 408, so its GSIV is **440**, and `Dsdt.asl` carried
  **408** with a comment calling it VERIFIED — 32 off, in a device the driver stream is building
  against.
- **Fixed** in its own follow-up commit: `Dsdt.asl` BTH0 now carries **440** (verified in the
  built AML: `iasl -d Dsdt.aml` shows `0x000001B8` in the BTH0 `_CRS`), and
  `EXYNOS_UART_BT_GSIV` in `Exynos9820.h` is 440 with the reasoning beside it. The whole header
  was audited mechanically against base.dtb + all 7 overlays: for every other GSIV define,
  `value − 32` resolves to the node the name claims (dwc3 241, pcie0 253, hsi2c 388, ALIVE keys
  8/9/11/25/23, modem 375/68/67/66 in dtbo0, mailbox 41, TMU 478/479); **only the BT one resolved
  to nothing at −32 while itself being a live SPI cell**. Two more citation errors surfaced in the
  same audit and are corrected in `notes/bt-driver.md`: PCIe0's DT cell is `0xfd` (253), not
  `0x11d`, and the UFS0 DSDT comment's "real UFS line GSIV 274 / SPI 242" is wrong — no node owns
  SPI 242; the UFS host line is SPI 249 / GSIV 281 (the parked 259 stays, it is deliberate).

### What is NOT yet known (each needs a source, not a guess)

- U1: **CLOSED by T1.** (Repo rule 7 still applies: this is a firmware-side fact, not device
  validation — the node is not `Ready` until V01 runs on the phone.)
  Node **`/mfc@188D0000`**, `compatible = "samsung,exynos-mfc"` (single string, no generation
  suffix), **no `status` property at all** — present in `dumps/hhxj6/base.dtb` (FDT v40, 1117
  nodes) and **not touched by any of the 7 `dtbo.img` overlays** — no codec symbol occurs in any overlay's
   `__fixups__` (T1 §1, §1b, **§1c**). Alias
  `mfc0 = "/mfc@188D0000"` at `dumps/hhxj6/base.dts:3957`. The only version datum is
  `ip_ver = <0x1300>` (raw `b'\x00\x00\x13\x00'`); reading that as "MFC v1.3" is inference.
- U2: **CLOSED for the DT side by T1** (§2-§5). `reg = <0x0 0x188d0000 0x10000>` → 0x188D0000,
  64 KiB (root bus `#address-cells=2`, `#size-cells=1`); `interrupts = <0x0 0x152 0x0>` →
  **SPI 338 / GSIV 370** (parent = root `interrupt-parent = <1>` →
  `/interrupt-controller@10100000`, `#interrupt-cells = <3>`); `clocks = <0x2 0x1a6>` +
  `clock-names = "aclk_mfc"` → provider `/clock-controller@0x1a240000`
  (`samsung,exynos9820-clock`, `#clock-cells = <1>`), **clock index 422**;
  `iommus = <0x47 0x48>` → `/sysmmu@18890000` + `/sysmmu@188B0000` (both
  `samsung,exynos-sysmmu`, `#iommu-cells = <0>`, `samsung,power-domain = <0x6>` →
  `/pd-mfc@15862600` reg 0x15862600/0x20, `cal_id = <0xb138000c>`, `status = "okay"`).
  Child windows: `iommu` 0x18890000+0x9000 / 0x188B0000+0x9000, `hwfc` 0x18A28000+0x100,
  `mmcache` 0x1A300000+0x100, `cmu` 0x1A200000/0x1BC00000/0x1BD00000/0x1BE00000/0x1BF00000
  each +0x8000. **Still open:** what clock gate index 422 *is* in the CMU MFC bank — the DT does
  not name it; needs `soc/samsung/cal-if/exynos9820/cmucal-*.h` in a mirror (the Image's SFR
  name table gives candidate names — `GATE_CLKCMU_MFC_MFC`, `GATE_MFC` — but no index, T1 §8).
- U3: **dump side CLOSED by T1 (§6); the mirror defconfig read is still open.** The driver is
  Samsung's downstream `drivers/media/platform/exynos/mfc/` tree (source-path strings
  `mfc_mem.h`, `mfc_hwlock.c`, `mfc_watchdog.c`, `mfc_sync.c` in the Image), matching on
  `samsung,exynos-mfc` (Image file 0x1271d70) and registering `s5p-mfc` / `s5p-mfc-{dec,enc}` /
  `s5p-mfc-{dec,enc}-secure` / `s5p-mfc-enc-otf{,-secure}`. **Built in (`y`), not a module**:
  the code is in `dumps/hhxj6/kernel`, `xref.py --va 0xFFFFFF80098A5E30` finds an ADRP xref at
  file 0xaa3968, and the vendor image contains **no `.ko` at all** (`/lib/modules`,
  `/lib64/modules` absent; `/vendor_dlkm` and `/odm_dlkm` hold only `etc/`).
  `kallsyms.log`/`symbols.tsv` are a dead end (no kallsyms in this kernel).
- U4: **CLOSED by T1 (§7).** Yes — **`mfc_fw.bin`, 687 912 bytes**, inode 673, mode 100644, in
  `dumps/hhxj6/vendor/vendor.raw` at in-image path `/firmware/mfc_fw.bin` (= `/vendor/firmware/`
  on the device), read with `debugfs -R 'ls -l /firmware'`. The kernel asks for that exact name
  (literal at Image file 0x18a5e30, next to `mfc_load_firmware`). **Not extracted** — the blob's
  internal version is still unknown.
- U5: **CLOSED by T2.** `ianmacd/d2s` is a complete **Exynos 9820 vendor kernel**, Linux 4.14.113,
  Note 10 family (d1/d2s/d2x), GPL-2.0, HEAD `d1d4fb4aa` (2020). Its MFC driver is the same
  generation as the 9820's — the dtsi values match our shipped DTB cell for cell, including
  `ip_ver = <0x1300>` and clock id 422.
- U6: **CLOSED by T3.** Two sanctioned shapes for a non-GPU hardware codec: an **AVStream
  minidriver** exposing decoder/encoder/video-processor KS filters (Media Foundation then proxies
  them as MFTs — no custom MFT needed), or a **custom kernel driver + proxy hardware MFT**. The
  D3D11/DXVA route is closed to us because it is implemented by the graphics driver (WDDM), which
  D5 established does not exist for Mali.
- U7: **CLOSED by T4.** Mainline `s5p-mfc` covers v5-v8 + exynos5433 + v10 only — **not** this
  block; no Windows driver for a Samsung MFC exists in any prior-art source checked.
- U8: **DT half CLOSED by T1 (§5); the `Exynos9820.h` / DSDT half is still open.** The SYSMMUs
  are fully described in the shipped base DTB: `/sysmmu@18890000` (`port-name = "MFC port 0"`,
  `interrupts = <0 0x157 0  0 0x158 0>` → SPI 343/344, `sysmmu,secure-irq;`,
  `sysmmu,secure_base = <0x188a0000>`, `clocks = <0x2 0x1a7>`) and `/sysmmu@188B0000`
  (`port-name = "MFC port 1, WFD"`, SPI 345/346, `secure_base = <0x188c0000>`,
  `clocks = <0x2 0x1a8>`), grouped by `/iommu-domain_mfc`
  (`samsung,exynos-iommu-bus`, `domain-clients = <0x100 0x101>` → `/mfc@188D0000`,
  `/tsmux@188E0000`). Whether those windows are already claimed/described in
  `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` or the DSDT is **not** checked
  in this pass.

### Constants frozen (all VERIFIED unless marked)

| Constant | Value | Status | Source |
|---|---|---|---|
| MFC DT node | `/mfc@188D0000`, `compatible = "samsung,exynos-mfc"`, no `status` | VERIFIED (base.dtb) | T1 §1 |
| MFC register window | `0x188D0000`, size `0x10000` | VERIFIED (raw cells) | T1 §2 |
| MFC interrupt | `interrupts = <0 0x152 0>` → SPI 338, GSIV **370** | VERIFIED cells; +32 rule is GIC arch | T1 §3 |
| MFC clock | provider `/clock-controller@0x1a240000`, index **0x1a6 (422)**, name `aclk_mfc` | VERIFIED cells; gate *name* unknown | T1 §4 |
| MFC SYSMMUs | `/sysmmu@18890000` (0x18890000/0x9000, SPI 343+344), `/sysmmu@188B0000` (0x188B0000/0x9000, SPI 345+346) | VERIFIED | T1 §5 |
| MFC power domain | `/pd-mfc@15862600`, reg `0x15862600`/0x20, `cal_id = <0xb138000c>`, `need_smc = <0x18820204>`, `status = "okay"` | VERIFIED | T1 §5 |
| HWFC window | `0x18A28000`, size `0x100` | VERIFIED | T1 §2 |
| MMCACHE window | `0x1A300000`, size `0x100` | VERIFIED | T1 §2 |
| CMU windows named by the MFC node | `0x1A200000`, `0x1BC00000`, `0x1BD00000`, `0x1BE00000`, `0x1BF00000` (each 0x8000) | VERIFIED as cells; which one is CMU_MFC is **UNVERIFIED** | T1 §2 |
| MFC freq ladder | `mfc_freqs = 134000/267000/400000/534000/666000` (unit unstated in DT) | VERIFIED cells; **unit = inference** | T1 §8 |
| Codec firmware blob | `mfc_fw.bin`, 687 912 B, in-image `/firmware/mfc_fw.bin` | VERIFIED (name+size only, not extracted) | T1 §7 |
| MFC ip_ver | `0x1300` | VERIFIED (DT cell, and the mirror's dtsi sets the same value) | T1 §8, T2 |
| What `0x1300` *means* | the driver splits it as major `0x13` / minor `0x00` and prints `"MFC v%x.%x"` → **v13.0 in hex**; Samsung's marketing generation name for the block is not established | **UNVERIFIED** (name only; the arithmetic is cited) | `mfc_common.h:198-199`, `mfc_run.c:99-103` |
| Driver build kind | built-in (`y`) — no `.ko` anywhere in vendor | VERIFIED (three reads) | T1 §6 |

**Landed 2026-10-13** in `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h`
(new "Video codec / MFC (D10)" block): the MFC window + GSIV 370 + `ip_ver`, the three clock
gate ids (422/423/424), both SYSMMU windows with their secure aliases and four GSIVs
(375/376/377/378), the hwfc and mmcache windows, the PMU power-domain offset, and the four
register offsets the bring-up needs (`RISC_ON 0x0000`, `RISC_BASE_ADDRESS 0x0054`,
`MFC_RESET 0x1070`, `MFC_VERSION 0xF028`) plus the two R2H completion codes (1, 12).
Five `PlatformMemoryMapLib` entries added in the same commit: `MFC`, `MFC_SYSMMU0`,
`MFC_SYSMMU1`, `MFC_HWFC`, `MFC_MMCACHE`.

**Build evidence** (Linux verification build, `/tmp/opencode/build-uefi-linux.sh` regenerated
from `notes/d1-build-env.md`, gcc 12.3.1): exit **0**, zero `error:` lines;
`BEYOND0LTE_UEFI.fd` **7 340 032 B**; `FVMAIN_COMPACT` 421 888 B total / **56 B free**;
`FVMAIN` 47 % full. Pre-existing warnings only (`FuelGauge.c`, `PeilessSec.c`).
**Verification-only FD — never flash.** No device rung has run.

Still deliberately **not** constants: the CMU bank that owns gate 422 (the MFC node names five
banks and none is labelled), and the `mfc_freqs` unit. Both stay in the open list above.

## Dead ends (do not repeat)

- T1 (2026-10-13): the full list is in the T1 subsection, "Dead ends from this pass". Headlines:
  `symbols.tsv`/`kallsyms.log` carry **no names** (shipped kernel has no kallsyms — already
  documented at `tools/dump-tools/kallsyms.py:4-8`); `xref.py --string` silently finds nothing
  (`find_string_offsets()` rejects a needle that starts at the beginning of a string);
  `debugfs` paths in vendor.raw are **rooted at `/vendor`**, so `/vendor/firmware` does not exist;
  absolute-pointer table scans do not work on this Image (relocatable); `ip_ver` in kernel strings
  is a `chip_ver` substring collision; `mfc_qos_table`/`mfc_qos_platdata` literals are absent;
  every `mfc` hit in the DTBO overlays and in `ov00.dts` is the **IDT Qi wireless charger**
  (`idt,mfc-charger`), and every `codec` hit is **audio** (cs35l40 / ASoC `sound/*/codec`);
  in `base.dtb` the `__symbols__` values are **path strings, not phandles** (749 entries), so
  `struct.unpack(">I", …)` on them raises `struct.error` — resolve overlay targets through
  `/__fixups__` instead (T1 §1c).

## Sources

All T1 evidence is from the local shipped dumps in `dumps/hhxj6/` (Android 12 ODIN package
G970FXXUHHXJ6, beyond0lte / SM-G970F). No web sources were used in T1. No device reads were made.

- `dumps/hhxj6/base.dtb` — raw FDT v40, 239 680 B, md5 `4c53ff438bc5bbdc87ccda6fb65c9bd3`
  (byte-identical to blob 0 of `dumps/hhxj6/dt.img`). Parsed with `tools/dump-tools/fdt.py`.
- `dumps/hhxj6/base.dts` — decompiled base (used only for line citations; its phandle
  annotations on `interrupts`/`clocks` are **wrong**, see T1 Method).
- `dumps/hhxj6/dtbo.img` (8 388 608 B, 7 FDT blobs) and the same blobs as
  `dumps/hhxj6/dts/dtb00.dtb … dtb06.dtb`; `dumps/hhxj6/ov00.dts` (decompiled overlay 0).
- `dumps/hhxj6/kernel` — raw ARM64 Image, 44 380 172 B; `strings -a` dump kept at
  `/tmp/opencode/video-mfc/kernel.strings.txt` (291 408 lines, gitignored scratch).
- `dumps/hhxj6/kallsyms.log`, `dumps/hhxj6/symbols.tsv` — negative result.
- `dumps/hhxj6/vendor/vendor.raw` — ext4 of the vendor partition, 1 153 433 600 B, read with
  `debugfs -R 'ls -l …'` / `debugfs -R 'cat /etc/media_codecs.xml'` (debugfs 1.47.0).
- `dumps/hhxj6/cm.bin` — no FDT blobs (negative).
- Tools: `tools/dump-tools/fdt.py`, `tools/dump-tools/xref.py` (VA base
  `0xFFFFFF8008000000` = file offset 0), `tools/dump-tools/kallsyms.py`.
- Reusable walker written for this pass (outside the tree, per repo rules):
  `/tmp/opencode/video-mfc/walk.py` — node/compatible matcher + raw-cell printer.
  Insert helper for these notes: `/tmp/opencode/video-mfc/ins.py`.

### Sources added by T2–T5 (the vendor-kernel clone, the Windows contract, prior art)

- `github.com/ianmacd/d2s` @ `d1d4fb4aa` — cloned to `/tmp/opencode/d2s` (2.1 GB, **outside the
  repo**, nothing imported). Linux 4.14.113, Exynos 9820 (Note 10 family), GPL-2.0. Files read:
  `Makefile`, `README_Kernel-n970f.txt`, `arch/arm64/configs/exynos9820-d2s_defconfig`,
  `arch/arm64/boot/dts/exynos/exynos9820-mfc.dtsi`, `exynos9820.dts`,
  `include/dt-bindings/clock/exynos9820.h`,
  `include/dt-bindings/interrupt-controller/exynos9820.h`,
  `drivers/irqchip/irq-gic.c`,
  `drivers/media/platform/exynos/mfc/{mfc.c,mfc_run.c,mfc_buf.c,mfc_pm.c,mfc_hwlock.c,mfc_regs.h,
  mfc_reg_api.h,mfc_hw_reg_api.h,mfc_common.h}`.
- `learn.microsoft.com/en-us/windows-hardware/drivers/stream/getting-started-with-hardware-codec-support-in-avstream`
- `learn.microsoft.com/en-us/windows-hardware/drivers/stream/encoder-devices`
- `learn.microsoft.com/en-us/windows-hardware/drivers/stream/encoder-installation-and-registration`
- `learn.microsoft.com/en-us/windows/win32/medfound/hardware-mfts`
  (raw: `MicrosoftDocs/win32/docs/desktop-src/medfound/hardware-mfts.md`)
- `learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation`
- `learn.microsoft.com/en-us/windows-hardware/drivers/display/d3d11-video-playback-improvements`
- `Documentation/devicetree/bindings/media/s5p-mfc.txt` (mainline binding, supported compatibles)
  and `drivers/media/platform/samsung/s5p-mfc/` on `torvalds/linux` master
- `Documentation/devicetree/bindings/interrupt-controller/arm,gic.yaml` (torvalds/linux master) —
  the SPI-numbering rule, second source
- `dumps/hhxj6/vendor/vendor.raw` — `debugfs -R "stat /firmware/mfc_fw.bin"` → Size 687912

## Review of the design doc (harsh-critic pass, 2026-10-13)

- Checked mechanically: `_HID "SAMS9828"` is unused anywhere in the tree
  (`grep -rhoE "SAMS98[0-9]{2}" firmware/ drivers/ docs/` → 9810, 9820, 9821, 9825, 9826, 9827);
  the FFS suffix `…554B` is free (used: 5521 AcpiTables, 5545 UfsHc, 5546 ResetLib,
  5547 RamManager, 554A ModemDxe); every register/window value in §2 traces to a row in the
  constants table above; the AVStream requirements quoted in §3/§4 are the ones cited in T3.
- Found: **P7 asserted a fact that was never read** — "Windows already decodes H.264/HEVC in
  software on this CPU". Nothing in this repo has verified an in-box software decoder on
  Windows ARM64, and the HEVC case has a licensing wrinkle on consumer Windows that was not
  checked. Rewritten as labelled inference with the measurement that would settle it.
- Fixed: in the design doc itself (the P7 row). No code affected.
- Not affected: the ladder, the `_CRS` order, the SYSMMU ownership ruling, the licence posture.

## Release-scan gate (2026-10-13, before the arc was collapsed)

`python3 tools/release/scan.py` → **611 files, 87 findings** (was 86 before this arc). The single
addition is `notes/video-mfc.md:456 [gpl]` — the sentence that *documents* the mirror tree's
`SPDX-License-Identifier: GPL-2.0`. It is a licence mention, not imported code, and the same rule
already fires in `notes/d8-audio-windows.md:342` for the identical reason, so the rise is expected
and accepted rather than silenced. No blob, no key, no device path was added: `mfc_fw.bin` is
recorded by name and size only.
