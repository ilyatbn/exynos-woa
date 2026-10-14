# Touch quality (double taps, jerky drag) - analysis

User report 2026-10-14: "screen a bit more responsive, still sometimes double taps, dragging icons kinda annoying, needs more smoothness".
Scope: analysis only; proposed patch in notes/touch-quality.patch; no commit, no deploy.

## Progress log
- 2026-10-14 started. Driver in use: drivers/S6SY761TouchBeyond0 (Device.c 3282 lines). Older: drivers/S6SY761Touch.
- Source read (Device.c, Beyond0 copy):
  - Interrupt-driven, NOT polled: WdfInterruptCreate PassiveHandling (Device.c ~1613), ALIVE EINT gpa1-0 level-low,
    filter 0xC0 (Device.c:1396-1425). ISR Device.c:2846-3016: READ_ONE_EVENT 0x60 (8 B), remain = ev[7]&0x3f, then
    READ_ALL_EVENTS 0x61 for remain*8 B (same as mainline s6sy761.c). Ack EINT pending after processing.
  - Every I2C error and every "remain>63" overflow calls TouchQueueAllUpReport (Device.c:2896,2905,2922) = synthetic
    tip-up of every active contact; the next MOVE of the same finger then becomes a fresh PRESS (contact->Active FALSE
    -> isMove FALSE, Device.c:2978) => a tap the user never made (double-tap candidate #1). Counters only written on
    D0Exit (Device.c:1797-1820) -> TouchI2cErrorCount/TouchOverflowCount/TouchQueueFlushCount in Device Parameters.
  - Release handling: only drops a contact on TS_RELEASE(3); TS_NONE ignored; status events (EID 1) ignored entirely.
  - Report: one HID report (0x54) per controller event (TouchQueueCurrentReportLocked per event), each report carries
    ALL active contacts + release-pending. Consecutive move-only reports coalesce only while queued (backpressure).
  - Scan time = KeQueryInterruptTime()/1000 (100 us units, 16-bit, matches descriptor UNIT_EXPONENT -4 s) taken when
    the report is BUILT in the ISR (Device.c:198), i.e. after the I2C read, not at controller sample time.
  - HoldTimer (S6SY761_HOLD_REPEAT_MS 20, Device.h:107) repeats the current report every 20 ms while a contact is
    down and the queue is empty (fix for press-and-hold, m12-touch.md).
  - Descriptor (Device.c:745-1056): 10 finger collections: Tip Switch, Contact ID (max 9), X, Y. NO Confidence (0x47),
    NO In Range (0x32), no Width/Height. Logical X/Y max patched to 4095 at EvtDeviceAdd (Device.c:1281-1291,
    "Y761 reports unscaled 12-bit coordinates"). Physical max still S9+ 2.71 x 5.58 in (Device.c:720-740 comment).
    Contact Count Max = 10 via feature report 0x54 (features = {0x54,10}).
  - D0Entry sends no command to the controller (no SENSE_ON, no report-rate, no reset); controller runs in whatever
    mode sboot/Android left it.
- Evidence: s10e-artifacts/run-m16-3/touchlog.txt (pre-HoldTimer driver 6.0.5058.4, 3.7 s hold + drags):
  - scan-time deltas between consecutive reports are exact multiples of 156 (15.6 ms) and two different reports
    carry the SAME scan time (7234/7250: both "scan 50492", positions differ). => KeQueryInterruptTime has clock-tick
    (15.625 ms) resolution; scan time is quantised to 15.6 ms and can repeat. (Device.c:198)
  - Windows re-created the pointer (PTR UP + new PTR DOWN = a tap) whenever the gap between reports was >~100-200 ms
    (e.g. 7265 -> 7406 gap 141 ms: UP/DOWN at the next report). The HoldTimer (20 ms) was the fix; any stall of the
    repeat path > ~100 ms (timer runs on a passive work item; device is CPU-slow) re-opens this.
  - Controller event spacing while moving: 15-16 ms and frequently 31/47/62/109 ms (slow drag) => controller only
    reports on change; the 4 px X steps at 2685..2735 are raw 12-bit units.
- parkinglot/touch-double-tap.md already lists suspects (repeat path, panel bounce, coalescing).
- Downstream S10e driver fetched (LineageOS lineage-22.2, CONFIG_TOUCHSCREEN_SEC_TS_Y771=y in beyond0lte defconfig):
  scratchpad touchsrc/y771_sec_ts.{c,h}. Findings:
  - y771_sec_ts.h:591-607 event layout: byte7 = left_event:5 | max_energy_flag:1 (bit5) | ttype_1_0:2;
    byte6 = z:6 | ttype_3_2:2. y771_sec_ts.c:1120 `left_event_count = buf[7] & 0x1F`, MAX_EVENT_COUNT 31.
    OUR DRIVER: S6SY761_MASK_LEFT_EVENTS 0x3f (Device.c:76) => bit5 max_energy_flag is counted as 32 extra events:
    reads 32 extra 8-byte slots over I2C (+256 B per IRQ) and processes any non-zero stale slot as a real event.
    Also the "remain > 63" overflow check (Device.c:2901) can never fire (value is masked to <= 63).
    Mainline s6sy761.c (S9 firmware): mask 0x3f with EVENT_COUNT 32 -> bit5 set => "overflow" => release-all.
    README "Hold an entire 64-event burst without the old 32-report overflow reset" = the S9 fork widened the
    count instead of masking the flag.
  - ttype = (buf[6]>>6)<<2 | (buf[7]>>6). sec_ts reports only NORMAL(0)/PALM(5)/WET(6)/GLOVE(3) coordinate events
    (y771_sec_ts.c ~1263); HOVER(1)/FLIPCOVER/STYLUS/PROXIMITY(7)/JIG are not finger touches. Our driver reports all.
  - sec_ts on I2C read error: just returns (no release-all). Release-all only on status events: QUEUE_FULL error,
    boot-complete-after-watchdog (+ reinit + SENSE_ON), and its own overflow (>30). Our driver ignores status events
    and releases all on every I2C error.
  - sec_ts syncs one frame per IRQ (input_sync after the loop, ~line 1521); our driver emits one HID report per event.
  - sync-reportrate-120 is only an Android feature flag (sec_ts_fn.c:1494), no controller command; report rate is
    the firmware default.
- Phone (ssh s10e, read-only): ACPI\SCSY0761\0 Started, oem3.inf = s6sy761touchbeyond0.inf 6.0.5058.5; DriverStore
  sys SHA256 63ad6ab5...2529 = the HoldTimer build (m12-touch.md) = repo HEAD source (last touch commit 283d5b6).
  Device Parameters: XMax/YMax 0xfff, TouchStartStage 0x1d; NO TouchIrqCount/TouchI2cErrorCount etc. (written only in
  D0Exit, which has never run with this build) -> error/flush counters are currently unobservable.
  Wisp\Touch (ssh user): TouchModeN_DtapDist 50, DtapTime 50, hold 1. 6 logical CPUs. Bus: DSDT I2C 400 kHz
  (Beyond0LtePkg Dsdt.asl:277-281) => +256 B read when bit5 set costs ~5.8 ms per IRQ.
- Phone, HSI2C bus telemetry (ACPI\SAMS9820\a\Device Parameters, boot 2026-10-14 13:16:54Z, read 13:33Z):
  I2cLastRequestStatus 0xC0000120 (STATUS_CANCELLED), I2cSequence 0x3c, I2cCompletionCount 0x92a (2346),
  I2cIsrCount 0x4a2d (18989), I2cWatchdogCount 0 (HSI2C own timeout 1000 ms, exynos9820hsi2c.h:67).
  Telemetry is republished only on the 1st completion and on every FAILED request (device.cpp:1359-1366);
  sequence is a seqlock (+2 per capture/status update, controller.cpp:761-841) => ~25 failed requests this boot
  (estimate: (60 - first-ISR/first-completion captures)/2; exact count not stored). The only SPB client is the touch
  driver; it sends with WDF timeout S6SY761_SPB_TIMEOUT_MS 250 (Device.c:79, SpbDeviceWrite/WriteRead) and a WDF
  send timeout completes as STATUS_CANCELLED => transfers stalled 250..1000 ms ~25 times in <=17 min.
  Each such failure in the ISR => TouchQueueAllUpReport (Device.c:2896/2922) = synthetic tip-up of the held finger,
  then the next controller event = new contact (new pointer id) => spurious second tap / dropped drag.
  ~8 HSI2C ISRs per request (18989/2346) - consistent with large FIFO-threshold reads (see 0x3f mask finding).
  Root cause of the >250 ms stalls is NOT known (lost HSI2C IRQ? starvation? clock stretching - DT has
  scl-clk-stretching). Needs a per-request latency histogram (proposed counters in the patch).
- MS docs read (learn.microsoft.com touchscreen-required-hid-top-level-collections, touchscreen-packet-reporting-modes):
  mandatory = Contact ID, X, Y, Tip, Contact Count, Report ID; Scan Time optional but "deltas should reflect the
  scanning frequency", 100 us units, same for all contacts of a frame; Confidence optional ("if your device always
  rejects accidental touches you need not include it"); physical range "must be accurately reported"; release must
  repeat the last tip-down X/Y with the same contact ID (driver does: release path keeps X/Y, Device.c:2966-2975).
  Deviations: (a) physical extents are the S9+'s 2.71 x 5.58 in (UNVERIFIED for S10e), (b) scan time quantised to
  15.6 ms with duplicates, (c) no palm rejection / Confidence while the controller flags PALM ttype. Parallel mode,
  contact count, CCM feature (10), IDs: compliant.
- Plan for patch (notes/touch-quality.patch): (1) left-event mask 0x1F + overflow > 30 (sec_ts); (2) precise,
  strictly increasing scan time; (3) no synthetic all-up on SPB errors - deferred stale-contact release instead;
  (4) drop PRESS/MOVE of non-finger ttypes for contacts not yet down; (5) live diagnostics: SPB latency max/slow
  count, error status, counters written to Device Parameters at error time.
- Patch written: notes/touch-quality.patch (Device.c + Device.h of S6SY761TouchBeyond0; 210+/45-).
  `git apply --check notes/touch-quality.patch` = OK on HEAD b09c83b. Compile check (scratch copy, NOT the repo
  tree, NOT deployed): MSBuild Release|ARM64 /p:VisualStudioVersion=17.0 SkipPackageVerification ApiValidator off,
  Rebuild -> 0 warnings / 0 errors under the project's /WX. No INF DriverVer bump in the patch (do it when packaging).
  Contents: (1) S6SY761_MASK_LEFT_EVENTS 0x1f, S6SY761_MAX_EVENT_COUNT 31, overflow at > 30 (sec_ts); counter
  TouchMaxEnergyFlagCount for bit5. (2) TouchInitializeReport: KeQueryInterruptTimePrecise, never repeats a scan
  time (+1). (3) SPB error in ISR -> TouchNoteSpbError (no all-up); HoldTimer releases only contacts with no event
  for S6SY761_ERROR_STALE_MS 300 (tuning value, labelled as such) after the error; counter TouchStaleReleaseCount.
  (4) PRESS/MOVE with ttype not NORMAL/GLOVE/PALM/WET cannot start a contact (TouchNonFingerCount); RELEASE always
  honoured. (5) SPB latency: TouchSpbMaxMs, TouchSpbSlowCount (>20 ms), TouchSpbLastError; all counters written to
  Device Parameters on every SPB error / new slowest transfer and at D0Exit (TouchRecordDiagnostics).
- Self-review of the patch (adversarial):
  - Overflow branch still does all-up + CLEAR_EVENT_STACK (= sec_ts behaviour) - intentional.
  - Stale check compares coarse KeQueryInterruptTime on both sides; an event in the same 15.6 ms tick as the error
    counts as stale (conservative: may release a still finger that would otherwise have continued).
  - A still finger during an SPB stall is still lifted (after 300 ms instead of at once) - trade-off vs stuck touch.
  - Registry writes happen in the passive ISR only on errors / new max latency (rare) - acceptable, but check
    TouchSpbMaxMs is not climbing every IRQ in the first run.
  - Not addressed (needs data first): the cause of the >250 ms SPB stalls (HSI2C side), physical extents
    (S9+ 2.71x5.58 in, UNVERIFIED for S10e), Confidence/palm usage (descriptor change shifts the byte offsets that
    EvtDeviceAdd patches at 40/41/57/58 + 58*n - must be redone together), one-report-per-event vs per-frame.
  - Bus clock: DSDT says 400 kHz; Linux DT hsi2c@104B0000 clock-frequency NOT recorded - UNVERIFIED; closing read in
    TWRP: `xxd /proc/device-tree/hsi2c@104B0000/clock-frequency`.
- Validation plan (device): baseline first with 6.0.5058.5: read HSI2C I2cSequence before/after 2 min of tapping +
  dragging (growth = failed SPB requests), s10etouchlog 20 single taps (count PTR DOWN per physical tap) + slow icon
  drag. Then patched build: same script; read ACPI\SCSY0761\0\Device Parameters Touch* counters:
  TouchMaxEnergyFlagCount>0 confirms the 0x3f bug was live; TouchI2cErrorCount/TouchSpbMaxMs/TouchSpbLastError
  quantify stalls; TouchStaleReleaseCount = lifts we still force; touchlog scan deltas no longer multiples of 156
  and never 0; PTR DOWN count == tap count.

## Applied (main session, 2026-10-14 ~18:35) - touch 6.0.5058.6
- notes/touch-quality.patch applied as reviewed, with one change: the non-finger ttype filter is COUNT-ONLY
  (TouchNonFingerCount + TouchLastNonFingerKind) - the ttype bit layout is from sec_ts y771 source, not seen on the
  device; a wrong layout with the drop enabled would kill every touch. Enable after the counters look right.
- INF DriverVer 6.0.5058.6; built s10e-artifacts\touch-5058-6 (sys sha256 08012fdb3f2e...). Deploy: SYSTEM swap of the
  DriverStore .sys (previous kept), reboot.
