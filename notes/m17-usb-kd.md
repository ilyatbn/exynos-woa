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
