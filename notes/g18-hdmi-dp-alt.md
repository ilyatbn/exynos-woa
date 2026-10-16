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
