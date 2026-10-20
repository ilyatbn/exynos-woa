# Research answers to coding-agent asks R1-R7 (2026-10-21)

Running notes for answering the M4/G30 research asks (see m4/findings.md context). Static RE on
m4/artifacts/* + ~/Downloads/firmware/bl + /data2/sams; public docs cited with URLs. Facts vs
inference marked; dead ends recorded.

## Progress log

- [init] file created; subagents launched for R1 (ARM ARM FWB/shareability + M4 errata) and
  R5 (mainline/postmarketOS 9820 SMP status). Static RE: R2 (uH fault path), R3 (secondary EL2),
  R4 (IMP-DEF sweep), R6 (cl2emul), R7 (reserved-memory cross-check) done in-session below.

## R5 (web): mainline/postmarketOS on Exynos 9820 — do the M4 cores work?

Started: 2026-10-20. Web research only. Questions: (1) pOS/mainline status for beyond0/1/2;
(2) are all 8 CPUs (2 M4 + 2 A75 + 4 A55) online/stable in those ports, or limited?;
(3) bug reports of corruption/hangs that vanish with M4 offline; (4) mainline exynos9820.dtsi
CPU nodes 6-7 + OPPs, and whether those ports run under Samsung's stock boot chain (sboot/uH/EL2).

### Findings (in progress)

## R1 (web part): stage-2 shareability combine rules + M4 errata

Scope: (1) ARM ARM DDI 0487 rules for combining stage-1/stage-2 memory type and SHAREABILITY
when HCR_EL2.FWB=0, with section numbers + quotes; (2) effective shareability when S1 = Normal
Inner-Shareable and S2 = Device; (3) public Samsung M4/Mongoose errata hunt; (4) any public
statement about Exynos 9820 DSU/coherency requiring Inner-Shareable for the M4 cluster.
Started 2026-10-20. Facts are quoted/paraphrased from a cited URL; my own reasoning is marked INFERENCE.

Contents of this section (appended in order, so other sections of this file are interleaved - find
them with `rg -n '^###? (Q[0-9]|Bottom line|Sources \(R1)' notes/m4-research-r1r7.md`):
"Q1 - ARM ARM: how S1+S2 attributes combine when HCR_EL2.FWB=0", "Q2 - S1 = Normal Inner Shareable,
S2 = Device", "Q3 - Samsung M4 / Mongoose errata hunt", "Q4 - ... Inner-Shareable ...",
"Bottom line for the coding agent", "Sources (R1 web part)".


**FACT** postmarketOS wiki (now served at wiki.nura.eco; wiki.postmarketos.org API redirects
there — Anubis bot-check blocks plain fetches of the HTML UI, the api.php works).
Device page **"Samsung Exynos 9820 (samsung-exynos9820)"** exists, pageid 13454, last edit
2026-10-11 (https://wiki.nura.eco/wiki/Samsung_Exynos_9820_(samsung-exynos9820), via api.php):
- Generic device pkg `samsung-exynos9820`, `booting=yes`, `packaged=yes`, category **testing**,
  `supported=yes`, pmos kernel **7.2.0**, kernel pkg `linux-postmarketos-exynos9820`,
  initial MR = **pmaports!8999**. Contributor/maintainer: User:Chiffa (pkg maintainer email
  chiffathefoxx@gmail.com per repology https://repology.org/project/device-samsung-exynos9820/packages).
- Feature matrix: Y = usbnet, emmc/internal storage, haptics, power sensor, uboot(secondary);
  P = battery, screen, touch. N = pmbootstrap flashing, sdcard, 3D (Mali-G76), audio, camera,
  wifi, BT, GPS, NFC, calls/SMS/mobile data, OTG, fingerprint, accel/magnet/prox/hall/barometer.
  `status_uboot_mainline=N`, `status_uboot_primary=N`, `status_uboot_secondary=Y`.
- Boot chain (Q4): **sboot kept as primary bootloader** ("Exynos 9820 devices come with the
  S-Boot that handles CPU, DRAM, UFS and screen initialization... it ignores cmdline and boot
  offset parameters from the boot.img and doesn't accept mainline-compatible DTBs"). Workaround:
  **custom U-Boot fork** https://github.com/chiffathefox/u-boot/tree/exynos9820 (`exynos9820_defconfig`),
  packed as boot.img (`mkbootimg --kernel u-boot.bin`), written to by-name/boot; U-Boot then
  boots kernel **via EFI**. So the chain is sboot -> U-Boot(fork) -> mainline kernel on EFI.
  Rootfs flashed to by-name/userdata. No prebuilt U-Boot images; upstreaming "ongoing".
- Known issues listed on page: suspend broken -> "unexpected lockups" (disable auto-suspend);
  touchscreen dead after reboot; aftermarket panels no events; wireless-charge state wrong;
  fast charging broken. **No mention of CPU/M4/core-count restrictions.**
- Install prereqs: OEM unlock, developer vbmeta, TWRP. (Knox trips.)

**FACT** Repo also has per-model pages (wiki search): "Samsung Galaxy S10 (samsung-beyond1lte)",
"Samsung Galaxy S10+ (samsung-beyond2lte)"; they are device entries whose genericdevice is
samsung-exynos9820 (cargo query on the generic page lists them).

**FACT** wiki page "Samsung Galaxy S10 (samsung-beyond1lte)"
(https://wiki.nura.eco/wiki/Samsung_Galaxy_S10_(samsung-beyond1lte)):
- Downstream-kernel port (pmoskernel 4.14.316; maintainer User:Faveoled2), Y: flashing, usbnet,
  battery, touch, wifi, otg; P screen; N 3d/audio/camera/gps/nfc/calls/sms/data/fde.
- "Mainline kernel" section: "Mainline boots with uniLoader
  (https://github.com/ivoszbg/uniLoader/commit/c4d3f6490eda8180ddf673d984b078324fa38d9c) and
  [https://gist.github.com/faveoled/e7c44486feb4b65ccf014c254e0f542c this] device tree.
  Only SimpleFB works." -> older mainline route = uniLoader (UEFI payload after sboot), not U-Boot.
- UnixBench log on the page (Dec 2021, "Linux 4.14.253 #1-Nura SMP"): **"8 CPUs in system"**,
  1-copy 22.9M DMIPS-lps / 8-copy 72.8M -> all 8 cores were online & benchmarked under the
  downstream-based Nura kernel. (FACT that it ran; says nothing about M4-specific bugs.)
- Slow boot note: SSH/display/GUI only ~3 min into boot on downstream kernel.
- See-also initial MR pmaports!2739 (downstream beyond1lte pkg).

**FACT** wiki page "Samsung Galaxy S10+ (samsung-beyond2lte)"
(https://wiki.nura.eco/wiki/Samsung_Galaxy_S10%2B_(samsung-beyond2lte)):
downstream port (4.14.309), booting=yes, packaged=yes; Y usbnet/flashing/wifi/battery,
N touch/screen/3d; initial MR pmaports!6161. Points at ubports guide for S10e(beyond0lte)/S10/S10+.

**FACT** pOS kernel pkg `linux-postmarketos-exynos9820` 7.2.0-r1 (pkgs.nura.eco
https://pkgs.nura.eco/package/main/postmarketos/aarch64/linux-postmarketos-exynos9820):
"Close-to-mainline kernel for Samsung Exynos 9820 devices", project
**https://github.com/chiffathefox/exynos-9820-mainline-linux**, GPL-2.0-only, build 2026-10-06,
maintainer chiffa. Repo is a fork of torvalds/linux (created 2026-01-04, pushed 2026-09-27) with
branches `exynos9820-7.1-rc2`, `exynos9820-7.2-rc2`, `master`. (Kernel version numbers in this
timeline: mainline ~7.2/7.3-rc.)

**FACT (dead end for Q4 as phrased)** Upstream torvalds/linux master (checked 2026-10-20 via
https://api.github.com/repos/torvalds/linux/contents/arch/arm64/boot/dts/exynos) has NO
exynos9820*/exynos9825* files. Dir contains exynos7/5433/7870/7885/850/8855/8895/9810/990/2200/
exynosautov9*/axis/google only. So the premise "mainline DTs for 9820 (exynos9820.dtsi)" is false
upstream; any exynos9820.dtsi lives in downstream/community forks. raw fetch of
torvalds/linux .../exynos9820.dtsi -> 404.
### Q1 — ARM ARM: how S1+S2 attributes combine when HCR_EL2.FWB=0 (exact section + quotes)

FACT. Primary source obtained: full ARM ARM **DDI 0487G.a** (Armv8.7, Sep 2021) PDF mirror at
`http://kib.kiev.ua/x86docs/ARM/ARMARMv8/DDI0487G_a_armv8_arm.pdf` (50 MB, 8539 PDF pages).
Local working copy: `/tmp/opencode/DDI0487G_a_armv8_arm.pdf`, text extract `/tmp/opencode/d5_5.txt`
(PDF pages 2753-2765 = printed pages D5-2753..D5-2765; offset is exactly 0: printed D5-nnnn = PDF page nnnn).

Section number in G.a: **D5.5 "Memory region attributes"**, and the combine rules are in
**D5.5.7 "Combining the stage 1 and stage 2 attributes, EL1&0 translation regime"**, printed pages
D5-2760 to D5-2763. (In the Armv8.0 manual DDI 0487A.a the same material is titled
"Combining the stage 1 and stage 2 attributes, Non-secure EL1&0 translation regime", page D4-1717,
inside D4.4 "Access controls and memory region attributes" - markdown mirror
`https://github.com/codingbelief/arm-architecture-reference-manual-for-armv8-a/blob/master/en/chapter_d4/d44_access_controls_and_memory_region_attributes.md`.
In the current (K/L/M-series) numbering the VMSA chapter is D8 and this section is D8.6 "Memory region
attributes" - see the developer.arm.com/ddi0487 index hit
`https://support.arm.com/documentation/ddi0487/md/-Part-D-The-AArch64-System-Level-Architecture/-Chapter-D8-The-AArch64-Virtual-Memory-System-Architecture/-D8-6-Memory-region-attributes`
("For translation regimes that support two translation stages, the stage 1 and stage 2 memory region
attributes are combined to produce a resultant memory type"), not yet verified page-by-page - see below.)

#### Q1(a) memory type combination (FWB=0) — DDI 0487G.a §D5.5.7, printed page D5-2761

FACT (verbatim, G.a D5.5.7, p. D5-2761):
> "Combining the stage 1 and stage 2 memory type attributes
> The combining of memory type attributes from the two stages of translation applies only if
> HCR_EL2.FWB is set to 0."

FACT (verbatim, Table D5-43 "Combining the stage 1 and stage 2 memory type assignments", p. D5-2762):
> Rule | If either stage of translation assigns: | The resultant memory type is:
> Device has precedence over Normal | Any Device memory type | A Device memory type
> non-Gathering has precedence over Gathering | A Device-nGxx memory type | A Device-nGxx memory type
> non-Reordering has precedence over Reordering | A Device-nGnRx memory type | A Device-nGnRx memory type
> No Early write acknowledge has precedence over Early write acknowledge | The Device-nGnRnE memory type | The Device-nGnRnE memory type

So the resultant Device flavor is the *most restrictive* of the two stages' Device flavors, and any
Device at either stage makes the whole mapping Device. Concretely for the M4 question:
S1 Normal + S2 Device-nGRE => resultant **Device-nGRE**; S1 Device-GRE + S2 Device-nGRE =>
**Device-nGRE**; S1 Device-nGRE + S2 Normal => **Device-nGRE**. (FACT: direct reading of Table D5-43.)

FACT (cacheability, Table D5-44, p. D5-2763, applies only if FWB=0): Non-cacheable at either stage
=> Non-cacheable; Write-Through at either stage (with the other WT or WB) => Write-Through; WB+WB => WB.
i.e. "the less cacheable of the two wins".

FACT (FWB=1 counter-case, G.a D5.5.7 p. D5-2760/2761):
> "When HCR_EL2.FWB is set to 1, and the stage 2 page or block descriptor [4:2] is set to 0b110, the
> resultant memory type is Normal Write-Back Cacheable regardless of the value of the stage 1 memory type."
and
> "When FEAT_S2FWB is implemented, the architecture requires that CLIDR_EL1.{LOUU, LOIUS} are zero so
> that no levels of data cache need to be cleaned in order to manage coherency with instruction fetches."


**FACT** chiffathefox kernel fork commit log (branch exynos9820-7.2-rc2, via GitHub API
https://api.github.com/repos/chiffathefox/exynos-9820-mainline-linux/commits?sha=exynos9820-7.2-rc2):
9820 work on top of 7.2-rc2 includes: clk-exynos9820 (blocks TOP/PERIS/CORE/MIF/BUSC/FSYS/PERIC/
**CPUCL0 (empty stub)**/**CPUCL2 (empty stub)**/G3D empty/DPU/CMGP), "clk: samsung: acpm:
exynos9820 support", ACPM DVFS clock ids, "dirty: pmu: exynos9820 support" on top of gs101/
Exynos850 exynos-pmu cpu{idle,hotplug} generalisation, TMU thermal 9820, power domains 9820,
pinctrl 9820, UFS + phy-exynos9820-ufs, s2mps19/max77705 PMIC, touchscreen fixes.
No cpufreq driver commit in the top 100; CPU DVFS appears to go through **ACPM** clock provider
(EXYNOS9820_CLK_ACPM_DVFS_CPUCL0/1/2). (ACPM = Arm Cortex Platform Manager, a SoC power/DVFS
microcontroller already spoken to by Samsung's own stack.)
NOTE: fork repo has NO arch/arm64/boot/dts/exynos/exynos9820* files -> DT is supplied externally.

**FACT** The DT actually used by the port lives in the U-Boot fork:
https://raw.githubusercontent.com/chiffathefox/u-boot/exynos9820/dts/upstream/src/arm64/exynos/exynos9820.dtsi
(plus exynos9820-common.dtsi, exynos9820-beyond1lte.dts, exynos9820-beyond2lte.dts, pinctrl, tmu).
U-Boot configs/exynos9820_defconfig (same repo): TARGET_EXYNOS_MOBILE,
DEFAULT_DEVICE_TREE="exynos/exynos9820-beyond1lte", OF_LIST beyond1lte+beyond2lte, MULTI_DTB_FIT,
SAVE_PREV_BL_FDT_ADDR=y (keeps DTB address from previous bootloader), CONFIG_SKIP_LOWLEVEL_INIT=y
(loaded by sboot, no SPL), ARM_SMCCC=y, PSCI_RESET not set, EXYNOS_PMU, UFS/PMIC/regulators.
CPU nodes in that dtsi — **all 8 CPUs described, M4s included and not status-disabled**:
- cpu0-3 @reg 0..3, arm,cortex-a55, clk ACPM_DVFS_CPUCL0, enable-method "psci",
  idle &ananke_cpu_sleep, OPP cpucl0_opp_table, capacity-dmips-mhz 272
- cpu4-5 @reg 4,5, arm,cortex-a75, ACPM_DVFS_CPUCL1, psci, &prometheus_cpu_sleep, cpucl1_opp, cap 531
- **cpu6-7 @ cpu@100/cpu@101 reg 0x100/0x101, compatible "samsung,mongoose-m4",
  clocks ACPM_DVFS_CPUCL2, enable-method "psci", idle &cheetah_cpu_sleep,
  cpucl2_opp_table, capacity-dmips-mhz 785, dynamic-power-coefficient 760**
M4 MPIDR affinities 0x100/0x101 (Aff1=1) — distinct cluster id, unlike A55/A75 which share
Aff1=0 with Aff0 0..5. Idle state names match Samsung downstream (ananke/prometheus/cheetah).
#### Q1(b) SHAREABILITY combination (FWB=0) — DDI 0487G.a §D5.5.7, printed pages D5-2762/2763

FACT (verbatim, G.a D5.5.7 "Combining the stage 1 and stage 2 shareability attributes for Normal
memory", p. D5-2763):
> "A memory region is treated as Outer Shareable, regardless of any shareability assignments at
> either stage of translation, if either:
> • The resultant memory type attribute, described in Combining the stage 1 and stage 2 memory type
> attributes on page D5-2761, is any type of Device memory.
> • The resultant memory type attribute ... is Normal memory, and the resultant cacheability ... is
> Inner Non-cacheable, Outer Non-cacheable.
> For a memory region with a resultant memory type attribute of Normal, that is not Inner
> Non-cacheable, Outer Non-cacheable, Table D5-45 shows how the stage 1 and stage 2 shareability
> assignments are combined."

FACT (verbatim, Table D5-45 "Combining the stage 1 and stage 2 Shareability assignments for Normal
memory", p. D5-2763) - 7 rows, complete:
> Assignment in stage 1 | Assignment in stage 2 | Resultant shareability
> Outer Shareable | Any | Outer Shareable
> Inner Shareable | Outer Shareable | Outer Shareable
> Inner Shareable | Inner Shareable | Inner Shareable
> **Inner Shareable | Non-shareable | Inner Shareable**
> Non-shareable | Outer Shareable | Outer Shareable
> Non-shareable | Inner Shareable | Inner Shareable
> Non-shareable | Non-shareable | Non-shareable

**Answer to the specific question:** a stage-2 **Non-shareable** attribute **cannot downgrade** a
stage-1 Inner Shareable mapping - the row "Inner Shareable / Non-shareable / Inner Shareable" is
explicit. The resultant shareability for Normal Cacheable memory is the **more shareable of the two
stages** (OSH > ISH > NS), i.e. shareability only ever *increases* through stage 2. Consistent with
the Note at the head of D5.5 (FACT, verbatim, p. D5-2753):
> "In a virtualization implementation, a hypervisor, executing at EL2, might usefully:
> • Reduce the permitted cacheability of a region.
> • Increase the required shareability of a region.
> The combining of attributes from stage 1 and stage 2 translations supports both of these options."

Also FACT (G.a D5.5.1, p. D5-2755, note under Table D5-36):
> "The shareability field is only relevant if the memory is a Normal Cacheable memory type. All
> Device and Normal Non-cacheable memory regions are always treated as Outer Shareable, regardless
> of the translation table shareability attributes."

And FACT (G.a D5.5.7, p. D5-2762, right after Table D5-43):
> "Regardless of any shareability attribute obtained as described in Combining the stage 1 and stage 2
> shareability attributes for Normal memory on page D5-2763:
> • Any location for which the resultant memory type is any type of Device memory is always treated
> as Outer Shareable.
> • Any location for which the resultant memory type is Normal Inner Non-cacheable, Outer
> Non-cacheable is always treated as Outer Shareable."

INFERENCE: the SH[1:0] bits of a stage-2 descriptor are therefore inert for Device and for Normal-NC
mappings, and for Normal Cacheable they are a floor, never a ceiling. Nothing in FWB=0 lets EL2
weaken coherence requirements relative to stage 1; the only "override" mechanism is FWB=1
(FEAT_S2FWB, Armv8.4) which replaces the *type/cacheability* combination, not the shareability one.


**FACT** pOS kernel apk (linux-postmarketos-exynos9820 7.2.0-r1, downloaded from
http://mirror.math.princeton.edu/pub/postmarketos/master/aarch64/): /boot/dtbs/exynos contains
NO exynos9820 dtb (only upstream exynos7/5433/7870/7885/850/8895/9810/990/2200/autov9*).
Kernel config has CPU_FREQ + CPUFREQ_DT(+PLATDEV), CPU_IDLE multiple-drivers + menu gov,
CPU_THERMAL/CPU_FREQ_THERMAL. -> The 9820 DTB is supplied by the U-Boot fork (dts/upstream dir,
MULTI_DTB_FIT), kernel itself carries no 9820 dtb.

**FACT** U-Boot fork exynos9820.dtsi idle-states block: ananke(A55)/prometheus(A75)/cheetah(M4)
cpu-sleep states ALL have `status = "disabled"`; entry-method "psci"; suspend param 0x0010000
(local-timer-stop). So in the community DT, CPUs are fully enabled but their deep *idle* states
are off everywhere, not just M4. Consistent with wiki "Suspend is currently broken...
unexpected lockups" known issue.

**FACT** device-samsung-exynos9820 deviceinfo (extracted from the .apk, mirror above):
codename samsung-exynos9820, 2019, handset, `deviceinfo_flash_method="none"` with comment
"Heimdall fails mid-transfer when flashing rootfs. Use TWRP instead"; boot_filesystem fat32,
rootfs sector size 4096. No cmdline/core restrictions in deviceinfo.

**DEAD END** gitlab.postmarketos.org (new pOS forge): anonymous raw + git clone refused
(login required), webfetch/curl of gitlab.com pmaports blocked by Cloudflare/Anubis. pmaports
MR 8999/2739/6161 contents not directly readable from this network; only wiki + apk evidence.

**FACT** upstream dt-bindings arm/cpus.yaml (github.com/torvalds/linux master blob fetched
2026-10-21): accepted cpu compatibles include samsung,mongoose-m2, samsung,mongoose-m3,
samsung,mongoose-m5 — **no samsung,mongoose-m4** in the list. So the "samsung,mongoose-m4"
used by the U-Boot-fork 9820 DT is not an upstream binding (as of that master).
(https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/arm/cpus.yaml)

**FACT** upstream history context: "[PATCH v4 01/10] dt-bindings: arm: cpus: Add Samsung
Mongoose M3" (Markuss Broks w/ Maksym Holovach, Oct 2024, applied by Krzysztof Kozlowski
https://git.kernel.org/krzk/linux/c/608c8f408dac3d1fd96191494f1a32ba2b1e133d ;
https://lists.openwall.net/linux-kernel/2024-10-28/632) and "[PATCH v2 0/6] Add minimal Exynos990
SoC and SM-N981B support" (Igor Belwon, mentallysanemainliners.org, Oct 2024,
https://lists.openwall.net/linux-kernel/2024-10-15/1797) — Exynos990 (Mongoose M5) has minimal
upstream DT (cpus/pinctrl/chipid/simple-framebuffer) and upstream docs say the preferred way to
boot it is **uniLoader shim** "which works around some issues with the stock, non-replacable
Samsung S-LK bootloader". Upstream exynos9810.dtsi has M3 cores cpu@100..103 reg 0x100..0x103
enable-method "psci" (same affinity pattern as 9820 M4s). Neither 9810 nor 990 upstream dtsi
disables their Mongoose cores.

**FACT** uniLoader (https://github.com/ivoszbg/uniLoader) gained a Samsung Galaxy S10 board:
commit c4d3f6490eda8180ddf673d984b078324fa38d9c "board: add support for Samsung Galaxy S10"
(faveoled, 2026-02-06; files board/samsung/board-beyond1lte.c, configs/beyond1lte_defconfig,
CONFIG_EXYNOS_9820=y, TEXT_BASE=0x89000000, PAYLOAD_ENTRY=0x90000000, simplefb 1440x3040 @0xca000000,
decon trigger fix at 0x19030000). It is the same shim the Exynos990 upstream docs recommend
("works around some issues with the stock, non-replacable Samsung S-LK bootloader"). Board file
credits David Wronek (2022), Ivaylo Ivanov (2024), faveoled (2026).

**FACT** mainline has no Samsung CPU plumbing at all (as of torvalds master 2026-10-21):
- arch/arm64/include/asm/cputype.h: no SAMSUNG implementer / mongoose part macros (grep empty)
- arch/arm64/kernel/cpu_errata.c: no samsung/mongoose entries (grep empty)
So mainline carries **zero Mongoose M4 errata workarounds**; errata coverage for M4 would be a
missing piece if one needed to reproduce Samsung's downstream protections. (M4-specific erratum
sheets: not found in mainline; dead end for "mainline erratum for M4".)

**FACT** UBports Ubuntu Touch (downstream-kernel) ports exist for beyond0lte/beyond1lte/
beyond2lte + d1/d1x/d2s/d2x (Android 10 base): https://gitlab.com/ubports/porting/community-
ports/android10/samsung-galaxy-s10/samsung-exynos9820 (README: "Ubuntu Touch for Samsung Galaxy
S10, S10e, S10+, Note10, Note10+ Exynos"). XDA thread: https://xdaforums.com/t/community-ubuntu-
touch-for-s10-s10e-s10-beyond1lte-beyond0lte-beyond2lte.4355013/ . devices.ubuntu-touch.io now
shows beyond0lte/1lte/2lte only under the **outdated xenial aggregation** ("This device isn't
ported yet" for current releases; xenial page lists it Halium-10-based, "Device is unmaintained,
last commit 2/26/2025"). UT gitlab issue list for the port shows generic issues (bootloader loop
on S10+ #6, wifi auth #7, libertine hang #9, flight mode #10) — **no M4/core-count/corruption
issue among them**. (beyond0lte UT spec table oddly says "Hexa-core" — porter data error, do not
read as core restriction.)

**FACT** Boot-chain/hypervisor context (Q4 sub-question):
- BlackHat USA 2020 "Breaking Samsung's Root of Trust: Exploiting Samsung S10 S-Boot"
  (https://etc.chipmunk.land/android/writeups/us-20-Chao-Breaking-Samsungs-Root-Of-Trust-
  Exploiting-Samsung-Secure-Boot.pdf ; summary https://teamt5.org/en/blog/blackhat-s-talk-
  breaking-samsung-s-root-of-trust-exploiting-samsung-secure-boot): Knox trusted boot on S10:
  "S-Boot: set handler for Monitor mode, drop privilege; request EL3 to initial TEEOS;
  **verify & load Hypervisor (uh.bin)**; verify & load Kernel (boot.img)". Exploit authors note
  "We got EL1" inside sboot; EL3 TZ state can't be reverted. So stock Linux/Android kernel on
  S10 runs **below an EL2 hypervisor (uh.bin)** set up by sboot.
- DayZeroSec "Reversing Samsung's H-Arx Hypervisor Framework - Part 1" 2025-03-08
  (https://dayzerosec.com/blog/2025-03-08/reversing-samsungs-h-arx-hypervisor-part-1.html):
  SBOOT loads EL2 module HARX at 0xc0000000 via load_el2_module + SMC 0x82000480; the **uH
  plugin is loaded to 0xc1400000 and runs in EL2**. Confirms EL2 stack = H-Arx core + uH plugin.
- allsoftwaresucks blog 2019-05 "Reverse-engineering Samsung Exynos 9820 bootloader and TZ"
  (http://allsoftwaresucks.blogspot.com/2019/05/reverse-engineering-samsung-exynos-9820.html):
  s-boot image layout includes **ACPM at offset 0x13C00**, PM tables, BL2 s-boot at 0xA4000,
  TEEGRIS SPKG (CSMC) handling "some SMC calls from the bootloader as part of Secure Boot for
  Linux", encrypted ARM Trusted Firmware (EL3 monitor) after Rijndael S-box; s-boot itself
  believed to run at EL1 NS. -> On 9820, ACPM power/DVFS firmware is *part of the sboot image*,
  which explains why the community mainline port drives CPU DVFS through an **ACPM IPC clock
  driver** rather than direct CMU register programming.

**INFERENCE (mine, flagged)**: since pOS/uniLoader ports keep sboot as primary bootloader and
hand it a custom boot.img, Samsung's EL2 init (H-Arx/uH) in the stock flow very likely still
happens before payload handoff; U-Boot/uniLoader then run *at EL1* (or drop to EL1), and the
mainline kernel boots with the stock EL2 hypervisor resident but unmanaged. No source states
this explicitly for 9820; unresolved point. The alternative — ports replacing the boot chain
before sboot — is NOT what these ports do (wiki is explicit sboot is kept).
#### Q2 — S1 = Normal Inner Shareable, S2 = Device: what is the effective shareability?

FACT. Two-stage chain, all from DDI 0487G.a:
1. Type: Table D5-43 (D5.5.7, p. D5-2762) - "Device has precedence over Normal / Any Device memory
   type => A Device memory type". With S1 Normal + S2 Device-nGRE the resultant type is
   **Device-nGRE**.
2. Shareability: D5.5.7 p. D5-2762 - "**Any location for which the resultant memory type is any type
   of Device memory is always treated as Outer Shareable**" (and p. D5-2763 repeats it as the first
   bullet of "Combining the stage 1 and stage 2 shareability attributes for Normal memory": "A memory
   region is treated as Outer Shareable, regardless of any shareability assignments at either stage of
   translation, if ... the resultant memory type attribute ... is any type of Device memory").
3. Same rule at the single-stage/application level, B2.7.2 "Device memory", p. B2-170: "Data accesses
   to memory locations are coherent for all observers in the system, and correspondingly are treated
   as being Outer Shareable."
4. And stage 2's own note, D5.5.4 (p. D5-2757): "The shareability field is only relevant if the memory
   is a Normal Cacheable memory type. All Device and Normal Non-cacheable memory regions are always
   treated as Outer Shareable, regardless of the translation table shareability attributes."

**Answer:** the effective shareability is **Outer Shareable**, and the stage-1 SH bits (IS) and stage-2
SH bits (whatever they are) are both irrelevant for that mapping. So a stage-2 Device mapping cannot
be made "less coherent" than OSH by any descriptor setting, and Device-nGRE at stage 2 is
forced-Outer-Shareable. Non-cacheable Normal is in the same bucket (always OSH): B2.7.1 note, p. B2-166:
"Because all data accesses to Non-cacheable locations are data coherent to all observers, Non-cacheable
locations are always treated as Outer Shareable."

INFERENCE (consequence for our firmware): if the S10e guest/UEFI maps a device region with stage 1
Normal-IS and the stage-2 mapping is Device-nGRE, the access is Device+OSH: no caching, no
speculation, barriers must cover the Outer shareable domain, and Device-nGRE ordering rules
(barriers required to order accesses - B2.7.2 p. B2-169: "The use of barriers is required to order
accesses to Device-nGRE memory") apply. There is no architectural way for the stage-2 Non-shareable
bit to reduce that.

FACT. Whether FWB changes any of this - G.a D5.5.5, p. D5-2758, verbatim:
> "The following are unaffected by the value of HCR_EL2.FWB:
> • The way that Shareability attributes from stage 1 and stage 2 are combined.
> • The way that stage 1 memory types and attributes are combined with stage 2 Device type and attributes."

So with FWB=1 the *shareability* combine table (D5-45) is still used, and stage-2 Device still forces a
Device result. FWB=1 only replaces the type/cacheability combine when stage 2 asks for
bits[4]==1 (Table D5-41: bits[3:2]==0b11 => "Normal Write-Back ... regardless of the value of the stage 1
memory type").

FACT. Stage-2 MemAttr encodings with FWB=0 (G.a Table D5-37/38/39, p. D5-2756):
MemAttr[3:2]==0b00 => Device, with MemAttr[1:0]: 00 = Device-nGnRnE, 01 = Device-nGnRE,
10 = Device-nGRE, 11 = Device-GRE. MemAttr[3:2]!=00 => Normal with MemAttr[1:0]: 01 = Inner
Non-cacheable, 10 = Inner WT, 11 = Inner WB, 00 = Reserved/CONSTRAINED UNPREDICTABLE.
Note "The following stage 2 translation table attribute settings leave the stage 1 settings unchanged:
MemAttr[3:2] == 0b11 ... MemAttr[1:0] == 0b11" - i.e. stage-2 MemAttr 0b1111 = pass-through for Normal WB.
With FEAT_S2FWB and FWB=1 the field moves to bits[5:2]: bit[4]=1 + bits[3:2]=0b10 => Normal
Non-cacheable for Normal stage 1 (Table D5-41), bit[4]=0 + bits[3:2]=0b10 => Device-nGRE (Table D5-42).

INFERENCE: the "0b101" encoding mentioned in the search hint is not a stage-2 MemAttr Device value in
G.a's FWB=0 table; in the FWB=1 layout bits[4:2]==0b101 (bit4=1, bits[3:2]=0b01) is "Normal
Non-cacheable", and bits[4:2]==0b110 (bit4=1, [3:2]=0b10) is the "force Normal Write-Back" case quoted
above. If a note elsewhere calls 0b101 "Device", it is using a different (likely HCR_EL2.FWB=1
bits[4]==0 => 0b0xx) numbering and should be re-checked against Table D5-42.


**FACT** Community U-Boot exynos9820.dtsi has (line ~612) the PSCI node with explicit quirk
comments (https://raw.githubusercontent.com/chiffathefox/u-boot/exynos9820/dts/upstream/src/arm64/exynos/exynos9820.dtsi):
  psci { /* PSCI advertises 0.2+ but AFFINITY_INFO is broken */
         compatible = "arm,psci"; method = "smc";
         cpu_suspend = <0xc4000001>; cpu_off = <0x84000002>; cpu_on = <0xc4000003>; };
-> sboot's monitor implements a PSCI-subset over SMC (standard function IDs); the port documents
that **PSCI AFFINITY_INFO is broken** on this firmware.
Also firmware node: `acpm_ipc: power-management { compatible = "samsung,exynos9820-acpm-ipc"; ... }`
with comment "/* Provide boot defaults since FREQ_GET is broken */" and assigned-clocks for
ACPM_DVFS_MIF/INT/CPUCL0/CPUCL1/CPUCL2/NPU/DISP/SCORE/AUD/CP/G3D/INTCAM/CAM/IVA.
M4 PMU node: pmu-mongoose-m4, compatible "samsung,mongoose-pmu", GIC_SPI 174/175,
interrupt-affinity <&cpu6 &cpu7>.

**FACT** Samsung downstream 9820.dts (mirror:
https://raw.githubusercontent.com/PixelExperience-Devices/kernel_samsung_exynos9820/thirteen/arch/arm64/boot/dts/exynos/exynos9820.dts):
- cpu@0000..0003 "arm,ananke" reg 0..3; cpu@0004/0005 "arm,prometheus" reg 4,5;
  **cpu@0100/0101 "arm,cheetah" (M4) reg 0x100/0x101, enable-method "psci"** — all 8 in stock DT.
- psci node: method "smc", cpu_suspend 0xc4000001, cpu_off 0x84000002, cpu_on 0xc4000003 —
  identical to community DT IDs. So community ports replicate Samsung's own SMP conduit
  (PSCI-over-SMC to sboot) *including* the M4 cluster; nothing in the stock mechanism requires
  keeping M4 offline.
**INFERENCE** The community 9820 mainline port's CPU stack = PSCI-over-SMC (sboot monitor) for
cpu_on/off/suspend + ACPM IPC for DVFS (ACPM firmware lives in the sboot image). It does NOT
reimplement Samsung's CPU bringup; it reuses it — hence "SMP bringup works under the Samsung
firmware stack" (Q4) is supported by construction, and the observed quirks are in that firmware
(AFFINITY_INFO, ACPM FREQ_GET) plus disabled cpuidle states (wiki: suspend lockups).

**FACT** U-Boot defconfig OF_LIST = beyond1lte + beyond2lte only; dts/upstream dir has dts for
beyond1lte/beyond2lte only. **No beyond0lte (S10e) DT in the pOS mainline boot chain** (as of the
exynos9820 branch tree fetched 2026-10-21). Wiki generic page image is an S10; S10e also has no
dedicated wiki page (wiki search "beyond"/"Galaxy S10").

## R2 (static RE): uH stage-2 fault path — answered

Artifacts: m4/artifacts/uh.disasm.txt (GHWC1==HXJ6 code per F12). Function boundaries below are exact.

- **EC dispatch**: 0x8701a16c: `ec=(ESR>>26)&0x3F; if ec-0x12 <= 0x13` -> jump table @0x87020480 (16
  entries; EC 0x12..0x25). EC 0x20 (instr s2 fault), 0x24/0x25 (data s2 fault) handled; others -> -1.
- **Pre-handler 0x87016aec**: returns 1 (fault "handled" = plain RETRY) iff global counter
  [0x870430c0] != 0. That counter is the in-flight table-update counter, incremented by `ldaxr/stlxr`
  in the table writers (0x87017ddc, 0x87017e68...). => stage-2 faults racing a table update simply
  re-execute. THIS is the concurrency answer for "two cores fault at once": second fault retries.
- **Fault flow** (0x8701a234 read-fault, 0x8701a2bc write-fault arms): dsb sy -> log ESR +
  (hpfar<<8) via 0x8700c4f4 -> if FAR > 0x8000000000: `at s12e1r` (EL1 ctx) else `at s12e1r`/`at s12e0r`
  (test-at) -> `isb; mrs par_el1; if !(PAR & 0x201)` translation OK -> return => **replay the same
  guest instruction unchanged** (it can only succeed if the mapping was static or made valid by someone).
- **ISV branch**: at the write-fault arm `tbnz ESR,#6` (bit24 ISV) selects the replay-at variant; with
  ISV=0 the fallback chain at 0x8701a378-0x8701a3f0 runs under the 0x870435c8 qspinlock:
  0x87010fa0 (fetch guest instruction at ELR) -> 0x870190fc (classify) -> 0x87016590; then two more
  descriptor variants (0x87018e58->0x870166d4, 0x87018848->0x87016818) -> 0x87019f94 (retry/accept
  decision) -> "cannot recover" tail 0x8701a5a0 (logs line 0x4d6; also handles SP/VBAR-class system
  instructions via bit7/bit9 tests of ESR.SVC). **Decoder internals NOT fully mapped this pass** —
  the exact LDP/STP/LSE/LDXR/DC-ZVA/FP coverage list stays OPEN; recorded as dead-end (time-boxed).
- **Insert helpers 0x87015280 / 0x870155bc / 0x870157fc**: all take the 0x870435d8 qspinlock
  (0x8700c858 = ldaxr/stxr qspinlock; 0x8700c8b4 unlock), lazily allocate table pages (0x8700b4f0),
  and walk a **4-level tree rooted at TTBR0_EL2** (indices bits38:30 / 29:21 / 20:12 / 11:3 ->
  4K final). 2 MiB blocks: 0x87015948 splits a valid block into an L3 table (block->table conversion).
  => **runtime-inserted trees belong to uH's OWN EL2 stage-1 (identity over the IPA space), not the
  guest's stage-2**. The guest s2 root is the STATIC table: `msr vttbr_el2,#0x87028000` appears only
  in an init/re-install path (0x87012e24, after asserting a 0xFFFFFFF9000000 tag check) and
  `msr vttbr_el2,xzr` (stage-2 OFF variant, 0x870172ac); VTTBR-rooted walks (0x8700a120 via 0x87013904)
  are READ-ONLY verification (walk L1[31:30]/L2[29:21]/L3[20:12], reject overlap with valid entries).
  **uH never extends the guest stage-2 at runtime.**
- **b. replace/split + BBM**: yes — entries are replaced/created/split, always break-before-make:
  `str xzr,[table,idx]` -> `bl 0x870118f4` (isb; TLBI IPAS2E1IS range loop; dsb sy; TLBI VMALLS12E1IS;
  dsb sy; isb) -> `str new` -> same invalidation again (see 0x87017e10-0x87017e24, 0x87016b30-0x87016b44).
- **c. TLBI forms**: ALL broadcast + `dsb sy`. Inventory at 0x8701187c..0x870119a0: `tlbi vae2is` range,
  `tlbi ipas2e1is` range (x2 variants), `tlbi vmalle1is`, `tlbi vaae1is` range, `tlbi alle2is`,
  `tlbi vmalls12e1is`. NO local-only (non-IS) invalidations anywhere in uh.bin. The "stale s2 on other
  cores via local TLBI" failure mode is ruled out statically.
- **f. bank3/bank4 static coverage** (regenerated from m4/artifacts/uh-stage2-map.txt): valid 2 MiB
  blocks = identity Norm-WB **SH=Non** AF=1 AP=1: 0x8000000-0x100000000 (gaps: uH window 0x87000000-
  0x87200000 = RO 4K pages 0x47f; plus 1 Dev block @0x2000000 AF=0 unused), and **bank 3 0x880000000-
  0x8C0000000 fully**. Bank 4 (0x900000000) and everything else unmapped -> fault path that can never
  resolve (guest s2 static). DTB memory nodes: DRAM = 2G@0x80000000 + 2G@0x880000000 + 2G@0x900000000
  (6 GB). **Actionable for G30: keep the UEFI/Windows map inside 0x8000000..0xFFFFFFFF and
  0x880000000..0x8BFFFFFF; 0x900000000+ must stay out of the RAM ranges handed to Windows** — a page
  touched there faults forever (retry loop or dead emulated path), which fits "heap comes out wrong"
  IF any code put memory there (cross-check our PUBLISH_HIGH_BANKS range generator against
  0x8C0000000!).

## R3 (static): secondary EL2 state — half answered

- uH ELF entry 0x87001000: requires EL2, stack = per-CPU slot (sp = &0x87046000 - 0x110*slot), then one
  common C init (bl 0x87012d00) — identical on every core that reaches it.
- MPIDR used at exactly 2 sites (0x87010ec4 affinity-index helper; 0x87010ee0 all-CPU affinity loop);
  **uH contains no MIDR read, one VBAR_EL2 write (init -> 0x8701b800), and VTTBR only static-or-zero**.
  uH has no warm-boot/secondary entry of its own and never registers with a monitor.
- sboot thunk table (0x45d0-0x4b3c: MRS/MSR pairs for EL1/EL2/EL3 incl. hcr/vttbr/vbar/actlr + the
  S3_1_c15_c2_1 M4 reg) is a data-driven save/restore library; sboot's own EL2 vector setups (0x16030,
  0x28430, 0xa4030) install sboot-local VBAR_EL2 (0x27800/0x29400/0xa4800) early, before uH replaces it.
- The consumer of the thunks on CPU_ON is the EL3 monitor (binary not present on any LUN, F15) =>
  static dead end CONFIRMED; P2 (hvc #0 probe per core class) remains the only decider for H1a/H1b.

## R4 (static): IMP-DEF sysreg sweep — answered (no M4-specific gating)

- **uH: zero IMP-DEF accesses** (no S3_x, no actlr, no midr).
- **sboot (both GHWC1 and HXJ6 identical)**: named-IMP-DEF-free raw set = one write each to
  S3_0_c3_c9_2, S3_1_c12_c15_1, **S3_1_c15_c2_1 (M4 fault-syndrome)**, S3_1_c2_c1_7, S3_3_c9_c9_4,
  S3_5_c10_c15_2 x3, S3_6_c13_c10_2, S3_6_c3_c13_5, S3_6_c5_c1_4 (+thunk writes actlr_el1/el2/el3,
  cntkctl/cpacr per-CPU). Sites 0xbab20-0xbab64 branch on MIDR PartNum only for generic ARM parts
  (0xc0f/0xd03/0xd07/0xc07...); **no 0x53xx (Mongoose) compare anywhere in sboot or uH** -> every
  M4-ish IMP-DEF write is unconditional, executed on whatever core runs the path (cold boot; the
  thunks run wherever sboot/monitor calls them — monitor-side use unresolvable, see R3).
- **cm/TZ**: S3_3_c4_c7_4 (= CPUACTLR_EL1 class) + S3_6_c15_c9_1 + others single writes — TZ-side
  core setup exists; monitor/CPU_ON usage unknown (binary context missing).
- => "Samsung hides an M4 erratum behind MIDR in sboot/uH" is NEGATIVE statically; any M4 workaround
  is either unconditional, in the EL3 monitor blob (unavailable), or in ACPM (SoC-internal).

## R7: reserved-memory cross-check — answered

- 9820 sboot repack writes exactly: **sboot,rmem @0xC9000000 size 0x1000000 (16 MB)** and
  **framebuffer/sboot,fb @0xCA000000 size 0x1400000 (20 MB)** (disasm 0x1037d0/0x103734). K10's
  0x1700000 is superseded by these live values.
- Repo ground truth agrees: firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h
  EXYNOS_FB_BASE=0xCA000000 (VERIFIED, s3cfb.bootloaderfb cmdline), our reservation 10 MiB
  (0xCA000000..0xCA9FFFFF). So the ranges the firmware hands Windows (0xC9000000-0xCA000000 and
  0xCAA00000-0xCB400000) match the sboot-reserved blocks; sboot itself does NOT touch the
  0x0A000000-0x14000000 middle part beyond the framebuffer base node — but uH's static s2 maps all
  of it as ordinary DRAM, so it is safe as RAM as long as no consumer writes the FB window.

## Sources (static)
- m4/artifacts/uh.disasm.txt, uh-stage2-map.txt, sboot.disasm.txt, sboot-ghwc1.disasm.txt,
  cm6.disasm.txt, dumps/hhxj6/base.dts (memory/devfreq nodes). No web sources in this section.

**FACT** Community exynos9820.dtsi cpucl2_opp_table (M4 cluster): 19 OPPs, 520,000,000 Hz ..
2,530,000,000 Hz with opp-microvolt 587500..1018750 (opp-shared, clock-latency-ns 500000).
No 2.73 GHz entry (Samsung's advertised M4 max) — top of this table is 2.53 GHz.
faveoled's uniLoader-era board DTS gist bootargs are only "pmos.nosplash" — no maxcpus/isolcpus
(https://gist.github.com/faveoled/e7c44486feb4b65ccf014c254e0f542c).

**FACT** Upstream Mongoose plumbing is DT-compatible-only: drivers/perf/arm_pmuv3.c has
PMUV3_INIT_SIMPLE(armv8_samsung_mongoose) + of_match "samsung,mongoose-pmu"
(https://github.com/torvalds/linux/blob/master/drivers/perf/arm_pmuv3.c); cpufeature.c and
cputype.h contain no Samsung implementer (0x53)/Mongoose part numbers. Downstream 9820 kernel
defines ARM_CPU_IMP_SEC=0x53, ARM_CPU_PART_CHEETAH=0x003 (M4), MIDR_CHEETAH
(https://github.com/PixelExperience-Devices/kernel_samsung_exynos9820/blob/thirteen/arch/arm64/include/asm/cputype.h)
and its cpu_errata.c contains **no cheetah/M4-specific erratum entries** (grep) — i.e. even
Samsung's own 4.14 kernel ships no public M4 errata workarounds; its protections are RKP/DEFEX
and the EL2 H-Arx stack, not CPU-erratum code.

### Answers

**A1. Do pOS/mainline support S10/S10e/S10+ (exynos9820)?**
- Upstream mainline: **No.** No exynos9820 DT, clk, pinctrl, PMU-SoC or power-domain driver
  exists in torvalds/master (dir listing + tree search). 9810 (M3) and 990 (M5) went upstream
  Oct 2024 with minimal DT (cpus/pinctrl/simplefb); **9820 never had an upstream submission**
  (LKML/LWN-adjacent archive searches; dead ends logged).
- postmarketOS: **Yes for S10/S10+ via a generic mainline-based port.** device-samsung-exynos9820
  ("Samsung Exynos 9820 Generic Device") + linux-postmarketos-exynos9820 7.2.0 (close-to-mainline
  fork of torvalds linux 7.2-rc2) are packaged in master (testing category); wiki page for the
  generic device and for samsung-beyond1lte / samsung-beyond2lte. Boot chain = sboot + U-Boot
  fork (or the older uniLoader shim route with a hand-made DT, SimpleFB only).
- **S10e (beyond0lte): no dedicated pOS page and no beyond0lte DT in the U-Boot OF_LIST** — the
  mainline port as published targets beyond1lte/beyond2lte only. S10e coverage elsewhere is
  downstream-kernel based: UBports Ubuntu Touch (Halium-10/12, xenial-aggregated page says
  unmaintained, last commit 2025-02-26), LineageOS (official, through Android 16 builds),
  TWRP etc.
- pOS feature completeness is handset-basic: display/touch/battery partial, no wifi/BT/audio/
  camera/modem/3D, no pmbootstrap flashing (TWRP/dd), suspend broken.

**A2. Are all 8 CPUs online/stable in those ports, or limited (maxcpus/isolcpus/offline cluster)?**
- **No core-count limit found anywhere.** The port DT defines all 8 CPUs (incl. both M4 at
  MPIDR 0x100/0x101) enabled, with OPP tables (M4: 520MHz–2.53GHz, 19 bins), capacities
  (M4=785 DMIPS/MHz vs A75=531 vs A55=272), ACPM DVFS clocks and a Mongoose PMU node
  (GIC_SPI 174/175). bootargs/deviceinfo carry no maxcpus/isolcpus.
- What *is* limited: **per-CPU cpuidle "c2" sleep states are status="disabled" for all three
  clusters**, and the wiki lists "Suspend is currently broken... unexpected lockups — disable
  automatic suspend". CPU hotplug/DVFS ride on sboot's PSCI-over-SMC + ACPM (comments there flag
  PSCI AFFINITY_INFO and ACPM FREQ_GET as broken in firmware).
- UnixBench "8 CPUs in system" on the beyond1lte wiki page proves all 8 were online under the
  downstream-based 4.14 Nura kernel (Dec 2021) — not the mainline port, but same sboot/PSCI
  bringup conduit.
- INFERENCE: for M4 specifically, the public evidence says mainline-style ports run both M4 cores
  with DVFS through ACPM; stability caveats they document (suspend/idle) apply cluster-wide, not
  M4-only.

**A3. Any reports of memory corruption/hangs on 9820 mainline that disappear with M4 offline?**
- **None found.** Searched pOS wiki (Known Issues has no such entry; core-count is never
  mentioned), the UT port's gitlab issue list (only wifi/libertine/bootloader-loop type issues),
  both community GitHub repos' issues (1 issue, clock-driver related), LKML/LWN archive mirrors,
  and general web queries (maxcpus/isolcpus/M4-offline/corruption terms). Also: neither upstream
  nor Samsung's downstream 4.14 kernel carries an M4-specific erratum workaround — nothing in the
  public record suggests anyone keeps M4 offline to dodge corruption. Closest documented issues:
  suspend lockups (idle states disabled in DT), broken PSCI AFFINITY_INFO, broken ACPM FREQ_GET.

**A4. Do 9820 DTs enable CPU nodes 6-7 with OPPs; does SMP bringup work under the Samsung
firmware stack (sboot/uH intact)?**
- Upstream: there is no exynos9820.dtsi — premise false (see A1). Community DTs: yes, both M4
  nodes enabled with full OPP/capacity/PMU (details above).
- SMP bringup: **reuses Samsung's own conduit.** Downstream 9820 DT and the community DT declare
  identical psci nodes (method "smc", cpu_on 0xc4000003, cpu_off 0x84000002, cpu_suspend
  0xc4000001) and identical M4 MPIDR affinities (0x100/0x101). So CPU_ON for the M4s is serviced
  by sboot's EL3 monitor (standard PSCI subset over SMC); the ports don't replace it. U-Boot
  runs as a second-stage under sboot (SKIP_LOWLEVEL_INIT, SAVE_PREV_BL_FDT_ADDR); the older route
  used uniLoader (same shim approach as upstream Exynos9810/990 docs recommend precisely because
  S-Boot is non-replaceable). ACPM (power/DVFS MCU, blob inside the sboot image) stays in charge
  of cluster DVFS — ports talk to it via samsung,exynos9820-acpm-ipc.
- EL2/uH: stock S10 flow has sboot verify & load **uh.bin (H-Arx hypervisor core at 0xc0000000,
  uH plugin at 0xc1400000, EL2)** before the payload (BlackHat us-20; dayzerosec 2025). The
  community ports do not document touching/removing that stage; no source says whether uH is
  resident when a custom boot.img is loaded (open point; no evidence either way that ports
  displace the EL2 stack).

### Sources (R5)
- https://wiki.nura.eco/wiki/Samsung_Exynos_9820_(samsung-exynos9820) (postmarketOS wiki, now
  served at nura.eco; api.php parse)
- https://wiki.nura.eco/wiki/Samsung_Galaxy_S10_(samsung-beyond1lte)
- https://wiki.nura.eco/wiki/Samsung_Galaxy_S10%2B_(samsung-beyond2lte)
- https://wiki.nura.eco/wiki/Samsung_Exynos_9820/9825 (SoC page, empty status fields)
- https://pkgs.nura.eco/package/main/postmarketos/aarch64/linux-postmarketos-exynos9820
- https://repology.org/project/device-samsung-exynos9820/packages
- http://mirror.math.princeton.edu/pub/postmarketos/master/aarch64/ (device+kernel apks)
- https://github.com/chiffathefox/exynos-9820-mainline-linux (branches exynos9820-7.1-rc2 /
  -7.2-rc2; commits API)
- https://github.com/chiffathefox/u-boot/tree/exynos9820 (configs/exynos9820_defconfig;
  dts/upstream/src/arm64/exynos/exynos9820.dtsi)
- https://gist.github.com/faveoled/e7c44486feb4b65ccf014c254e0f542c
- https://github.com/ivoszbg/uniLoader (commit c4d3f6490eda8180ddf673d984b078324fa38d9c;
  configs/beyond1lte_defconfig; board/samsung/board-beyond1lte.c)
- https://api.github.com/repos/torvalds/linux/contents/arch/arm64/boot/dts/exynos (no 9820 files);
  https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/arm/cpus.yaml;
  arch/arm64/{include/asm/cputype.h,kernel/cpu_errata.c,kernel/cpufeature.c},
  drivers/perf/arm_pmuv3.c (raw fetches, greps)
- https://lists.infradead.org/pipermail/linux-arm-kernel/2024-October/972766.html (Exynos9810
  series + uniLoader rationale); .../973912.html (M3 binding applied);
  https://lists.openwall.net/linux-kernel/2024-10-28/632 ;
  https://lists.openwall.net/linux-kernel/2024-10-15/1797 (Exynos990 series)
- https://github.com/PixelExperience-Devices/kernel_samsung_exynos9820 (thirteen: exynos9820.dts
  psci+cpu nodes; cputype.h 0x53/0x003 Cheetah; cpu_errata.c; exynos-smc.S)
- https://gitlab.com/ubports/porting/community-ports/android10/samsung-galaxy-s10/samsung-exynos9820 ;
  https://devices.ubuntu-touch.io/device/beyond0lte/release/xenial/ ;
  https://xdaforums.com/t/community-ubuntu-touch-for-s10-s10e-s10-beyond1lte-beyond0lte-beyond2lte.4355013/ ;
  https://forums.ubports.com/topic/7566/... (S10e/S10/S10+ install guide)
- https://etc.chipmunk.land/android/writeups/us-20-Chao-Breaking-Samsungs-Root-Of-Trust-Exploiting-Samsung-Secure-Boot.pdf ;
  https://teamt5.org/en/blog/blackhat-s-talk-breaking-samsung-s-root-of-trust-exploiting-samsung-secure-boot ;
  https://dayzerosec.com/blog/2025-03-08/reversing-samsungs-h-arx-hypervisor-part-1.html ;
  http://allsoftwaresucks.blogspot.com/2019/05/reverse-engineering-samsung-exynos-9820.html
- https://github.com/LineageOS/android_device_samsung_exynos9820-common (init.exynos9820.rc via
  review.lineageos.org gerrit); https://wiki.lineageos.org/devices/beyond1lte/
- https://github.com/chiffathefox/exynos-9820-mainline-linux/issues/1 (display/clock upstreaming
  activity, 2026)

### Dead ends (R5)
- wiki.postmarketos.org HTML + lore.kernel.org: Anubis JS challenge (curl 403); only api.php
  redirect to wiki.nura.eco worked. gitlab.com (pmaports/apkbrowser): Cloudflare.
- gitlab.postmarketos.org (new forge): sign-in required for web+git; pmaports!8999/2739/6161 and
  pmaports issue threads unread → relied on wiki/apk/repo evidence.
- wiki.mentallysanemainliners.org: connection failed (curl 000); mainliners' 9820 status page
  unread (if any).
- No Wayback snapshots for pmaports MR 8999 (archive.org availability API empty).
- Searches for "exynos9820 maxcpus / M4 offline / mongoose corruption / mongoose crash" returned
  no matching bug report (multiple engines/terms; results dominated by downstream Android kernel
  repos).
- pOS kernel package contains no 9820 dtb — DTB location resolved to U-Boot fork, but installed
  .dtb on-device not byte-verified (device not flashed for this research).
#### Q1 cross-check in the current numbering — DDI 0487**K.a** (Armv9.3, Mar 2024)

FACT. Second full ARM ARM mirror: `http://kib.kiev.ua/x86docs/ARM/ARMARMv8/DDI0487K_a_a-profile_architecture_reference_manual.pdf`
(87 MB, 14778 pages; local `/tmp/opencode/DDI0487K_a_*.pdf`, extract `/tmp/opencode/d8_6.txt`; page label
offset again 0 - printed D8-6694 = PDF page 6694).

Modern section numbers (K.a): **D8.6 "Memory region attributes"**; FWB=0 typing/caching in
**D8.6.5 "Stage 2 memory type and Cacheability attributes when FWB is disabled"** (printed page
D8-6690) with **D8.6.5.1 "Combining stage 1 and stage 2 memory type attributes"** (Table D8-91,
p. D8-6691) and **D8.6.5.2** cacheability (Table D8-92); shareability in
**D8.6.7.1 "Combining the stage 1 and stage 2 Shareability attributes for Normal memory"**, printed
pages D8-6694/6695, **Table D8-97**. The table is byte-for-byte the same 7 rows as G.a Table D5-45,
including "Inner Shareable | Non-shareable | Inner Shareable".

FACT (verbatim, K.a D8.6.7.1, rule ID ILTFXF, p. D8-6694):
> "The value of HCR_EL2.FWB does not affect how Shareability attributes from stage 1 and stage 2 are combined."

FACT (verbatim, K.a D8.6.7.1, rule ID RQPHXK):
> "If the resultant memory type from a stage 1 and stage 2 translation is one of the following, the
> memory region is treated as Outer Shareable: • Any Device memory type. • Normal Inner
> Non-cacheable, Outer Non-cacheable."

FACT (verbatim, K.a D8.6.5, rule IDs RMFDHF/RQPHXK/RTNHFM): "All statements in this section and
subsections require that the Effective value of HCR_EL2.FWB is 0." / "If the Effective value of
HCR_EL2.FWB is 0, then the stage 1 and stage 2 memory type and Cacheability attributes are combined."
Table D8-91 rules are the same four precedence rows as G.a Table D5-43 ("Device has precedence over
Normal", "Non-Gathering has precedence over Gathering", "Non-Reordering has precedence over
Reordering", "No Early write acknowledge has precedence over Early write acknowledge").

FACT (K.a Table D8-90 "Stage 2 MemAttr[3:0] encoding", p. D8-6690, FWB=0): 0000 Device-nGnRnE,
0001 Device-nGnRE, 0010 Device-nGRE, 0011 Device-GRE, 0101 Normal Outer/Inner Non-cacheable,
0111 Normal Outer NC / Inner WB, 1111 Normal Outer WB / Inner WB, 1000 and 1100 Reserved
(CONSTRAINED UNPREDICTABLE). So stage-2 MemAttr **0b101 is Normal Non-cacheable, never Device**;
Device is only MemAttr[3:2]==0b00 (0b0000-0b0011).

INFERENCE: for the M4/9820 case (Armv8.2 core, no FEAT_S2FWB advertised) only the FWB=0 path exists
in hardware, so the combine tables above are the whole story: stage 2 can only weaken cacheability and
strengthen shareability; it can never reduce shareability below the stage-1 value, and any Device at
either stage pins the mapping to Device + Outer Shareable.

### Q3 — Samsung M4 / Mongoose errata hunt

FACT (local evidence, no errata registry entry). Samsung's own downstream Exynos 9820 kernel tree
(mirror of the GPL S10/Note10 kernel), `https://github.com/CruelKernel/samsung-exynos9820`
(branch cruel-HVJ5-v5.3, HEAD 41aba77656e3377e8c55691b8b807d915e545343, last commit 2023-09-04),
contains `arch/arm64/kernel/cpu_errata.c` and `Documentation/arm64/silicon-errata.txt` with **no
Samsung/Exynos/M4 entries at all** (verified: `grep -i -E "samsung|exynos" ...` returns nothing in
either file, local copies `/tmp/opencode/k9820/...`).

FACT (same tree, S10e defconfig `arch/arm64/configs/exynos9820-beyond0lte_defconfig`, the exact
board this port targets). The only CPU errata workarounds Samsung enables are stock ARM ones:
ARM64_ERRATUM_826319, 827319, 824069, 819472, 832075, 845719, 843419, 1024718, 1188873,
QCOM_FALKOR_ERRATUM_E1041, FSL_ERRATUM_A008585, HISILICON_ERRATUM_161010101, ARM64_ERRATUM_858921
(plus ARM_ARCH_TIMER_OOL_WORKAROUND). There is **no CONFIG for an Exynos/M4 core erratum**
(grep of "ERRATUM|WORKAROUND" over that defconfig, 20 hits total).
INFERENCE: either the M4 has no publicly-worked-around errata in the Linux kernel, or Samsung's
workarounds live in firmware (sboot/bl1/bl2) rather than the kernel - the kernel tree gives no
evidence either way, so the second possibility is unproven.

FACT (device-tree names, same tree, `arch/arm64/boot/dts/exynos/exynos9820.dts`). The three CPU
clusters are named by ARM internal codenames in the compatible strings:
- `cpu@0000`-`cpu@0003`: `compatible = "arm,ananke", "arm,armv8"` (the 4x Cortex-A55), reg 0x0-0x3
- `cpu@0004`,`cpu@0005`: `compatible = "arm,prometheus", "arm,armv8"` (the 2x Cortex-A75), reg 0x4,0x5
- `cpu@0100`,`cpu@0101`: `compatible = "arm,cheetah", "arm,armv8"` (**the 2x Exynos M4**, reg 0x100, 0x101)
So "M4 = cheetah" (matching the NamuWiki statement that the M4 development code name was Cheetah:
`https://en.namu.wiki/w/%EC%82%BC%EC%84%B1%EC%A0%84%EC%9E%90/%EB%A7%88%EC%9D%B4%ED%81%AC%EB%A1%9C%EC%95%84%ED%82%A4%ED%85%8D%EC%B2%98`).
All use `enable-method = "psci"`. In `cpu-map` the A55s and A75s are `coregroup0`/`coregroup1` of
`cluster0`, and the two M4 (cheetah) cores are the only cores of `cluster1`; the Samsung scheduler
node in the same file lists `vcluster_cnt = <0x3>`, `vcluster0_sibling = "0-3"`,
`vcluster1_sibling = "4-5"`, `vcluster2_sibling = "6-7"`.
INFERENCE: the MPIDR of the M4 cores carries a different Aff1 (0x100/0x101 => Aff1=1, Aff0=0/1), i.e.
the M4 pair is a separate coherency/physical cluster from the A55+A75 group; secondary-core bringup
must target those MPIDR values, and "Inner Shareable" on the M4 pair means those 2 cores only.

FACT. `grep -i -E "mongoose|_m4|m4_"` over the whole 71 165-file kernel file list returns no
Samsung CPU errata file; and no file in the tree is named like an M4 errata workaround.
Dead end: this avenue (kernel-embedded errata names) is exhausted for 9820.

#### Q3 continued — the one documented Samsung-custom-core CPU bug (9810/M3, not M4)

FACT. `https://reviews.llvm.org/D114523` (landed as
`https://github.com/llvm/llvm-project/commit/cce4a7258b81159e57a411896011ee2742f17def`, Dec 14 2021),
summary verbatim:
> "Big.LITTLE Heterogeneous architectures, as described by ARM [1], require that the instruction set
> architecture of the big and little cores be compatible. However, the Samsung Exynos 9810 is known to
> have different ISAs in its core. According to [2], some cores are ARMv8.2 and others are ARMv8.0.
> Since LSE is for ARMv8.1 and later, it should be disabled for this broken CPU."
and the shipped code comment in `compiler-rt/lib/builtins/cpu_model.c`:
> "// Some cores of Exynos 9810 are ARMv8.2 and others are ARMv8.0, // so disable the lse atomics
> // completely." (gated on Android property `ro.arch` == "exynos9810")
Reviewer note (srhines): the fix matters mainly for NDK-built apps; jyknight: "problems have only been
reported on the Galaxy S9 and S9+ devices, running older versions of the device firmware (based on
Android 8) ... presumably the kernel in the more recent builds has been updated to (correctly) not
report HWCAP_ATOMICS as available."

FACT. The underlying report: `https://github.com/golang/go/issues/28431` ("cmd/compile: go binaries not
working on exynos 64 bit CPUs", Oct 2018, fixed for Go 1.12). On a Galaxy S9+ (Exynos 9810) a
userland `LDADDAL` executed on a big core raised **SIGILL**; /proc/cpuinfo on that device showed
big cores `CPU implementer : 0x53, CPU variant : 0x1, CPU part : 0x002, CPU revision : 0`
(= Samsung Mongoose M3, ARMv8.0, **no FEAT_LSE**) and little cores
`CPU implementer : 0x41, CPU part : 0xd05, CPU revision : 1` (= Cortex-A55 r0p1, ARMv8.2, has LSE),
with the *same* `Features:` string including `atomics` on both - i.e. the kernel was reporting the
union of features. Later comment in the same issue: "Update: the kernel problem was resolved in an
update. The kernel no longer reports support for atomics."
FACT (the rule cited there): "ARM documentation states that the big and little cores must have the same
instruction support" - `https://developer.arm.com/technologies/big-little`.

INFERENCE for this port: the 9810 class of bug (a custom Samsung core implementing an *older* ISA than
its companion Cortex cores, plus firmware/Linux reporting the union of features) is the single public
"Exynos custom core erratum-like" issue found. It does **not** apply to the 9820 in the same form:
M4 is ARMv8.2 (`https://fuse.wikichip.org/news/2051/samsung-discloses-exynos-m4-changes-upgrades-support-for-armv8-2-rearranges-the-back-end`:
"The Exynos M4 finally introduces ARMv8.2 support - including full FP16 scalar instructions and the
integer dot product extension ... which means all the cores on the Exynos 9820 should be largely
ISA-compatible"), i.e. LSE is present on the M4 too. Still worth an ID-register check per cluster on
real silicon before trusting `ID_AA64ISAR0_EL1.Atomic` for the whole SoC.

FACT (naming gap). Mainline Linux documents Samsung custom cores in
`https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/arm/cpus.yaml`
(lines 231-233) as `samsung,mongoose-m2`, `samsung,mongoose-m3`, `samsung,mongoose-m5` - **there is no
`samsung,mongoose-m4` binding** as of today. The M3 binding was added Oct 2024 in
`https://lists.infradead.org/pipermail/linux-arm-kernel/2024-October/972767.html`.
INFERENCE: a mainline 9820 dts would have to add `samsung,mongoose-m4` first; nothing in mainline
carries an M4 erratum workaround.

DEAD ENDS so far for "M4 errata sheet":
- No TRM for the M4/Mongoose cores is public. Samsung's public "Exynos 9820" product page has only
  marketing-level info (`https://semiconductor.samsung.com/processor/mobile-processor`,
  `https://news.samsung.com/global/the-exynos-9-series-9820-intelligence-from-within`). Searches for
  "S5E9820 TRM"/"Exynos 9820 technical reference manual" return only ARM Cortex-M4 TRMs (different "M4")
  - the "M4" string collision makes this search class useless unless combined with "Mongoose"/"Exynos".
- No "software optimization guide" for M4 exists publicly (searches returned only OneUI battery guides).
  ARM publishes SOGs only for its own cores; Samsung custom cores have none.
- No arm-community-erratics entries for Samsung cores found (search returned unrelated groups).
  ARM errata pages exist only for ARM-designed cores; Samsung's custom cores are not in the
  erratics/`developer.arm.com/a/` errata database.
- `Documentation/arch/arm64/silicon-errata.rst` in mainline has no Samsung vendor row (grep for
  "samsung" over the raw file returns nothing).

### Q4 — does anything public say the 9820/M4 cluster needs Inner-Shareable mappings, or ignores Non-shareable?

FACT. No such public statement was found (searches on "Exynos 9820" + shareability / DSU / coherent
interconnect / snoop filter, and on "Mongoose M4" + shareability, produced nothing core-specific).
Treated as a dead end; the honest answer is **no vendor statement exists publicly**.

FACT (closest hardware statement that is public): the ISCA 2020 industry paper by the SARC core team,
"Evolution of the Samsung Exynos CPU Microarchitecture" (IEEE ISCA 2020 Industry Track; PDF mirror
`https://people.engr.tamu.edu/djimenez/pdfs/exynos_isca2020.pdf`, publisher page
`https://dl.acm.org/doi/10.1109/ISCA45697.2020.00015`), says of the memory system (section X,
"Reducing main memory latency"):
> "The coherent interconnect contains a snoop filter directory that is normally looked up in the path
> to memory access for coherency management."
and in the abstract/intro:
> "The cores are superscalar out-of-order designs capable of up to 2.9GHz, and employ up to three
> levels of cache hierarchy in a multi-node cluster. Each Exynos M-series CPU cluster is complemented
> by an ARM Cortex-A series cluster in a big/little configuration in generations one through three, and
> in a big/medium/little configuration in subsequent generations."
Table I lists for **M4**: L1I 64KB 4w, L1D 64KB 4w, "L2 Cache 1024KB 8w", "L2 Shared/Private: private",
L3 "3072KB", "L3 Ways & Banks: 16w 3 bank", process 8nm LPP, product frequency 2.7GHz; for M5 the L2 is
"shared by 2 cores". The paper notes the M3->M4 size reductions in section III: "M4's reduction in L3
size due to changing from a 4-core cluster to a 2-core cluster".
FACT: the paper contains **no** mention of shareability, Device memory, barriers, TLB/shareability
programming, or any restriction on Non-shareable mappings (grep for shareab/device memory/barrier/DSB/DMB
over the extracted text finds none). So it neither imposes nor relaxes anything about shareability.
INFERENCE: the M4 pair is a private-L2 + shared-3MB-L3 cluster whose coherency is maintained by a
snoop-filter directory in the interconnect; a hardware snoop filter that tracks lines by address means
the core/cluster does not depend on the software shareability hint for correctness - which is exactly
what the architecture allows (next bullet).
FACT (same paper, section III Microarchitectural Overview): "Two examples are M3's reduction in L2 size
due to the change from shared to private L2 as well as the addition of an L3, and M4's reduction in L3
size due to changing from a 4-core cluster to a 2-core cluster."

FACT (architecture already covers the "ignores Non-shareable hints" question, DDI 0487G.a B2.7.1
"Shareable Normal memory", printed page B2-166):
> "System designers can use the shareability attribute to specify the locations in Normal memory for
> which coherency must be maintained. However, software developers must not assume that specifying a
> memory location as Non-shareable permits software to make assumptions about the incoherency of the
> location between different PEs in a shared memory system. Such assumptions are not portable between
> different multiprocessing implementations that might use the shareability attribute. Any
> multiprocessing implementation might implement caches that are shared, inherently, between different
> processing elements."
and, same page:
> "This architecture assumes that all PEs that use the same operating system or hypervisor are in the
> same Inner Shareable shareability domain." (repeated as "The Inner Shareable domain is expected to be
> the set of PEs controlled by a single hypervisor or operating system.")

**Answer for the port (INFERENCE from those facts):** Non-shareable is only a hint and hardware is free
to be coherent anyway, so an M4 that "ignores" Non-shareable is architecturally legal and no erratum is
needed to explain it; conversely, since the ARM ARM assumes all PEs under one OS/hypervisor are in the
same Inner Shareable domain, everything Windows shares between the M4 pair, the A75 pair and the A55
quad must be mapped at least Inner Shareable - there is no supported configuration in which a
Non-shareable Normal-WB mapping is safe for inter-PE data, and Device/Normal-NC mappings are forced to
Outer Shareable anyway (Q1/Q2 above).

FACT (topology, secondary source): Arm's DSU product page describes the "DynamIQ Shared Unit - Enabling
DynamIQ big.LITTLE for Armv8.2 CPUs, up to 8 cores (4 big max), and up to 4MB L3"
(`https://www.arm.com/technologies/dynamiq`), and Chips and Cheese: "Arm's first DSU, the one introduced
with A55 and A75, supports up to 4 MB of L3"
(`https://chipsandcheese.com/p/arms-cortex-a55`). Combined with the 9820 dts cpu-map (one `cluster0`
holding the 4 A55 + 2 A75 as two `coregroup`s, and a separate `cluster1` holding the 2 cheetah/M4 cores),
INFERENCE: the 9820 CPU subsystem is a Cortex DSU-style hexa-core cluster **plus** a separate Samsung
2-core M4 cluster with its own 3 MB L3; the M4 side is not a DSU, so no DSU TRM errata apply to it, and
cross-cluster coherency is handled by the SoC coherent interconnect with snoop filter directory (ISCA
paper). Whether the implementation's Inner Shareable domain spans both clusters is not stated publicly -
INFERENCE: it must, because Linux/Android run SMP across all 8 cores with ISH barriers (DMB ISH /
`dsb(ish)`) and rely on the ARM ARM's IS assumption above; no public Exynos 9820 document says otherwise.

#### Q3 extra — how the M4 core is identified in software (MIDR), and the only M-series ISA quirk in LLVM

FACT. `https://raw.githubusercontent.com/llvm/llvm-project/main/llvm/lib/TargetParser/Host.cpp`
(lines 329-352) maps Samsung's MIDR (implementer **0x53**) to core names:
> "if (Implementer == "0x53") { // Samsung Electronics Co., Ltd.
>   // The Exynos chips have a convoluted ID scheme that doesn't seem to follow
>   // any predictive pattern across variants and parts.
>   ...
>   unsigned Exynos = (Variant << 12) | PartAsInt;
>   switch (Exynos) {
>     default: // Default by falling through to Exynos M3.  [[fallthrough]];
>     case 0x1002: return "exynos-m3";
>     case 0x1003: return "exynos-m4";
>   }"
INFERENCE (arithmetic on that mapping + the M3 cpuinfo dump in golang#28431): M3 = variant 0x1, part
0x002 (MIDR_EL1 = 0x5310_002x); **M4 = variant 0x1, part 0x003 => MIDR_EL1 = 0x5310_003x**. Reading
MIDR_EL1 on the S10e (EL1 read from a small bare-metal test or from a Linux kernel on the device) is
the read that would confirm it. Note LLVM's own comment: Samsung's variant/part scheme is not
predictable, so a `default:` fallthrough to M3 is what upstream does - i.e. an unknown Samsung part
number must not be assumed to be M4.
FACT. LLVM's AArch64 subtarget only has tuning data for `ExynosM3`
(`https://raw.githubusercontent.com/llvm/llvm-project/main/llvm/lib/Target/AArch64/AArch64Subtarget.cpp`
line 173); there is no `ExynosM4` tuning case, and `-mcpu=exynos-m1` (not m4) is the historically
documented Samsung tune in clang. WikiChip says Samsung patches added `-mcpu=exynos-m4` to
Samsung's own compiler, not upstream:
`https://fuse.wikichip.org/news/2051/samsung-discloses-exynos-m4-changes-upgrades-support-for-armv8-2-rearranges-the-back-end`.

#### Q1 addendum — what the combine rules mean for the HCR_EL2.DC=1 / stage-1-disabled case (our EL2 setup)

FACT (DDI 0487G.a D5.2.9 "The effects of disabling a stage of address translation", printed page D5-2709):
> "EL1 and EL0 accesses if the HCR_EL2.DC bit is set to 1 — For the EL1&0, when EL2 is enabled,
> translation regime, when the value of HCR_EL2.DC is 1, the stage 1 translation assigns the **Normal
> Non-shareable**, Inner Write-Back Read-Allocate Write-Allocate, Outer Write-Back Read-Allocate
> Write-Allocate memory attributes."
and (p. D5-2710): "When the value of HCR_EL2.DC is 1: • The SCTLR_EL1.M bit behaves as if it is 0 ...
EL1&0 stage 1 address translation is disabled. • The HCR_EL2.VM bit behaves as if it is 1 ... EL1&0
stage 2 address translation is enabled."
and (p. D5-2710): "Behavior when stage 2 address translation is disabled: • The IPA output from the
stage 1 translation maps flat to the PA. • The memory attributes and permissions from the stage 1
translation apply to the PA."

INFERENCE, combining this with Table D5-45: with HCR_EL2.DC=1 the stage-1 side is fixed at Normal WB
**Non-shareable**, so the stage-2 descriptor SH bits alone decide shareability
(NS+NS => Non-shareable, NS+ISH => Inner Shareable, NS+OSH => Outer Shareable). Consequence for a
stage-2-only EL2 mapping of DRAM on the S10e: if the stage-2 blocks are created with SH=0b00, every PE
runs with a Non-shareable WB mapping of the same memory, and per B2.7.1 the hardware is then *not
obliged* to keep those caches coherent - the safe setting for anything shared between PEs is SH=0b10
(Outer) or 0b11 (Inner) in the stage-2 descriptors, which costs nothing architecturally because
shareability only ever adds snooping, never removes it.

FACT (DDI 0487G.a B2.8 "Mismatched memory attributes", printed page B2-175): a location is accessed
with mismatched attributes if "all accesses to the location do not use a common definition of all of
the following attributes of that location: • Memory type ... • Shareability. • Cacheability ...", and
then "the only software visible effects are one or more of the following: • Uniprocessor semantics for
reads and writes to that memory Location might be lost ... • There might be a loss of coherency when
multiple agents attempt to access a memory Location ... • If all Load-Exclusive/Store-Exclusive
instructions ... do not use consistent memory attributes, the Exclusives monitor state becomes UNKNOWN
• Bytes written without the Write-Back cacheable attribute within the same Write-Back granule as bytes
written with the Write-Back cacheable attribute might have their values reverted ...".
INFERENCE: because S1+S2 attributes are *combined* (not compared), two guests/stages cannot create a
mismatch by themselves - the mismatch hazard is between different VA aliases of one PA (e.g. our EL2
direct map vs the guest's own map), which is why the effective shareability/type of every alias must be
computed with Table D5-43/D5-45 and kept consistent.

FACT (secondary, Arm's own guide; page is JS-rendered so the wording was taken from the search
snippet, verify before quoting in code comments): "Learn the architecture - AArch64 memory model",
section "Combining Stage 1 and Stage 2 attributes", `https://developer.arm.com/documentation/102376/0100/Combining-Stage-1-and-Stage-2-attributes`:
> "In the Arm architecture, the default is to use the most restrictive type. In this example, Device is
> more restrictive than Normal. Therefore, the resulting type is Device." and "For the type and
> cacheability, an additional control (HCR_EL2.FWB) allows this behavior to be overridden. When FWB is
> set, Stage 2 can override the Stage 1 type and cacheability settings, instead of the combining
> behavior. Note: HCR_EL2.FWB was introduced in Armv8.4-A."
This agrees with the primary source, and its phrase "instead of the combining behavior" is limited to
**type and cacheability** - consistent with the rule that shareability combining is unaffected by FWB.

#### Q1 cross-check in the Armv8.2-era manual (the ISA generation the M4 belongs to) — DDI 0487**C.a**

FACT. Third full ARM ARM mirror, **DDI 0487C.a** (Armv8.2 + extensions, ID121917, Dec 2017):
`http://kib.kiev.ua/x86docs/ARM/ARMARMv8/DDI0487C_a_armv8_arm.pdf` (60 MB, 6667 pages; local
`/tmp/opencode/DDI0487C_a_armv8_arm.pdf`, text `/tmp/opencode/fullC.txt`).
In this revision the material is **D4.5 "Memory region attributes"** (starts printed page D4-2174) and
**D4.5.4 "Combining the stage 1 and stage 2 attributes, Non-secure EL1&0 translation regime"**
(printed pages D4-2178 to D4-2182):
- the Device-forces-OSH rule, verbatim (printed page D4-2181): "Regardless of any shareability
  attribute obtained as described in Combining the stage 1 and stage 2 shareability attributes for
  Normal memory: • Any location for which the resultant memory type is any type of Device memory is
  always treated as Outer Shareable. • Any location for which the resultant memory type is Normal
  Inner Non-cacheable, Outer Non-cacheable is always treated as Outer Shareable."
- **Table D4-43 "Combining the stage 1 and stage 2 Shareability assignments for Normal memory"**, printed
  page D4-2182, same 7 rows as G.a Table D5-45 and K.a Table D8-97, including
  "Inner Shareable | Non-shareable | Inner Shareable" and "Non-shareable | Non-shareable | Non-shareable",
  with footnote "a. Applies only if the Normal memory is not Inner Non-cacheable, Outer Non-cacheable, see text."
- cacheability combine = Table D4-42 (printed page D4-2181), same rows as D5-44/D8-92.
FACT: the string "FWB" does not appear on any of printed pages D4-2179..D4-2182 of C.a - i.e. in the
Armv8.2 manual these combine rules are **unconditional** (FEAT_S2FWB/HCR_EL2.FWB came in Armv8.4), which
is exactly the regime an ARMv8.2 M4 implements.
So the answer to Q1(b) is the same in the v8.2 (D4.5.4 / Table D4-43), v8.7 (D5.5.7 / Table D5-45) and
v9.3 (D8.6.7.1 / Table D8-97) manuals: **stage 2 can never reduce shareability.**

### Bottom line for the coding agent (INFERENCE from the facts above)

1. FWB=0 combine rules, any ARM ARM revision: type/cacheability = most restrictive wins (Tables
   D4-41/42 in C.a, D5-43/44 in G.a, D8-91/92 in K.a); **shareability = most shareable wins**
   (C.a Table D4-43 p. D4-2182, G.a Table D5-45 p. D5-2763, K.a Table D8-97 p. D8-6694). A stage-2
   Non-shareable attribute cannot downgrade a stage-1 Inner/Outer Shareable mapping.
2. Device at either stage => Device + **Outer Shareable**, and Normal Inner-NC/Outer-NC => Outer
   Shareable, regardless of both stages' SH bits. So the "Device-nGRE forced Outer Shareable" answer
   is yes, always, and the SH bits are then dead.
3. HCR_EL2.FWB changes only the type/cacheability path (and only when stage 2 sets bit[4]=1); the
   manual says explicitly that shareability combining and stage-2-Device handling are unaffected.
   For an Armv8.2 M4, FWB is expected to be absent at all - `ID_AA64MMFR2_EL1.FWB == 0` is the read
   that settles it (INFERENCE: FEAT_S2FWB is Armv8.4, so unless Samsung backported it, the FWB=1
   path is unavailable on the S10e and must not be relied on).
4. No public M4 errata exists: no TRM, no SOG, no erratics entry, no kernel workaround, no mainline
   DT binding. The only public Samsung-custom-core CPU defect write-up is the 9810/M3 ISA-mismatch +
   bogus HWCAP_ATOMICS story (LLVM D114523 / golang#28431); M4 is Armv8.2 so it has LSE and that
   specific trap does not apply - but the general lesson (check ID registers *per cluster*, do not
   trust a union of features) does.
5. Practical hazard found while answering (1) above: with HCR_EL2.DC=1 the stage-1 default is
   "Normal **Non-shareable** WB", so an EL2 direct-map style setup is only as coherent as its
   stage-2 SH bits allow. Set stage-2 SH to Inner Shareable (0b11) for anything shared between PEs
   and to Outer Shareable for anything shared with non-CPU agents; never leave 0b00 on shared DRAM.
   Device/NC regions: use Device-nGRE or Device-nGnRE at stage 2 (MemAttr 0b0010 / 0b0001) and accept
   that they are Outer Shareable.

Open reads that would close the remaining uncertainty (not web-searchable):
- MIDR_EL1 per PE on the S10e (expect M4 = implementer 0x53, variant 0x1, part 0x003 from LLVM's map).
- ID_AA64MMFR2_EL1.FWB (0 => no FWB on 9820) and CLIDR_EL1 of each cluster (levels of coherence,
  LOUU/LOIUS) - the latter matters for whether instruction fetch coherency needs cleaning between the
  M4 cluster's 3 MB L3 and the Cortex cluster's L3.
- Whether the implementation's Inner Shareable domain spans the M4 cluster and the A55/A75 cluster
  (no public document says; an SMP Linux/Android running across all 8 cores with ISH barriers is
  strong evidence it does).

### Sources (R1 web part)

Primary, Arm ARM (full PDF mirrors used; local copies in /tmp/opencode):
- http://kib.kiev.ua/x86docs/ARM/ARMARMv8/DDI0487C_a_armv8_arm.pdf — DDI 0487C.a, Armv8.2, §D4.5.4, Tables D4-41/42/43, pp. D4-2178..D4-2182
- http://kib.kiev.ua/x86docs/ARM/ARMARMv8/DDI0487G_a_armv8_arm.pdf — DDI 0487G.a, Armv8.7, §D5.5.1/5.5.4/5.5.5/5.5.7, Tables D5-36..D5-45, pp. D5-2753..D5-2764; §D5.2.9 pp. D5-2709/2710; §B2.7.1/B2.7.2 pp. B2-166..B2-170; §B2.8 p. B2-175; §D5.9.1 p. D5-2787
- http://kib.kiev.ua/x86docs/ARM/ARMARMv8/DDI0487K_a_a-profile_architecture_reference_manual.pdf — DDI 0487K.a, Armv9.3, §D8.6/ D8.6.5 / D8.6.7.1, Tables D8-90/91/92/97, pp. D8-6686..D8-6695
- http://kib.kiev.ua/x86docs/ARM/ARMARMv8/ (directory listing of all DDI 0487 revisions, newest M_d 2026-10-11)

Arm secondary:
- https://developer.arm.com/documentation/102376/0100/Combining-Stage-1-and-Stage-2-attributes (JS-rendered; text seen via search snippet)
- https://support.arm.com/documentation/ddi0487/md/-Part-D-The-AArch64-System-Level-Architecture/-Chapter-D8-The-AArch64-Virtual-Memory-System-Architecture/-D8-6-Memory-region-attributes (index hit only, not fetched)
- https://www.arm.com/technologies/dynamiq (DSU: "Armv8.2 CPUs, up to 8 cores (4 big max), up to 4MB L3")
- https://www.kernel.org/doc/html/v6.5/admin-guide/perf/arm_dsu_pmu.html (DSU definition)
- https://github.com/codingbelief/arm-architecture-reference-manual-for-armv8-a (DDI 0487A.a markdown mirror; D4.4 TOC line "Combining the stage 1 and stage 2 attributes, Non-secure EL1&0 translation regime on page D4-1717")

Samsung M4 / Exynos:
- https://people.engr.tamu.edu/djimenez/pdfs/exynos_isca2020.pdf — "Evolution of the Samsung Exynos CPU Microarchitecture", ISCA 2020 Industry Track (also https://dl.acm.org/doi/10.1109/ISCA45697.2020.00015 , https://www.semanticscholar.org/paper/Evolution-of-the-Samsung-Exynos-CPU-Grayson-Rupley/d51a16787b46569af999a671227efd0a79574ae4 , video https://www.youtube.com/watch?v=6pmeA05g-wo )
- https://www.xda-developers.com/samsung-austin-details-unreleased-exynos-m6-cpu-microarchitecture (summary of the ISCA paper; M4 load-load cascade, memory-controller bypass)
- https://fuse.wikichip.org/news/2051/samsung-discloses-exynos-m4-changes-upgrades-support-for-armv8-2-rearranges-the-back-end (M4 = Armv8.2, FP16 + dotprod, AGU changes, "-mcpu=exynos-m4" in Samsung's compiler)
- https://en.namu.wiki/w/%EC%82%BC%EC%84%B1%EC%A0%84%EC%9E%90/%EB%A7%88%EC%9D%B4%ED%81%AC%EB%A1%9C%EC%95%84%ED%82%A4%ED%85%8D%EC%B2%98 (M4 codename Cheetah; M3 Meerkat, M5 Lion, M6 Grizzly; Armv8.2-A)
- https://en.wikipedia.org/wiki/Exynos (9820 = S5E9820, 8 nm 8LPP, 2x M4 "Cheetah" 2.73 GHz + 2x A75 + 4x A55)
- https://news.samsung.com/global/how-the-exynos-9-series-9820-is-ushering-in-a-new-era-of-mobile-experiences (Samsung: tri-cluster, 4th-gen custom CPU; 8nm LPP)

Errata-adjacent public evidence:
- https://reviews.llvm.org/D114523 and https://github.com/llvm/llvm-project/commit/cce4a7258b81159e57a411896011ee2742f17def (Exynos 9810 LSE workaround)
- https://github.com/golang/go/issues/28431 (9810 M3 SIGILL on LDADDAL; 9810 /proc/cpuinfo MIDRs; kernel later stopped reporting atomics)
- https://raw.githubusercontent.com/llvm/llvm-project/main/llvm/lib/TargetParser/Host.cpp (Samsung 0x53 variant/part → exynos-m3/m4 map)
- https://raw.githubusercontent.com/llvm/llvm-project/main/llvm/lib/Target/AArch64/AArch64Subtarget.cpp (only ExynosM3 tuning case)
- https://github.com/CruelKernel/samsung-exynos9820 (mirror of Samsung's Exynos 9820 Android kernel; used for cpu_errata.c, silicon-errata.txt, beyond0lte defconfig, exynos9820.dts)
- https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/arm/cpus.yaml (mongoose-m2/m3/m5, no m4)
- https://lists.infradead.org/pipermail/linux-arm-kernel/2024-October/972767.html and .../972766.html (M3 binding added Oct 2024)
- https://lkml.org/lkml/2024-10-16/336 and https://lists.infradead.org/pipermail/linux-arm-kernel/2024-October/970224.html (Mongoose M5 binding series)
- https://lwn.net/Articles/1022460 (KVM: arm64: Map GPU device memory as cacheable - FWB context, not read in detail)

Dead ends / searches that returned nothing useful (do not repeat):
- "Samsung M4 r0p" errata; "Exynos 9820 errata"; "Mongoose M4 software optimization guide";
  site:groups.google.com arm-community-erratics + Samsung/Exynos; "S5E9820 TRM";
  "Exynos 9820 technical reference manual" (only ARM Cortex-M4 TRMs, unrelated core);
  "mongoose-m4" dt-bindings; TF-A/OP-TEE + Exynos 9820 erratum; developer.arm.com doc pages fetched
  directly return empty (JS-rendered) - use the PDF mirrors instead.
- The mirror `https://cs140e.sergio.bz/docs/ARMv8-Reference-Manual.pdf` (DDI 0487B.a) was not needed
  once G.a/K.a/C.a were obtained.

Progress log (this section):
- [Q1] C.a + G.a + K.a combine tables extracted, quotes + page/section numbers recorded.
- [Q2] Device/NC forced-Outer-Shareability confirmed in all three revisions + B2.7.2/B2.7.1.
- [Q3] no M4 errata doc public; 9810/M3 LSE-ISA bug documented instead; MIDR map recorded; dead ends listed.
- [Q4] no vendor shareability statement; ISCA 2020 paper read (no shareability content); architecture
  notes substituted; cluster topology inferred from 9820 dts + ISCA Table I.
- Not done: reading ID_AA64MMFR2_EL1/CLIDR_EL1/MIDR on the device (needs the phone; not reachable from this host).


## Completion note (2026-10-21)
All R1-R7 asks answered except two time-boxed sub-items recorded above (fault-path instruction-decoder
class coverage; sboot cl2emul + 0x1F0000-0x250000 table decode - remain in findings §5). R1/R5 web
sections complete with sources. Distilled into m4/findings.md F19/F20 + section 2 update block.
