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
D5-2760 to D5-2764. (In the Armv8.0 manual DDI 0487A.a the same material is titled
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

FACT (cacheability, Table D5-44, p. D5-2762, applies only if FWB=0): Non-cacheable at either stage
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

Also FACT (G.a D5.5.1, p. D5-2754, note under Table D5-36):
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
4. And stage 2's own note, D5.5.4 (p. D5-2758): "The shareability field is only relevant if the memory
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

