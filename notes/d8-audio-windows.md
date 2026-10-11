# D8 — Windows-side prior art: audio + hardware buttons (S10e / Exynos 9820 WoA)

Companion to `notes/d8-audio.md` (device-tree facts) and `notes/d8-audio-linux.md`.
Started 2026-10-11. Every claim carries a source URL; **(inference)** = my read.
Questions: Q1 in-box audio driver architecture; Q2 WoA prior art (Mu-Silicium,
Renegade, Cirrus/SOF); Q3 hardware buttons (PNP0C0C, VHF, PNP0D40);
Q4 usbaudio2.sys on ARM64; Q5 firmware licensing.

## Progress log
- 2026-10-11: file created; Q1 research started.

## Q1 — Windows ARM64 audio driver architecture

### Q1.1 Official samples in microsoft/Windows-driver-samples (FACT, verified via GitHub API)
`audio/` contains exactly: `Acx`, `SoundWire`, `simpleaudiosample`, `sysvad`, `tests`.
(https://api.github.com/repos/microsoft/Windows-driver-samples/contents/audio)
- **SimpleAudioSample** = the minimal **portcls (PortCls) WaveRT miniport** sample.
  https://github.com/microsoft/Windows-driver-samples/tree/main/audio/simpleaudiosample
- **SYSVAD** = full "virtual audio device" portcls sample (WaveRT+topology, offload).
  https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad
- **Acx/Samples/AudioCodec** = minimal **ACX** (Audio Class eXtension) KMDF driver sample.
  https://github.com/microsoft/Windows-driver-samples/tree/main/audio/Acx/Samples/AudioCodec
- **SoundWire/Samples/SdcaVad** = SDCA ("Smart Device Class Audio", MIPI SoundWire Device
  Class for Audio) virtual DSP/codec/XU sample; includes `AcpiReader.cpp` (reads ACPI _DSD
  properties to configure an audio DSP). Docs: SDCA = "specification for audio devices that
  operate over SoundWire interfaces" (https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/audiosensors/ns-audiosensors-sdca_endpoint_config),
  MIPI SDCA spec (https://www.mipi.org/specifications/mipi-soundwire-device-class-audio).

### Q1.2 portcls does NOT require PCI (FACT from the sample INF)
SimpleAudioSample.inx (https://raw.githubusercontent.com/microsoft/Windows-driver-samples/main/audio/simpleaudiosample/Source/Main/SimpleAudioSample.inx):
- `Class=MEDIA`, `ClassGuid={4d36e96c-e325-11ce-bfc1-08002be10318}` (Media class, not a special audio class).
- Hardware ID = **`ROOT\SimpleAudioSample`** — a *root-enumerated* device; no PCI, no ACPI.
  Installed with devcon (`devcon install SimpleAudioSample.inf Root\SimpleAudioSample`,
  https://stackoverflow.com/questions/78467724/win-driver-sample-simple-audio-sample).
- `Include=ks.inf,wdmaudio.inf` + `Needs=KS.Registration,WDMAUDIO.Registration` — this is the
  audio registration; **there is no `devaudio.sys`** anywhere in the INF (dead end: the
  "devaudio.sys" guess is wrong; endpoints come from wdmaudio.inf registration +
  AudioEndpointBuilder service + the AddInterface directives below).
- Endpoints: `[...NT.Interfaces] AddInterface=%KSCATEGORY_AUDIO%/%RENDER%/%CAPTURE%/%REALTIME%/%TOPOLOGY%`
  + per-interface `HKR,EP\0,PKEY_AudioEndpoint_Association` etc. → AudioEndpointBuilder creates
  the endpoint devices from these KS interface registrations.
- `KmdfService = SimpleAudioSample` — the portcls miniport **is a KMDF driver** (matches our
  existing KMDF/WDK 26100 ARM64 build setup).
- INF section guard `NT$ARCH$.10.0...22000` → Win11+ (22000), any arch incl. arm64.
- `SignatureAttributes.DRMLevel=1300` + `SignatureAttributes.PETrust=true` — DRM level attrs
  present in the sample (relevance for ARM64 load policy: UNVERIFIED).
- Docs caveat: "Introduction to Port Class" says port drivers "Do not support external buses
  (for example, USB). All port drivers support devices that reside on the system buses
  (PCMCIA, and PCI)" (https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/introduction-to-port-class)
  — legacy wording; the ROOT-enumerated sample proves PCI is not required. **(inference)**
  An ACPI-enumerated PDO (ACPI\EXYN9820-style) with memory-mapped resources should work the
  same way — miniport just MmMapIoSpace's its translated resources — but no official
  ACPI-enumerated portcls example found yet (searching).

### Q1.3 ACX — the modern (Win11) alternative, designed exactly for WoA-style SoC audio
- ACX = "Audio Class eXtension", a **KMDF class extension for audio** ("Portcls audio drivers,
  the current legacy model, are WDM... ACX is built using KMDF"); WaveRT-only streaming;
  coexists with portcls. Explicit goal: multi-stack "DSP, CODEC and AMP functionality
  implemented by different audio technology vendors" — i.e. exactly the ABOX+CS47L93+CS35L40
  topology. https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/acx-audio-class-extensions-overview
- AudioCodec.inf (ACX sample): Class=MEDIA, `ROOT\AudioCodec`, KmdfService, guard
  `NT$ARCH$.10.0...19041` (Win10 19041+).
  https://raw.githubusercontent.com/microsoft/Windows-driver-samples/main/audio/Acx/Samples/AudioCodec/Driver/AudioCodec.inf
- Cirrus Logic's current PC parts (CS35L56 smart amp + CS42L43 codec) "are designed to work
  within Microsoft's ACX framework" + MIPI SoundWire/SDCA — Cirrus press release 2023.
  https://investor.cirrus.com/news-and-events/investor-news/news-details/2023/Cirrus-Logic-Helps-Ease-PC-Industry-Transition-to-New-MIPI-SoundWire-Interface/default.aspx
  **(inference)** ACX is the intended path for ARM64 SoC audio in Win11; portcls remains
  supported (Snapdragon laptops still ship portcls-era stacks — to verify in Q1.4).

## Q2 — WoA prior art

### Q2.1 Mu-Silicium / Project-Silicium (Exynos WoA) — audio NOT working on any Exynos target
Org = `Project-Silicium` (successor of Mu-Silicium). Status page:
https://github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md (fetched 2026-10-11).
- **Galaxy S10 (beyond1lte, Exynos 9820)**: UEFI-only project state "Inactive"; Windows Boot ❌.
  Side Buttons ✅ only at *UEFI* level (firmware consumes them for keycombos).
- **Galaxy S9 (starlte, Exynos 9810)**: Windows PE only; Speakers ❌, Microphone ❌, Side Buttons ❌.
- **Galaxy S20 5G (x1s, Exynos 9830)**: WinPE only; Speakers ❌, Microphone ❌, Side Buttons ❌.
- **Galaxy Tab A 10.1 (gtaxllte, Exynos 7870)**: full Windows boots; "Needs Windows Drivers"
  → https://github.com/AistopGit/windows_oem_samsung_gtaxllte — that repo contains ONLY an
  MSHC (SD host) driver (dwcmshc.sys/inf, verified via GitHub tree API). Speakers ❌, Mic ❌.
- **No audio driver code anywhere in the org**: repos = Mu-Silicium (UEFI), Device-Binaries
  (EFI blobs), Guides, Silicium-ACPI (ASL), Carbon-Hypervisor (S5E9810 hypervisor).
  https://api.github.com/orgs/Project-Silicium/repos
- Contrast (Snapdragon targets in the same status page): Xiaomi Pad 5 (nabu), Xiaomi Mi 9,
  Poco X3 Pro boot Windows with Speakers ✅ / Microphone ✅ / Side Buttons ✅ using the
  community Qualcomm driver pack (e.g. https://github.com/erdilS/Port-Windows-11-Xiaomi-Pad-5/releases/tag/Drivers).
  **(inference)** Their audio works via the Qualcomm ADSP/Q6 Windows stack, which has no
  Exynos equivalent — consistent with our ABOX problem being unsolved by this project.
- Note: Exynos 9820/9810/9830 devices "Only 23H2 Windows Builds and Lower are able to Boot"
  (same Status.md) — relevant to our own target-SKU choice.

### Q2.2 Renegade Project (Snapdragon 835/845/855 WoA) — audio partial, closed Qualcomm stack
- Status matrix (https://renegade-project.tech/en/state, via search-engine snapshot 2026-10-11):
  Audio column: MSM8998/SD835 = **N** (not supported); SDM845 = **P** (problematic/partial);
  SM8150 = **P**; SM8250 = N; SM8350 = N.
- Community: "Many drivers were ported over from snapdragon 855 laptops. Only things that
  aren't working are audio, cameras, sensors, and USB OTG." (OnePlus 7 Pro thread,
  https://www.reddit.com/r/OnePlus7Pro/comments/101fyct/windows_11_with_renegade_project_uefi/)
- Renegade's GitHub org (https://github.com/orgs/renegade-project/repositories) has **0 public
  repos** — code self-hosted; no open audio driver found there (dead end).
- The audio that DOES work on Snapdragon WoA targets uses the OEM/Qualcomm ADSP (Q6) AVStream
  driver stack distributed via Windows Update for Snapdragon laptops (binary, not open source;
  Qualcomm publishes no public driver downloads for it — "Qualcomm does not currently offer
  drivers ... for other products", https://www.qualcomm.com/drivers).
  **(inference)** Not reusable on Exynos ABOX: it talks to Qualcomm's ADSP/Q6 IPC, not to
  Samsung's Calliope DSP; only the general architecture (DSP-backed AVStream/ACX miniport +
  firmware IPC) transfers.

### Q2.2b Renegade UEFI (mu_andromeda_platforms, WOA-Project fork) — button handling is UEFI-side
- https://github.com/WOA-Project/mu_andromeda_platforms contains `Platforms/AndromedaPkg/Driver/
  GpioButtons/GpioButtons.c` (UEFI GPIO buttons driver) — buttons handled in firmware for boot
  keycombos, NOT exposed to Windows via a documented ACPI button device in that repo (tree scan
  2026-10-11). No DSDT .asl sources for buttons/audio in that repo (uses device's own ACPI).

### Q2.3 Cirrus Logic Windows drivers for Madera-family codecs (FACTS from cirrus.com)
- Cirrus publishes Windows "Device Packs" behind a click-through license gate
  (`/support/software/lic?uri=...`) on https://www.cirrus.com/support/technologies/codecs:
  - **Device Pack for CS47L24**, Mar 2023, v1.1.8, 12.8 MB (`CS47L24_Setup_RevA_1_1_8.zip`)
  - **Device Pack for CS47L35**, Feb 2023, v2.2.16 (`CS47L35_Setup_RevA_2_2_16.zip`)
  - **CS47L63 Setup**, Jun 2021, RevA 1.0.57
  - **Device Pack for CS47L90**, Sep 2017, v1.3.22 (`CS47L90_Setup_RevA_1_3_22.zip`)
  CS47L90/24/35/63 are Madera-family parts (same register family as our CS47L93).
  **No public pack for CS47L93 or CS35L40** (smartphone parts — OEM/NDA only). **(fact: absent
  from the list; inference: NDA)** Binary zips, source not stated; arch (x64 vs ARM64)
  UNVERIFIED (not downloaded).
- Microsoft Update Catalog carries "Cirrus Logic, Inc. **AudioProcessingObject** Driver Update"
  (v2.49.60878.1 etc., Win11 24H2/25H2 driver servicing) — i.e. Cirrus ships APOs + driver
  packages through WU for Windows laptops (Surface-class hardware uses Cirrus codecs).
  https://www.catalog.update.microsoft.com/Search.aspx?q=cirrus
  (search "Cirrus Logic CS47L" → 0 results; "CS47L92" → 0 results — the codec driver itself is
  not catalog-searchable by part number; dead end for a public ARM64 Madera driver.)
- Open source from Cirrus: **github.com/CirrusLogic/mcu-drivers** — Apache-2.0 MCU (bare-metal
  C) drivers for cs35l41, cs35l42, cs35l56, cs47l15, cs47l35, cs47l63, cs40l* + tools.
  https://github.com/CirrusLogic/mcu-drivers — NOT Windows drivers, but Apache-2.0 register
  sequences for Madera-family parts are directly reusable in a KMDF driver.
  (CS35L40 and CS47L93 specifically: not in that repo's list — verify against Linux `madera`
  driver instead, see notes/d8-audio-linux.md.)
- Cirrus' modern PC direction = MIPI SoundWire + SDCA + **Microsoft ACX framework**
  (CS35L56 + CS42L43 press release, 2023):
  https://investor.cirrus.com/news-and-events/investor-news/news-details/2023/Cirrus-Logic-Helps-Ease-PC-Industry-Transition-to-New-MIPI-SoundWire-Interface/default.aspx

### Q2.4 SOF (Sound Open Firmware) on Windows
- SOF firmware is BSD-3; "SOF drivers BSD/MIT licensed firmware and BSD/GPL licensed drivers"
  (https://sofproject.org, https://github.com/thesofproject/sof README).
- SOF maintainer (plbossart, Intel, Sep 2024): "**There is already a Windows driver based on
  SOF**, so it's possible" — the Windows SOF driver exists but is NOT public (Intel internal /
  OEM binary). https://github.com/orgs/thesofproject/discussions/9506
  Maintainer lgirdwood adds the Windows driver "is probably a good reference" for ports.
- **(inference)** No open-source Windows SOF/Madera driver exists to fork; the closest open
  building blocks for our ABOX+Madera design are: (a) Cirrus mcu-drivers (Apache-2.0),
  (b) Linux madera/cs35l41 drivers (GPL — cannot copy into a Windows binary without
  respecting GPL? actually GPL kernel code linking to a proprietary driver is the same
  userspace-driver question; kernel-internal port = GPL obligations), (c) Microsoft ACX +
  SDCA samples as the framework skeleton.

## Q5 — Licensing / redistribution of firmware blobs (partial, see also Q2.3)

### Q5.1 Cirrus Logic software/firmware — explicit click-through license (FACT, full text read 2026-10-11)
Every Cirrus download behind `/support/software/lic?uri=...` (incl. the Madera "Device Packs")
is governed by Cirrus' **Software Tools License Agreement** (Last Updated June 15, 2021):
https://www.cirrus.com/support/software/lic?uri=/pubs/software/CS47L90_Setup_RevA_1_3_22.zip
Key clauses (paraphrased, quotes from that page):
- Grant: "personal, non-exclusive, non-transferable … internal use" only for the "Purpose" =
  "enabling the use of a CIRRUS LOGIC semiconductor device in Company's end product(s)".
- "Supply of the Software Tools … does not convey a License nor imply any rights to implement
  or distribute any Software Tools **or the output image file in any finished end user
  product**". Binary images of Cirrus firmware embedded in a Cirrus device are governed by a
  separate "Software License Agreement".
- No decompiling/reverse-engineering, no distribution, no porting, export-control clauses.
- Open-source parts inside tools keep their OSS licenses.
**(inference)** The wmfw amp firmware files on the S10e (`/vendor/firmware/cs35l41-*-wmfw.bin`
etc.) are Cirrus-licensed binaries; copying them into a Windows partition on the SAME device
is arguably "internal use … enabling the use of a Cirrus device" by the device owner, but
**redistributing them in a public driver package is not clearly permitted**. Mark: legally
uncertain, no explicit end-user clause found. Not legal advice.

### Q5.2 Samsung Calliope/ABOX firmware — see "Q5 (cont.)" below

## Q3 — Hardware buttons on Windows ARM64

### Q3.1 THE answer: ACPI0011 generic button device + inbox Hidinterrupt.sys (FACT)
Microsoft Learn, "ACPI Button Device" (updated 2024-01-11):
https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/acpi-button-device
- "The generic button device is a standard device for reporting button events through hardware
  interrupts, and mapping those interrupts to specific usages defined in the HID specification."
- "ACPI device Id of a generic button device is **ACPI0011**. Windows loads the
  Microsoft-provided **in-box driver, Hidinterrupt.sys** for that device."
- Official ASL sample (for "Windows 10 desktop editions") declares Device(BTNS) with
  `_HID "ACPI0011"`, `_CRS` = one `GpioInt(Edge, ActiveBoth, …)` per button, and `_DSD` with
  UUID **FA6BD625-9CE8-470D-A2C7-B3CA36C4282E** mapping each _CRS interrupt index to a HID usage:
  - Power button → Usage Page 0x01 (Generic Desktop), Usage **0x81 "System Power Down"**
    (confirms the 0x0081 guess — page 0x01, not "Consumer").
  - Volume Up → Page **0x0C** (Consumer), Usage **0xE9 "Volume Increment"**;
    Volume Down → 0x0C/**0xEA "Volume Decrement"** — inbox, no extra software.
  - Windows/Home → Page 0x07 usage 0xE3 (Keyboard Left GUI); Rotation Lock → 0x01/0xCA;
    (phone sample also shows Back 0x0C/0x224, Search 0x0C/0x221, camera page 0x90).
- Collection wrapper: top-level collection Page 0x01 Usage 0x0D ("Portable Device Control").
**(design inference)** S10e mapping: one ACPI0011 device, 4 GpioInts on our ALIVE GPIO
controller: gpa0-3→0x0C/0xE9, gpa0-4→0x0C/0xEA, gpa2-4→0x01/0x81, gpa0-6 (Bixby)→0x07/0xE3
or 0x0C/0x221. Requires only a GpioClx-compatible GPIO controller driver for ALIVE — no VHF,
no custom HID driver.

### Q3.2 SoC bring-up button requirements (FACT)
"Other ACPI Namespace Objects" (SoC/ARM64 bring-up guide, updated 2023-03-23):
https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/other-acpi-namespace-objects
- Two supported power-button styles: ACPI **Control Method Power Button** (ACPI 5.0 §4.8.2.2.1.2,
  i.e. PNP0C0C) implemented "using GPIO-Signaled ACPI Events (§5.6.5)": GpioInt Exclusive on the
  pin, listed in the GPIO controller's **_AEI**, event method notifies the in-box Control Method
  Button driver. → **No EC required on ARM64**; a firmware/ACPI-side GPIO notify works.
- **Windows-compatible five-button array**: vendor `_HID` + `_CID "PNP0C40"`, five dedicated
  interrupt pins, Edge/ActiveBoth; _CRS order: Power (ExclusiveAndWake), Windows
  (ExclusiveAndWake), Volume Up (Exclusive), Volume Down (Exclusive), Rotation Lock (optional,
  Exclusive). "Supported by the Windows **inbox button driver**". A _DSM exists to remap
  (see "Windows Button Array Device-Specific Method (_DSM)" page).
- Hard requirement for ActiveBoth buttons: "GPIO ActiveBoth interrupts must be connected to an
  **on-SoC GPIO controller** (not an SPB-connected GPIO controller)"; the controller "must
  support level-mode interrupts and dynamic polarity reprogramming" and "must use ActiveBoth
  emulation provided by the GPIO framework extension (**GpioClx**)".
  → our ALIVE GPIO driver must implement GpioClx (prior art: official sample below).
- Also in that doc: XHCI _CID **PNP0D10** (matches our DSDT's XHC0), HID-over-I2C _CID **PNP0C50**
  (relevant to S6SY761Touch), SD host _CID PNP0D40.

### Q3.3 GPIO controller prior art for GpioClx (FACT)
Official samples, microsoft/Windows-driver-samples `gpio/samples/`
(https://github.com/microsoft/Windows-driver-samples/tree/main/gpio/samples):
- **SimGpio**: "GPIO controller driver that communicates with GpioClx … written for a
  hypothetical **memory-mapped GPIO controller**" + ASL example — exactly the ALIVE pattern.
- **SimGpio_I2C**: same for an I2C-connected controller; **SimDevice kmdf/umdf**: client driver
  consuming GpioInt/GpioIo resources + ACPI ASL (`GpioSampleKMDF.asl`).
- GpioClx = inbox "GPIO framework extension" (GPIOClx0101.sys):
  https://learn.microsoft.com/en-us/windows-hardware/drivers/gpio/gpio-driver-support-overview

### Q3.4 VHF (Virtual Hardware Framework) — usable but NOT needed for buttons (FACT)
- Docs: https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-
  "Starting in Windows 10 … write a HID source driver using KMDF or WDM"; in-box lower-filter
  driver = **vhf.sys** (NOT "vhid.sys" — vhid.sys is the separate hidden-HID enumerator used
  by hidi2c; correction to the task prompt), static lib **vhfkm.lib** in the WDK, DDI Vhf.h
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/), HID class pair
  hidclass.sys+mshidkmdf.sys on top. vhf.sys present in Windows 10 AND 11 inbox
  (https://batcmd.com/windows/11/services/vhf, https://revertservice.com/11/vhf — third-party
  confirmations of inbox path %WinDir%\System32\drivers\vhf.sys).
- **No official VHF sample in microsoft/Windows-driver-samples** — repo has no `vhf` folder
  (verified via GitHub API tree listing 2026-10-11); GitHub issue
  https://github.com/microsoft/Windows-driver-samples/issues/154 ("VHF: need example", 2017)
  still open. Dead end. Community KMDF+VHF drivers exist (e.g. haarisxk/Waypoint,
  hulirou/Virtual-keyboard-and-mouse — https://github.com/topics/vhf?l=c).
- ARM64 usability: VHF is an inbox WDK framework with no arch-specific constraint;
  **(inference, high confidence)** works on Win11 ARM64 like on x64. Not directly verified.

### Q3.5 PNP0D40 is NOT a GPIO controller (correction, FACT)
- PNP0D40 = _CID for **SDA-standard-compliant SD Host Controller** (SoC bring-up doc above;
  also https://www.driveridentifier.com/... "Compatible ID: ACPI\PNP0D40 — SDA Standard
  Compliant SD Host Controller").
- msgpiowin32.sys = Microsoft's driver for **Intel Serial IO GPIO host controllers**
  (INT33FC/INT34xx _HIDs) — x86-oriented, not a generic ACPI GPIO consumer
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/general-purpose-i-o--gpio-,
  treexy driver list "Intel Serial IO GPIO Host Controller ACPI\INT3450…").
- Confirmed: **no inbox Windows component maps raw GpioInt ACPI events to volume keys without a
  GPIO controller driver + ACPI0011/PNP0C40 declaration** — the ACPI0011 route in Q3.1 IS the
  native mechanism, but it still needs a GpioClx provider for ALIVE.

### Q3.6 WoA port DSDT prior art for buttons (FACT, from repo scans)
- Xiaomi Mi 9 (cepheus) DSDT in Project-Silicium/Silicium-ACPI
  (https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Platforms/Xiaomi/cepheus/Decompiled/DSDT.dsl):
  **no PNP0C0C**; power/PMIC buttons via `Device (PMBT) { _HID "QCOM0531" }` with multiple
  `GpioInt (Edge, ActiveHigh, …) "\\_SB.PM01"` resources (PMIC-GPIO PON block: power + volume
  keys on Qualcomm PMICs), GPIO controller `QCOM0527` (\_SB.GIO0). Qualcomm vendor driver,
  closed. (grep of the .dsl, 2026-10-11)
- Mu-Silicium Exynos DSDTs (starlte/b4q/q2q): grep found **no PNP0C0C/GpioInt/Volume/audio
  strings** in those files (dead end — their Windows side has no button support, matching
  Status.md "Side Buttons ❌").
- Snapdragon laptops' volume keys: standard keyboard/EC matrix on x86-style; on WoA laptops
  (Snapdragon X) buttons are handled via ACPI GPIO events / HID — **(inference, not verified
  from a primary source this session)**.

### Q1.4 portcls/ACX on ARM64 — existence proof (FACT)
- The in-box USB Audio 2.0 class driver **usbaudio2.sys is itself "a WaveRT audio port class
  miniport"** shipped with Windows 10 1703+ (https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers)
  and is in-box for Windows 11 (https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/supported-usb-classes).
  Community: "USB Audio Class 2 in-box driver, is the same code on x64 and Arm64"
  (RME forum, https://forum.rme-audio.de/viewtopic.php?id=35352).
  → **portcls.sys is present and used on Windows ARM64** (usbaudio2 loads on every Snapdragon
  laptop). **(inference from these two facts)**
- ACX AudioCodec sample solution builds `Release|ARM64` (sln contents,
  https://github.com/microsoft/Windows-driver-samples/blob/main/audio/Acx/Samples/AudioCodec/Driver/AudioCodec.sln).
- Microsoft is rewriting the USB Audio 2.0 class driver "using the ACX framework" with
  WaveRT+ASIO for Arm64 (announced Snapdragon Summit 2024; in-box rollout) —
  https://devblogs.microsoft.com/windows-music-dev/making-music-on-windows and
  https://devblogs.microsoft.com/windows-music-dev/fall-2025-windows-musician-technology-and-arm64-update
  → ACX is definitely shipping on ARM64 Windows 11.
- "mshack" sample: **not found** in current docs; archived WDK sample list = Msvad, Ac97,
  Dmusuart, ddksynth, Fmsynth, Audio Adapters, MsFilter, gsm610
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/sample-audio-drivers).
  Dead end for "mshack"; the relevant legacy Microsoft HD-Audio portcls miniport in-box is the
  UAA "Microsoft High Definition Audio" function driver (name MsHdaUaa.sys — UNVERIFIED this
  session, flag for follow-up). No official ACPI-enumerated memory-mapped portcls *sample*
  exists; SimpleAudioSample (ROOT) + SDCA (SOUNDWIRETEST\DSP + ACPI _DSD reading) are the
  closest skeletons.

## Q4 — usbaudio.sys / usbaudio2.sys on Windows 11 ARM64
- FACT: UAC1 in-box = Usbaudio.sys (Media class), UAC2 in-box = usbaudio2.sys since Win10
  1703; both listed for Windows 11 in "USB Device Class Drivers Included in Windows".
  https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/supported-usb-classes
- FACT: same code on Arm64 per RME forum quote (see Q1.4); RME shipped native ARM64 WDM/ASIO
  drivers "fully compatible with Windows 11 ARM 64-bit" for their USB interfaces
  (https://audioxpress.com/news/rme-introduces-windows-11-arm-64-bit-drivers-for-all-rme-usb-interfaces).
- FACT: Microsoft's new in-box low-latency UAC2+ASIO class driver (with Qualcomm/Yamaha)
  "for devices … USB Audio Class 2-compliant, they will work with our new in-box low-latency
  audio driver" on Arm64 (devblogs, URLs above).
- FACT (industry guide): on WoA, audio drivers must be native Arm64 (no emulation in kernel);
  USB class audio works, vendor x64 drivers don't install
  (https://www.sweetwater.com/sweetcare/articles/windows-on-arm-compatibility-guide).
- Community WoA confirmation: Renegade/Mu-Silicium status pages don't explicitly test USB audio
  dongles; Snapdragon WoA laptops demonstrably run USB audio interfaces (RME/Audient/Focusrite
  ARM64 support pages). **(inference)** A USB-C audio dongle is a viable interim D8 fallback on
  our XHC0 (PNP0D10) once USB host works — zero driver work needed.

### Q3.7 HID usage confirmations + terminology corrections (FACT)
- USB HID Usage Tables v1.12 (https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf):
  Generic Desktop page: **0x80 System Control (CA), 0x81 System Power Down, 0x82 System Sleep,
  0x83 System Wake Up** → the task's "0x0081 System Power Down" is CORRECT (page 0x01).
  Consumer page: **0xE9 Volume Increment, 0xEA Volume Decrement, 0xE2 Mute** (§15.9.1).
- Windows HID Architecture doc's class-driver table: page 0x0001 usage 0x0080 "System controls
  (Power)" and page 0x000C usage 0x0001 "Consumer controls" are handled by the stock HID class
  stack (Shared, no minidriver) — https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture
  → a plain HID device (USB/BLE/VHF-injected) with Consumer 0xE9/0xEA reports changes system
  volume with no extra software (this is how every USB keyboard's media keys work; corroborated
  by countless descriptor examples, e.g. https://notes.iopush.net/blog/2016/custom-usb-hid-device-descriptor-media-keyboard).
- **"HID-prox" is NOT the power button**: in Windows hardware docs the HID proximity/presence
  sensor is a Modern-Standby wake/lock feature
  (https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/sensors-presence-sensors);
  in the security industry "HID prox" means proximity access cards. The power-button mechanism
  is the HID System Power Down usage (0x01/0x81) via ACPI0011/Hidinterrupt.sys or a HID
  keyboard-style System Control collection. Terminology correction for the plan.

## Q5 (cont.)

### Q5.2 Samsung ABOX/Calliope side (FACTS + honest uncertainty)
- The S10-series **kernel driver** for ABOX is GPL-2.0: `sound/soc/samsung/abox/abox_core.c`
  in the exynos9820 kernel release carries "Copyright (c) 2018 Samsung Electronics Co. Ltd. …
  GNU General Public License version 2"
  (https://github.com/LineageOS/android_kernel_samsung_exynos9820, master, fetched 2026-10-11).
  → register/IPC knowledge is legally usable as reference; a derivative Windows driver would
  inherit GPL obligations (fine for this repo's licensing posture, but MS-signing is separate).
- The **DSP firmware blobs are proprietary**: LineageOS's `proprietary-files.txt` for starlte
  lists `/firmware/calliope_dram.bin`, `vendor/firmware/calliope_sram.bin`, `calliope_iva.bin`
  as *proprietary files* (https://github.com/LineageOS/android_device_samsung_starlte/blob/lineage-20/proprietary-files.txt).
  They are NOT in Samsung's opensource kernel releases (opensource.samsung.com releases are
  GPL kernel source; Samsung's obligation under GPLv2 covers the kernel only —
  https://www.androidpolice.com/samsung-releases-galaxy-s22-kernel-source).
- No Samsung public license authorizing redistribution of device firmware was found (dead end);
  Samsung's Android end-user software agreement (shipped with the device) restricts copying the
  device software. **(inference)** Extracting the blobs from OUR OWN device and using them on
  that same device is the hobbyist-standard position (analogous to Linux distros NOT shipping
  them but users extracting locally); **redistributing them in the repo/driver package is
  legally risky** — keep them out of git, fetch from the device at install time.
- Contrast — Cirrus' own amp firmware in linux-firmware IS redistributable under
  **LICENSE.cirrus** (WHENCE: "License: Redistributable. See LICENSE.cirrus"; cs35l41 wmfw/bin
  files): license grants "use and integrate" + "reproduce and distribute … in its complete and
  unmodified form, provided all use … is in connection with CIRRUS LOGIC semiconductor devices",
  no modification, must pass the agreement to recipients, no reverse engineering.
  https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/WHENCE
  (lines ~7773+, 8796) and full text
  https://raw.githubusercontent.com/CirrusLogic/linux-firmware/main/LICENSES/LICENSE.cirrus
  **(inference)** those particular files are laptop (HDA/SoundWire) variants; the S10e's
  Samsung-tuned `cs35l41-*-wmfw` on /vendor may be a different deliverable not covered by
  LICENSE.cirrus — UNVERIFIED. Safest: load the wmfw from the device's own /vendor partition
  at runtime instead of bundling it.

## Q1 bottom line — minimum viable in-box-compatible audio design (INFERENCE from Q1/Q2 facts)
1. **Enumeration**: ACPI device in our DSDT (`ACPI\EXYN9820`-style _HID already used for other
   devices) with MMIO `_CRS` for ABOX windows — portcls/ACX miniports don't care about the bus
   (SimpleAudioSample is ROOT-enumerated; SDCA reads ACPI _DSD).
2. **Framework choice**:
   - *portcls WaveRT miniport* (SimpleAudioSample/SYSVAD skeleton) — legacy but fully supported
     on Win11 ARM64 (usbaudio2 proves portcls.sys ships there), KMDF-based, simplest endpoint
     story (ks.inf/wdmaudio.inf + AddInterface).
   - *ACX circuit driver* (AudioCodec skeleton) — Microsoft's intended path for DSP+codec+amp
     multi-stack SoC audio on Win11; newer, less community documentation, builds ARM64.
   Recommendation (inference): start portcls/SimpleAudioSample-derived for rung 1 (raw I2S via
   ABOX UAIF+DMA, codec brought up via I2C from the same driver), keep ACX as rung 2 target.
3. **INF requirements** (from both sample INFs): Class=MEDIA + ClassGuid {4d36e96c-…},
   AddService + KmdfService, Include=ks.inf,wdmaudio.inf Needs=KS.Registration,WDMAUDIO.Registration,
   AddInterface KSCATEGORY_* per endpoint + PKEY_AudioEndpoint_* reg keys; no special audio
   class extension exists for portcls (portcls is called via the miniport's service; ACX uses
   its own class extension per its docs).
4. Correction to Q1.4 note: the in-box generic HD Audio function driver is the "High Definition
   Audio Device" class driver, bus driver hdaudio.sys (per
   https://matthewvaneerde.wordpress.com/2010-08-23/troubleshooting-how-to-install-the-microsoft-hd-audio-class-driver);
   the "MsHdaUaa.sys" name was NOT confirmed this session — treat as unverified folklore.

## Progress log (append)
- 2026-10-11: Q1 (samples, portcls non-PCI, ACX, SDCA, endpoint INF mechanics) — done.
- 2026-10-11: Q2 (Mu-Silicium/Project-Silicium status + repos, Renegade matrix, Cirrus device
  packs + mcu-drivers + WU APOs, SOF Windows status) — done.
- 2026-10-11: Q3 (ACPI0011/Hidinterrupt.sys, PNP0C40 button array, GpioClx samples, VHF facts,
  PNP0D40 correction, cepheus DSDT prior art, HID usage confirmations) — done.
- 2026-10-11: Q4 (usbaudio/usbaudio2 in-box incl. ARM64, new ACX-based UAC2+ASIO driver) — done.
- 2026-10-11: Q5 (Cirrus Software Tools License + LICENSE.cirrus, Samsung GPL driver vs
  proprietary calliope blobs) — done, with UNVERIFIED items marked.

## Dead ends recorded
- `devaudio.sys` — does not exist in the audio INF world (mechanism is wdmaudio.inf +
  AudioEndpointBuilder + AddInterface).
- "mshack" WDK sample — not in any current sample list.
- Official VHF sample in windows-driver-samples — none (issue #154 open since 2017).
- PNP0D40 as GPIO controller — wrong; it's the SD host controller _CID.
- Mu-Silicium/Renegade audio drivers — none exist for Exynos; Snapdragon audio is closed binary.
- Microsoft Update Catalog search for "CS47L92"/"Cirrus Logic CS47L" — zero results.
- Renegade-Project GitHub org — 0 public repos (code self-hosted).
- grep.app code search — rate-limited (429), unusable this session.

## Sources
1. https://api.github.com/repos/microsoft/Windows-driver-samples/contents/audio (+ repo tree API calls)
2. https://github.com/microsoft/Windows-driver-samples/tree/main/audio/simpleaudiosample (+ raw SimpleAudioSample.inx)
3. https://github.com/microsoft/Windows-driver-samples/tree/main/audio/Acx/Samples/AudioCodec (+ raw AudioCodec.inf)
4. https://github.com/microsoft/Windows-driver-samples/tree/main/audio/SoundWire (+ raw SdcaVDsp.inx)
5. https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/acx-audio-class-extensions-overview
6. https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/introduction-to-port-class
7. https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/sample-audio-drivers
8. https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers
9. https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/supported-usb-classes
10. https://devblogs.microsoft.com/windows-music-dev/making-music-on-windows
11. https://devblogs.microsoft.com/windows-music-dev/fall-2025-windows-musician-technology-and-arm64-update
12. https://audioxpress.com/news/rme-introduces-windows-11-arm-64-bit-drivers-for-all-rme-usb-interfaces
13. https://forum.rme-audio.de/viewtopic.php?id=35352
14. https://www.sweetwater.com/sweetcare/articles/windows-on-arm-compatibility-guide
15. https://github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md
16. https://api.github.com/orgs/Project-Silicium/repos
17. https://github.com/AistopGit/windows_oem_samsung_gtaxllte (tree API)
18. https://renegade-project.tech/en/state
19. https://www.reddit.com/r/OnePlus7Pro/comments/101fyct/windows_11_with_renegade_project_uefi/
20. https://github.com/WOA-Project/mu_andromeda_platforms (tree API)
21. https://www.cirrus.com/support/technologies/codecs
22. https://www.cirrus.com/support/software/lic?uri=/pubs/software/CS47L90_Setup_RevA_1_3_22.zip (Software Tools License Agreement)
23. https://github.com/CirrusLogic/mcu-drivers
24. https://investor.cirrus.com/news-and-events/investor-news/news-details/2023/Cirrus-Logic-Helps-Ease-PC-Industry-Transition-to-New-MIPI-SoundWire-Interface/default.aspx
25. https://www.catalog.update.microsoft.com/Search.aspx?q=cirrus (+ CS47L92 / "Cirrus Logic CS47L" searches → 0)
26. https://github.com/orgs/thesofproject/discussions/9506 ; https://sofproject.org ; https://github.com/thesofproject/sof
27. https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/acpi-button-device
28. https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/other-acpi-namespace-objects
29. https://github.com/microsoft/Windows-driver-samples/tree/main/gpio/samples (+ README)
30. https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-
31. https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/
32. https://github.com/microsoft/Windows-driver-samples/issues/154
33. https://batcmd.com/windows/11/services/vhf ; https://revertservice.com/11/vhf
34. https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Platforms/Xiaomi/cepheus/Decompiled/DSDT.dsl
35. https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf
36. https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture
37. https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/sensors-presence-sensors
38. https://github.com/LineageOS/android_kernel_samsung_exynos9820 (abox_core.c header)
39. https://github.com/LineageOS/android_device_samsung_starlte/blob/lineage-20/proprietary-files.txt
40. https://www.androidpolice.com/samsung-releases-galaxy-s22-kernel-source
41. https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/WHENCE
42. https://raw.githubusercontent.com/CirrusLogic/linux-firmware/main/LICENSES/LICENSE.cirrus
43. https://www.mipi.org/specifications/mipi-soundwire-device-class-audio
44. https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/audiosensors/ns-audiosensors-sdca_endpoint_config
45. https://matthewvaneerde.wordpress.com/2010-08-23/troubleshooting-how-to-install-the-microsoft-hd-audio-class-driver
46. https://www.qualcomm.com/drivers
