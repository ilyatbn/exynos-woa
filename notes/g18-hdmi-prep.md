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
