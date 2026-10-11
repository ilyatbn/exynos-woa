# M17 - Windows kernel debugging over USB (KDNET over USB device mode)

Goal: debug the Windows kernel live over the S10e's USB-C port (WinDbg on the PC) - for the M4 big cores
hang (notes/m14-smp.md: "Needs kernel-level debugging (KDNET/USB) - parked"), early bugchecks like 0x14C
(notes/m16-usb.md), and driver work generally.

## Known going in (2026-10-11)
- DWC_usb31 1.70a on this SoC has NO xHCI Debug Capability (UEFILOG run 4: DebugCapSupOffset 0xFFFFFFFF,
  notes/m16-usb.md) -> KDUSB over xHCI DbC is impossible. The route is the controller in DEVICE mode as a
  Windows KD transport (KDNET over USB EEM, if Windows supports it on DWC3).
- DSDT/ACPI: no DBG2/SPCR (notes/m10-windows-deploy-plan.md:63).
- Prior art in tree: Star2LtePkg PlatformBootManagerLib UsbDebugHid.c / UsbDebug.c = DWC3 peripheral-mode
  register vocabulary (PRTCAP_DEVICE, EP0 + HID IN), compiled off (MILESTONES.md:284, notes/dock-desktop.md §9).
- Our UEFI already brings DWC3 up in HOST mode (Beyond0LteUsbHostDxe: PHY, clocks, DWC3 core) - reusable.

## Progress log
- 2026-10-11: task opened; research split: (A) Windows side - DBG2 encoding, kd extension module, bcdedit,
  host side; (B) repo inventory of reusable device-mode code.
- Host (x64, 26100) System32 kd modules: kdnet.dll, kdnet_uart16550.dll, kdusb.dll, kdcom.dll, kd_02_<vid>
  (class 02 = network NICs), kd_07_1415 (class 07 serial, Oxford), **kd_0C_8086.dll** (class 0C = serial bus /
  USB, vendor Intel). INFERRED: kd_0C_8086 = KDNET over USB for Intel xDCI, which is a Synopsys DWC3 device
  controller -> Windows likely already has a DWC3 device-mode KD transport. TODO: list System32\kd*.dll in the
  ARM64 image (TWRP read after Setup) - which kd_0C_* it ships decides the DBG2 subtype/vendor.
- 2026-10-11 research (A, web): MS doc "Setting Up USB KDNET EEM Kernel-Mode Debugging (KDNET-EEM-USB)"
  https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/setting-up-kernel-mode-debugging-over-usb-eem-arm-kdnet
  : target needs "a Synopsys USB 3.0 controller connected to a USB type C port", Win10 20H2+; ARM transport
  binary = kd_8003_5143.dll (x64: kd_0C_8086.dll); ARM configured via ACPI DBG2, busparams=1|2|3 = DBG2 entry
  index (entry 0 normally serial); kdnet.exe lists "Device-mode USB controller with Vendor ID: 5143";
  bcdedit: debugtype NET, hostip 169.254.255.255 (used for all USB EEM), port 50000-50039, key, busparams 1;
  host sees "Windows KDNET USB-EEM network adapter"; windbg -k net:port=<p>,key=<k>.
- DBG2 spec https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-debug-port-table :
  Net = PortType 0x8003, Subtype = PCI vendor ID (so kd_8003_5143.dll = Net/0x5143 Qualcomm VID);
  USB 0x8002/0 = xHCI w/ debug interface (needs DbC - not us). Serial 0x8000 subtypes incl. 0x0003 PL011,
  0x000A SAM5250 (Samsung Exynos5250 UART!), 0x000E SBSA, 0x0012 16550 GAS. Device info struct layout
  (Rev 0, GAS array + AddressSize array + NamespaceString, '.' if no ACPI device, optional OemData).
- KDNET extensibility doc https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/how-to-develop-kdnet-extensibility-modules :
  DBG2 (non-PCI) module name = kd_<PortType>_<PortSubtype>.dll; kd_8003_5143.dll is INBOX, "for supporting KDNET on
  Qualcomm USB controllers", 0x5143 = Qualcomm PCI VID. Module has 1 export KdInitializeLibrary, no imports; called
  by winload (size query) and KdInitSystem; KdInitializeController must do full HW init (PHY up, link) itself,
  interrupts masked, polled; memory = block reserved by winload, DMA via KdGetPhysicalAddress. Allowed imports
  include KdMapPhysicalMemory64. Serial/USB DBG2 subtypes must be reserved with MS (kdnet@microsoft.com).
  => a custom kd_8003_XXXX.dll for Exynos is possible in principle (signing: see below).

## Repo inventory (Explore subagent, 2026-10-11; file:line as reported, re-verify before editing)
- 9810 Star2LtePkg PlatformBootManagerLib UsbDebugHid.c (1932 L, compiled off, Star2LtePkg.dsc:26
  STAR2LTE_USB_DEBUG_MODE=0): full DWC3 HID gadget. UsbEnablePhy 601-697 (incl. VBUS fake: UTMI BVALID|
  VBUSVALID BIT4/5 + HSP VBUSVLDEXTSEL|VBUSVLDEXT BIT13/12, 681-693); UsbInitializeController 1577 (GSNPSID,
  DCTL CSFTRST, GCTL CORESOFTRESET + PHY soft resets, PRTCAP=DEVICE 2<<12, DCFG HS addr 0, event buffer,
  EP0/EP3 config, DEVTEN, RUN_STOP); 1 ms polled. DWC3 base 0x10C00000 same on both SoCs; CMU + PHY layout
  (9810 PHY 0x11100000) 9810-specific. UsbDebug.c = read-only CMU snapshots (9810 offsets).
- 9820 Beyond0LteUsbHostDxe: no CMU writes (ACPM-owned, relies on sboot; CTRL_VER audit UsbPhy.c:153-163);
  PHY USB2-only init UsbPhy.c:144-213 deliberately skips the VBUS fake (UsbHw.h:85-93, bits defined).
  Dwc3.c: no CSFTRST in host mode, PRTCAP=HOST :243, event buffer setup reusable. Host-only: PRTCAP_HOST,
  GFLADJ/PORTSC, NDD/XhciDxe, VBUS boost (Pmic.c:380), SOURCE-attach gate.
- ACPI: Beyond0LtePkg/AcpiTables (Dsdt.asl, Fadt/Madt/Pptt/Gtdt.aslc); AcpiTables.inf:19-20 `# TODO: Dbg2.aslc`
  / `Spcr.aslc`; no Dbg2 anywhere in tree (edk2 DynamicTablesPkg Dbg2Generator.c is the only template).
  XHC0 Dsdt.asl:311-334 PNP0D10 0x10C00000/0x8000 GSIV 273, _STA keyed on 'USBH' at 0x8FE0F008.
- Exynos9820.h: USBDRD 0x10C00000/0x10000 :452, USBDRD_PHY 0x10B00000/0x200 :454, COMBO_PHY 0x10AE0000 :456,
  PCS 0x10AF0000 :458, GSIV 273 :460. No USB QCH/gate offsets for 9820.
- Notes: PC host presents Rp -> CCIC settles SINK (d1-usb-host.md:528-534), so device mode needs no VBUS boost;
  no real VBUS-present register known (HSP_TEST linestate = proxy, UsbHw.h:115). Mu-Silicium UsbfnDwc3Dxe works
  only on 9830 (d1-usb-host.md:268-290).
- Public Qualcomm DBG2 example (SDM845, from QC reference ACPI): https://raw.githubusercontent.com/silime/oneplus-sdm845-acpi/HEAD/Common/dbg2.aslc
  (original Renegade copy: https://git.renegade-project.org/?a=blob_plain&f=Common%2Fdbg2.aslc&hb=refs%2Fheads%2Fmaster&p=edk2-sdm845-acpi.git - host down 2026-10-11).
  Entry 0 = UART KDCOM (0x8000/0x0011). Entries 1,2 = KDNET on SNPS controller: PortType 0x8003, Subtype 0x5143,
  DDI Revision field 1 (!= spec's 0), 2 GAS both = DWC3 base (0x0A600000), AddressSize {0xFFFFF, 0x1000},
  Namespace "\_SB.URS0", OemData = ACPI_DEBUG_DEVICE_OEM_DATA_V3: PortType(ctrl type)=5 USBFN_SYNOPSYS
  (1=USBFN ChipIdea-era, 3=USBFN_BUFFERED), Reserved 0, Signature 'FIX2', WriteCount, 8x{BAR idx u8, Phase u8
  (1=after reset,2=config done,3=connect done), rsvd u16, Offset u32, AND u32, OR u32}, then {UsbCore u8, rsvd, rsvd,
  'USBC'}. Writes used: DCFG(0xC700) &= ~7 (force HighSpeed), QC QSCRATCH 0xF8810 |= 0x10100000 (VBUS valid override).
  Table rev 1. => per-SoC PHY/glue quirks are expressed as OEM register writes; module itself is SNPS-generic (INFERRED).
- Serial fallback: MS UART library (WDK kdserial sample, mirrored at https://github.com/HTC-Leo-Revival-Project/KdSerial ,
  "Copyright (c) Microsoft") has sam5250.c = DBG2 serial subtype 0x000A (hardware.c table index 0xA). Registers
  ULCON 0x00/UCON 0x04/UFCON 0x08/UTRSTAT 0x10/UERSTAT 0x14/UFSTAT 0x18/UTXH 0x20/URXH 0x24/UINTP 0x30/UINTM 0x38,
  TX-full = UFSTAT bit24, RX-ready = UTRSTAT bit0 = same s3c/exynos UART layout as 9820 (INFERRED compatible).
  Init writes UCON=0, ULCON=3, UFCON=1, UINTM=0xF, UINTP=0xF, UCON=5. SetBaud does NOTHING ("no way to determine
  reference clock") -> firmware must leave UART clock + UBRDIV/UFRACVAL programmed (115200 from sboot/UEFI).
- Custom kd modules: https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/kdserial-extensibility-code-samples
  - unsigned custom kdcom.dll loads only with testsigning AND boot debugger connected; not used for boot debugger.
- LOCAL VERIFICATION: our own ARM64 22631 image ships it: C:\Users\ilya\code\s10e-artifacts\win-w5\out\esp\EFI\Microsoft\Boot\kd_8003_5143.dll
  (64480 B, PE machine aa64, FileDescription "Qualcomm Network Kernel Debug Extensibility Module", 10.0.22621.2506).
  Same ESP also has kdstub.dll, kdnet_uart16550.dll, kd_07_1415.dll, kd_02_*. (System32 copy not checked - install.wim
  needs elevated DISM; ISO 7z 9.13 too old.) Strings: both "Chipidea Controller" and "Synopsys Controller" backends;
  UsbFnMpSynopsys*; only DWC3 *device* regs logged (GCTL, DCTL, DCFG, GEVNTADR Hi/Lo, GEVNTSIZ, GEVNTCOUNT, DEVTEN/DSTS);
  ResetController/RestartController; "Device Speed HS/SS", BOS SS descriptor; two personalities "KdNet VirtEth" (WINUSB
  compat ID) and "KdNet EEM" (EEM selected via SET_CONTROL_LINE_STATE). NO PHY/clock/Qualcomm-PHY strings ->
  firmware must leave PHY+clocks+glue powered; SoC glue quirks go via DBG2 OEM 'FIX2' writes (INFERRED from strings).
- Opcode scan of kd_8003_5143.dll (movz immediates): uses DWC3 offsets 0xC110 (GCTL), 0xC200 (GUSB2PHYCFG), 0xC2C0
  (GUSB3PIPECTL), 0xC700 (DCFG), 0xC704 (DCTL); NO 0xC120 (GSNPSID) / 0x5533 / 0x3331 immediates -> no visible core-ID
  gate (heuristic, INFERRED) - DWC_usb31 1.70a should not be rejected by ID check.
- Exynos device-mode glue already in tree: Beyond0LteUsbHostDxe/UsbHw.h UTMI_FORCE_VBUSVALID BIT5 / UTMI_FORCE_BVALID
  BIT4 ("device-mode only"), LINKCTRL_DIS_QACT_BVALID BIT9 -> candidates for UEFI pre-set or DBG2 FIX2 writes via BAR1=PHY.
- Fallback serial: 619k ID resistor = JIG_UART_ON on Samsung MUIC (S7 guide https://xdaforums.com/t/guide-samsung-galaxy-s7-uart.3743895/ ,
  jig https://github.com/Otus9051/uart-usb-jig) - S10e Type-C routing not verified.
- KDUSB (DBG2 0x8002/0x0000) requires xHCI DbC -> ruled out (DebugCapSupOffset 0xFFFFFFFF).
- CONCLUSION: route = DBG2 entry 1 PortType 0x8003 Subtype 0x5143 (inbox, MS-signed, Synopsys backend), base
  0x10C00000; firmware leaves PHY/clocks/VBUS-valid set; DCFG forced HS via FIX2; bcdedit net hostip 169.254.255.255
  busparams 1. Serial SAM5250 (0x8000/0x000A) as entry 0 if a UART cable path exists.
- Verified locally (2026-10-11): win-w5 ESP EFI\Microsoft\Boot\kd_8003_5143.dll = PE machine 0xAA64, contains
  'UsbFnMpSynopsys'; kdstub.dll + kd_02_* + kdnet_uart16550 alongside.

## Task plan (one commit each)
- T1 firmware: Beyond0LteUsbHostDxe gets a KD-device mode (flag): PMU de-iso + USB2 PHY init + UTMI/HSP VBUS
  fake, DWC3 left powered in device role, no xHCI registration, no VBUS boost, XHC0 stays hidden.
- T2 ACPI: a DSDT device for the controller in function mode (the DBG2 namespace target).
- T3 ACPI: Dbg2.aslc - one Net entry 0x8003/0x5143, BAR = DWC3, OemData FIX2 (DCFG HS), into AcpiTables.inf.
- T4 Windows: bcdedit /dbgsettings net hostip:169.254.255.255 port:50005 key + busparams 0 + /debug on.
- T5 host: WinDbg -k net:port=50005,key=...; link test (USB 3 Type-A port + C-to-A cable).
- T1 (firmware): BEYOND0LTE_USB_KD_DEVICE 1 in Beyond0LteUsbHostDxe.c -> UsbKdDeviceBringUp: UsbPhyInit (which
  already sets UTMI FORCE_BVALID|VBUSVALID + HSP VBUSVLDEXTSEL|EXT unconditionally, kernel-faithful) +
  Dwc3CoreInit(TRUE): same globals as host, GCTL PRTCAP=DEVICE, DCTL RUN_STOP cleared, returns before GFLADJ /
  UEFI event buffer / xHCI PORTSC. No PMIC/I2C traffic, no NDD/XhciDxe. Status word [2] = 'USBD' (0x44425355)
  on success (DSDT UFN0 in T2). Dwc3HostInit renamed Dwc3CoreInit(Device, &Rev). Build DEBUG exit 0
  (b-kd1-build.log). Device-side HSP tune left at the HOST column (DT device column UNVERIFIED/unused).
- T1 review (d4dac3f): host path identical except the rename (Dwc3CoreInit(FALSE)); KD path never calls
  PmicBusInit so skipping PmicBusRelease is right; 'USBD' word unused until T2 (expected). Deviations from the
  kernel's gadget path, recorded as first suspects if enumeration fails: GFLADJ not programmed in device mode;
  GUSB2PHYCFG.SUSPHY / GUSB3PIPECTL.SUSPHY left set (Linux keeps them unless dis_u2_susphy_quirk; kd module's
  handling unknown); GUCTL USBHSTINAUTORETRYEN (host-only bit) set, harmless. No follow-up code task.
- T2 (DSDT): Device(UFN0) _HID SAMS9821 (project-local, no driver), _CCA 0, Memory32Fixed 0x10C00000/0x10000
  (EXYNOS_USBDRD_BASE/_SIZE - whole block incl. device regs 0xC700+), no interrupt (polled KD), _STA 0x0F only
  on 'USBD' at 0x8FE0F008. Build exit 0; Dsdt.aml contains UFN0/SAMS9821. Review: 'USBH'/'USBD' are exclusive
  (one word), so XHC0 and UFN0 never both present; overlapping _CRS therefore never double-claimed. OK.
- T3 (DBG2): AcpiTables/Dbg2.aslc, listed in AcpiTables.inf. Build exit 0; compiled Dbg2.acpi decoded (256 B):
  rev 1, 1 device @44: rev 1 len 212, type 0x8003 sub 0x5143, BAR x2 = 0x10C00000 (sizes 0x10000/0x1000),
  ns "\_SB.UFN0", OEM: type 5 (USBFN_SYNOPSYS), 'FIX2' = 0x46495832 (= kd_8003_5143.dll literal pool bytes
  32 58 49 46 - verified), 1 write {BAR0, phase 2, 0xC700 AND 0xFFFFFFF8} (DCFG HS), trailer 'USBC'.
  Beyond0LteAcpiPlatformDxe installs every table in the section (only GTDT/MADT special-cased) -> DBG2 ships.
  Review: 'USBC' absent from the DLL -> trailer likely ignored (kept for layout parity). Revision 1 (Qualcomm
  layout) vs spec 0 - Windows' reader unverified either way; first suspect if kd ignores the entry.
  RE breadcrumb: right after the FIX2 literal the DLL does cmp w0,#4 / cmp w0,#3 - controller-type values 3/4
  may be what this build accepts (Qualcomm table says 5). Second suspect.
- MS doc (setting-up-kernel-mode-debugging-over-usb-eem-arm-kdnet, fetched 2026-10-11): ARM uses DBG2,
  busparams = the DBG2 array index ("typically 0 or 1"; entry 0 is normally the serial port) -> ours is 0 (one
  entry). hostip 169.254.255.255 for every EEM link, port 50000-50039; host: USB 3 Type-A + C-to-A cable,
  firewall off, host shows "Windows KDNET USB-EEM network adapter"; windbg -k net:port=..,key=.. ; target
  device shows "reserved for use by the Windows kernel debugger". Host has WinDbg x64 + ARM64 kdnet.exe
  (Windows Kits 10 Debuggers).
- boot #72 = T1-T3 firmware (pack-uefi-beyond0lte.ps1 output; 18 255 872 B like #45+), s10e-artifacts\
  boot72-v2.img sha 2d9609eb…; 56 pages differ from #71 (176eaf5b…, matches its notes). NOT flashed yet
  (Windows Setup running on #71).
- T4: drivers/S10eDiag/esp/extra-kd.cmd (kdnet.exe list -> bcdedit /dbgsettings net hostip:169.254.255.255
  port:50005 key:1.2.3.4, busparams 0, dhcp No, /debug on, UFN0 pnputil dump) + extra-kd-off.cmd; staged by
  firmware/tools/s10e-stage-kd.sh [off] from TWRP (ESP \S10EDIAG\extra.cmd + \S10EDIAG\kd\ kdnet set).
- 2026-10-11 ~19:58: boot #72 FLASHED (readback 2d9609eb… = image); s10e-stage-kd.sh staged extra-kd.cmd +
  kd\ (kdnet.exe, kdnet-dev-ext.dll, VerifiedNICList.xml) on the ESP. Next boot: agent sets BCD debug; KD live
  from the boot after that.
- T5 host prep: WinDbg installed (winget Microsoft.WinDbg 1.2610.1001.0, WinDbgX.exe in WindowsApps). Instead of
  disabling the firewall (MS doc), an inbound rule "KDNET S10e (UDP 50005 in)" (UDP 50005, any profile) - the
  target sends UDP to the host's debugger port over the EEM adapter. Connect:
  WinDbgX.exe -k net:port=50005,key=1.2.3.4

## First live test (2026-10-11 ~20:20) - debug on => boot hangs at the Windows logo (spinner), forever
- Boot #72 first boot (no debug yet) reached the desktop. UEFILOG: `Dwc3CoreInit: DEVICE role, DCTL F00000 ->
  F00000, DSTS D3030C`, `KD device bring-up Success, UFN0 visible`.
- Agent extra-kd output: target kdnet.exe lists `busparams=0, Device-mode USB controller with vendor ID: 5143
  (Default)` -> our DBG2 entry is recognized. BCD: debug Yes, NET, busparams 0, 169.254.255.255:50005, key 1.2.3.4.
  UFN0 (debug still off that boot): code 28 "no compatible drivers" (expected; would read "reserved" with KD on).
- Next boots (debug on, phone cabled to this PC - adb uses the same cable/port): logo + spinner forever, twice
  (hard reboot between). Host PnP: NO KDNET/EEM device ever enumerated -> kd_8003_5143 init/first transmit
  hangs before the USB device ever connects.
- Recovery: ESP BCD pulled, elevated `bcdedit /store ... /set {default} debug off` (UAC; the shell itself is not
  elevated), pushed back (sha 00ff354d… readback = host); ESP extra.cmd + WINRUN.FLG removed. Copies:
  s10e-artifacts\bcd\BCD.m17-debugon / BCD.m17-debugoff.
- Next: RE of kd_8003_5143.dll init path (subagent -> notes/m17-kd-dll-re.md).
- kd_8003_5143.dll RE (subagent, public PDB via symchk; full log notes/m17-kd-dll-re.md): OEM type 5 = buffered
  KdUsbFn + Synopsys backend (correct; 1/other/no OemData silently -> ChipIdea). CORRECTION of the T3 breadcrumb:
  the cmp #3/#4 next to the FIX2 literal are IsLinkDown/IsLinkUp link-state compares, not controller types.
  Core reset (0x4630): CSFTRST then wait with NO timeout; EP command helper loops on CMDACT with NO timeout. No
  GSNPSID check, no access outside 0xC000-0xCFFF, no PHY init. FIX2 phases: 1 after its CSFTRST, 2 before
  RUN_STOP, 3 on ConnectDone. Leading suspect: our handoff leaves GUSB2PHYCFG SUSPHY (b6) + ENBLSLPM (b8) set ->
  suspended UTMI PHY clock -> CSFTRST or the first EP command never completes (Linux clears SUSPHY around EP
  commands / soft reset for exactly this).
- Fix A (UEFI, Dwc3CoreInit device branch): clear GUSB2PHYCFG SUSPHY|ENBLSLPM (GUSB3PIPECTL.SUSPHY kept - SS
  held down), then rehearse CSFTRST with a 100 ms timeout and log GCTL / GUSB2PHYCFG before->after / PIPECTL /
  DSTS; EFI_TIMEOUT (=> no 'USBD') if it sticks. Build exit 0.
- Fix B (DBG2 FIX2): WriteCount 2: {BAR0, phase 1 (after kd's CSFTRST), 0xC200 AND 0xFFFFFEBF} clears SUSPHY|
  ENBLSLPM inside Windows too, then the phase-2 DCFG HS write. Compiled table decoded = as intended.

## Test 2 (2026-10-11 ~20:45): boot #73 (SUSPHY fixes + DBG2 FIX2 phase-1 + MADT eff fix)
- boot #73 flashed, readback 1b927acb… = image. ESP BCD: debug on again (elevated session; dbgsettings unchanged:
  NET busparams 0 169.254.255.255:50005 key 1.2.3.4), on-device sha fdc75865… = host. WinDbgX listening,
  log %TEMP%\kd73.log.
- Test 2 result (user, ~20:45): boot #73 with debug on BOOTS to the desktop (no hang - SUSPHY fix confirmed as the
  hang cause, INFERRED from the change set). Phone Device Manager shows "Microsoft Kernel Debug Network Adapter"
  (kdnic = KDNET transport initialized on the target). User also reports Windows feels faster (MADT eff fix).
- But host (AMD xHCI port 6) sees NOTHING from the phone: no KDNET EEM adapter, no failed-enumeration device,
  no PnP events -> the phone never attaches D+ (no pull-up seen). WinDbg (user's instance, pid 9844, 20:01) was
  listening on 50005; my second instance failed "port in use" (harmless).
- Suspects for no attach: (1) D+/D- not routed to the AP: the MAX77705 MUIC owns the USB-C data switch and
  nothing in our tree programs it (no COMN1SW/COMP2SW anywhere in notes/code) - Android's muic driver does it;
  (2) RUN_STOP never set / kd gave up after its 3 s + 3 s attach timeouts (notes/m17-kd-dll-re.md); (3) PHY
  not driving (UTMI opmode / termination). Need: UEFILOG of this boot (CSFTRST rehearsal line).
- UEFILOG of test 2 boot (#73): `CSFTRST cleared after 40 us; GCTL 30C12004 GUSB2PHYCFG 102540 -> 102400
  GUSB3PIPECTL 1B021103 DSTS 520004` - rehearsal passes, SUSPHY/ENBLSLPM cleared.
- TWRP sysfs (same cable, adb up): /sys/class/sec/switch usb_sel=PDA, attached_dev=USB, vbus 5 V; ccic
  max77705 fw 1A.00. The MUIC switch is in the always-on PMIC and the cable stayed attached through the reboot
  into our firmware -> data switch most likely still AP. Suspect (1) demoted.
- boot #74 (sha db56d6c5…): BEYOND0LTE_USB_KD_ATTACH_PROBE 1 - UEFI runs the device side 8 s (HS, our event
  buffer, RUN_STOP) logging DSTS link-state changes, then stops. Host watch for a failed enumeration in that window.
- s10escale deployed (System32 + Public Desktop "Display scale.exe", sha da443209… = build).
- Test 3 (boot #74, 20:55): own WinDbgX listening ("Waiting to reconnect..."); host PnP watcher 200 s from
  `adb reboot`: NO new USB device at all (no failed enumeration either) through UEFI's 8 s probe window and the
  Windows KD boot. Need the UEFILOG DSTS trace (TWRP).

## Attach-probe result + TWRP register comparison (2026-10-11 ~21:10)
- UEFILOG boot #74 probe: t0 linkst 4 (Disconnected) -> t1 0 (ON) -> t4 5 (Early suspend) -> t7 3 (Suspend),
  SOFFN 0, GEVNTCOUNT 0, for 8 s. The core attaches internally, then sees an idle bus: no host reset, no SOF ->
  the pull-up / line state never reaches the host. Problem is between the DWC3 and the connector (PHY wrapper),
  before Windows - so it also likely explains M16's dark dock in host mode.
- TWRP (Linux device mode WORKING on the same cable): /dev/mem absent (CONFIG_DEVMEM off), but debugfs has
  10b00000.phy/{regdump,regmap}, 10c00000.dwc3/{regdump,link_state,mode}, max77705-regs. Saved to
  s10e-artifacts\twrp-usb-regs-20261011\. dwc3: mode=device, link_state=U2.
- DWC3 globals in Linux == ours: GCTL 30C12004, GUSB2PHYCFG 00102540 (Linux keeps SUSPHY+ENBLSLPM SET),
  GSBUSCFG0 22220009 -> the core is not the difference.
- PHY wrapper (Linux values) vs our UsbPhyInit:
  HSREWA_CTRL 0x108 = 0x10000000 (DIG_BYPASS_CON_EN bit28 = 1)  -- ours never written  (prime suspect: REWA
     block sits between link and HS PHY; bypass lets the link's UTMI termsel/opmode reach the transceiver)
  HSPCTRL 0x54 = 0x10003300 (RETENABLE_EN bit28 = 1)            -- ours never written
  CLKRSTCTRL 0x20 = 0x00001084 (LINK_PCLK_SEL bit7 = 1)          -- ours never written
  HSPPARACON 0x58 = 0xC16F3337 (device-column tune)              -- ours host column (SQRX 5, TXRES 1)
  LINKCTRL 0x04 = 0x00043FF0 (FORCE_PIPE_EN bit16 = 0, DIS_BUSPEND_QACT 1) -- ours sets FORCE_PIPE_EN
  LINKPORT 0x08 = 0x0001107C; UTMICTRL 0x50 = 0x30; HSPTEST 0x5C = 0x00040000; SECPMACTL 0x48 = 0x100.
- boot #75 (sha 8d5329a5…): KD device path applies the TWRP-read PHY bits after UsbPhyInit (HSREWA_CTRL
  DIG_BYPASS_CON_EN, HSPCTRL RETENABLE_EN, HSPPARACON = 0xC16F3337, CLKRST LINK_PCLK_SEL + 3 ms), with PHY dumps
  before/after in UEFILOG; attach probe still on. Host mode unchanged.
- boot #75 result: Windows logo, no spinner (new hang). UEFILOG: PHY before = LINKCTRL 53FF0 LINKPORT 1107C
  CLKRST 1004 UTMI 30 HSP 10003300 TUNE C12F3537 TEST 80000 REWA_CTRL 10000000 -> REWA bypass and RETENABLE
  were ALREADY set (prime suspect wrong; the PHY wrapper == Linux except TUNE, CLKRST LINK_PCLK_SEL and
  LINKCTRL FORCE_PIPE_EN). With LINK_PCLK_SEL=1 the link never left Disconnected and the core would not halt
  (100 ms) -> dead link clock (SS PHY held down) -> kd module hang. Reverted (boot #76, sha 3e992f62…), device
  tune kept; probe now also logs PHY LINESTATE per change + a per-state ms histogram.
- Remaining suspect: the MAX77705 USB-C D+/D- switch (research -> notes/m17-muic-switch.md).
- boot #76: desktop again (user, ~21:20) - LINK_PCLK_SEL revert confirmed.
- MAX77705 research (notes/m17-muic-switch.md): Linux's max77705_usbc_shutdown -> com_to_open() sets the COM
  switch OPEN (CONTROL1 0x3F) + resets the CCIC on EVERY reboot; only the next Linux boot's muic init_detect writes
  COM_USB 0x09 again. Switch reachable only via the USBC opcode mailbox (0x25: OPCODE_WRITE 0x21, WRITE_END 0x41,
  READ 0x51; CTRL1_R 0x05 / CTRL1_W 0x06). Our firmware never wrote it -> open D+/D- explains the idle bus
  (and likely M16's dark dock).
- boot #77 (sha 582d4b46…): KD path = PmicBusInit -> Max77705UsbSwitchToAp (log FW rev, BC_STATUS, STATUS1/2,
  CONTROL1 before, write 0x09, read back) -> PmicBusRelease -> PHY -> DWC3 -> attach probe (+linestate).
