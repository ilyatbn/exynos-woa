# Docked-desktop goal — research notes

End goal (from the user): these phones become **donated desktop computers** used through a
**USB-C dock** — USB keyboard + mouse, LAN/WIFI, and **HDMI output to a screen through the dock**.
Findings appended as discovered; every claim carries its source URL; *(inference)* = mine.

## 1. Live reads: the phone's USB-host and WLAN hardware (adb, read-only)

**USB-C host path (M10) — the hardware is there, but Android never turns it on:**

```
/proc/device-tree/usb@10C00000/            -> contains a child named  dwc3
/sys/devices/platform/10c00000.usb/        -> child 10c00000.dwc3 (listed), but
/sys/devices/platform/10c00000.dwc3/       -> does NOT exist
```

- Samsung's **USB-function controller is at `0x10C00000`** on the S10e and it *declares* a
  **DWC3** child (the Designware DWC3 = the USB-C host engine). *(inference)* same family as
  Mu-Silicium's `UsbfnDwc3Dxe`, whose release note says "Added Support for Exynos DWC3".
- The DWC3 host device is **not instantiated** while Android runs, because Android uses the
  port as a *device* (connected-to-PC) side, not a *host* side. So host mode is a
  **boot/firmware state change**, not a Windows driver problem: the UEFI must put the port
  into host mode and Windows then sees ordinary USB keyboards/mice/ethernet.
- Also present: `usb_notifier`, `usb_phy_generic.8.auto`, `usb_phy_generic.9.auto`
  (two generic USB PHY slots) and `abc_hub` — *(inference)* the two PHY slots are the
  two USB-C connectors (rear + the dock-side), which matters for which port the dock uses.

**WLAN (M12) — it is a Broadcom DHD device, not a PC wifi card:**

```
CONFIG_WLAN=y  CONFIG_WLAN_VENDOR_BROADCOM…  CONFIG_BCM_DHD_WLAN=y
CONFIG_BROADCOM_WIFI=y  CONFIG_BROADCOM_WIFI_RESERVED_MEM=y
CONFIG_BCMDHD_FW_PATH="/etc/wifi/bcmdhd_sta.bin"
CONFIG_BCMDHD_NVRAM_PATH="/etc/wifi/nvram_net.txt"
CONFIG_WLAN_REGION_CODE=100
```

- The DTB's `wlan` node is only a pin-control stub (`compatible`, `gpios`, `status`) — no
  MMIO address, so the WLAN is reached through Samsung's reserved-memory + firmware-blob
  path, not a plain device tree entry.
- *(inference)* this is why WiFi on Windows for Exynos phones is the hard one: there is no
  standard Windows driver for a "Broadcom DHD over reserved memory" device; the vendor's
  userspace blobs (`bcmdhd_sta.bin`, `nvram_net.txt`) are what Android uses.

**LAN through the dock (M13) — the Android kernel already lists the USB-ethernet drivers:**

```
CONFIG_USB_USBNET=y  CONFIG_USB_NET_AX8817X=y  CONFIG_USB_NET_AX88179_178A=y
CONFIG_USB_NET_CDCETHER=y  CONFIG_USB_NET_SMSC75XX=y  CONFIG_USB_NET_SMSC95XX=y
CONFIG_USB_NET_ZAURUS=y  CONFIG_USB_RTL8150=y  CONFIG_USB_RTL8152=y
```

- So the dock's ethernet is expected to be one of the standard USB-net devices
  (ASIX AX88179/178A, SMSC GL990x, RTL8150/2). *(inference)* Windows has vendor WWD drivers
  for the same chips on ARM64, so LAN is likely the *easy* half of M13 once host mode works.
- **Acronym trap:** `CONFIG_USB_NET_CDC_EEM` in this kernel is *Ethernet Management*
  (mobile-modem class), **not** the "EEM" of USB-C dock displays (Extended Mode / SLDCOM).
  Do not let these two meanings collide in the plan.

## 2. M11 — the dock's HDMI: DisplayLink is the viable path, EEM probably is not

**DisplayLink (a real GPU chip inside the dock) is supported on Windows on ARM64:**

- "DisplayLink USB graphics drivers support Windows 11 on ARM systems. Synaptics release notes
  for the 11.3 branch and newer list Windows 10 and 11 on ARM64 as supported… The 11.5 branch is
  where ARM64 support became part of the standard EXE installer package."
  — https://kb.plugable.com/are-displaylink-drivers-available-for-windows-11-on-arm
- Targus "validated the Synaptics DisplayLink version 11.5M3 … for all DisplayLink-based
  Targus … docking station models … **for Windows 10/11 for X64 and ARM64 processors**"
  — https://us.targus.com/blogs/discover-targus/targus-validates-synaptics-displaylink-11-5m3-software-drivers-update-for-windows
- DisplayLink's own page: "works across macOS, Windows, ChromeOS, Ubuntu & Android on Apple,
  Intel, AMD, Qualcomm **and any ARM SoC**… **Servers or smartphones**, POS terminals or
  projectors" — https://www.synaptics.com/products/displaylink-graphics
  *(inference)* "smartphones" being an advertised host means the phone-as-host use case is
  already inside DisplayLink's supported envelope.

**Why this matters:** a normal USB-C dock's HDMI is fed by **EEM/SLDCOM (USB Extended Mode)**,
a mirroring protocol that needs a host-side driver; Windows has no such driver. A
**DisplayLink dock carries its own small GPU** and is driven by a normal Windows WDDM driver,
so the HDMI becomes real, driver-supported output. *(the EEM dead-end still needs its own
citation — open task below.)*

**Consequence for the plan:** standardise the donation build on a **DisplayLink-based dock**,
not a generic USB-C hub dock. That turns M11 from "probably impossible" into "supported,
needs the driver installed in the image".

