# Parked: touch registers a double click on a single tap / hold (user, 2026-10-12)

Symptom: one tap (or the start of a press-and-hold) sometimes arrives in Windows as two clicks.

Suspects (driver drivers/S6SY761TouchBeyond0, v6.0.5058.5; notes/m12-touch.md):
1. The 20 ms held-contact repeat (commit 283d5b6): if the repeated report ever goes out with Tip Switch = 0 or a
   new contact ID, Windows sees up/down = a second click. Check TouchQueueCurrentReportLocked(…, TRUE) keeps
   TipSwitch=1 and the same ContactID; check the repeat cannot run after the panel already reported the release
   (race between OnInterruptIsr marking the contact inactive and TouchHoldTimerFunc reading it).
2. Panel contact bounce: the S6SY761 may report LEAVE then ENTER within a few ms for a light touch (no debounce
   in the driver). Evidence: s10etouchlog.exe (desktop "Touch log") logs raw reports + gaps per contact.
3. Report coalescing in the input queue (InputReportCoalesceCount) emitting a stale up between two downs.
Next step: capture a s10etouchlog run with single taps that double, then fix in the driver (debounce a release
followed by a re-touch < ~30 ms at the same position, or fix the repeat path).
