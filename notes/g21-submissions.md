# G21 - fewer GPU submissions per frame (Game Dev Story: ~18 Render DDIs per frame)

User 2026-10-17: "see if you can do something about the 18 submissions".
Measured (notes/g20-scaled-modes.md, gamebench): native gameplay 15 presents/s with ~273 renders/s, 720p 20/s with
~359 renders/s = ~18 Render submissions per presented frame; GPU 3D 26-29 % busy, the two MID cores 60-75 %.

## Progress log
- 21:15 UMD tlogs of the two gameplay runs (traces/g21/malikm-KairoGames.exe-{8056 native,7852 720p}.txt):
  every "submit N" line: **17 allocations listed** per Render (slabs, 0102), Render mean 133-172 us (max ~1.5 ms).
  18 Renders/frame x ~150 us = **~2.7 ms of a 50-66 ms frame** (~4-5 %). App pacing: GPU wait mean 0.26-0.77 ms,
  present 0.1 ms, frame gaps 50-100 ms. => the submissions themselves are NOT the bottleneck; batching them would save
  at most a few %. Also seen: "own" BOs keep rising (3233 -> 3445 in ~7 s native; 8689 -> 9123 at 720p, ~100 MB) -
  separate question (Unity dynamic buffers? leak?).
  Note: the game's UMD is maliumd32.dll = x86 code, so the whole Mesa driver runs EMULATED in this process (Prism),
  like the game itself. Next: measure the share of each frame spent inside our UMD (all DDI entry points).
- 21:20 Mesa WIP (uncommitted) UmdEntryTimer: LOG_ENTRYPOINT() = scoped QPC timer (outermost DDI per thread), app pacing adds 'app frames N: frame X ms, inside the UMD Y ms/frame'. x86 build mali-umd-0104-x86 (1601b123) live in SysWOW64 (diagnostic).
- 21:25 native gameplay run with the timer (traces/g21/malikm-KairoGames.exe-3548.txt; the screen was off during the
  sample - KMD presents 0, DWM idle - so the game ran unthrottled by composition): "app frames: frame 50.3-50.6 ms,
  **inside the UMD 29.1-29.9 ms/frame (58-59 %)**". The game-side frame is dominated by OUR driver's CPU time
  (x86 Mesa under emulation in this WOW64 process), not by the 18 Renders (~2.7 ms) or GPU waits (~0.5 ms).
  Next: per-DDI breakdown of those 29 ms.
- 21:30 per-DDI breakdown (traces/g21/malikm-KairoGames.exe-2600.txt, native gameplay, screen off), per 120 frames:
  ResourceMap 139320 calls 1583 ms, DrawIndexed 69960 calls 1490 ms, _Present 120 calls ~130 ms, ResourceUnmap 139320
  calls 86 ms, QueryGetData 240 calls ~60 ms, Flush 120 calls ~51 ms, ClearRenderTargetView ~34 ms.
  Per frame: **1161 Maps = 13.2 ms (11.4 us each), 583 DrawIndexed = 12.4 ms (21 us each)** of the 29 ms in the UMD.
  A 2D sprite game: ~580 draws/frame, 2 buffer maps per draw (Unity dynamic VB/IB). Target 1: ResourceMap path.
- 21:33 per-ioctl counters (shim, malikm_ioctl_report; traces/g21/malikm-KairoGames.exe-6504.txt), per 120 frames:
  **WAIT_BO (0x41) 4389 calls 1551 ms** (~37/frame, ~353 us each = 12.9 ms/frame = nearly all of ResourceMap's 13.2),
  SUBMIT (0x40) 960 calls 110 ms (8/frame from the game itself, ~0.9 ms/frame; the rest of the "18" was DWM),
  MADVISE 8774 calls 3.5 ms, MAP/MMAP ~1 ms.
  ROOT CAUSE: malikm_drm.c WAIT_BO -> wddm_idle(): ignores the BO and timeout_ns, waits (<= 10 s) for ALL submitted GPU
  work + wdead_flush. Panfrost's per-BO busy checks (map of a buffer the GPU used, BO-cache fetch with timeout 0)
  become full CPU<->GPU syncs -> the GPU part of each frame adds to the frame instead of overlapping.
  Fix: per-BO last-use submission + completed-submission tracking; WAIT_BO waits only for that BO's submission,
  timeout 0 = non-blocking busy check (-EBUSY).
- 21:40 Fix written (Mesa WIP): wbo.use_seq stamped per batch BO at SUBMIT; ddi_state ring of 32 submission events (ev/ev_seq, last_event = newest), done_seq; wddm_wait_seq(seq, poll): done_seq shortcut, wait on the first ring event >= seq (in-order queue), timeout 0 -> WaitForSingleObject(0) / -EBUSY (Linux panfrost answer for a zero timeout), else <= 10 s; no event -> old wddm_idle; unbind closes the ring events.
- 21:40 RESULT with the fix (x86 27f737ef, traces/g21/malikm-KairoGames.exe-3504.txt, native gameplay, screen off),
  per 120 frames: WAIT_BO 4512 calls **5.7 ms** (was 1551), ResourceMap **195 ms** (was 1583) -> -11.6 ms/frame of CPU.
  Frame time unchanged at 50.3 ms: the GPU wait moved to _Present (SYNCOBJ_WAIT 0xc3, 120 calls ~1260 ms = 10.5 ms per
  frame; app pacing "wait mean 10.6 ms") - our Present flushes and waits for the whole frame on the GPU (DxgiFns,
  "the frame is finished first") - and the frame gaps (half <=50 ms, half 50-100, mean 50.3 in every screen-off run,
  before and after) = the game's own **20 fps cap**. The real test is native gameplay WITH the screen on (15 fps
  before, below the cap: DWM composition competes for the same cores). The screen went off and injected input did not
  wake it (wake.ps1 + Shift + ES_DISPLAY_REQUIRED, dump still black) -> pending the user.
- Commits: Mesa 9ad66c9 = 0104 (patches/0104-umd-frame-profile.patch: UmdEntryTimer, "app frames / app umd top / app
  ioctl top" lines), f091833 = 0105 (patches/0105-wait-bo-per-bo.patch: the WAIT_BO fix). The split was made by
  reversing the fix script, committing, re-applying (scratchpad g21_waitbo*.py); tree = the tested build.
- Next: ARM64 build (DWM, Edge and every 64-bit app do the same full waits in WAIT_BO - DWM benefits too, but a wrong
  per-BO wait there shows as corruption on the desktop: test with a reboot + the user's eyes); then consider the
  Present wait (10.5 ms CPU<->GPU serialisation per frame).
- Review of Mesa f091833 (0105):
  - timeouts: every panfrost_bo_wait caller passes 0 or INT64_MAX (rg pan_bo.c/pan_context.c/pan_resource.c) -> poll /
    <= 10 s covers all; uapi timeout is absolute but Mesa never passes other values. Mesa's panfrost_kmod_bo_wait treats
    -1 with ETIMEDOUT or EBUSY as "busy" (panfrost_kmod.c:440-444); MK_EBUSY 16 == CRT EBUSY 16 (crt_errno passes it).
  - ordering: a CPU event enqueued with SignalSynchronizationObject2 on the context fires after all earlier work of
    that context; waiting on the first ring event >= seq is therefore sufficient whatever the KMD's slot order
    (vertex/tiler vs fragment submits of one batch both stamp; the later seq wins).
  - seq bookkeeping: w_render stores wseq+1 only after a successful pfnRenderCb + signal; SUBMIT increments wseq right
    after, so they agree; a failed Render returns before both. No event (signal failed) -> old wddm_idle / -EBUSY.
  - scope unchanged vs before: own process's submissions only (opened BOs of other processes were never covered).
  - lifetime: 32 events per binding stay open (was 1); unbind now closes them (the old last_event leaked at unbind).
  - Nothing to fix. 0104: D3D11.cpp entry points carry no LOG_ENTRYPOINT (the D3D11 wrappers call timed D3D10
    functions; untimed ones make "inside the UMD" a lower bound).
- 21:45 SCREEN OUTAGE: from ~21:20 the scan-out stayed black; injected input (wake.ps1) and the user's power button
  did not wake it ("screen wont start on my end either"); it came back by itself after a while (user: "took a while.
  weird"). tools/phone/g21/wakeevents.ps1: no Display/dxgkrnl/Kernel-Power/DWM events, no TDR, DWM same process since
  boot (Responding). DWM tlog (traces/g21/malikm-dwm.exe-1360.txt): last primary re-creation = 720x1520 at ~20:58;
  the 2280x1080 mode sets my gamebench runs requested at 21:2x-21:3x ("test 0 -> 0") never re-created DWM primaries -
  the display had already timed out (~20 min after the last wake) and the requests queued. No long GPU waits logged.
  OPEN: slow wake from display-off, possibly with mode changes requested while off. Not explained yet.
