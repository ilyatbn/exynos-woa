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
- W1 (T4): drivers/Exynos9820WlanCx: Cx.h (device context, worker, command queue), CxDevice.cpp (DriverEntry,
  DeviceAdd in the documented order, PrepareHardware = WlanProbeChip + WlanDlRun/WlanMsgRun with Resident
  forced 1 and RescanSeconds 0, then WiFiCx capabilities; Release; D0Entry starts the worker, D0Exit stops it
  and quiesces; run log Exynos9820WlanCx-run.txt rotated into -run.1..4.txt at 3/4 full), CxWifi.cpp
  (capabilities: 2.4 GHz ch 1-13 HR-DSSS/ERP/HT, 5 GHz ch 36-165 OFDM/HT/VHT, 20 MHz, OPEN/NONE + RSNA_PSK/CCMP,
  1x1 claim; dispatcher: TASK_SET_RADIO_STATE (M3, RADIO_STATUS if changed, M4), TASK_DOT11_RESET (M3, M4),
  a list of WDI_SET_* accepted and logged, everything else NOT_SUPPORTED; every command's TLVs dumped with
  DumpMessageTlvByteStream into the run log, max 400), CxAdapter.cpp (station NetAdapter, MAC = cur_etheraddr,
  MTU 1500, 1 tx + 1 rx queue, system-managed 2 KB rx buffers; W1 queues drop tx / never fill rx), CxMem.cpp
  (operator new/delete for the TLV library). INF Class=Net, *IfType 71, *MediaType 16, *PhysicalMediaType 9,
  Characteristics 0x84, BusType 0, NumberOfNetworkInterfaces 1. Build W4/WX clean, links against
  WificxTLVGenParse.lib + WifiCxStub (1.0) + NetAdapterCx 2.2; infverif /w: INF is VALID.
  **Package s10e-artifacts\wifi-cx1 (1.0.9.1, stampinf date):**
  - Exynos9820WlanCx.sys 188824 B sha256 95578A793B9BB0E86845D03249882145CF81C4072EF5EA8061B099960CEB98B4
  - Exynos9820WlanCx.inf   2601 B sha256 B705E23906661B992B08E0DCEF1F386C40CAFB844F1032C563C5E4A10A30779D
  - exynos9820wlancx.cat   2538 B sha256 63BB0E63EAAAE3151116FBB1E93820150BCD9351AFE96E791AAB7270697D57B0
  Not device-tested.
- Review of 5801d63 (W1): checked DeviceAdd order vs the sample, every command path ends in exactly one
  WifiRequestComplete (dispatcher default, parse failure, queue-alloc failure, CxWorkerStop), tasks send M4 after
  M3 with the copied header (the request buffer is not touched after M3), CreateAdapter does not write the run
  log (the worker owns it), PrepareHardware fails start when the firmware is not resident. FOUND: (1) Dev->Adapter
  was never cleared, so a second EvtWifiDeviceCreateAdapter (PnP stop/start) would be refused; (2) the run-log
  header claimed a 5 ms "linked" poll that the worker does not do. Fixed in a follow-up commit (adapter cleanup
  callback; header line). W1 package wifi-cx1 is superseded by the next package.
- W2 (T5): CxScan.cpp. WlanMsgSetCxHooks(CxBssRecord) at PrepareHardware; CxBssRecord upserts per BSSID into a
  64-entry cache (WDF memory child of the device): beacon period / capability / chanspec / RSSI / ctl_ch / IEs (cut
  at 1400 B, flagged). TASK_SCAN: parse (SSID count logged), M3, broadcast escan (WlanMsgRescan), one log line
  per BSS, BSS_ENTRY_LIST indications of up to 16 entries (unsolicited, TransactionId 0, like the sample
  wifihal.cpp:580-585) with the probe-response body = timestamp 0 + beacon interval + capability + IEs, RSSI,
  link quality (linear -100..-50 dBm -> 0..100, host choice), channel + band (ch <= 14 -> 2.4 GHz else 5 GHz),
  age = KeQuerySystemTimePrecise; then M4 SCAN_COMPLETE (failure status only when the escan failed and nothing
  was found). Build W4/WX clean. Not device-tested. Known gaps: hidden SSIDs (directed probe), 6 GHz n/a.
- Review of 523a50e (W2): bounds of the per-slot frame body (12 + IesLen <= 12 + 1400), ie_offset/ie_length
  checks against the record length, BSSID upsert cap 64, the ArrayOfElements pointers detached before the
  destructors run (SimpleAssign(NULL, 0)), SSID count read before CleanupParsed, M3 before the long escan, M4
  after the BSS lists. No findings.
- W3 (T6): CxData.cpp + CxAdapter.cpp queues. rx: WlanMsgSetRx(CxRxFrame) -> RxFifo (64 x 2 KB, spinlock) ->
  NetRxQueueNotifyMoreReceivedPacketsAvailable when NetAdapterCx armed notification; rx Advance copies one frame
  per OS fragment (system-managed 2 KB buffers), Layer2Type Ethernet; rx queue destroy clears the queue handle.
  tx: tx Advance gathers each packet's fragments (virtual-address extension) into TxFifo and returns it at once
  (drop + count when full / > 2 KB), Layer2Type IEEE 802.11 frames are converted to 802.3 (802.11-2016 address
  table, RFC 1042 SNAP); worker CxTxDrain -> WlanMsgTx before and after each poll (no buffer / ring full = keep,
  no flowring = drop). First 8 tx layouts recorded and logged by the worker ("[tx] packet layout #n: layer2 type
  ..."): this is the device-side check of the UNVERIFIED frame format. Worker waits CX_LINK_WAIT_MS (5 ms) while
  Dev->Linked (set in W4). Build W4/WX clean. Not device-tested.
- Review of 68f0695 (W3): checked the rx fragment/packet pairing against the NetAdapterCx samples (one fragment
  per packet at Fr->BeginIndex), Ignore packets returned unsent, the tx fragment walk ends at the packet's last
  fragment, notification is one-shot under the FIFO lock, the rx queue handle is cleared in EvtDestroy, the
  802.11->802.3 conversion offsets (A2 @10, A3 @16, QoS +2). FOUND: (1) tx Advance called CxFifoPut on a NULL
  TxFifo when the FIFO allocation failed (crash); (2) the tx layout log could print stale stack bytes for a
  packet shorter than 14 B. Fixed in a follow-up commit.
- Review of e1eb9fa (W3 fixes): diff re-read; the NULL check precedes the FIFO call, the zeroing only touches the
  14-byte header the log prints. No findings.
- W4 (T7): CxConnect.cpp. TASK_CONNECT: parse; only OPEN/NONE or RSNA_PSK + CCMP/CCMP accepted (else M3
  NOT_SUPPORTED, matching the station capabilities); M3; up to 2 host BSS entries in order: chanspec + IEs from
  the scan cache (else the entry's probe-response / beacon body), RSN IE required for PSK; WlanMsgConnect;
  ASSOCIATION_RESULT per attempt (status SUCCESS / NO_ASSOC_RESPONSE on timeout / ASSOC_FAILED_BY_PEER; on success
  the association request body = cap + listen + req IEs, response body = cap + status + AID + resp IEs, beacon
  body from the cache, PortAuthorized FALSE, DSInfo unknown); on success Linked (1 ms timer resolution, 5 ms worker
  waits, NetAdapterSetLinkState connected) + LINK_STATE_CHANGE (speed 54000 placeholder, quality from RSSI); M4
  CONNECT_COMPLETE. SET_ADD_CIPHER_KEYS: drain tx + WlanMsgWaitTxIdle(200 ms) first, then per CCMP key: pairwise
  -> wsec_key idx 0 ea = peer, group -> idx = key id, primary, rsc (byte order UNVERIFIED). TASK_DISCONNECT and
  DOT11_RESET -> WLC_DISASSOC + DISASSOCIATION (by host / by device reset). Firmware loss (WLAN_LINK_FN) ->
  DISASSOCIATION (peer deauthenticated / disassociated / unable to maintain). SET_DELETE_CIPHER_KEYS accepted.
  Build W4/WX clean. **Package s10e-artifacts\wifi-cx2 (W1-W4; inx 1.0.9.4, stampinf DriverVer
  10/12/2026,23.32.47.394):**
  - Exynos9820WlanCx.sys 216984 B sha256 692BD06A3EA5603C00F1842D2FB82286E87B91A4E278EDF382692C1F69ECB4EE
  - Exynos9820WlanCx.inf   2601 B sha256 02E00A9055580E8F0C5CBDC6317DCE0E060BB2C8D63B2884C226D13A600BAEAE
  - exynos9820wlancx.cat   2538 B sha256 4FDE2B0DDD33870B9FBC8F97F270F428235233218C42AACB26E19EF994CC7587
  Not device-tested.
- Review of b983875 (W4): checked every early return of CxTaskConnect frees P/C/A and sends exactly one M3; the
  M4 is sent on every path after M3; ExSetTimerResolution set/release balanced by the Linked guard (also on D0
  exit); LinkFn cannot fire inside WlanMsgConnect (Joining); association-result frame buffers sized 4/6 + 1024 B
  match WLAN_ASSOC_IES_MAX. FOUND: a group key without a CipherKeyID was installed as key id 1 - a guessed
  constant. Fixed in a follow-up commit (rejected with STATUS_INVALID_PARAMETER). Noted, not changed (W5): the
  worker is blocked for up to 2 x 20 s during a connect (JOIN_TIMEOUT_MS) while WDI's normal connect time is 10 s,
  and abort requests queue behind it.
- Review of b473ae3 (W4 fix): the group-key path now refuses a missing key id before WlanMsgSetKey; the pairwise
  path is unchanged. No findings.
- **Final package s10e-artifacts\wifi-cx3 (W1-W4 + review fixes; inx 1.0.9.4, stampinf DriverVer
  10/12/2026,23.34.53.127), supersedes wifi-cx1 / wifi-cx2:**
  - Exynos9820WlanCx.sys 216984 B sha256 DD9A51480207BB3E3E05842C6B6756D90DDD39C57AA2090E18C7584CAB5E6C26
  - Exynos9820WlanCx.inf   2601 B sha256 8C081CEE7D00D6E0F75BF77831B6339B9F7E9C85A98E28420219F862FCBF5778
  - exynos9820wlancx.cat   2538 B sha256 B214EFB186A7A93D21F289B5B41840AF80612B2C8EF396B8A35089D38734BBE9
  - extra-wifi-cx.cmd (copy of drivers/S10eDiag/esp/extra-wifi-cx.cmd) sha256 598c3ff71033b6d0...
  signtool verify /pa stops only at the untrusted WDK test root (expected; the phone runs testsigning with
  wdktestcert installed).

## ESP test (device run, not done in this session)
1. ESP: package -> `\S10EDIAG\drivers\Exynos9820WlanCx\` (sys, inf, cat), blobs already in `\S10EDIAG\bcm4375\`,
   `extra-wifi-cx.cmd` -> `\S10EDIAG\extra.cmd`. Boot Windows (S10eAgent runs it). Results in out\last\wificx\.
2. If key.txt shows "DL 6" / "mismatch" (warm restart while the KMDF driver's firmware ran): reboot once with
   extra-wifi-cx.cmd again (cold load of the Cx driver).
3. User, at the phone: open the network flyout -> Wi-Fi list -> pick the home SSID -> type the password -> connect;
   then `ipconfig`, open a web page. Afterwards collect again (re-run extra-wifi-cx.cmd: it reinstalls the same
   package = no-op install, then collects; or extra-wifi-collect-style copy of the two LogFiles).
What the logs must show (Exynos9820WlanCx.txt = bring-up, -run.txt = runtime):
- Bring-up: "[dl] D11 core @... resetctrl", "pass 1 ... 0 mismatches", "DL 0", "MSG 0", "SCAN 0", "RESIDENT",
  "CX 0: WiFiCx capabilities 0x00000000; MAC A8:DB:03:06:3C:EF" (first line names the OS WDI version).
  "CX 1" = firmware not resident (device fails start); "CX 2" = a capability call was refused (status printed).
- Run log: "[cx] D0 entry", "[cx] worker started", then "[wdi] <- id N" for each WiFiCx command with its TLV dump;
  expected early ids 85 (SET_ADAPTER_CONFIGURATION), 11 (TASK_SET_RADIO_STATE), 7 (DOT11_RESET), 4 (TASK_SCAN). Any
  "M3 id N status 0xC00000BB" = a command we answer NOT_SUPPORTED - the list is the input for the next round.
- Scan: "[scan] WDI scan ... SCAN 0, n records -> m BSS", one line per BSS, "-> indication 62" and "-> indication 60
  (M4 ...)". Windows side: netsh-networks.txt lists the SSIDs; the flyout shows them.
- Connect (after the user picks the network): "[join] WDI connect: k BSS entries, auth 7 unicast 4 multicast 4",
  "CONNECT 0 ... flowring open", "[join] assoc_info: req ...", "-> indication 76", "[link] connected", "-> indication
  53", "-> indication 64 (M4 ...)". Then EAPOL through the data path: "[tx] packet layout #1: layer2 type ..."
  (tells the NetAdapter frame format: Ethernet expected), WlanMsg "[rx] ... type 0x888E" / "[tx] post ... type
  0x888E", then "[key] add: type 1 algo 4 ... ccmp 16 B" + "[key] pairwise key idx 0 -> MSG 0" and the group key.
  netsh-interfaces.txt "State : connected", ipconfig shows an IPv4 lease on the Wi-Fi adapter.
- Failure signatures: no indication 62 / empty list -> scan path or BSS TLV format; CONNECT 3 -> firmware join
  refused; connected but no "[key]" -> the OS supplicant did not get EAPOL (check rx "type 0x888E" lines and the
  tx layout line); keys installed but no DHCP -> data frame format (layout line) or rx delivery (alive line rx fifo
  in/out counters).

## Untested / known gaps (W5 and later)
- Everything above is device-unvalidated. UNVERIFIED in particular: wificx.sys present in 22631 (doc says yes);
  OS acceptance of WDI_VERSION_LATEST; NetAdapter frame format (Ethernet assumed); WDI RSC byte order; whether
  WiFiCx expects the client to call NetAdapterSetLinkState (done on connect / loss); link speed unit.
- Not implemented: radio off (state only), hidden-SSID directed scan, roaming, power save / D3 (a resume from
  sleep fails start: the firmware is quiesced at D0 exit and only PrepareHardware reloads it), interrupt instead of
  polling, flowring delete on disconnect (single flowring keeps the first AP's DA), TKIP group cipher, WPA3/SAE,
  abort of a running connect (worker blocked up to 2 x 20 s).
- Review of e20e069 (script + notes): working copy CRLF, repo blob LF like extra-wifi-collect.cmd (same
  attribute handling); findstr searches literal (/l: "[wdi]" etc. would otherwise be regex classes); only the
  bring-up/run key lines become marks, the per-command [wdi] lines go to wdi.txt; no credentials anywhere; the
  package's sha256 in the notes re-checked against s10e-artifacts\wifi-cx3. No findings.
