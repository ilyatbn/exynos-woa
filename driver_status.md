# Galaxy S10e Windows driver status

As of 2026-10-12 evening. Windows was just reinstalled, so everything below describes the last known state on the previous install.

🔴 nothing at all · 🟠 detected, but not usable · 🟡 works, not perfect · 🟢 working

| Device | Status | In short |
|---|---|---|
| Booting Windows | 🟡 | boots, sometimes hangs |
| CPU | 🟡 | 6 of 8 cores |
| RAM | 🟡 | 5.2 of 6 GB |
| Storage (UFS) | 🟡 | works, very slow |
| Display | 🟡 | works, basic driver only |
| Screen rotation | 🟡 | manual only (Bixby hold) |
| Touch | 🟡 | sometimes double taps |
| Hardware buttons | 🟢 | power, Bixby work |
| USB (docks, mouse, keyboard, drives) | 🟡 | works, no hot-plug |
| Battery / charging | 🟡 | works, slow to update |
| GPU (graphics acceleration) | 🔴 | written, never tested |
| Speaker | 🟠 | detected, no sound |
| Headphone jack | 🟠 | detected, no sound |
| Microphones | 🔴 | nothing yet |
| WiFi | 🟠 | connects, no data |
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
- **Storage:** works and is safe, but reads at about 25 MB/s, where Android on the same phone reads over 600 MB/s. It's not the connection to the chip. The likely causes are a few settings copied from the older S9 chip. Tests to find out are ready.
- **Display:** shows the desktop through Windows' basic display driver, so there's no graphics acceleration and no brightness control.
- **Screen rotation:** holding Bixby switches between portrait and landscape. Automatic rotation needs the motion sensor, which has no driver yet.
- **Touch:** works, including right-click by holding. A single tap sometimes registers as a double click.
- **Buttons:** power short press turns the screen off and on. Power long press opens a reboot/shutdown menu. Bixby opens Task Manager. Volume keys don't change the volume yet; that comes with sound.
- **USB:** dock, mouse, keyboard and USB drive all work, but only if plugged in before boot. Plugging in later doesn't work yet. The phone can't charge while a dock is powering USB devices.
- **Battery:** the battery icon shows the real level, but takes a few seconds to update. If the battery runs out, plugging in the charger boots the phone back up.
- **GPU:** the drivers and graphics library are written and built, but haven't run on the phone yet. That's next after the storage tests. The S9+ developer showed it working on their phone, but never released code.
- **Speaker / headphones:** sound already plays from both before Windows starts, so the hardware path is proven. In Windows the speaker shows up as a device, but playback fails to start. Volume keys come after that.
- **WiFi:** connects to your network and logs in with the password. No data comes back from the WiFi chip yet. A fix is built but untested. A small app to type the network name and password is ready. The normal Windows WiFi menu comes later.
- **Bluetooth:** same Broadcom chip as WiFi, on a separate serial line. The research, design and an untested driver skeleton are done (parkinglot/bluetooth-d4c.md).
- **Cellular, cameras, fingerprint, sensors, vibration, NFC, GPS, microphones, wireless charging:** not started.
