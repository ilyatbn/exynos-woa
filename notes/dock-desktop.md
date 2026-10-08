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

## 8. M14 — power while docked: the port is dual-role, and that is the whole problem

**The port is a Dual-Role (DRD) controller, and the shipping build pins it to the device side**
(vendor DT, `exynos9820.dts` + the board patch):

```
&usbdrd_dwc3 { dr_mode = "otg"; maximum-speed = "super-speed-plus"; ... }
```

Mainline's own binding gives the legal values
(https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/Documentation/devicetree/bindings/usb/usb-drd.yaml):

```
dr_mode:  enum: [host, peripheral, otg]   default: otg
usb-role-switch:            "capable of assigning the USB data role (host or device)"
role-switch-default-mode:   enum: [host, peripheral]  default: peripheral
```

- *(fact)* A dock needs **`host`**. The phone ships with **`otg`** (device side). That single
  switch is what M10 is about — and it is a **device-tree/firmware** decision, not a Windows
  driver decision.
- *(fact)* The 9820 DWC3 also declares `enable_sprs_transfer = 1`, `xhci_l2_support = 1`,
  `is_not_vbus_pad = 1`, and `exynos-pm` declares `usbl2_suspend_available = 1` with
  `usbl2_suspend_mode_idx = 0xc`.
- The mainline schema says the DWC3 has **two power-domains: the PMU and everything else**
  (https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/Documentation/devicetree/bindings/usb/snps,dwc3.yaml)
  — and the 9820 PMU is `system-controller@15860000` (§5). So putting the port into host mode
  touches the same PMU the whole thermal/power stack uses.

**Why charging and host mode are the same question:** the S10e has **one** USB-C connector.
The dock plugs into the port that would otherwise charge the phone. `dr_mode = "host"` makes
that port send data *outward*; whether the dock's power still flows *inward* is exactly the
combination the community warns about ("a powered hub plus host mode can irreversibly damage
your device"). Mu-Silicium lists **USB Power Delivery ❌ on every Exynos target** they have.

**Consequences for the donation build (M15):**
1. A donated phone-desktop must be assumed to run **on its own battery**, with no reliable
   charging while docked, until proven otherwise on a spare unit.
2. That makes **battery exhaustion a normal, expected event** — the build must degrade
   gracefully (save/close, not crash) and must be restartable by a non-technical recipient.
3. The **boot.img auto-flasher** pattern (restore Android's boot image when the battery is
   low) exists in the WOA community for exactly this reason — but it is a *dual-OS* trick and
   must never be the recipient's first point of failure.
4. Windows on ARM has **no battery interface at all in our repo today** (grep for
   battery/UPS across `drivers/` and `firmware/` finds none) — so today the device already
   runs Windows with an invisible, unmanaged battery. That is the honest starting point, not
   a solved state.

## 9. What our own firmware already has, and exactly what is missing

`firmware/Platform/Star2LtePkg/Library/PlatformBootManagerLib/UsbDebugHid.c` is a
**complete DWC3 register vocabulary** — PHY link/clk/pwr/UTMI/HSP offsets, the xHCI-side
`GUSB2PHYCFG`/`GUSB3PIPECTL`/`GSBUSCFG` windows, the device-side `DCTL`/`DSTS`/`DEP*`
engine, and TRB command words. It is compiled in only when
`STAR2LTE_USB_DEBUG_MODE == HID`, and `Star2LtePkg.dsc:26` defaults that to **0 (off)**.

**But it is peripheral mode, not host mode.** The giveaway is what it programs:
`USB_EP0_OUT/IN`, `USB_HID_EP_IN = 3`, `DWC3_DCTL_RUN_STOP`, `DWC3_DEPCMD_START_TRANSFER`,
and `DWC3_GCTL_PRTCAP_DEVICE = (0x2 << 12)`. That is the phone presenting itself as an HID
*device* down a cable (the header comment says the point is "so Windows can consume firmware
logs without a custom kernel driver"). A dock needs the opposite capability field:
`PRTCAP` **host** (`0x1 << 12`), plus host-side endpoint allocation for whatever is plugged in.

*(inference)* So M10 is a **narrow, well-understood change**: reuse the register vocabulary
that already exists, flip `PRTCAP`, and let Windows' inbox `USBXHCI.sys` bind to the xHCI
window — which `Exynos9810.h:154` already says is what happens on the 9810 today.

**The CMU clock windows move, and the USB code depends on them.** Our `UsbDebug.c` pokes
`CMU_FSYS0_USB_*` QCH offsets under `EXYNOS_CMU_FSYS0_BASE = 0x11000000` (9810). The 9820
device tree declares a different set:

```
cmu      reg = <0x0 0x1a200000 0x8000  0x0 0x1bc00000 0x8000  0x0 0x1bd00000 0x8000
               0x0 0x1be00000 0x8000  0x0 0x1bf00000 0x8000>
cmu_ewf  reg = <0x0 0x1a240000 0x1000>
```

- *(fact)* 9820 CMU banks: **0x1A200000**, 0x1BC00000, 0x1BD00000, 0x1BE00000, 0x1BF00000 —
  none of them is the 9810's `0x11000000`.
- *(fact)* the 9820 USB wrapper asks for clocks `aclk`/`sclk` (ids `0x11d`, `0x120`) and the
  PHY asks for `ext_xtal`/`aclk` — so the USB function is clocked from the CMU, exactly as
  the 9810 code assumes, but through **different windows**.
- *(inference)* this is the single biggest reason M10 cannot be "copy the 9810 module": the
  clock/QCH plumbing has to be re-pointed before `PRTCAP` is flipped, or the PHY runs at the
  wrong rate and the host-side pipes mis-time.

## 10. DisplayLink's own docs validate the plan — and confirm the charging dead end

Source: https://support.displaylink.com/knowledgebase/articles/1915774-android-phone-is-not-cloning-to-a-displaylink-scre
and https://www.synaptics.com/products/displaylink-graphics/downloads/android

**The host-mode gate is exactly what they describe** *(fact)*:

> "The phone is not a USB host … When connecting to a DisplayLink enabled dock, the phone
> needs to become the USB host and the dock or adapter is the USB device. To switch the phone
> into a USB host, a USB OTG adapter is used. With USB C this should happen automatically,
> but is not supported by all phones."

So a phone driving a DisplayLink dock is a **supported product configuration**, and the one
thing that makes it fail is the host-mode switch — which is M10. *(inference)* our job is to
make that switch exist on Exynos, where Mu-Silicium shows it never did.

**Charging while docked is a stated dead end, not a guess** *(fact)*:

> "Android devices cannot currently enable USB and charge at the same time. This means it will
> not be possible for your Android device to charge while docked to a DisplayLink enabled
> docking station."

That is the vendor, describing the same single-port collision §8 derived from the hardware.
*(inference)* if it is impossible on Android's mature stack, it is not going to become
possible on our Windows stack — so M14's honest answer is **the donated device runs on its
battery while docked**, and the build must be designed around that.

**Two more vendor caveats that change the shopping list** *(fact)*:

> "USB C docks may not all use DisplayLink technology and instead rely on USB Alternate mode
> for a video output. Alternate mode is not supported by all phones … Look for the DisplayLink
> 'Plug and Display' logo on your product to ensure it is DisplayLink enabled."

> "The phone cannot deliver enough power to a DisplayLink enabled device … particularly
> relevant to USB bus powered monitors … you may have to attach it to an externally powered
> USB hub first, as the Android device might not be able to provide enough power."

- So the dock must be chosen by **DisplayLink chip generation**, not by "USB-C dock" generally.
- A **powered hub** is the vendor's own suggested fix for the power shortfall — which is the
  same configuration the WOA community warns "can irreversibly damage your device". *(inference)*
  that warning is about our SoC, not theirs, so it must be re-tested on a spare unit rather
  than assumed fatal.
- "DisplayLink's wired Ethernet connection is not currently supported" is a statement about
  **the Android app**, not about Windows — *(inference)* on Windows the dock's ethernet is an
  ordinary USB-net device (M13), so do not carry this limitation over.

**What this settles:** the donation build is **phone + DisplayLink dock + monitor + USB
keyboard/mouse + LAN**, running on battery, with the phone's own panel as the fallback display.

## 11. M11, the preferred path: DP Alt Mode — the S10e really has it

The user prefers the **non-DisplayLink** dock because it is cheaper, and asked how DeX works.
DeX does not use DisplayLink and does not use EEM mirroring. It uses **DisplayPort Alt Mode**:
a real DisplayPort signal transmitted out of the phone's own USB-C connector.

*(fact)* The S10e is listed as DP-Alt-Mode capable:
https://uperfect.com/blogs/wikimonitor/list-of-smartphones-with-displayport-alt-mode —
row "Samsung Galaxy S10e | Samsung DeX | smartphone | Android | 2019".

*(fact)* Samsung's own instructions are a cable, not a driver: "Connect an HDMI adapter to your
mobile device… Connect an HDMI cable to the HDMI adapter to a TV or monitor's HDMI port…
The Samsung DeX screen will appear on the connected TV or monitor"
— https://www.samsung.com/ph/support/mobile-devices/how-to-use-a-samsung-usb-c-to-hmi-adapter
(the S10 series is in the compatible list).

*(fact)* DisplayLink's own support page draws the same line we should keep:
"USB C docks may not all use DisplayLink technology and instead rely on USB **Alternate mode**
for a video output" — §10 source.

**And the hardware agrees with them.** From the 9820 device tree (§5):

```
phy@10B00000   compatible = "samsung,exynos-usbdrd-phy"
               reg #2 = 0x10ae0000 +0x2800      has_combo_phy = 1
displayport@0x130B0000                          (its own S2S node)
decon_f@0x19030000  decon_t@0x19050000          (display pipeline, two halves)
```

The Exynos 9820 has a **USB/DP combo PHY** — one connector window that carries both the USB
pipes and a DisplayPort pipe. That is the thing the 9810 header already calls
`EXYNOS_USB_COMBO_PHY_BASE`. *(inference)* DeX's picture is DECON → DP pipe → the dock's HDMI.

### What this means for the plan, honestly

| Step | Status |
|---|---|
| The S10e can emit DP over its USB-C port | **hardware fact** — vendor DT + the DP-alt-mode list |
| A cheap USB-C→HDMI dock will receive it | **needs-dock** — the owner's non-DisplayLink dock, plus the XDA caveat that some docks' **DP** input fails while their **HDMI** works |
| The firmware can set up DECON→DP scanout | **new work** — the repo today reuses the sboot-lit framebuffer for the **phone panel** and never touches DECON (`Exynos9810.h:203` says so) |
| Windows treats the external monitor as a **second display** | **the hard part** — Windows only enumerates displays a WDDM driver reports; a second output is real driver work, not a config change |

So the cheap path is **not** cheaper in engineering, only in purchase price. It should be
attempted, but the plan must not pretend the last row is free. DisplayLink remains the
fallback because Synaptics already wrote the WDDM driver for ARM64 (§2).

## 12. M3 — the HSI2C clock plumbing on 9820: same names, different windows

Verified against the 9810 driver's own constants header
(`drivers/Exynos9810Hsi2c/exynos9810hsi2c.h`) and the 9820 kernel:

| Constant (9810 driver) | 9820 evidence | Verdict |
|---|---|---|
| `EXYNOS_HSI2C10_PHYSICAL_BASE 0x104B0000` | vendor DT `hsi2c@104B0000`, live `104b0000.hsi2c` | **identical** |
| `EXYNOS_HSI2C_SOURCE_CLOCK_HZ 200000000` | vendor DT `default-clk = <0xbebc200>` = 200,000,000 | **identical** |
| `EXYNOS_PERIC0_GPIO_PHYSICAL_BASE 0x10430000` | vendor DT `pinctrl@10430000`, live node | **identical** |
| `EXYNOS_GPIO_GPP1_SCL_PIN 4` / `SDA_PIN 5` | DT `gpio_scl = <0x74 0x4 0x1>`, `gpio_sda = <0x74 0x5 0x1>`, and `gpp1` has phandle `0x74` | **identical** |
| `EXYNOS_PERIC0_CMU_PHYSICAL_BASE 0x10400000` | **no such node on 9820**; the DT's `cmu` node declares `1A200000`, `1BC00000`, `1BD00000`, `1BE00000`, `1BF00000` | **moves — window must be re-chosen** |
| `EXYNOS_PERIC0_SYSREG_PHYSICAL_BASE 0x10411000` | no such node; 9820 has `sysreg-controller@13020000` / `@13C10000` | **moves** |
| `EXYNOS_CMU_GATE_USI03_*` offsets `2018/206C/20DC/20E0`, `QCH 3038` | the 9820 uses **the same names** (`GATE_USI03_I2C`, `USI03_I2C_QCH`) but resolves them through Samsung's CAL headers | **same vocabulary, new numbers** |

The 9820 clock driver `drivers/clk/samsung/clk-exynos9820.c` includes
`../../soc/samsung/cal-if/exynos9820/cmucal-vclk.h`, `cmucal-node.h`, `cmucal-qch.h` —
and those files **do exist** in the tree (reachable in raw form as
`.../drivers/clk/samsung/../../soc/samsung/cal-if/exynos9820/cmucal-qch.h`, 17 KB):

```
enum qch_id { APBIF_GPIO_ALIVE_QCH = QCH_TYPE, ... }
  USI03_I2C_QCH        -> entry 676
  USI03_USI_QCH        -> entry 677
  GPIO_PERIC0_QCH      -> entry 664
  USB31DRD_QCH_REF     -> entry 349   (SLV_CTRL 350, SLV_LINK 351, APB 352, PCS 353)
```

- *(fact)* the 9820 keeps **USI03** as the name for the HSI2C #10 clock, and adds a full
  `USB31DRD` QCH family — the same five handles the 9810 firmware pokes in `UsbDebug.c`
  (`USB_PLL_QCH`, `CTRL_QCH`, `LINK_QCH`, `PHY_QCH`, `PCS_QCH`).
- *(inference)* the QCH **index → address** rule lives in `cmucal.h`/the s2cmu implementation,
  so the 9820 numbers must be derived from there, not guessed. Until that is done, the CMU
  half of the 9820 HSI2C driver stays `UNVERIFIED` per the repo's rule.
- *(fact)* `USB31DRD` hangs off `UMUX_CLKCMU_FSYS0A_BUS`, while `USI03_*` hangs off
  `UMUX_CLKCMU_PERIC0_BUS` — **two different CMU banks**, which is why M10 (USB) and M3
  (touch bus) cannot share one CMU window constant.

## 13. M5 — the S10e's storage, read live (this is what the write fence must be re-derived from)

All of it from the connected phone, nothing destructive.

### Facts

`/proc/partitions` is readable on this build (it is `/proc`, not `/sys`, so SELinux
does not hide it). `sda` = **124,944,384 KiB** = 31,236,096 logical blocks of 4 KiB.

`/dev/block/by-name/` symlinks give the name → partition map:

```
sda1 bota0 4608      sda9 steady 4096      sda17 nad_refer 1024    sda25 system  5836800
sda2 bota1 7680      sda10 uh 2048         sda18 persistent 512    sda26 vendor  1126400
sda3 efs 20480       sda11 uhcfg 4096      sda19 misc 1024         sda27 product  634880
sda4 param 8192      sda12 dtb 8192        sda20 keyrefuge 16384   sda28 cache    614400
sda5 up_param 8192   sda13 dtbo 8192       sda21 keydata 16384     sda29 omr      51200
sda6 keystorage 512  sda14 boot 56320      sda22 dqmdbg 16384      sda30 cp_debug  5120
sda7 sec_efs 20480   sda15 recovery 66048  sda23 vbmeta 64         sda31 userdata 116334592
sda8 bota2 8192      sda16 radio 51200     sda24 bota3 1472
```

Sizes are in KiB. Partitions 16 and above have a **different device major** (259, not 8)
— a kernel numbering consequence of having more than 16 partitions, not a second disk.

Other LUNs exist and are **not** part of `sda`:

```
sdb 4096 KiB   sdc 4096 KiB   sdd 8192 KiB (sdd1 cpefs 6144 KiB)   sde 8192 KiB
mmcblk0 125026304 KiB, mmcblk0p1 125009920 KiB
```

*(fact)* `cpefs` lives on `sdd1`, i.e. **a separate LUN** — the 9810 driver's assumptions
were written against a single `sda`, so multi-LUN is a real behavioural difference, not a
cosmetic one. *(inference)* `mmcblk0` being the same size as `sda` means the media is also
reachable through the `dwmmc2`/`mmc_host` path; which of the two the UFS HCI actually
services has to be decided on the Windows side, not assumed.

The device's own `/vendor/etc/fstab.exynos9820` confirms three things independently of
the device tree:

```
/dev/block/platform/13d60000.ufs/by-name/keydata      <- UFS HCI base 0x13D60000, again
/devices/platform/13d00000.dwmmc2/mmc_host*           <- SD card slot hangs off dwmmc2
/devices/platform/10c00000.usb*                       <- vold's USB device is the DWC3 node
```

That third line is a second, independent confirmation that `10c00000.usb` is the same
connector Android's file-transfer path uses — the port M10 has to turn into a host.

### Derived, NOT read: the LBA map

The arithmetic below is anchored on one verified fact from the 9810 header — EFS is
`sda3`, LBA **4096..9215**, i.e. 5120 blocks — and the S10e's `sda3` is **exactly 20480 KiB
= 5120 blocks**. Fitting the S10e's sizes against that anchor makes the partitions
contiguous with no slack:

```
sda1 starts at LBA 1024 (4 MiB)
  sda1 1024..2175      sda2 2176..4095      sda3 EFS 4096..9215   <- matches the 9810 exactly
  sda14 BOOT     27264..41343
  sda15 RECOVERY 41344..57855
  sda25 system   83968..1543167
  sda28 cache    1983488..2137087      <- the ESP candidate
  sda31 userdata 2151168..31234815    <- the Windows volume candidate
  tail           31234816..31236095   (1280 blocks, claimed by nothing)
```

Front gap 1024 blocks + tail 1280 blocks = 2304 blocks = 9216 KiB, which is exactly the
difference between the sum of all partition sizes and the disk size. The arithmetic closes.

*(inference)* — every number above is derived, not read. It must be confirmed against the
S10e's own GPT before any fence constant is written down as verified, and the only ways to
read that GPT are (a) root on the phone, or (b) the Windows driver itself, which already
reads LBA 1 and reports capacity.

### What this changes in M5

| 9810 constant | S10e |
|---|---|
| `UFS_WRITE_FENCE_FIRST_LBA 68736` (sda18 SYSTEM) | the S10e has no sda18 SYSTEM; the equivalent scratch is `sda18 persistent` at LBA 70912 — **different meaning entirely** |
| `UFS_PROTECTED_PARTITION_*` (sda19..sda25) | the S10e's protected set is `sda3 EFS`, `sda7 sec_efs`, `sda14 BOOT`, `sda15 RECOVERY`, `sda20/21` keydata — **a different partition list** |
| `UFS_EXPECTED_LAST_LOGICAL_BLOCK 15615999` | **31,236,095** — the disk is twice as large |
| `UFS_DMA_WINDOW_FIRST/LAST 90000000..BC800000` | derived from the 9810's DRAM banks; the S10e's banks are `80000000`+`80000000`, `880000000`, `900000000` — **must be re-derived** |
| EFS range | **identical** — 4096..9215 on both |

## 14. M4 — the touch path barely moves, and the FOD question does not apply

**The bus identity is identical.** On the live S10e:

```
/proc/device-tree/hsi2c@104B0000/touchscreen@48
```

`touchscreen@48` is a child of **`hsi2c@104B0000`** — the same bus window and the same
slave address the validated 9810 driver uses (`EXYNOS_HSI2C10_PHYSICAL_BASE 0x104B0000`,
`EXYNOS_TOUCH_I2C_ADDR 0x48`). Enumerating every `hsi2c@…` node on the phone shows the
touch IC under this one and no other. *(fact)*

**The panel family is `y761`, not `y771`.** The S10e's own `sec,firmware_name` is
`tsp_sec/y761_beyond0_phole.bin`, while the 9820 kernel's *source directory* is
`sec_ts/y771` and its default blob is `tsp_sec/y771_beyond2.bin`; the S10 (`beyond1lte`)
DTS used `y771_beyond1_phole.bin`. *(inference)* `y771` names the kernel driver family,
whereas the panel IC shipped in the **S10e** is a **y761** — the same family our Windows
driver is already named after (`drivers/S6SY761Touch`, bound to `ACPI\SCSY0761`). So the
touch port is not a rename job; it is a tuning-values job.

**Tuning properties on the S10e's own node** (names are readable; the values below are the
ones already captured in `notes/s10e-live-reads.md`):

```
sec,project_name  = BEYOND0G970        sec,tclm_level   = 2
sec,afe_base      = 0x0205             sec,max_coords   = 4096 x 4096
sec,firmware_name = tsp_sec/y761_beyond0_phole.bin
sec,irq_gpio  sec,irq_type  sec,area-size
sec,regulator_avdd  sec,regulator_dvdd  sec,regulator_boot_on
enable_settings_aot  support_dex_mode  support_ear_detect_mode  sync-reportrate-120
```

Two of those matter to us:

- *(fact)* **there is no `support_fod` on this node.** The 9820 `.dtsi` family has it, but the
  S10e's own shipping node does not — so the fingerprint-on-glass complication M4 was
  braced for **does not apply to the donation device**. That closes the row.
- *(fact)* `sync-reportrate-120` is present, and our driver's report-delivery changes are
  written around a 64-event controller FIFO. *(inference)* a 120 Hz panel is where the
  coalescing rules in `drivers/S6SY761Touch/README.md` are most likely to be exercised,
  so the touch candidate is the part most at risk of needing re-qualification on this model.
- `sec,afe_base` is **0x0205 on the S10e vs 0x0206 on the S10** — a per-model value, so
  anything in our stack that hardcodes it must be re-read, not carried over.

## 15. M8 — the S10e's CPU topology: three clusters, and the 9810's math assumes two

Live read of `/proc/cpuinfo` on the connected S10e — **8 logical CPUs**, and the
per-core identity fields are readable:

```
processor 0-3   implementer 0x41 (ARM)      variant 0x1  part 0xd05
processor 4-5   implementer 0x41 (ARM)      variant 0x2  part 0xd0a
processor 6-7   implementer 0x53 (Samsung)  variant 0x1  part 0x003
```

*(fact)* three distinct core identities → **three clusters, sized 4 + 2 + 2**, not
the S9+'s two clusters of four. The live platform nodes agree: `/sys/devices/platform/`
carries `10090000.LITTLE`, `10090000.MID` and `10090000.BIG` — three power domains,
and the 9810 has only `BIG` and `LITTLE`.

*(fact)* the 9810's `ArmPlatformGetCorePosition` computes

```c
CoresPerCluster = PcdGet32 (PcdCoreCount) / PcdGet32 (PcdClusterCount);
return (GET_CLUSTER_ID (MpId) * CoresPerCluster) + GET_CORE_ID (MpId);
```

which **assumes every cluster has the same number of cores**. With 8 cores over 3
clusters that division is not integral (8/3 = 2), so the linear position of the MID
and BIG clusters would be computed wrong — and that index selects the per-core stack
and structure slots in the SEC entry path. A wrong index is a memory bug in the
bootstrapping, not a cosmetic one.

*(inference)* the 9820 lib must therefore carry an **explicit** cluster/core table
rather than the division. The same applies to the MADT: Windows' processor count and
the "I have CPU cores" bar depend on the MPIDR affinities published there, and the
9810 publishes only the boot core.

*(inference)* the two Samsung-implemented cores (`0x53`/'S', part `0x003`) are the
SoC's own design, so their MPIDR affinities and any PSCI behaviour have to be
confirmed on the device rather than assumed to match ARM's A55/A76-class cores.

## 16. The CPU topology, CLOSED by two independent reads (this corrects §15)

§15 inferred three MPIDR clusters from the power-domain names `LITTLE`/`MID`/`BIG`.
That inference was **wrong**, and the device tree settles it — node names are
readable even though property contents are not, and DT CPU nodes are named by
MPIDR.

**Live S10e**, `/proc/device-tree/cpus/`:

```
cpu@0000 cpu@0001 cpu@0002 cpu@0003 cpu@0004 cpu@0005
cpu@0100 cpu@0101
```

**Vendor DT**, `arch/arm64/boot/dts/exynos/exynos9820.dts` — same eight nodes,
with the `reg` and identity fields the phone cannot show:

```
cpu@0000  reg = <0x0 0x0>    compatible = "arm,ananke",    "arm,armv8"
cpu@0004  reg = <0x0 0x4>    compatible = "arm,prometheus", "arm,armv8"
cpu@0100  reg = <0x0 0x100>  compatible = "arm,cheetah",    "arm,armv8"
```

Cross-checked against `/proc/cpuinfo` on the phone, which enumerates in the same
order:

| processor | MPIDR | DT identity | `/proc/cpuinfo` implementer/part |
|---|---|---|---|
| 0–3 | `0x000`–`0x005`… i.e. 0x0, 0x1, 0x2, 0x3 | `ananke` | 0x41 / 0xd05 (Cortex-A55 class) |
| 4–5 | 0x4, 0x5 | `prometheus` | 0x41 / 0xd0a |
| 6–7 | **0x100, 0x101** | `cheetah` | 0x53 (Samsung) / 0x003 |

*(fact)* **Two MPIDR clusters, sized 6 + 2** — Aff1 = 0 for the six ARM cores and
Aff1 = 1 for the two Samsung cores. `enable-method = "psci"` on every node, so
secondary cores come up over PSCI, as the 9810 assumed.

*(fact)* there are still **three efficiency classes** — `ananke` (LITTLE),
`prometheus` (MID), `cheetah` (BIG) — and the live platform tree's three power
domains `10090000.LITTLE` / `MID` / `BIG` match those three names. So the
"three clusters" idea was right about *efficiency* and wrong about *affinity*.

*(fact)* the DT also carries a `cpu-map` node with `cluster0` → `coregroup0` →
`core0…`, i.e. Samsung groups the two ARM coregroups inside one cluster.

### What this changes in the code

- `PcdClusterCount` is **2**, not 3.
- The explicit core-position table is **{6, 2}**, not {4, 2, 2} — and the 9810's
  division is still wrong, because 8/2 = 4 would put the two BIG cores at
  positions 4 and 5, which are the MID cores' slots.
- The MADT's MPIDR column becomes 0x000…0x005 then 0x100, 0x101 — the previous
  draft's 0x200/0x201 does not exist on this SoC.
- Efficiency classes stay three: BIG = 0, MID = 1, LITTLE = 2.

## 17. M8 — DRAM: bank0 is not 2 GiB of usable RAM, and the reserved list is published

**The SoC-level vendor DT declares three 2 GiB banks:**

```
memory@80000000    reg = <0x0 0x80000000 0x80000000>
memory@880000000   reg = <0x8 0x80000000 0x80000000>
memory@900000000   reg = <0x9 0x00000000 0x80000000>
```

**The live S10e publishes five nodes** — the low bank is split by the board-level
file at the SAME split points the 9810's starlte map used:

```
memory@80000000   memory@C0000000   memory@E1900000   memory@880000000   memory@900000000
```

*(fact)* the bases are a read; the sizes are not (property contents are blocked).
*(inference)* the 9810's `0xC0000000 + 0x20000000` and `0xE1900000 + 0x1E700000`
tile the gap exactly — `0xC0000000 + 0x21900000 = 0xE1900000` — so the same
split is the likely one here. Kept as a candidate, not a constant.

**What is reserved inside the low bank** (`exynos9820-rmem_s5100_dual.dtsi`, the
file the S10e's board `.dts` includes):

```
modem_spare       0xD0000000 + 0x1600000
modem_if2         0xD1600000 + 0x4200000
cp2cp_shm         0xD5800000 + 0x4000000
s5100_msi         0xD9800000 + 0x0001000
modem_if          0xF0000000 + 0xB900000     (ends 0xFB900000)
cp_ram_logging    0xFB900000 + 0x2000000     (ends 0xFD900000)
```

*(fact)* those are contiguous from **0xD0000000 to 0xFD900000** — 729 MB in the
middle of bank0 that belongs to the modem/CP stack, not to the OS.

**Consequence for the memory map:** declaring bank0 as one 2 GiB cached region
would hand the modem's memory to Windows as conventional RAM. The conservative
mapping the code uses now is `0x80000000 + 0x50000000` (up to the first
reserved region), which is 1.25 GiB — and the tail `0xFD900000..0x100000000`
(39 MB) plus the two high 2 GiB banks are what has to be added once the split
sizes are confirmed.

**Also from the DT:** `chosen` carries `initrd-start = 0x84000000`,
`initrd-end = 0x841fffff` — 32 MB of the front of bank0 is taken by the Android
initrd, which is the same reason the 9810 flow loads the firmware at
`0x80080000` rather than at the base.
