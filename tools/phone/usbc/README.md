# USB-C / HDMI test tools (G18, notes/g18-hdmi-dp-alt.md)

`usbcwatch.exe [SECONDS] [INTERVAL_MS] [FIRST] [COUNT]` - read-only watch of the max77705 USB-C/PD controller
through S10eBattery 1.0.0.6+ (prints a full register dump, then every change with a timestamp).

## User test: what happens when the hub / HDMI dongle is plugged (no risk - nothing is written)
1. On the phone, in an admin terminal (or from the PC over ssh):
   `C:\S10eTmp\usbcwatch.exe 120 100 > C:\S10eTmp\usbc-hub.txt`
2. Within the 2 minutes: unplug the PC cable, plug the USB-C hub with the HDMI monitor attached and powered,
   wait ~20 s, unplug it, plug it in again with the hub's power (PD) input connected if it has one.
3. Leave `C:\S10eTmp\usbc-hub.txt` on the phone - it shows the attach, the role (DFP), and whether the controller
   starts USB-PD / alt-mode traffic (VDM messages) by itself.

`usbcaltmode.exe -dry | [SECONDS] [-stop]` - G18 H2 DP alt-mode entry (notes/g18-hdmi-prep.md P2): sends only the
allowlisted max77705 opcodes (alt mode on/off, Discover Identity, Enter Mode, DP Configure, read VDM responses) through
S10eBattery 1.0.0.7 and logs every reply. `-dry` prints the plan without opening anything.

## User test 2: DP alt mode with the USB-C HDMI adapter, then the hub (writes to the PD controller - user's go needed)
What it proves: the adapter/hub is a DP alt-mode device, the phone enters DP mode, the monitor's HPD reaches us.
The monitor stays dark in this test - the DP controller / DECON_T driver does not exist yet (P3/P4d).
0. Decide: allow the H2 writes (only the opcodes above; no VBUS / role / charging opcode is in the driver's
   allowlist, notes/g18-h2-altmode-research.md section G).
1. Install the driver once (admin): `pnputil /add-driver C:\S10eTmp\batt107\S10eBattery.inf /install`
   (S10eBattery 1.0.0.7, sys 97026BEE; reads stay the same as 1.0.0.6).
2. Power the phone off. Plug the USB-C to HDMI adapter with the monitor attached and on. Power on into Windows
   (the firmware turns on the 5 V boost at power-on when a sink is attached; plugged later, a bus-powered adapter
   gets no power). No PC cable needed - SSH works over Wi-Fi.
3. Run (or tell the PC session to run it over ssh): `C:\S10eTmp\usbcaltmode.exe 90 > C:\S10eTmp\altmode-adapter.txt`
   Expected lines: DISCOVER_ID (adapter VID/PID), DISCOVER_SVIDS 0xFF01 present, DISCOVER_MODES pins, ENTER_MODE
   cmd_type 1, DP_STATUS connected 1 HPD 1, DP_CONFIGURE ACK "link configured, pin C/D/E".
4. Repeat 2-3 with the hub (`altmode-hub.txt`), with its power input connected if it has one.
5. Afterwards: reboot with nothing plugged - the firmware leaves the charger in OTG boost after a boot with a sink
   attached (notes/battery-driver.md 2026-10-18); a normal boot returns to charging. Rollback of the driver:
   `pnputil /enum-drivers` -> delete the 1.0.0.7 oemNN.inf (1.0.0.6 stays in the store).
