# Galaxy S10e Windows driver status

As of 2026-10-20 (tag alpha9). Windows 11 Pro 23H2, build 22631.6199. Windows Update is turned off on the phone.

🔴 nothing at all · 🟠 detected, but not usable · 🟡 works, not perfect · 🟢 working

| Device | Status | In short |
|---|---|---|
| Booting Windows | 🟡 | boots reliably; custom boot screen, remembers brightness |
| CPU | 🟡 | 6 of 8 cores, clocks now change with load (up to 1.95 / 2.4 GHz) |
| Temperature / clocks | 🟡 | temperatures read, automatic clocks and throttling |
| RAM | 🟡 | 5.9 of 6 GB |
| Storage (UFS) | 🟢 | ~500 MB/s reads, close to Android |
| Display | 🟢 | our own display driver, brightness works |
| Screen rotation | 🟡 | portrait and landscape work (button / tool); redraw glitch while switching |
| Touch | 🟢 | works, smooth; touch keyboard works |
| Hardware buttons | 🟢 | power, Bixby work |
| USB (docks, mouse, keyboard, drives) | 🟡 | works, no hot-plug |
| Battery / charging | 🟡 | works, slow to update |
| GPU (graphics acceleration) | 🟡 | desktop, browsers, D3D11 and OpenGL apps and games on the Mali GPU |
| Hardware video decode | 🟡 | H.264 on the video chip for Windows apps (Photos, mpv) |
| Speakers | 🟢 | both speakers, stereo, balance app |
| Headphone jack | 🟢 | sound out, own "Headphones" device; no headset mic |
| Microphones | 🔴 | nothing yet |
| WiFi | 🟡 | fast on 5 GHz; 2.4 GHz slower and disturbs Bluetooth |
| Bluetooth | 🟡 | pairing, headphones and music work; choppy when WiFi is on 2.4 GHz |
| External display (USB-C to HDMI) | 🔴 | researched only; Remote Desktop works |
| Cellular / SIM | 🔴 | nothing yet |
| Cameras | 🔴 | nothing yet |
| Fingerprint scanner | 🔴 | nothing yet |
| Sensors (rotation, light, proximity) | 🔴 | nothing yet |
| Vibration | 🔴 | nothing yet |
| NFC | 🔴 | nothing yet |
| GPS | 🔴 | nothing yet |
| Wireless charging | 🔴 | nothing yet |

## Details

- **Booting:** Windows reaches the desktop in about 80 seconds. The reset that used to hit every boot after about 47 minutes was our own disk driver's watchdog rule; it is fixed.
- **CPU:** 6 of the 8 cores work. The 2 big Samsung-designed cores (M4) pass every bare-metal test we ran, but with all 8 cores Windows still crashes with memory corruption at every boot, so the phone runs on 6. Windows' own scheduler puts foreground work on the 2 faster cores.
- **Temperature / clocks:** our power driver reads the chip temperatures and sets the clocks, and a small service (the governor) raises and lowers them with load and turns everything down with the screen off. With the big cores off, the full speeds are usable in bursts: small cores 1.95 GHz, fast cores 2.4 GHz, GPU 702 MHz. A thermal guard steps clocks down at 83 °C. "clocks.exe" on the desktop shows the live speeds.
- **RAM:** Windows sees 5.9 GB and reports 6 GB installed (was 5.2 GB): memory that was only set aside for Android is given back to Windows.
- **Storage:** large reads went from about 117 MB/s to about 500-560 MB/s, and small reads take half the time, after our disk driver learned to use interrupts and keep several commands in flight. Apps start much faster: Brave's first start 6.6 → 2.4 s, Steam about 50 → 30 s. Data integrity is checked with hash tests after every driver change.
- **Display:** our own display driver (MaliG76Wddm) drives the screen. Brightness works through the normal Windows slider. A 720p mode is offered for games. A blank screen saver turns the picture black after 2 minutes idle; the screen itself can't switch off yet.
- **Screen rotation:** portrait and landscape both work. Switch with the Bixby button or the desktop rotate shortcuts. While switching, the screen shows a few broken frames. Automatic rotation needs the motion sensor, which has no driver yet.
- **Touch:** works, including right-click by holding. Windows treats the phone as a tablet: touch-sized taskbar and an on-screen keyboard.
- **Buttons:** power short press turns the screen off and on, long press opens a reboot/shutdown menu. Bixby opens Task Manager.
- **USB:** dock, mouse, keyboard and USB drive all work, but only if plugged in before boot. Plugging in later doesn't work yet. The phone can't charge while a dock is powering USB devices.
- **Battery:** the battery icon shows the real level, but takes a few seconds to update. If the battery runs out, plugging in the charger boots the phone back up.
- **GPU:** the Mali GPU runs the whole Windows desktop (Start menu, Search, transparency) and apps through our driver (now a modern WDDM 2.0 driver) and a ported open-source graphics library (Mesa/Panfrost):
  - Direct3D 11 up to feature level 10_1, plus compute shaders and geometry shaders (emulated).
  - Browsers: Edge runs with Canvas, compositing, WebGL and WebGPU on the GPU; an x64 Chromium build runs WebGL too.
  - Apps of all three kinds: native ARM64, emulated x64 and old 32-bit x86 programs.
  - OpenGL apps through our own OpenGL driver: Stardew Valley runs at 58-60 fps.
  - Compressed game textures (DXT/BC), which this GPU can't read, are converted on the fly.
  - Shaders are saved per program, so the second start of a game compiles less. A "Shader cache" app on the desktop lists each program's cache and can clear one or all.
  - Games: Dave the Diver runs at about 26 fps, and reaches its title screen 18 s faster than before (122 s); most of that wait is the game itself under x64 emulation. Need for Speed Underground 2 runs through dgVoodoo, and Game Dev Story runs.
  - Not perfect yet: feature level 11 (newer games, Chrome video), the GPU loses some speed to our driver's own overhead, and DirectX 12 / Vulkan are not supported.
- **Hardware video decode:** H.264 video is decoded by the phone's video chip for normal Windows apps (Media Foundation: Photos, mpv), with smooth 1080p playback in mpv. Photos itself still plays slowly; browsers don't use it yet.
- **Speakers:** the earpiece and bottom speaker play together in stereo, with a balance set by ear. "S10e Speakers" in the Start menu adjusts them live.
- **Headphone jack:** plugging in headphones switches to a separate "Headphones" device and back. The headset microphone isn't done.
- **WiFi:** works through the normal Windows WiFi menu. 5 GHz reaches 220-280 Mbit/s; 2.4 GHz about 35-40 Mbit/s.
- **Bluetooth:** pairing works, and Bluetooth headphones play music. When WiFi uses 2.4 GHz the sound cuts out, because WiFi and Bluetooth share one radio and the coordination Android does isn't done yet; on 5 GHz WiFi it's fine. Transfers are slow (the chip's serial link still runs at the start-up speed).
- **External display:** the USB-C chip's DisplayPort mode is researched but not switched on yet. Remote Desktop works for using the phone from another screen; a virtual-monitor prototype runs but streams black frames.
- **Cellular, cameras, fingerprint, sensors, vibration, NFC, GPS, microphones, wireless charging:** not started.
- **Remote access (for development):** an SSH server on the phone allows remote control and driver updates over WiFi.
