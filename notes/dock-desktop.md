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
