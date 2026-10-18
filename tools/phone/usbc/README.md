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
