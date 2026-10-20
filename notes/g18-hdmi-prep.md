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
