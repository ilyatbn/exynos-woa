# Galaxy S10e Windows driver status

As of 2026-10-14 evening (tag alpha1). Windows 11 Pro 23H2, build 22631.6199. Windows Update is turned off on the phone. Rows not mentioned in the 2026-10-14 notes below are unchanged from 2026-10-12.

🔴 nothing at all · 🟠 detected, but not usable · 🟡 works, not perfect · 🟢 working

| Device | Status | In short |
|---|---|---|
| Booting Windows | 🟡 | boots, sometimes hangs |
| CPU | 🟡 | 6 of 8 cores |
| RAM | 🟡 | 5.2 of 6 GB |
| Storage (UFS) | 🟡 | works, 6x faster today |
| Display | 🟢 | our own display driver, brightness works |
| Screen rotation | 🔴 | portrait only with the GPU driver |
| Touch | 🟡 | sometimes double taps |
| Hardware buttons | 🟢 | power, Bixby work |
| USB (docks, mouse, keyboard, drives) | 🟡 | works, no hot-plug |
| Battery / charging | 🟡 | works, slow to update |
| GPU (graphics acceleration) | 🟡 | works: desktop and 3D on the Mali GPU, clock not raised yet |
| Speaker | 🟡 | bottom speaker works, earpiece speaker silent |
| Headphone jack | 🟠 | detected, no sound |
| Microphones | 🔴 | nothing yet |
| WiFi | 🟡 | works, slow |
| Bluetooth | 🔴 | researched, driver skeleton only |
| Cellular / SIM | 🔴 | nothing yet |
| Cameras | 🔴 | nothing yet |
| Fingerprint scanner | 🔴 | nothing yet |
| Sensors (rotation, light, proximity) | 🔴 | nothing yet |
| Vibration | 🔴 | nothing yet |
| NFC | 🔴 | nothing yet |
| GPS | 🔴 | nothing yet |
| Wireless charging | 🔴 | nothing yet |

## Details

- **Booting:** Windows reaches the desktop in about 80 seconds. Before today's reinstall, every boot hung for about 2 minutes. Most likely the disk was damaged by edits made from TWRP. The phone also once powered itself off with a power-chip overload warning. We'll watch whether either comes back on the fresh install.
- **CPU:** 6 of the 8 cores work. The 2 big Samsung-designed cores aren't used because they lack a couple of features the other cores have, and Windows refuses them. Next step is to look at it with the kernel debugger, which hasn't managed to connect yet.
- **RAM:** Windows sees 5.2 GB of 6 GB. About 0.1 GB doesn't exist even for Android. The rest is set aside for things like the modem and logs. I tested it and up to about 0.5 GB of that could be given back to Windows, but we agreed to discuss that first.
- **Storage:** works and is safe. Today it went from about 20 MB/s to about 117 MB/s for large reads, after fixing two problems in our driver: the chip asked Windows to split reads into tiny 8 KB pieces, and a leftover debug check was re-reading every block byte by byte. Android still reads faster (about 600 MB/s), so there is more to gain; writes have not been re-measured yet.
- **Display:** our own display driver (MaliG76Wddm) drives the screen. Brightness works through the normal Windows slider (Settings and quick settings), from the bootloader's brightness at 100% down to about 2 nits at 0%. Automatic dimming when idle doesn't trigger yet. A blank screen saver turns the picture black after 2 minutes idle; the screen itself can't switch off yet.
- **Screen rotation:** with the GPU driver the screen is portrait only. Two landscape attempts blacked out the screen (Windows hands the driver a rotated picture that the driver doesn't turn yet), so rotation is switched off until that's done. Automatic rotation also needs the motion sensor, which has no driver yet.
- **Touch:** works, including right-click by holding. A single tap sometimes registers as a double click.
- **Buttons:** power short press turns the screen off and on. Power long press opens a reboot/shutdown menu. Bixby opens Task Manager. Volume keys don't change the volume yet; that comes with sound.
- **USB:** dock, mouse, keyboard and USB drive all work, but only if plugged in before boot. Plugging in later doesn't work yet. The phone can't charge while a dock is powering USB devices.
- **Battery:** the battery icon shows the real level, but takes a few seconds to update. If the battery runs out, plugging in the charger boots the phone back up.
- **GPU:** the Mali GPU runs Windows' desktop (including transparency) and Direct3D 11 apps through our driver plus a ported open-source graphics library (Mesa/Panfrost). A test cube runs at about 28-30 fps with no errors in long runs. "GPU Check" on the desktop shows it working. The GPU runs at 156 MHz; it won't be sped up until Windows can read temperatures and throttle. Windows Terminal can still crash on start.
- **Speaker / headphones:** Windows now plays sound through the bottom speaker (the earpiece speaker isn't used yet), and the Windows volume and mute work (tested quietly, heard by the PC mic). There are small pops when sound starts/stops, the physical volume buttons are being hooked up, and the headphone jack is not done in Windows yet.
- **WiFi:** works through the normal Windows WiFi menu (pick a network, type the password). It is slow for now; speed, sleep/wake and roaming come next.
- **Bluetooth:** same Broadcom chip as WiFi, on a separate serial line. The research, design and an untested driver skeleton are done (parkinglot/bluetooth-d4c.md).
- **Cellular, cameras, fingerprint, sensors, vibration, NFC, GPS, microphones, wireless charging:** not started.
- **Remote access (for development):** an SSH server on the phone allows remote control and driver updates over WiFi.
