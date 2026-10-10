# D4 — WiFi: USB adapter choice for the donation build

Plan-of-record task list: [`docs/exynos9820-port/DRIVERS-MILESTONES.md`](../docs/exynos9820-port/DRIVERS-MILESTONES.md) §D4.
Findings appended as discovered; facts carry source URLs; inference marked; dead
ends recorded.

## Closed already (do not re-open without new evidence)

- The S10e's own WLAN is Broadcom **bcmdhd** with no Windows driver and no DT
  device node — `notes/dock-desktop.md` §6, MILESTONES M12. Vendor WiFi on
  Windows is OUT.
- LAN-first is the network plan (M13): a USB ethernet dock device is inbox on
  Windows ARM64 (ASIX/Realtek RTL8153 class). D4 only covers the case where
  WiFi is genuinely wanted.
- Everything hangs off D1 host mode working (xHCI up, non-powered hub budget).

## Questions the research must answer

1. **Inbox**: does Windows 11 ARM64 ship ANY in-box USB WiFi driver? (Check the
   compatibility INF dumps for wlan class + USB PIDs; historically the inbox set
   is PCIe/NDIS-centric.)
2. **Vendor ARM64 drivers**: which USB WiFi chip vendors ship a signed ARM64
   Windows driver today — Realtek (RTL8812AU/RTL8822CU/RTL8192EU/RTL8188GU?),
   MediaTek (MT7601U/MT7610U/MT7612U?), Qualcomm/Atheros (USB AR9271 — last
   driver update?), Ralink RT3070? Look for "ARM64" or "Windows on ARM" in the
   driver packages, and where they are distributed (vendor site, Windows Update
   catalog).
3. **WoA community reports**: Renegade/Mu/Windows-on-Dragonboard threads — which
   specific adapters are confirmed working under Windows 11 ARM64 on Snapdragon
   devices (that is the same driver environment we will present).
4. **802.11 IHV wrinkle**: USB WiFi drivers are NIS + IHV-HAL miniports — any
   ARM64 signing/compat gotchas (e.g. drivers that work on x64 WoA emulation?
   no — kernel drivers never emulate; must be native ARM64).
5. **Recommendation**: name 1-2 concrete adapters (chip + a retail SKU) for the
   donation build, with the driver source URL and the evidence tier (inbox /
   vendor-signed / community-confirmed / unproven).

## Progress log

### 2026-10-10 — Q2: Microsoft Update Catalog probing (Realtek)

- Dead end: catalog title search `Realtek USB Wireless LAN ARM64` → 0 results.
  The catalog does **not** index "ARM64" in search; architecture is only on the
  per-update detail page (`ScopedViewInline.aspx?update_id=…`). Must search by
  chip name and check each detail page. (fact)
  https://www.catalog.update.microsoft.com/Search.aspx?q=Realtek%20USB%20Wireless%20LAN%20ARM64
- Catalog search `Realtek 8822CU` → 67 updates; newest "Realtek Semiconductor Corp.
  Driver Update (1030.52.1216.2025)", dated 2026-01-08, for Windows 11 24H2+,
  package sizes 3.3 MB / 2.8 MB (the trailing numbers in the results table are
  byte sizes, not update IDs). Multiple same-version entries
  with different sizes = different arch/platform packages. (fact)
  https://www.catalog.update.microsoft.com/Search.aspx?q=Realtek+8822CU
- Catalog search `8812AU` → 141 updates, same newest Realtek bundle versions
  (1030.52.1216.2025 / 1030.52.731.2025 / 1030.44.1014.2024). So the 8812AU and
  8822CU are covered by one big rtwlan-family package on WU. (fact)
  https://www.catalog.update.microsoft.com/Search.aspx?q=8812AU
- Checked Architecture field on 12 detail pages of that Realtek bundle (versions
  1030.44.1014.2024, 1030.52.731.2025, 1030.52.1216.2025 — the rtwlanu/rtkusb
  family INF covering RTL8188EU/8812AU/8814AU/8821AU/8822CU/8821CU/8192EU/8723DU/8188GU
  and D-Link/TP-Link/ASUS/Edimax retail SKUs): **every entry is X86 or AMD64 —
  no ARM64**. (fact, checked via curl of ScopedViewInline.aspx?updateid=… for
  GUIDs 3ed4383c…, 264dd32e…, 2fea2bb5…, 8edd6620…, 206c53ad…, 13075756…,
  88bc96b1…, 531f2ae7…, d2fde238…, 008910f0…, b1aaf39a…, a28694d6…)
  Example: https://www.catalog.update.microsoft.com/ScopedViewInline.aspx?updateid=70d9ffac-71bd-4f55-9ba1-117d9d70606a (AMD64)
  Caveat: only page 1 of 3 for "Realtek 8822CU" (67 total) was enumerated; pages
  2-3 not yet checked for ARM64 entries. (inference: unlikely, WU driver packages
  are per-arch and Realtek has not shipped ARM64 rtwlan to date)

### 2026-10-10 — Q3: community threads (first pass)

- Broadcom community forum, on WoA networking generally: "the primary bottleneck
  for WoA networking is the lack of native Arm64-compiled drivers for retail USB
  wireless chipsets." (fact — quoted sentiment, community source)
  https://community.broadcom.com/vmware-cloud-foundation/discussion/windows-11-not-detecting-wifi-adapter
- Parallels forum (Windows ARM VM users): "There seem to be no USB Wireless
  adapters with Windows ARM64 drivers. Those who advertise Plug n Play
  functionality usually have drivers [for x86/x64 only]" and another report:
  adapter recognized as USB WiFi device but "no ARM64 drivers for it (the last
  one available is for Windows 8 64-bit)". (fact — community reports)
  https://forum.parallels.com/threads/anybody-get-a-usb-wifi-adapter-to-work.368139/
  https://forum.parallels.com/threads/seeking-usb-wi-fi-adapter-that-works-inside-windows-arm.361960/
- Qualcomm support forum (Snapdragon laptop owner): "not a single USB Wi-Fi
  dongle works or offers drivers for ARM Windows with Snapdragon." (fact —
  community report, 2024-era)
  https://mysupport.qualcomm.com/supportforums/s/question/0D5dK00000D8EOUSA3/is-there-any-compatible-usb-wifi-adapter-for-windows-arm-with-snapdragon-processor
- Microsoft Q&A "WIFI USB Dongle Windows 11 ARM64 compatible" (2023-12): no
  working adapter named; the only answer links the **Windows IoT Core hardware
  compatibility list WiFi dongles** page — which is ARM32 IoT Core, not
  Windows 11 ARM64. Dead end for our purpose but documents the confusion. (fact)
  https://learn.microsoft.com/en-us/answers/questions/4073368/wifi-usb-dongle-windows-11-arm64-compatible
  https://learn.microsoft.com/en-us/previous-versions/windows/iot-core/learn-about-hardware/hardwarecompatlist#WiFi-Dongles
- Reddit r/WindowsHelp "Any USB WiFi Adapters with Native Windows 11 on ARM" —
  fetch returned empty (reddit blocks); retry via old.reddit.

### 2026-10-10 — Q1: inbox drivers (primary source) + catalog chip scan

- **Microsoft Learn "Manage inbox network drivers" (Windows 11, page dated
  2021-06, still the current doc)**: the complete WiFi FOD/inbox table lists
  Broadcom/Intel/Qualcomm/Ralink/Realtek/Marvell WLAN drivers — **every entry is
  Arch=Amd64 or x86; there is no ARM64 inbox WiFi driver**, and every listed
  driver is a PCIe/legacy miniport (netrtwlane*, netr28x, athw*, Qcamain*,
  net8187se/8192se/819xp/8185 = PCIe). **No USB WiFi class driver at all**
  (no netr28u, no netrtuwlanu, no mt7601u in the inbox set). (fact)
  https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/inbox-network-drivers?view=windows-11
  Caveat (inference): doc is 2021-era and x86-OEM-focused; Windows 11 ARM64
  images could carry ARM64 inbox NIC drivers not in this table — but the
  community evidence below (RPi WoA team, Q&A posters) consistently says no
  in-box USB WiFi exists on WoA, so the doc's negative is corroborated.
- Microsoft Q&A "USB-WIFI Driver for Windows 10 on ARM64" (2021, WoA on RPi400):
  "there is not a single WIFI USB device supported by default on Windows 10 on
  ARM64"; also notes Windows 10 IoT Core's ARM32 WiFi dongle drivers **cannot be
  installed on ARM64** ("ARM32 drivers can't be installed on an ARM64 image or
  vice versa"). (fact — community + corroborated by worproject README)
  https://learn.microsoft.com/en-us/answers/questions/3802496/usb-wifi-driver-for-windows-10-on-arm64
- **worproject/RPi-Windows-Drivers README** (the WoA-on-Raspberry-Pi driver
  team): (fact)
  https://github.com/worproject/RPi-Windows-Drivers
  - "most of the drivers require the *Test Signing mode* to be enabled"
  - "ARM32 drivers can't be installed on an ARM64 image or vice versa"
  - On-board USB-attached WLAN (Cypress CYW43455/CYW43438): "No driver
    available / Not working" — even the one community actively porting has no
    WLAN driver.
  - USB **Ethernet** (LAN9514/LAN7515, ARM64 recompiled by MCCI/Microchip):
    **Working** — corroborates the LAN-first plan (M13) as the reliable path.

### 2026-10-10 — Q2: catalog chip-by-chip scan (complete single-page queries)

- `AR9271`: 8 updates, **all X86/AMD64**, products listed only Windows 7/8/
  Vista/XP — Qualcomm Atheros' AR9271 USB driver on WU is frozen at Win8-era,
  no ARM64. (fact)
  https://www.catalog.update.microsoft.com/Search.aspx?q=AR9271
- `RT5372`: 6 updates, X86/AMD64 only, latest dated 2021-07-19 (v5.1.38.0,
  "Windows 8.1 Drivers" classification). No ARM64. (fact)
  https://www.catalog.update.microsoft.com/Search.aspx?q=RT5372
- `RT3070`: 12 updates, X86/AMD64 only, Win7/8/8.1-era. No ARM64. (fact)
  https://www.catalog.update.microsoft.com/Search.aspx?q=RT3070
- `MT7601U`, `MT7612U`, `MT7610U`: **zero results** — MediaTek USB WiFi has
  never been published to the WU catalog under those names. (fact)
  https://www.catalog.update.microsoft.com/Search.aspx?q=MT7601U
- Method caveat: the catalog "Architecture:" field showed AMD64 even for the
  Marvell AVASTAR "- Net -" entries (used in Surface Pro 7 x64; SPX ARM64
  drivers may be deeper in 159 results). I could not download a CAB to inspect
  INF `[NTarm64]` sections — DownloadDetails.aspx refuses scripted GET/POST
  (error 8DDD0024-style shell page). Dead end recorded; arch-field readings
  above are taken at face value but flagged (inference).
  [RESOLVED below: a positive ARM64 control was later found, validating the
  arch-field readings.]
- `8852BU`/`8852AU` (Realtek Wi-Fi 6 USB): exactly 1 catalog update
  ("Realtek Semiconductor Corp. - Net - 5001.15.129.0"), Architecture: AMD64.
  No ARM64 for the Wi-Fi-6 USB family either. (fact)
  https://www.catalog.update.microsoft.com/Search.aspx?q=8852BU
- **Method validation (positive ARM64 control found)**: catalog search
  `USB GbE Family` → Realtek Driver Update 1153.21.1009.2025 has entries with
  **Architecture: ARM64** (e.g. updateID 4ad46070-792b-4263-9534-56430c040e7f,
  Win11 25H2); `ASIX USB Ethernet` → ASIX - Net - 3.22.3.0 **ARM64**
  (2c71d308-2296-4266-aec7-03ca08f91507). So (a) the Architecture field DOES
  report ARM64 when present — the all-X86/AMD64 WiFi readings above are
  trustworthy; (b) **USB Ethernet vendors (Realtek RTL8153 family, ASIX) ship
  signed ARM64 drivers on Windows Update** — primary-source support for the
  LAN-first plan (M13). (fact)
  https://www.catalog.update.microsoft.com/Search.aspx?q=USB+GbE+Family
  https://www.catalog.update.microsoft.com/Search.aspx?q=ASIX+USB+Ethernet
- Contrast worth stating: Realtek publishes ARM64 packages to WU for USB
  **Ethernet** (1153.x branch) but not for USB **WiFi** (1030.x rtwlan branch,
  5001.x Wi-Fi6 branch). The gap is vendor choice, not a WU/WHCP limitation.
  (inference from the catalog evidence)

### 2026-10-10 — Q2/Q3: WoA driver packs + the PicoPiFi workaround

- **edk2-porting/WOA-Drivers** (the SD835/845 phone driver pack Renegade tells
  us to install — same environment as our S10e): the only WLAN driver in the
  whole tree is `qcwlan850.inf/sys` for the **on-device** QCA6174 (SDIO/PCIe),
  plus Broadcom `bdwlan.*` NVRAM blobs. **No USB WiFi adapter driver ships in
  the community driver pack.** (fact, via GitHub trees API
  https://api.github.com/repos/edk2-porting/WOA-Drivers/git/trees/HEAD?recursive=1)
- **PicoPiFi** (github.com/sidd-kishan/PicoPiFi, 72 stars, updated 2025):
  Raspberry Pi **Pico W** flashed with open firmware presents itself as a
  **USB RNDIS Ethernet gadget + CDC control port**; the Pico W's CYW43439
  joins the WiFi network and bridges frames. Windows sees plain "Remote NDIS
  based Device" — **no WLAN driver, no IHV, no test-signing needed**. (fact)
  https://github.com/sidd-kishan/PicoPiFi
  - Author's write-up: uses the inbox `usbrndis6`/`usb80236.sys` driver
    "readily available on all architectures of Windows — x86, x64, and ARM64 —
    since Windows XP"; claims ~20 mA draw; USB 1.1 = 12 Mbps ceiling;
    2.4 GHz 802.11n 1x1 only; config via local web UI
    (https://sidd-kishan.github.io/PicoPiFi/). (fact for the claims; the 20 mA
    figure is the author's — independent measurements of Pico W put idle at
    ~38-43 mA at 5 V with WiFi power-save, ~52 mA with WiFi+LED active, and a
    ~150 mA startup spike — still far inside a bus-powered hub budget)
    https://medium.com/@aayush.deo1994/a-pico-wi-fi-firmware-revolutionizing-connectivity-in-windows-on-arm-ecosystem-8a6a6bde097c
    https://peppe8o.com/raspberry-pi-pico-w-power-consumption/
    https://forums.raspberrypi.com/viewtopic.php?t=337145
  - Explicitly aimed at "Windows on Raspberry Pi and other windows arm64
    setups like Qualcomm, Nvidia, Samsung" — i.e. exactly our driver
    environment. Instructables build guide:
    https://www.instructables.com/Driverless-USB-WIFI-for-Windows-on-Raspberry-Pi-R-/
- **RNDIS host driver is inbox on all supported Windows** — Microsoft docs
  (updated 2024-09): "Microsoft Windows versions beginning with Windows XP
  include a Remote NDIS driver for USB devices. This NDIS miniport driver,
  Rndismp.sys, is implemented and maintained by Microsoft and is distributed
  as part of all supported Windows versions." (the modern class package is
  usb80236.sys/usbrndis6.inf wrapping Rndismp6.sys). Combined with PicoPiFi's
  WoA reports, the RNDIS path needs no third-party driver on ARM64. (fact)
  https://learn.microsoft.com/en-us/windows-hardware/drivers/network/remote-ndis--rndis-2
- Same Medium article's comparison of WoA WiFi options (community analysis):
  internal WiFi = no driver; **USB WiFi→Ethernet bridge** (~150 mA, >$50,
  occupies 2 ports); **USB tethering from a smartphone** (fast but ties up a
  phone). PicoPiFi chosen as the cheap low-power middle ground. (fact — their
  analysis)
- GitHub topic `windows-on-arm` scan: no repo ships an ARM64 USB-WLAN vendor
  driver; the WiFi gap is served by RNDIS-bridge projects (PicoPiFi and forks).
  (fact — sampled top results)
  https://github.com/topics/windows-on-arm

### 2026-10-10 — Q4: IHV + signing facts

- **Kernel drivers are never emulated on Arm64** — Microsoft WoA FAQ
  (updated 2025-11): "In all cases, kernel-mode drivers and user-mode print
  drivers MUST be built as native Arm64 binaries to work on Arm64 devices.
  Additionally, you cannot execute an x86 or x64 setup program to install an
  Arm64 driver on Arm64 devices." So every x64 Realtek/MediaTek USB WiFi
  package found above is dead on arrival regardless of installer tricks. (fact)
  https://learn.microsoft.com/en-us/windows/arm/faq
- WLAN driver model: a real WiFi adapter needs a Native 802.11 miniport
  (ExtSTA) + IHV Extensions DLL (+ optional UI ext DLL); modern drivers use the
  WDI model ("WLAN Device Driver Interface (WDI) Miniport Driver Design Guide"
  with "IHV component in the WDI model"). All components must be ARM64. (fact)
  https://learn.microsoft.com/en-us/windows-hardware/drivers/network/overview-of-ihv-extensibility
  https://learn.microsoft.com/en-us/windows-hardware/drivers/network/wdi-ihv-component-model
- Signing: attestation signing requires Partner Center registration + an **EV
  code-signing certificate**; and per the current doc (2025-07): "Attestation
  signed drivers that target retail audiences aren't published on Windows
  Update. Support continues for attestation-signed drivers when you're testing
  scenarios with the CoDev or Test Registry Key / Surface SSRK options." So a
  hobbyist ARM64 USB-WiFi port could at best be attestation-signed for manual
  install — not auto-delivered. (fact)
  https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation
- Test-signing fallback: the WoA community driver packs openly rely on it —
  worproject: "most of the drivers require the *Test Signing mode* to be
  enabled". For a **donation build this is disqualifying**: testsigning means
  a desktop watermark, a boot-affecting bcdedit change, broken Secure Boot
  trust, and no path for a non-technical recipient to enable it. Any
  recommendation that needs a test-signed ARM64 WLAN miniport must be flagged
  as such. (fact for the quote; the disqualification is our engineering
  judgement)
  https://github.com/worproject/RPi-Windows-Drivers

### 2026-10-10 — Q5: recommendation

**Bottom line: there is no such thing as a plug-in USB WiFi *adapter* with a
signed ARM64 Windows 11 driver today — inbox, vendor, or Windows Update.**
Evidence tiers:

1. **Inbox**: none (Q1: WiFi FOD table has zero ARM64 entries and zero USB
   WLAN drivers).
2. **Vendor-signed-on-WU**: none for USB WLAN (Q2: every Realtek/Atheros/
   Ralink/MediaTek USB WiFi package is X86/AMD64; MediaTek USB WiFi absent
   from WU entirely). Contrast: USB *Ethernet* (RTL8153, ASIX) does have
   ARM64 WU packages — the gap is vendor choice.
3. **Community-confirmed-on-WoA**: the only class that works is **WiFi→USB
   Ethernet bridging via RNDIS** (inbox `usbrndis6`/`usb80236.sys` on ARM64):
   - **Pick: Raspberry Pi Pico W (RP2040 + CYW43439) flashed with PicoPiFi**
     (~$7, sold at Micro Center / raspberrypi.com / Amazon / Pimoroni).
     Confirmed by its community on Windows-on-RPi (ARM64) and pitched at
     "Qualcomm, Nvidia, Samsung" WoA setups. No driver install, no test
     signing, no IHV. Limits: 2.4 GHz 802.11n 1x1, ≤12 Mbps USB-1.1 link,
     config via web UI. Do **not** buy Pico 2 W (RP2350) — PicoPiFi support
     for it is an open issue (github.com/sidd-kishan/PicoPiFi/issues/2).
   - **Alternate: a USB WiFi travel-router/repeater in RNDIS "client" mode**
     (e.g. GL.iNet AR750S / MT300N-class) — Windows sees USB Ethernet;
     ~100 Mbps; but ~$30-50, bigger, and the RNDIS behavior of specific SKUs
     is UNVERIFIED (inference; would need a bench check before recommending
     to a recipient).
4. **Unproven**: any retail Realtek/MediaTek dongle (8812AU/8822CU/8192EU/
   AR9271/RT3070…) — no ARM64 driver exists, period; do not buy for this
   build.

USB-power fit (non-powered hub, 100 mA/port pre-enumeration, 500 mA total):
- Pico W: ~38-43 mA idle with WiFi power-save, ~52 mA active, ~150 mA
  inrush spike (community measurements above) — comfortably inside the
  budget on a hub port; the spike is at plug-in, before the radio starts.
- Dual-band AC1200+ dongles (8812AU/8814AU class) typically request
  450-500 mA (bMaxPower) — they would eat the whole hub budget even if they
  had drivers. (inference from product classes — UNVERIFIED per-SKU; moot
  since no ARM64 driver exists anyway)
- The PicoPiFi is the only option that leaves hub budget for keyboard/mouse.

## Recommendation

For the donation build: **Raspberry Pi Pico W + PicoPiFi firmware** as the
WiFi option (community-confirmed-on-WoA tier, driverless RNDIS, low power,
~$7), with **USB Ethernet via the dock (M13, vendor-signed-on-WU tier)**
remaining the primary network path. Do not recommend any retail USB WiFi
dongle: none has an ARM64 Windows driver (inbox or vendor), and any
community ARM64 miniport port would need test-signing — unacceptable for a
non-technical recipient.

## Sources

- https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/inbox-network-drivers?view=windows-11
- https://learn.microsoft.com/en-us/windows/arm/faq
- https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation
- https://learn.microsoft.com/en-us/windows-hardware/drivers/network/remote-ndis--rndis-2
- https://learn.microsoft.com/en-us/windows-hardware/drivers/network/overview-of-ihv-extensibility
- https://www.catalog.update.microsoft.com/Search.aspx?q=Realtek+8822CU (and ScopedViewInline detail pages, see above)
- https://www.catalog.update.microsoft.com/Search.aspx?q=8812AU
- https://www.catalog.update.microsoft.com/Search.aspx?q=AR9271
- https://www.catalog.update.microsoft.com/Search.aspx?q=RT5372
- https://www.catalog.update.microsoft.com/Search.aspx?q=RT3070
- https://www.catalog.update.microsoft.com/Search.aspx?q=MT7601U
- https://www.catalog.update.microsoft.com/Search.aspx?q=8852BU
- https://www.catalog.update.microsoft.com/Search.aspx?q=USB+GbE+Family
- https://www.catalog.update.microsoft.com/Search.aspx?q=ASIX+USB+Ethernet
- https://github.com/worproject/RPi-Windows-Drivers
- https://github.com/sidd-kishan/PicoPiFi (+ issues/2, Medium write-up, Instructables guide)
- https://medium.com/@aayush.deo1994/a-pico-wi-fi-firmware-revolutionizing-connectivity-in-windows-on-arm-ecosystem-8a6a6bde097c
- https://peppe8o.com/raspberry-pi-pico-w-power-consumption/
- https://forums.raspberrypi.com/viewtopic.php?t=337145
- https://learn.microsoft.com/en-us/answers/questions/3802496/usb-wifi-driver-for-windows-10-on-arm64
- https://learn.microsoft.com/en-us/answers/questions/4073368/wifi-usb-dongle-windows-11-arm64-compatible
- https://forum.parallels.com/threads/anybody-get-a-usb-wifi-adapter-to-work.368139/
- https://forum.parallels.com/threads/seeking-usb-wi-fi-adapter-that-works-inside-windows-arm.361960/
- https://mysupport.qualcomm.com/supportforums/s/question/0D5dK00000D8EOUSA3/is-there-any-compatible-usb-wifi-adapter-for-windows-arm-with-snapdragon-processor
- https://community.broadcom.com/vmware-cloud-foundation/discussion/windows-11-not-detecting-wifi-adapter
- https://renegade-project.tech/en/install
- https://github.com/edk2-porting/WOA-Drivers (trees API scan)
- https://github.com/topics/windows-on-arm

## Dead ends (do not repeat)

- Catalog title search for "ARM64" / "wireless ARM64" → 0 (arch not indexed).
- Catalog ASP.NET pagination postback (`nextPageLinkText`) from scripts →
  error page; only page 1 of multi-page queries is scriptable.
- Catalog CAB download (`DownloadDetails.aspx`) from scripts → error shell
  page; INF `[NTarm64]` inspection not done.
- Realtek.com public Wi-Fi driver download category → gone (site now
  WU/OEM-distributed).
- Reddit fetches (reddit.com and old.reddit.com) → empty/blocked from this
  environment.
- `MT7601U`/`MT7612U`/`MT7610U`/`RTL8188`/`RTL8852`/`RTL8153` as literal
  catalog queries → 0 hits (use driver-model strings like "8822CU",
  "USB GbE Family" instead).
