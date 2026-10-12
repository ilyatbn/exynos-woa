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
