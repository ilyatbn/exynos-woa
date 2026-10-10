# M8: UEFI boot image for S10e (beyond0lte) — research notes

Running notes. Goal: determine exactly what it takes to produce a flashable UEFI boot image for the S10e from BEYOND0LTE_UEFI.fd (the 9820 equivalent of firmware/tools/pack-uefi.ps1).

## Progress log

### Step 1 - read firmware/tools/pack-uefi.ps1 (whole file, 126 lines)
FACT C:\Users\ilya\code\exynos-woa\firmware\tools\pack-uefi.ps1:1-126 - the script is hard-wired to star2lte:
  :44-45  FD path = <ws>\edk2\Build\Star2LtePkg\<Target>_<Toolchain>\FV\STAR2LTE_UEFI.fd  (package name literal)
  :55-57  freshness-guard source dirs = Platform\Star2LtePkg, Silicon\Exynos9810Pkg, tools\bootshim
  :68     default Ref = <ws>\device-data\reference\twrp-3.7.0_9-0-star2lte.img
  :30     UefiBase default "0x90000000" (param, overridable)
  :79-81  UEFI_SIZE = ceil(fdLen/16)*16  -> derived from the FD's actual length, NOT from the FDF
  :92     clang -target aarch64-none-elf -ffreestanding -nostdlib -mgeneral-regs-only -DUEFI_BASE=.. -DUEFI_SIZE=.. -c bootshim.S
  :93     ld.lld -Ttext=0x80080000 -e _head --no-relax   (shim LINK base, literal)
  :94     llvm-objcopy -O binary -> bootshim.bin
  :102-107 sanity check: code0 must be `adr x1,#imm`, magic must be "ARM\x64"
  :111-116 kernel = cat(shim.bin, FD)
  :121    python tools\repack-with-ref.py --ref --kernel --output
  :120    default output = Platform\Star2LtePkg\star2lte-uefi-boot.img
FACT: firmware/tools/repack-with-ref.py DOES NOT EXIST in this repo.
  git ls-files firmware/tools/* -> only bootshim/bootshim.S, build-uefi.ps1, pack-uefi.ps1.
  git log --all -- firmware/tools/repack-with-ref.py -> empty (never tracked).
  So pack-uefi.ps1 line 121 invokes a missing tool; the star2lte pipeline is itself incomplete here.

### Step 2 - the reference boot image is ALSO missing (both devices)
FACT: firmware/device-data does not exist (Test-Path False). pack-uefi.ps1:68 default Ref
  = firmware/device-data/reference/twrp-3.7.0_9-0-star2lte.img -> MISSING.
FACT: no *.img / *.fd / *.efi files anywhere in the repo outside firmware/edk2 (recursive scan).
  So there is NO reference boot image of any kind checked in, for star2lte or for beyond0lte.
FACT: tools/bootimg-pack.sh (cited by both FDF headers, Star2LtePkg.fdf:3 and Beyond0LtePkg.fdf:3)
  does not exist either - Test-Path False for tools/ and firmware/tools/.
INFERENCE: the repo carries the *description* of the packaging step but not the tool nor the inputs.

### Step 3 - bootshim.S constants and what is device-specific
FACT C:\Users\ilya\code\exynos-woa\firmware\tools\bootshim\bootshim.S:
  :37-42  UEFI_BASE default 0x90000000 ("must equal PcdFdBaseAddress (DSC/FDF)"),
          UEFI_SIZE default 0x700000 ("must equal FD_SIZE (7 MB)")
  :47-48  header code0/code1 = `adr x1,_payload` + `b _start`
  :50     text_offset  .quad 0x80000
  :51     image_size   .quad 0x800000  (8 MB - "shim + 7 MB FD + margin")
  :52     flags        .quad 0xa  (4K pages, load anywhere, LE)
  :56     magic "ARM\x64"
  :72-75  watchdog kill: WTCON=0 at 0x10050000 (cl0) and 0x10060000 (cl1)  [literal]
  :82-86  SCTLR C/I clear
  :94-104 DIAG ramoops 'DBGC' at 0xFED14000  [literal - 9810 address]
  :110-120 DIAG RED band: FB 0xCC000000, 1440x2960 BGRA, rows 200-650, 648000 px  [literal - 9810]
  :125-133 16-byte ldp/stp copy of UEFI_SIZE bytes from x1 to UEFI_BASE
  :142-152 DIAG GREEN band rows 750-1200 at 0xCC41EB00  [literal - 9810 geometry]
  :171-173 temp SP = 0xA0000000 ("plain DRAM above the 7 MB FD, below EDK II's own stack ~0xB4800000")
  :198    br x5 -> enter FD
  :206-207 _payload MUST be last emitted so `cat` lands the FD under the adr
FACT: every one of those literals is a star2lte observation. The shim has NO 9820 variant in the repo
  (only one bootshim.S; grep for beyond0lte in firmware/tools -> none).

### Step 4 - the 9820 FDF/DSC pair (what produces BEYOND0LTE_UEFI.fd)
FACT C:\Users\ilya\code\exynos-woa\firmware\Platform\Beyond0LtePkg\Beyond0LtePkg.fdf:
  :68   DEFINE FD_BASE_ADDRESS = 0x0000000080080000   (header :58-67 marks it UNVERIFIED as an observed address)
  :69   DEFINE FD_SIZE         = 0x0000000000700000   "carried from the 9810 (build parameter, not a read)"
  :71   [FD.BEYOND0LTE_UEFI]  BaseAddress=$(FD_BASE_ADDRESS) Size=$(FD_SIZE)
  :74-76 ErasePolarity 1, BlockSize 0x1000, NumBlocks 0x700  -> 0x700*0x1000 = 0x700000
  :79   single region line 0x00000000|0x00700000  FV = FVMAIN_COMPACT  (one bank, whole FD)
  :89   [FV.FVMAIN_COMPACT] NumBlocks = 0 (auto-size)
  :131  [FV.FVMAIN] NumBlocks = 0x400 (fixed 4 MB)
FACT C:\Users\ilya\code\exynos-woa\firmware\Platform\Beyond0LtePkg\Beyond0LtePkg.dsc:
  :415  gArmTokenSpaceGuid.PcdFdBaseAddress|0x0000000080080000
  :417  gArmTokenSpaceGuid.PcdFvBaseAddress|0x0000000080080000
  (Star2LtePkg.dsc:293/295 = 0x90000000; Star2LtePkg.fdf:16 = 0x90000000)
FACT: the FD is ONE bank (0x00000000|0x00700000) in both FDFs - there is no multi-bank split to mirror.
FACT: no built FD exists on disk (recursive scan found no *.fd outside firmware/edk2 sources).

### Step 5 - the 9820 FD DOES exist as a build artifact (corrects the earlier scan)
FACT: firmware/edk2/Build/Beyond0LtePkg/DEBUG_CLANGPDB/FV/BEYOND0LTE_UEFI.fd
  length 7 340 032 = 0x700000 exactly (the FDF's fixed FD_SIZE).
  notes/m8-build.md:53-54 and :109-111 and :169-171 record three builds of it
  (sha256 drifts between rebuilds - notes/m8-build.md:184-199 says the FD is NOT
  byte-deterministic; used/free counts drift by 24 B).
FACT: build-uefi.ps1:36 already has [ValidateSet("Star2LtePkg","Beyond0LtePkg")] $Platform,
  so the 9820 FD is buildable today: `build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG`
  ran with exit 0 (notes/m8-build.md:43-51).
FACT: pack-uefi.ps1 has NO such -Platform switch (:24-32 param block) - it is star2lte-only.

### Step 6 - sboot/boot-image constraints the star2lte pipeline depends on
FACT: pack-uefi.ps1:121 wraps [shim][FD] as the "kernel" of a *known-good TWRP boot image*
  ("which sboot accepts on this device", :14). So the accepted-ness of the container is
  device-proven, not computed: the reference image is the thing sboot will read.
FACT: bootshim.S:47-57 is the Linux ARM64 Image header sboot parses (text_offset, image_size,
  flags, res2-4, "ARM\x64" magic, res5). bootshim.S:10-13 says sboot drops the kernel near
  0x80100000 = DRAM base + text_offset (0x80000).
FACT: pack-uefi.ps1:126 tells the operator to "Flash to BOOT (in TWRP) ... (or dd to sda10)"
  - sda10 is the 9810's BOOT partition. On the S10e BOOT is sda14 (notes/s10e-root-reads.md:174,
  MILESTONES.md:230, HANDOFF.md:310).

### Step 7 - the installer/release side already knows about a UEFI image per Windows build
FACT: docs/map/installer.md:43 FirmwareCatalog.cs (165L) parses firmware.json = Windows build -> UEFI image + hash.
FACT: docs/map/release.md:16 make-payload.ps1 assembles firmware.json + UEFI images, each checked vs catalog hash.
FACT: docs/map/firmware.md:83-84 "Not scripted byte-for-byte from this tree (research lineage).
  Released star2lte-uefi-<build>.img images are validated builds, one per Windows build, listed in firmware.json."
  -> the released 9810 images were NOT produced reproducibly from this tree; only the pipeline description is.

### Step 8 - the size constraint the installer already encodes
FACT C:\Users\ilya\code\exynos-woa\installer\src\S9Woa.Installer.Core\Toolset\Validation.cs:
  :9   RecoveryPartitionBytes = 16_512 * 4096 = 67 633 920  (beyond0lte RECOVERY; the SMALLER of the two PITs)
  :12  BootPartitionBytes     = 14_080 * 4096 = 57 671 680  ("BOOT is 14,080 x 4096 on BOTH phones")
  :58  ValidateUefi(file) = Validate(file, BootPartitionBytes) -> a UEFI image must be <= 57 671 680 B
       and must start with "ANDROID!" (:14/:35)
CROSS-CHECK: notes/s10e-root-reads.md:58 BOOT = LBA 27 264..41 343 = 14 080 LBAs. Consistent.
FACT: ToolsetManager.cs:194 pairs (UefiPayload, BootImage.BootPartitionBytes); :487 CopyPayload(file, UefiPayload, BootImage.ValidateUefi(file), id)
  -> the installer consumes a UEFI boot image named/validated as an Android boot image for BOOT.

### Step 9 - searching for a reference boot image OUTSIDE the repo
FACT: recursive scan of C:\Users\ilya\code for *.img (excluding the repo) -> ZERO hits.
  So no beyond0lte TWRP boot image, and no star2lte one, exists on this machine under code\.
FACT: C:\Users\ilya\code\Dumps\ holds the SM-G970F stock ODIN payload set and related artefacts:
  AP_G970FXXSGHWC1_..._meta_OS12.tar 6 339 594 430 B   (the "AP" package - contains boot.img/recovery.img inside)
  BL_G970FXXSGHWC1_...tar               4 935 857 B   (sboot/BL itself)
  CP_G970FXXSGHWB3_...tar              27 996 349 B
  CSC_OXM_...tar                      508 283 064 B
  HOME_CSC_OXM_...tar                 508 242 109 B
  BEYOND0LTE_EUR_OPEN.pit                   6 224 B   (the PIT notes/pit-geometry.md parses)
  pit_data_extracted.txt                   28 040 B
  twrp_device_tree\ (a beyond0lte TWRP *device tree*, not an image):
    kernel 44 363 788 B, dtbo 1 236 904 B, recovery.fstab, BoardConfig.mk,
    recovery/root/{fstab.exynos9820, init.recovery.*.rc, ueventd.samsungexynos9820.rc},
    recovery/root/sbin/* (busybox applets) - i.e. an UNPACKED TWRP ramdisk tree.

### Step 10 - decoding the ARM64 Image header of C:\Users\ilya\code\Dumps\twrp_device_tree\kernel (44 363 788 B)
FACT (first 64 bytes, hex):
  00 00 56 14 | 00 00 00 00 | 00 00 08 00 00 00 00 00 | 00 80 13 03 00 00 00 00 |
  0a 00 00 00 00 00 00 00 | 00*8 | ef ec ef ec 00 00 00 00 | 00*8 | 41 52 4d 64 00 00 00 00
  Decoded against bootshim.S:49-57 (code0/code1 u32, then u64 fields):
    code0 = 0x14560000 (a `b`), code1 = 0
    text_offset = 0x80000            (SAME as bootshim.S:50)
    image_size  = 0x03138000 = 51 642 624
    flags       = 0x0a               (SAME as bootshim.S:52)
    res2 = 0, res3 = 0xECEFECEF (non-zero - the 9810 shim writes 0), res4 = 0
    magic = "ARM\x64" at offset 0x38 = 56  (matches pack-uefi.ps1:104 reading magic at 56)
  -> this is the vendor/TWRP ARM64 kernel Image header, i.e. the real sboot-accepted header on
     SM-G970F. It CONFIRMS text_offset 0x80000 and flags 0xa for the 9820 (INFERENCE: it is a
     kernel Image, not a boot.img container - no "ANDROID!" magic at 0).

### Step 11 - the phone is NOT attached in this session (so the read must be specified, not run)
FACT: & "C:\Users\ilya\code\platform-tools\adb.exe" devices -> "List of devices attached" with NO entries
  (run twice, 2026-10-09 this session). AGENTS.md rule 8 claims RF8M21HYDSZ is attached and authorized;
  it is not right now. So every device read below is written as the exact command to run, and is
  recorded as NOT YET EXECUTED. Nothing was guessed in its place.

### Step 12 - overlap analysis: the shim's hardcoded RAM addresses vs the 9820's verified map
Method: arithmetic on the VERIFIED reads in Exynos9820.h and notes/s10e-root-reads.md §4.
Marked INFERENCE (mine) unless the source line says otherwise.

FD (Beyond0LtePkg.fdf:68-69) = 0x80080000 .. 0x80780000 (0x700000).
DT reservations (notes/s10e-root-reads.md:109-122, VERIFIED as root in TWRP):
  sec_debug_magic 0x80000000..0x80001000   kaslr 0x80001000..0x80002000   -> both BELOW the FD, no overlap
  el2_code        0x87000000..0x87002000   -> ABOVE the FD, no overlap
  mm_debug        0x91200000..0x91201000
  seclog_mem      0xC0000000..0xC0080000
  secure_camera   0x96000000..0x98200000
  camera_contig   0x98200000..0x99000000
  camera          0x99000000..0xA7400000   <-- CONTAINS 0xA0000000
  cp_ram_logging  0xFB900000..0xFD900000
  debug_snapshot  0xFD900000..0x100000000 (contiguous, notes/vendor-dt-reserved.md:56-65)
  modem_spare     0xD0000000..0xD1600000   modem_if2 0xD1600000..0xD5800000
  cp2cp_shm       0xD5800000..0xD9800000   s5100_msi 0xD9800000..0xD9801000
  modem_if        0xF0000000..0xFB900000
CONCLUSIONS (INFERENCE):
  * bootshim.S:172 temp SP 0xA0000000 lands INSIDE the 9820's `camera` reservation
    (0x99000000 + 0xE400000). On the 9810 it was "plain DRAM above the 7 MB FD".
    That justification does not hold here -> SP base is a NEW constant to close.
  * bootshim.S:111/143 DIAG framebuffer stores at 0xCC000000 / 0xCC119400 / 0xCC41EB00
    with 1440x2960 geometry: the 9820's fb is 0xCA000000, 1440x3040 (Exynos9820.h:450-452).
    0xCC000000 is NOT the 9820's scanout -> the DIAG paint writes into ordinary DRAM.
  * bootshim.S:94-104/:154-161/:189-196 ramoops pmsg at 0xFED14000 is UNCONDITIONAL in the
    shim (not compiled out) and 0xFED14000 sits inside debug_snapshot log_bcm/log_llc
    (0xFEB10000..0xFF400000 fully occupied) -> known-bad write on this device.
  * bootshim.S:72-75 watchdog WTCON at 0x10050000 and 0x10060000: Exynos9820.h:426-431
    VERIFIES watchdog_cl0@10050000 and watchdog_cl2@10060000 - SAME addresses, different
    unit name (cl2 not cl1). The shim's two WTCON stores are therefore address-correct here.
  * bootshim.S:26 cites the 9810 RKP-monitored kernel zones 0x80094000-0x828DBFFF. No 9820
    equivalent read exists anywhere in the repo (grep) -> the "clean RAM above RKP zones"
    justification for 0x90000000 has no 9820 counterpart; that is exactly why the FDF moved
    to 0x80080000 (Beyond0LtePkg.fdf:63-65).

### Step 13 - CONFLICT in the repo about the initrd, which is the FD-base rationale
FACT notes/dock-desktop.md:784-787 - the vendor DT `chosen` node: initrd-start = 0x84000000,
  initrd-end = 0x841fffff (32 MB at the front of bank0) -> "the same reason the 9810 flow loads
  the firmware at 0x80080000 rather than at the base". Same claim Exynos9820.h:75-76.
FACT notes/s10e-root-reads.md:103 - the LIVE phone's /proc/device-tree/chosen/bootargs region
  reports "initrd at 0x89000000..0x89a67000".
INFERENCE (mine): these two disagree (0x84000000 vs 0x89000000). The live read is the stronger
  source for what sboot actually does today, and it is CONSISTENT with an FD at 0x80080000..0x80780000
  (no overlap either way). But the DT value is what the FD-base decision was written down from, so
  the rationale should be re-read from the live `chosen` node's initrd-start/initrd-end properties
  before the FD base is trusted. That is a read, not a guess.

### Step 14 - the star2lte reference image's own size (a data point for the S10e equivalent)
FACT installer\src\S9Woa.Installer.Core\Twrp\WinReTwrpBuilder.cs:63-67
  OfficialTwrpSha256 = f674dab0134f3c929982077b6a3a8de7df209a45c76ead80bc009381bcd835e1 (official twrp-3.7.0_9-0-star2lte.img)
  OfficialTwrpBytes  = 42 670 080  ("its last section ends here, no partition padding")
  -> 42 670 080 < BootPartitionBytes 57 671 680, so the star2lte ref DOES fit BOOT. Consistent
     with pack-uefi.ps1:126 flashing the wrapped image to BOOT.
FACT: that SHA is a star2lte gate - WinReTwrpBuilder.cs:63,195,196 and PowerOffRoutePatch.cs:55
  refuse any other image (notes/m9-install-attempt.md:43-45).

---

## ANSWER 1. What the star2lte pipeline does, step by step, and which inputs are device-specific

### 1a. The three steps (FACT, from pack-uefi.ps1 + bootshim.S)

| # | Step | Where | What it actually does |
|---|---|---|---|
| 0 | locate the FD | pack-uefi.ps1:44-48 | `<ws>/edk2/Build/Star2LtePkg/<Target>_<Toolchain>/FV/STAR2LTE_UEFI.fd`; refuses if absent |
| 0b | freshness guard | pack-uefi.ps1:52-67 | refuses a stale FD (GNU make 3.81 silently failed twice); source dirs :55-57 |
| 1 | derive UEFI_SIZE | pack-uefi.ps1:79-81 | `ceil(fdLen/16)*16` — from the FD's actual byte length, so it always equals FD_SIZE |
| 2 | assemble the shim | pack-uefi.ps1:89-91 | clang aarch64-none-elf, `-DUEFI_BASE`, `-DUEFI_SIZE`, `-c bootshim.S` |
| 3 | LINK the shim | pack-uefi.ps1:92-93 | `ld.lld -Ttext=0x80080000 -e _head --no-relax` — the comment :86-88 says a `-c`-only object leaves adr/branch offsets unapplied -> wrong entry |
| 4 | flatten | pack-uefi.ps1:94-97 | `llvm-objcopy -O binary` -> bootshim.bin |
| 5 | sanity-check the shim | pack-uefi.ps1:102-107 | code0 must match `adr x1,#imm` (mask 0x9F00001F == 0x10000001), the adr immediate must be non-zero, magic at file offset 56 must be "ARM" |
| 6 | concatenate | pack-uefi.ps1:111-116 | `kernel.bin = shim.bin ++ FD` (raw cat, no padding) |
| 7 | wrap in a reference boot image | pack-uefi.ps1:121 | `python tools/repack-with-ref.py --ref --kernel --output` |
| 8 | output | pack-uefi.ps1:120 | default `Platform/Star2LtePkg/star2lte-uefi-boot.img` |
| 9 | flash | pack-uefi.ps1:126 | "Flash to BOOT (in TWRP) ... (or dd to sda10)" |

### 1b. What the shim itself does at runtime (FACT, bootshim.S)
`adr x1,_payload` (:47) captures the FD's RUNTIME address -> mask IRQs (:65) ->
WTCON=0 at 0x10050000 and 0x10060000 (:72-75) -> clear SCTLR C/I (:82-86) ->
ramoops breadcrumb 'A' at 0xFED14000 (:94-104) -> RED paint at 0xCC119400, 648000 px (:110-120) ->
16-byte ldp/stp copy of UEFI_SIZE from x1 to UEFI_BASE (:125-133) -> GREEN paint (:142-152) ->
breadcrumb 'B' (:154-161) -> SP = 0xA0000000 (:171-173) -> breadcrumb 'M' (:189-196) -> `br x5` (:198).
`_payload` MUST be last (:206-207) so the `cat` lands the FD exactly under the `adr`.

### 1c. Which inputs are DEVICE-SPECIFIC (the list that must be re-derived for 9820)

| Input | Where it enters | Why it is device-specific |
|---|---|---|
| reference boot image | pack-uefi.ps1:68 (Ref), :121 (--ref) | "known-good ... which sboot accepts on this device" (:14). The container's page size, section order, ramdisk/second/dt/tail are whatever that phone's sboot will read. NOT computed - a captured artefact |
| FD base (UEFI_BASE / PcdFdBaseAddress) | pack-uefi.ps1:30 default, :90 -D, bootshim.S:38, DSC :293, FDF :16 | must equal the FD's link address; the choice 0x90000000 is justified by a 9810 RAM observation (:23-25, FDF :12-15) |
| FD size (UEFI_SIZE / FD_SIZE) | pack-uefi.ps1:79-81, bootshim.S:41, FDF :17/:24-27 | derived from the FD, but the FD's size is a build parameter carried from the 9810 |
| shim link base | pack-uefi.ps1:93 `-Ttext=0x80080000` | where sboot drops the kernel; bootshim.S:12-13 says sboot loads near DRAM base + text_offset |
| ARM64 Image header values | bootshim.S:50-56 (text_offset 0x80000, image_size 0x800000, flags 0xa, magic) | "star2lte-proven values" (:49 comment); sboot checks this header |
| watchdog addresses | bootshim.S:72-75 | sboot arms cl0/cl1 WTCON before handoff |
| MMU/cache state at handoff | bootshim.S:19-22, :78-86 | "MMU is OFF at handoff (device-verified)" — a 9810 observation |
| RKP-monitored kernel zones | bootshim.S:23-25, :163-170 | 0x80094000-0x828DBFFF — a 9810 observation; decides both the copy target and the SP |
| ramoops/pstore address | bootshim.S:94-104 (0xFED14000) | must be a region TWRP exposes AND that no vendor owner uses |
| framebuffer base + geometry | bootshim.S:107-120, :139-152 (0xCC000000, 1440x2960) | panel scanout address and dimensions |
| temp SP | bootshim.S:171-173 (0xA0000000) | must be free DRAM between the FD and EDK II's own stack |
| target partition | pack-uefi.ps1:126 (sda10) | BOOT's device node |
| page/segment layout | repack-with-ref.py (MISSING) | header v0 + page-aligned kernel/ramdisk/second/dt + tail; page size is read from the ref (bootimg.py:34-35, :51) |
| sboot acceptance | the ref image itself | the whole reason a ref is needed: sboot rejects some containers |

### 1d. MISSING PIECES of the star2lte pipeline as checked in (FACT)
- `firmware/tools/repack-with-ref.py` — referenced at pack-uefi.ps1:121, never existed in this repo
  (`git ls-files firmware/tools/*` -> only bootshim.S, build-uefi.ps1, pack-uefi.ps1;
  `git log --all -- firmware/tools/repack-with-ref.py` -> empty).
- `firmware/device-data/reference/twrp-3.7.0_9-0-star2lte.img` — the default Ref, absent
  (`firmware/device-data` does not exist).
- `tools/bootimg-pack.sh` — cited by both FDF headers (:3) and by bootshim.S's own comment
  (Star2LtePkg.fdf:61, Beyond0LtePkg.fdf:113), absent everywhere.
- No released `star2lte-uefi-*.img` on disk (recursive *.img scan of C:\Users\ilya\code -> 0 hits).
INFERENCE: so the star2lte flow is documented but not reproducible from this checkout either;
  the only checked-in building block for the missing wrapper is `tools/twrp-winre/bootimg.py`
  (BootImage.parse/serialize) and its C# port `AndroidBootImage.cs`.

---

## ANSWER 2. The 9820 equivalents, and which are already VERIFIED here

Legend: V = a read recorded in this repo; U = still UNVERIFIED; C = carried from the 9810 as a
build parameter (neither a read nor a device-specific decision).

| star2lte input | 9820 equivalent | value | status | where the value lives |
|---|---|---|---|---|
| FD base 0x90000000 | FD base | `0x80080000` | **U** as an observed address (the initrd bounds are the read; the base is the decision that follows) | `Beyond0LtePkg.fdf:68`, `Beyond0LtePkg.dsc:415,417`, rationale `Beyond0LtePkg.fdf:58-67`, `Exynos9820.h:75-76`, `notes/m8-dscfdf.md:117` |
| FD size 0x700000 | FD size | `0x700000` (7 MB) | **C** — "carried from the 9810 (build parameter, not a read)" | `Beyond0LtePkg.fdf:69,:76,:79`; the built FD really is 7 340 032 B (verified on disk) |
| shim `-Ttext=0x80080000` | shim link base | same literal in pack-uefi.ps1:93 | **not device-specific in substance** — bootshim.S:29-30 says the shim is fully position-independent, so `-Ttext` only resolves local adr/branch offsets | `pack-uefi.ps1:93` |
| text_offset 0x80000 | ARM64 Image header text_offset | 0x80000 | **V (by artefact, provenance unverified in-repo)** — the SM-G970F kernel Image in `C:\Users\ilya\code\Dumps\twrp_device_tree\kernel` carries text_offset 0x80000 at header +0x08 | read from that file's first 64 B (this session) |
| flags 0xa | ARM64 Image flags | 0xa | **V (same artefact)** — 0x0a at header +0x18 | same |
| image_size 0x800000 | ARM64 Image image_size | must cover shim+FD+margin; the vendor kernel declares 0x03138000 for itself | **U** — nothing says what sboot requires for OUR image_size | `bootshim.S:51` |
| WTCON cl0 0x10050000 | watchdog_cl0 | 0x10050000 | **V** | `Exynos9820.h:431` (`watchdog_cl0@10050000`), `Exynos9820.h:426` comment |
| WTCON cl1 0x10060000 | the 9820's second watchdog is **cl2** at 0x10060000 | 0x10060000 | **V (address)**; the *name* differs (cl2, not cl1) | `Exynos9820.h:426` comment "watchdog_cl2@10060000" |
| MMU OFF at handoff | 9820 MMU state at handoff | — | **U** — "device-verified" in bootshim.S:21-22 is a 9810 observation; no 9820 read exists | `bootshim.S:19-22`, `Beyond0LtePkg.fdf:175-187` (the 9810 trail stall) |
| RKP kernel zones 0x80094000-0x828DBFFF | 9820 monitored zones | **none known** | **U — no 9820 equivalent read anywhere in the repo** (grep of `0x80094000`/`RKP` finds only 9810 text and the 9820 files that say so) | `Beyond0LtePkg.fdf:63-65`, `Beyond0LtePkg.dsc:339-345,:410-412` |
| ramoops 0xFED14000 | 9820 breadcrumb address | 0xFED14000 is **inside** `debug_snapshot.log_bcm`/`log_llc` (0xFEB10000..0xFF400000 fully occupied) | **V that the 9810 value is WRONG**; the chosen replacement gap `0xD9801000..0xF0000000` is a **decision, deliberately not enabled** | `notes/s10e-root-reads.md:124-138`, `notes/vendor-dt-reserved.md:55-70`, `Exynos9820.h:110-115`, `ArmPlatformLib.c:23-35` |
| FB 0xCC000000, 1440x2960 | FB base + geometry | `0xCA000000`, **1440 x 3040**, 4 B/px, region 0xCA000000..0xCA10B29FF | **V** | `Exynos9820.h:450-456`, `notes/s10e-root-reads.md:88,:91-96` (`s3cfb.bootloaderfb=0xca000000`) |
| temp SP 0xA0000000 | 9820 temp stack | **0xA0000000 is inside the 9820's `camera` reservation** (0x99000000 + 0xE400000 = 0x99000000..0xA7400000) | **V that the 9810 choice is wrong**; the replacement is **U** | `notes/s10e-root-reads.md:116-117`, `bootshim.S:163-173` |
| EDK II's own stack ~0xB4800000 | 9820 PEI window | the 9810 window 0x90000000..0xBC800000 is explicitly NOT carried over | **U** | `Beyond0LtePkg.dsc:339-345` |
| flash target sda10 | BOOT partition | **sda14**, LBA 27 264..41 343, 14 080 x 4096 = 57 671 680 B; use `/dev/block/by-name/boot` | **V** | `notes/s10e-root-reads.md:58,:174`, `notes/s10e-partitions.md:38`, `Validation.cs:12`, `HANDOFF.md:310` |
| RECOVERY (where TWRP lives) | RECOVERY partition | **sda15**, LBA 41 344..57 855, 16 512 x 4096 = 67 633 920 B | **V** | same |
| ref TWRP image | beyond0lte TWRP `.img` | **absent** — no `.img` anywhere on the machine | **U / missing artefact** | `notes/what-to-lookup.md:33-39`, `notes/installer-s10e-gaps.md:56` |
| page size / section order of the container | 9820 boot image page size | unread; would come from the ref image's own header word at +0x24 | **U** | `bootimg.py:51`, `AndroidBootImage.cs:40` |
| (new, 9820-only) initrd | live initrd | `0x89000000..0x89a67000` (live cmdline) vs `0x84000000..0x841fffff` (vendor DT) | **V (live) but CONFLICTING** | `notes/s10e-root-reads.md:103` vs `notes/dock-desktop.md:784-787` |

### 2b. Things the 9820 side already has that the star2lte pipeline did not need
FACT: `tools/twrp-winre/bootimg.py:39-122` (and the byte-compatible C# port
`installer/.../Twrp/AndroidBootImage.cs:15-146`) already implement exactly the container
handling repack-with-ref.py would need: parse header v0 with Samsung's 0x28 = dt_size
(`bootimg.py:80-83`, `AndroidBootImage.cs:55-58`), page-aligned kernel/ramdisk/second/dt
(`:73-84` / `:64-71`), `tail` preserved (`:92` / `:69`), and `serialize()` rewrites the four
size words at 0x08/0x10/0x18/0x28 and the SHA-1 id at 0x240 (`:105-122` / `:113-137`).
INFERENCE: a 9820 packer is therefore `bootimg.py.Parse(ref)` -> set `.Kernel = cat(shim, FD)`
-> `.Serialize()`; nothing else in the repo does this today.

### 2c. Defect found while reading the shim (harsh-review style)
FACT: bootshim.S:12-13 says sboot drops the kernel "near 0x80100000 (DRAM base + text_offset)".
With DRAM base 0x80000000 (Exynos9820.h:79) and text_offset 0x80000 (bootshim.S:50) the arithmetic
gives 0x80080000, not 0x80100000. The comment's own numbers disagree by one 0x80000. Nothing in the
repo resolves which is right for the 9820, so the shim's drop address is a read-to-close, not a fact.

---

## ANSWER 3. Does a reference boot image for the S10e exist, and the exact read that produces it

### 3a. Search result (FACT)
- In the repo: **no**. `firmware/device-data` does not exist; `git ls-files` has no `.img`;
  a recursive scan of the whole repo for `*.img`/`*.fd`/`*.efi` outside `firmware/edk2` returned
  nothing (the only FD is the build artifact `BEYOND0LTE_UEFI.fd`).
- Outside the repo, on this machine: a recursive scan of `C:\Users\ilya\code` for `*.img`
  returned **zero** files.
- What DOES exist for SM-G970F, outside the repo, in `C:\Users\ilya\code\Dumps\`:
  * `AP_G970FXXSGHWC1_CL25257816_QB62768393_REV01_user_low_ship_meta_OS12.tar` (6 339 594 430 B)
    — Samsung's ODIN "AP" package for this exact phone. Its payload includes the stock
    `boot.img` and `recovery.img` for SM-G970F. INFERENCE: untarring it yields a *stock Android*
    boot image, which is a container sboot accepts but is NOT a TWRP image.
  * `twrp_device_tree\` — a beyond0lte TWRP **device tree** (BoardConfig.mk, recovery.fstab,
    an unpacked `recovery/root` ramdisk tree, a 44 363 788 B ARM64 kernel Image, a 1 236 904 B dtbo).
    `notes/what-to-lookup.md:36-39` already says this "proves a beyond0lte recovery exists —
    but the built `.img` is what's needed".
- `notes/what-to-lookup.md:33-39` and `notes/installer-s10e-gaps.md:56` both record the
  beyond0lte TWRP `.img` as a **missing artefact** to download
  (official name must contain `beyond0lte`, `Validation.cs:45-54`).

### 3b. So: must it be read from the phone? YES — and the phone is the better source
FACT: the phone is currently booted in TWRP as root (`notes/s10e-root-reads.md:3` —
`RF8M21HYDSZ   recovery`, `uid=0(root)`), and that pass already proved block-device reads work
from TWRP as root (`:12-16`: `mknod /tmp/sda b 8 0`, `dd if=/tmp/sda ...`).
INFERENCE (strong): the image in the RECOVERY partition right now IS a boot image this device's
sboot accepted in order to put us into TWRP. It is therefore the *device-proven* reference
container — strictly better than a downloaded file, because it is the one this phone actually reads.

### 3c. The exact reads (NOT executed — the phone is not attached this session, see Step 11)

Read 1 — the container header, 4 KiB, to learn page size / section sizes / dt presence:
```
adb shell 'dd if=/dev/block/by-name/recovery of=/tmp/ref-head.img bs=4096 count=1'
adb pull /tmp/ref-head.img
```
Then parse locally with `tools/twrp-winre/bootimg.py:58-93` (`BootImage.parse`) on the first page:
kernel_size @0x08, ramdisk_size @0x10, second_size @0x18, page_size @0x24, dt_size @0x28
(`bootimg.py:51,:64-71`). That gives the total image length
= page + pad(kernel)+pad(ramdisk)+pad(second)+pad(dt) + tail.

Read 2 — the whole image, sized by Read 1 (bounded, never the raw partition):
```
adb shell 'dd if=/dev/block/by-name/recovery of=/tmp/ref-recovery.img bs=1048576 count=<ceil(len/1MiB)>'
adb pull /tmp/ref-recovery.img
```
Constraint check: RECOVERY = 67 633 920 B, BOOT = 57 671 680 B (`Validation.cs:9,:12`).
INFERENCE: if the recovery image is larger than 57 671 680 B it cannot be reused verbatim as a
BOOT payload — the star2lte ref is 42 670 080 B (`WinReTwrpBuilder.cs:66-67`), i.e. comfortably
under BOOT, so the S10e ref must be measured against the same bound (`Validation.cs:58`
`ValidateUefi` = `BootPartitionBytes`).

Read 3 — the stock BOOT partition image, as the alternative container (also sboot-accepted):
```
adb shell 'dd if=/dev/block/by-name/boot of=/tmp/ref-boot.img bs=4096 count=14080'
```
INFERENCE: this is the phone's *current* boot slot; on a TWRP-booted S10e it may already be a
modified image, so Read 2 is the preferred reference and Read 3 is the cross-check.

Read 4 — the live `chosen` node's initrd bounds, to close the FD-base rationale (closes the
conflict in Step 13):
```
adb shell 'od -An -tx1 /proc/device-tree/chosen/initrd-start /proc/device-tree/chosen/initrd-end'
```
FACT: contents of `/proc/device-tree/**` were Permission denied from the *unprivileged* shell
(`notes/s10e-live-reads.md:244-247`) but readable as root in TWRP (`notes/s10e-root-reads.md:105-132`),
so this read only works in the TWRP-root state.

### 3d. What the reads would NOT give (FACT, already recorded)
`notes/s10e-live-reads.md:289-292` records that the fb base and PRAM choice cannot be settled by
adb from the unprivileged shell; §3 of `notes/s10e-root-reads.md` closed the fb base from
`/proc/device-tree/chosen/bootargs` as root, so the bootargs node IS readable in TWRP-root and
would also carry `s3cfb.bootloaderfb=` again.

### Step 15 - mechanical check of the FD's entry (the shim's assumption about the FD)
FACT (read this session, first 8 bytes of firmware/edk2/Build/Beyond0LtePkg/DEBUG_CLANGPDB/FV/BEYOND0LTE_UEFI.fd):
  bytes 00 c8 01 14 | 00 00 00 00
  code0 = 0x1401C800, opcode mask 0x14000000 = unconditional ARM64 `B`
  imm26 = 116 736 -> branch to FD+0x72000 (FD base + 466 944)
  -> bootshim.S:26-27 ("The FD's first word is an ARM64 `B` that GenFv wrote into the FV-header
     zero-vector, jumping to the PeilessSec SEC entry") holds for the 9820 FD too. Verified, not assumed.
FACT: FV header FvGuid at FD+0x10 = 78e58c8c-3d8a-1c4f-9935-8961-85c3-2dd3 (the built FVMAIN_COMPACT identity).

---

## ANSWER 4. The new constants that must be closed before an S10e image can be built

Each row: the constant, what the star2lte pipeline uses today, the read that closes it, and
whether that read is cheap (a device read / a file read) or needs a boot cycle.

| # | Constant | Today's value in the 9820 tree | Read that closes it | Cheap? |
|---|---|---|---|---|
| K1 | **the reference boot image itself** (the container sboot accepts) | none — `firmware/device-data` absent, no `.img` on the machine | `dd if=/dev/block/by-name/recovery` first page then whole image, from TWRP as root (Read 1/2 in ANSWER 3). Cross-check: untar `AP_G970FXXSGHWC1_..._meta_OS12.tar` in `C:\Users\ilya\code\Dumps\` for the stock `boot.img`/`recovery.img` | cheap (device read / local tar) |
| K2 | **boot image page size** and section order for the 9820 container | unread | the ref image's own header word at +0x24 (`bootimg.py:51`, `AndroidBootImage.cs:40`); comes free with K1 | free with K1 |
| K3 | **does the wrapped image fit BOOT** (57 671 680 B) | unknown for S10e; star2lte ref is 42 670 080 B (`WinReTwrpBuilder.cs:66-67`) | computed from K1's section sizes; `Validation.cs:58` enforces it | free with K1 |
| K4 | **UEFI_BASE** (the FD's link/run address, and the shim's copy destination) | `0x80080000` (`Beyond0LtePkg.fdf:68`, `Beyond0LtePkg.dsc:415,417`) marked UNVERIFIED as an observed address | two reads: (a) `/proc/device-tree/chosen/initrd-start` + `initrd-end` as root in TWRP, to re-derive the "clear of the initrd" constraint and settle the 0x84000000-vs-0x89000000 conflict (Step 13); (b) the real sboot kernel-drop address, which is only observable by booting the shim (the DIAG breadcrumb/paint is the instrument) — or from Samsung's LK4/LK5 source as a primary document | (a) cheap, (b) NOT cheap |
| K5 | **UEFI_SIZE** | `0x700000`, derived from the FD's real length (pack-uefi.ps1:79-81) — already correct for the built FD | no read needed; it is `ceil(fdLen/16)*16` | n/a |
| K6 | **ARM64 Image header the sboot accepts**: text_offset, image_size, flags | `0x80000`, `0x800000`, `0xa` (`bootshim.S:50-52`) | text_offset and flags are CLOSED by the SM-G970F kernel Image header read this session (`C:\Users\ilya\code\Dumps\twrp_device_tree\kernel`: 0x80000, 0x0a) — FACT. `image_size`'s *requirement* is still open: the vendor kernel declares 0x03138000 for itself; nothing says what sboot demands for ours | cheap (local file) for 2 of 3 |
| K7 | **shim link base** `-Ttext` | `0x80080000` (`pack-uefi.ps1:93`) | no read needed — bootshim.S:29-30 says the shim is position-independent, so this only resolves local offsets | n/a |
| K8 | **watchdog WTCON addresses and the offset inside the window** | `0x10050000`, `0x10060000` (`bootshim.S:72-75`) | bases are VERIFIED (`Exynos9820.h:431` cl0@10050000; `:426` cl2@10060000). The **register offset** the shim assumes (WTCON at window+0x00) is NOT read: `ExYNOS_WDT_SIZE` itself is marked UNVERIFIED (`Exynos9820.h:432`). Close with Samsung's s3c2410 header or a DT `reg` for `watchdog_cl0` | cheap (vendor DT / kernel source) |
| K9 | **MMU / I-cache / D-cache state at handoff** | assumed MMU OFF, caches off (`bootshim.S:19-22`, `:78-86`, `:175-196`) — "device-verified" there means on star2lte | no cheap read. Close by the first boot with the DIAG paints/breadcrumbs, or from Samsung's LK4 handoff code | NOT cheap |
| K10 | **the 9820's monitored / write-protected RAM zones** (the RKP analogue) | none known; the 9810's `0x80094000-0x828DBFFF` is explicitly not carried (`Beyond0LtePkg.fdf:63-65`, `Beyond0LtePkg.dsc:339-345`) | no DT node carries them. Close from Samsung's LK4/RKP configuration (primary source) or by observing a reset on the first boot | NOT cheap |
| K11 | **temp SP for the shim** | `0xA0000000` (`bootshim.S:172`) — now known to sit INSIDE the 9820's `camera` reservation 0x99000000..0xA7400000 (`notes/s10e-root-reads.md:116-117`) | closable by scan against the VERIFIED reservation table (notes/s10e-root-reads.md §4 + notes/vendor-dt-reserved.md): the interval 0x80780000 (FD end) .. 0x87000000 (`el2_code` base) is unclaimed by any read. Caveat: the scan is incomplete while K10 is open | cheap, but only as complete as K10 |
| K12 | **DIAG breadcrumb (ramoops pstore) address** | `0xFED14000` (`bootshim.S:94-104`, `:154-161`, `:189-196`) — VERIFIED wrong (inside `log_bcm`/`log_llc`) | already DECIDED: the unclaimed gap `0xD9801000..0xF0000000` (`notes/vendor-dt-reserved.md:61-65`). Decision for the first boot is to COMPILE IT OUT, matching `BEYOND0LTE_PRAM_DIAG 0` (`ArmPlatformLib.c:31,:33-35`) — note the shim writes it unconditionally today, so this is a code change, not just a constant | cheap |
| K13 | **DIAG framebuffer base + geometry** | `0xCC000000`, 1440x2960, rows 200-650 / 750-1200 (`bootshim.S:107-120`, `:139-152`) | CLOSED now: base `0xCA000000`, 1440 x 3040, 4 B/px (`Exynos9820.h:450-456`, `notes/s10e-root-reads.md:91-96`). Row-200 address becomes 0xCA000000 + 200*1440*4 = 0xCA1C2000; a 450-row band is still 648 000 px (width unchanged) | cheap (already read) |
| K14 | **flash target** | `sda10` (`pack-uefi.ps1:126`) | CLOSED: `sda14` / `/dev/block/by-name/boot`, LBA 27 264..41 343 (`notes/s10e-root-reads.md:58,:174`, `notes/s10e-partitions.md:38`) | cheap (already read) |
| K15 | **the packer script** (`repack-with-ref.py`) | MISSING from the repo entirely | not a hardware constant — write it on top of `tools/twrp-winre/bootimg.py:39-122` (or the C# `AndroidBootImage.cs`), which already round-trips this container format | n/a |
| K16 | **the freshness-guard source dirs** | `Platform\Star2LtePkg`, `Silicon\Exynos9810Pkg` (`pack-uefi.ps1:55-57`) | mechanical, not a read: retarget to `Beyond0LtePkg` / `Exynos9820Pkg` | n/a |

### 4b. Minimum set that blocks a first attempt
INFERENCE (mine), ordered by blast radius:
1. **K1 + K2 + K3** — without a device-proven container there is nothing to flash into, and
   the size bound (57 671 680) is unmeasured. All three come from ONE device read.
2. **K15** — the wrapper does not exist in the repo for either device.
3. **K13, K14, K12** — already closed by reads in this repo; they are code changes to bootshim.S,
   not new reads.
4. **K4, K6(image_size), K9, K10, K11** — these are the boot hazard, not the build hazard:
   a build succeeds with them wrong (HANDOFF §2 rule 31-34 / §5 "A build will succeed with them
   wrong, so they are a boot-time hazard, not a compile-time one").
5. K5, K7, K8(bases), K16 need no new reads.

### 4c. What is NOT blocking (FACT)
The FD exists and builds (`notes/m8-build.md:43-51`, `build-uefi.ps1:36` has the
`Beyond0LtePkg` branch); its entry word is a real `B` (Step 15); the FD is exactly 0x700000
so UEFI_SIZE needs no decision; the GIC, UART, CPU topology, fb base and geometry are VERIFIED.

---

## 16. This pass — the shim's constants, checked against the 9820 map

Read `firmware/tools/bootshim/bootshim.S` in full (207 lines). What is device-shaped
in it, and what this repo already says about each:

| shim constant | star2lte value | 9820 status |
|---|---|---|
| `UEFI_BASE` (`:38`, must equal `PcdFdBaseAddress`) | `0x90000000` | `Beyond0LtePkg.fdf:68` carries `0x80080000` and marks it **UNVERIFIED** |
| `UEFI_SIZE` (`:41`) | `0x700000` | verified: the built `BEYOND0LTE_UEFI.fd` is exactly 7 340 032 B |
| watchdog cl0/cl1 (`:72,:74`) | `0x10050000`, `0x10060000` | **VERIFIED** for the 9820 (`Exynos9820.h:426,431`) |
| ramoops breadcrumb base (`:95,:156,:190`) | `0xFED14000` | **known bad on the 9820**: inside `debug_snapshot.log_bcm`/`log_llc` (`notes/s10e-root-reads.md` §4) |
| framebuffer paint (`:111`) | `0xCC000000`, 1440×2960 | **wrong here**: `EXYNOS_FB_BASE = 0xCA000000`, 1440×3040 (`Exynos9820.h:450-456`, `notes/s10e-root-reads.md` §3) |
| temp SP (`:172`) | `0xA0000000` | **inside the verified `camera` reservation** `0x99000000+0xE400000` on the 9820 |
| Linux-header `text_offset` (`:50`) | `0x80000` | the SM-G970F kernel Image artefact confirms `0x80000` and `flags 0xa`; the **required `image_size`** for this sboot is not known |
| FD's first word is an ARM64 `B` (`:26-27` assumption) | true | checked mechanically on the 9820 FD: first word `0x1401C800` is a real `B` |

Unchanged from the earlier pass: `repack-with-ref.py`, `firmware/device-data/` and
`tools/bootimg-pack.sh` do not exist in this checkout, so no container wrapper exists
for either phone. The reference container for the S10e has to come off the phone
itself (`/dev/block/by-name/recovery` = sda15, `/dev/block/by-name/boot` = sda14).

## Sources (this task)
Local, read this session:
- `firmware/tools/pack-uefi.ps1` (1-126), `firmware/tools/build-uefi.ps1` (1-147),
  `firmware/tools/bootshim/bootshim.S` (1-207)
- `firmware/Platform/Beyond0LtePkg/Beyond0LtePkg.fdf` (1-210 of 302), `Beyond0LtePkg.dsc` (330-429 of 544)
- `firmware/Platform/Star2LtePkg/Star2LtePkg.fdf` (1-120 of 239), `Star2LtePkg.dsc` (grep :293,:295)
- `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` (1-471, whole)
- `firmware/tools/twrp-winre/../tools/twrp-winre/bootimg.py` (1-274, whole)
- `installer/src/S9Woa.Installer.Core/Toolset/Validation.cs` (1-96), `Twrp/AndroidBootImage.cs` (1-146),
  `Twrp/WinReTwrpBuilder.cs` (55-84 of 481)
- `notes/m8-build.md`, `notes/m8-dscfdf.md`, `notes/s10e-root-reads.md`, `notes/s10e-live-reads.md` (§1-80, §225-292),
  `notes/s10e-partitions.md`, `notes/vendor-dt-reserved.md`, `notes/what-to-lookup.md`,
  `notes/installer-s10e-gaps.md`, `notes/m9-install-attempt.md`
- `docs/exynos9820-port/HANDOFF.md` (1-120, 290-421), `docs/exynos9820-port/MILESTONES.md` (grep),
  `docs/map/firmware.md`, `docs/map/tools.md`, `docs/map/installer.md`, `docs/map/release.md`
- `release/provenance.json:513-518`, `release/import-manifest.json:45-47`
- `firmware/edk2/Build/Beyond0LtePkg/DEBUG_CLANGPDB/FV/BEYOND0LTE_UEFI.fd` (first 16 bytes, size)
- `C:\Users\ilya\code\Dumps\` listing; `C:\Users\ilya\code\Dumps\twrp_device_tree\kernel` first 64 bytes
- `& "C:\Users\ilya\code\platform-tools\adb.exe" devices` -> empty (Step 11)
Web: none consulted in this task.

## Correction pass (harsh review of these notes)
- `notes/s10e-root-reads.md` camera row is **line 115** (`| reserved-memory/camera | 0x99000000 | 0xE400000 |`),
  not 116-117. 116 = camera_contig, 117 = secure_camera. The claim "0xA0000000 is inside
  camera (0x99000000+0xE400000 = 0x99000000..0xA7400000)" is unchanged and correct.
- Re-verified mechanically: `Beyond0LtePkg.fdf:68` = 0x80080000 and `Beyond0LtePkg.dsc:415,417`
  = 0x80080000 (grep); `Star2LtePkg.dsc:293,295` = 0x90000000 (grep). Confirmed.
- Re-verified: `git ls-files firmware/tools/*` -> exactly three files; no repack-with-ref.py,
  no bootimg-pack.sh, no device-data. Confirmed.
- Re-verified: FD length 7 340 032 = 0x700000 exactly, so pack-uefi.ps1:79-81 yields
  UEFI_SIZE = 0x700000 with no rounding change. Confirmed.
- Re-verified: FD code0 = 0x1401C800 (opcode 0x14000000 = `B`, imm26 116736 -> +0x72000). Confirmed.
- Unchanged uncertainty: K4b (the real sboot kernel-drop address), K9, K10 remain NOT closable
  by a cheap read; recorded as such rather than filled with a value.

## Correction pass 2 (tool invocation line numbers in Step 1)
Re-read of pack-uefi.ps1:86-97 gives: clang = **:89-90** (not :92), ld.lld `-Ttext=0x80080000 -e _head --no-relax`
= **:92** (not :93), llvm-objcopy -O binary = **:94** (not :94 - correct), `$shimLen` = :96.
The freshness-guard comment that explains why the LINK step is needed is :86-88.
Everything else cited in Step 1 (:30, :44-45, :55-57, :68, :79-81, :102-107, :111-116, :120, :121, :126)
is confirmed at those lines.

## Step 16 - the S10e's OWN sboot-accepted container, read this session (closes K1/K2/K3)

The phone was attached and in TWRP (`& adb devices` -> `RF8M21HYDSZ   recovery`). Reads
only, as root in TWRP (the state `notes/s10e-root-reads.md` proved can read block devices):

```
adb shell 'dd if=/dev/block/by-name/recovery of=/tmp/ref-recovery-head.img bs=4096 count=1'
adb shell 'dd if=/dev/block/by-name/boot     of=/tmp/ref-boot-head.img     bs=4096 count=1'
adb pull <both>   # parsed locally with the field layout of tools/twrp-winre/bootimg.py:58-93
```

FACT (parsed from the two pulled first pages, header v0 `<9I` at +8):

| field | recovery (sda15, the TWRP this phone booted) | boot (sda14) |
|---|---|---|
| magic @0 | `ANDROID!` | `ANDROID!` |
| kernel_size @0x08 | 44 363 788 | 44 380 172 |
| ramdisk_size @0x10 | 10 905 981 | 0 |
| second_size @0x18 | 0 | 0 |
| page_size @0x24 | **2048** | **2048** |
| dt_size @0x28 | 1 | 1 |
| kernel_addr @0x0c | 0x10008000 | 0x10008000 |
| ramdisk_addr @0x14 | 0x11000000 | 0 |
| min image (no tail) | 55 277 568 (52.7 MiB) | 44 386 304 (42.3 MiB) |
| fits BOOT (57 671 680, `Validation.cs:12`) | **yes** | **yes** |

Closes:
* **K1** — the reference container is the phone's own RECOVERY partition (sda15). It is the
  image this device's sboot read to enter TWRP, so it is device-proven, not a download.
  Cross-check: recovery kernel_size 44 363 788 == the `C:\Users\ilya\code\Dumps\twrp_device_tree\kernel`
  artefact byte-for-byte (Step 10 read that file's header; same length) -> the recovery slot's
  kernel IS that TWRP kernel.
* **K2** — the 9820 container's **page_size is 2048**, not the 4096 the notes assumed. The repack
  must read it from the ref (bootimg.py:51 already does), never hardcode 4096.
* **K3** — the wrapped image fits BOOT. kernel becomes cat(shim,FD) ~ 7.34 MB; keeping the ref's
  ramdisk (10.9 MB) the total is ~18.3 MB << 57 671 680. `Validation.cs:58` `ValidateUefi` passes.

INFERENCE (mine): dt_size = 1 in both headers means these containers carry no real DTBH blob
(the word is not a header_version here); the repack keeps the ref's `dt`/`tail` verbatim, so
whatever the vendor put there is preserved and sboot's acceptance is unchanged.

NOT closed by this read (still boot hazards, named UNVERIFIED in the shim): K4b the real sboot
kernel-drop address, K6 image_size requirement, K9 MMU state at handoff, K10 the 9820 write-
protected zones. kernel_addr 0x10008000 in the header is the *Android* load address; bootshim.S
says sboot drops at DRAM base + text_offset, which is the unresolved 0x80080000-vs-0x80100000
disagreement (Step 2c / ANSWER 2 §2c).

## Step 17 - starting the 9820 boot-shim port (resume after crash, HANDOFF §14 step 2)

Phone attached in TWRP (`& adb devices` -> RF8M21HYDSZ recovery). Star2lte bootshim.S
assembles with clang (exit 0), so `ic iallu` is accepted by the integrated assembler.

Port location: firmware/tools/beyond0lte-bootshim/bootshim.S (sibling of the star2lte
bootshim/bootshim.S, which stays byte-unchanged; pack-uefi.ps1:72 hardcodes the star2lte
path so the new file is not picked up by the 9810 packer).

Constant decisions (all from VERIFIED reads in Exynos9820.h unless marked):
- UEFI_BASE 0x80080000 = Beyond0LtePkg.fdf:68 / dsc:415,417. UNVERIFIED as an OBSERVED
  address (the initrd bound is the read; the base is the decision that follows).
- UEFI_SIZE 0x700000 = VERIFIED (built BEYOND0LTE_UEFI.fd is exactly 7340032 B).
- text_offset 0x80000, flags 0xa = VERIFIED via the SM-G970F kernel Image artefact
  (C:\Users\ilya\code\Dumps\twrp_device_tree\kernel header, notes §10).
- image_size 0x800000 = requirement UNVERIFIED (vendor kernel declares 0x03138000 for itself).
- watchdog stores 0x10050000 (cl0) + 0x10060000 (cl2, NOT cl1) = bases VERIFIED
  (Exynos9820.h:431 / :426). The WTCON register offset (window+0x00) is UNVERIFIED
  (EXYNOS_WDT_SIZE:432 UNVERIFIED).
- ramoops breadcrumb 0xFED14000 = VERIFIED WRONG (inside debug_snapshot log region
  0xFD900000..0x100000000, Exynos9820.h:112-114). COMPILED OUT behind
  BEYOND0LTE_PRAM_DIAG (default 0), matching ArmPlatformLib.c:31 BEYOND0LTE_PRAM_DIAG 0.
  Chosen replacement gap 0xD9801000..0xF0000000 is a DECISION, deliberately not enabled.
- DIAG paint base 0xCC000000 -> 0xCA000000 (EXYNOS_FB_BASE, Exynos9820.h:450), geometry
  1440x2960 -> 1440x3040 (EXYNOS_FB_HEIGHT:452). Row offsets unchanged because width
  (stride) is unchanged: row200 = 0xCA000000+200*1440*4 = 0xCA119400; row750 =
  0xCA000000+750*1440*4 = 0xCA41EB00; 450 rows x 1440 = 648000 px unchanged.
- temp SP 0xA0000000 = VERIFIED WRONG (inside camera reservation 0x99000000..0xA7400000,
  notes/s10e-root-reads.md:115). Candidate in the unclaimed gap FD-end 0x80780000 .. el2_code
  0x87000000, below both initrd readings (vendor DT 0x84000000, live 0x89000000): 0x83000000.
  Marked UNVERIFIED (K10 write-protected zones still open).
- MMU OFF at handoff = UNVERIFIED on 9820 (the 'device-verified' claim is a 9810 observation).
- RKP-monitored zones 0x80094000-0x828DBFFF = NO 9820 equivalent read (K10 open).

## Step 18 - the port itself, and its mechanical verification

`firmware/tools/beyond0lte-bootshim/bootshim.S` edited from the byte-identical
copy. What changed, and how each change was checked (not by re-reading):

- watchdog comment: cl1 -> cl2 (Exynos9820.h:426 says `watchdog_cl2@10060000`).
- ramoops blocks A/B/M: compiled out behind `#if BEYOND0LTE_PRAM_DIAG` (default 0,
  matching ArmPlatformLib.c:31 / Beyond0LteResetSystemLib.c:59 / PlatformMemoryMapLib.c:48
  / Beyond0LteAcpiPlatformDxe.c:57), base now `ldr x12, _pram_base` from a guarded
  `.quad BEYOND0LTE_PRAM_BASE` literal, so no 0xFED14000 survives.
- paint: `movk x9, #0xCC11` -> `#0xCA11` (0xCA119400) and `#0xCC41` -> `#0xCA41`
  (0xCA41EB00). Arithmetic re-checked: 0xCA000000 + 200*1440*4 = 0xCA119400;
  + 750*1440*4 = 0xCA41EB00.
- temp SP: `movk x6, #0xA000` -> `#0x8300` (0x83000000), marked UNVERIFIED (K10 open).
- DEFECT FOUND AND FIXED (harsh-review style): the 9810 shim reloaded its destination
  base with a second `ldr x5, _uefi_base` AFTER the copy loop. With UEFI_BASE 0x80080000
  the destination range 0x80080000..0x80780000 covers the shim's own literals, so that
  reload would read FD bytes, not the base. Fixed by keeping the base in x7 across the
  loop (`mov x7, x5` before, `mov x5, x7` after) - no second literal read.
- HAZARD RECORDED, NOT PAPERED OVER: if sboot drops the kernel at 0x80080000 or
  0x80100000 (K4b, UNVERIFIED), the destination range contains the running shim and
  the copy eventually fetches overwritten instructions. The live initrd bound
  (0x89000000) would be clear of it; the vendor DT bound (0x84000000) would not.
  No cheap read closes this; it stays a comment in the source.

Verification that ran (all exit 0):

```
clang -target aarch64-none-elf -ffreestanding -nostdlib -mgeneral-regs-only
      -DUEFI_BASE=0x80080000 -DUEFI_SIZE=0x700000 [-DBEYOND0LTE_PRAM_DIAG=0|1] -c bootshim.S
ld.lld -o shim.elf shim.o -Ttext=0x80080000 -e _head --no-relax
llvm-objcopy -O binary shim.elf shim.bin
```

- diag=0: shim.bin 288 B, code0 = 0x10000901, `code0 -band 0x9F00001F` = 0x10000001
  (the pack-uefi.ps1:105 check), adr immediate 0x120 != 0, magic "ARM" at +56.
  `llvm-nm`: _head 0x...0000, _payload 0x...0120 = end of file. Correct.
- diag=1: shim.bin 392 B, code0 = 0x10000C41, _payload 0x...0188 = end of file,
  _pram_base 0x...0050 emitted BEFORE _start 0x...0058, so the guarded literal
  precedes its uses (no literal pool after _payload, which would break the `cat`).
- `rg "0x90000000|0xCC|0xFED1|0xA000|STAR2LTE|2960"` in the ported file: 5 hits, all
  in comments that explicitly name the 9810 value as WRONG here. No live 9810 constant.
- The 9810 `firmware/tools/bootshim/bootshim.S` is byte-unchanged: `fc.exe` against
  the pre-port copy reported no differences before editing; `git diff` shows no change
  to that path.

## Correction pass 3 - harsh review of the port commit (d54d805)

Checked mechanically: `git diff HEAD~1 HEAD -- firmware/tools/bootshim/bootshim.S` empty
(9810 untouched); FD first word re-read as 0x1401C800 and length 7 340 032 (matches the
comment's claim); `rg x7` shows the register used only in the two new places; both
`-DBEYOND0LTE_PRAM_DIAG=0|1` builds assemble, link at -Ttext=0x80080000 and objcopy
exit 0; `llvm-nm` puts _payload at the last byte of the image in both builds.

Found and fixed (own commit, not folded into d54d805):

1. The header's Build comment claimed "No link step needed - there are no absolute
   relocations", which contradicts pack-uefi.ps1:86-88 (the link IS needed: a -c-only
   object can leave the adr/branch offsets unapplied). Comment corrected to show the
   real three-step invocation.
2. The 'B' and 'M' breadcrumbs read the `_pram_base` literal AFTER the FD copy, so
   with the channel enabled they inherit the same K4b self-overwrite question the copy
   has. Not silently left: recorded in the breadcrumb-channel comment.
3. A stray tab before the `br x5` comment (introduced by the comment rewrite).
4. `Set-Content -Encoding UTF8` used while fixing (3) added a UTF-8 BOM and CRLF to a
   file whose 9810 sibling is BOM-less LF - rewritten to BOM-less LF
   (`[System.IO.File]::WriteAllBytes` + `Encoding.UTF8.GetBytes`), re-verified:
   first bytes 47,42,32 and no CR anywhere, 14 344 B.

## Step 19 - the repack tool (HANDOFF §14 step 3), and the run that proves it

`firmware/tools/repack-with-ref.py` written: the tool pack-uefi.ps1:121 already
calls but that never existed in this checkout. It is device-agnostic - the page
size, the section order and the tail come from the reference image through
`tools/twrp-winre/bootimg.py` (BootImage.parse/serialize), and it refuses to
write unless the re-parsed output keeps ramdisk/second/dt/tail byte-identical
and keeps `page_size`.

Reference container pulled this session (reads only, root in TWRP, phone
`RF8M21HYDSZ recovery`):

```
adb shell 'dd if=/dev/block/by-name/recovery of=/tmp/ref-recovery.img bs=2048 count=26991'
adb pull /tmp/ref-recovery.img      # 55 279 616 B; trimmed to the header's own
                                    # end = 2048 + pad(44363788) + pad(10905981)
                                    #      + pad(0) + pad(1) = 55 277 568
```
Matches Step 16's numbers exactly (page 2048, kernel 44 363 788, ramdisk
10 905 981, second 0, dt 1, tail 0).

Run (kernel = cat(shim.bin 288 B, BEYOND0LTE_UEFI.fd 7 340 032 B) = 7 340 320 B):

```
repack-with-ref.py --ref ref-recovery-trim.img --kernel uefi-beyond0lte-kernel.bin \
    --output uefi-beyond0lte.img            -> exit 0, 18 253 824 B
```

Verified mechanically on the output, not by reading the code:
- `ramdisk`/`second`/`dt`/`tail` re-parse byte-identical to the ref; `page_size`
  still 2048.
- header page differs from the ref's in **24 bytes**, offsets 8..595: the four
  size words at 0x08/0x10/0x18/0x28 (only 0x08 actually changes value) and the
  refreshed SHA-1 id at 0x240..0x253. Nothing else in the header moved.
- kernel[0:4] = 0x10000901 (the shim's `adr x1,_payload`), kernel[288:292] =
  0x1401C800 (the FD's first word) - the cat order is right.
- 18 253 824 < 57 671 680 (BOOT, `Validation.cs:12`) - the K3 fit holds with the
  real shim length, not the 7.34 MB estimate.

## Correction pass 4 - harsh review of the repack-tool commit (4215fe8)

Checked: the tool re-ran after every edit (exit 0, same 18 253 824 B); the
byte-diff of the header page was measured (24 bytes, offsets 8..595) rather than
asserted; `ramdisk`/`second`/`dt`/`tail` compared byte-for-byte after re-parse;
`kernel[0:4]` and `kernel[288:292]` decoded to prove the cat order.

Found and fixed (own commit):
- The inline comment claimed "the five header words" while the docstring (and
  bootimg.py:105-122) rewrite four size words plus the SHA-1 id - comment
  corrected to name them.
- Earlier in the same pass, before committing: the dead `if ... : pass` kernel
  check, `BootImage.BOOT_MAGIC` (module constant, not a class attribute), the
  wrong `sys.path` parent count that made the import fail, the overflow check
  placed after `serialize()` instead of before, an unused `import struct`, and
  a docstring sentence broken by the "five words" edit - all fixed pre-commit,
  each verified by re-running the tool.

## Step 20 - the beyond0lte packer, and the run that reproduces Step 19

`firmware/tools/pack-uefi-beyond0lte.ps1` written as a SIBLING of the 9810
`pack-uefi.ps1` (which stays byte-unchanged - `git diff` on that path is empty):
the 9810 script hardcodes `tools\bootshim` (`:71-72`), the Star2Lte FV dir
(`:44-45`) and `STAR2LTE_UEFI.fd`, so it cannot be pointed at this phone by
parameters alone. Differences: the ported shim path, `Beyond0LtePkg` FV dir,
`-UefiBase` default `0x80080000` (= `Beyond0LtePkg.dsc:415`), and NO default
reference image (nothing in this checkout is an S10e `.img`, so `-Ref` is
required rather than silently wrong).

Run (exit 0, artefacts only under the Build/ dir and the shim dir):

```
powershell -ExecutionPolicy Unrestricted -File firmware\tools\pack-uefi-beyond0lte.ps1 \
  -Ref <pulled ref-recovery-trim.img> -Output <temp>\pack-out.img
==> FD 7 340 032 B ; shim.bin 288 B ; code0 0x10000901 ; magic ARM
==> kernel 7 340 320 B ; repack -> 18 253 824 B, page_size 2048 read from the ref
```
Same numbers as the manual Step 19 run, so the pipeline is reproducible in-tree.

Also: the shim's `.o/.elf/.bin` are regenerable artefacts that were showing up
as untracked; added explicit `.gitignore` lines for both phones' shim dirs
(no rule existed before, and the 9810 dir happens to be clean only because it
was never built in this checkout).

## Step 21 - the reference container's identity, so the ref is re-pullable

```
adb shell 'dd if=/dev/block/by-name/recovery of=/tmp/ref-recovery.img bs=2048 count=26991'
adb pull /tmp/ref-recovery.img
```
`bs=2048 count=26991` = 55 277 568 B exactly (the header's own end:
2048 + pad(44 363 788) + pad(10 905 981) + pad(0) + pad(1)); an earlier pull
used count=26992 and was trimmed locally to the same bytes.

- ref-recovery-trim.img  SHA1 `B9CC7676445D88A5698202B84E451D2A3D5A8FE6` (55 277 568 B)
- pack-out.img (shim+FD in that ref) SHA1 `B3F4459A9DF281B5E0DDAD3A6FD3BC1FDE486E8D` (18 253 824 B)

Neither file is in the repo (`*.img` is gitignored, `.gitignore:11`); the hashes
are the identity. A ref that does not hash to the first value is a different
container and the comparison above must be redone, not assumed.

Still NOT closed by this: the image has never been flashed or booted (ground
rules 5/6), and K4b (where sboot drops the kernel) is what decides whether the
shim's own copy step survives - see the hazard comment in bootshim.S.

## Session 2026-10-10 - "build and run": first boot attempt (T07)

User gave explicit consent to attempt the first Windows boot on the S10e. This
is the point where ground rules 5/6 get their green light. Progress log below.

### B1 - rebuild the UEFI platform

FACT ran `powershell -NoProfile -ExecutionPolicy Unrestricted -File firmware\tools\build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG`
- exit 0, `- Done -`, build total time 42 s (incremental).
- `BEYOND0LTE_UEFI.fd` 7 340 032 B (unchanged size).
- `FVMAIN_COMPACT` [99%Full] 700416 total / 697344 used / **3072 B free**; `FVMAIN` [27%Full] 1161064 used.
- log: `%TEMP%\m8-build-uefi-20261010.log`.
- FD is not byte-deterministic (notes/m8-build.md §8) - compare sizes/FV numbers, not hashes.

### B2 - rebuild the UFS storage driver

FACT ran `powershell -NoProfile -ExecutionPolicy Unrestricted -File drivers\Exynos9820Ufs\build-ufs.ps1 -SourceDir drivers\Exynos9820Ufs -OutDir %TEMP%\opencode\ufs-9820-run1 -WideUncached -DmaWindow`
- exit 0, `EXACT_BUILD_OK` / `BUILD_OK`.
- `Exynos9820Ufs.sys` 49152 B sha256 `3a19a6bb5b88cac04aaccbe292fb998c60bad9be0a6c783dbb1fafd22caf0d0b` - **identical to the fence-pass build** (HANDOFF §12/§13 `3a19a6bb…caf0d0b`), so the driver is byte-stable and the fence constants are unchanged.
- `UfsDiag.exe` 213504 B sha256 `7f4f7436f275125657b0801fcb57ada1c7a49604e7909a6e02863caf542965bf` - also identical.
- ABI `{"signature":960910165,"version":1376256,"bytes":992,"perf":1,"wide_uncached":1,"dma_window":1,"portable_kernel_contract":1}`.

### B3 - rebuild the installer

FACT `dotnet build installer\S9Woa.Installer.sln -c Debug -p:Platform=x64` -> Build succeeded, 0 warnings / 0 errors.
FACT `dotnet test installer\tests\S9Woa.Installer.Core.Tests\S9Woa.Installer.Core.Tests.csproj -p:Platform=x64` -> **194 passed, 0 failed** (matches the recorded suite count; `beyond0lte` still `NotImplemented` on purpose - see T08).
- logs: `%TEMP%\m9-installer-build.log`, `%TEMP%\m9-installer-test.log`.

### B4 - regenerate the candidate boot image

FACT ran `powershell -NoProfile -ExecutionPolicy Unrestricted -File firmware\tools\pack-uefi-beyond0lte.ps1 -Ref %TEMP%\opencode\ref-recovery-trim.img -Output %TEMP%\opencode\pack-out-run1.img`
- exit 0. FD 7 340 032 B; shim.bin 288 B; code0 0x10000901; magic ARM64; kernel 7 340 320 B.
- ref page_size 2048; kernel slot 44 363 788 -> 7 340 320; ramdisk 10 905 981 / second 0 / dt 1 / tail 0 all verbatim.
- **output 18 253 824 B** (not 18 255 872). The 2048-byte delta vs HANDOFF §17/§18 is the ref: §17 packed against the UNtrimmed `ref-recovery.img` (55 279 616 B); this run used the byte-exact trimmed `ref-recovery-trim.img` (55 277 568 B, notes Step 21). Same kernel, same base 0x83300000. The trimmed ref is the correct container end, so 18 253 824 B is the current candidate.
- recovery slot 55 279 616 B -> 3.0x margin, well under BOOT 57 671 680 B.

### B5 - the flash/run blockers (found before any write)

FACT `adb devices -l` -> **empty**, twice (also after a no-op `adb reboot`). The S10e is NOT attached to this PC right now. Nothing can be flashed or booted until the user plugs it in and accepts the access prompt.
FACT heimdall is NOT installed as an executable (`Get-Command`/`where` empty). Only a source tarball exists: `C:\Users\ilya\code\platform-tools\Heimdall-v2.2.2.tar.gz` (note: v2.2.2, newer than the v2.1.0 the notes analysed). It has never been built or run here, and the notes' two hazards (no partition-size check PR #555; untested on an S10e) still apply.
CONCLUSION: the "run" half of "build and run" is blocked on the device. Build side is complete and green. Stopping here per the "if it can only be decided on a real phone, say so and stop" rule; the flash is also the destructive write on the only S10e.

### B6 - T01: UFS DMA window re-picked (phone re-attached first)

FACT `adb devices -l` now shows `RF8M21HYDSZ device product:beyond0ltexx model:SM_G970F` - the S10e is attached and authorized (normal Android, not recovery).
CHANGE `Exynos9820Ufs.h:914-915`: `UFS_DMA_WINDOW_FIRST 0x90000000 -> 0xA7400000`, `LAST 0xBC800000 -> 0xBAB00000`, both still UNVERIFIED (CHOSEN band). Added `C_ASSERT(FIRST < LAST)`. Comment names the reads (bank0 end 0xBAB00000 = DTB reg + /proc/iomem; camera reservation end 0xA7400000).
VERIFY rebuilt both modes: `-DmaWindow` (ENFORCE=1) EXACT_BUILD_OK 49152 B sha256 `47391120…` (window baked into DmaWindowFirst/Last diagnostic fields, .c:4956-4957); default (ENFORCE=0) EXACT_BUILD_OK 48640 B. Old `9810ULL` values gone from the #defines; remaining `0xBC800000` hits are prose.
FOLLOW-UP (recorded, not fixed): `Exynos9820Ufs.c:1427` and `Exynos9820Ufs.h:79` still
narrate `EXYNOS_DRAM` end as `0xBC800000` (9810 total) - stale vs live `0xBAB00000`, different
constant, out of T01 scope.

## B7 - the first-boot WRITE (recovery slot), consented by the user

FACT user flashed vbmeta_disabled + TWRP manually, booted Android, then `adb reboot recovery`
-> device reconnected as `recovery` (TWRP). TWRP shell OK: /tmp tmpfs 2.5G,
`by-name/recovery -> sda15`, `by-name/boot -> sda14` (boot untouched).
FACT `adb push pack-out-run1.img -> /tmp/uefi-recovery.img` = 18 253 824 B on device.
FACT WRITE: `dd if=/tmp/uefi-recovery.img of=/dev/block/by-name/recovery bs=1024` ->
17826 records in/out, 18 253 824 B copied, then `sync`. Recovery slot 55 279 616 B, so the
image occupies the first 18.25 MB; trailing old-TWRP bytes remain (container header defines
the size, sboot reads from the start).
VERIFY byte-exact: local sha256 `403478fb3e528f2cebe7d83fa2c62444862c07732cdf084f2dea5f8a39424215`
== on-device `dd if=recovery bs=1024 count=17826 | sha256sum` = same. The slot now holds the
UEFI container.
NEXT: reboot to recovery and OBSERVE the panel (UEFI paint at 0xCA000000 / bootloop / silent).

## B8 - first-boot OBSERVATION: fell back to DOWNLOAD mode

FACT `adb reboot recovery` (exit 0) -> device did NOT come back as `recovery`/Android; the
user reports the phone **rebooted into download mode** (loke). adb stayed `(none)` through the
poll. So sboot rejected the recovery container or the payload crashed and the boot-failure path
entered download mode. This is a BOOT hazard, not a build error - the write was byte-exact (B7).
FAULT CLASS (T07 §3): need to separate (a) sboot rejected the container before running it
(header/AVB/load address) vs (b) the payload ran then reset (watchdog not stopped / crash).
The panel content during the attempt decides which. ASKING the user what the screen showed.
Device is currently in download mode; recovery slot still holds the UEFI container (write not
reverted).

CONFIRMED: the panel went **straight to the download-mode screen** - no UEFI text, no paint, no
Windows. So the payload never reached a painted frame. Two readings remain, and the panel alone
cannot fully separate them (UEFI paints only after it boots far enough):
  (a) sboot refused the container before jumping to the payload (header / AVB / drop address), or
  (b) the shim ran and jumped to UEFI at the DECISION base 0x83300000, but UEFI faulted before
      painting (bad base / MMU state K9 / write-protected zone K10 / watchdog not stopped).
Either way the prime suspects are the never-booted DECISION constants: UEFI_BASE 0x83300000
(K4b), the shim temp SP, and the K10 write-protected zones - all UNVERIFIED, none validated.
SAFETY: boot (sda14) is untouched, so normal Android still boots (`adb reboot`). The recovery
slot now holds the UEFI container, not TWRP; restore TWRP by re-flashing the tar before any
further experiment. Windows booting is NOT established, so ground rules 5/6 still forbid
casual media writes.

## B9 - sboot's own diagnostic text (from the download-mode screen) REFRAMES the fault

User read the download-mode screen's error text:
```
dt table header check fail: <unknown error>
load_kernel: could not do Normal boot. (DT LOAD fail)
vmbeta: Error verifying vmbeta image: OK_NOT_SIGNED (3)
CUSTOM VMBETA, VMBETA No sign info
```
READ (mine, from the text):
- **vbmeta is NOT the blocker.** `OK_NOT_SIGNED (3)` + `CUSTOM VMBETA` = sboot accepts the
  verity-disabled vbmeta as a custom/unsigned image (a warning, not a refusal). The AVB ordering
  was fine.
- **The blocker is the boot container's DT (device-tree) load**, not the UEFI payload:
  `dt table header check fail` -> `load_kernel: could not do Normal boot (DT LOAD fail)`. sboot
  took the **Normal boot** path for the recovery slot, tried to load a DTB, the DT header check
  failed, so it never jumped to the kernel at all.
CONCLUSION: fault class is (a) - sboot refused the container BEFORE running the payload. The
never-booted DECISION constants (UEFI_BASE 0x83300000, temp SP, K10 zones, fb base) are NOT the
cause here - they are never reached. The suspect is the container format: the repack kept the
ref's `dt` section (packer log B4 said `dt 1 (verbatim)` - i.e. ~empty), but sboot's Normal-boot
path on the 9820 wants a valid DTB header. Investigate `repack-with-ref.py` + the ref container's
dt/dtbo fields.

## B10 - the ref container has NO device tree; that is the DT LOAD fail

FACT probed `ref-recovery-trim.img` (the beyond0lte TWRP recovery container we repacked):
`kernel=44363788 ramdisk=10905981 second=0 page=2048 dt_size=1`; no `DTBH` magic and no
DTBO magic (0xA1123344) anywhere in the 55 MB. So the TWRP-stripped recovery container carries
**no device-tree blob**.
READ (mine): sboot's `load_kernel` Normal-boot path requires a valid DT (DTBH/DTBO). The TWRP
container has dt_size=1 (empty), so the DT header check fails and sboot never jumps to the
kernel. This is a CONTAINER-format fault, upstream of the UEFI payload - the shim's ARM64 Image
header (text_offset 0x80000, image_size 0x800000, flags 0xa) is identical in shape to the 9810's
booting shim, so the shim is not the differentiator.
FIX DIRECTION (ties to T06): repack the [shim][FD] kernel into a container that DOES carry a
valid DTBH - the phone's **stock** recovery/boot container (in `C:\Users\ilya\code\Dumps\`, the
AP_G970FXXSGHWC1 meta tar) has the vendor dtb; the TWRP one does not. T06 ("re-read the STOCK
recovery container geometry before repacking") is exactly this. Re-derive the ref from stock,
not from the TWRP-era pull.
CAUTION: this is inference from sboot's diagnostic text + the container probe; not yet a
validated fix. Nothing re-flashed; recovery slot still holds the failed container.

## B11 - CORRECTION: the stock container does NOT carry a DTBH either (B10 fix-direction was wrong)

FACT extracted + LZ4-decompressed the STOCK images from `AP_G970FXXSGHWC1_..._meta_OS12.tar`
(python lz4 4.4.5, frame format):
- stock `recovery.img` 67 633 152 B: `ANDROID!` kernel=44380172 ramdisk=14946983 second=0
  page=2048 **dt_size=1**, no DTBH, no DTBO magic.
- stock `boot.img` 57 671 680 B: kernel=44380172 ramdisk=0 second=0 page=2048 **dt_size=1**,
  no DTBH/DTBO; the "dt region" is the `SEANDROIDENFORCE` trailer.
So the stock containers have the SAME dt_size=1 as the TWRP ref. **B10's fix direction
("repack against a stock container that carries a DTBH") is FALSE** - the stock container has no
DTBH to carry. On this Android-12 device the device trees are SEPARATE partitions
(`dt.img.lz4`, `dtbo.img.lz4` in the meta tar), not embedded in the boot container.
REVISED (mine): sboot's `load_kernel` DT check is about the **kernel Image's appended DTB**
(ARM64 boot protocol: a FDT 0xD00DFEED blob after `image_size`), which our `[shim][FD]` EFI
payload lacks. The stock/TWRP kernels are real Linux Images that carry an appended DTB; the
9810's `[shim][FD]` booted because the 9810 sboot is older/looser, the 9820's (Android-12 ABL)
enforces it. So the fix is in the PAYLOAD's Image header/DTB, not the container ref.
NEXT: compare the 9810 bootshim vs the 9820 bootshim for any appended-DTB / flag difference;
if none, the 9820 needs a valid appended DTB (or the header flag bit4 0x10 "can use device tree")
that sboot accepts. T06's premise (stock ref) is corrected here.

## B12 - shim comparison settles the fix: the payload needs an appended DTB

FACT read the 9810 `firmware/tools/bootshim/bootshim.S` in full: its ARM64 Image header is
`text_offset 0x80000, image_size 0x800000, flags 0xa, magic "ARM\x64"` and it appends **no DTB**
- identical shape to the 9820 `beyond0lte-bootshim/bootshim.S`. Yet star2lte boots Windows.
CONCLUSION (mine, from B10/B11 + this): the difference is the **loader generation**, not the shim.
The 9820 runs an Android-12 ABL whose `load_kernel` reads the kernel Image header, loads
`image_size` bytes, then expects a **DTB (FDT magic 0xD00DFEED)** at the next page-aligned offset
and checks its header. The stock/TWRP kernels are real Linux Images that carry an appended DTB
inside the kernel blob; our `[shim][FD]` EFI has `image_size 0x800000` but only ~0x700000 of
payload and zero padding after -> `dt table header check fail` -> `DT LOAD fail`.
FIX (the real one): append a valid DTB to the payload and make the Image header's
`image_size`/`flags` point sboot at it. Source for a DTB: the vendor `dtbo.img`/`dt.img`
(in the meta tar) or the phone's live FDT (`/sys/firmware/fdt`) read in a TWRP root shell.
T06 (stock-container ref) is NOT the fix; superseded. New task: append DTB to the boot payload.

## B13 - the vendor DTBs extracted (source for the T09 payload fix)

FACT extracted + LZ4-decompressed `dt.img.lz4`/`dtbo.img.lz4` from the meta tar (both 8 MiB
partitions, Samsung header then a real FDT):
- `dt.img`: FDT (0xD00DFEED) at 0x40, totalsize **239680** (0x3a840) -> `stock/dt.dtb` 239680 B.
- `dtbo.img`: FDT at 0x100, totalsize **176648** (0x2b208) -> `stock/dtbo.dtb` 176648 B.
So the primary device tree is `dt.dtb` (239680 B). That is the blob to append to the payload.
PLAN (T09): rebuild the container kernel as `[shim][FD][pad->align][DTB]` and set the shim's ARM64
Image header so sboot's load_kernel finds a valid FDT after `image_size` (likely set flags bit4
0x10 "can use device tree" -> 0xa|0x10=0x1a, and place the DTB at align_up(image_size, page)).
UNVERIFIED: sboot's exact DTB placement rule and whether it wants dt.dtb vs dtbo.dtb - decide by
a boot observation, not by guessing. Files: `C:\Users\ilya\AppData\Local\Temp\opencode\stock\`.

## B14 - boot #2 with the DTB candidate: AVB now bypassed, DT check STILL fails

FACT re-flashed TWRP, wrote `pack-out-dtb.img` (19 542 016 B, sha256 `894299e8…7994d34`, byte-exact
on sda15), rebooted to recovery -> **download mode again**. New panel text vs attempt 1:
`VERIFICATION_DISABLED bit is set` (standard AVB term) - so the vbmeta re-flash this time genuinely
disabled AVB verification (attempt 1 did not show it). GOOD: AVB is bypassed.
STILL FAILING: `dt table header check fail` -> `DT LOAD fail`. So the T09 DECISION (append raw
`dt.dtb` FDT at align(image_size=0x800000) + set flags bit4 0x10) did NOT satisfy sboot's DT check.
The DTB contract is still wrong. NEXT (principled): mirror the phone's OWN stock kernel Image
convention - inspect `stock/boot.img`'s kernel blob (44 380 172 B) ARM64 Image header (image_size,
flags) and where its appended DTB actually sits, and replicate THAT in the payload, rather than
guessing the placement. The stock kernel boots, so its Image-header/DTB layout is the ground truth.
SAFETY: boot (sda14) untouched, Android still boots; recovery slot holds the boot#2 container.

## B15 - CORRECTION: the appended-FDT theory (B12) is WRONG; stock kernel has no appended FDT

FACT parsed `stock/boot.img` kernel blob (44 380 172 B) ARM64 Image header:
`code0=0x14688000 code1=0 text_offset=0x80000 image_size=0x327b000 (52 932 608) flags=0xa
magic "ARM\x64" @0x38`. And `k.find(FDT 0xD00DFEED)` = **-1** - the stock kernel blob contains
NO appended FDT. So the stock kernel boots WITHOUT an appended device tree in its kernel blob.
=> B12's inference (sboot wants an appended FDT after image_size) is FALSE. The DT comes from the
separate `dt.img`/`dtbo.img` partitions (Android-12 ABL loads them by partition, not from the
container). Note the stock Image header `image_size` (52.9 MB) EXCEEDS the container `kernel_size`
(44.38 MB) - the container kernel is a partial/segmented load; sboot reassembles it with the
separate DT. Our single-shot `[shim][FD]` doesn't fit that model.
CONCLUSION: the DT LOAD fail is about sboot not finding the DT for a recovery boot of a
non-stock container, NOT about an appended FDT. Trial-and-error on the only device is the wrong
tool here. Reliable route = the edk2 S10e (beyond0lte) BootShim recipe (the Renegade/edk2-exynos
projects boot EDK2 on this exact device) - study how they satisfy the 9820 DT check, rather than
guess placement. T09's appended-DTB attempt is a dead end; reframe.

## B16 - ANSWER via prior art: the container must be Android boot image HEADER-V2 with a DTB section

Fetched edk2-exynos `Platform/Samsung/exynos9820/s10.sh.inc` `platform_build_bootimg()`: it runs
AOSP `mkbootimg.py --header_version 2 --dtb "Platform/Samsung/exynos9820/FdtBlob_compat/${DEVICE}.dtb"
--base 0x10000000 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --second_offset 0xf0000000
--tags_offset 0x100 --dtb_offset 0x0 --pagesize 2048 --os_version 13.0.0 --os_patch_level 2023-07
--cmdline buildvariant=userdebug --ramdisk dummyramdisk --kernel uefi-s10-kernel`. So sboot's
Android-12 ABL `load_kernel` "dt table header check" wants a **header-v2 boot image with a real
DTB section** (the FDT lives in the container's dtb section, `dt_size` at header +0x670, `dtb_addr`
+0x674). Our v0 Samsung repack leaves `dt_size=1` -> fails. The shim/payload shape is fine.
Built `firmware/tools/build-bootimg-v2.py` mirroring AOSP `write_header()` (V2 size 1660) byte-for-byte.

## B17 - boot #3 WRITE (header-v2 candidate), consented; observation pending

Candidate `pack-out-v2.img` 7 587 840 B, sha256 `53142c71776b6c6a2975de3a02da905dd6e924f1b4683f5b05b2baa8e614a9ef`
(kernel=[shim][FD] 7 340 320, empty cpio ramdisk 512, second 0, dtb=stock/dt.dtb 239 680 at file
offset 7 346 176 magic d00dfeed; header_version 2, page 2048, base 0x10000000, kernel_addr 0x10008000,
dtb_addr 0x10000000). Pushed to /tmp (on-device sha256 matched), `dd` to /dev/block/by-name/recovery
(sda15) 7 587 840 B, read back first 7410 KiB sha256 == candidate (byte-exact). `adb reboot recovery`
issued; `adb devices` now empty (consistent with download mode OR a real boot - panel decides).
PENDING: user observation of the panel. If it gets PAST the DT check we expect UEFI paint (the shim
paints a RED band rows 200-649, then GREEN rows 750-1199 after the FD copy).

## B18 - boot #3 OBSERVED: SAME `DT LOAD fail` -> the v2 container did NOT clear it on the RECOVERY path

Panel fell to download mode with the IDENTICAL `dt table header check fail` / `DT LOAD fail`. So
header-v2 + in-container DTB did not help when written to the RECOVERY slot. Reframe (consistent
with B15): the recovery partition is booted by ABL's *segmented recovery* flow - the stock recovery
kernel is a partial/segmented ARM64 Image (container kernel_size 44.38 MB < its declared image_size
52.9 MB) that sboot reassembles with the separate `dt.img`/`dtbo.img` partitions. A single-shot
`[shim][FD]` does not fit that model, so the DT LOAD fail is about the RECOVERY BOOT FLOW, not the
container header version. The proven edk2-exynos path writes `boot.img` to the **BOOT partition**
(sda14) and boots the normal Android boot flow, where ABL reads the header-v2 container's DTB.
NEXT (needs consent, destructive to Android boot): write `pack-out-v2.img` to sda14, reboot, observe.
Rollback = reflash stock `boot.img` (extracted at C:\Users\ilya\AppData\Local\Temp\opencode\stock\boot.img).

## B19 - boot #4 WRITE: v2 image to the BOOT partition (sda14), consented; reboot NORMAL pending

User consented ("do what you need"), back in TWRP. Confirmed by-name/boot -> sda14, by-name/recovery -> sda15.
Pushed pack-out-v2.img (sha256 53142c71...), `dd` to /dev/block/by-name/boot (7 587 840 B), read back
first 7410 KiB sha256 == candidate (byte-exact). This is the proven edk2-exynos path (boot.img on the
boot partition, normal Android boot flow reads the header-v2 container's DTB). `adb reboot` (normal)
issued next. PENDING: panel observation. Expect UEFI paint (RED band rows 200-649, then GREEN rows
750-1199) if the DT check clears and the shim runs.

## B20 - boot #4 OBSERVED: BOOT LOOP (not download mode) -> the DT check is now CLEARED

Panel showed a reboot loop, NOT the `DT LOAD fail` download-mode fallback. That is a real change:
the header-v2 container on the BOOT partition got PAST sboot's `load_kernel` DT check (B16/B18's
fix was correct) and sboot jumped into the kernel blob. The loop is downstream - the shim runs (or
partially) and faults/resets, so sboot retries. Failure has moved from the CONTAINER to the
SHIM/PAYLOAD stage: the never-validated DECISION constants (drop address vs UEFI_BASE 0x83300000,
temp SP 0x83000000, watchdog WTCON offsets, FB paint 0xCA000000) and the FD's own SEC entry.
adb shows no device (mid-loop or settled). PENDING: what it settled into (download / still looping).

## B21 - CONFIRMED: DT check cleared; loop is the shim faulting; sboot healthy

User brought the phone back to download mode MANUALLY (button combo) and it now shows the NORMAL
download-mode INFO banner, NOT the `dt table header check fail` / `DT LOAD fail` error. So sboot is
healthy and the header-v2 container on the boot partition definitively cleared the DT check (B16 fix
correct). The boot loop = sboot loaded the kernel, jumped to the shim, the shim faulted/resets, sboot
retries. Failure is now entirely in the SHIM/PAYLOAD stage (unvalidated 9820 constants: watchdog WTCON
addresses, FB paint address, drop-vs-UEFI_BASE copy, temp SP, FD SEC entry). Recovery slot (sda15)
still holds the failed boot #3 v2 image, so TWRP must be reflashed to get a shell. NEXT: reflash TWRP
(user), restore stock boot.img to sda14 (undo the loop -> Android safe), then diff our bootshim.S vs
edk2-exynos' device-proven exynos9820 BootShim.S to find the wrong 9820 constant.

## B22 - boot-loop diagnosis: our shim does far more than the device-proven exynos9820 BootShim

Fetched edk2-exynos `tools/BootShim/BootShim.S` (their proven flow for exynos9820): it is MINIMAL -
`adr x1,_Payload; b _Start`; a fake Linux header (first header text_offset/image_size/flags all ZERO,
then `_StackBase=UEFI_BASE`, `_StackSize=UEFI_SIZE`); `_Start`: x4=payload src, x5=UEFI_BASE, if equal
goto _Entry else `ldp/stp` copy UEFI_SIZE bytes, then `_Entry: br x5`. It does **NOT** touch the
watchdog, does **NOT** read/write `SCTLR_EL1`, does **NOT** disable caches, does **NOT** paint, does
**NOT** set SP. Our `firmware/tools/beyond0lte-bootshim/bootshim.S` adds all of those (watchdog stores
0x10050000/0x10060000, `mrs SCTLR_EL1`+`msr`, RED/GREEN paint 0xCA119400/0xCA41EB00, SP=0x83000000).
Any one faults on the 9820 -> the boot loop. Prime suspect: `mrs SCTLR_EL1` faults if sboot hands off
at EL0 (the proven shim never touches a privileged reg); also the watchdog/paint stores to unvalidated
9820 addresses. GROUNDED FIX: rewrite our 9820 shim to the proven minimal copy+branch flow (keep
UEFI_BASE/UEFI_SIZE + fake header), drop the unvalidated additions.

## B23 - session resume (Claude): the working-tree shim rewrite was left BROKEN; prior-art constants fetched

State found: `firmware/tools/beyond0lte-bootshim/bootshim.S` (uncommitted) was mid-rewrite to the minimal
flow - the copy loop is TRUNCATED after `ldp` (`ldr /* placeholder */`, no `stp`/`subs`/`b.ne`), so it
does not assemble, and `_dead` hand-encodes `.long 0xd420007e` labelled `wfe` (wfe is 0xd503205f; that
word is a BRK-class encoding). Neither fault was ever built or flashed (boot #4 ran the OLD shim).

Fetched from edk2-exynos master (raw.githubusercontent / git tree API, 2026-10-10):
- `tools/BootShim/BootShim.S` verbatim: `adr x1,_Payload; b _Start`; zero header (text_offset/image_size/
  flags 0) + "ARM\x64" + `.long 0` + `.ascii "EDK2-MSM"`; `_StackBase: .quad UEFI_BASE`; `_StackSize:
  .quad UEFI_SIZE`; `_Start: mov x4,x1; ldr x5,_StackBase; cmp; beq _Entry; ldr x6,_StackSize;
  _CopyLoop: ldp x2,x3,[x4],#16; stp x2,x3,[x5],#16; subs x6,x6,#16; b.ne; ldr x5,_StackBase;
  _Entry: br x5; _Dead: b _Dead`. No cache maintenance, no SCTLR, no SP, no MMIO.
- `configs/exynos9820.conf`: **FD_BASE=0x90000000, FD_SIZE=0x00700000** (build.sh:121 passes these to the
  shim as UEFI_BASE/UEFI_SIZE). OURS IS 0x83300000 (a DECISION, notes/k4b-drop-address.md). Prior art
  boots the 9820 FD at 0x90000000. Our reservation reads put nothing in 0x90000000..0x90700000 (nearest:
  mm_debug 0x91200000). -> follow-up task: move the FD base to the prior-art value (4 lockstep sites).
- `Platform/Samsung/exynos9820/exynos9820.dsc`: PcdArmArchTimerFreqInHz=26000000 (prior art for T05 -
  same as our carried value; still not an on-device read), PrePiStackBase 0x80C00000 size 0x40000,
  CpuVectorBase 0x80C40000, UefiMemPool 0x80C50000+0x0F3B0000, MipiFrameBuffer 0xca000000 (matches ours),
  GICD 0x10101000 / GICC 0x10100000.

### B23 review (harsh, of b17d8f6)
Checked mechanically: objdump of the built shim vs the fetched BootShim.S (same instruction sequence);
`_payload` == image end (0x90 of 144 B); packer code0 mask check holds (0x10000481); 9810 shim tree
not in the diff; nothing in pack-uefi-beyond0lte.ps1/build-bootimg-v2.py reads the old header flags.
Found: (1) header comment asserted the 9810 steps "fault" on the 9820 - only suspected (B22); (2) it
cited pack-uefi.ps1 :92/:102-107, the 9820 packer is pack-uefi-beyond0lte.ps1 :91/:101-103. Fixed in
the follow-up commit. (3) MILESTONES "header-v2 container" row still says todo though B20/B21 show it
cleared the DT check - separate follow-up.

## B24 - boot #4 loop shape (user observation, 2026-10-10) + phone state

User: no colour band ever flashed; loop = Samsung logo -> "Galaxy S10e" splash -> repeat. So sboot
reached its kernel jump each time, and the OLD shim never produced a visible RED band. Old shim order
(git show d1c6564:.../bootshim.S :141-194): WTCON stores 0x10050000/0x10060000 -> mrs/msr SCTLR_EL1 ->
RED paint. => it died before/at the paint, OR the 0xCA000000 paint is not what the panel scans out.
Not distinguishable from this. User then put the phone in download mode, reflashed TWRP: now
`adb devices` -> `RF8M21HYDSZ recovery`, TWRP 3.7.0_9-2, root shell, by-name boot=sda14,
recovery=sda15. User consented to boot #5 ("you can proceed in the next attempt").

## B25 - FD base moved 0x83300000 -> 0x90000000 (prior art), rebuilt end to end

Four lockstep sites: Beyond0LtePkg.dsc PcdFdBaseAddress/PcdFvBaseAddress, .fdf FD_BASE_ADDRESS, shim
UEFI_BASE default, pack-uefi-beyond0lte.ps1 -UefiBase default (+ its stale `.dsc:415` pointer -> :439).
Source: edk2-exynos configs/exynos9820.conf FD_BASE=0x90000000 (B23). Range 0x90000000..0x90700000 vs
reads: above kernel window (ends 0x832D0000) and el2_code (ends 0x87200000), below mm_debug 0x91200000,
inside bank0 (ends 0xBAB00000), disjoint from the UFS DMA window 0xA7400000.. . No C code reads the base.
Unchanged and noted: PeilessSec puts the UEFI region at top of PcdSystemMemory = 0xC8000000..0xD0000000
(bank1 RAM; contains the live boot FB 0xCA000000) - not touched here.
Verified by running: build-uefi.ps1 exit 0 (FVMAIN_COMPACT 3136 B free), PeilessSec AutoGen
`_PCD_VALUE_PcdFdBaseAddress 0x0000000090000000ULL`; pack-uefi-beyond0lte.ps1 -> shim header OK
(code0 0x10000481), uefi-beyond0lte-kernel.bin 7 340 176 B = 144 + FD, quad @0x48 = 0x90000000, FD bytes
after the shim cmp-identical to BEYOND0LTE_UEFI.fd. (Cosmetic: the packer's "kernel ([shim][FD])" line
printed a stale 8628288 B - the file on disk is 7340176.)

## B26 - shim diagnostic for boot #5 (BEYOND0LTE_SHIM_DIAG, default ON) + the boot #5 image

Why: our FD draws nothing before BDS (Beyond0LteFbText has no callers; SimpleFramebufferDxe needs a DECON
present), and the panel does not show raw scanout stores without DECON being told - so boot #4's "nothing
visible" said nothing. Prior art (edk2-exynos Exynos9820Pkg PlatformPrePiLib/PlatformUtils.c
PlatformInitialize) does exactly `MmioWrite32(0x19030000, 0x1281)` "Enable Frame Buffer Writes".
Shim (DIAG=1): str 0x1281 -> decon_f 0x19030000 (base VERIFIED Exynos9820.h, value PRIOR ART) -> WHITE
band rows 200-649 (entered) -> copy -> WHITE band rows 750-1199 (about to br into FD) -> br. Bands are
0xFFFFFFFF so the unread pixel format does not matter; leaf `bl _band` uses x9-x11/x30 only.
Verified: DIAG=1 208 B / DIAG=0 144 B, _payload = last byte in both; DIAG=0 is cmp-identical to the B25
shim; objdump reviewed; packer shim checks OK (code0 0x10000681).
Boot #5 image: %TEMP%\b25\boot5-v2.img 7 587 840 B sha256 1379b50aa33061788a088c9ce912c38354e00cb716887d0ac80a5e37110b3765
= build-bootimg-v2.py --kernel kernel-b5.bin (7 340 240 = 208 + FD) --dtb opencode/stock/dt.dtb. Header vs
boot #4's pack-out-v2.img (which cleared the DT check): only kernel_size and the id hash differ; DTB section
byte-identical, same addrs/page/v2.
Reading boot #5: no bands = shim never entered (or 0x1281 does not unblank) ; band 1 only = fault in copy ;
both bands then loop = FD SEC/PEI/DXE faults ; bands then UEFI/Windows output = progress.

## B27 - boot #5 WRITE (FD @0x90000000 + diagnostic shim) to BOOT sda14, consented

User added adb allow rules (/permissions) and asked to retry. In TWRP: pushed boot5-v2.img (sha256 1379b50a...0b3765
on-device), by-name/boot -> /dev/block/sda14, `dd ... conv=fsync` 7 587 840 B, read back 3705 x 2048 B ->
sha256 1379b50aa33061788a088c9ce912c38354e00cb716887d0ac80a5e37110b3765 (byte-exact). `adb reboot` next.
Observation pending (decode per B26).

## B28 - boot #5 OBSERVED: BOTH white bands drawn over the "Galaxy S10e" splash, then boot loop

User: "boot looping. the galaxy s10e logo is now kinda broken with 2 parts of white covering most of it".
Decode (B26): band 1 (rows 200-649) AND band 2 (rows 750-1199) drawn over sboot's splash ->
- FACT sboot enters our shim (first on-device proof); DECON/FB stores at 0x19030000/0xCA000000 are seen.
- FACT the 0x700000 copy to 0x90000000 completed and the shim reached `br x5` into the FD.
- => the loop is now INSIDE the FD (PeilessSec SEC -> PEI -> DxeCore -> ...). Shim + header-v2 container
  are device-validated as a path; FD stages are not.
(Not separable from this: whether the 0x1281 write was needed, or sboot's splash buffer is live anyway.)
User put the phone back in download mode. sda14 holds boot5-v2.img.
