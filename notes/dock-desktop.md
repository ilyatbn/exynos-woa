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

## 3. M10 — USB host mode: the controller does not move, the mode does

**Our own repo already depends on this controller, and the S10e reports the same value:**

- `tools/twrp-winre/build.py:588` anchors `setprop sys.usb.controller 10c00000.dwc3` — that is
  the **star2lte (S9+)** USB controller in `init.recovery.usb.rc`.
- On the live S10e: `getprop sys.usb.controller` → **`10c00000.dwc3`**, and
  `getprop sys.usb.state` → `rndis,adb` (i.e. currently *device* mode).
  *(fact, read from the phone)* **the DWC3 controller identity is byte-identical between 9810 and 9820.**
- So M10 is **not** an address-porting problem. What has to change is the **mode**: Android keeps
  the port on the device side; a dock needs the host side.

**Prior art says host mode is the part nobody has finished on Exynos** — Mu-Silicium Status.md
(https://raw.githubusercontent.com/Project-Silicium/Mu-Silicium/main/Status.md):

| Device | Codename | State | USB Host | USB Device | Windows Boot |
|---|---|---|---|---|---|
| Galaxy S9 | `starlte` | Active | ❌ | ❌ | ✅ — "Only Windows PE boots" |
| Galaxy S10 | `beyond1lte` | Inactive | ❌ | ❌ | ❌ |
| Galaxy S20 5G | `x1s` | Active | ❌ | ✅ | ✅ — "WinPE Only", needs the `UH` partition erased |

- *(inference)* our repo is the thing that got past PE on Exynos (full install on star2lte), so
  host mode is genuinely new work here, not something to copy.
- `x1s` shows how destructive some boot paths are: "the Platform Hypervisor Extensions partition
  needs to be erased via `dd if=/dev/zero of=/dev/block/by-name/uh`… This will stop android from
  booting until the partition is restored" — a pattern we must **not** adopt for a donated device.

**The Snapdragon side has a working recipe, and a warning that matters for docks**
(https://github.com/n00b69/woa-mh2lm5g/blob/main/guide/materials.md, LG V50S under Windows):

> "Disable USB host mode if you use a **powered USB hub**, as this can **irreversibly damage
> your device**. If you don't use a powered USB hub, enable USB host mode or you will not be
> able to use any USB devices."

- Tool: **USB Host Mode Control** — https://github.com/Misha803/My-Scripts/releases/tag/USB-Host-Mode-Control
- *(inference)* for our donation build this is a hard rule: the dock must be **non-powered
  (passive)** or the host-mode/charging combination has to be validated before we ship a device
  to anyone. A powered dock is exactly what most "desk docks" are.
- Same guide documents a **boot.img auto-flasher** (restore Android's boot image on Windows boot
  or when the battery is under 15%) — directly relevant to M14 and to keeping donated devices
  serviceable.
- Renegade Project's own troubleshooting page assumes a **USB keyboard during OOBE**
  (https://renegade-project.tech/en/Troubleshooting) — *(inference)* host mode + HID works on
  their Snapdragon targets, so the pattern is proven on other SoCs, just not on Exynos.

## 4. The 9810 → 9820 constants table (what actually has to move)

Source of the left column: our own
[`firmware/Silicon/Exynos9810Pkg/Include/Platform/Exynos9810.h`](../firmware/Silicon/Exynos9810Pkg/Include/Platform/Exynos9810.h),
which is the single place the repo keeps MMIO windows. Source of the right column: **live
node names on the connected S10e** (`/proc/device-tree/`, `/sys/devices/platform/`).
Node names carry the address, and names *are* readable even though the property *contents*
are SELinux-blocked — that is the whole trick.

| Block | 9810 (repo) | 9820 (live S10e) | Verdict |
|---|---|---|---|
| USB DWC3 core (xHCI) | `0x10C00000` | `usb@10C00000` | **identical** |
| USB PHY | `0x11100000` | `phy@10B00000` | **moves** |
| USB/DP combo PHY | `0x110A0000` | no separate node; `phy@10B00000` has `has_combo_phy` | **unknown — probe** |
| USB/DP PCS | `0x110B0000` | no separate node; same node has `has_other_phy` | **unknown — probe** |
| Touch HSI2C bus | `0x104B0000` | `104b0000.hsi2c` | **identical** |
| Touch I2C slave addr | `0x48` | `touchscreen@48` | **identical** |
| UFS HCI | `0x11120000` | `13d60000.ufs` | **moves** (pinned in `notes/exynos9820-research.md`) |
| CMU TOP | `0x1A240000` | `1a240000.cmu_ewf` | **identical base** |
| CMU FSYS0 | `0x11000000` | not present at that address | **moves / unknown** |
| PMU "system controller" | `0x14060000` | `system-controller@15860000` | **moves** |
| SPEEDY | `0x141C0000` | `speedy@15970000`, `speedy1@15980000` | **moves** (and there are two) |
| GIC (GICD/GICC/GICH/GICV) | `0x10101000`/`2000`/`4000`/`6000` | no `gic@…` node at top level; only `abox-gic@18cf0000` | **unknown — needs the DTS** |
| chip ID | `0x10000000` | `chipid@10000000` | **identical** |
| Debug UART | `0x10440000` | `uart@10440000` | **identical** |
| Watchdog cl0 | `0x10050000` | `watchdog_cl0@10050000` | **identical** |
| GPIO PERIC0 | `0x10430000` | `pinctrl@10430000` | **identical** |
| ALIVE GPIO | `0x14050000` | no node there; `pinctrl@15850000` exists | **moves / unknown** |
| DECON (display) | `0x16030000` | `decon_f@0x19030000`, `decon_t@0x19050000` | **moves** (and split in two) |
| DSIM | `0x16080000` | `19080000.dsim` | **moves** |
| Framebuffer geometry | 1440 × 2960 | S10e panel is 1440 × **3040** | **moves** |

**What this changes about the plan:** the port is not "everything moves". Six of the blocks
the drivers depend on — **DWC3 core, touch bus, touch slave address, CMU TOP, chip ID,
debug UART, watchdog, PERIC0 GPIO** — are byte-identical. The blocks that move are the
**PHY windows, the PMU/SPEEDY/DECON region, and UFS**. That is why M3 (HSI2C) is nearly
free and M5 (UFS) is the big constants job.

Two more live facts that matter to the dock goal:

- `13ed0000.pcie0` and `pcie1@13120000` exist on the S10e — *(inference)* Samsung's S2S
  "PCIe" transport, not a slot for external devices. Do **not** read this as a path to an
  external GPU/dock display.
- `sec_detect_conn`, `sec_thermistor@0/2/7`, `battery`, `vbat`, `hall`, `certify_hall` are
  live platform nodes — *(inference)* the thermal/charging policy M14 has to reason about
  is Samsung's own device-freq/thermal stack (`17000010.devfreq_mif` … `devfreq_*`), which
  Windows has no notion of.

## 5. The 9820 device tree, resolved (vendor DT — closes the constants questions)

Found the S10e's own decompiled DT in a kernel tree that lists **G970F** (our exact model)
among its supported devices: `CruelKernel/samsung-exynos9820`, branch `cruel-HVJ5-v5.3`,
file `arch/arm64/boot/dts/exynos/exynos9820.dts`
(https://github.com/CruelKernel/samsung-exynos9820 — archived 2026-08-12, read-only).
Numeric `phandle`s and `<0x…>` values show this is the **decompiled device tree of the
shipping build**, i.e. exactly what the phone runs — better evidence than a vendor `.dtsi`.

### Resolved values, compared against our `Exynos9810.h`

| Block | 9810 (repo) | 9820 (vendor DT) | Verdict |
|---|---|---|---|
| **GIC** GICD/GICC/GICH/GICV | `10101000`/`10102000`/`10104000`/`10106000` | `10101000 0x1000`, `10102000 0x1000`, `10104000 0x2000`, `10106000 0x2000` | **identical** — node is `interrupt-controller@10100000`, `arm,cortex-a15-gic`/`arm,cortex-a9-gic` (GICv2) |
| USB DWC3 core | `10C00000` + 0x10000 | `usb@10C00000` `reg = <0x0 0x10c00000 0x10000>` | **identical** |
| USB PHY | `11100000` + 0x1000 | `phy@10B00000` reg #1 = `10b00000` + **0x200** | **moves** |
| USB combo PHY | `110A0000` + 0x1000 | reg #2 = **`10ae0000` + 0x2800** | **moves** |
| USB PCS | `110B0000` + 0x1000 | reg #3 = **`10af0000` + 0x800** | **moves** |
| HSI2C #10 (touch bus) | `104B0000` + 0x1000 | `hsi2c@104B0000`, `samsung,exynos5-hsi2c` | **identical** |
| HSI2C #10 IRQ | SPI 404 → GSIV 436 | `interrupts = <0x0 0x184 0x0>` → SPI **388** → GSIV **420** | **moves** |
| HSI2C default clock | — | `default-clk = <0xbebc200>` = **200 MHz** | new fact, drives M3 timing |
| Touch I2C slave addr | `0x48` | live `touchscreen@48` (added by the board `.dts`, not this file) | **identical** |
| GPIO PERIC0 (`pinctrl@10430000`) | `10430000` + 0x1000 | `10430000` + 0x1000, SPI `0x17c`=380 | **identical** |
| **GPIO ALIVE** | `14050000` + 0x1000 | `exynos-pm` `gpio_alive_base` = **`15850000`** + 0x1000 | **moves** |
| GIC ispendrn latch page | — | `gicd_ispendrn_base` = `10101200` + 0x100 | new |
| PMU syscon | `14060000` + 0x10000 | `system-controller@15860000`, `samsung,exynos9820-pmu` | **moves** |
| CMU TOP | `1A240000` + 0x10000 | `cmu_ewf` `reg = <0x0 0x1a240000 0x1000>` | **same base**, smaller declared window |
| DECON | `16030000` | `decon_f@0x19030000` + 0x10000, `decon_t@0x19050000` + 0x10000 | **moves**, and split in two |
| DSIM | `16080000` | `19080000` | **moves** |
| UFS HCI | `11120000` | `13d60000` | **moves** (already pinned in `notes/exynos9820-research.md`) |
| chip ID | `10000000` | `chipid@10000000` | **identical** |
| Watchdog cl0 | `10050000` | `watchdog_cl0@10050000` | **identical** |
| Debug UART | `10440000` + 0x1000 | `uart@10440000` + **0x100**, `fifo-size = 0x100` | same base, smaller window |
| DRAM banks | `80000000` + 0x3C800000, hole, high bank | `memory@80000000` = 0x80000000+**0x80000000**, `memory@880000000`, `memory@900000000` | **different layout** |

### IRQ numbers the ACPI tables will need (SPI + 32 = GSIV, repo's convention)

| Device | SPI (hex) | SPI (dec) | GSIV |
|---|---|---|---|
| DWC3 core (`usb@10C00000/dwc3`) | `0xf1` | 241 | **273** |
| USB PHY (two) | `0xf5`, `0xf6` | 245, 246 | **277, 278** |
| HSI2C #10 | `0x184` | 388 | **420** |
| `pinctrl@10430000` | `0x17c` | 380 | **412** |
| `decon_f` | `0xb7`,`0xb8`,`0xbd`,`0x197` | 183,184,189,407 | 215,216,221,439 |

### Facts that change the M10 plan

- Both `usb@10C00000` and `phy@10B00000` carry **`status = "disabled"`** in the shipping
  build — the vendor DT itself proves the port is not in host mode under Android. *(fact)*
- `dwc3` declares `phys = <&phy 0>, <&phy 1>` with `phy-names = "usb2-phy", "usb3-phy"`:
  **one PHY node serves both the USB2 and USB3 ports**, and `has_combo_phy = 1`,
  `has_other_phy = 0`. So the 9810's three-window model collapses to one node with three
  register windows on 9820.
- `xhci_l2_support = 1` and `enable_sprs_transfer = 1` on the DWC3 child; `exynos-pm` has
  `usbl2_suspend_available = 1`, `usbl2_suspend_mode_idx = 0xc` — *(inference)* the PHY can
  be put to sleep for L2 suspend, which is exactly the power interaction M14 has to reason
  about when a dock is attached.
- `phy_version = 0x301`, `sub_phy_version = 0x400` — the PHY **generation differs from the
  9810's**, so the 9810 PHY-init sequence cannot be pasted; it must be re-derived.
- Mainline Linux documents this exact shape: `Documentation/devicetree/bindings/usb/samsung,exynos-dwc3.yaml`
  (https://lists.infradead.org/pipermail/linux-arm-kernel/2023-January/806932.html) — a
  `samsung,exynos-dwusb` wrapper with no MMIO of its own, holding a `snps,dwc3` child.
  *(inference)* the wrapper's `clocks`/`regulators` are the CMU hooks our firmware already
  pokes at in `UsbDebug.c`.



## 6. M12 — WiFi: the phone's WLAN has no address window, so Windows cannot bind to it

Two independent directions agree.

**From the vendor device tree** (`exynos9820.dts`, `CruelKernel/samsung-exynos9820`):
the only WLAN entries are **GPIO pins**, not devices —

```
wlan_host_wake = "/pinctrl@15850000/wlan_host_wake"   (ALIVE GPIO)
cfg_wlanen     = "/pinctrl@10430000/cfg_wlanen"       (PERIC0 GPIO)
```

There is **no `wlan` device node with a `reg =`** anywhere in the file. *(fact)*

**From the live phone**: `/sys/devices/platform/wlan/` exists but has **no `driver` symlink**
and an **empty `modalias`**, and its `of_node` points at `/wlan` in the DT. *(fact)*
So in the shipping build the WLAN is not driven as an MMIO device at all — Samsung's
Broadcom-DHD path runs through reserved memory plus the userspace blobs
(`bcmdhd_sta.bin`, `nvram_net.txt`, seen in the 9820 defconfig in §1).

**Consequence:** there is nothing for a Windows driver to attach to. Windows WiFi needs
either a WWD/NDIS device or a standard card; this is neither. *(inference)*

**Decision for the donation build:**
1. **LAN through the dock is the network path** — it is a plain USB-net device (M13) and
   needs nothing beyond host mode.
2. If WiFi is genuinely required, the answer is **a USB WiFi adapter in the dock**, not the
   phone's own WLAN. That is an ordinary Windows device once M10 works.
3. The phone's internal WLAN stays a documented dead end, not a stage in the installer.

## 7. M11 — what is *not* yet proven, and the dead ends so far

The DisplayLink decision (§2) does **not** depend on the EEM claim, so the plan is safe either
way. But the claim "Windows has no EEM/SLDCOM driver" is still **uncited** — recorded as
inference, not fact.

Dead ends already tried (do not repeat):
- `websearch` for "Windows support EEM SLDCOM" → only dock-review noise, no primary source.
- `https://github.com/emd2v2/emd2` → **404** (the EMD2 project is not at that path).
- `https://www.sldicom.com/` → transport error.
- GitHub API repo search for "extended mode display driver" → returned an unrelated junk repo.

What *is* cited and usable:
- DisplayLink supports Windows 10/11 **ARM64** (Plugable KB, Targus validation note, Synaptics
  product page) — §2.
- A generic USB-C dock's HDMI is fed by a host-side protocol, not by a video pipe we control;
  the Framework/Startech/Reddit threads all describe the same split (EEM-style docks need a
  driver; DP-Alt-Mode docks do not) — https://community.frame.work/t/usb-c-thunderbolt-dock-megathread/1460
- Next concrete step instead of more searching: **ask the user which dock they standardise on**,
  because the dock's own spec sheet says whether it is EEM or DisplayLink.
