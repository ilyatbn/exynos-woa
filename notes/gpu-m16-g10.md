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
