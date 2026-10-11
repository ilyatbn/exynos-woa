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
