# MaliG76Wddm full-adapter hang (FullAdapter 1 / GpuEnable 1 -> black screen, Windows dead)

Task (2026-10-13, sub-agent, NO phone access): make the hang observable (PRAM mark trace of every DDI), find the
hang statically, fix the top suspects behind Parameters switches, build + test-sign to s10e-artifacts\mali-g13.

Device facts this starts from (HANDOFF §32 update, coordinator):
- Mode 0 (KMDOD) works on 3 boots: Started 1, FbMatchesGop 1 (0xCA000000 1080x2280 pitch 0x10E0 fmt 0x16),
  CommitStatus 0, Presents 1, QueryInterface c00000bb, QueryDeviceDescriptor c01d0001.
- FullAdapter 1 GpuEnable 0: black screen, agent never runs, no return to TWRP in 15 min, the firmware's 82 s
  Windows watchdog did NOT fire, force reset needed. Same with GpuEnable 1. No registry trace survived.

## Progress log
- 2026-10-13 - read drivers/MaliG76Wddm/{MaliG76Wddm.h,adapter.c,command.c,display.c,memory.c,MaliG76Wddm.inf},
  maligpu.c MkGpuProbe/MkGpuQuiet/MkIsr; DSDT GPU0 (_CRS: 2 memory + 3 Level/ActiveHigh/Exclusive SPIs 320/319/321).
  First observations (to be ranked below):
  - MwControlInterrupt (command.c:589-596) returns STATUS_SUCCESS for every type incl. DXGK_CRTC_VSYNC, and
    nothing ever calls DxgkCbNotifyInterrupt(DXGK_INTERRUPT_CRTC_VSYNC) - there is no DECON interrupt.
  - MwSetVidPnSourceAddress (display.c:576-582) returns SUCCESS and does nothing; no flip-completion VSYNC either.
  - MwGetScanLine (command.c:598-604) always InVerticalBlank FALSE, ScanLine 0.
  - MwInterruptRoutine (adapter.c:273-281) returns FALSE whenever !GpuUp (always in mode 1) - a level line that
    stays asserted would storm at DIRQL forever (no BSOD, no watchdog pet) -> must be ruled in/out.
  - MwLog (adapter.c:17-39) does ZwFlushKey per value: every QAI/Seen/Enter value is a synchronous hive flush.
- 2026-10-13 - **WINPRAM layout inventory** (rg of 8FE0xxxx / WINPRAM_BASE over drivers/, firmware/Platform, Silicon,
  bootshim; Build/ excluded). Base 0x8FE00000 (Beyond0LteBootLog.h:36 BEYOND0LTE_WINPRAM_BASE, a firmware choice in
  reserved bank0 RAM), the firmware saves the first 0x10000 B (BEYOND0LTE_WINPRAM_DUMP, :38) as \WINPRAM.BIN:
  - +0x0000..0x3FFF Exynos9820Ufs ring (UFS_PRAM_PHYSICAL_BASE, Exynos9820Ufs.h:106-125, window 0x4000) incl. the
    firmware's records +0x3F54 (DMA pool, Beyond0LteBootLog.h:52) and +0x3FB0 (handoff marker, Exynos9820Ufs.h:354)
  - +0x8000..0x8FFF Exynos9820Ufs early marks (UFS_EARLY_MARK_OFFSET, Exynos9820Ufs.c:1486, one page)
  - +0xA000..0xDFFF S10eDiag mark ring (S10eDiagIoctl.h:30-35: "S10EMARK" + ULONG next at +8, 255 x 64 B lines
    "<ms 10 digits> <text>"; writer S10eDiag.c:68-104 S10eMarkRaw under a driver-local spin lock)
  - +0xF000.. progress page: words [0],[1] (bootshim/SEC/BootLogDxe), [4] GPU0 'GPU0'/'NOGP', [5] AUD0, +0xF008/0xF00C
    (DSDT hand-off words)
  - **free: +0x4000..0x7FFF, +0x9000..0x9FFF, +0xE000..0xEFFF** - nothing in the tree reads or writes them.
  Decision: MaliG76Wddm gets its own slice **+0x4000, 16 KiB** (0x8FE04000) rather than sharing S10eDiag's ring:
  S10eDiag's lock is private to its driver, and an atomic on the ring's index word in PRAM would be an exclusive /
  LSE atomic on a non-cacheable (Device) mapping - not guaranteed on ARMv8 (may never succeed or abort); a private
  slice needs no cross-driver protocol. Same line format as S10eDiag so the same `tr`/strings decode works.
  Dump timing (Beyond0LteBootLogDxe.c:517-600): after a force reset (Vol Down + Power, PMIC; RAM survives) the next
  UEFI boot finds \WINRUN.FLG -> DumpMemoryToEsp \WINPRAM.BIN (64 KiB) -> zeroes it -> TWRP (no stay.flg) or
  continues (stay.flg). So the slice of the hang boot reaches the ESP on the boot right after the force reset.
- 2026-10-13 - **watchdog reading**: the "82 s" WDT is armed at ExitBootServices only when the image has
  BEYOND0LTE_WINDOWS_WATCHDOG 1 (Beyond0LteBootLogDxe.c:44-51, 873-905; 0 = stopped for the life of Windows), and the
  Exynos9820Ufs driver pets watchdog_cl0 from its 1 Hz Storport timer (Exynos9820Ufs.h:708-733). So "did not fire in
  15 min" means either the image had WINDOWS_WATCHDOG 0, or the UFS timer DPC kept petting = **DPCs kept running,
  the CPUs were not wedged at DIRQL/DISPATCH** (a soft hang: some thread waits forever). No bugcheck either (S10eDiag's
  bugcheck callback resets straight to TWRP, S10eDiag.c:140-165). An interrupt storm or a DISPATCH spin would end in
  0x133/0x101 or stop the pets -> unlikely given what was seen.
- 2026-10-13 - **agent reading**: S10eAgent waits for explorer.exe to run 30 s, or \S10EDIAG\wait.txt seconds,
  default **1800 s** (S10eAgent.c:6-10, DEFAULT_WAIT_S) before collect.cmd. A DWM that never composes (no explorer /
  stuck logon) therefore looks exactly like "the agent never runs" for 15 min while Windows itself is alive. Check
  on the next run: ESP wait.txt value, out\agent.log / the S10eDiag ring "waiting: LogonUI=.. explorer=.." lines
  (S10eAgent.c:296-313) in WINPRAM.BIN of the hang boot.

## T1 - PRAM DDI trace (drivers/MaliG76Wddm/pram.c)
- Slice 0x8FE04000 (WINPRAM + 0x4000), 16 KiB = 256 x 64 B lines, S10eDiag line format "<ms 10 digits> <text>":
  line 0 header "MALIG76T" + ULONG next rolling line (+8), next first-call line (+12), sequence (+16); line 1 the
  1 Hz heartbeat "HB i<isr> d<dpc> v<vsync> s<submits> p<presents> f<fence>" (rewritten in place); lines 2-111 the
  first entry + exit of every DDI in call order (never overwritten); lines 112-255 rolling: every DDI entry
  ("<seq> >Name") / exit ("<seq> <Name status"), every registry trace value ("<seq> R Name value"). Seq = 6 hex.
- Writer: InterlockedIncrement on the driver's own counters picks the line, then byte stores into the
  MmMapIoSpaceEx(PAGE_NOCACHE) mapping; no lock, no allocation, no ntstrsafe -> any IRQL (ISR, SubmitCommand at
  DISPATCH, SetVidPnSourceAddress up to DIRQL). A later load in the same boot continues after a "LOAD ... cont" line.
- Covered: every T_ wrapper (all DDIs of both init tables) + AddDevice, StartDevice (entry/exit), RemoveDevice,
  ResetDevice, StopDeviceAndReleasePostDisplayOwnership, SystemDisplayEnable, RenderKm, GetNodeMetadata,
  QueryDependentEngineGroup; DriverEntry's DxgkInitialize status; ISR/DPC as counters in the heartbeat only.
  Parameters\PramTrace (DWORD, default 1; 0 = off). Entry line written BEFORE MwEnter's registry write + ZwFlushKey.
- Build 3 (scratchpad\mali-t1): exit 0 first try (/W4 /WX), sys sha256 C6D3D4CE...
- Reading it (PC side, after pulling WINPRAM.BIN from the ESP):
  `python -c "d=open('WINPRAM.BIN','rb').read()[0x4000:0x8000];print(d[:8],int.from_bytes(d[8:12],'little'),int.from_bytes(d[12:16],'little'));[print(d[i*64:i*64+63].decode('ascii','replace').rstrip()) for i in range(1,256)]"`
  (lines 112..255 are a ring: start reading at the line number in header +8.)

## Review of 7eee385 (PRAM trace, adversarial)
Checked: every DDI pointer in both init tables now goes through a T_ wrapper or an explicit PRAM line (rg
"init.DxgkDdi" adapter.c: only MwAddDevice/MwStartDevice/MwRemoveDevice/MwResetDevice/MwUnload/MwInterruptRoutine/
MwDpcRoutine/MwSystemDisplayWrite are direct - the first five trace themselves, ISR/DPC are heartbeat counters,
SystemDisplayWrite is per bugcheck-screen blit and stays silent); HbStop runs before the adapter is freed on both
StopDevice and RemoveDevice (a StopDeviceAndReleasePostDisplayOwnership + RemoveDevice sequence is covered by the
latter); KeFlushQueuedDpcs only at PASSIVE (Stop/Remove); a failed DriverEntry unmaps (no Unload follows).
Line arithmetic: Begin = 18 chars (10 ms + ' ' + 6 seq + ' '), 63 printable per line.
Found:
1. **Value cut off after long names** (fixed in the follow-up commit): "<StopDeviceAndReleasePostDisplayOwnership" =
   18+1+40 = 59, the status (9 more) did not fit; "R Order_GetStandardAllocationDriverData" the same. Str/WStr now
   keep room for the value and cut the name instead.
2. Header +8 can lag by a line when two writers race (each stores its own snapshot of the counter): the reading
   rule is "sort the rolling lines by the 6-hex sequence", not "start at header +8" (instruction corrected here).
3. Behaviour change accepted: StopDeviceAndReleasePostDisplayOwnership, GetNodeMetadata, QueryDependentEngineGroup and
   RenderKm now also write the registry trace on their first call / on failure (all PASSIVE DDIs per d3dkmddi.h).
Rebuild after fix 1: exit 0, sys sha256 94656DC0... (scratchpad\mali-t1b).

## T2 - static hunt: ranked suspects (FullAdapter 1 and GpuEnable 1 both black, no bugcheck, WDT quiet)
Sources: d3dkmddi.h (WDK 10.0.26100 shared/): DXGKDDI_SUBMITCOMMAND `_IRQL_requires_(DISPATCH_LEVEL)` (:4382-4390),
DXGKDDI_SETVIDPNSOURCEADDRESS PASSIVE..PROFILE_LEVEL-1 (:6385-6394), DXGKDDI_CONTROLINTERRUPT / GETSCANLINE /
PRESENT PASSIVE. MS Learn DxgkDdiControlInterrupt (2024-03): "A call to DxgkDdiControlInterrupt to enable the
specified interrupt type indicates that the operating system requires that the driver call DxgkCbNotifyInterrupt
... to report when the interrupt type is triggered ... During every vertical retrace period ... the interrupt should
be triggered and reported" and "The driver must return STATUS_NOT_IMPLEMENTED if an interrupt type other than
DXGK_INTERRUPT_CRTC_VSYNC is supplied." MS Learn DxgkCbNotifyInterrupt: callers run at DIRQL; must DxgkCbQueueDpc
after it; the page's own example calls it from a DxgkCbSynchronizeExecution routine (software engine).
Reference: the original's installed-Windows full adapter, run 6 (W:docs/results/m5.2-board-run6-2026-10-02.md):
"No bugcheck. The local screen went black, and an RDP session connected but was black too" - Windows ALIVE, the
same symptom as ours; ControlInterrupt was called (order 29), Render/Present/SubmitCommand/SetVidPnSourceAddress
never were.

1. **VSync promised, never delivered** (command.c:588-596): MwControlInterrupt answers STATUS_SUCCESS to
   DXGK_INTERRUPT_CRTC_VSYNC enable and nothing ever calls DxgkCbNotifyInterrupt(CRTC_VSYNC) - the S10e has no DECON
   interrupt in this driver. Everything in dxgkrnl that waits for a vertical blank (D3DKMTWaitForVerticalBlankEvent -
   DWM frame pacing, DXGI sync-interval presents, flip retirement) waits forever: a thread-level hang, DPCs alive
   (WDT petted), no bugcheck, black screen - matches every observation. Same in mode 1 and mode 2 (the GPU half is
   not involved) - matches "same with GpuEnable 1". Also wrong for other types (must be NOT_IMPLEMENTED).
   MwGetScanLine (command.c:598-604) always InVerticalBlank FALSE / ScanLine 0: a D3DKMTGetScanLine poller waiting
   for the blank spins forever. -> **Fix F1: software vsync** (60 Hz KTIMER -> DPC -> DxgkCbSynchronizeExecution ->
   DxgkCbNotifyInterrupt(CRTC_VSYNC) + DxgkCbQueueDpc, only while enabled; GetScanLine derived from its phase),
   Parameters\SoftVsync (default 1).
2. **Nothing the compositor draws can reach the panel**: MwPresent (command.c:132-171) handles Blt/ColorFill only -
   a Flip present (pAllocationList[SOURCE] = the new primary, no destination) becomes an empty op;
   MwSetVidPnSourceAddress (display.c:576-582) is a no-op; SetVidPnSourceVisibility(FALSE) zeroes the GOP fb
   (display.c:406-408). The firmware scan-out keeps showing 0xCA000000, which then stays black even if DWM runs.
   -> **Fix F2**: Flip present = CPU blt of the source allocation into the GOP fb at SubmitCommand (DISPATCH, the
   existing DoPresent path); SetVidPnSourceAddress records the address for the vsync report.
3. **Agent artefact** (see agent reading above): the 15 min "agent never runs" is what a stuck DWM looks like with
   the default 1800 s wait; not proof of a kernel hang. -> procedure: wait.txt 300 on the ESP for GPU runs.
4. **UMD**: DWM's device on maliumd.dll (Mesa d3d10umd, D3D10 DDI) - in mode 1 GPU_INFO Valid = 0; run 6 shows a
   UMD that gives up leaves a black, live system. Not fixable in the KMD; the PRAM trace shows whether
   CreateDevice/Render/Present ever come (G13 work).
5. **Preemption**: PreemptionAware 1 (adapter.c DriverCaps) with a no-op MwPreemptCommand (never reports
   DMA_PREEMPTED); extra-gpu-mode.cmd sets the mode but NOT EnablePreemption 0 (only extra-gpu-g10.cmd does, one boot
   later). Mode 1 completes every submission inside SubmitCommand, so nothing is ever preemptable; mode 2 would
   TDR -> 0x116/0x117 after repeats, not a silent hang. -> **Fix F3**: extra-gpu-mode.cmd writes EnablePreemption 0
   for the full modes too.
6. **Level-line storm** (adapter.c MwInterruptRoutine returns FALSE whenever !GpuUp): only if a line is asserted
   while unmasked (GpuProbe failed -> no MkGpuQuiet). Would stop the WDT pets or end in 0x133 -> contradicted.
   Low; the heartbeat's isr count shows it (thousands per second).
7. **ZwFlushKey per traced value inside DDIs** (adapter.c MwLog): synchronous hive writes under dxgkrnl's locks,
   slow, not a deadlock. Kept; PRAM now precedes it.
8. **DxgkCbSynchronizeExecution failing** (no interrupt connected for 3 line SPIs?) -> SubmitCommand returns
   STATUS_UNSUCCESSFUL -> VidSch bugcheck, not silent. Visible in PRAM as "<SubmitCommand c0000001".

## F1 - software vsync (command.c, Parameters\SoftVsync default 1)
- ControlInterrupt: non-CRTC_VSYNC -> STATUS_NOT_IMPLEMENTED (MS Learn); CRTC_VSYNC enable starts a periodic 16 ms
  KTIMER (whole-ms resolution of KeSetTimerEx), disable cancels it; PRAM line "Vsync on/off <SoftVsync>".
- Timer DPC -> DxgkCbSynchronizeExecution(SyncVsync): DxgkCbNotifyInterrupt(CRTC_VSYNC, target 0, PhysicalAddress =
  last SetVidPnSourceAddress PrimaryAddress) + DxgkCbQueueDpc; VsyncTicks -> heartbeat "v". If the very first
  SynchronizeExecution fails (no interrupt object) the timer stops and PRAM says "SoftVsync sync failed, stopped".
- GetScanLine: line = phase of the interrupt clock in a 1/60 s frame of Height+45 lines; InVerticalBlank in the
  last 45. SetVidPnSourceAddress keeps PrimaryAddress (a store, legal up to DIRQL).
- MwCommandStop (StopDevice, full modes) clears VsEnabled, cancels the timer before KeFlushQueuedDpcs.
- UNVERIFIED: the panel's real refresh (sboot's DECON timing) - 60 Hz is what the VidPN reports, not a register read.
- Build 5: exit 0, sys sha256 047BA8E8...

## Review of 77a56ec (F1, adversarial)
Checked: VsTimer/VsDpc initialised for every mode in AddDevice (MwCommandInit) but only armed by ControlInterrupt,
which only the full init table has (rg "DxgkDdiControlInterrupt" adapter.c: InitFull only); StopDevice -> MwCommandStop
clears VsEnabled + cancels before KeFlushQueuedDpcs, and ControlInterrupt after Stopping does nothing; the DPC's
failure path cancels its own periodic timer (KeCancelTimer is legal at DISPATCH). Build 5 exit 0.
Found: the comment claimed the timer "runs at the 60 Hz the VidPN reports" - 16 ms is 62.5 Hz nominal and the
expiries snap to the 15.6 ms clock tick. Comment corrected in the follow-up commit (no code change; rate kept: a
vsync slightly faster than the panel only shortens vblank waits, never blocks one).

## F2 - presents reach the panel (command.c, memory.c, Parameters\FlipBlt default 1)
- Found while writing it: **with the GPU half off (mode 1) no present could ever reach the panel** -
  BuildPagingBuffer MAP_APERTURE_SEGMENT returned before recording the allocation's MDL when !GpuUp (memory.c), and
  DoPresent's AllocationVa needs that MDL -> every Blt counted as PresentSkips. Now the MDL is recorded (and cleared
  on UNMAP) without any GPU mapping when the GPU half is off.
- Present with Flags.Flip (and FlipBlt 1): op = BLT of the whole source allocation (pAllocationList[1], Width x
  Height from its creation data) into the GOP fb (ToScanout), done at SubmitCommand like every present. The
  allocation list of a present has fixed slots (DXGKARG_PRESENT has no list size; DXGK_PRESENT_SOURCE_INDEX 1,
  DESTINATION_INDEX 2, d3dkmddi.h:163-165).
- Cost: one full 1080x2280x4 = 9.8 MB copy per flip at DISPATCH (SubmitCommand) into a write-combined mapping.
  Acceptable for a first light; FlipBlt 0 restores the original (flips dropped).
- Build 6: exit 0 (after dropping a guard on a field DXGKARG_PRESENT does not have), sys sha256 58E7B879...

## Review of the F2 commit (adversarial)
- DoPresent reads alloc->Mdl at DISPATCH without MmuLock (a fast mutex cannot be taken there): same as the
  original's design - VidMm keeps every allocation a submitted DMA buffer references resident until its fence
  completes, so UNMAP cannot run under an in-flight present. Not new.
- BuildPagingBuffer takes MmuLock (fast mutex) in the new !GpuUp branches: the original already took it on the
  GpuUp path of the same DDI; no new IRQL assumption.
- MW_PRESENT_OP grew (ToScanout) -> MW_DMA_PRIVATE grew; CreateContext reports sizeof(MW_DMA_PRIVATE) and Render/
  Present/SubmitCommand check against the same sizeof - consistent (rg "sizeof(MW_DMA_PRIVATE)\|sizeof(\*priv)").
- **Open, UNVERIFIED (not fixable offline):** for a DMA flip (FlipCaps 0) the software vsync reports
  ScanoutAddress = the last SetVidPnSourceAddress address, not the flipped-to allocation's. If dxgkrnl retires
  flips by matching that address, flips would still queue up. The PRAM trace answers it: Present(Flip) entries
  with the heartbeat "p" counter stuck = flips not retiring -> next step is FlipOnVSyncMmIo (dxgkrnl then calls
  SetVidPnSourceAddress per flip and the vsync reports exactly that address).
No code defect found in the F2 diff; nothing to fix.

## F3 - EnablePreemption 0 before the first full-mode boot (drivers/S10eDiag/esp/extra-gpu-mode.cmd)
- Pass 1 (GPU0 hidden) now writes GraphicsDrivers\Scheduler EnablePreemption 0 when full.flg or gpu.flg asks for a
  full mode, deletes it for 0/0 - the same rule extra-gpu-g10.cmd applies, one boot earlier. Not inside a ( ) block,
  so %ANYFULL% expands at the right time. CRLF kept (.gitattributes eol=crlf).
- Review of 00f8f74: GPU/FULL are always set (0 defaults) before the new lines; `reg delete` of an absent value only
  prints an error into set-mode.txt; the scheduler key path matches extra-gpu-g10.cmd's. No defect.
- Follow-up task: pass 2 (the boot in the new mode, if it gets that far) also saves the live PRAM slice:
  `s10ediag dumpram 0x8FE04000 0x4000 gpu\maliwddm-pram.bin` and a `peek` hex dump (dumpram uses MmCopyMemory
  physical, which may refuse firmware-reserved RAM - the peek is the fallback; both outputs kept).

## INF 1.0.13.1
- DriverVer 10/13/2026,1.0.13.1 (was 1.0.10.1): the phone has 1.0.10.1 installed; PnP ranks a same-version package
  by date only, so the bump makes the new package win when GPU0 is next enumerated. Switches PramTrace / SoftVsync /
  FlipBlt documented in the INF header (no AddReg: absent = default 1, so the install's MW_Parameters stays 0/0).
- extra-gpu-mode.cmd pass 1 also stages \S10EDIAG\gpu\MaliG76Wddm\MaliG76Wddm.inf with `pnputil /add-driver ...
  /install` while GPU0 is hidden (no live swap of the running display driver); output gpu\pnputil-stage.txt. The
  existing extra-gpu-g10.cmd installs only when the service is absent, so without this the phone would keep 1.0.10.1.
- Review of d5e6d28: whether PnP re-ranks an ALREADY-INSTALLED device (ACPI\SAMS9828\0 has 1.0.10.1) when it
  reappears after a hidden boot is **UNVERIFIED** - pnputil's /install documents updating matching devices, a
  non-present one may just keep its driver. Follow-up: drivers/S10eDiag/esp/extra-gpu-update.cmd - a live update in
  display-only mode (GPU0 visible, the way the first install replaced Basic Display), recording the device's driver
  before/after (pnputil /enum-devices /drivers) and a PRAM peek. The test steps run it first, so a full mode never
  starts on the old build (that would hang again with no trace).

## Package s10e-artifacts\mali-g13 (build 13, 2026-10-13; build-package.ps1 -Build 13, InfVerif /w "INF is VALID"
exit 0, sys + cat signed by the WDK test cert 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48, as mali-g10)
- MaliG76Wddm\MaliG76Wddm.sys 64 920 B `453C653A8D64F1D5FCD569069197E69759201E55EEA0D0353D85070C0FA76F2D`
- MaliG76Wddm\MaliG76Wddm.inf (1.0.13.1) `4AB4F4D106316CBD084ED3379405B7CD22AD4D42B2E33267A38406BA17F3773F`
- MaliG76Wddm\malig76wddm.cat `62C3B7AD4EC9FF82E688B249C2A6C7CE481B78E6DD86AB501D59E3EA4FAF869B`
- extra-gpu-update.cmd `025CE1FD2EE4BC3EE0EC424164E7820ABBDEAF6FECF30390C18C72A42C702318`
- extra-gpu-mode.cmd `0F49038DBEF66DC7E2EB1BF9F99A52B6F21FA026F4E14EE6858FB7EEF85B12ED`
- extra-gpu-g10.cmd (unchanged, CRLF) `423B894CC765F444572761AE145F4A8BAD86FCF82007FFD4DC3B47B6DB730CEF`
- m5test.exe (from mali-g10, unchanged) `7B8C003F38A18556BD07DEF8F3ACBC72508667A95672E81796875C7B3AB60B42`
NOTHING here ran on the phone.

## Device test steps (needs-boot; any GPU_POWER_ON 1 image with the nogpu.flg path, e.g. boot #113)
Start state (HANDOFF §32): \S10EDIAG\nogpu.flg SET, MaliG76Wddm 1.0.10.1 installed, Parameters 0/0.
0. ESP (TWRP, `mount -t vfat /dev/block/by-name/cache /tmp/esp`): copy mali-g13\MaliG76Wddm\* ->
   \S10EDIAG\gpu\MaliG76Wddm\ (replace), extra-gpu-g10.cmd -> \S10EDIAG\gpu\, extra-gpu-mode.cmd +
   extra-gpu-update.cmd -> \S10EDIAG\ ; **\S10EDIAG\wait.txt = 300** (the agent otherwise waits up to 1800 s for a
   desktop that a broken DWM never shows, S10eAgent.c DEFAULT_WAIT_S); no full.flg / gpu.flg in \S10EDIAG\gpu.
1. Update in display-only mode: `rm /tmp/esp/S10EDIAG/nogpu.flg`; extra.cmd = `call "%ESP%\S10EDIAG\extra-gpu-update.cmd"`.
   Boot. Expect: desktop as before; out\last\gpu\update-after.txt names malig76wddm.inf version 1.0.13.1, Started;
   update-pram-peek.txt holds "MALIG76T" and lines (">StartDevice", "<CommitVidPn 00000000", heartbeat "HB ...").
   If update-after still says 1.0.10.1: stop, record, do not go on (a full mode on the old build = blind hang).
2. Full adapter, GPU off: touch \S10EDIAG\gpu\full.flg and \S10EDIAG\nogpu.flg; extra.cmd =
   `call "%ESP%\S10EDIAG\extra-gpu-mode.cmd"`. Boot: pass 1 sets FullAdapter 1 + EnablePreemption 0, removes
   nogpu.flg, clean restart -> the full-mode boot.
   a) Desktop appears / agent runs: pass 2 collects gpu\maliwddm-pram.bin + -peek.txt, trace.reg, video.txt.
   b) Black again: wait 6 min (wait.txt 300 + collect). If no return to TWRP: force reset (Vol Down + Power ~10 s)
      and let it boot on - the next UEFI boot finds WINRUN.FLG, saves \WINPRAM.BIN (64 KiB incl. our slice at
      +0x4000) and goes to TWRP (unattended, no stay.flg). Then IMMEDIATELY `touch /tmp/esp/S10EDIAG/nogpu.flg`
      (rescue) and pull \WINPRAM.BIN + out\.
   Read: slice 0x4000..0x7FFF (python one-liner in T1). Questions it answers: last DDI entered without exit (= the
   blocking call); heartbeat uptime still advancing at the reset? (DPCs alive = thread-level hang); "v" counter > 0
   (soft vsync running) and "Vsync on 00000001" present; "p"/"s" counters and Present/SubmitCommand lines (did DWM
   present at all; flips?); "SoftVsync sync failed" (no interrupt object); R CommitStatus. Also the S10eDiag ring
   (+0xA000) "waiting: LogonUI=.. explorer=.." lines from S10eAgent.
3. A/B if step 2 is still black with presents arriving: Parameters\FlipBlt 0 or SoftVsync 0 (reg add in pass 1 by
   hand, same key) to see which switch changes the PRAM picture.
4. GpuEnable 1 only after mode 1 gives a desktop or a trace that explains it: rm full.flg, touch gpu.flg, step 2 again.
5. Back to display-only: rm the flags, extra-gpu-mode.cmd once (writes 0/0, deletes EnablePreemption).

## Review of 75035bd (package)
- Packaged INF sha256 == repo INF (4AB4F4D1...); the .sys was built after the last code commit (F2) and the INF
  bump; signer thumbprint of sys and cat = 5DD5DEE1... (Get-AuthenticodeSignature; signtool verify /pa stops at the
  untrusted WDK test root on this PC, as expected). No defect.

## Device run g13fullc (boot #114, coordinator) - analysis 2026-10-13
Logs: scratchpad\runs\g13fullc (marks.txt, last\gpu\*, WINPRAM.BIN, UEFILOG.TXT).
- **The mode was right; the WINPRAM.BIN was from another boot.** The live peek taken in pass 2
  (last\gpu\maliwddm-pram-peek.txt, decoded) says `0000003066 000001 LOAD mode 1 build 0000000d new`, Mode 1 is also
  the registry trace's latest value, and parameters-this-boot.txt = GpuEnable 0 FullAdapter 1. The saved
  WINPRAM.BIN's slice (`LOAD mode 0 ... new` at 11.2 s, SetPowerState at 78 s, presents p49e) is an earlier
  display-only boot: this run's clean restart was dumped by the firmware as \WINPRAM-RESTART.BIN (UEFILOG
  "DUMP \WINPRAM-RESTART.BIN 0x8FE00000+0x10000 -> Success", Beyond0LteBootLogDxe.c:565 - the 0x80 restart-marker
  path, which also does NOT zero the slice). Pull WINPRAM-RESTART.BIN for runs that end in a clean restart.
  ReadMode is DriverEntry-only and nothing changes MwMode later (rg "MwMode =": adapter.c DriverEntry only);
  StartDevice never falls back to display-only (ResContract/GpuProbe failures only leave the GPU half off).
- **Mode 1 did NOT hang with mali-g13** (live trace): StartDevice ok at 11.5 s, QAI, GetNodeMetadata, CreateDevice/
  CreateContext, BuildPagingBuffer, VidPN, Get/CreateAllocation (primary), CommitVidPn 401e0307
  (STATUS_GRAPHICS_MODE_NOT_PINNED, informational), SetVidPnSourceVisibility, **ControlInterrupt + "Vsync on
  00000001" at 17.1 s**, Escape 0, CreateDevice x3, DestroyDevice at 19.2/19.7 s; heartbeat at 319.6 s
  `HB i0 d499d v499d s0 p0 f0` = 18 845 soft vsyncs in ~302 s (62/s), DPCs alive, **zero Render/Present/Submit**.
  The desktop came up (explorer 30 s, agent ran). So the device after the fix shows what run 6 showed for the
  original: DWM's device on maliumd.dll gives up after an Escape (DestroyDevice) and never presents; Windows composes
  elsewhere (Basic Render, no output on our adapter) -> dark screen. The g10 hang with no vsync is consistent with
  F1 being the fix (A/B: SoftVsync 0 on this build should reproduce the hang - one boot answers it).
- s10ediag dumpram failed "ioctl 0x8d102000 failed: 59" (MmCopyMemory physical refuses this firmware-reserved RAM);
  the peek worked -> dumpram line removed from extra-gpu-mode.cmd.
- Added: DriverEntry PRAM lines `Params g.f.keys.vs.fb <hex>` (GpuEnable byte, FullAdapter byte, keys opened
  1 = service 2 = Parameters, SoftVsync, FlipBlt) and `UnixTime <hex>` (wall clock, to match a dump to its boot).
- Package **s10e-artifacts\mali-g14** (build 14, INF 1.0.14.1 so it outranks 1.0.13.1; InfVerif /w exit 0; signer
  5DD5DEE1...): sys `9CC997557ACBCAD4652029BF3260B52CC71278DF5AC624AC7B31C6B4C7580FCA`, inf
  `5DDB0F0BC8722CE5E6B09ED636B8DBD606144A9F47CEF517DD0BAE2E44243852`, cat
  `7A83B3D80A8A8D6FEBDA8A29F00F1660FC6251E1F2A15A9046A6A0EA5FFA6229`, extra-gpu-mode.cmd (CRLF)
  `31BE3A58153482D56633D1DE81B9CEC67A0F0A2275B82F6B655C791E31DAB7BC`, extra-gpu-update.cmd / extra-gpu-g10.cmd /
  m5test.exe unchanged from mali-g13. Not booted.
- Review of the two commits above: Params/UnixTime lines are 49/35 chars (fit 63); KeQuerySystemTimePrecise is
  PASSIVE-safe (DriverEntry); the script edit removed exactly the dumpram line (diff: 1 line -, comment updated).

## G13 UMD - why DWM's device on maliumd.dll gives up (2026-10-13, static + g13fullc logs)
- **Root cause (static, high confidence): maliumd.dll cannot give DWM the device it asks for.** The G12 build of Mesa
  d3d10umd has `SUPPORT_D3D10_1 0` and `SUPPORT_D3D11 0` (mali\mesa-26.2-g76 src/gallium/frontends/d3d10umd/
  State.h:39-40), so GetSupportedVersions (Adapter.cpp:140-152) offers only the D3D10.0 DDI versions and OpenAdapter10
  rejects 10.1/11 (Adapter.cpp:113-131); the D3D11 DDI function tables are not implemented at all in that frontend
  (the flag only gates the version lists). It also exports no D3D9 `OpenAdapter` (mali-g12 dumpbin exports: only
  OpenAdapter10, OpenAdapter10_2 + D3DKMT forwarders), and UserModeDriverName names maliumd.dll for all four slots.
  DWM (Windows 8+) composes through Direct3D 11 (feature levels down to 9_x, which the D3D11 runtime reaches through
  the D3D9 DDI) - neither path exists in this UMD. [DWM-uses-D3D11 is general Windows knowledge, not re-verified here;
  the probe below measures it.]
- In mode 1 the shim also finds no GPU: malikm_bind_d3dddi asks MW_ESCAPE_GPU_INFO and binds only if Valid != 0
  (wddm-mali-g52 win32-compat/malikm_drm.c:977-990); MaliG76Wddm answers Valid = 0 while !GpuUp (adapter.c GpuInfo),
  so d3d10umd falls back to softpipe on the GDI winsys (d3d10_gdi.c:203-247), whose present is StretchDIBits into a
  window - not a WDDM present. Matches the trace: one known Escape (no EscapeUnknownCode in trace.reg), CreateDevice
  x3 / DestroyDevice x2, no Render/Present/SubmitCommand ever. maliumd.dll WAS loaded (umd-copy.txt: "being used
  by another process").
- The original project never had DWM on its full adapter either: run 6 (WARP UMD) black, then render-only + Basic
  Display; its composed desktops are all malidod (KMDOD) - the same as our mode 0 (W:docs/results).
- **Nothing in the KMD can fix this**: without a UMD the full adapter is code 43 (maliwddm.inf:7-9), and Microsoft's
  WARP UMD speaks Basic Render's private protocol (run 6). A composed desktop on the full adapter needs a D3D11 DDI
  (or D3D9 DDI) in the UMD = Mesa d3d10umd D3D11 work (or a different UMD) - G13 proper, not one package.
- One decisive run (mali-g15): tools/d3dprobe (d3dprobe.exe) in pass 2 of extra-gpu-mode.cmd, as SYSTEM, with
  MALIKM_TRACE=1: per DXGI adapter the UMD file names the kernel reports for DX9/DX10/DX11, the WDDM version, and
  HRESULT + feature level of D3D11CreateDevice (plain and BGRA, as DWM) and D3D10CreateDevice; plus `dxdiag /t`.
  Expected if the analysis holds: Mali adapter D3D11CreateDevice fails (E_FAIL / DXGI_ERROR_UNSUPPORTED or
  E_NOINTERFACE), D3D10CreateDevice S_OK (softpipe), shim stderr "no Mali"/Valid 0. If D3D11 SUCCEEDS on the Mali
  adapter, the analysis is wrong and DWM's failure is elsewhere (then the next tool is an ETW DWM/DxgKrnl trace).
- Package **s10e-artifacts\mali-g15** = mali-g14's driver unchanged (sys 9CC99755..., inf 1.0.14.1 5DDB0F0B...,
  cat 7A83B3D8...) + `d3dprobe.exe` 148 376 B `E5C5B4218CA9D658B8F1E443FEB699D494699912248D11D1F9BB88AF83BE236F`
  (test-signed 5DD5DEE1...) + extra-gpu-mode.cmd (CRLF) `E71BD6CB0976FC52DCCE186BB23DF0053F239004D113F80EC9FEF0A8A7992886`
  + extra-gpu-update.cmd / extra-gpu-g10.cmd / m5test.exe as before. Not run.
- **Steps (one decisive run, unattended-safe: mode 1 reached the desktop on g13 with soft vsync):**
  1. ESP: mali-g15\MaliG76Wddm\* -> \S10EDIAG\gpu\MaliG76Wddm\, d3dprobe.exe + extra-gpu-g10.cmd -> \S10EDIAG\gpu\,
     extra-gpu-mode.cmd + extra-gpu-update.cmd -> \S10EDIAG\; wait.txt 300; no stage.flg.
  2. Display-only boot, extra.cmd = extra-gpu-update.cmd: update-after.txt must name 1.0.14.1 (the PRAM peek's
     first lines now include `Params ...` and `UnixTime ...`).
  3. touch \S10EDIAG\gpu\full.flg + \S10EDIAG\nogpu.flg; extra.cmd = extra-gpu-mode.cmd -> pass 1 (mode write,
     clean restart) -> pass 2 in mode 1 collects gpu\d3dprobe.txt, d3dprobe-stderr.txt, dxdiag.txt, PRAM peek
     (marks "d:" / "x:" in the S10eDiag ring if files are lost). Pull WINPRAM-RESTART.BIN too.
  4. Read: Mali adapter line - UMD DX9/DX10/DX11 names, D3D11CreateDevice hr (plain/bgra), D3D10CreateDevice hr;
     dxdiag "DDI Version"/"Feature Levels" for the Mali card; stderr shim lines. Then rm full.flg, run
     extra-gpu-mode.cmd once more to return to 0/0 (optional: the probe in mode 0 too, as a baseline).

## Run g15fullb (mode 1, mali-g15) - analysis 2026-10-13
- d3dprobe.txt ends after "CreateDXGIFactory1 hr 0x00000000", stderr empty. Probable cause (my bug): the output
  loop `while (EnumOutputs(i) != DXGI_ERROR_NOT_FOUND)` spins forever on any other failure (and the adapter line is
  printed only after that loop). Fixed in c55a397: stop on any failure, SEH per step, step marks, 90 s watchdog
  ("TIMEOUT in <step>"), plus output descs and the D3DKMT adapter type bits (render/display/software/post).
- dxdiag on the Mali card: **DDI Version 11.2, Feature Levels 10_0,9_3,9_2,9_1** - so the D3D11 runtime DOES get
  a feature-level-10_0 device out of the D3D10.0-only Mesa UMD (it maps 10-level onto the D3D10 DDI). This corrects
  the G13 section above: "DWM cannot get a D3D11 device" is wrong for FL 10_0. Also: Device Type Full Device (POST),
  Driver Model WDDM 1.3, Graphics Preemption DMA, **Current Mode: Unknown, Displayable: Not Supported**.
- PRAM (live peek, build 14 "Params 00010311" = gpu 0, full 1, both keys, vs 1, fb 1): every CommitVidPn returns
  **0x401E0307 STATUS_GRAPHICS_MODE_NOT_PINNED** (32.4 s twice); our MwCommitVidPn then sets PathActive FALSE.
  No later commit with a pinned mode. Same in g13fullc and in the original's run 6 (CommitStatus 0x401e0307 there
  too). Display-only (same VidPN code, target frequencies unspecified) commits with a pinned mode (CommitStatus 0).
  => **The full adapter never gets a display mode.** That explains "Current Mode: Unknown" / Win32_VideoController
  resolution empty / "Displayable: Not Supported" (no active source) and why DWM has no output on it to present to -
  before any UMD question. Then the Escape + CreateDevice x3 + DestroyDevice x2 are the runtime probing an adapter
  without a desktop. UNVERIFIED why the OS never pins: candidates are our "no recommended functional VidPN" answer on
  a full driver (dxgkrnl then derives one from the monitor modes) and the synthesised timing.
- Change (behind Parameters\RecommendVidPn, default 1, full adapter only): MwRecommendFunctionalVidPn builds path
  0 -> 0 (identity, sRGB 8/8/8, graphics, no protection, default gamma) and pins our one source and one target
  mode; any failure is logged ("RecommendVidPn path/addpath/pinned <status>") and answered as before
  (NO_RECOMMENDED_FUNCTIONAL_VIDPN). PRAM diagnostics: "Cofunc pivot <type>", "IsSupported pinned.ok <0x10 pinned |
  1 ok>", "Commit src.chk.paths <src<<16 | checks<<8 | paths>". Build 16 exit 0.
- Realistic paths to a composed GPU desktop (ranked): (1) make the full adapter own a mode first (this change; the
  PRAM trace says if dxgkrnl pins it) - without a display mode nothing else matters; (2) then DWM on maliumd at FL
  10_0 needs a WDDM present path in the UMD (d3d10umd's Present goes to GDI/StretchDIBits on softpipe; patch 0019's
  pfnPresentCb only when Panfrost is bound = GpuEnable 1) - Mesa work; (3) fallback that ships now: display-only
  desktop (mode 0) + the GPU as a compute/render device for apps is NOT available either (render-only on an ACPI
  device was refused, maliwddm.h:24-34), so mode 0 stays the desktop until (1)+(2) work.
- Package **s10e-artifacts\mali-g16** (build 16, INF 1.0.16.1, InfVerif /w exit 0, signer 5DD5DEE1...): sys
  `75871BCF6016D62D92432535F027EEC38987A44654BC8AB561DE1EE3D31ECCEA`, inf
  `A6ED4A60810CCA402B6CF6D79B5DDC8F9B28C6DF55B4090E8F25B74545BFF363`, cat
  `D1EDC07C87E89C36D28C6DFE543D57FE934CB1B81115284D3B97E769BFD1238C`, d3dprobe.exe (robust)
  `0791B40D78330B83568DD90FA3A507E045F96CC12D2DC76FE2DA8B7E411DAB58`, extra-gpu-mode.cmd (main incl. the
  coordinator's holdrestart/stage.flg edits, CRLF) `A98668E5EB81A149F455DBD5E59DA586E97D4115D7028D6B3EB14D4F195992BC`;
  extra-gpu-update.cmd / extra-gpu-g10.cmd / m5test.exe unchanged. Not run.
- Steps: as mali-g15 (update live in mode 0 -> must show 1.0.16.1; full.flg + nogpu.flg; extra-gpu-mode.cmd). Read
  in the PRAM peek: "RecommendVidPn pinned 00000000"?, then "Commit ... " + "R CommitStatus" - **00000000 = the full
  adapter got a mode** (expect dxdiag Current Mode 1080 x 2280, Displayable Supported, and Present/Flip lines /
  heartbeat p > 0 if DWM composes on it); still 401e0307 = dxgkrnl ignored the recommendation (next: log the
  monitor/target mode sets dxgkrnl offers). d3dprobe.txt: adapter type display bit, outputs, D3D11/D3D10 hr.
  Rollback switch for this change: Parameters\RecommendVidPn 0.
- Review of this change: RecommendFunctionalVidPn is only in the full init table and returns the old answer in
  display-only (mode 0 behaviour unchanged - the validated desktop); pointers initialised (C4703 caught it at
  build); failure paths log and return the old status; IsSupported's log uses `pinned` only as a NULL test after
  release. Not verified: whether dxgkrnl expects the target mode set to also carry the monitor's other modes.

## Run g16full (mode 1, mali-g16) - analysis 2026-10-13
- d3dprobe (fixed) completes: Mali adapter dedicated 0 / shared 1024 MB, **outputs 0 (EnumOutputs(0) 0x887A0022
  DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)**, UMD DX9/10/11 = maliumd.dll, WDDM 0x514, adapter type 0xB (render, display,
  post), D3D11CreateDevice plain/bgra S_OK at 10_0, D3D10CreateDevice S_OK. So the UMD path works; the adapter has
  no active output.
- PRAM (build 16): **RecommendFunctionalVidPn never called** (no "RecommendVidPn pinned" line). dxgkrnl runs many
  IsSupported (pinned.ok 0x11 = our source mode pinned and accepted, 0x01 = nothing pinned) / EnumCofunc rounds
  (pivots 1-5: source, target, scaling, rotation, none), then **"Commit src.chk.paths 00000100" = source 0,
  connectivity checks 1 (IGNORE), 0 PATHS** -> CommitStatus 401e0307. dxgkrnl itself gives up on every path and
  commits an empty topology; our DDIs never fail. The original's run 6 (real HDMI EDID) had the same 0x401e0307.
  Display-only (unspecified target frequencies) pins fine.
- Working hypothesis (UNVERIFIED): the monitor dxgkrnl builds for a target with NO descriptor gets default
  frequency ranges, and our synthesised 1360x2325 @ 60 Hz = 189.72 MHz / 139.5 kHz target mode is outside them,
  so no target mode is cofunctional with the monitor and every path is dropped.
- Change (full adapter only, Parameters\SynthEdid default 1): QueryDeviceDescriptor returns a 128-byte EDID 1.3
  built for exactly FillSignal's timing: preferred DTD 1080x2280, blanking 280/45, 18972 x 10 kHz (= 189.72 MHz,
  60.0 Hz / 139.5 kHz, decoded independently in Python), range limits 50-70 Hz / 30-160 kHz / 200 MHz, name
  "S10e panel", id "EXW"/0x9820, size undefined, sRGB chromaticity, checksum. Nothing is read from the panel or
  programmed. PRAM: "EDID off.len", "Monitor modes <n>", "Cofunc path s.t.scl.rot". MW_HBLANK/MW_VBLANK shared by
  FillSignal and the EDID. INF 1.0.17.1. Build 17 exit 0.
- Package **s10e-artifacts\mali-g17** (InfVerif /w exit 0, signer 5DD5DEE1...): sys
  `47A15F54D26B577F3BD679ECEFDC169837351D3F61CEB27ED8A3982F72A25EE9`, inf
  `90C692ED9B2BFBE224D6E39600253D2B8099033FE928522D021B3E04DB94594B`, cat
  `76A1670CDCFE7859990AD06C28349BA6399F33DC026E0F4FA9977AFFEF89530D`; d3dprobe / scripts / m5test as mali-g16.
- Read next run: "EDID off.len 00000080" (dxgkrnl took it), "Monitor modes" count, then "Commit src.chk.paths" with
  paths 01 and "R CommitStatus 00000000"; d3dprobe outputs 1; dxdiag Current Mode 1080 x 2280. If paths stay 0: the
  next suspect is VOT_INTERNAL on a full adapter (try D3DKMDT_VOT_HD15/HDMI as the original's run 6 child... which
  also failed) or the target-mode set content - then log the pinned target mode in IsSupported.
- Review: display-only path unchanged (QueryDeviceDescriptor returns NO_DESCRIPTOR there, as validated); BuildEdid
  refuses sizes > 12 bits or > 200 MHz (no truncated DTD); C4310 caught a byte cast at build; the descriptor copy is
  bounded by both the request and 128.

## Run g17full (mode 1, mali-g17) - analysis 2026-10-13
- PRAM (decoded): **QueryDeviceDescriptor returned 0** (EDID taken; the second call at offset 128 got
  c01d0008 NO_MORE_DESCRIPTOR_DATA - trace.reg Fail_QueryDeviceDescriptor), RecommendMonitorModes 0. Still never a
  RecommendFunctionalVidPn call. Paths seen in every EnumCofunc round: source 0 -> target 0, scaling/rotation
  identity (01) or unpinned (fe). IsSupported accepts every VidPN (0x11 our source mode pinned, 0x01 none).
- The decisive sequence, every time: cofunc rounds -> **GetStandardAllocationDriverData x2 -> CreateAllocation ->
  OpenAllocation -> CloseAllocation -> DestroyAllocation -> CommitVidPn with 0 paths** (src.chk.paths 00000100) ->
  401e0307. dxgkrnl builds the mode's shared primary through us, drops it immediately (no BuildPagingBuffer =
  VidMm never paged it in, no DescribeAllocation, no SetVidPnSourceAddress) and falls back to an empty topology. So
  the EDID hypothesis is refuted; the failure is in dxgkrnl's mode-set step around the primary, after all our DDIs
  returned success - only dxgkrnl's own diagnostics can say what it disliked.
- Candidates (ranked, none verifiable offline): (1) the primary lives in our single **aperture** segment - VidMm may
  refuse to pin a scan-out primary there (a primary needs a segment it can keep resident and hand to
  SetVidPnSourceAddress; WDDM 1.x drivers normally give the primary a memory segment); (2) the POST-adapter takeover
  (adapter type 0xB = render+display+post; dxgkrnl reports 'Device Type: Full Device (POST)') expects something of
  the POST framebuffer path we do not provide; (3) the primary's private data (our MW_ALLOCATION_INFO Size =
  Pitch*Height, Pitch w*4) vs dxgkrnl's expected pitch/size.
- Minimal VidPN for FlipBlt-mirroring without a path: not possible - DWM only presents to outputs, and an adapter
  whose committed topology has no path has no output (d3dprobe: EnumOutputs 0x887A0022). The full adapter must own
  path 0 -> 0 with a mode for any desktop on it; FlipBlt only decides how that output's pixels reach the GOP fb.
- Next step = one decisive capture: extra-gpu-mode.cmd pass 1 now creates an ETW AutoLogger "MaliDxgk" for
  Microsoft-Windows-DxgKrnl {802EC45A-1E99-4B83-9920-87C98277BA9D} (all keywords, level 5, 32 MB sequential,
  C:\S10eTmp\dxgk.etl) for full modes; pass 2 stops + deletes it, keeps gpu\dxgk.etl, decodes with tracerpt to
  dxgk.xml + dxgk.csv and filters fail/unable/invalid/error/VidPn/primary lines into dxgk-msgs.txt. Event 494
  ("EventDescription") is how the original project read dxgkrnl's reasons (W:docs/results/m5.2-pe-run*-dxgkrnl-etw.txt).
- Package **s10e-artifacts\mali-g18** = mali-g17's driver (1.0.17.1, sys 47A15F54...) + extra-gpu-mode.cmd with the
  ETW capture (CRLF) `DFBE06688F91DB3FBDFD57FBF7A249A04FCF94AB67BD55977D014638B7A02C9D`; rest as mali-g17. No driver
  change -> no update boot needed if 1.0.17.1 is installed: copy extra-gpu-mode.cmd, full.flg + nogpu.flg, run.

## Run g18etw - DxgKrnl ETW decoded (2026-10-13) -> the cause
Decoded on the PC (scratchpad\dxgkcsv.py over gpu\dxgk.csv; 54 rows of event 494 "DCStart" = dxgkrnl's own
diagnostic text). Every mode-set attempt of the full adapter repeats:
1. **"All segment specified in supported write segment set for a shared primary must be CPU visible."** (data 1907)
   - our only segment, the aperture (memory.c MwQuerySegment), had Flags.CpuVisible 0 (the original's flags); our
   primaries name it in SupportedWriteSegmentSet -> dxgkrnl refuses the shared primary -> DestroyAllocation (the
   create/destroy pair in every PRAM trace).
2. **"Driver returned an invalid NTSTATUS code: 0x%I64x" with 1075708679 = 0x401E0307** - our MwCommitVidPn handed
   back STATUS_GRAPHICS_MODE_NOT_PINNED from pfnAcquirePinnedModeInfo as its own result.
3. "VidPn source 0 ... is not in any session view ..." follows; later "Failed to get display config buffer sizes
   when looking for internal target", "Can't reference adapter by luid", "Failed to get pairing adapters" - the
   consequences of having no committed path.
Fixes (both full-adapter only; display-only never reports segments and its commits pin):
- memory.c: aperture segment Flags.CpuVisible = 1 (Parameters\SegmentCpuVisible, default 1; 0 = old flags).
  Presents are CPU copies through the allocation's MDL anyway. UNVERIFIED: whether dxgkrnl wants anything else of a
  CPU-visible APERTURE segment (StartDevice / QAI failure would show as code 43 + PRAM "<QueryAdapterInfo" status).
- display.c: CommitVidPn returns STATUS_SUCCESS for a source with no pinned mode (it is just inactive).
- dxgk-msgs.txt filter narrowed to event 494 rows (findstr /r "DxgKrnl, *DCStart *, *494," - 54 rows on g18etw).
- INF 1.0.19.1; PRAM DriverEntry line "SegCpuVis.RecVidPn.Edid <hex>". Defaults unchanged: GpuEnable 0 /
  FullAdapter 0 (the INF's MW_Parameters writes 0/0 on install) = display-only stays the default mode.
Package **s10e-artifacts\mali-g19** (build 19, InfVerif /w exit 0, signer 5DD5DEE1...): sys
`066BAFE3171A98FCA4EC48F720ACB2B53BEC3FFA00F1380BDC45AAED2B952C34`, inf
`905D765C191BBA2A7EEA87171967A340453D6993D98F3BD29989D6389EEE899A`, cat
`700670549FBB9C293F5A70EA1D12FDF1AD46C4D34624A724AEB4F79B191BB52C`, extra-gpu-mode.cmd (CRLF)
`F1BF9636FE3665E37A1202C90C57566A1CA6C9E273C8E3A9BF91F06E4E53CD39`; d3dprobe/update/g10 scripts/m5test as before.
Steps: update live in display-only (must show 1.0.19.1) -> full.flg + nogpu.flg + extra-gpu-mode.cmd. Read:
gpu\dxgk-msgs.txt (no "must be CPU visible", no "invalid NTSTATUS"), PRAM "Commit src.chk.paths" paths 01 +
"R CommitStatus 00000000", BuildPagingBuffer / SetVidPnSourceAddress lines, d3dprobe outputs 1, dxdiag Current
Mode 1080 x 2280; heartbeat p > 0 / Present lines if DWM composes (UMD questions start only after that).

## Run g19full - the KMD mode set now WORKS (2026-10-13)
- ETW event 494 (scratchpad\dxgkcsv.py): "must be CPU visible" and "invalid NTSTATUS" are GONE. Remaining: 267x
  "There should be at least one broadcast context passed to signal" (data 960), 2x "Cannot find the file version
  resource in DxgkpGetFileVersion" (our .sys has no VERSIONINFO resource), 1x "When opening a synchronization
  object, the NoGPUAccess flag ... must match" (a user-mode runtime's sync object) - none about the mode set.
- PRAM (build 19): GetStandardAllocationDriverData -> CreateAllocation -> OpenAllocation -> **DescribeAllocation ->
  CommitVidPn 00000000 -> SetVidPnSourceAddress -> SetVidPnSourceVisibility -> ControlInterrupt -> Present / Patch /
  SubmitCommand** x52 (every ~47 ms, 27.6-30.1 s: the boot console's CDD presents into our primary, CPU blts to the
  GOP fb), then at 30.27-30.29 s visibility off/on, BuildPagingBuffer x2, Close/DestroyAllocation of that primary -
  and **no DDI call at all for the next 300 s** (heartbeat at 335 s: v 0x4b26 vsyncs, s/p 0x34 = still 52). So the
  adapter owns a mode and accepts presents; the session-1 compositor (DWM) never creates a primary, an allocation or
  a present on it.
- Why (static, consistent with every trace): maliumd.dll in mode 1 = Mesa d3d10umd on **softpipe with the GDI
  software winsys** (malikm_bind_d3dddi refuses to bind without GPU_INFO Valid, d3d10_gdi.c default driver
  softpipe): its resources are user-mode memory (no pfnAllocateCb -> no CreateAllocation in the KMD) and its present
  is StretchDIBits into a window (d3d10_gdi.c d3d10_gdi_acquire_hdc / sw winsys displaytarget_display), never
  pfnPresentCb -> no KMD Present. DWM "runs" on it and nothing reaches the kernel or the panel.
- Also: the session-0 d3dprobe / dxdiag ("outputs 0", "Current Mode Unknown") were taken as SYSTEM in session 0,
  which owns no display - not proof of no mode. g20's script repeats d3dprobe in the user session and lists DWM's
  loaded graphics modules (maliumd / libgallium = DWM's device is on our adapter).
- **The next fix is in the UMD, not the KMD** (Mesa, mali\mesa-26.2-g76; MILESTONES G13): (a) a WDDM present path
  for the software screen - wrap the GDI sw winsys' displaytarget_display to map the display target and hand the
  pixels to malikm_present_d3dddi (patch 0019's pfnPresentCb path, today only on the Panfrost screen) and let
  malikm_bind_d3dddi bind the runtime callbacks for presenting even when GPU_INFO.Valid is 0; (b) for GpuEnable 1,
  make Panfrost the default screen when bound to the maliwddm runtime (DWM has no GALLIUM_DRIVER). Whether DWM's own
  flip-model swap chain is served by pfnPresentCb-from-a-staging-copy is UNVERIFIED (never validated upstream).
- Package **s10e-artifacts\mali-g20** = mali-g19 driver (1.0.19.1, sys 066BAFE3...) + extra-gpu-mode.cmd with the
  user-session probe and DWM module list `FB4F16A9FC5DE10B95773E133F2A3EC6DAABB97A4953794AAE3AAD2F767C7B26` (CRLF),
  run-as-user.ps1 `DDB47BE5...` (same as the repo's). No driver change -> no update boot.

## G13 Mesa change (2026-10-13) - tools/mesa-build/patches/0020
Working copy mali\mesa-26.2-g76 commit 6efa57c (on a36e5bb = G12); the same diff is kept byte-exact in the repo
(`tools/mesa-build/patches/0020-g13-runtime-present-without-gpu-and-panfrost-default.patch`, `-text` in
.gitattributes because the touched files are CRLF; build-mesa-g76.ps1's source stage now applies tools/mesa-build/
patches/*.patch after the reference patches, one commit each).
- include/win32-compat/malikm_drm.c `malikm_bind_d3dddi`: GPU_INFO answered by maliwddm (Count != 0) with Valid == 0
  -> **present-only bind**: a runtime context is created and ddi.{on,adapter,device,context,cb,dxgi} kept, so
  malikm_present_d3dddi works; `wddm`/wgpu/command buffers are NOT set (Panfrost stays off). Returns 1 (0 = full
  bind as before, -1 = not maliwddm). Requires pfnLockCb/UnlockCb/DeallocateCb (used by the present surface).
- src/gallium/targets/d3d10umd/d3d10_gdi.c: d3d10_bind_runtime keeps the result; **default driver = panfrost when
  the full bind succeeded** (GALLIUM_DRIVER still overrides); a softpipe screen created while bound (0 or 1) gets
  d3d10_pan_flush_frontbuffer (read back the resource, BGRX, malikm_present_d3dddi -> DXGI pfnPresentCb -> KMD
  Present blt) with softpipe's own GDI flush_frontbuffer as the fallback when the runtime present fails.
- Build: `build-mesa-g76.ps1 -Stage cross,collect -Out ...\mali-g21\umd` incremental (6 steps; only the existing
  wpatch/wpatch_size unused warnings), libgallium_d3d10.dll = maliumd.dll sha256
  `4ded674dd9077a191bd6ea19814d2d59071ee556de081d0e1dd52539ac22db76`, AA64, CUI subsystem as G12.
- UNVERIFIED: whether DWM's swap chain present (DXGI _Present -> flush_frontbuffer) on the D3D11-over-D3D10-DDI
  runtime reaches pfnPresentCb with a context the runtime accepts; the PRAM trace answers it (KMD Present lines from
  DWM after the boot console's, heartbeat p growing).
- Review of b0981d5: the claim "byte-exact because the touched files are CRLF" was wrong - the Mesa blobs are LF
  (`git show a36e5bb:<file> | grep -c CR` = 0; the CRLF seen in the working tree is a checkout artefact) and the
  exported patch is LF. Verified: `git apply --check` of the repo patch on a fresh LF tree of a36e5bb's two files ->
  OK, and `git apply -R --check` on the working copy -> OK. The `-text` .gitattributes line is removed again and the
  build script comment corrected (follow-up commit). Also checked: the present-only branch never sets `wddm`, so
  wddm_init() still requires GPU_INFO Valid for Panfrost; malikm_present_d3dddi takes wddm_lock as before.
- UMD deployment (extra-gpu-mode.cmd pass 1, GPU0 hidden, before the clean restart): maliumd.dll and
  libgallium_d3d10.dll from \S10EDIAG\gpu are copied into System32 with sha256 into gpu\umd-deploy.txt (+ "umd:"
  marks) - in pass 2 DWM holds maliumd.dll (g13fullc "being used by another process"). extra-gpu-g10.cmd's own
  pass-2 copy stays (it just fails while DWM has the file).
- Package **s10e-artifacts\mali-g21**: driver = mali-g19/g20 (1.0.19.1, sys 066BAFE3...), maliumd.dll =
  libgallium_d3d10.dll `4DED674DD9077A191BD6EA19814D2D59071EE556DE081D0E1DD52539AC22DB76` (umd\ has pdb, SHA256SUMS,
  dumpbin, vc_redist), extra-gpu-mode.cmd (CRLF) `333621D846101941BF4C51C12E4B67AE4A14A542EB59BE1DD16970BD78B0D967`,
  d3dprobe/run-as-user/update/g10/m5test as mali-g20.
- Steps: ESP \S10EDIAG\gpu\ <- maliumd.dll + libgallium_d3d10.dll (+ d3dprobe.exe); \S10EDIAG\ <- extra-gpu-mode.cmd,
  run-as-user.ps1; full.flg + nogpu.flg; extra.cmd = extra-gpu-mode.cmd. Read: gpu\umd-deploy.txt (both hashes
  4ded674d...), dwm-modules.txt (maliumd loaded), PRAM: KMD Present/SubmitCommand lines AFTER the boot console's
  (~30 s) and a growing heartbeat "p", MALIKM shim line in d3dprobe-stderr ("present only"), webcam: desktop.
  Mode 1 only for this run (GpuEnable 1 = Panfrost default needs G11 m5test first).

## Run g21full - DWM now presents through maliumd, dxgkrnl rejects its flips (2026-10-13)
- d3dprobe-stderr: "malikm: bind to the runtime device ...: present only (maliwddm without a GPU)" x3 - patch 0020's
  present-only bind works. dwm-modules: DWM loaded dxgi, d3d11, D3D10Warp.dll AND maliumd.dll; pass-2 hashes.txt:
  the ESP maliumd = 4ded674d... (the new build). user-task: "no explorer.exe owner - nobody logged on" = logon never
  completed in that boot.
- DxgKrnl ETW event 494: **227x "Source of Flip must be primary"** with status 0xC000000D (+ the usual StartDevice /
  file-version lines); nothing about the mode set. = DWM's frames now reach the runtime's present callback as FLIPS
  (DXGI_DDI_PRESENT_FLAGS.Flip), from the shim's single non-primary present surface -> dxgkrnl refuses each one.
- PRAM (live, build 19): mode set + 50 boot-console presents as in g19 (heartbeat s/p 0x32), no KMD Present after
  that; from ~235 s to the end of the ring a DescribeAllocation every ~1.39 s (someone - most likely DWM retrying
  its swap chain - describing an allocation over and over).
- The long pass-2 boot / no clean restart: WINPRAM-RESTART.BIN holds the first mode-1 boot - heartbeat alive at
  696 s, last DDI lines at 511 s (VidPN re-enumeration), **no StopDevice / SetPowerState ever** -> shutdown never got
  to the display driver; consistent with winlogon/LogonUI stuck behind a DWM that cannot present (UNVERIFIED; a
  driver-side D3 hang would show ">SetPowerState" without "<"). The later WINPRAM.BIN is a mode-0 boot.
- **Fix = Mesa patch 0021** (mesa-26.2-g76 be83cc4; tools/mesa-build/patches/0021-g13-flip-from-primary-present-
  surfaces.patch, applies to a fresh tree): DxgiFns.cpp _Present records Flags.Flip in a thread-local
  `d3d10_present_flip`; d3d10_gdi.c passes it to `malikm_present_d3dddi(..., primary)`; the shim keeps two primary
  present surfaces (D3DDDI_ALLOCATIONINFO.Flags.Primary 1, VidPnSourceId 0, MW_ALLOC_PRIMARY, A8R8G8B8 like the
  VidPN source mode) used in turn for flips; blt presents keep the old surface. KMD unchanged: a Flip present is
  the FlipBlt CPU copy into the GOP fb (F2). Build: incremental 8 steps, maliumd = libgallium_d3d10
  `0721D314EAF4294CCC2B2E38829CD127E0F29BF682AA301EDA85205DA5093FE1`.
- Package **s10e-artifacts\mali-g22** = mali-g21 + this UMD (driver 1.0.19.1 unchanged, scripts unchanged).
  Steps: ESP \S10EDIAG\gpu\ <- maliumd.dll + libgallium_d3d10.dll; full.flg + nogpu.flg; extra-gpu-mode.cmd (pass 1
  deploys the UMD). Read: dxgk-msgs.txt without "Source of Flip must be primary"; PRAM ">Present" lines after ~30 s
  and the heartbeat "p" growing (each = one DWM frame blitted to the panel); webcam desktop; whether the clean
  restart now happens. UNVERIFIED: dxgkrnl may next want flips to retire via the vsync's reported address
  (FlipOnVSyncMmIo) - the trace would show presents stalling after 2.

## Run g22full - flips reach dxgkrnl, rejected as PRESENT_MODE_CHANGED (2026-10-13)
- Event 494: "Source of Flip must be primary" gone; left: StartDevice/AddDevice/virtualization lines, 3x "Failed to
  get pairing adapters from adapter .. VidPn source 1", 2x file version, 1x NoGPUAccess - none fatal.
- **DxgKrnl event 184 (present: hContext, VidPnSourceId 0, FlipInterval 1, Flags 0x401004 = flip, hSrcAllocHandle,
  ReturnStatus): 440 flips, ALL ReturnStatus 0xC01E0005 STATUS_GRAPHICS_PRESENT_MODE_CHANGED**, alternating the two
  primaries (0x40000800 / 0x40000840) - the patch-0021 surfaces; 4 other presents (flags 0x8000) succeeded.
- PRAM: KMD Present never called after the boot console (heartbeat p 0x30); a DescribeAllocation PAIR every ~1.4 s
  = dxgkrnl describing both primaries before each flip retry, then refusing them. user-task: nobody logged on again.
- Cause (strong): MwDescribeAllocation answered RefreshRate 60/1 for every allocation (the original's constant),
  while the committed target mode carries VSyncFreq = PixelRate / (totalW * totalH) = 189720000 / 3162000
  (FillSignal). dxgkrnl's flip path compares the primary's description with the current mode; the rationals differ
  -> PRESENT_MODE_CHANGED. (The boot console used Blt presents, which skip that check - why they worked.)
- Fix (Parameters\ModeRefreshExact, default 1): a MW_ALLOC_PRIMARY allocation reports the mode's exact VSyncFreq
  (new display.c MwModeRefresh from FillSignal); the first 8 descriptions go to PRAM ("Describe wxh", "Describe
  fmt.flags") so a size/format mismatch of DWM's swap chain shows next run. INF 1.0.23.1, build 23 exit 0.
- Logon + restart hang: both follow from DWM never getting a frame out (LogonUI cannot show; shutdown never reached
  the driver in g21) - expected to clear with working flips; if the restart still hangs with flips working, the
  next read is the PRAM tail of WINPRAM-RESTART.BIN for ">SetPowerState" without "<".
- Package **s10e-artifacts\mali-g23**: sys `657C7E72F114830A8249CEB6601C577CF7BF690005E02D62E3FD0A4F4FB89F38`, inf
  `B3E0F82D36953E6EA974371B3DA810242B6DA729225956054C68A04E4A7E5A3B`, cat
  `B2FCC65CDA70B3D94DA07E82D55722E9ECD52425CD9155BBDFF014EFD6A58913` (InfVerif /w 0, signer 5DD5DEE1...); UMD
  0721d314 (mali-g22) and scripts unchanged. Steps: update live in display-only (1.0.23.1) -> full.flg + nogpu.flg
  -> extra-gpu-mode.cmd. Read: ETW 184 ReturnStatus 0 for flips, PRAM ">Present" after 30 s + heartbeat p growing,
  "Describe" lines (DWM's swap chain size = 1080x2280? format 0x15 A8R8G8B8?), webcam desktop, clean restart.

## Run g23full - still PRESENT_MODE_CHANGED (2026-10-13)
- ETW 184: the same 440 flips (two primaries 0x40000800/0x40000840, flags 0x401004) all 0xC01E0005; 4 non-flip
  presents ok. No other DxgKrnl event near a failed flip carries a reason (only 471 process lines). PRAM: KMD
  Present never after the boot console (p 0x31), DescribeAllocation pairs every ~1.4 s, nobody logged on.
- The "Describe" lines were written to the ROLLING region and the DescribeAllocation flood overwrote them - my
  mistake (follow-up done here: first-call region). So the exact-rational refresh did not change the outcome, and
  what dxgkrnl compares is still unknown.
- g24 changes (build 24, INF 1.0.24.1):
  1. FillSignal (full adapter) states the timing as reduced rationals - VSync 60/1, HSync 60*totalH/1 (PixelRate
     unchanged) - so the mode, DescribeAllocation (60/1, also via MwModeRefresh) and the runtime's 60 Hz can no
     longer differ as rationals. Parameters\Refresh601 (default 1; 0 = the original's PixelRate/total form).
     RISK: the EDID's DTD still yields 189.72 MHz / 1360 / 2325; if dxgkrnl then refuses the target mode as not
     cofunctional (back to 0 paths / "Commit src.chk.paths 00000100"), Refresh601 0 restores g23's behaviour.
  2. PRAM first-call region: "Describe wxh" / "Describe fmt.flags" (first 8) and "StdAlloc type.fmt" / "StdAlloc
     wxh" (first 4: the shared primary dxgkrnl asks for = the mode it holds) - the next run shows whether DWM's
     swap-chain primaries match the mode in size and format.
- Package **s10e-artifacts\mali-g24**: sys `A34D88F2E4A5052B104FAC5C42A62F67FDA42441168B4255E6FE86B24D905EBF`, inf
  `745D73780572B0E415C1A2E4D455A6E50341CB5B1568CA73B5FFE0ECD4B5449D`, cat
  `DA938EB394D9B08C983F15AEF9CCE3AB48C95B752E3C3772227A04DF7E6E21BD` (InfVerif /w 0, signer 5DD5DEE1...); UMD and
  scripts = mali-g23. Steps as g23 (update live to 1.0.24.1, then full mode). Read: ETW 184 status; first-call PRAM
  lines StdAlloc (type 1 = shared primary: format 0x15/0x16? 1080x2280?) vs Describe (DWM's primaries); commit paths.

## Run g24full - descriptions identical, flips still MODE_CHANGED -> swap-chain resources (2026-10-13)
- Commit fine with Refresh601 (CommitVidPn 0; the EDID risk did not happen).
- First-call PRAM: dxgkrnl's own primaries "StdAlloc type.fmt 01000015 / 02000015" (shared primary, then shadow)
  "wxh 043808e8" = 1080x2280 A8R8G8B8; every "Describe" (DWM path primaries) = 1080x2280, fmt 0x15, flags 03 -
  **identical** to the mode. ETW 184: 442 flips, all 0xC01E0005 again (the same two shim surfaces 0x40000800/840).
  => not the allocation description. What remains: the flipped allocations are the SHIM's (pfnAllocateCb without a
  runtime resource) while DXGI flips DWM's swap-chain resource - dxgkrnl ties a flip's primary to the runtime
  resource / mode it was created for, and an allocation with no resource can never match (inference; the status is
  the one DXGI uses to make a swap chain re-create its buffers, which DWM does every ~1.4 s = the Describe pairs).
- **Mesa patch 0022** (mesa-26.2-g76 1f49bdd; tools/mesa-build/patches/0022-g13-dxgi-primaries-get-runtime-resource-
  allocations.patch, fresh-tree apply OK): d3d10umd CreateResource with pPrimaryDesc (2D) calls
  `malikm_primary_create(hRTResource, VidPnSourceId, w, h)` -> pfnAllocateCb with **D3DDDICB_ALLOCATE.hResource =
  the runtime resource**, Flags.Primary, VidPnSourceId, MW_ALLOCATION_INFO A8R8G8B8 1080-style pitch; the handle is
  kept in Resource.kmt_primary; _Present passes it (thread-local d3d10_present_alloc); flush_frontbuffer then copies
  the frame into THAT allocation (locked once) and calls pfnPresentCb with it; the shim surfaces remain the fallback;
  DestroyResource forgets it (the runtime frees resource-owned allocations). Build: maliumd = libgallium_d3d10
  `C93FE995097A9FE45E8E680476765C3FFA247BD47358A1EE2C8B478CCFC536DD`.
- **One-run test** (mali-g25 = g24 driver 1.0.24.1 + this UMD; no update boot): full.flg + nogpu.flg +
  extra-gpu-mode.cmd. Decisive in ETW 184: flips whose hSrcAllocHandle is NOT 0x40000800/0x40000840 (= the
  resource-owned primaries) and their ReturnStatus - 0 => the question is answered and PRAM must then show KMD
  Present lines + heartbeat p growing; 0xC01E0005 on resource-owned primaries too => it is not the resource tie
  (next: DXGI's mode tracking - the runtime's own GetDisplayModeList / mode the swap chain was created for).
  If no new handles appear at all, CreateResource never got a pPrimaryDesc (DWM uses a different swap-chain path).

## Run g25full - resource-owned primaries also refused -> MMIO flips (2026-10-13)
- ETW 184: **442 flips, all from ONE new handle 0x400008C0** (the resource-owned primary of patch 0022 - the shim
  pair 0x40000800/840 is gone) and **all still 0xC01E0005**; 4 non-flip presents ok. Decision table: "new handles
  but still MODE_CHANGED" -> it is not the allocation's resource tie, nor its description (g24).
- What every attempt shared: the adapter reports **no FlipCaps**, so dxgkrnl does DWM's flips as DMA-buffer flips
  (Present(Flip) -> SubmitCommand) - the path the original project never validated (its full adapter never got
  to DWM: run 6), and not the one WDDM 1.x display drivers normally take: FlipOnVSyncMmIo, where dxgkrnl calls
  DxgkDdiSetVidPnSourceAddress per flip and retires it when the driver's CRTC_VSYNC reports that PrimaryAddress
  (d3dkmddi.h DXGK_FLIPCAPS "Support Flip as mmio at vsync interrupt"; the CrtcVsync.PhysicalAddress field =
  "Physical Address of displaying buffer"). UNVERIFIED that this is what PRESENT_MODE_CHANGED wants - it is the
  remaining structural difference, and cheap to try behind a switch.
- **g26 (build 26, INF 1.0.26.1), Parameters\FlipMmio default 1**:
  - DRIVERCAPS FlipCaps.FlipOnVSyncMmIo = 1.
  - SetVidPnSourceAddress (any IRQL; first call's IRQL to PRAM "SetSourceAddress irql") queues the allocation
    lock-free (PendingAddress, then InterlockedExchangePointer PendingPrimary).
  - The soft-vsync DPC first "latches": takes the pending primary under ScanLock, CPU-copies it whole into the GOP
    fb (DoPresent ToScanout, the allocation's MDL), sets ScanoutAddress = its PrimaryAddress, counts Latches (added
    to the heartbeat "p"), THEN reports CRTC_VSYNC with that address - a display controller's vblank latch.
  - FreeAllocation drops a queued latch of the allocation under ScanLock (MwScanoutForget) - no copy from freed
    memory. Present(Flip) no longer copies at SubmitCommand while FlipMmio (no double copy).
  - FlipMmio 0 = g25 behaviour.
- Package **s10e-artifacts\mali-g26**: sys `7B65F3A344127E79A24083E8C916609C30E9E1DC2F33F7B9CCCF21C49AB24CCB`, inf
  `C1BC0DA103B82CF43AF521852A591BA45A81B5E5745FE838FF35BCC8A1287FE0`, cat
  `328BDCD3B3A790C00454B0483B32BEBB6ABD634FDE9903E18C063F71A940C740` (InfVerif /w 0, signer 5DD5DEE1...); UMD
  c93fe995 (g25) + scripts unchanged. Steps: update live to 1.0.26.1, then full mode. Read: ETW 184 status of the
  flips, PRAM ">SetVidPnSourceAddress" lines + "SetSourceAddress irql", heartbeat p (= presents + latches) growing.
- If this also fails, the options left (no more cheap toggles): (1) stop flipping - make the UMD present every
  DWM frame as a BLT to the shared primary (DXGI_DDI_PRESENT_FLAGS from the runtime decide; would need the UMD to
  refuse flip-model: report DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT on primaries so DXGI composes by blt); (2) a
  live kernel debug (KDNET/serial M17) breakpoint on dxgkrnl's flip validation to read the exact check; (3) keep
  display-only + no GPU desktop (mode 0, works) until the Mesa UMD is a real WDDM UMD (G13 proper).

## Run g26full - MMIO flips fail the same way -> no flips at all (2026-10-13)
- runs\g26full has UEFILOG.TXT, WINPRAM.BIN, WINPRAM-RESTART.BIN and last\gpu\* (dxgk.*, peek, trace.reg, ...).
- ETW 184: 442 flips from 0x400008C0, all 0xC01E0005 again. PRAM (build 26, "Switches 00111111" = FlipMmio on):
  ">SetVidPnSourceAddress" only once (the mode set, "SetSourceAddress irql 00000000" = PASSIVE), never for a DWM
  flip; heartbeat p 0x35 (= 0x33 presents + 2 latches of the mode-set primary). So dxgkrnl refuses DWM's flips
  BEFORE either flip mechanism (DMA or MMIO) reaches the driver - flip caps are not the issue either.
- **Option 1, Mesa patch 0023** (mesa-26.2-g76 d729e24; tools/mesa-build/patches/0023-g13-dxgi-primaries-no-
  scanout.patch, fresh-tree apply OK): on the maliwddm runtime (new `d3d10_runtime_is_maliwddm()` = last bind
  result >= 0) CreateResource sets `pPrimaryDesc->DriverFlags |= DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT` (dxgiddi.h
  0x1; the flag the frontend's own #if 0 block names) and skips patch 0022's resource-owned primary allocation, so
  DXGI composes by Blt presents into the shared primary (the path the boot console's presents take, which work).
  MALIKM_SCANOUT=1 restores the flip path. KMD unchanged (g26 1.0.26.1; Blt presents to the primary = DoPresent CPU
  copy to the GOP fb at SubmitCommand). UMD maliumd = libgallium_d3d10
  `CFA93C590E0C90295B25E874A91AE2BB3BDA6781E7620EB707CA3BAB6AF67A4E` (contains "MALIKM_SCANOUT"; Resource.cpp.obj
  rebuilt).
- Package **s10e-artifacts\mali-g27** = g26 driver + this UMD + unchanged scripts. Display-only remains the default
  (install writes GpuEnable 0 / FullAdapter 0). Steps: full.flg + nogpu.flg + extra-gpu-mode.cmd (pass 1 deploys the
  UMD; no update boot). Read: ETW 184 - flags 0x1 (Blt) presents with ReturnStatus 0 and no 0x401004 flips; PRAM
  ">Present" lines after ~30 s and heartbeat p growing; webcam desktop; clean restart.
