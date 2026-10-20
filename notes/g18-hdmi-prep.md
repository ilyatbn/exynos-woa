# G18 prep - HDMI over the USB-C adapter / the UGREEN hub (DP alt mode), done unattended

User 2026-10-20: "When thats done do as much prep work as possible on hdmi (via type c adapter and usb hub, i have
both)." Prior work (do not re-derive): notes/d6-external-display.md (vendor chain P2/P3b, verdict P7),
notes/g18-hdmi-dp-alt.md (rungs H1-H5, H1 done read-only), notes/g18-h2-altmode-research.md (H2 opcode sequence).

## Gates (what needs the user)
- Plugging the adapter / hub + a monitor (any H1-H4 plugged read).
- Writes to the max77705 PD controller (H2 alt-mode entry): deferred to the user on 2026-10-18 (build refused by the
  session's auto-mode safety classifier). Sending opcodes stays user-gated here; everything up to it is prepared.

## Prep tasks (one commit each)
- P1 build + review S10eBattery 1.0.0.7 (H2 opcode IOCTL with allowlist) - package only, not deployed.
- P2 PC/phone test tool for H2 (sequence of section H, logs every reply), dry-run mode without the IOCTL.
- P3 DP controller bring-up sequence (AUX init, DPCD + EDID over AUX, link training, DECON_T video) from the vendor
  displayport_reg.c / displayport_drv.c, constants UNVERIFIED unless in Exynos9820.h.
- P4 WDDM second target design (MaliG76Wddm: child relations, hot plug from the CCIC HPD, EDID, VidPN with two
  sources, clone Offset270) with MS Learn quotes.
- P5 the user test script (what to plug when, what is read, how to roll back).

## Progress log
- P1 (2026-10-20): S10eBattery 1.0.0.7 BUILT (build-package.ps1 -Out s10e-artifacts\s10ebattery-1.0.0.7: sys
  97026BEE, inf 50EB6A19, cat 3A7AD128). Code review of the H2 paths (Driver.c S10eUsbcOpcodeAllowed / OpcodeIoctl /
  WriteRegIoctl) against g18-h2-altmode-research.md section H: allowlist = 0x55 data 00/01/03, 0x4B any one-byte
  step, 0x48 only the Identity / Enter Mode / DP Configure (one-hot pin A-F) payloads; register writes only the INT
  masks 0x0E-0x11; FILE_WRITE_ACCESS; one opcode in flight under BusLock; 3 s reply budget with the lock released
  between 5 ms polls; every planned payload <= 10 bytes = one HSI2C burst to 0x21 (vendor writes one burst). No
  finding. Files copied to the phone (C:\S10eTmp\batt107). The live install (pnputil /add-driver /install) was
  DENIED by the session's auto-mode safety classifier -> left for the user, like the 2026-10-18 build refusal.
  The phone still runs the read-only 1.0.0.6.
- P2 (2026-10-20): tools/phone/usbc/usbcaltmode.c + build-usbcaltmode.cmd (arm64 default, x64 for a PC dry run).
  Implements research section H: status/mask read, stale-latch drop, 0x55 0x03 + Altmode check, Identity only when
  SINK + DFP (section F), then a 50 ms poll of the clear-on-read latches 0x02-0x05 with the section-D mapping
  (VDM_INT 0x05 b0-b3 = ID / SVIDs / Modes / Enter, PD_INT 0x04 b2-b4 = DP status / configure / attention): reads each
  response with 0x4B, sends Enter Mode after Modes (UFP_D capable + pins), DP Configure after a connected DP status
  (pin by the vendor preference order), logs HPD; detach resets; -stop sends 0x55 0x01. Every reply byte is printed.
  PC dry run (x64 build, "-dry"): payloads 55[03], 48[09 01 80 00 FF], 48[09 04 81 01 FF], 48[0A 11 81 01 FF 06 PP
  00 00] = exactly the driver allowlist. ARM64 build OK (/W4 /WX). Not run on the phone (needs 1.0.0.7 installed +
  the user's go for PD writes).
- Review of P2: payloads mechanically equal to the allowlist (dry run); latch handling: the driver's opcode path reads
  only 0x02 (UIC) so PD/VDM latches survive an opcode and are seen on the next poll. GAP (follow-up when the first real
  run shows it): the vendor resends DP Configure when the 0x48 reply is an Attention while the last request was
  Configure (usbc.c:1970-1987) - usbcaltmode only logs the reply. Not fixed blind: the first plugged run shows whether
  it happens.
- P4 (2026-10-20): notes/g18-p4-wddm-target.md - second-target design for MaliG76Wddm (today: 1 source / 1 child,
  source 0 / target 0 hard-coded in display.c). MS Learn quotes for StartDevice NumberOfChildren (potential children
  counted from the start) and DxgkCbIndicateChildStatus (HpdAwarenessInterruptible, StatusConnection, IRQL <=
  DISPATCH). Plan: 2 sources / 2 children (UID 1 = DISPLAYPORT_EXTERNAL, interruptible HPD), per-path VidPN,
  P4b always-disconnected child (validatable now), P4c diagnosis-only phantom monitor (validates multi-source VidPN
  without DP hardware), P4d real HPD/EDID/DECON_T, then clone (Offset270). Enum checked in d3dkmdt.h.
- P3 delegated to a background research agent (notes/g18-p3-dp-bringup.md, in progress).
- P4b code (2026-10-20, uncommitted until validated): MaliG76Wddm full adapter 2 sources / 2 children; child UID 1 =
  D3DKMDT_VOT_DISPLAYPORT_EXTERNAL, HpdAwarenessInterruptible, QueryChildStatus FALSE, no descriptor; IsSupportedVidPn
  refuses any path other than 0 -> 0 (TopologyPanelOnly); Cofunc leaves non-panel paths alone; display-only 1/1.
  Power handler acts on UID 0 only (adapter.c MwSetPowerState) - child 1 power calls are no-ops. Built (MwBuild 1),
  test-signed sys d2eb6b95 (s10e-artifacts\maliwddm-p4b), kmdswap into malig76wddm.inf_arm64_6557c1d415c2ef25 (= the
  service's BINARY_PATH_NAME). Rollback: TWRP rename MaliG76Wddm.sys.prev back. Reboot next.
- P4b VALIDATED (reboot 19:36): DriverStore sys = d2eb6b95 (loaded), adapter Started, dwm/explorer up, desktop normal
  (traces/g18-p4b-desk.png), one active monitor (WmiMonitorConnectionParams: DISPLAY\EXW9820\...UID0 only), no new
  monitor device for UID 1 (disconnected as intended), KMD DDI trace shows QueryChildRelations calls; no Code 43.
  (An old "Generic Monitor (S2719DGF)" devnode in state Unknown predates this change.) INF version not bumped - a
  sys-only swap; the next package build carries it.
- Review of P4b: FINDING - with two sources, the per-source / per-path DDIs ignored the ids: SetVidPnSourceVisibility
  (a source-1 "invisible" would zero the PANEL fb), SetVidPnSourceAddress (a source-1 flip would move the panel),
  CommitVidPn (a commit scoped to source 1 re-evaluated source 0 and could set PathActive FALSE),
  RecommendMonitorModes (would give a target-1 monitor the panel's mode), UpdateActiveVidPnPresentPath (target 1's
  rotation would become the panel's). None had fired (Windows creates no path to a disconnected target) but any
  reconfiguration touching source 1 would. Fixed (own commit): early returns for source / target != 0
  (Commit: AffectedVidPnSourceId other than 0 and D3DDDI_ID_ALL). sys 1f72350c (s10e-artifacts\maliwddm-p4b2),
  kmdswap + reboot 19:42: adapter Started, desktop normal (traces/g18-p4b2-desk.png), Edge hardware video 23.5 fps
  dropped 0 (traces/g35-edge-hw3.png). QueryVidPnHWCapability needs no guard (PinnedSourceKind of source 1 = 0).
- P5 (2026-10-20): user procedure "User test 2" in tools/phone/usbc/README.md (decision, install, plug before boot
  for VBUS, run usbcaltmode 90, expected lines, hub repeat, reboot unplugged afterwards, rollback). usbcaltmode.exe
  staged at C:\S10eTmp (copy only, not run). The monitor stays dark in that test by design (no DP driver yet).
- P4c deferred with a checklist (notes/g18-p4-wddm-target.md "P4c checklist"): per-target vsync + per-source flip
  latch are needed first, best built with P4d when DECON_T exists; not done unattended.
- P3 (2026-10-20, background research agent, vendor source lineage-23.2, every claim file:line, constants
  UNVERIFIED): notes/g18-p3-dp-bringup.md. Key corrections to the brief: the DP link block 0x130B0000 is in BLK_FSYS0
  (CMU_FSYS0 0x13000000 gates), not the DPU; AUX orientation = the gpg0-4 level (gpg4-0 := gpg0-4), GPIO banks shared
  with codec reset / WLAN_EN -> read-modify-write only; configuring DECON_T writes DECON_F registers
  (DP_CONNECTION_CONTROL 0x0270, window regs) -> a mistake hits the live panel. Biggest hazard: DP lanes need the
  combo PHY powered by the USB side, which our firmware parks (USB PLL never locked) -> touching it under the running
  xHCI is a BOOT HAZARD. Useful: the DP controller's own BIST (solid colour / bars) gives first light without
  DECON_T/DPP/SYSMMU (rung H4a). 19-step order, steps 1-7 read-only probes; 7 open questions listed there.
  Parent review: structure checked (8 sections + open questions), Exynos9820.h has no DP / PMU 0x704 define (grep) ->
  UNVERIFIED tags correct; the PHY-overlap claim vs notes/m16-ss-phy.md not re-derived here (re-check at the PHY step).

## 2026-10-20 ~20:30 - P4b REVERTED (user: display "lagging like crazy", "screen draws itself left to right")
- Measured with the P4b KMD (1f72350c) after hours of uptime: software vsync 59/s while idle but 24-33/s while DWM
  presents (KMD heartbeat), DWM 3-5 presents/s, Mali 3D engine 0.15 % busy (DWM waiting), CPU idle -> governor kept
  G3D at 260 MHz (its GpuBusyPct follows the 3D engine counter). Edge (left looping the test clip) dropped nearly all
  frames. Not the cause: touch boost never worked (touch 6.0.5058.7 with the S10eTouchActivity event was never
  installed - TouchEvent 0 / Touches 0; tools/phone/power/touchstate.ps1 new).
- Restored the previous live KMD d69c01cd (s10e-artifacts\mwddm-g32-defer, G32 DeferPresent) by kmdswap + reboot
  20:39: vsync 59/s also while presenting, user: "works fine now".
- Build check (offline): commit 7ee013d built with the same msbuild command = .text / PAGE / INIT / .data / .reloc
  byte-identical to d69c01cd (only .rdata / .pdata debug-directory bytes differ) -> the build method is not the cause;
  the P4b source change (2 sources / 2 children, second child disconnected) is.
- My P4b validation was too short (one screenshot + one Edge run right after boot); the regression shows under use /
  over time. Repo KMD sources restored to 7ee013d (= live). Root cause of the 2-source slowdown NOT known yet;
  P4b redo only with a measured soak (vsync rate while presenting, presents/s, 30+ min of use) and the user's go.
- 20:50 root-cause A/B (user: "do it"), tools/phone/g18/pacingab.sh (new: swaprate.ps1 = d3dswap composition + window
  swap chains 300 frames, + KMD heartbeat over the load and a 30 s idle window):
  - A d69c01cd (1 source): composition 59.0 / 59.5, window 59.5 / 59.1 presents/s; vsync 58.8/s load and idle.
  - B 1f72350c (P4b2, 2 sources) FRESH BOOT 20:54: 58.5 / 59.2, 59.3 / 59.4; vsync ~59/s -> no difference at boot.
  - B + Edge looping the H.264 clip (hardware decode) for 1 min: 59.4 / 59.3, vsync 58.8/s -> still fine.
  - => the regression needs time. Soak running: pacingsoak.sh (new) every 5 min for 45 min with Edge looping, stops
    at the first composition rate < 45/s. The live d69c01cd is kept at C:\S10eTmp\kmd-d69c01cd.sys for the rollback.
- Soak B (1f72350c, Edge looping): rounds 21:03 / 21:09 / 21:14 / 21:19 all 59/s, vsync 58.8 -> no slow drift.
- 21:2x USER refreshed the video page (F5) on B -> bad again. Measured: swap-chain presents 12.2/s and 3.2/s (stalls
  up to 5.1 s), idle vsync ~15/s; Edge's GPU process had restarted at the refresh (start 22 min after the browser),
  no crash dump (not a crash - Chromium replaces a hung GPU process); MFC held by the new GPU process.
- Same scenario on A (d69c01cd, rolled back 21:25; tools/phone/g35/browserrefresh.ps1 new = F5 to the Edge window):
  baseline 58.1 / 58.9; play 1 min + F5 -> 59.3 / 59.2, vsync 58.8; two more F5 (page counter restarted 3223 -> 621,
  reloads real) -> 59.3 / 59.5; Edge GPU process never restarted.
- => the failure needs BOTH the second (disconnected) video source AND a video decoder teardown/re-create in Edge.
  Working hypothesis (UNVERIFIED): something waits for a vblank on source/target 1 - our software vsync only ever
  reports target 0 (command.c SyncVsync) - e.g. a vblank wait in the GPU process during the page teardown never returns,
  Chromium's watchdog replaces the GPU process, and DWM's composition stalls behind it. Next experiment (with the user's
  go): P4b + a vsync notification for target 1 as well, repeat the F5 test. Also asked by the user: a driver-only
  restart (pnputil /restart-device) at the next reproduction, to see if it clears the state without a reboot.
- CORRECTION (user): "its not about the refresh. after a while the video went black so i refreshed the video and then
  it started dropping." -> on B the video went black by itself after 20+ min (my soak only checked DWM pacing, not
  the video); the refresh came after. The A refresh test (minutes) proves nothing about the long run. The conclusion
  "needs a decoder teardown" above is withdrawn. Fair test now: the same long Edge run on A (d69c01cd) with the page's
  frame counter + GPU-process pid + pacing every 3 min (tools/phone/g35/videosoak.sh + browsertitle.ps1, new),
  traces/g35-vsoak-A-d69c.txt.
