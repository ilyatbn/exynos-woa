# M16 G10 + G11 - drivers/MaliG76Wddm (full WDDM 1.3 KMD, display + GPU half)

Task (2026-10-12): retarget wddm-mali-g52 `drivers/maliwddm` + `drivers/common/maligpu` (local clone
`C:\Users\ilya\code\mali\wddm-mali-g52` @16f7e7c, GPL-2.0) to the S10e Mali-G76 as `drivers/MaliG76Wddm/`.
No device access in this session: code + build + package only. Citations `W:path:line` = that clone.

## Progress log
- 2026-10-12 - worktree branch fast-forwarded to main d9b8572 (was 436 commits behind; brings MaliG76Probe,
  gpu-m16-g9b.md, G9 DSDT GPU0).
- 2026-10-12 - read notes/gpu-wddm-mali-g52.md (R1-R19), notes/gpu-m16-g9b.md (GPU0 contract), MILESTONES G9-G12.
- 2026-10-12 - read W:drivers/maliwddm/{maliwddm.h,maliwddm.inf,maliwddm.vcxproj,adapter.c}. adapter.c facts:
  MapGpu = first CmResourceTypeMemory of the TRANSLATED list (adapter.c:106-126); StartDevice: display first,
  GPU optional (adapter.c:128-160) - GpuUp FALSE leaves the adapter displaying; ISR returns FALSE when !GpuUp
  (adapter.c:229-237); caps HighestAcceptableAddress 2^40-1 (adapter.c:260); registry trace under the device key
  subkey `maliwddm` (adapter.c:10-33). Link: displib.lib only (vcxproj:330). INF: Class Display, demand start,
  ErrorControl 0, LoadOrderGroup Video, EnablePreemption 0 HKLM (inf:49-61).
- 2026-10-12 - read W:maliwddm/{display.c,command.c,memory.c}, W:include/maliwddm_abi.h, W:common/maligpu.{h,c},
  W:tools/windows/m5test.c, W:docs/results/m5.1-board-2026-10-02.md, m5.2-board-run6-2026-10-02.md.
  **Key finding (shapes the GpuEnable=0 design):** the reference FULL adapter on installed Windows with WARP
  (d3d10warp.dll) as UMD started but the local screen + RDP went BLACK - DWM's device died after an escape
  c000000d, Render/Present/SubmitCommand never called (run6:1-26); a full adapter with NO loadable UMD fails with
  code 43 (maliwddm.inf:7-9, m5-pe-test.cmd:46-49). Only the DISPLAY-ONLY KMDOD `malidod` (DxgkInitializeDisplay
  OnlyDriver, WIN8 interface, no UMD) drove a desktop on installed Windows 11 23H2 (m5.1-board:3-5, malidod.c:842).
  => "display-only validation" through the full maliwddm adapter would reproduce run 6 (black). Decision: one .sys,
  DriverEntry reads Services\<svc>\Parameters: GpuEnable=0 and FullAdapter=0 (default) -> KMDOD (malidod's DDI set:
  PresentDisplayOnly CPU copy into the GOP fb, no UMD, no GPU job; DWM renders with Microsoft Basic Render = WARP,
  exactly what BasicDisplay does today); FullAdapter=1 -> maliwddm full WDDM 1.3 adapter, GPU half only with
  GpuEnable=1 (GpuEnable=1 implies the full adapter). Full mode = expected black desktop until the Mesa UMD (G12).
- maliwddm facts: one aperture segment 1 GiB, nominal base MK_VA_START, CacheCoherent FALSE, paging buffer 10 pages
  (memory.c:14-40); VA bitmap 32 MB..4 GB (maligpu.h:119-122) - must stay >= segment size; GPU VA reserved at
  CreateAllocation only if GpuUp (memory.c:63-70); Kick completes every submission at once when !GpuUp
  (command.c:289-294) - so the full adapter without GPU is a legal "no-GPU" mode; m5test uses Slot 1 (JS1)
  (m5test.c:225) and MW_ESCAPE_GPU_INFO Count != 0 to find the adapter (m5test.c:113-121); GetNodeMetadata names
  "Mali-G52 job manager" (command.c:553).
- maligpu facts to retarget: PmuSaysGpuIsOn maps RK PMU_PA itself (maligpu.c:222-236) -> ours from _CRS (2nd
  memory); GPU_ID gate `>>16 != 0x7402` (maligpu.c:152 in the .c listing line 349); quirks TILER/JM only
  (maligpu.c:292-295); PWRON *_LO only (297-304); page tables below 40 bits (AllocTable high 0xFFFFFFFFFF);
  JOB_INT_MASK 0x70007; SoftReset leaves GPU_INT_MASK = FAULT|MULTIPLE_FAULT (maligpu.c:283-284).
- malidod: no DxgkDdiInterruptRoutine in its init table (malidod.c:842-870), WDDM1.2 caps with SupportNonVGA +
  SupportSmoothRotation, target frequencies UNSPECIFIED (m5.1 run 4 lesson) vs maliwddm synthesising 60 Hz (full
  driver needs them, display.c:28-33). Both lessons kept per mode.
- S10e: GOP = 1080x2280, PixelBlueGreenRedReserved8BitPerColor, PixelsPerScanLine 1080 (FB_TEXT_SCALE 1), base
  0xCA000000, size 9849600 (SimpleFramebufferDxe.c:67,420-430; Exynos9820.h:905-911 VERIFIED base/geometry,
  "format still unread" for the panel itself). DRAM tops out at bank4 0x900000000+0x80000000 = 0x980000000 < 2^36
  (Exynos9820.h:110-121) -> the 40-bit page-table/HighestAcceptableAddress limit holds whatever the G76 PA bits
  (MMU_FEATURES[15:8], panfrost_regs.h:19-20) say, as long as they are >= 36; the driver now clamps to them.

## Task A - rescue path first (R19), firmware side
Reference rescue: `m5-wddm-rescue.cmd` / `m5-dod-rescue.cmd` load the offline SYSTEM hive from WinPE and set
Services\maliwddm Start=4 (W:tools/windows/m5-wddm-rescue.cmd:20). We have no WinPE and the HANDOFF rule is "no NTFS
writes from TWRP" (HANDOFF §30: fsck.ntfs damage after TWRP-side DriverStore swaps). So the rescue must not touch
NTFS. What disables the KMD completely: the INF binds **only** `ACPI\SAMS9828` (maliwddm.inf binds only
`ACPI\RKCP7402` the same way, W:maliwddm.inf:38; ours below); with GPU0._STA = 0 ACPI enumerates no device node,
so the service is never started, and Basic Display (root-enumerated, uses the boot framebuffer) keeps the
screen - the state every boot so far had. Two firmware triggers:
1. **Image flag**: `BEYOND0LTE_GPU_POWER_ON 0` now ALWAYS hides GPU0 (Beyond0LteGpuProbeDxe.c entry: `Usable &&
   !BEYOND0LTE_GPU_POWER_ON` -> Usable FALSE + log "G76 usable as found, but BEYOND0LTE_GPU_POWER_ON 0 = rescue
   image - GPU0 hidden"). Before this, flag 0 + sboot leaving G3D on would have shown GPU0 (gpu-m16-g9b.md "Cases").
   => any flag-0 image (e.g. boot107-loop-v2.img lineage) is a guaranteed rescue image.
2. **ESP flag, no reflash**: `\S10EDIAG\nogpu.flg` on the ESP (vfat by-name/cache, mounted rw from TWRP as
   firmware/tools/s10e-install-diag.sh:44 does) -> Beyond0LteBootLogDxe OnReadyToBoot (after its ConnectController
   pass, before the WINDOWS_PAIR block) writes word [4] = 'NOGP' 0x50474F4E (any value != 'GPU0' makes _STA 0,
   Dsdt.asl GPU0._STA) + dsb + log "BootLog: \S10EDIAG\nogpu.flg present - DSDT GPU0 hidden (word [4] = NOGP)".
   ReadyToBoot precedes ExitBootServices, and _STA is evaluated by Windows' ACPI driver from the RAM word, so the
   last writer before EBS wins; GpuProbeDxe runs at DXE dispatch, earlier. From Windows, `s10ediag peek 0x8FE0F010`
   tells the three states apart: 30555047 visible, 50474F4E hidden by flag file, 0 hidden by probe/flag-0 image.
   TWRP: `mount -t vfat /dev/block/by-name/cache /tmp/esp && touch /tmp/esp/S10EDIAG/nogpu.flg && sync && umount /tmp/esp`
   (undo: rm the file). Works only if a FAT volume holding \EFI\Microsoft\Boot\BCD is found (OpenEspRoot), which
   every Windows boot needs anyway.
3. Driver-side kill switch from an ESP file: not possible (the ESP is not mounted when a boot-start/demand display
   driver starts; NTFS-only registry) -> (1)+(2) only, as the task expected. The KMD's own default (GpuEnable=0)
   adds a third layer: no GPU register write beyond masking interrupts unless the registry opts in (Task B).
- Build (scratchpad\build-g10.ps1 = build-g9b.ps1 with $ws -> this worktree, WORKSPACE `subst W:` of
  scratchpad\g10build, Conf copied from g9bbuild): flag 0 (as committed) **exit 0**, FVMAIN_COMPACT 774144 total,
  772328 used, **1816 B free** (was 1952 after G9: +136 B), FD 7 340 032 B sha256 `BDFF55BD376BE19D...`
  (scratchpad\BEYOND0LTE_UEFI-g10-flag0.fd); BootLogDxe.efi 17 920 B contains "nogpu.flg present", GpuProbeDxe.efi
  14 336 B contains "rescue image". Flag 1 (temporary source edit, restored byte-identical): **exit 0**,
  FVMAIN_COMPACT auto-grown to 778240, 3744 free, FD sha256 `CC9CCE9E633465F8...`. Not packed, not flashed.

## Review of 61e3d3c (rescue path, adversarial)
- 9810 trees untouched (`git diff --stat d9b8572..HEAD` on Star2LtePkg, Exynos9810Pkg, drivers/Exynos9810*: empty).
- Every writer of word [4] (rg over Beyond0LtePkg): SEC clears (ArmPlatformLib.c:102), GpuProbeDxe clears at entry
  and sets GPU0/0 at its end (DXE dispatch), BootLogDxe writes NOGP at ReadyToBoot (BDS, later). Nothing writes it
  after ReadyToBoot -> NOGP wins whenever the file exists. The page lies in the W32 2 MiB hand-off window at
  0x8FE00000 that the DSDT already reads for XHC0/WLAN (PlatformMemoryMapLib.c:367).
- Flag-1 image + nogpu.flg: UEFI still powers G3D on and leaves it at 260 MHz with no OS user. Accepted: an idle
  powered GPU, same as the G9 state before any driver is installed; recorded as a known cost of the ESP rescue.
- Flag-0 image: GpuProbe still runs read-only (logs), only the hand-off is suppressed - log line names why.
- Build evidence: both module strings present in the built .efi files (above). No defect found; nothing to fix.

## Task B - drivers/MaliG76Wddm design (2026-10-12)
- One .sys, mode chosen in DriverEntry from `HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters`:
  - default (GpuEnable 0, FullAdapter 0): **KMDOD** = malidod's DDI set (DxgkInitializeDisplayOnlyDriver, WIN8
    interface, DRIVERCAPS WDDMv1_2 + SupportNonVGA + SupportSmoothRotation, target frequencies unspecified,
    PresentDisplayOnly = CPU copy of dirty/move rects into the GOP fb). **No ISR, no GPU register access at all**
    (malidod.c:870-872: no VSync -> ControlInterrupt/GetScanLine and with them ISR/DPC must stay NULL). Exactly the
    configuration that gave a desktop on installed Windows (m5.1-board). = G10 validation step 1.
  - FullAdapter 1, GpuEnable 0: maliwddm full WDDM 1.3 adapter, GPU half off (Kick completes every submission at
    once, command.c:289-294). If the GPU is powered and answers as a G76, only its three INT_MASKs are written 0 (so a
    level line left asserted by firmware cannot storm the connected ISR); nothing else. Needs a UMD name (code 43
    otherwise) -> the ESP script writes `UserModeDriverName = d3d10warp.dll` (their steps a/b, maliwddm.inf:10-11)
    into the adapter's class key. Expected: adapter Started, desktop BLACK (run 6). = G10 step 2 (DDI acceptance).
  - GpuEnable 1: full adapter + GPU half (retargeted maligpu) = G11; m5test through D3DKMT.
- Display (both modes): child `D3DKMDT_VOT_INTERNAL` (R14), always connected, no EDID; the fb from
  DxgkCbAcquirePostDisplayOwnership is used as reported (it is what Basic Display uses) and compared with the
  Exynos9820.h GOP values (0xCA000000, 1080x2280, pitch 4320) -> registry `FbMatchesGop` 1/0, mismatches logged not
  fatal; only A8R8G8B8/X8R8G8B8 accepted (display.c:11-14). Rotation: identity only, as both references -
  **regression vs Basic Display, which rotates today** (notes/d8-buttons-agent.md:47, m15-perf.md:112: Bixby
  rotate toggle works on BasicDisplay) -> follow-up, recorded in MILESTONES G10.
- Resource contract (as MaliG76Probe): translated list = memory 0x18500000/0x5000, memory 0x15862000/0x1000,
  3 interrupts; otherwise the GPU half stays off (the display still starts).
- GPU half retarget (maligpu.c/.h copied into the subtree): R4 PMU page from _CRS, both bits 1, re-checked in the ISR
  before any GPU read; R5 ((id>>16)&0xFF0F)==0x7201; R6 SHADER_CONFIG = SC_TLS_HASH_ENABLE + TILER/JM as before;
  R7 L2_PRESENT exactly 1 bit and no cores above bit 31 (else GPU half off, logged); R11 page tables and
  HighestAcceptableAddress limited to min(40, MMU_FEATURES PA bits); polls 20 ms for L2/shader/tiler (panfrost);
  JOB_INT_MASK limited to the JS_PRESENT slots 0-2. Single job chain at a time, 1 ms poll fallback, TDR =
  re-bringup - unchanged.
- Aperture segment: 1 GiB kept. It is a GPU-VA window that VidMm commits system pages into on demand (memory.c:1-9),
  not a carve-out; Windows sees 5.24 GiB on the S10e (HANDOFF §25), DRAM PAs end at 0x980000000 (< 2^36). The VA
  allocator spans 32 MB..4 GB (maligpu.h MK_VA_*), so the segment must stay <= 4 GB - 32 MB; 1 GiB leaves VidMm
  CommitLimit at ~19 % of RAM. Raising it is a G12/G13 tuning question, not a hardware constant.
- Names: service/sys `MaliG76Wddm`, trace subkey `Device Parameters\MaliG76Wddm`, node name "Mali-G76 job manager";
  ABI header kept byte-identical (`maliwddm_abi.h`, MW_ABI_VERSION 1) so m5test and the Mesa shim stay compatible.
- 2026-10-12 - written so far: LICENSE (= upstream LICENSE text, LF; sha256 edaef632... same as MaliG76Probe's, the
  clone's CRLF working copy differs only in line ends), include/maliwddm_abi.h (upstream content, LF), maligpu.{h,c}
  (G76 retarget), MaliG76Wddm.h, display.c (both modes), command.c/memory.c (ported by scratchpad\port_cm.py: include
  + node name + CollectDbgInfo prefix; plus SyncReset re-bringup guarded by Gpu.Ready && mode FullGpu - in the
  original it ran whenever Regs was mapped, which in our FullNoGpu mode would have powered the GPU up on a TDR).
- 2026-10-12 - first build `build-package.ps1 -Out scratchpad\mali-g10-try1`: **exit 0** first try (VS18 MSBuild,
  WDK 10.0.26100, WDM + displib.lib, /W4 /WX), Inf2Cat ok, signed. InfVerif /w /v: "INF is VALID", exit 0. Imports:
  ntoskrnl (Mm*/Zw*, incl. ZwLoadDriver/ZwUnloadDriver from displib's DxgkInitialize* stubs) + HAL. Package not
  final yet (README/NOTICE/ESP scripts next).

## Review of 91cfc76 (MaliG76Wddm, adversarial)
Checked mechanically:
- `diff` of every ported file against the clone (CR stripped), code lines only: maligpu.c changes are exactly
  PmuSaysGpuIsOn -> MkPowered (PMU page, 1 = on), MkGpuProbe (GPU_ID gate + PA bits), MkGpuQuiet, SoftReset masks
  JOB/MMU too, SHADER_CONFIG quirk, the layout check, JobIntMask, power checks in Bringup/Stop/FlushRange/ISR, PA
  limit in AllocTable/MapPages; display.c = maliwddm display.c + malidod's DOD pieces + VOT_INTERNAL + FbMatchesGop;
  command.c/memory.c = include, node name, CollectDbgInfo prefix, SyncReset guard. ABI header byte-identical to the
  upstream blob (LF). 9810 trees untouched (diff --stat empty).
- panfrost facts re-read (local linux clone): hw_features_g76 has COHERENCY_REG + TLS_HASHING
  (panfrost_features.h:84-96), hw_issues_g76 = 0 (panfrost_issues.h:259), init_quirks writes SHADER_CONFIG only when
  quirks != 0 (panfrost_gpu.c:141-145), GPU_SHADER_CONFIG 0xF04 / SC_TLS_HASH_ENABLE BIT(17) (panfrost_regs.h:180,211).
Found (fixed in the follow-up commit, not in 91cfc76):
1. **False citation + changed timeout**: maligpu.c said "20 ms for every domain, as panfrost ... (the original gave the
   tiler 1 ms)". panfrost_gpu_power_on polls the tiler with 1000 us (panfrost_gpu.c:476-478) - the original matched
   panfrost. Tiler poll back to 1 ms, comment corrected. (MaliG76Probe's notes make the same "20 ms per poll" claim for
   the tiler - G9b, recorded here, not changed: a longer upper bound is harmless there.)
2. Three wrong line citations into malidod.c in display.c comments: FillSignal 126-140 -> 129-141, unspecified target
   frequencies 487-495 -> 490-497, caps 331-366 -> 331-368, present 667-722 -> 667-720.
3. maligpu.h claimed mainline panfrost uses Mali-LPAE "on this GPU": the Exynos 9820 is not a mainline panfrost SoC;
   reworded to the R10 fact (every SoC but MT8188/8192/8370) and that G9b/G11 prove it on the G76.
4. Notes Task B said the FullNoGpu mode writes "only its three INT_MASKs ... nothing else": it also READS GPU_ID and
   MMU_FEATURES (MkGpuProbe) before masking. Corrected here (the notes line above is left as written).
Accepted / open (recorded, not defects of the commit):
- adapter.c DriverCaps comment and README/INF name an ESP script (`esp\extra-gpu-g10.cmd`) that sets
  EnablePreemption 0 and the UMD name - it is written in the next task (must match that name).
- KMDOD with interrupt resources in _CRS but no ISR: malidod's DSP0 test device is not shown to have had interrupts,
  so whether dxgkrnl accepts a display-only adapter whose device lists 3 interrupts is UNVERIFIED (first boot answers
  it: Started=1 or code 43 / Fail_* in the trace).
- VOT_INTERNAL instead of the validated HDMI: UNVERIFIED for dxgkrnl acceptance (R14 says cosmetic, not proven).
- Rebuild after the fixes: exit 0, MaliG76Wddm.sys sha256 4101B0A8... (scratchpad\mali-g10-try2; not the final package).

## Task C - UMD name, device name, m5test, ESP procedure, package (2026-10-12)
- Coordinator request (G12 Mesa build done: s10e-artifacts\mali-g12\maliumd.dll): keep `UserModeDriverName =
  maliumd.dll` x4 in the INF (as W:maliwddm.inf MW_Settings) -> done (MW_Settings AddReg). This replaces my earlier
  "no UMD name in the INF" choice; risk recorded: whether dxgkrnl ignores UserModeDriverName for a KMDOD (the
  default display-only mode) is UNVERIFIED - malidod.inf had none. The full-adapter modes now get the real Mesa UMD
  instead of WARP, so the full adapter is not necessarily black (run 6 was WARP); DWM on it is G13.
- **Device / symbolic-link name: MaliG76Wddm creates none** (no IoCreateDevice/IoCreateSymbolicLink; maliwddm had
  none either - the only `WdfDeviceCreateSymbolicLink` in the clone is drivers/malikm/device.c:253). `\\.\MaliG52`
  (`MALIKM_USER_PATH`, W:include/malikm_ioctl.h:37-39) belongs to **malikm** (the KMDF test driver, backend A), which
  is not ported. The shim tries it first and, when CreateFileW fails, falls back to the WDDM adapter through
  D3DKMT (W:win32-compat/malikm_drm.c:66-83 `malikm_open` -> `wddm_init`), choosing the adapter whose
  MW_ESCAPE_GPU_INFO answer has Count != 0 && Valid != 0 (malikm_drm.c:693-696; inside the runtime UMD
  malikm_drm.c:989-990). MaliG76Wddm answers that escape exactly as maliwddm (Count 45, Valid = 0 unless the GPU
  half is up) -> **no shim change is needed for the device name**; a rename would only change log strings. The
  only G52-specific shim content is the MALIKM_FAKE table (PROD_ID 0x7402, malikm_drm.c:1271), CI-only.
- m5test: `drivers/MaliG76Wddm/tools/m5test/m5test.c` (from W:tools/windows/m5test.c, user-mode D3DKMT test, GPL-2.0):
  the adapter must answer GPU_INFO with Valid != 0 and (Value[0] & 0xFF0F) == 0x7201; still WRITE_VALUE on JS1, magic
  0xC0FFEE52. build-m5test.cmd (vcvarsall x64_arm64, /W4 /WX, signed): M5TEST_BUILD_OK (plus a harmless "vswhere not
  recognized" line from vcvarsall). Exit: 0 PASS, 1 no adapter, 2 step failed, 3 thunks missing.
- ESP script `drivers/MaliG76Wddm/esp/extra-gpu-g10.cmd`: records the GPU0 word, pnp state, Parameters, the registry
  trace (Enum\ACPI\SAMS9828\0\Device Parameters\MaliG76Wddm), Win32_VideoController; copies maliumd.dll to System32 if
  on the ESP; installs the package once (pnputil, test cert); runs m5test when THIS boot is GpuEnable 1; then writes
  the next boot's mode from \S10EDIAG\gpu\{full,gpu}.flg (EnablePreemption 0 only for the full modes, deleted
  otherwise). %ERRORLEVEL% inside the m5test block deferred with `call` (block-parse expansion).
- **Package** `C:\Users\ilya\code\s10e-artifacts\mali-g10` (build-package.ps1 -Build 2: exit 0; InfVerif /w exit 0):
  - MaliG76Wddm\MaliG76Wddm.sys 54 168 B `4E1487E53A6712DF522DD30CF6C06EBEC0A4016C4F1BEC44D7D51B51350B08EA`
  - MaliG76Wddm\MaliG76Wddm.inf `C0F1292EDA9EEDE3FE4E639CAF8C8A602BEC5A5E0EE84E3B71A75841B715F0AE`
  - MaliG76Wddm\malig76wddm.cat `34FBB2A4DD1ECB24A96C0C2955DDEC65FEC76A627E42C11AE93E1474D0159576`
  - m5test.exe 154 008 B `7B8C003F38A18556BD07DEF8F3ACBC72508667A95672E81796875C7B3AB60B42`
  - extra-gpu-g10.cmd `2B05937748118804347BCB858AEA6AAD51FF2F5FEDBB373F0FDAFAAAB873D9ED`
  Signed with the WDK test cert 5DD5DEE1... (as every package). Firmware: no boot image packed here - use a
  BEYOND0LTE_GPU_POWER_ON 1 build of this branch (scratchpad\BEYOND0LTE_UEFI-g10-flag1.fd was built, not packed).

## Validation procedure on the phone (needs-boot; nothing here was run)
Prerequisites: G9 validated (UEFILOG "ACPI GPU0 visible", `s10ediag peek 0x8FE0F010` = 30555047, Device Manager
ACPI\SAMS9828 with 2 memory + 3 IRQ) - ideally G9b PASS (MaliG76Probe) first, then **remove MaliG76Probe**
(pnputil /delete-driver oemN.inf /uninstall): both bind ACPI\SAMS9828, only one may be installed.
Rescue at every step: from TWRP `mount -t vfat /dev/block/by-name/cache /tmp/esp; touch /tmp/esp/S10EDIAG/nogpu.flg;
sync; umount /tmp/esp` -> next boot GPU0 hidden, Basic Display back (or flash a GPU_POWER_ON 0 image, e.g. #107).
1. **Display-only (G10 step 1).** ESP: s10e-artifacts\mali-g10\MaliG76Wddm\ -> \S10EDIAG\gpu\MaliG76Wddm\;
   m5test.exe and mali-g12\maliumd.dll -> \S10EDIAG\gpu\; extra-gpu-g10.cmd as \S10EDIAG\extra.cmd (or call it); no
   flag files; flag-1 image; stay.flg for the first try (user watches). Expected: out\last\gpu\pnputil-install.txt ok;
   the screen flickers once (Basic Display -> MaliG76Wddm) and the desktop stays; trace.reg: Mode 0,
   AcquirePostDisplay 0, FbWidth 0x438 FbHeight 0x8e8 FbPitch 0x10e0, FbPhysLo 0xca000000, **FbMatchesGop 1**,
   Started 1, CommitStatus 0, Presents >= 1, no Fail_*; video.txt names "Arm Mali-G76 MP12 (Galaxy S10e, G10/G11)" at
   1080x2280. Reboot 3x, desktop each time = G10 display PASS. Rotation (Bixby toggle) is refused (identity only) -
   expected, not a failure.
2. **Full adapter, GPU off (G10 step 2).** touch \S10EDIAG\gpu\full.flg; boot once (the script writes FullAdapter 1),
   boot again. Expected trace: Mode 1, ResContract 1, GpuProbe 0, GpuQuieted 1, Started 1, QAI_* 0, Seen_CreateDevice
   / Seen_CreateContext; the screen either composes through maliumd.dll or goes black (their run 6, with WARP) -
   record which; the agent still collects and returns to TWRP. 3 boots without bugcheck.
3. **GPU half (G11).** rm full.flg, touch gpu.flg; boot once (writes GpuEnable 1), boot again: trace Mode 2,
   PmuEmbeddedG3dStatus/PmuG3dStatus bit0 1, GpuId 72?1????, PaBits, Step 6 (READY), L2Present one bit, GpuUp 1;
   gpu\m5test.txt: "PASS: the Mali ran a job submitted through WDDM", exit 0, stats `jobs done 1 failed 0 timedout 0
   fault 0x0`, **irqs > 0** (else only the 1 ms poll completed it - R2 trigger question stays open). A deliberately
   bad job (TDR recovery) is a later step.
4. Back to display-only: rm gpu.flg; boot once (writes 0/0 and deletes EnablePreemption).
