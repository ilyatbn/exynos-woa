# G18 - external display over USB-C (DP alt mode) under Windows

Goal (user 2026-10-16): HDMI out through the USB-C to HDMI dongle and the UGREEN hub. Prior research (do not
re-derive): notes/d6-external-display.md (vendor enable chain P2/P3b, verdict P7, hub identified as DP alt mode:
Genesys GL352 hub + ASIX AX88179B, no DisplayLink). Path: max77705 CCIC PD alt-mode entry -> combo PHY DP lanes
(shared with USB3, PMU 0x15860704 + lane mux; boot hazard for USB host) -> DP AUX/DPCD/EDID -> link training ->
DECON_T (0x19050000, id 2, out_type DP) video timing -> second VidPN target in MaliG76Wddm (clone needs Offset270,
panel is portrait-first).

## Rungs (each one commit, each ends in a read-back on the device)
- H1 (read-only) CCIC status from Windows: the max77705 USBC/PD block over HSI2C (same bus as S10eBattery's FG 0x36
  / charger 0x69; USBC client address from the vendor DTS - UNVERIFIED until read). Read attach/orientation/alt-mode
  status with nothing plugged, then with the hub + HDMI monitor plugged (user action).
- H2 alt-mode entry: the vendor's max77705_alternate.c opcode/VDM sequence (DP discover modes, configure pin
  assignment D/C), forward HPD. Write access to the PD chip -> review VBUS/charging side effects first.
- H3 DP AUX: aux switch GPIOs (gpg1-6 OE active low, gpg4-0 SBU sel, gpg0-4 PHY_CON strap read), combo-PHY DP lane
  power, AUX channel transactions -> DPCD caps + EDID over I2C-over-AUX. Read-back = EDID bytes.
- H4 link training + DECON_T test pattern (DP video stream from a fixed-color DECON_T window).
- H5 WDDM: second video present source/target in MaliG76Wddm (EDID modes, hot plug from HPD), clone + extend.

## Progress log
- 2026-10-17 02:30 plan written. The user must plug the hub + monitor for H1's second read (ping on the app then).
- 02:50 H1 DONE: S10eBattery 1.0.0.5 (sys 6578C452) reads the max77705 USB-C block (7-bit 0x25 = [K] 0x4A >> 1,
  max77705-private.h) every poll, read-only, revs 0x00-0x01 + status 0x06-0x0D (interrupt latches 0x02-0x05 skipped:
  clear-on-read). Device Parameters UsbcRev/UsbcSt0/UsbcSt1; decoder tools/phone/wddm2/usbc.ps1 (bit fields from
  include/linux/ccic/max77705.h). First read, phone on the PC's USB cable: UsbcRev 0x1A1A (HW 0x1A, FW 0x1A), BC_STATUS
  0x81 = VBUS present, ChgTyp 1 (SDP); CC_STATUS0 0x51 = SINK on CC1; CC_STATUS1 0x08 (ConnStat 0, Altmode 0);
  PD_STATUS0 0x19, PD_STATUS1 0x47 = UFP, PSRDY 0. Opcode interface for H2: OPCODE_WRITE 0x21, OPCODE_READ 0x51,
  32 data bytes ([K] max77705.h). Next: same read with the hub + HDMI monitor plugged (user).
- 2026-10-18 H1b: S10eBattery 1.0.0.6 (sys 97DAA0E5, s10e-artifacts\s10ebattery-1.0.0.6, pnputil live install -> oem102.inf)
  adds IOCTL_S10E_USBC_READ (private IOCTL on the battery device, handled before BatteryClassIoctl; up to 128 bytes of
  the USB-C block in 16-byte FIFO chunks under BusLock; read-only). tools/phone/usbc/usbcwatch (new): polls 0x00-0x7F
  every 100 ms and logs each register change. Smoke run (PC cable only): 50 reads / 5 s, 0 failed. Map seen:
  0x00-0x0D status (1A 1A 00 00 00 00 27 31 81 10 51 08 19 47), 0x21.. opcode write buffer (21=1C 22=1F - last opcode
  sent by sboot/Android before Windows), 0x51.. opcode read buffer (51=1C 52=65). Battery still reports (class OK).
  USER TEST (H1 second read): `C:\S10eTmp\usbcwatch.exe 120 100 > C:\S10eTmp\usbc-hub.txt`, then plug the hub + HDMI
  monitor within the 2 minutes (see tools/phone/usbc/README.md).
- 2026-10-18 H2 SOURCE ONLY (S10eBattery 1.0.0.7 in the tree, NOT built, NOT deployed): IOCTL_S10E_USBC_OPCODE
  (allowlist: 0x55 data 00/01/03, 0x48 only the Identity / Enter Mode / DP Configure payloads from
  notes/g18-h2-altmode-research.md section H, 0x4B any step; write from 0x21 in FIFO chunks, commit 0x00 at 0x41,
  poll UIC_INT b7 3 s, reply from 0x51) + IOCTL_S10E_USBC_WRITE_REG (INT masks 0x0E-0x11 only). The package build
  was refused by the session's auto-mode safety classifier (writes to the USB-C power-delivery controller) - left
  for the user to decide; nothing was sent to the max77705. The phone still runs the read-only 1.0.0.6.
