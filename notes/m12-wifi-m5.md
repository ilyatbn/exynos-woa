# M12 WiFi milestone 5 — firmware resident for the device lifetime

Goal: after PrepareHardware the BCM4375 firmware keeps running with its rings and DMA buffer, owned by the
device context; a WDF timer -> passive work item drains the D2H control ring every 100 ms; 30 s after start
one more escan proves the firmware is still alive and usable. Teardown: D0Exit (stop poll, quiesce = ARM halt,
1 ms, BME off), ReleaseHardware (free buffer + state, unmap BAR0/BAR2, unmap the EP config window).

## Progress log
- 2026-10-12 code (1.0.5.0):
  - WlanDl: WLAN_BUS.Keep; when the callback sets it, WlanDlRun hands the chip over (C = NULL, no unmap).
    New WlanDlRelease + WlanBusSetReport (WlanDl logs through C->R, incl. the BAR0 window helper, so every
    runtime entry re-points it).
  - WlanMsg: escan extracted to EscanRun (used at M4 and for the runtime rescan). Resident hand-over at
    step 9 when Config->Resident && M3 OK && SCAN 0: Bus copied into M, S->Bus re-pointed, M->Config cleared
    (stack). Runtime API: WlanMsgPoll (TRAP check -> quiesce), WlanMsgRescan, WlanMsgQuiesce, WlanMsgFree
    (refuses while not quiesced: leak rather than free under live DMA).
  - WlanProbe: WLAN_MSG_CTX moved from the PrepareHardware stack into the device context; Cfg kept when
    resident; poll timer + work item (children of the device); D0Entry starts, D0Exit stops + quiesces,
    ReleaseHardware frees. Run log `C:\Windows\System32\LogFiles\Exynos9820Wlan-run.txt` (WLAN_REPORT.FileName).
  - Registry Resident (default 1), RescanSeconds (default 30).
  - Build s10e-artifacts\wifi-m5, sys 4C3FFC47….
- Hazard (watched, not closed): the unattended agent ends Windows with a hard reset (flushreset), so D0Exit
  does not run and the firmware is still bus-mastering at reset. Expected safe: the SoC reset takes down the
  PCIe RC (no link = no DMA) and WLAN_REG_ON (GPIO back to its reset level), and UEFI M1 power-cycles the chip.
  Check after the run: the next boot's UEFILOG is complete and `debug_history` shows no new reset reason.
- Run m5-1 (02:57, 1.0.5.0):
  - PrepareHardware: SCAN 0 (9 BSS), then "RESIDENT: firmware kept running (buffer 0xEFF5E000)". Run log:
    at t = 30 s the rescan with the resident firmware was accepted on the first attempt and returned BSS
    lines, so **the firmware stays alive and usable after PrepareHardware**.
  - Then the D2H ctrl ring stalled for good at lap-2 item 38: epoch 39 = its lap-1 value (lap 1: index i has
    epoch i + 1; lap 2 wants (254 + 64 + 38) % 253 = 103). The log repeated every poll until the reset.
  - The fw W index had passed 38, so the item content we read is stale. Re-reads 100 ms apart never change,
    which points at a stale cache line, not DMA latency.
  - Why lap 1 worked is open. _CCA 0 should give an uncached common buffer, but UEFI programs the PCIe
    sharability bits (Dsdt.asl P5 note).
- 1.0.5.1 experiment:
  - Parameters\D2hInvalidate (default 1): MDL over the D2H ctrl ring + KeFlushIoBuffers(Mdl, TRUE, TRUE)
    before each item read.
  - The "not synced" line is now logged once per item, with fw w/r indices and the raw 24 B, plus a "synced
    after N stalled polls" line.
  - Build wifi-m5b, sys 4DADE1BC….
