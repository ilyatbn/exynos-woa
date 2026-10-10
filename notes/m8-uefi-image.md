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

Still NOT closed by this: the image has never been flashed or booted (ground
rules 5/6), and K4b (where sboot drops the kernel) is what decides whether the
shim's own copy step survives - see the hazard comment in bootshim.S.
