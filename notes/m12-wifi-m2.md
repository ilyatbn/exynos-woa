# M12 WiFi milestone 2 — BCM4375 firmware download + ARM start from the Windows probe (no NDIS, no DMA)

Base: main 7f03e5b (milestone 1 device-validated, notes/m12-wifi-m1.md). Worktree branch, no phone access
in this session (the main session runs the device test).

Goal: Exynos9820Wlan (KMDF, System class) does the brcmfmac/bcmdhd PCIe "download and start": BAR0
backplane window, EROM core walk, ARM CR4 held in reset, firmware -> TCM, nvram -> end of RAM, release
the ARM, poll the last RAM word for the pcie_shared pointer (<= 5 s), read pcie_shared flags/version.
No bus mastering, no interrupt.

## Tasks (one commit each)

1. Notes + references (this file): register map with file:line citations.
2. Blobs into C:\Users\ilya\code\s10e-artifacts\wifi-m2\ (not in git).
3. Driver: backplane + EROM + download/start + report; DriverVer bump; build.
4. ESP script extra-wlan2.cmd (copy blobs, update driver, collect).
5. Device test procedure + MILESTONES/handoff lines.

## Progress log

- 2026-10-12: worktree fast-forwarded to main 7f03e5b.
- References fetched (read-only, scratchpad, not committed): bcmdhd = LineageOS
  android_kernel_samsung_exynos9820 @ lineage-23.2 `drivers/net/wireless/broadcom/bcmdhd/` (the driver
  Samsung ships for this phone); brcmfmac = torvalds/linux master (2026-10-12)
  `drivers/net/wireless/broadcom/brcm80211/{brcmfmac,include}/`. Every citation below is file:line in
  those trees as fetched on 2026-10-12.
- Blobs re-extracted offline from `Dumps\AP_G970FXXSGHWC1_..._meta_OS12.tar` (vendor.img.lz4 -> sparse ->
  raw ext4, throw-away python readers in scratchpad) into `C:\Users\ilya\code\s10e-artifacts\wifi-m2\bcm4375\`
  (NOT in git). SHA256:
  - bcmdhd_sta.bin_b1 1297036 B 1676f46ce56b96f5...; FW-TAG `4375b1-roml/config_pcie_release Version=18.41.117
    (B1 Network/rsdb) Date=2022-07-19 FWID=01-1b4772fd`; first word 0xBBDCF186 (reset instruction);
    offset 0x6C = 0xFAFAFAFA -> NO 'RAMS' ramsize record (hnd_debug.h:37,161) -> RAM size must come from
    the CR4 bank registers (si_tcm_size / brcmf_chip_tcm_ramsize), not from the image.
  - bcmdhd_clm.blob 29232 B 76d719ae334a841d... (not downloaded in M2: CLM goes over the msgbuf ioctl
    path after the bus is up - milestone 3).
  - nvram.txt_1rh_es43_b1 12811 B 2bd914456cde0d8f...: header `BCM4375 BEYOND0 EU, MURATA 1RH ES4.3 ...
    BCM4375B1_FW276.1_BY0_EU`; boardrev=0x1101 boardtype=0x0891 macaddr=00:90:4c:12:d0:01 (placeholder MAC).
  - nvram.txt_CS01_semco_b1 12247 B 7ac8818de6ba3bfb...: header `BCM4375B1 SWB-E43 R03 SEMCO ... BY0_EU`.
  - Neither file has `devpath`/`pcie/` multi-device keys -> brcmfmac's multi_dev strip does not apply.

## Register map + flow, with citations (dhd = bcmdhd lineage-23.2, fmac = brcmfmac master)

Config space (EP, reached through the CFG0 window 0x14FFE000 the probe already maps):
| what | value | source |
|---|---|---|
| BAR0 backplane window (4 KB at BAR0+0) | cfg 0x80 | dhd include/pcicfg.h:170 `PCI_BAR0_WIN 0x80`; fmac pcie.c:122 `BRCMF_PCIE_BAR0_WINDOW 0x80`, REG_SIZE 0x1000 (:123) |
| BAR1(=PCI BAR2) backplane window | cfg 0x84 | dhd pcicfg.h:171 `PCI_BAR1_WIN`; set to 0 on download enter dhd_pcie.c:6710-6711; dhd_pcie_linux.c:1115-1120 |
| ChipCommon at BAR0+0x3000 (fixed) | | dhd pcicfg.h:314 `PCI_CC_CORE_ENUM_OFFSET 0x3000` (m1 used pcie_core.h:55-60) |
| enum base | 0x18000000 | dhd hndsoc.h:55 `SI_ENUM_BASE_DEFAULT`; dhd_pcie.c:1378-1379 sets BAR0 win to it first |
| COMMAND bus master | NOT set | (milestone rule: no DMA); fmac pcie.c `pci_set_master` in get_resource is deliberately not copied |

Backplane access = fmac `brcmf_pcie_buscore_prep_addr` (pcie.c:1844-1853): window = addr & ~0xFFF, read/write
BAR0 + (addr & 0xFFF). Window write is read back and rewritten once if it did not stick (fmac select_core
pcie.c:630-653).

TCM access = BAR2 (4 MB, `DONGLE_TCM_MAP_SIZE` dhd_pcie.h:78) with BAR1_WIN = 0, so BAR2 offset == backplane
address (dhd dhdpcie_os_chkbpoffset dhd_pcie_linux.c:1135-1146; fmac writes `tcm + rambase`, pcie.c:1711).

ChipCommon (fmac chipcommon.h): chipid 0x00 (CID id 15:0, rev 19:16, type 31:28: 1 = AI), eromptr 0xFC
(chipcommon.h:107; dhd sbchipc.h `eromptr /* 0xfc */`).

EROM walk = dhd `ai_scan` (aiutils.c:142-400) + `get_erom_ent`/`get_asd` (aiutils.c:59-136); constants dhd
include/aidmp.h:49-115: ER_TAG 0xE, ER_VALID 1, ER_CI 0, ER_MP 2, ER_ADD 4, ER_END 0xE, ER_REMAPCONTROL 0xE00
(scan limit), CIA_CID 0x000FFF00>>8, CIA_MFG 0xFFF00000>>20, CIB_REV 0xFF000000>>24, CIB_NSW 0x00F80000>>19,
CIB_NMW 0x0007C000>>14, CIB_NSP 0x3E00>>9, CIB_NMP 0x1F0>>4, AD_ADDR_MASK 0xFFFFF000, AD_SP 0xF00>>8,
AD_ST 0xC0 (SLAVE 0, BRIDGE 0x40, SWRAP 0x80, MWRAP 0xC0), AD_SZ 0x30 (SZD 0x30), AD_AG32 0x8, SD_SZ_MASK
0xFFFFF000, SD_SG32 0x8. Skip rule (aiutils.c:234-245): nsp == 0, or MFGID_ARM 0x43B && cid DEF_AI_COMP 0xFFF.
nmw+nsw == 0 components kept only for PMU 0x827 / GCI 0x840 / SR 0x841 / HUB 0x84B / OOBR 0x85C / CCB 0x50B
(aiutils.c:247-263). Wrapper = first master wrapper, else first slave wrapper (aiutils.c:345-405).
Core ids (dhd hndsoc.h): CC 0x800 (:148), SOCRAM 0x80E (:162), D11 0x812 (:166), PMU 0x827 (:187),
PCIE2 0x83C (:205), ARMCR4 0x83E (:207), GCI 0x840 (:208), ARMCA7 0x847 (:212), SYSMEM 0x849 (:213).

AI wrapper (dhd aidmp.h:309-314): ioctrl 0x408, resetctrl 0x800 (AIRC_RESET 1, :363), resetstatus 0x804.
ioctrl bits (dhd hndsoc.h:264-265, siutils.h:211): CLOCK_EN 0x1, FGC 0x2, CPUHALT 0x20.
core reset = dhd `_ai_core_reset` (aiutils.c:1157-1222): wait resetstatus==0 (300 us) -> resetctrl=1 -> 10 us ->
ioctrl = bits|resetbits|FGC|CLK -> loop(<=10){resetctrl=0} -> ioctrl = bits|CLK. `ai_core_reset` repeats it for
the 2nd/3rd wrapper first (aiutils.c:1224-1243). Halt = core_reset(CPUHALT, CPUHALT) (dhd_pcie.c:6766); run =
core_reset(0, 0) (dhd_pcie.c:6888). Same shape as fmac cr4_set_passive/active (chip.c:1322-1350).

CR4 TCM size = dhd `si_tcm_size` (siutils.c:2873-2934) == fmac `brcmf_chip_tcm_ramsize` (chip.c:680-708):
cap 0x04 (nab = bits 3:0, nbb = 7:4), BANKIDX 0x40, BANKINFO 0x44; per bank ((info & 0x7F)+1) * (info & 0x200 ?
1 KB : 8 KB) (dhd siutils.h:201-215). RAM base 0x170000: dhd dhd_pcie.c:1600-1602 (`case BCM4375_CHIP_ID:
case BCM4369_CHIP_ID: CR4_4369_RAM_BASE`), sbchipc.h:3285. fmac has no 4375 entry (chip.c:710-754; same base for
43751/43752/4377).

Download (dhd `_dhdpcie_download_firmware` dhd_pcie.c:3130-3180 + `dhdpcie_bus_download_state`
dhd_pcie.c:6687-6900):
1. BAR1_WIN = 0; CR4: core_reset(CPUHALT,CPUHALT); zero last RAM word rambase+ramsize-4 (init_shared_addr
   dhd_pcie.c:9032-9042).
2. firmware -> rambase (dhd_pcie.c:2860-2870: resetinstr = first word; offset += dongle_ram_base).
3. nvram (write_vars dhd_pcie.c:6908-6995): varaddr = rambase + ramsize - 4 - roundup(len,4); length token
   (~(len/4) << 16 | len/4) in the LAST word. Our buffer = fmac `brcmf_fw_nvram_strip` (firmware.c:395-447:
   comments/blank lines dropped, NUL-separated, +boardrev=0xff only if absent, padded to roundup(len+1,4), token
   appended) -> written so that the token lands in the last word: identical placement.
4. random seed (dhd `dhdpcie_wrt_rnd` dhd_pcie.c:11047-11070, bcmutils.h:948-965): at varaddr-8 {count=128,
   signature 0xFEEDC0DE}, 128 random bytes below it. (fmac pcie.c:1731-1747 same footer, 256 bytes, only for
   fwseed chips; dhd is what ships with this firmware -> dhd sizes used.)
5. write resetinstr to backplane 0 (BAR2+0), read back, must match (dhd_pcie.c:6865-6884).
6. CR4 core_reset(0,0) -> ARM runs.
7. poll last word until != 0 && != token (dhd dhdpcie_readshared dhd_pcie.c:8517-8545 polls 2 s; fmac
   pcie.c:117,1763-1770 50 ms x 5000 ms). Valid if rambase <= addr <= last word.
8. pciedev_shared_t (dhd bcmpcie.h:330-395): flags 0x00 (version 7:0, ASSERT_BUILT 0x100, ASSERT 0x200,
   TRAP 0x400, bcmpcie.h:50-82), trap_addr 0x04, assert_exp 0x08, assert_file 0x0C, assert_line 0x10,
   console_addr 0x14, msgtrace 0x18, fwid 0x1C, dma_rxoffset 0x24, h2d_mb 0x28, d2h_mb 0x2C, rings_info 0x30
   (fmac pcie.c:226-236 agree). Console hnd_cons_t (hnd_cons.h:54-84): log.buf +0x08, buf_size +0x0C, idx +0x10.

Deliberately NOT done (each a dhd/fmac step):
- No chip reset at attach (dhd dhdpcie_dongle_reset -> FLR dhd_pcie.c:4906 / fmac watchdog pcie.c:688-690): the
  UEFI raised WLAN_EN this boot (power-on state) and FLR resets the BARs the UEFI assigned. Consequence: a
  SECOND start in one power cycle (driver restart) runs on a dirty chip; first start per boot is authoritative.
- No pwr_req: MULTIBP is off in this tree (siutils.h:793-794 MULTIBP_CAP FALSE, `_multibp_enable` never set).
- No CLM, no rings, no interrupts, no bus mastering.

Module (nvram) selection - what Samsung's dhd does on this phone (Makefile:400-406: BCM4375_CHIP,
SUPPORT_BCM4375_MIXED_MODULES, USE_CID_CHECK on): `concate_revision_bcm4375` (dhd_pcie.c:2593-2626) uses
/data/.cid.info, else `dhd_find_naming_info_by_chip_rev` -> `dhd_parse_board_information_bcm`
(dhd_pcie.c:2405-2489, called from :2491-2540): write 0x18010324 = 0x00FA0000 (OTP control 1) and 0x18012618 = 0x0E4FFFFF (PMU
min_res_mask, sbchipc.h:63), read CIS words 0x18011120..0x18011177 (dhd_pcie.c:2270-2272), tuples
{id 0x80, len, tag, data}; tag 0x81 = vendor id (2 bytes). Table dhd_custom_cis.c:803-825: {0x43,0x22} ->
murata 1rh_es43 -> nvram `_1rh_es43_b1`; {0x51,0x33}/{0x53,0x33} -> semco e43_cs51/53 -> `_CS01_semco_b1`
(dhd_pcie.c:2335-2356). Driver does the same read; the two writes are only done when the EROM shows the GCI
core at 0x18010000 and the PMU core at 0x18012000 (so the addresses mean what dhd assumes). Unknown vid / no
file -> registry `NvramFile`, default `nvram.txt_1rh_es43_b1`.

## Review of commit 8e209e1 (1/5, notes + MILESTONES line)

- Re-checked every cited line by `sed -n <line>p` on the fetched trees. FOUND: three aiutils.c ranges were
  ~30 lines off (skip rule, nmw+nsw rule, wrapper rule, ai_core_reset) - written from memory of a long `sed`
  dump instead of grep. FIXED in the follow-up commit (234-245, 247-263, 345-405, 1224-1243). All dhd_pcie.c,
  pcicfg.h, aidmp.h, hndsoc.h, siutils.h, sbchipc.h, dhd_custom_cis.c, Makefile and fmac refs matched.
- MILESTONES: one new line, status `doing`, no other line touched (show --stat: 2 files).

## Task 3 progress (driver)

- Expected nvram processing (python port of fmac brcmf_fw_nvram_strip, scratchpad nvstrip.py; the driver
  must log the same numbers): 1rh_es43_b1 -> 8720 B padded + 4 B token = 8724 B, token 0xF77B0884;
  CS01_semco_b1 -> 8844 B + 4 = 8848 B, token 0xF75C08A3. No multi-device keys, both files carry boardrev
  (so no boardrev=0xff added), no CR, last line newline-terminated (fmac drops an unterminated last value).
- BAR sizes used by the driver come from the device-validated UEFI print of boot #88 (m1 notes): BAR0 64-bit
  0x10000 @0x14000000, BAR2 64-bit 0x400000 @0x14400000. The driver does not re-size BARs (no BAR writes);
  it checks BAR2 is 64-bit, upper dword 0, 4 MB aligned and fits the MEM window.

## RESULT (boot #92 + driver 1.0.2.0, unattended, 2026-10-12 ~01:43) - FIRMWARE RUNS. Milestone 2 DEVICE-VALIDATED
- EROM @0x18111000, 12 cores: CC 0x800 r64 @0x18000000, D11 0x812 r82 x2, ARMCR4 0x83E r15 @0x18002000 wrap
  0x18102000, PCIE2 0x83C r64 @0x18003000, 0x844 r2, GCI 0x840 @0x18010000, PMU 0x827 r35 @0x18012000, SR 0x841 x3,
  0x240 @0x19000000.
- OTP CIS @0x18011120: 80 02 00 0B 80 03 02 00 13 80 03 1B ... -> vid 0x5133 -> SEMCO -> nvram.txt_CS01_semco_b1
  (12247 B -> 8848 B, token 0xF75C08A3 = the offline python port exactly).
- CR4 cap 0x00351585, 13 banks -> RAM 0x170000..0x300000 (1600 KB). fw @0x170000, nvram @0x2FDD70, seed
  @0x2FDCE8, 0 read-back mismatches; reset instr 0xBBDCF186.
- pcie_shared at 0x0024F8BC after 180 ms; flags 0x30040007 (version 7, no trap/assert); console 0x002ACB4C;
  fwid 0x1B4772FD. Console: "RTE (SDIO-PCIE-MSGBUF) 18.41.117 (B1 Network/rsdb) on BCM4375 r5 @ 37.4/160.0/
  400.0MHz", "pciemsgbuf0: Broadcom PCIE MSGBUF driver", wl0 (5G) + wl1 (2G) "Broadcom BCM4375 802.11 Wireless
  Controller", "ThreadX v5.6 initialized".
- Cosmetic: report header still says "milestone-1 probe" (string edit missed) - fix with milestone 3.
- Next = milestone 3: msgbuf rings (bus mastering + DMA buffers in the PCIe window), D2H/H2D mailboxes, first
  firmware ioctl (cur_etheraddr / ver), then the NDIS layer.
