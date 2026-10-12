# M12 WiFi — native Windows WiFi (WiFiCx client driver)

User decision 2026-10-12 (notes/m12-wifi-m7.md, end): from the connected stage (1.0.7.4 joined the user's
WPA2 network) go to native Windows WiFi: adapter in the Windows network UI, scan list, connect with a
password, ipconfig shows a real adapter, browsing works. This session: build + test-sign + package only,
**no phone access** (no adb). Everything here is device-unvalidated until a device run says otherwise.

Base: main ba1820e. Existing driver: drivers/Exynos9820Wlan (KMDF, ACPI\SAMS9825, Class=System,
1.0.7.4).

## Progress log
- 2026-10-12: read AGENTS.md, notes/m12-wifi-m7.md, notes/m12-wifi-m7-rx-audit.md, the driver's README /
  build-package.ps1 / .inx / .vcxproj. NetAdapterCx version research already exists (m7 notes:75-84:
  target NetAdapterCx 2.3 for 22631).
- WDK 10.0.26100.0 on this box ships WiFiCx headers 1.0 / 1.1 / 1.2
  (Include\10.0.26100.0\km\wificx\1.x\wificx.h), stub libs Lib\10.0.26100.0\km\arm64\wificx\1.x\wificxstub.lib,
  the WDI TLV generator/parser Lib\...\km\arm64\wlan\2.0\WificxTLVGenParse.lib with its headers
  km\dot11wificxintf.h (1786 lines) + km\dot11wificxtypes.hpp (C++, 1139 lines).
- WiFiCx 1.0 API (wificx.h): WifiDeviceInitConfig(DeviceInit), WifiDeviceInitialize(Device, WIFI_DEVICE_CONFIG
  {WdiVersion, SendCommand, CreateAdapter, CreateWifiDirectDevice}), WifiDeviceSet{Device,Station,Band,Phy,
  WiFiDirect}Capabilities, WifiDeviceReceiveIndication(Device, MessageId, WDFMEMORY), WifiAdapterInitialize,
  WifiRequestGetInOutBuffer / GetMessageId / SetBytesNeeded / Complete, WifiAdapterInitGetType, AddPeer...
  => the OS talks WDI messages (TLV-encoded) to EvtWifiDeviceSendCommand; the driver answers with
  WifiRequestComplete and pushes WDI indications with WifiDeviceReceiveIndication.
- WiFiCx sample: microsoft/Windows-driver-samples network/wlan/wificx (cloned 2026-10-12 to scratchpad
  ref/wds, HEAD 2dc3fd3a, MS-PL; read, nothing copied verbatim). Facts used:
  - DeviceAdd order: NetDeviceInitConfig -> WifiDeviceInitConfig -> WdfDeviceCreate -> WifiDeviceInitialize
    (driver.cpp:99-161), TlvContext.PeerVersion = WifiDeviceGetOsWdiVersion (driver.cpp:180).
  - Capabilities set from EvtDevicePrepareHardware (device.cpp:9-20 -> wifihal.cpp:76-500).
  - EvtWifiDeviceCreateAdapter: WifiAdapterInitGetType must be EXTENSIBLE_STATION, datapath callbacks,
    NetAdapterCreate, WifiAdapterInitialize, caps, NetAdapterStart (device.cpp:34-94, adapter.cpp:22-63).
  - Command flow: EvtWifiDeviceSendCommand -> WifiRequestGetInOutBuffer/GetMessageId; M3 = WifiRequestComplete;
    task M4 = WifiDeviceReceiveIndication(completion indication id, header with the request's TransactionId);
    unsolicited indications use TransactionId 0 (wifirequest.cpp:34-97, wifitransition.cpp:179-216).
  - TLV parse/generate via WificxTLVGenParse.lib (Lib\10.0.26100.0\km\arm64\wlan\2.0) + operator new(size_t,
    ULONG_PTR AllocationContext) / delete supplied by the client (memorymanagement.cpp:118-148).
  - Data path sample (network/netadaptercx/netvadapterlibrary) uses Ethernet-like addressing; the frame
    format on the NetAdapter queues for WiFiCx is **UNVERIFIED** (see design).
- Version check (learn.microsoft.com "Building a WiFiCx driver for multiple versions of Windows", fetched
  2026-10-12): Windows 11 21H2 and 22H2: KMDF 1.33, NetAdapterCx 2.2, **WiFiCx 1.0 only**; "To load a
  WiFiCx-based driver on Windows 11, versions 21H2 and 22H2, the NetAdapterCx version has to be forced to 2.2";
  22H2 Moment 3+ allows 2.2+. WiFiCx 1.1/1.2 need build >= 25330. The device runs 22631.2861 (ISO
  Windows11_22631.2861_Pro_arm64.iso, install.wim index 3 = 22621.2861 + 23H2 enablement,
  notes/m10-windows-deploy-plan.md:43,173) = 22H2 servicing branch.
  **Decision: WIFI_VERSION 1.0, NETADAPTER_VERSION 2.2, KMDF 1.33.** The WDK msbuild default for WiFiCx 1.0 is
  NetAdapterCx 2.1 (build\10.0.26100.0\WindowsDriver.KernelMode.CX.Default.props:80-90); overridden to 2.2
  per the doc. Whether wificx.sys is in the device image: not read (install.wim needs DISM elevation; the
  7-Zip 9.13 on this box cannot open it) - UNVERIFIED; documented as inbox since Windows 11 21H2.
  WDI version announced: WDI_VERSION_LATEST (2.0.13, dot11wificxintf.h:1403) as the sample does; the OS's
  WDI version is read with WifiDeviceGetOsWdiVersion and logged (the TLV parser adapts via PeerVersion).
- Who runs WPA2 (learn.microsoft.com "WiFiCx Wi-Fi 7 feature requirements" + OID_WDI_SET_ADD_CIPHER_KEYS):
  "For data frames, including 4-way handshake and 802.1x authentication frames ...", "Windows supports
  GCMP-256 in the 4-way handshake", keys plumbed with OID_WDI_SET_ADD_CIPHER_KEYS; TlvGenerated_.hpp has no
  PMK/PSK TLV (only PMKID / PMK names). => **the OS supplicant runs the 4-way handshake over the NetAdapter
  data path and plumbs PTK/GTK with WDI_SET_ADD_CIPHER_KEYS.** The driver never sees the PSK.
  Consequences: (1) EAPOL frames pass through rx/tx unchanged (the M7b capture is off in the Cx driver);
  (2) the association result must carry the association request frame (the RSN IE the OS echoes in msg 2):
  "Windows requires that the driver indicates the Association Request and Response frames" (wificx-wi-fi-7);
  (3) EAPOL msg 4 must be on the air before the pairwise key is installed (dhd wl_cfg80211.c:6903-6904
  dhd_wait_pend8021x) -> the worker drains tx and waits for tx completion before SET_ADD_CIPHER_KEYS.
- OID_WDI_TASK_CONNECT doc: no own scan needed, use the host's BSS list; report ASSOCIATION_RESULT per attempt;
  M4 = CONNECT_COMPLETE; normal execution time 10 s.
- WDI message ids (dot11wificxintf.h:36-167): TASK_SCAN 4, TASK_CONNECT 6, TASK_DOT11_RESET 7,
  TASK_DISCONNECT 8, TASK_SET_RADIO_STATE 11, SET_ADD_CIPHER_KEYS 29, SET_DELETE_CIPHER_KEYS 30,
  INDICATION_SCAN_COMPLETE 60, BSS_ENTRY_LIST 62, DOT11_RESET_COMPLETE 63, CONNECT_COMPLETE 64,
  RADIO_STATUS 67, ASSOCIATION_RESULT 76, DISASSOCIATION 51, LINK_STATE_CHANGE 53, DISCONNECT_COMPLETE 21,
  SET_RADIO_STATE_COMPLETE 22, SET_ADAPTER_CONFIGURATION 85.

### Warm restart (TCM read-back mismatch on a device restart while the firmware runs)
- Observed (m7 notes:284-285): pnputil restart while fw runs -> next download "DL 6: TCM read-back mismatch"
  (28 mismatches). Our quiesce halts only the ARM CR4 and clears BME (WlanMsg.c WlanMsgQuiesce).
- fmac brcmf_chip_cr4_set_passive (chip.c:1323-1337, run before every download): halt the CR4 **and put
  every D11 core in reset** (brcmf_chip_coredisable, prereset PHYRESET|PHYCLOCKEN, reset PHYCLOCKEN;
  D11_BCMA_IOCTL_PHYCLOCKEN 0x4, PHYRESET 0x8 chip.c:81-82); ai coredisable chip.c:350-382 (skip if already in
  reset; IOCTL = prereset|FGC|CLK; RESET_CTL = 1; wait RESET_CTL == 1 up to 300 us; IOCTL = reset|FGC|CLK).
  Our EROM has two D11 cores 0x812 rev 82 (notes/m12-wifi-m2.md:144). Hypothesis (UNVERIFIED): the D11 MAC
  keeps DMAing received frames into TCM while only the ARM is halted. Backplane-only, no PCIe-level side
  effect -> done by default.
- Heavier resets exist and are NOT used (link-level side effects that cannot be tested without the phone):
  dhd dhdpcie_dongle_reset (dhd_pcie.c:1328-1350) = FLR (DevCtl bit 15 when DevCap bit 28 is set,
  dhd_bus_perform_flr :4906-5000, config-space save/restore incl. BARs), else the ChipCommon watchdog
  (pcie_watchdog_reset pcie_core.c:52-115: ASPM off, watchdog = F0 subsystem reset | 4 for ccrev >= 65); fmac
  brcmf_pcie_reset_device (pcie.c:656-708: ASPM off, CC watchdog = 4, 100 ms). Our EP config space is a CFG0
  iATU window behind the UEFI-trained RC; a reset that wipes the EP's BARs/COMMAND cannot be re-enumerated
  from Windows. Next step only if the D11 disable does not cure DL 6.

## Plan (milestones W1..W5; tasks = one commit each)
Architecture: new KMDF project **drivers/Exynos9820WlanCx** (Class=Net, WiFiCx 1.0 + NetAdapterCx 2.2,
ACPI\SAMS9825, service Exynos9820WlanCx) that compiles the existing C bring-up (WlanDl.c, WlanMsg.c,
WlanSup.c + a shared WlanCommon.c) from drivers/Exynos9820Wlan plus new C++ WiFiCx files. The KMDF probe
driver (Exynos9820Wlan, Class=System) stays buildable with unchanged behaviour; only one of the two can own
the device (the Cx INF is newer and Class=Net).
Threading: one worker thread owns the msgbuf state (existing code is PASSIVE + single owner): runs queued
WiFiCx commands, drains the NetAdapter tx FIFO into the flowring, polls the D2H rings (rx -> rx FIFO ->
NetAdapter rx queue) and raises unsolicited indications (link loss). SendCommand (<= DISPATCH) only queues.
Logs: Exynos9820WlanCx.txt (bring-up) + Exynos9820WlanCx-run.txt (runtime, rotated when full).
- T0 notes + plan (this commit).
- T1 warm restart: D11 cores into reset before the image write (fmac cr4_set_passive) + log the first
  mismatching addresses + one rewrite. Shared WlanDl.c (both drivers).
- T2 refactor: report/config/chip-probe helpers out of WlanProbe.c into WlanCommon.c (pure move); report
  file names overridable per project.
- T3 WlanMsg.c API for the Cx driver: EAPOL pass-through switch, per-BSS scan hook, link-loss hook,
  connect / disconnect / set key / wait tx idle.
- **W1 skeleton** (T4): project, INF, DeviceAdd / PrepareHardware (bring-up) / Release / D0, worker, WiFiCx
  capabilities (2.4 + 5 GHz, OPEN/NONE + RSNA_PSK/CCMP), CreateAdapter (MAC = cur_etheraddr), dispatcher:
  SET_RADIO_STATE, DOT11_RESET, SET_ADAPTER_CONFIGURATION + other sets accepted and logged, unknown ->
  NOT_SUPPORTED; every command TLV-dumped into the log. Adapter visible, no traffic.
- **W2 scan** (T5): TASK_SCAN -> escan -> BSS_ENTRY_LIST (probe response body synthesized from wl_bss_info:
  timestamp 0, beacon period, capability, IEs) -> SCAN_COMPLETE.
- **W3 data path** (T6): NetAdapter tx/rx queues over the flowring / rx completions (copy FIFOs).
- **W4 connect** (T7): TASK_CONNECT -> firmware join (no host supplicant) -> ASSOCIATION_RESULT (assoc
  req/resp bodies from assoc_info + assoc_req_ies / assoc_resp_ies) + LINK_STATE_CHANGE -> CONNECT_COMPLETE;
  SET_ADD/DELETE_CIPHER_KEYS -> wsec_key; TASK_DISCONNECT and firmware deauth/link-down -> DISASSOCIATION.
- W5 power / roam / interrupt / flowring delete on disconnect: not this session (todo row).
- T8 ESP install/collect script drivers/S10eDiag/esp/extra-wifi-cx.cmd.

## Reviews
- Review of ab86145 (T0, notes only): re-checked every cited line with grep. FOUND: sample citations
  driver.cpp:96-151 / :166 were off (actual NetDeviceInitConfig :99, WifiDeviceInitialize :161, PeerVersion
  :180) and device.cpp:35 (actual :34). Fixed in a follow-up commit. dhd wl_cfg80211.c:6903-6904,
  dot11wificxintf.h:1403, CX.Default.props:80-90, m2 notes:144, fmac chip.c:81-82/350-382/1323-1337,
  pcie.c:656-708, dhd_pcie.c:1328-1350/4906-5000, pcie_core.c:52-115 verified.
- T1 (warm restart): WlanDl.c CoreDisable (fmac ai_coredisable chip.c:350-382) + D11Disable over every EROM
  core 0x812 (Wrap[0]) with prereset PHYRESET|PHYCLOCKEN, reset PHYCLOCKEN (Exynos9820Wlan.h D11_IOCTL_*, fmac
  chip.c:81-82) right before the image write; image write+verify now notes the first 4 mismatches
  ("[dl]   mismatch @addr read want") and is done twice before DL 6. Parameters\D11Reset (default 1) = A/B
  switch. KMDF Exynos9820Wlan build W4/WX clean (scratchpad cx\t1-kmdf, sys 2D4CEFCD2C924575...). Device
  expectation: cold boot "[dl] D11 core ... resetctrl 0x1 -> 0x1" (already in reset) or 0x0 -> 0x1; after
  a device restart "pass 1 ... 0 mismatches" = cured; mismatch addresses = where the writer is.
- Review of c124f92 (T1): checked CoreDisable against fmac chip.c:350-382 step by step (skip-if-in-reset, IOCTL
  prereset|FGC|CLK, RESET_CTL 1, 10 us, wait == 1 up to 300 us, IOCTL reset|FGC|CLK); D11 bits match chip.c:81-82;
  the ARM release (CoreReset(Cr4, 0, 0)) still only touches the CR4 wrappers, so the D11 cores stay in reset for
  the firmware to bring up (fmac comment chip.c:1328-1330); retry loop rewrites the same seed; Bad is reset per
  pass; BadNote bounded by DL_BAD_LOG. FOUND: WlanProbe.c's "Parameters key settings" comment lists neither
  D11Reset nor (already before) CacheMaint. Fixed in a follow-up commit.
- T2 (refactor): ReportLine / ReportWriteFile / WlanReadConfig / CfgRead + the chip-probe half of
  WlanEvtPrepareHardware moved from WlanProbe.c into new WlanCommon.c (WlanProbeChip); WLAN_PROBE +
  WLAN_RESULT_* and the prototypes moved to WlanDl.h (which now includes wdf.h itself: WlanSup.c includes
  WlanDl.h without wdf.h); WLAN_REPORT_FILE / WLAN_RUN_REPORT_FILE are #ifndef-guarded so the Cx project can
  name its own logs. Mechanical check: every line removed from WlanProbe.c exists verbatim in WlanCommon.c /
  WlanProbe.c / WlanDl.h except "ULONG Bar0Lo;" (now "= 0"). KMDF build W4/WX clean (scratchpad cx\t2-kmdf,
  sys D5C5CB84C886F90C...).
- Review of e8fc3fb (T2): read WlanCommon.c's WlanProbeChip against the old PrepareHardware: every goto still
  reaches the Bar0 unmap; *Out/*CfgOut/*Bar0LoOut are written on every path; the only ReportWriteFile
  dropped from the moved block is re-issued by WlanProbe.c on RESULT 0 (same order as before); registry
  report still written on every path. FOUND: WlanDl.h's file comment still says the report/registry live in
  WlanProbe.c. Fixed in a follow-up commit (comment only).
- T3 (WlanMsg API for the Cx driver): WlanMsgSetCxHooks (BSS record hook from EscanResult, link-loss hook from
  EventPacket once per association, EAPOL pass-through to RxFn), WlanMsgConnect (event mask, INFRA, wpa_auth
  PSK|disabled, auth open, wsec AES|none, wpaie = BuildOwnRsn from the AP's RSN IE, SET_SSID with BSSID +
  chanspec when known = fmac fallback join cfg80211.c:2599-2615 / fwil_types.h:482-512 (52 B), wait SET_SSID,
  flowring, assoc_info + assoc_req_ies / assoc_resp_ies like fmac cfg80211.c:6344-6400), WlanMsgDisconnect
  (WLC_DISASSOC 52 + scb_val {reason 8, ea}, fmac cfg80211.c:2647-2650), WlanMsgSetKey (InstallKey: pairwise idx 0
  ea=peer / group idx primary + rsc), WlanMsgWaitTxIdle. New constants in Exynos9820Wlan.h "M9" block, each
  cited. KMDF probe driver unaffected (hooks NULL, EapolToRx FALSE, Connected FALSE); build W4/WX clean (sys
  CAF761AB84E0FA5A...).
- Review of 1415eef (T3): checked RxComplete routes EAPOL to RxFn only when EapolToRx (KMDF driver keeps the
  M7b capture); LinkFn fires once (Connected cleared first) and not for host DISASSOC; assoc IE copies bounded by
  WLAN_ASSOC_IES_MAX and RespLen; 52 B join layout vs fmac fwil_types.h:482-512 (chanspec_num @44, list @48);
  WaitTxIdle exits on the first all-idle scan. FOUND: a failed / timed-out WlanMsgConnect left the firmware
  pursuing the SET_SSID target (no disassoc). Fixed in a follow-up commit: DISASSOC (reason 8, AP) when SET_SSID
  was accepted but the connect did not succeed. Build clean (sys 2EDB158D5C3B0968...).
